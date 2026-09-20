"""Exercise a minimal native release with no desktop GIS paths in the child environment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--install-dir", required=True, type=Path)
parser.add_argument("--work", required=True, type=Path)
args = parser.parse_args()
install, work = args.install_dir.resolve(), args.work.resolve()
work.mkdir(parents=True, exist_ok=False)
if (install / "bin/arcgis-pro").exists(): raise AssertionError("Removed backend in current release")
for name in ["bin/geomodelbridge.exe", "bin/native-filegdb/GeoModelBridge.NativeWriter.exe", "bin/native-filegdb/FileGDBAPI.dll", "bin/demo/textured_quad.fbx", "bin/demo/checker.png"]:
    destination = work / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(install / name, destination)
env = {key:value for key,value in os.environ.items() if not key.upper().startswith(("ARCGIS", "ESRI", "GMB_"))}
env["PATH"] = os.pathsep.join([str(Path(os.environ["SystemRoot"]) / "System32"), os.environ["SystemRoot"]])
env["ARCGIS_PRO_INSTALL_DIR"] = str(work / "absent-pro")
env["GMB_PRO_WRITER"] = str(work / "absent-pro.exe")
cli, writer = work / "bin/geomodelbridge.exe", work / "bin/native-filegdb/GeoModelBridge.NativeWriter.exe"
def run(*command):
    result = subprocess.run([str(value) for value in command], cwd=work, env=env,
                            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    return result.stdout
version = run(cli, "--version").strip()
probe = json.loads(run(writer, "--probe"))
assert probe["arcgis_pro_required"] is False and probe["backend"] == "native-filegdb"
input_path = work / "bin/demo/textured_quad.fbx"
input_hash = hashlib.sha256(input_path.read_bytes()).hexdigest()
run(cli, "convert", input_path, "--output", work / "textured.gdb", "--wkid", 32650,
    "--origin", 500000, 3000000, 100, "--report", work / "conversion.json")
report = json.loads((work / "conversion.json").read_text(encoding="utf-8"))
assert report["status"] == "written_and_readback_verified" and report["backend"] == "native-filegdb"
assert report["verification"]["feature_count"] == 1
assert report["verification"]["geometry_material_uv_texture_readback"] is True
assert any(check["textured_patches"] > 0 for check in report["verification"]["checks"])
shutil.copytree(work / "textured.gdb", work / "copy.gdb")
run(writer, "--verify-gdb", work / "copy.gdb", "--expected-report", work / "conversion.json", "--report", work / "copy-check.json")
copy = json.loads((work / "copy-check.json").read_text(encoding="utf-8"))
assert copy["status"] == "standalone_copy_verified"
assert hashlib.sha256(input_path.read_bytes()).hexdigest() == input_hash
summary = {"status":"passed", "version":version, "probe":probe, "feature_count":1,
           "default_native_conversion":True, "textured_readback":True, "standalone_copy":True,
           "input_unchanged":True, "child_path":env["PATH"], "pro_backend_in_package":False,
           "limitation":"Minimal-package test; does not by itself prove that the host has never had desktop GIS installed. Graphical acceptance is separate."}
(work / "assessment.json").write_text(json.dumps(summary,indent=2),encoding="utf-8")
print("PASS minimal native deployment, default backend, textured readback and copied GDB")
