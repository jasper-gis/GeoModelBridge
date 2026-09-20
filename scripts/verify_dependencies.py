"""Verify the exact vendored dependency files; no network or package installation."""
import argparse
import hashlib
import json
from pathlib import Path
from gmb_platform import sdk_manifest, installed_sdk_files

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--install-dir", type=Path, default=root / "dist")
args = parser.parse_args()
install = args.install_dir.resolve()
manifest = json.loads((root / "third_party" / "manifest.json").read_text(encoding="utf-8-sig"))
for dependency in manifest["dependencies"]:
    for file in dependency["files"]:
        path = root / file["path"]
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != file["sha256"]:
            raise SystemExit(f"Dependency verification failed: {file['path']}")
    print(f"Verified {dependency['name']} {dependency['version']}")

# The optional official FileGDB runtime is copied only by an explicit native install.
for target in ("windows-x64", "linux-x64"):
    sdk = sdk_manifest(target)
    mapping = {key: install / value for key, value in installed_sdk_files(sdk).items()}
    if not any(mapping["sdk/" + name].exists() for name in sdk["runtime_files"]):
        continue
    for item in sdk["hashes"]:
        if item["path"] not in mapping:
            continue
        path = mapping[item["path"]]
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
            raise SystemExit(f"Installed FileGDB dependency verification failed: {path}")
    print(f"Verified FileGDB API {sdk['version']} runtime and original license files")
