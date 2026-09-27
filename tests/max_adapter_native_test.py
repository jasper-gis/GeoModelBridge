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
rejected = ("binding", "duplicate_mesh", "duplicate_material", "missing_material", "missing_texture", "duplicate_texture")
for mode in rejected:
    bad = work / (mode + ".max")
    sample = ("instanced_mirror.fbx" if mode == "duplicate_mesh" else
              "multi_material.fbx" if mode in ("duplicate_material", "missing_material") else fixture.name)
    bad.write_text(json.dumps(dict(mode=mode, fbx=str(fixture.parent / sample))), encoding="utf-8")
    output = work / (mode + ".gdb")
    try:
        engine.convert(ConversionRequest(bad, output, 32650, (500000, 3000000, 100), max_batch=stub, max_frame=5))
    except ConversionError:
        pass
    else:
        raise AssertionError("Invalid export manifest was accepted: " + mode)
    assert not output.exists(), mode
log_source = work / "log-error.max"
log_source.write_text(json.dumps(dict(mode="ok_log_error", fbx=str(fixture))), encoding="utf-8")
log_result = engine.convert(ConversionRequest(log_source, work / "log-error.gdb", 32650, (500000, 3000000, 100),
    max_batch=stub, max_frame=5))
log_report = json.loads(log_result.report_path.read_text(encoding="utf-8"))
assert any(d["code"] == "MAX_LOG_READ_ERROR" for d in log_report["reader_diagnostics"])
with (work / "test-results.json").open("x") as stream:
    json.dump(dict(status="passed", real_max_runtime_used=False, real_filegdb_written=True,
                   independent_copy_readback=True, installed_client=True, invalid_binding_rejected_before_gdb=True,
                   rejected_manifest_modes=list(rejected), log_error_preserves_verified_result=True), stream, indent=2)
print("MAX protocol substitute -> installed stdlib client -> real textured GDB and independent copy passed; no real MAX validation")
