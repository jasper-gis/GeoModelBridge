"""Verify the exact vendored dependency files; no network or package installation."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / "third_party" / "manifest.json").read_text(encoding="utf-8-sig"))
for dependency in manifest["dependencies"]:
    for file in dependency["files"]:
        path = root / file["path"]
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != file["sha256"]:
            raise SystemExit(f"Dependency verification failed: {file['path']}")
    print(f"Verified {dependency['name']} {dependency['version']}")

# The optional official FileGDB runtime is copied only by an explicit native install.
runtime = root / "dist/bin/native-filegdb/FileGDBAPI.dll"
if runtime.exists():
    sdk = json.loads((root / "backends/native-filegdb/sdk-sources.json").read_text(encoding="utf-8-sig"))
    mapping = {
        "sdk/bin64/FileGDBAPI.dll": runtime,
        "sdk/license/Apache License.pdf": root / "dist/licenses/filegdb-api/Apache License.pdf",
        "sdk/license/userestrictions.txt": root / "dist/licenses/filegdb-api/userestrictions.txt",
        "sdk/README-windows_VS2022.txt": root / "dist/licenses/filegdb-api/README-windows_VS2022.txt",
    }
    for item in sdk["hashes"]:
        if item["path"] not in mapping:
            continue
        path = mapping[item["path"]]
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
            raise SystemExit(f"Installed FileGDB dependency verification failed: {path}")
    print(f"Verified FileGDB API {sdk['version']} runtime and original license files")
