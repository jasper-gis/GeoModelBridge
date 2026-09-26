"""Exercise both executables directly from the build tree, before installation."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cli", required=True, type=Path)
parser.add_argument("--writer", required=True, type=Path)
args = parser.parse_args()
cli, writer = args.cli.resolve(), args.writer.resolve()
fixture = Path(__file__).resolve().parent / "fixtures/textured_quad.fbx"
env = {key: value for key, value in os.environ.items()
       if not key.upper().startswith(("GMB_", "ARCGIS", "ESRI", "FILEGDB_"))}
env.pop("LD_LIBRARY_PATH", None)
env.pop("LD_PRELOAD", None)
env["PATH"] = os.pathsep.join((str(Path(os.environ["SystemRoot"]) / "System32"), os.environ["SystemRoot"])) if os.name == "nt" else "/usr/bin:/bin"

with tempfile.TemporaryDirectory(prefix="gmb-build-") as directory:
    work = Path(directory)

    def run(*command):
        result = subprocess.run(list(map(str, command)), cwd=work, env=env, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=60)
        assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
        return result.stdout

    doctor = json.loads(run(cli, "doctor"))["native_filegdb"]
    assert doctor["writer_present"] is True
    assert Path(doctor["writer_path"]).resolve() == writer
    assert writer.parent == cli.parent / "native-filegdb"
    # No --writer override: verify the layout the user gets from cmake --build.
    run(cli, "convert", fixture, "--output", work / "model.gdb", "--wkid", 32650,
        "--origin", 500000, 3000000, 100)
    report_path = work / "model.gdb.report.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    assert report["status"] == "written_and_readback_verified"
    assert report["coordinates"]["wkid"] == 32650
    assert report["coordinates"]["origin"] == [500000, 3000000, 100]
    assert any(check["textured_patches"] > 0 for check in report["verification"]["checks"])
    shutil.copytree(work / "model.gdb", work / "copy.gdb")
    run(writer, "--verify-gdb", work / "copy.gdb", "--expected-report", report_path,
        "--report", work / "copy.json")
    assert json.loads((work / "copy.json").read_text())["status"] == "standalone_copy_verified"
    obj = fixture.with_suffix(".obj")
    run(cli, "convert", obj, "--output", work / "obj.gdb", "--wkid", 32650,
        "--origin", 500000, 3000000, 100)
    obj_report_path = work / "obj.gdb.report.json"
    obj_report = json.loads(obj_report_path.read_text(encoding="utf-8"))
    assert obj_report["status"] == "written_and_readback_verified"
    assert obj_report["source"] == str(obj)
    assert any(check["textured_patches"] > 0 for check in obj_report["verification"]["checks"])
    shutil.copytree(work / "obj.gdb", work / "obj-copy.gdb")
    run(writer, "--verify-gdb", work / "obj-copy.gdb", "--expected-report", obj_report_path,
        "--report", work / "obj-copy.json")
    assert json.loads((work / "obj-copy.json").read_text())["status"] == "standalone_copy_verified"
print("PASS root build layout: FBX/OBJ textured GDBs and standalone copy readback")
