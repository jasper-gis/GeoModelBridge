"""Python API contract tests; subprocess boundary is mocked except log/argv checks."""
from dataclasses import FrozenInstanceError, replace
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import tracemalloc
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from geomodelbridge import CallbackError, ConversionError, ConversionRequest, Engine, GeoModelBridgeError, ValidationError, __version__
from geomodelbridge import InspectionBounds, InspectionCounts, InspectionError, InspectionRequest, InspectionResult
from geomodelbridge.client import LOG_TAIL_BYTES, _ProcessResult, _run


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gmb-client-中文 空格-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "模型 & 数据.fbx"
        self.source.write_bytes(b"source must remain unchanged")
        self.exe = self.root / ("geomodelbridge.exe" if os.name == "nt" else "geomodelbridge")
        self.exe.touch()
        self.engine = Engine(self.exe)
        self.engine.writer.parent.mkdir()
        self.engine.writer.touch()
        self.request = ConversionRequest(self.source, self.root / "结果.gdb", 3857, (100, 100, 100))

    def success_report(self, request=None):
        request = request or self.request
        return dict(status="written_and_readback_verified", version=__version__, backend="native-filegdb",
                    conversion_profile=request.profile, missing_texture_policy=request.missing_textures,
                    output=str(request.output_gdb), source=str(request.input_fbx), feature_class=request.feature_class,
                    coordinate_system=dict(wkid=request.wkid, projected=True, unit="meter",
                                           source_coordinates_assigned_without_reprojection=True), reader_diagnostics=[],
                    coordinates=dict(wkid=request.wkid, unit="meter", up_axis="Z", space="referenced",
                                     origin=list(request.origin), origin_explicit=True),
                    verification=dict(level="closed_reopened_file_geodatabase", feature_count=1,
                                      geometry_material_uv_texture_readback=True, checks=[dict(mesh_index=0, passed=True)]))

    def fake_run(self, report=None, code=0, create_gdb=True, write_report=True):
        def run(command):
            if command[-1] == "--version":
                return _ProcessResult(0, f"GeoModelBridge V{__version__}\n", "")
            if command[-1] == "--probe":
                return _ProcessResult(0, json.dumps(dict(status="available", version=__version__,
                        backend="native-filegdb", arcgis_pro_required=False)), "")
            values = list(map(str, command))
            output = Path(values[values.index("--output") + 1])
            report_path = Path(values[values.index("--report") + 1])
            if create_gdb:
                output.mkdir()
            if write_report:
                report_path.write_text(json.dumps(report if report is not None else self.success_report()), encoding="utf-8")
            return _ProcessResult(code, "done", "warning details")
        return run

    def test_import_has_no_arcpy_dependency(self):
        code = "import sys; import geomodelbridge; assert 'arcpy' not in sys.modules"
        env = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parents[1] / "python"))
        subprocess.run([sys.executable, "-S", "-c", code], check=True, env=env)

    def test_default_and_explicit_policies_and_argument_boundaries(self):
        args = self.engine.command(self.request)
        self.assertIn(str(self.source), args)
        self.assertEqual(args[args.index("--profile") + 1], "strict")
        self.assertEqual(args[args.index("--missing-textures") + 1], "material-color")
        self.assertEqual(args[args.index("--origin") + 1:args.index("--origin") + 4], ("100.0",) * 3)
        request = replace(self.request, profile="gis-static", missing_textures="error", texture_dirs=(self.root, self.root / "native-filegdb"))
        args = self.engine.command(request)
        self.assertEqual(args.count("--texture-dir"), 2)
        self.assertIn("gis-static", args)
        self.assertIn("error", args)
        self.assertIsNone(self.request.report_path)

    def test_obj_source_and_coordinate_options(self):
        obj = self.root / "模型.obj"
        obj.write_text("o example\n", encoding="utf-8")
        request = replace(self.request, input_fbx=obj, obj_up_axis="Y", obj_unit_meters=0.01)
        args = self.engine.command(request)
        self.assertEqual(args[args.index("--obj-up-axis") + 1], "Y")
        self.assertEqual(args[args.index("--obj-unit-meters") + 1], "0.01")
        for change in (dict(obj_up_axis="X"), dict(obj_unit_meters=0), dict(obj_unit_meters=float("nan")),
                       dict(obj_unit_meters=10**400)):
            with self.subTest(change=change):
                with self.assertRaises(ValidationError) as failure, patch("geomodelbridge.client._run") as runner:
                    self.engine.convert(replace(request, **change))
                self.assertEqual(failure.exception.code, "INVALID_REQUEST")
                runner.assert_not_called()

    def test_glb_source(self):
        glb = self.root / "模型.glb"
        glb.write_bytes(b"placeholder")
        request = replace(self.request, input_fbx=glb)
        args = self.engine.command(request)
        self.assertIn(str(glb), args)
        self.assertNotIn("--obj-up-axis", args)

    def test_dae_source(self):
        dae = self.root / "模型.DAE"
        dae.write_text("placeholder", encoding="utf-8")
        args = self.engine.command(replace(self.request, input_fbx=dae))
        self.assertIn(str(dae), args)
        self.assertNotIn("--obj-up-axis", args)

    def test_invalid_requests(self):
        changes = [dict(wkid=True), dict(wkid="3857"), dict(wkid=0), dict(wkid=2**31),
                   dict(origin=(1, 2)), dict(origin=(1, 2, float("nan"))), dict(origin=(1, 2, float("inf"))),
                   dict(origin=(1, 2, True)), dict(origin=(1, 2, 10**400)), dict(feature_class="--evil"),
                   dict(feature_class="A"*65), dict(profile="loose"), dict(missing_textures="ignore"),
                   dict(texture_dirs=str(self.root)), dict(texture_dirs=(self.root / "absent",)),
                   dict(output_gdb=self.root / "UPPER.GDB"), dict(output_gdb=self.root / "absent/new.gdb"),
                   dict(report_path=self.root / "absent/new.json"), dict(report_path=self.root / "结果.gdb/r.json"),
                   dict(input_fbx=""), dict(input_fbx="\0"), dict(input_fbx=self.root),
                   dict(input_fbx=self.root / "absent.fbx"), dict(report_path=self.root / "another.gdb")]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValidationError), patch("geomodelbridge.client._run") as runner:
                self.engine.convert(replace(self.request, **change))
            runner.assert_not_called()

    def test_max_parameters_and_platform(self):
        source = self.root / "模型.max"
        source.touch()
        request = replace(self.request, input_fbx=source, max_batch=self.exe, max_frame=-12)
        if os.name != "nt":
            with self.assertRaises(ValidationError) as error:
                self.engine.command(request)
            self.assertEqual(error.exception.code, "MAX_PLATFORM_UNSUPPORTED")
            return
        command = self.engine.command(request)
        self.assertEqual(command[command.index("--max-frame") + 1], "-12")
        self.assertEqual(command[command.index("--max-batch") + 1], str(self.exe))
        for changes in (dict(max_frame=None), dict(max_frame=True), dict(max_frame=0.5), dict(max_frame=1000001),
                        dict(max_timeout=0), dict(max_timeout=True), dict(max_batch=None)):
            with self.subTest(changes=changes), self.assertRaises(ValidationError):
                self.engine.command(replace(request, **changes))
        with self.assertRaises(ValidationError):
            self.engine.command(replace(self.request, max_frame=0))

    @unittest.skipUnless(os.name == "nt", "Windows-only MAX client")
    def test_max_report_requires_matching_frame_and_runtime(self):
        source = self.root / "模型.max"
        source.touch()
        request = self.engine.validate(replace(self.request, input_fbx=source, max_batch=self.exe, max_frame=7))
        report = self.success_report(request)
        request.output_gdb.mkdir()
        from geomodelbridge.client import _diagnostics
        with self.assertRaises(ValueError):
            self.engine._verify_report(report, request, _diagnostics(report))
        proof = dict(adapter_protocol_version=1, engine_version=__version__, status="exported",
                     frame=7, source=str(source), batch_executable=str(self.exe))
        def set_proof(value):
            report["reader_diagnostics"] = [dict(severity="warning", code="MAX_ADAPTER_PROVENANCE", message=json.dumps(value), context=str(source))]
        set_proof(proof)
        self.assertEqual(self.engine._verify_report(report, request, _diagnostics(report)), 1)
        for changes in (dict(frame=8), dict(frame=7.0), dict(batch_executable=str(self.engine.writer)),
                        dict(engine_version="0.0.0"), dict(source=str(self.source)),
                        dict(adapter_protocol_version=True), dict(adapter_protocol_version=1.0)):
            set_proof(dict(proof, **changes))
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.engine._verify_report(report, request, _diagnostics(report))

    @unittest.skipUnless(os.name == "nt", "Windows-only MAX client")
    def test_malformed_max_provenance_retains_postprocess_error_context(self):
        source = self.root / "模型.max"
        source.touch()
        request = replace(self.request, input_fbx=source, max_batch=self.exe, max_frame=7)
        proof = dict(adapter_protocol_version=1, engine_version=__version__, status="exported",
                     frame=7, source=str(source), batch_executable=str(self.exe))
        malformed = [json.dumps(dict(proof, **change)) for change in
                     (dict(source=None), dict(source=""), dict(batch_executable=None),
                      dict(batch_executable="\0"), dict(adapter_protocol_version=True),
                      dict(adapter_protocol_version=1.0))]
        depth = sys.getrecursionlimit() + 100
        malformed.append('{"nested":' + '[' * depth + '0' + ']' * depth + '}')
        report_path = Path(str(request.output_gdb) + ".report.json")
        for message in malformed:
            report = self.success_report(request)
            report["reader_diagnostics"] = [dict(severity="info", code="MAX_ADAPTER_PROVENANCE", message=message)]
            fake = self.fake_run(report)
            def run(command):
                result = fake(command)
                return replace(result, stdout_truncated=True, stderr_truncated=True) if "convert" in command else result
            with self.subTest(message=message[:80]):
                try:
                    with patch("geomodelbridge.client._run", side_effect=run), self.assertRaises(ConversionError) as failure:
                        self.engine.convert(request)
                    error = failure.exception
                    self.assertEqual(error.code, "INVALID_REPORT")
                    self.assertEqual(error.exit_code, 0)
                    self.assertEqual(error.report_path, report_path)
                    self.assertEqual(error.diagnostics[0].message, message)
                    self.assertEqual(error.stdout_tail, "done")
                    self.assertEqual(error.stderr_tail, "warning details")
                    self.assertTrue(error.stdout_truncated and error.stderr_truncated)
                    self.assertTrue(request.output_gdb.is_dir())
                    self.assertTrue(report_path.is_file())
                finally:
                    # Only fixtures created in this test's private directory.
                    if request.output_gdb.is_dir():
                        request.output_gdb.rmdir()
                    if report_path.is_file():
                        report_path.unlink()

    def test_existing_outputs_and_reports_are_preserved(self):
        for path in (self.request.output_gdb, Path(str(self.request.output_gdb) + ".report.json")):
            with self.subTest(path=path):
                path.write_bytes(b"keep me")
                with self.assertRaises(ValidationError) as failure:
                    self.engine.convert(self.request)
                self.assertEqual(failure.exception.code, "PATH_EXISTS")
                self.assertEqual(path.read_bytes(), b"keep me")
                path.unlink()  # Created by this test in its private TemporaryDirectory.

    def test_symlink_rejected(self):
        alias = self.root / "alias.fbx"
        try:
            alias.symlink_to(self.source)
        except OSError:
            self.skipTest("Host does not allow unprivileged symlinks")
        with self.assertRaises(ValidationError):
            self.engine.validate(replace(self.request, input_fbx=alias))

    def test_explicit_writer_and_environment_are_deterministic(self):
        with patch.dict(os.environ, {"GMB_NATIVE_WRITER": "untrusted-other-writer"}):
            self.assertEqual(Engine(self.exe).writer, self.engine.writer)
        self.assertEqual(Engine(self.exe, writer=self.exe).writer, self.exe)

    def test_probe_success(self):
        with patch("geomodelbridge.client._run", side_effect=self.fake_run()):
            result = self.engine.check()
        self.assertEqual(result.version, __version__)
        self.assertFalse(result.arcgis_pro_required)

    def test_version_and_backend_failures(self):
        invalid = [_ProcessResult(1, "", "bad version"), _ProcessResult(0, "GeoModelBridge V999.0.0", "")]
        for result in invalid:
            with self.subTest(result=result), patch("geomodelbridge.client._run", return_value=result), self.assertRaises(GeoModelBridgeError) as failure:
                self.engine.check()
            self.assertEqual(failure.exception.code, "VERSION_MISMATCH")
        for probe in ("not json", "{}", '{"status":"available","status":"available"}',
                      json.dumps(dict(version=__version__, status="available", backend="native-filegdb", arcgis_pro_required=True))):
            with patch("geomodelbridge.client._run", side_effect=[_ProcessResult(0, f"GeoModelBridge V{__version__}", ""), _ProcessResult(0, probe, "")]), self.assertRaises(GeoModelBridgeError) as failure:
                self.engine.check()
            self.assertEqual(failure.exception.code, "BACKEND_UNAVAILABLE")

    def test_missing_executable(self):
        with self.assertRaises(GeoModelBridgeError) as failure:
            Engine(self.root / "missing.exe").check()
        self.assertEqual(failure.exception.code, "ENGINE_UNAVAILABLE")

    def test_truncated_probe_output_cannot_masquerade_as_valid_json(self):
        version = _ProcessResult(0, f"GeoModelBridge V{__version__}", "")
        probe = self.fake_run()(["--probe"])
        for responses in ([replace(version, stdout_truncated=True)], [version, replace(probe, stdout_truncated=True)]):
            with patch("geomodelbridge.client._run", side_effect=responses), self.assertRaises(GeoModelBridgeError) as failure:
                self.engine.check()
            self.assertTrue(failure.exception.stdout_truncated)

    def test_launch_failure(self):
        with patch("geomodelbridge.client._run", side_effect=OSError("access denied")), self.assertRaises(GeoModelBridgeError) as failure:
            self.engine.check()
        self.assertEqual(failure.exception.code, "LAUNCH_FAILED")

    def test_verified_result_and_main_thread_grouped_callbacks(self):
        report = self.success_report()
        report["reader_diagnostics"] = [dict(severity="warning", code="MISSING_TEXTURE_FALLBACK", message="missing", context="material:a")] * 100
        events, thread = [], threading.get_ident()
        def callback(event):
            self.assertEqual(threading.get_ident(), thread)
            events.append(event)
        with patch("geomodelbridge.client._run", side_effect=self.fake_run(report)):
            result = self.engine.convert(self.request, on_message=callback)
        self.assertEqual(result.feature_class_path, self.request.output_gdb / "Models")
        self.assertEqual(result.request.origin, (100., 100., 100.))
        self.assertEqual(result.feature_count, 1)
        self.assertEqual(len(result.diagnostics), 100)
        self.assertEqual([e.code for e in events], ["CHECKING_ENGINE", "CONVERTING", "MISSING_TEXTURE_FALLBACK", "VERIFIED"])
        self.assertEqual(events[2].count, 100)
        self.assertEqual(self.source.read_bytes(), b"source must remain unchanged")

    def test_callback_conflict_is_revalidated(self):
        def callback(event):
            if event.code == "CONVERTING":
                self.request.output_gdb.mkdir()
        with patch("geomodelbridge.client._run", side_effect=self.fake_run()) as runner, self.assertRaises(ValidationError):
            self.engine.convert(self.request, on_message=callback)
        self.assertEqual(runner.call_count, 2)

    def test_callback_exception_before_start_propagates(self):
        with patch("geomodelbridge.client._run") as runner, self.assertRaises(CallbackError) as failure:
            self.engine.convert(self.request, on_message=lambda event: (_ for _ in ()).throw(RuntimeError("callback")))
        runner.assert_not_called()
        self.assertIsNone(failure.exception.result)
        self.assertEqual(str(failure.exception.__cause__), "callback")

    def test_callback_failure_after_commit_retains_verified_result(self):
        report = self.success_report()
        report["reader_diagnostics"] = [dict(severity="warning", code="MISSING_TEXTURE_FALLBACK", message="missing")]
        for stage in ("MISSING_TEXTURE_FALLBACK", "VERIFIED"):
            def callback(event):
                if event.code == stage:
                    raise RuntimeError("host message delivery failed")
            with patch("geomodelbridge.client._run", side_effect=self.fake_run(report)), self.assertRaises(CallbackError) as failure:
                self.engine.convert(self.request, on_message=callback)
            error = failure.exception
            self.assertEqual(error.code, "CALLBACK_FAILED")
            self.assertEqual(error.event.code, stage)
            self.assertEqual(error.result.feature_count, 1)
            self.assertTrue(error.result.output_gdb.is_dir())
            self.assertTrue(error.result.report_path.is_file())
            self.assertEqual(error.result.diagnostics, error.diagnostics)
            self.assertEqual(error.exit_code, 0)
            self.assertIsInstance(error.__cause__, RuntimeError)
            error.result.output_gdb.rmdir()
            error.result.report_path.unlink()

    def test_invalid_callback_is_rejected_before_process_start(self):
        with patch("geomodelbridge.client._run") as runner, self.assertRaises(ValidationError):
            self.engine.convert(self.request, on_message="not callable")
        runner.assert_not_called()

    def test_nonzero_exit_keeps_diagnostics_even_with_invalid_or_success_report(self):
        for code in (2, 3, 4, 5, 6, 123):
            report = dict(status="rejected", diagnostics=[dict(severity="error", code="INVALID_NORMAL", message="invalid", context="mesh")])
            with patch("geomodelbridge.client._run", side_effect=self.fake_run(report, code, create_gdb=False)), self.assertRaises(ConversionError) as failure:
                self.engine.convert(self.request)
            self.assertEqual(failure.exception.exit_code, code)
            self.assertEqual(failure.exception.diagnostics[0].code, "INVALID_NORMAL")
            self.assertEqual(failure.exception.stderr_tail, "warning details")
            failure.exception.report_path.unlink()
        with patch("geomodelbridge.client._run", side_effect=self.fake_run(code=5)), self.assertRaises(ConversionError):
            self.engine.convert(self.request)

    def test_exit_zero_without_report_or_gdb_is_failure(self):
        for create, write in ((False, True), (True, False)):
            with patch("geomodelbridge.client._run", side_effect=self.fake_run(create_gdb=create, write_report=write)), self.assertRaises(ConversionError) as failure:
                self.engine.convert(self.request)
            self.assertEqual(failure.exception.code, "INVALID_REPORT")
            if create:
                self.request.output_gdb.rmdir()
            if write:
                failure.exception.report_path.unlink()

    def test_report_link_created_by_process_is_not_a_verified_result(self):
        target = self.root / "other-report.json"
        target.write_text(json.dumps(self.success_report()), encoding="utf-8")
        alias = self.root / "symlink-check"
        try:
            alias.symlink_to(target)
        except OSError:
            self.skipTest("Host does not allow unprivileged symlinks")
        alias.unlink()
        fake = self.fake_run(write_report=False)
        def run(command):
            result = fake(command)
            if "convert" in command:
                Path(str(self.request.output_gdb) + ".report.json").symlink_to(target)
            return result
        with patch("geomodelbridge.client._run", side_effect=run), self.assertRaises(ConversionError) as failure:
            self.engine.convert(self.request)
        self.assertEqual(failure.exception.code, "INVALID_REPORT")
        self.assertEqual(json.loads(target.read_text(encoding="utf-8")), self.success_report())

    def test_report_parent_link_created_by_process_is_not_verified(self):
        parent = self.root / "reports"
        target = self.root / "moved-reports"
        parent.mkdir()
        request = replace(self.request, report_path=parent / "result.json")
        fake = self.fake_run()
        def run(command):
            result = fake(command)
            if "convert" in command:
                parent.rename(target)
                if os.name == "nt":
                    import _winapi
                    _winapi.CreateJunction(str(target), str(parent))
                else:
                    parent.symlink_to(target, target_is_directory=True)
            return result
        try:
            with patch("geomodelbridge.client._run", side_effect=run), self.assertRaises(ConversionError) as failure:
                self.engine.convert(request)
            self.assertEqual(failure.exception.code, "INVALID_REPORT")
            self.assertTrue((target / "result.json").is_file())
        finally:
            if os.name == "nt" and os.path.lexists(parent):
                parent.rmdir()  # Remove only the junction created by this test.
            elif parent.is_symlink():
                parent.unlink()

    @unittest.skipUnless(hasattr(os, "mkfifo"), "FIFO report regression requires POSIX")
    def test_nonregular_report_is_rejected_without_blocking_on_open(self):
        fake = self.fake_run(write_report=False)
        def run(command):
            result = fake(command)
            if "convert" in command:
                os.mkfifo(str(self.request.output_gdb) + ".report.json")
            return result
        with patch("geomodelbridge.client._run", side_effect=run), self.assertRaises(ConversionError) as failure:
            self.engine.convert(self.request)
        self.assertEqual(failure.exception.code, "INVALID_REPORT")

    def test_report_must_match_complete_contract(self):
        mutations = [lambda r: r.update(status="prepared"), lambda r: r.update(version="0.0.0"),
                     lambda r: r.update(backend="arcgis-pro"), lambda r: r.update(conversion_profile="gis-static"),
                     lambda r: r.update(missing_texture_policy="error"), lambda r: r.update(feature_class="Other"),
                     lambda r: r.update(output="relative.gdb"), lambda r: r.update(output=str(self.root / "other.gdb")),
                     lambda r: r.update(source=str(self.root / "other.fbx")),
                     lambda r: r["verification"].update(level="not_reopened"),
                     lambda r: r["verification"].update(geometry_material_uv_texture_readback=False),
                     lambda r: r["verification"].update(feature_count=True), lambda r: r["verification"].update(feature_count=0),
                     lambda r: r["coordinate_system"].update(wkid=4326), lambda r: r["coordinate_system"].update(projected=False),
                     lambda r: r["coordinate_system"].update(unit="feet"),
                     lambda r: r.pop("coordinates"), lambda r: r.update(coordinates=[]),
                     lambda r: r["coordinates"].update(origin=[100, 100, 99]),
                     lambda r: r["coordinates"].update(origin=[100, 100]),
                     lambda r: r["coordinates"].update(origin=[100, 100, True]),
                     lambda r: r["coordinates"].update(origin_explicit=False),
                     lambda r: r["coordinates"].update(wkid=4326),
                     lambda r: r["coordinates"].update(unit="feet"),
                     lambda r: r["coordinates"].update(up_axis="Y"),
                     lambda r: r.update(verification=[]), lambda r: r.update(coordinate_system=[]),
                     lambda r: r.pop("reader_diagnostics"), lambda r: r.update(reader_diagnostics={}),
                     lambda r: r.update(reader_diagnostics=[dict(severity="error", code="BAD", message="error")]),
                     lambda r: r.update(reader_diagnostics=[dict(severity="unknown", code="BAD", message="bad")]),
                     lambda r: r.update(diagnostics=[dict(severity="error", code="WRITE_FAILED", message="failed")]),
                     lambda r: r.update(diagnostics={})]
        mutations.extend([
            lambda r: r["verification"].pop("checks"),
            lambda r: r["verification"].update(checks=[]),
            lambda r: r["verification"].update(checks={}),
            lambda r: r["verification"]["checks"][0].update(passed=False),
            lambda r: r["verification"]["checks"][0].update(passed=1),
            lambda r: r["verification"]["checks"][0].update(mesh_index=True),
            lambda r: r["verification"]["checks"][0].update(mesh_index=-1),
            lambda r: r["verification"]["checks"][0].update(mesh_index=1),
            lambda r: r["verification"].update(feature_count=2, checks=[dict(mesh_index=0, passed=True)] * 2),
            lambda r: r["coordinate_system"].pop("source_coordinates_assigned_without_reprojection"),
            lambda r: r["coordinate_system"].update(source_coordinates_assigned_without_reprojection=False),
            lambda r: r.update(reader_diagnostics=[dict(severity="warning", code="", message="blank code")]),
        ])
        for mutation in mutations:
            report = self.success_report()
            mutation(report)
            with self.subTest(report=report), patch("geomodelbridge.client._run", side_effect=self.fake_run(report)), self.assertRaises(ConversionError) as failure:
                self.engine.convert(self.request)
            self.assertEqual(failure.exception.code, "INVALID_REPORT")
            self.request.output_gdb.rmdir()
            failure.exception.report_path.unlink()

    def test_error_policy_rejects_fallback_in_success_report(self):
        request = replace(self.request, missing_textures="error")
        report = self.success_report(request)
        report["reader_diagnostics"] = [dict(severity="warning", code="MISSING_TEXTURE_FALLBACK", message="missing")]
        with patch("geomodelbridge.client._run", side_effect=self.fake_run(report)), self.assertRaises(ConversionError):
            self.engine.convert(request)

    def test_malformed_duplicate_and_oversize_reports_reject(self):
        for contents in (b"{", b"[]", b'{"status":"a","status":"b"}', b'{"a":NaN}', b'{"a":1e999}', b"x" * 101):
            def run(command):
                result = self.fake_run()(command)
                if "convert" in command:
                    Path(str(self.request.output_gdb) + ".report.json").write_bytes(contents)
                return result
            with patch("geomodelbridge.client.REPORT_LIMIT_BYTES", 100), patch("geomodelbridge.client._run", side_effect=run), self.assertRaises(ConversionError) as failure:
                self.engine.convert(self.request)
            self.assertEqual(failure.exception.code, "INVALID_REPORT")
            self.request.output_gdb.rmdir()
            failure.exception.report_path.unlink()

    def test_process_argument_safety_utf8_and_bounded_log_tails(self):
        value = '路径 with spaces & $(echo no); "literal"'
        code = "import os,sys; os.write(1, b'x' * 9000000); os.write(2, b'y' * 9000000); os.write(1, sys.argv[1].encode('utf8')); os.write(2, '末尾'.encode('utf8'))"
        tracemalloc.start()
        try:
            result = _run([sys.executable, "-c", code, value])
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertLess(peak, 4 * 1024 * 1024)
        self.assertEqual(result.exit_code, 0)
        self.assertTrue(result.stdout.endswith(value))
        self.assertTrue(result.stderr.endswith("末尾"))
        self.assertLessEqual(len(result.stdout.encode("utf8")), LOG_TAIL_BYTES)
        self.assertLessEqual(len(result.stderr.encode("utf8")), LOG_TAIL_BYTES)
        self.assertTrue(result.stdout_truncated and result.stderr_truncated)
        self.assertFalse(any(t.name in ("gmb-stdout", "gmb-stderr") for t in threading.enumerate()))

    def test_short_and_empty_process_logs_remain_exact(self):
        result = _run([sys.executable, "-c", "import os; os.write(1, '中文'.encode('utf8'))"])
        self.assertEqual(result.stdout, "中文")
        self.assertEqual(result.stderr, "")
        self.assertFalse(result.stdout_truncated or result.stderr_truncated)

    def test_process_launch_failure_does_not_leave_reader_threads(self):
        with self.assertRaises(OSError):
            _run([self.root / "absent-engine"])
        self.assertFalse(any(t.name in ("gmb-stdout", "gmb-stderr") for t in threading.enumerate()))

    def test_reader_start_failure_never_launches_engine(self):
        original_start = threading.Thread.start
        calls = []
        def start(reader):
            calls.append(reader.name)
            if len(calls) == 2:
                raise RuntimeError("thread resource unavailable")
            original_start(reader)
        with patch("geomodelbridge.client.threading.Thread.start", start), patch("geomodelbridge.client.subprocess.Popen") as spawn:
            with self.assertRaises(GeoModelBridgeError) as failure:
                self.engine.check()
        spawn.assert_not_called()
        self.assertEqual(failure.exception.code, "LOG_READ_FAILED")
        self.assertFalse(any(t.name in ("gmb-stdout", "gmb-stderr") for t in threading.enumerate()))

    def test_log_truncation_is_visible_on_results_and_errors(self):
        def run(command):
            result = self.fake_run()(command)
            return replace(result, stdout_truncated=True, stderr_truncated=True) if "convert" in command else result
        with patch("geomodelbridge.client._run", side_effect=run):
            result = self.engine.convert(self.request)
        self.assertTrue(result.stdout_truncated and result.stderr_truncated)
        result.output_gdb.rmdir()
        result.report_path.unlink()
        def fail(command):
            result = run(command)
            return replace(result, exit_code=5) if "convert" in command else result
        with patch("geomodelbridge.client._run", side_effect=fail), self.assertRaises(ConversionError) as failure:
            self.engine.convert(self.request)
        self.assertTrue(failure.exception.stdout_truncated and failure.exception.stderr_truncated)


class InspectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gmb-inspect-中文 空格-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "模型 & 数据.fbx"
        self.source.write_bytes(b"unchanged source")
        self.exe = self.root / ("geomodelbridge.exe" if os.name == "nt" else "geomodelbridge")
        self.exe.touch()
        self.engine = Engine(self.exe)
        self.request = InspectionRequest(self.source, self.root / "模型 report.json")

    def success_report(self, request=None):
        request = request or self.request
        return dict(schema_version=1, status="inspected", version=__version__, backend="none",
                    source=str(request.input_model), conversion_profile=request.profile,
                    missing_texture_policy=request.missing_textures,
                    coordinates=dict(space="local", unit="meter", up_axis="Z", wkid=0,
                                     origin=[0, 0, 0], origin_explicit=False),
                    counts=dict(meshes=1, triangles=2, corner_vertices=6, materials=1, textures=0, texture_bytes=0),
                    geometry_bounds=dict(min=[-1, -2, -3], max=[4, 5, 6]), diagnostics=[],
                    fidelity=dict(validation_passed=True, strict_validation_passed=request.profile == "strict",
                                  compatibility_adjustments=False, gdb_written=False, gdb_readback_verified=False))

    def fake_run(self, report=None, code=0, write_report=True, truncated=False):
        def run(command):
            values = tuple(map(str, command))
            if values == (str(self.exe), "--version"):
                return _ProcessResult(0, f"GeoModelBridge V{__version__}\n", "")
            self.assertEqual(values[:2], (str(self.exe), "inspect"))
            self.assertNotIn("--writer", values)
            self.assertNotIn("--output", values)
            if write_report:
                destination = Path(values[values.index("--report") + 1])
                # The fake child observes the same exclusive-create contract.
                with destination.open("x", encoding="utf-8") as stream:
                    json.dump(report if report is not None else self.success_report(), stream)
            return _ProcessResult(code, "inspection stdout", "inspection stderr", truncated, truncated)
        return run

    def test_readonly_validation_and_argument_boundaries_without_writer(self):
        before = set(self.root.iterdir())
        request = replace(self.request, texture_dirs=[self.root], profile="gis-static", missing_textures="error")
        with patch("geomodelbridge.client._run") as runner:
            normalized = self.engine.validate_inspection(request)
            command = self.engine.inspection_command(request)
        runner.assert_not_called()
        self.assertEqual(set(self.root.iterdir()), before)
        self.assertEqual(command[:3], (str(self.exe), "inspect", str(self.source)))
        self.assertEqual(command[command.index("--report") + 1], str(request.report_path))
        self.assertEqual(command[command.index("--texture-dir") + 1], str(self.root))
        self.assertEqual(command[command.index("--profile") + 1], "gis-static")
        self.assertEqual(command[command.index("--missing-textures") + 1], "error")
        for omitted in ("--output", "--writer", "--backend", "--origin", "--wkid", "--feature-class"):
            self.assertNotIn(omitted, command)
        self.assertEqual(normalized.texture_dirs, (self.root,))
        self.assertFalse(self.engine.writer.exists())

    def test_validated_result_is_immutable_and_never_loads_writer(self):
        report = self.success_report()
        report["diagnostics"] = [dict(severity="warning", code="MISSING_TEXTURE_FALLBACK", message="missing")] * 3
        events = []
        thread = threading.get_ident()
        def callback(event):
            self.assertEqual(threading.get_ident(), thread)
            events.append(event)
        with patch("geomodelbridge.client._run", side_effect=self.fake_run(report, truncated=True)) as runner:
            result = self.engine.inspect(self.request, on_message=callback)
        self.assertEqual(runner.call_count, 2)
        self.assertIsInstance(result, InspectionResult)
        self.assertEqual(result.counts, InspectionCounts(1, 2, 6, 1, 0, 0))
        self.assertEqual(result.bounds, InspectionBounds((-1., -2., -3.), (4., 5., 6.)))
        self.assertEqual(result.backend, "none")
        self.assertEqual(result.version, __version__)
        self.assertEqual([e.code for e in events], ["CHECKING_ENGINE", "INSPECTING", "MISSING_TEXTURE_FALLBACK", "INSPECTED"])
        self.assertEqual(events[2].count, 3)
        self.assertTrue(result.stdout_truncated and result.stderr_truncated)
        self.assertEqual(result.stdout_tail, "inspection stdout")
        self.assertEqual(result.stderr_tail, "inspection stderr")
        self.assertEqual(self.source.read_bytes(), b"unchanged source")
        self.assertFalse(self.engine.writer.exists())
        with self.assertRaises(FrozenInstanceError):
            result.counts.meshes = 2
        with self.assertRaises(TypeError):
            result.bounds.minimum[0] = 5

    def test_obj_and_max_command_options_share_conversion_rules(self):
        source = self.root / "source.OBJ"
        source.touch()
        command = self.engine.inspection_command(replace(self.request, input_model=source, obj_up_axis="Y", obj_unit_meters=0.01))
        self.assertEqual(command[command.index("--obj-up-axis") + 1], "Y")
        self.assertEqual(command[command.index("--obj-unit-meters") + 1], "0.01")
        source = self.root / "source.max"
        source.touch()
        request = replace(self.request, input_model=source, max_batch=self.exe, max_frame=-7, max_timeout=90)
        if os.name != "nt":
            with self.assertRaises(ValidationError) as failure:
                self.engine.inspection_command(request)
            self.assertEqual(failure.exception.code, "MAX_PLATFORM_UNSUPPORTED")
        else:
            command = self.engine.inspection_command(request)
            self.assertEqual(command[command.index("--max-batch") + 1], str(self.exe))
            self.assertEqual(command[command.index("--max-frame") + 1], "-7")
            self.assertEqual(command[command.index("--max-timeout") + 1], "90")

    def test_invalid_requests_never_start_process_or_create_outputs(self):
        changes = [dict(input_model=""), dict(input_model="\0"), dict(input_model=self.root),
                   dict(input_model=self.root / "missing.fbx"), dict(input_model=self.exe),
                   dict(report_path=None), dict(report_path=""), dict(report_path=self.root / "missing/r.json"),
                   dict(report_path=self.root / "output.GDB"), dict(report_path=self.root / "nested.gdb/r.json"),
                   dict(profile="other"), dict(missing_textures="ignore"), dict(texture_dirs=str(self.root)),
                   dict(texture_dirs=(self.root / "missing",)), dict(obj_up_axis="X"),
                   dict(obj_unit_meters=0), dict(obj_unit_meters=True), dict(obj_unit_meters=float("nan")),
                   dict(obj_unit_meters=10**400), dict(max_frame=1), dict(max_timeout=2), dict(max_batch=self.exe)]
        before = set(self.root.iterdir())
        for changeset in changes:
            with self.subTest(changeset=changeset), patch("geomodelbridge.client._run") as runner:
                with self.assertRaises(ValidationError):
                    self.engine.inspect(replace(self.request, **changeset))
                runner.assert_not_called()
            self.assertEqual(set(self.root.iterdir()), before)
        with patch("geomodelbridge.client._run") as runner, self.assertRaises(ValidationError):
            self.engine.inspect(self.request, on_message="invalid")
        runner.assert_not_called()
        with self.assertRaises(ValidationError):
            self.engine.validate_inspection(object())

    def test_existing_report_source_and_callback_collisions_are_preserved(self):
        for path in (self.request.report_path, self.source):
            original = path.read_bytes() if path.exists() else None
            if original is None:
                path.write_bytes(b"existing report")
            with patch("geomodelbridge.client._run") as runner, self.assertRaises(ValidationError) as failure:
                self.engine.inspect(replace(self.request, report_path=path))
            runner.assert_not_called()
            self.assertEqual(failure.exception.code, "PATH_EXISTS")
            self.assertEqual(path.read_bytes(), original if original is not None else b"existing report")
            if original is None:
                path.unlink()
        def callback(event):
            if event.code == "INSPECTING":
                self.request.report_path.write_bytes(b"created by callback")
        with patch("geomodelbridge.client._run", side_effect=self.fake_run()) as runner, self.assertRaises(ValidationError):
            self.engine.inspect(self.request, on_message=callback)
        self.assertEqual(runner.call_count, 1)
        self.assertEqual(self.request.report_path.read_bytes(), b"created by callback")

    def test_request_snapshot_survives_callback_cwd_and_list_changes(self):
        previous_cwd = Path.cwd()
        texture_dirs = ["."]
        report = self.success_report()
        def callback(event):
            if event.code == "CHECKING_ENGINE":
                texture_dirs[:] = ["missing"]
                os.chdir(previous_cwd)
        try:
            os.chdir(self.root)
            request = replace(self.request, input_model=self.source.name,
                              report_path=self.request.report_path.name, texture_dirs=texture_dirs)
            with patch("geomodelbridge.client._run", side_effect=self.fake_run(report)) as runner:
                result = self.engine.inspect(request, on_message=callback)
            command = tuple(map(str, runner.call_args.args[0]))
            self.assertEqual(result.request.input_model, self.source)
            self.assertEqual(result.request.report_path, self.request.report_path)
            self.assertEqual(result.request.texture_dirs, (self.root,))
            self.assertEqual(command[command.index("--texture-dir") + 1], str(self.root))
        finally:
            os.chdir(previous_cwd)

    def test_version_mismatch_and_missing_engine_never_inspect(self):
        for result in (_ProcessResult(0, "GeoModelBridge V0.0.0", "version error"),
                       _ProcessResult(3, f"GeoModelBridge V{__version__}", "failed"),
                       _ProcessResult(0, f"GeoModelBridge V{__version__}", "", True, False)):
            with patch("geomodelbridge.client._run", return_value=result) as runner, self.assertRaises(GeoModelBridgeError) as failure:
                self.engine.inspect(self.request)
            self.assertEqual(failure.exception.code, "VERSION_MISMATCH")
            self.assertEqual(runner.call_count, 1)
            self.assertFalse(self.request.report_path.exists())
        with patch("geomodelbridge.client._run") as runner, self.assertRaises(GeoModelBridgeError) as failure:
            Engine(self.root / "absent.exe").inspect(self.request)
        self.assertEqual(failure.exception.code, "ENGINE_UNAVAILABLE")
        runner.assert_not_called()

    def test_process_failure_retains_diagnostics_and_both_log_contexts(self):
        report = dict(status="rejected", diagnostics=[dict(severity="error", code="INVALID_NORMAL", message="invalid")])
        for code in (2, 3, 4, 5, 6):
            with self.subTest(code=code), patch("geomodelbridge.client._run", side_effect=self.fake_run(report, code, truncated=True)):
                with self.assertRaises(InspectionError) as failure:
                    self.engine.inspect(self.request)
            error = failure.exception
            self.assertEqual(error.code, "PROCESS_FAILED")
            self.assertEqual(error.exit_code, code)
            self.assertEqual(error.report_path, self.request.report_path)
            self.assertEqual(error.diagnostics[0].code, "INVALID_NORMAL")
            self.assertEqual(error.stdout_tail, "inspection stdout")
            self.assertEqual(error.stderr_tail, "inspection stderr")
            self.assertTrue(error.stdout_truncated and error.stderr_truncated)
            self.request.report_path.unlink()

    def test_inspection_launch_failure_retains_report_destination(self):
        def run(command):
            if "inspect" in command:
                raise OSError("launch denied")
            return self.fake_run()(command)
        with patch("geomodelbridge.client._run", side_effect=run), self.assertRaises(GeoModelBridgeError) as failure:
            self.engine.inspect(self.request)
        self.assertEqual(failure.exception.code, "LAUNCH_FAILED")
        self.assertEqual(failure.exception.report_path, self.request.report_path)
        self.assertFalse(self.request.report_path.exists())

    def test_callbacks_preserve_verified_result_or_prevent_process_start(self):
        def always_fail(event):
            raise RuntimeError("callback failure")
        with patch("geomodelbridge.client._run") as runner, self.assertRaises(CallbackError) as failure:
            self.engine.inspect(self.request, on_message=always_fail)
        runner.assert_not_called()
        self.assertIsNone(failure.exception.result)
        report = self.success_report()
        report["diagnostics"] = [dict(severity="warning", code="MISSING_TEXTURE_FALLBACK", message="missing")]
        for stage in ("MISSING_TEXTURE_FALLBACK", "INSPECTED"):
            def callback(event):
                if event.code == stage:
                    raise RuntimeError("delivery failure")
            with patch("geomodelbridge.client._run", side_effect=self.fake_run(report, truncated=True)):
                with self.assertRaises(CallbackError) as failure:
                    self.engine.inspect(self.request, on_message=callback)
            error = failure.exception
            self.assertEqual(error.code, "CALLBACK_FAILED")
            self.assertIsInstance(error.result, InspectionResult)
            self.assertEqual(error.result.counts.triangles, 2)
            self.assertEqual(error.result.diagnostics, error.diagnostics)
            self.assertTrue(error.result.report_path.is_file())
            self.assertTrue(error.stdout_truncated and error.stderr_truncated)
            self.assertEqual(error.exit_code, 0)
            self.assertIsInstance(error.__cause__, RuntimeError)
            self.request.report_path.unlink()

    def test_complete_report_contract_rejects_malformed_claims(self):
        mutations = [lambda r: r.update(status="prepared"), lambda r: r.update(backend="native-filegdb"),
                     lambda r: r.update(version="0.0.0"), lambda r: r.update(schema_version=True),
                     lambda r: r.update(schema_version=1.0), lambda r: r.update(schema_version=2),
                     lambda r: r.update(source="relative.fbx"), lambda r: r.update(source=str(self.exe)),
                     lambda r: r.update(source=None), lambda r: r.update(conversion_profile="gis-static"),
                     lambda r: r.update(missing_texture_policy="error"), lambda r: r.pop("coordinates"),
                     lambda r: r.update(coordinates=[]), lambda r: r["coordinates"].update(wkid=True),
                     lambda r: r["coordinates"].update(wkid=3857), lambda r: r["coordinates"].update(space="referenced"),
                     lambda r: r["coordinates"].update(unit="feet"), lambda r: r["coordinates"].update(up_axis="Y"),
                     lambda r: r["coordinates"].update(origin_explicit=True),
                     lambda r: r["coordinates"].update(origin_explicit=0),
                     lambda r: r["coordinates"].update(origin=[0, 0, True]),
                     lambda r: r["coordinates"].update(origin=[0, 0, 1]),
                     lambda r: r["coordinates"].update(origin=[0, 0]), lambda r: r.pop("fidelity"),
                     lambda r: r.update(fidelity=[]), lambda r: r["fidelity"].update(validation_passed=1),
                     lambda r: r["fidelity"].update(gdb_written=True), lambda r: r["fidelity"].update(gdb_written=0),
                     lambda r: r["fidelity"].update(gdb_readback_verified=True),
                     lambda r: r["fidelity"].update(strict_validation_passed=1),
                     lambda r: r["fidelity"].update(compatibility_adjustments=0),
                     lambda r: r["fidelity"].update(compatibility_adjustments=True),
                     lambda r: r.pop("diagnostics"), lambda r: r.update(diagnostics={}),
                     lambda r: r.update(diagnostics=[dict(severity="error", code="BAD", message="failure")]),
                     lambda r: r.update(diagnostics=[dict(severity="invalid", code="BAD", message="failure")]),
                     lambda r: r.update(diagnostics=[dict(severity="warning", code="", message="failure")]),
                     lambda r: r.update(counts=[]), lambda r: r.pop("counts"), lambda r: r.pop("geometry_bounds"),
                     lambda r: r.update(geometry_bounds=[]), lambda r: r["geometry_bounds"].pop("min"),
                     lambda r: r["geometry_bounds"].update(min=[0, 0]),
                     lambda r: r["geometry_bounds"].update(min=[False, 0, 0]),
                     lambda r: r["geometry_bounds"].update(min=[0, "0", 0]),
                     lambda r: r["geometry_bounds"].update(min=[7, 0, 0]),
                     lambda r: r["geometry_bounds"].update(max=[0, 0, 10**400]),
                     lambda r: r["counts"].update(meshes=3), lambda r: r["counts"].update(corner_vertices=2),
                     lambda r: r["counts"].update(textures=1),
                     lambda r: r["counts"].update(textures=2, texture_bytes=1)]
        for key in ("meshes", "triangles", "corner_vertices", "materials", "textures", "texture_bytes"):
            for value in (-1, True, 1.0, None):
                mutations.append(lambda r, key=key, value=value: r["counts"].update({key: value}))
        for key in ("meshes", "triangles", "corner_vertices", "materials"):
            mutations.append(lambda r, key=key: r["counts"].update({key: 0}))
        for index, mutate in enumerate(mutations):
            report = self.success_report()
            mutate(report)
            with self.subTest(mutation=index), patch("geomodelbridge.client._run", side_effect=self.fake_run(report, truncated=True)):
                with self.assertRaises(InspectionError) as failure:
                    self.engine.inspect(self.request)
            error = failure.exception
            self.assertEqual(error.code, "INVALID_REPORT")
            self.assertEqual(error.exit_code, 0)
            self.assertEqual(error.report_path, self.request.report_path)
            self.assertEqual(error.stdout_tail, "inspection stdout")
            self.assertEqual(error.stderr_tail, "inspection stderr")
            self.assertTrue(error.stdout_truncated and error.stderr_truncated)
            self.assertTrue(self.request.report_path.exists())
            self.request.report_path.unlink()

    def test_strict_mapped_shading_and_gis_static_accept_reported_adjustments(self):
        for profile in ("strict", "gis-static"):
            request = replace(self.request, profile=profile)
            report = self.success_report(request)
            report["fidelity"].update(strict_validation_passed=False, compatibility_adjustments=True)
            report["diagnostics"] = [dict(severity="warning", code="UNLIT_SHADING_MAPPED", message="mapped")]
            with patch("geomodelbridge.client._run", side_effect=self.fake_run(report)):
                result = self.engine.inspect(request)
            self.assertEqual(result.request.profile, profile)
            self.request.report_path.unlink()

    def test_error_policy_rejects_fallback_claim(self):
        request = replace(self.request, missing_textures="error")
        report = self.success_report(request)
        report["diagnostics"] = [dict(severity="warning", code="MISSING_TEXTURE_FALLBACK", message="missing")]
        with patch("geomodelbridge.client._run", side_effect=self.fake_run(report)), self.assertRaises(InspectionError) as failure:
            self.engine.inspect(request)
        self.assertEqual(failure.exception.code, "INVALID_REPORT")
        self.assertEqual(failure.exception.diagnostics[0].code, "MISSING_TEXTURE_FALLBACK")

    def test_missing_malformed_duplicate_oversized_and_nonfinite_reports(self):
        depth = sys.getrecursionlimit() + 100
        for contents in (None, b"{", b"[]", b'{"status":"a","status":"b"}', b'{"a":NaN}', b'{"a":1e999}',
                         b"x" * 101, ('{"a":' + '[' * depth + '0' + ']' * depth + '}').encode()):
            def run(command):
                result = self.fake_run(write_report=False, truncated=True)(command)
                if "inspect" in command and contents is not None:
                    self.request.report_path.write_bytes(contents)
                return result
            with patch("geomodelbridge.client.REPORT_LIMIT_BYTES", 100), patch("geomodelbridge.client._run", side_effect=run):
                with self.assertRaises(InspectionError) as failure:
                    self.engine.inspect(self.request)
            self.assertEqual(failure.exception.code, "INVALID_REPORT")
            self.assertTrue(failure.exception.stdout_truncated and failure.exception.stderr_truncated)
            if self.request.report_path.exists():
                self.request.report_path.unlink()

    def test_linked_input_and_replaced_report_are_rejected(self):
        target = self.root / "target.json"
        target.write_text(json.dumps(self.success_report()), encoding="utf-8")
        alias = self.root / "alias.fbx"
        try:
            alias.symlink_to(self.source)
        except OSError:
            self.skipTest("Host does not allow unprivileged symlinks")
        with patch("geomodelbridge.client._run") as runner, self.assertRaises(ValidationError):
            self.engine.inspect(replace(self.request, input_model=alias))
        runner.assert_not_called()
        self.request.report_path.symlink_to(target)
        with self.assertRaises(ValidationError):
            self.engine.validate_inspection(self.request)
        self.request.report_path.unlink()
        def run(command):
            result = self.fake_run(write_report=False)(command)
            if "inspect" in command:
                self.request.report_path.symlink_to(target)
            return result
        with patch("geomodelbridge.client._run", side_effect=run), self.assertRaises(InspectionError) as failure:
            self.engine.inspect(self.request)
        self.assertEqual(failure.exception.code, "INVALID_REPORT")
        self.assertEqual(json.loads(target.read_text(encoding="utf-8")), self.success_report())

    def test_replaced_report_parent_is_rejected_without_removing_report(self):
        parent, target = self.root / "reports", self.root / "moved-reports"
        parent.mkdir()
        request = replace(self.request, report_path=parent / "result.json")
        def run(command):
            result = self.fake_run()(command)
            if "inspect" in command:
                parent.rename(target)
                if os.name == "nt":
                    import _winapi
                    _winapi.CreateJunction(str(target), str(parent))
                else:
                    parent.symlink_to(target, target_is_directory=True)
            return result
        try:
            with patch("geomodelbridge.client._run", side_effect=run), self.assertRaises(InspectionError) as failure:
                self.engine.inspect(request)
            self.assertEqual(failure.exception.code, "INVALID_REPORT")
            self.assertTrue((target / "result.json").is_file())
        finally:
            if os.name == "nt" and os.path.lexists(parent):
                parent.rmdir()  # Remove only the junction made by this test.
            elif parent.is_symlink():
                parent.unlink()

    def test_nonregular_report_is_rejected_without_reading(self):
        def run(command):
            result = self.fake_run(write_report=False)(command)
            if "inspect" in command:
                self.request.report_path.mkdir()
            return result
        with patch("geomodelbridge.client._run", side_effect=run), self.assertRaises(InspectionError) as failure:
            self.engine.inspect(self.request)
        self.assertEqual(failure.exception.code, "INVALID_REPORT")
        self.assertTrue(self.request.report_path.is_dir())

    @unittest.skipUnless(os.name == "nt", "Windows-only MAX client")
    def test_max_provenance_matches_runtime_frame_source_and_retains_errors(self):
        source = self.root / "source.max"
        source.touch()
        request = replace(self.request, input_model=source, max_batch=self.exe, max_frame=7)
        proof = dict(adapter_protocol_version=1, engine_version=__version__, status="exported", frame=7,
                     source=str(source), batch_executable=str(self.exe))
        report = self.success_report(request)
        report["diagnostics"] = [dict(severity="info", code="MAX_ADAPTER_PROVENANCE", message=json.dumps(proof))]
        with patch("geomodelbridge.client._run", side_effect=self.fake_run(report)):
            result = self.engine.inspect(request)
        self.assertEqual(result.request.max_frame, 7)
        self.request.report_path.unlink()
        messages = [json.dumps(dict(proof, **changes)) for changes in
                    (dict(frame=8), dict(frame=7.0), dict(adapter_protocol_version=True), dict(engine_version="0.0.0"),
                     dict(source=None), dict(source=str(self.exe)), dict(batch_executable="\0"),
                     dict(source="source.max"), dict(batch_executable=self.exe.name))]
        messages.append("{")
        for message in messages:
            report["diagnostics"][0]["message"] = message
            with patch("geomodelbridge.client._run", side_effect=self.fake_run(report, truncated=True)):
                with self.assertRaises(InspectionError) as failure:
                    self.engine.inspect(request)
            self.assertEqual(failure.exception.code, "INVALID_REPORT")
            self.assertEqual(failure.exception.diagnostics[0].message, message)
            self.assertTrue(failure.exception.stdout_truncated and failure.exception.stderr_truncated)
            self.request.report_path.unlink()
        for entries in ([], [dict(severity="info", code="MAX_ADAPTER_PROVENANCE", message=json.dumps(proof))] * 2):
            report["diagnostics"] = entries
            with patch("geomodelbridge.client._run", side_effect=self.fake_run(report)), self.assertRaises(InspectionError) as failure:
                self.engine.inspect(request)
            self.assertEqual(failure.exception.code, "INVALID_REPORT")
            self.request.report_path.unlink()


if __name__ == "__main__":
    unittest.main()
