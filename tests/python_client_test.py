"""Python API contract tests; subprocess boundary is mocked except log/argv checks."""
from dataclasses import replace
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
        for change in (dict(obj_up_axis="X"), dict(obj_unit_meters=0), dict(obj_unit_meters=float("nan"))):
            with self.subTest(change=change), self.assertRaises(ValidationError):
                self.engine.validate(replace(request, **change))

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


if __name__ == "__main__":
    unittest.main()
