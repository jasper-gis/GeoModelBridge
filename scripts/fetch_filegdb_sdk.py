"""Fetch the pinned official FileGDB SDK into a new development directory."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import urllib.request
import zipfile
import tarfile
import shutil
from gmb_platform import sdk_manifest

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--archive", type=Path, help="Use an already downloaded archive after the same hash check")
parser.add_argument("--platform", choices=("windows-x64", "linux-x64"), help="Defaults to the current OS and CPU")
args = parser.parse_args()
output = args.output.resolve()
if output.exists(): raise SystemExit("SDK output must be a new directory")
manifest = sdk_manifest(args.platform)
hashes = {item["path"]: item["sha256"] for item in manifest["hashes"]}
with tempfile.TemporaryDirectory(prefix="gmb-sdk-download-") as temporary:
    archive = args.archive.resolve() if args.archive else Path(temporary) / manifest["archive_name"]
    if not args.archive:
        urllib.request.urlretrieve(manifest["download_url"], archive)
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != hashes[manifest["archive_name"]]:
        raise SystemExit("Official SDK archive SHA-256 mismatch")
    if manifest["archive_name"].endswith(".zip"):
        with zipfile.ZipFile(archive) as package:
            for member in package.infolist():
                target = (output / member.filename).resolve()
                if not target.is_relative_to(output) or (member.external_attr >> 16) & 0o170000 == 0o120000:
                    raise SystemExit("Unsafe SDK archive path")
            output.mkdir(parents=True, exist_ok=False)
            package.extractall(output)
    else:
        # Flatten only the pinned archive root; extract regular files/directories,
        # never archive links, ownership, device nodes or absolute paths.
        with tarfile.open(archive, "r:gz") as package:
            members = []
            for member in package.getmembers():
                relative = Path(member.name).relative_to(manifest["archive_root"])
                target = (output / relative).resolve()
                if not target.is_relative_to(output) or not (member.isfile() or member.isdir()):
                    raise SystemExit("Unsafe SDK archive entry: " + member.name)
                members.append((member, target))
            output.mkdir(parents=True, exist_ok=False)
            for member, target in members:
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with package.extractfile(member) as source, target.open("xb") as destination:
                        shutil.copyfileobj(source, destination)
for name, expected in hashes.items():
    if not name.startswith("sdk/"): continue
    path = output / name.removeprefix("sdk/")
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise SystemExit("Extracted SDK file SHA-256 mismatch: " + name)
print("Verified FileGDB SDK " + manifest["version"] + ": " + str(output))
