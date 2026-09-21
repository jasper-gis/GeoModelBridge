"""Exercise a minimal native release with no desktop GIS paths in the child environment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from gmb_platform import executable_names, sdk_manifest, platform_name

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--install-dir", required=True, type=Path)
parser.add_argument("--work", required=True, type=Path)
args = parser.parse_args()
install, work = args.install_dir.resolve(), args.work.resolve()
work.mkdir(parents=True, exist_ok=False)
if (install / "bin/arcgis-pro").exists(): raise AssertionError("Removed backend in current release")
cli_name, writer_name = executable_names()
runtime_names = ["bin/native-filegdb/" + Path(name).name for name in sdk_manifest()["runtime_files"]]
python_files = ["python/geomodelbridge/" + name for name in ("__init__.py", "_version.py", "client.py")]
for name in [cli_name, writer_name, *runtime_names, "bin/demo/textured_quad.fbx", "bin/demo/checker.png", *python_files]:
    destination = work / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(install / name, destination)
env = {key:value for key,value in os.environ.items() if not key.upper().startswith(("ARCGIS", "ESRI", "GMB_"))}
env.pop("LD_LIBRARY_PATH", None)
env.pop("LD_PRELOAD", None)
env["PATH"] = os.pathsep.join([str(Path(os.environ["SystemRoot"]) / "System32"), os.environ["SystemRoot"]]) if platform_name() == "windows-x64" else "/usr/bin:/bin"
env["ARCGIS_PRO_INSTALL_DIR"] = str(work / "absent-pro")
env["GMB_PRO_WRITER"] = str(work / "absent-pro.exe")
cli, writer = work / cli_name, work / writer_name
def run(*command):
    result = subprocess.run([str(value) for value in command], cwd=work, env=env,
                            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    return result.stdout
version = run(cli, "--version").strip()
if os.name != "nt":
    env["PATH"] = str(cli.parent) + os.pathsep + env["PATH"]
    doctor = json.loads(run("geomodelbridge", "doctor"))
    assert Path(doctor["native_filegdb"]["writer_path"]) == writer
    assert doctor["native_filegdb"]["writer_present"] is True
probe = json.loads(run(writer, "--probe"))
assert probe["arcgis_pro_required"] is False and probe["backend"] == "native-filegdb"
input_path = work / "bin/demo/textured_quad.fbx"
input_hash = hashlib.sha256(input_path.read_bytes()).hexdigest()
run(cli if os.name == "nt" else "geomodelbridge", "convert", input_path, "--output", work / "textured.gdb", "--wkid", 32650,
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
# Import only the relocated client with site-packages disabled. Its child engine
# inherits the same minimal environment, including the intentionally absent Pro.
python_check = """
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from geomodelbridge import ConversionRequest, Engine
result = Engine(sys.argv[2]).convert(ConversionRequest(sys.argv[3], sys.argv[4], 3857, (100,100,100)))
assert result.feature_count == 1 and result.report_path.is_file()
assert result.feature_class_path == Path(sys.argv[4]) / 'Models'
assert 'arcpy' not in sys.modules
print('PASS relocated Python client without site-packages')
"""
run(sys.executable, "-S", "-c", python_check, work / "python", cli, input_path, work / "python-client.gdb")
assert hashlib.sha256(input_path.read_bytes()).hexdigest() == input_hash
summary = {"status":"passed", "version":version, "probe":probe, "feature_count":1,
           "default_native_conversion":True, "textured_readback":True, "standalone_copy":True,
           "input_unchanged":True, "child_path":env["PATH"], "pro_backend_in_package":False, "relocated_python_client":True,
           "limitation":"Minimal-package test; does not by itself prove that the host has never had desktop GIS installed. Graphical acceptance is separate."}
(work / "assessment.json").write_text(json.dumps(summary,indent=2),encoding="utf-8")
print("PASS minimal native deployment, default backend, textured readback and copied GDB")
