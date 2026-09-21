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
from geomodelbridge import ConversionRequest, ConversionError, Engine, ValidationError
assert Path(geomodelbridge.__file__).resolve().is_relative_to(install / "python")
assert "arcpy" not in sys.modules
engine = Engine(install / "bin" / ("geomodelbridge.exe" if os.name == "nt" else "geomodelbridge"))
assert not engine.check().arcgis_pro_required
fixtures = Path(__file__).resolve().parent / "fixtures"
source_dir = work / "中文 模型 & 输入"
source_dir.mkdir()
for name in ("textured_quad.fbx", "checker.png", "missing_texture.fbx"):
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
        assert not rejected.output_gdb.exists()
    results.append(name)

repaired = replace(request, input_fbx=normal_file, output_gdb=work / "法线修复.gdb", profile="gis-static")
result = engine.convert(repaired)
report = verify_copy(result)
assert any(d.code == "NORMALS_REPAIRED" for d in result.diagnostics)
assert any(check["textured_patches"] > 0 for check in report["verification"]["checks"])
assert all(hashlib.sha256(path.read_bytes()).hexdigest() == digest for path, digest in hashes.items())
assert "arcpy" not in sys.modules
assessment = dict(version=geomodelbridge.__version__, status="passed", python=sys.version,
                  cases=results, installed_client=True, arcpy_imported=False, input_unchanged=True,
                  copied_gdb_readbacks=3, graphical_acceptance="not_performed", atbx_execution="not_performed")
(work / "assessment.json").write_text(json.dumps(assessment, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"PASS installed Python client: {len(results)} cases, 3 GDBs independently copied and reopened")
