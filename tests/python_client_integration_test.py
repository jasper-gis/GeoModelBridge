"""Exercise the INSTALLED Python library through real CLI/native GDB conversions."""
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
from geomodelbridge import CallbackError, ConversionRequest, ConversionError, Engine, ValidationError
assert Path(geomodelbridge.__file__).resolve().is_relative_to(install / "python")
assert "arcpy" not in sys.modules
engine = Engine(install / "bin" / ("geomodelbridge.exe" if os.name == "nt" else "geomodelbridge"))
assert not engine.check().arcgis_pro_required
fixtures = Path(__file__).resolve().parent / "fixtures"
source_dir = work / "中文 模型 & 输入"
source_dir.mkdir()
for name in ("textured_quad.fbx", "textured_quad.obj", "textured_quad.mtl", "checker.png", "missing_texture.fbx"):
    shutil.copy2(fixtures / name, source_dir / name)
base = (fixtures / "textured_quad.fbx").read_text(encoding="utf-8")
broken = re.sub(r"Normals: \*\d+ \{ a: [^}]+", "Normals: *12 { a: " + ",".join(["0"] * 12) + " ", base)
normal_file = source_dir / "无效法线.fbx"
normal_file.write_text(broken, encoding="utf-8")
hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in source_dir.iterdir()}
results = []
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
assert all(hashlib.sha256(path.read_bytes()).hexdigest() == digest for path, digest in hashes.items())
assert "arcpy" not in sys.modules
assessment = dict(version=geomodelbridge.__version__, status="passed", python=sys.version,
                  cases=results, installed_client=True, arcpy_imported=False, input_unchanged=True,
                  copied_gdb_readbacks=5, graphical_acceptance="not_performed", atbx_execution="not_performed")
(work / "assessment.json").write_text(json.dumps(assessment, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"PASS installed Python client: {len(results)} cases, 5 GDBs independently copied and reopened")
