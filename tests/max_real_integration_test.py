"""Opt-in real MAX -> installed stdlib client -> native textured GDB acceptance.

Requires a licensed Autodesk runtime and a supported textured user fixture.
No mocks, automatic skips, source rewriting or existing-output overwrites.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--install-dir", type=Path, required=True)
parser.add_argument("--max-batch", type=Path, required=True)
parser.add_argument("--input", type=Path, required=True)
parser.add_argument("--frame", type=int, required=True)
parser.add_argument("--wkid", type=int, required=True)
parser.add_argument("--origin", type=float, nargs=3, required=True)
parser.add_argument("--texture-dir", type=Path, action="append", default=[])
parser.add_argument("--work", type=Path, required=True)
parser.add_argument("--timeout", type=int, default=600)
args = parser.parse_args()
if os.name != "nt":
    parser.error("Real MAX acceptance requires Windows")
install = args.install_dir.resolve()
if not args.max_batch.is_file() or not args.input.is_file() or args.input.suffix.lower() != ".max":
    parser.error("A real 3dsmaxbatch.exe and a textured .max fixture are required")
sys.path.insert(0, str(install / "python"))
from geomodelbridge import ConversionRequest, Engine, __version__
from geomodelbridge.client import _check_path_links
source = args.input.absolute()
work = args.work.absolute()
_check_path_links(source)
_check_path_links(work)
work.mkdir(parents=True, exist_ok=False)

def sha(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()

before = sha(source)
try:
    engine = Engine(install / "bin/geomodelbridge.exe")
    result = engine.convert(ConversionRequest(source, work / "model.gdb", args.wkid, tuple(args.origin),
        profile="gis-static", max_batch=args.max_batch, max_frame=args.frame, max_timeout=args.timeout,
        texture_dirs=tuple(args.texture_dir)))
    report = json.loads(result.report_path.read_text(encoding="utf-8"))
    assert any(item["textured_patches"] > 0 for item in report["verification"]["checks"]), "Fixture must produce a textured GDB"
    assert sha(source) == before, "Original MAX file was modified"
    shutil.copytree(result.output_gdb, work / "independent-copy.gdb")
    process = subprocess.run([str(engine.writer), "--verify-gdb", str(work / "independent-copy.gdb"),
        "--expected-report", str(result.report_path), "--report", str(work / "copy-readback.json")],
        capture_output=True, timeout=120, check=True)
    copy = json.loads((work / "copy-readback.json").read_text(encoding="utf-8"))
    assert copy["status"] == "standalone_copy_verified"
    summary = dict(status="passed", version=__version__, real_max_runtime_used=True,
        source=str(source), source_sha256=before, max_batch=str(args.max_batch.absolute()), frame=args.frame,
        independent_copy_readback=True, visual_acceptance_performed=False)
except Exception as error:
    summary = dict(status="failed", real_max_runtime_used=True, error=str(error), source_unchanged=sha(source) == before)
    with (work / "acceptance.json").open("x", encoding="utf-8") as stream:
        json.dump(summary, stream, ensure_ascii=False, indent=2)
    raise
with (work / "acceptance.json").open("x", encoding="utf-8") as stream:
    json.dump(summary, stream, ensure_ascii=False, indent=2)
print("Real MAX acceptance passed:", work / "acceptance.json")
