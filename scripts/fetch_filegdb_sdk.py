"""Fetch the pinned official FileGDB SDK into a new development directory."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import urllib.request
import zipfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--archive", type=Path, help="Use an already downloaded archive after the same hash check")
args = parser.parse_args()
output = args.output.resolve()
if output.exists(): raise SystemExit("SDK output must be a new directory")
manifest = json.loads((root / "backends/native-filegdb/sdk-sources.json").read_text(encoding="utf-8-sig"))
hashes = {item["path"]: item["sha256"] for item in manifest["hashes"]}
with tempfile.TemporaryDirectory(prefix="gmb-sdk-download-") as temporary:
    archive = args.archive.resolve() if args.archive else Path(temporary) / "FileGDB_API_VS2022.zip"
    if not args.archive:
        urllib.request.urlretrieve(manifest["download_url"], archive)
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != hashes["FileGDB_API_VS2022.zip"]:
        raise SystemExit("Official SDK archive SHA-256 mismatch")
    with zipfile.ZipFile(archive) as package:
        for member in package.infolist():
            target = (output / member.filename).resolve()
            if not target.is_relative_to(output) or (member.external_attr >> 16) & 0o170000 == 0o120000:
                raise SystemExit("Unsafe SDK archive path")
        output.mkdir(parents=True, exist_ok=False)
        package.extractall(output)
for name, expected in hashes.items():
    if not name.startswith("sdk/"): continue
    path = output / name.removeprefix("sdk/")
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise SystemExit("Extracted SDK file SHA-256 mismatch: " + name)
print("Verified FileGDB SDK " + manifest["version"] + ": " + str(output))
