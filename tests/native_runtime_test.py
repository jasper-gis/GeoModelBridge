"""Verify the build-tree writer has pinned SDK libraries and runs without SDK paths."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from gmb_platform import sdk_manifest

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--writer", required=True, type=Path)
args = parser.parse_args()
writer = args.writer.resolve()
manifest = sdk_manifest()
hashes = {item["path"]: item["sha256"] for item in manifest["hashes"]}
for name in manifest["runtime_files"]:
    path = writer.parent / Path(name).name
    assert path.is_file(), f"Runtime missing beside built writer: {path}"
    assert hashlib.sha256(path.read_bytes()).hexdigest() == hashes["sdk/" + name], path
env = dict(os.environ)
for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "FILEGDB_API_ROOT"):
    env.pop(key, None)
env["PATH"] = os.pathsep.join((str(Path(os.environ["SystemRoot"]) / "System32"), os.environ["SystemRoot"])) if os.name == "nt" else "/usr/bin:/bin"
if os.name != "nt":
    dependencies = subprocess.check_output(["ldd", str(writer)], env=env, text=True, timeout=60)
    for name in manifest["runtime_files"]:
        filename = Path(name).name
        resolved = next(line.split("=>", 1)[1].strip().split(" (", 1)[0]
                        for line in dependencies.splitlines() if line.strip().startswith(filename + " =>"))
        assert Path(resolved).resolve() == writer.parent / filename, dependencies

def run(option):
    result = subprocess.run([str(writer), option], env=env, cwd=writer.parent,
                            capture_output=True, text=True, encoding="utf-8", timeout=60)
    assert result.returncode == 0, (option, result.returncode, result.stdout, result.stderr)
    return result.stdout

assert run("-h") == run("--help")
probe = json.loads(run("--probe"))
assert probe["status"] == "available" and probe["backend"] == "native-filegdb"
assert probe["version"] == (Path(__file__).resolve().parents[1] / "VERSION").read_text().strip()
print("PASS build-tree runtime hashes, writer -h/--help and SDK probe without development SDK paths")
