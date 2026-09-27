"""Mock MAX preprocessing + REAL installed client/FileGDB, never real MAX evidence."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--install-dir", type=Path, required=True)
p.add_argument("--stub", type=Path, required=True)
p.add_argument("--work", type=Path, required=True)
a = p.parse_args()
assert os.name == "nt"
install, stub, work = a.install_dir.absolute(), a.stub.absolute(), a.work.absolute()
work.mkdir(parents=True, exist_ok=False)
sys.path.insert(0, str(install / "python"))
from geomodelbridge import Engine, ConversionRequest, ConversionError
fixture = Path(__file__).resolve().parent / "fixtures/textured_quad.fbx"
engine = Engine(install / "bin/geomodelbridge.exe")
source = work / "protocol-stub.max"
source.write_text(json.dumps(dict(mode="ok", fbx=str(fixture))), encoding="utf-8")
result = engine.convert(ConversionRequest(source, work / "model.gdb", 32650, (500000, 3000000, 100),
    max_batch=stub, max_frame=5))
report = json.loads(result.report_path.read_text(encoding="utf-8"))
assert report["source"] == str(source)
assert any(check["textured_patches"] > 0 for check in report["verification"]["checks"])
shutil.copytree(result.output_gdb, work / "copy.gdb")
subprocess.run([str(engine.writer), "--verify-gdb", str(work / "copy.gdb"),
    "--expected-report", str(result.report_path), "--report", str(work / "copy.json")], check=True)
assert json.loads((work / "copy.json").read_text())["status"] == "standalone_copy_verified"
bad = work / "rejected-stub.max"
bad.write_text(json.dumps(dict(mode="binding", fbx=str(fixture))), encoding="utf-8")
try:
    engine.convert(ConversionRequest(bad, work / "rejected.gdb", 32650, (500000, 3000000, 100), max_batch=stub, max_frame=5))
except ConversionError:
    pass
else:
    raise AssertionError("Changed image binding was accepted")
assert not (work / "rejected.gdb").exists()
with (work / "test-results.json").open("x") as stream:
    json.dump(dict(status="passed", real_max_runtime_used=False, real_filegdb_written=True,
                   independent_copy_readback=True, installed_client=True, invalid_binding_rejected_before_gdb=True), stream, indent=2)
print("MAX protocol substitute -> installed stdlib client -> real textured GDB and independent copy passed; no real MAX validation")
