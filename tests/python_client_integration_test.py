"""Exercise the INSTALLED Python library through real inspections and GDB conversions."""
import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--install-dir", type=Path, required=True)
parser.add_argument("--work", type=Path, required=True)
args = parser.parse_args()
install, work = args.install_dir.resolve(), args.work.resolve()
work.mkdir(parents=True, exist_ok=False)
sys.path.insert(0, str(install / "python"))
import geomodelbridge
from geomodelbridge import (CallbackError, ConversionRequest, ConversionError, Engine,
                            InspectionError, InspectionRequest, ValidationError)
assert Path(geomodelbridge.__file__).resolve().is_relative_to(install / "python")
assert "arcpy" not in sys.modules
engine = Engine(install / "bin" / ("geomodelbridge.exe" if os.name == "nt" else "geomodelbridge"))
assert not engine.check().arcgis_pro_required
fixtures = Path(__file__).resolve().parent / "fixtures"
source_dir = work / "中文 模型 & 输入"
source_dir.mkdir()
for name in ("textured_quad.fbx", "textured_quad.obj", "textured_quad.glb", "textured_quad.gltf", "textured_quad.bin", "textured_quad.wrl", "textured_quad.dae", "textured_quad.mtl", "checker.png", "missing_texture.fbx"):
    shutil.copy2(fixtures / name, source_dir / name)
base = (fixtures / "textured_quad.fbx").read_text(encoding="utf-8")
broken = re.sub(r"Normals: \*\d+ \{ a: [^}]+", "Normals: *12 { a: " + ",".join(["0"] * 12) + " ", base)
normal_file = source_dir / "无效法线.fbx"
normal_file.write_text(broken, encoding="utf-8")
hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in source_dir.iterdir()}
results = []
verified_gdbs = []
request = ConversionRequest(source_dir / "textured_quad.fbx", work / "贴图 输出.gdb", 3857, (100, 100, 100), feature_class="ImportedFBX")

def verify_copy(result):
    report = json.loads(result.report_path.read_text(encoding="utf-8"))
    assert result.feature_count == 1
    assert result.feature_class_path == result.output_gdb / result.request.feature_class
    assert report["coordinate_system"]["wkid"] == 3857
    assert report["coordinates"]["origin"] == list(result.request.origin)
    copied = work / (result.output_gdb.stem + "-copy.gdb")
    shutil.copytree(result.output_gdb, copied)
    copy_report = work / (result.output_gdb.stem + "-copy.json")
    subprocess.run([str(engine.writer), "--verify-gdb", str(copied), "--expected-report", str(result.report_path),
                    "--report", str(copy_report)], check=True, stdout=subprocess.DEVNULL)
    assert json.loads(copy_report.read_text(encoding="utf-8"))["status"] == "standalone_copy_verified"
    results.append(result.output_gdb.stem)
    verified_gdbs.append(result.output_gdb.stem)
    return report

events = []
result = engine.convert(request, on_message=events.append)
report = verify_copy(result)
assert any(check["textured_patches"] > 0 for check in report["verification"]["checks"])
assert events[-1].code == "VERIFIED"
obj_request = replace(request, input_fbx=source_dir / "textured_quad.obj", output_gdb=work / "OBJ 贴图.gdb",
                      feature_class="ImportedOBJ")
obj_result = engine.convert(obj_request)
obj_report = verify_copy(obj_result)
assert any(check["textured_patches"] > 0 for check in obj_report["verification"]["checks"])
assert any(d.code == "OBJ_COORDINATE_ASSUMPTION" for d in obj_result.diagnostics)
glb_request = replace(request, input_fbx=source_dir / "textured_quad.glb", output_gdb=work / "GLB 贴图.gdb",
                      feature_class="ImportedGLB", profile="strict")
glb_result = engine.convert(glb_request)
glb_report = verify_copy(glb_result)
assert any(check["textured_patches"] > 0 for check in glb_report["verification"]["checks"])
assert glb_report["coordinates"]["origin"] == [100, 100, 100]
for extension in ("gltf", "wrl", "dae"):
    converted = engine.convert(replace(request, input_fbx=source_dir / ("textured_quad." + extension),
        output_gdb=work / (extension + ".gdb"), profile="strict"))
    verified = verify_copy(converted)
    assert any(c["textured_patches"] > 0 for c in verified["verification"]["checks"])
    assert verified["coordinates"]["origin"] == [100, 100, 100]

try:
    engine.convert(request)
    raise AssertionError("Existing GDB accepted")
except ValidationError as error:
    assert error.code == "PATH_EXISTS"
results.append("existing-output-preserved")

missing = replace(request, input_fbx=source_dir / "missing_texture.fbx", output_gdb=work / "缺图.gdb")
result = engine.convert(missing)
verify_copy(result)
assert any(d.code == "MISSING_TEXTURE_FALLBACK" for d in result.diagnostics)
for source, name, policy, expected_code in ((missing.input_fbx, "required-texture", "error", "MISSING_TEXTURE"),
                                           (normal_file, "strict-normal", "material-color", "INVALID_NORMAL")):
    rejected = replace(request, input_fbx=source, output_gdb=work / (name + ".gdb"), missing_textures=policy)
    try:
        engine.convert(rejected)
        raise AssertionError("Invalid input accepted")
    except ConversionError as error:
        assert error.exit_code == 3 and error.report_path.is_file()
        assert any(d.code == expected_code for d in error.diagnostics)
        assert json.loads(error.report_path.read_text(encoding="utf-8"))["coordinates"]["origin"] == [100, 100, 100]
        assert not rejected.output_gdb.exists()
    results.append(name)

repaired = replace(request, input_fbx=normal_file, output_gdb=work / "法线修复.gdb", profile="gis-static")
result = engine.convert(repaired)
report = verify_copy(result)
assert any(d.code == "NORMALS_REPAIRED" for d in result.diagnostics)
assert any(check["textured_patches"] > 0 for check in report["verification"]["checks"])
callback_request = replace(request, output_gdb=work / "消息失败但成果有效.gdb", origin=(-12.25, 0, 123.125))
def fail_final_message(event):
    if event.code == "VERIFIED":
        raise RuntimeError("simulated host message failure")
try:
    engine.convert(callback_request, on_message=fail_final_message)
    raise AssertionError("Callback failure disappeared")
except CallbackError as error:
    assert error.exit_code == 0 and error.result is not None
    verify_copy(error.result)
    assert error.result.request.origin == (-12.25, 0, 123.125)
invalid_image = source_dir / '目录不是贴图.png'
invalid_image.mkdir()
invalid_source = source_dir / '贴图路径错误.fbx'
invalid_source.write_text(base.replace('checker.png', invalid_image.name), encoding='utf-8')
hashes[invalid_source] = hashlib.sha256(invalid_source.read_bytes()).hexdigest()
for profile in ('strict', 'gis-static'):
    invalid_request = replace(request, input_fbx=invalid_source, output_gdb=work / ('invalid-image-' + profile + '.gdb'), profile=profile)
    try:
        engine.convert(invalid_request)
        raise AssertionError('A directory texture path was treated as missing')
    except ConversionError as error:
        assert error.code == 'PROCESS_FAILED' and error.exit_code == 3
        assert any(d.code == 'TEXTURE_READ_ERROR' for d in error.diagnostics)
        assert not any(d.code == 'MISSING_TEXTURE_FALLBACK' for d in error.diagnostics)
        assert not invalid_request.output_gdb.exists() and error.report_path.is_file()
        assert invalid_image.is_dir() and not list(invalid_image.iterdir())
    results.append('invalid-image-' + profile)

# Inspection requires only the matching CLI. Its reports have their own fresh
# destinations and never create GDBs, even with an explicitly unavailable writer.
inspection_dir = work / "模型检查报告"
inspection_dir.mkdir()
unavailable_writer = work / ("unavailable-writer.exe" if os.name == "nt" else "unavailable-writer")
assert not unavailable_writer.exists()
inspection_engine = Engine(engine.executable, writer=unavailable_writer)
inspect_cases = []
local_coordinates = dict(unit="meter", up_axis="Z", space="local", wkid=0,
                         origin=[0, 0, 0], origin_explicit=False)

def verify_inspection(result, minimum, maximum, *, textured=True):
    report = json.loads(result.report_path.read_text(encoding="utf-8"))
    assert report["status"] == "inspected" and report["backend"] == result.backend == "none"
    assert report["version"] == result.version == geomodelbridge.__version__
    assert Path(report["source"]) == result.request.input_model
    assert report["coordinates"] == local_coordinates
    assert report["conversion_profile"] == result.request.profile
    assert report["missing_texture_policy"] == result.request.missing_textures
    assert report["fidelity"]["validation_passed"] is True
    assert report["fidelity"]["gdb_written"] is False
    assert report["fidelity"]["gdb_readback_verified"] is False
    assert result.counts.meshes == result.counts.materials == 1
    assert result.counts.triangles == 2 and result.counts.corner_vertices == 6
    assert result.counts.textures == (1 if textured else 0)
    assert result.counts.texture_bytes == (len((source_dir / "checker.png").read_bytes()) if textured else 0)
    assert tuple(report["geometry_bounds"]["min"]) == result.bounds.minimum == minimum
    assert tuple(report["geometry_bounds"]["max"]) == result.bounds.maximum == maximum
    assert "output" not in report and "verification" not in report
    assert not any(d.severity == "error" for d in result.diagnostics)
    inspect_cases.append(result.report_path.stem)
    return report

expected_bounds = {
    "fbx": ((0, 0, 0), (2, 3, 0)),
    "obj": ((0, 0, 0), (2, 2, 0)),
    "glb": ((10, -2, 1), (12, 1, 1)),
    "gltf": ((10, -2, 1), (12, 1, 1)),
    "wrl": ((10, -2, 1), (12, 1, 1)),
    "dae": ((10, 20, 30), (12, 23, 30)),
}
for extension, bounds in expected_bounds.items():
    inspection_request = InspectionRequest(source_dir / ("textured_quad." + extension),
                                            inspection_dir / (extension + ".json"))
    inspection_events = []
    inspected = inspection_engine.inspect(inspection_request, on_message=inspection_events.append)
    verify_inspection(inspected, *bounds)
    assert inspection_events[-1].code == "INSPECTED"
    assert not any(event.code == "VERIFIED" for event in inspection_events)

obj_inspection = InspectionRequest(source_dir / "textured_quad.obj", inspection_dir / "obj-y-up-centimeters.json",
                                   obj_up_axis="Y", obj_unit_meters=0.01)
verify_inspection(inspection_engine.inspect(obj_inspection), (0, 0, 0), (0.02, 0, 0.02))

# Existing bytes with a damaged PNG signature must be rejected, never treated
# as an absent image. Full image decoding remains a separate writer-side check.
corrupt_image = source_dir / "损坏图片.png"
corrupt_image.write_bytes(b"\0" + (source_dir / "checker.png").read_bytes()[1:])
corrupt_source = source_dir / "损坏贴图.fbx"
corrupt_source.write_text(base.replace("checker.png", corrupt_image.name), encoding="utf-8")
for path in (corrupt_image, corrupt_source):
    hashes[path] = hashlib.sha256(path.read_bytes()).hexdigest()

for profile in ("strict", "gis-static"):
    fallback_request = InspectionRequest(source_dir / "missing_texture.fbx",
                                         inspection_dir / ("missing-fallback-" + profile + ".json"), profile=profile)
    fallback = inspection_engine.inspect(fallback_request)
    verify_inspection(fallback, *expected_bounds["fbx"], textured=False)
    assert any(d.code == "MISSING_TEXTURE_FALLBACK" for d in fallback.diagnostics)
    for input_model, label, policy, expected_code in (
        (fallback_request.input_model, "missing-required", "error", "MISSING_TEXTURE"),
        (invalid_source, "invalid-image", "material-color", "TEXTURE_READ_ERROR"),
        (corrupt_source, "corrupt-png-signature", "material-color", "UNSUPPORTED_TEXTURE_FORMAT"),
    ):
        rejected = InspectionRequest(input_model, inspection_dir / (label + "-" + profile + ".json"),
                                     profile=profile, missing_textures=policy)
        try:
            inspection_engine.inspect(rejected)
            raise AssertionError("Inspection accepted an invalid texture resource")
        except InspectionError as error:
            assert error.code == "PROCESS_FAILED" and error.exit_code == 3
            assert any(d.code == expected_code for d in error.diagnostics)
            assert not any(d.code == "MISSING_TEXTURE_FALLBACK" for d in error.diagnostics)
            assert error.report_path == rejected.report_path and error.report_path.is_file()
            assert json.loads(error.report_path.read_text(encoding="utf-8"))["coordinates"] == local_coordinates
        inspect_cases.append(label + "-" + profile)

existing_report = inspection_dir / "fbx.json"
report_bytes = existing_report.read_bytes()
try:
    inspection_engine.inspect(InspectionRequest(source_dir / "textured_quad.fbx", existing_report))
    raise AssertionError("Inspection overwrote an existing report")
except ValidationError as error:
    assert error.code == "PATH_EXISTS"
assert existing_report.read_bytes() == report_bytes
inspect_cases.append("existing-inspection-report-preserved")

def fail_inspected_message(event):
    if event.code == "INSPECTED":
        raise RuntimeError("simulated inspection host message failure")

try:
    inspection_engine.inspect(InspectionRequest(source_dir / "textured_quad.fbx",
                              inspection_dir / "verified-inspection-callback-failure.json"),
                              on_message=fail_inspected_message)
    raise AssertionError("Inspection callback failure disappeared")
except CallbackError as error:
    assert error.exit_code == 0 and error.result is not None
    assert error.event.code == "INSPECTED"
    verify_inspection(error.result, *expected_bounds["fbx"])
assert all(path.is_file() and path.suffix == ".json" for path in inspection_dir.iterdir())
assert not unavailable_writer.exists()
assert all(hashlib.sha256(path.read_bytes()).hexdigest() == digest for path, digest in hashes.items())
assert "arcpy" not in sys.modules
assessment = dict(version=geomodelbridge.__version__, status="passed", python=sys.version,
                  cases=results, inspect_cases=inspect_cases, inspection_without_writer=True,
                  installed_client=True, arcpy_imported=False, input_unchanged=True,
                  copied_gdb_readbacks=len(verified_gdbs), graphical_acceptance="not_performed", atbx_execution="not_performed")
(work / "assessment.json").write_text(json.dumps(assessment, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"PASS installed Python client: {len(results)} conversion cases, {len(inspect_cases)} inspection cases, "
      f"{len(verified_gdbs)} GDBs independently copied and reopened")
