"""Package source, installed programs and verified examples, excluding development caches."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

root = Path(__file__).resolve().parents[1]
version = (root / "VERSION").read_text(encoding="utf-8").strip()
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
output = args.output.resolve()
if output.exists() or root in output.parents:
    raise SystemExit("Output must be a new archive outside the project directory")
cli = root / "dist/bin/geomodelbridge.exe"
writer = root / "dist/bin/arcgis-pro/GeoModelBridge.ProWriter.dll"
native = root / "dist/bin/native-filegdb/GeoModelBridge.NativeWriter.exe"
runtime = native.with_name("FileGDBAPI.dll")
if not all(p.is_file() for p in (cli, writer, native, runtime)):
    raise SystemExit("Build both release backends and explicitly include the official FileGDB runtime first")
subprocess.run([sys.executable, str(root / "scripts/check_version.py")], check=True)
subprocess.run([sys.executable, str(root / "scripts/verify_dependencies.py")], check=True)
actual = subprocess.check_output([str(cli), "--version"], text=True).strip()
if actual != f"GeoModelBridge V{version}":
    raise SystemExit("CLI and source version do not match")
pro_help = subprocess.check_output(["dotnet", str(writer), "--help"], text=True)
if pro_help.splitlines()[0] != f"GeoModelBridge ArcGIS Pro adapter {version}":
    raise SystemExit("Pro writer and source version do not match")
summary = root / "examples" / f"V{version}" / "verification-summary.json"
if not summary.is_file() or json.loads(summary.read_text(encoding="utf-8"))["status"] != "passed":
    raise SystemExit("Generate and verify the release examples first")
crosscheck = root / "docs/evidence" / f"V{version}" / "native-independent-pro/summary.json"
if not crosscheck.is_file():
    raise SystemExit("Verify the native outputs independently with scripts/verify_native_with_pro.py first")
cross = json.loads(crosscheck.read_text(encoding="utf-8"))
if cross["status"] != "passed" or cross["cases"] != 14:
    raise SystemExit("All 14 native reference outputs must pass independent Pro readback")
native_probe = json.loads(subprocess.check_output([str(native), "--probe"], text=True))
if native_probe.get("version") != version or native_probe.get("status") != "available":
    raise SystemExit("Native writer version/runtime does not match the release")

output.parent.mkdir(parents=True, exist_ok=True)
count = 0
with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for file in sorted(root.rglob("*")):
        if not file.is_file():
            continue
        relative = file.relative_to(root)
        if any(part in {".git", "build", "__pycache__", ".vs"} for part in relative.parts):
            continue
        if relative.parts[0] == "backends" and any(part in {"bin", "obj"} for part in relative.parts[2:]):
            continue
        if file.name.endswith(".lock"):
            continue
        archive.write(file, (Path("GeoModelBridge") / relative).as_posix())
        count += 1
with zipfile.ZipFile(output) as archive:
    problem = archive.testzip()
    if problem:
        raise SystemExit(f"Archive integrity check failed: {problem}")
digest = hashlib.sha256(output.read_bytes()).hexdigest()
checksum = output.with_suffix(output.suffix + ".sha256")
with checksum.open("x", encoding="utf-8") as file:
    file.write(f"{digest}  {output.name}\n")
print(f"Packaged {count} files: {output}\nSHA256 {digest}")
