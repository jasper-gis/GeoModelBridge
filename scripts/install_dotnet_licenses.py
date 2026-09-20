"""Install the original notices for the GUI's restored self-contained runtimes.

Resolve versions and package roots from NuGet's project.assets.json rather than
guessing a global package path or silently copying notices from another version.
Only notice/provenance files are copied; SDK packages remain in the NuGet cache.
"""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET
import zipfile


RUNTIMES = (
    "microsoft.netcore.app.runtime.win-x64",
    "microsoft.windowsdesktop.app.runtime.win-x64",
)
NOTICE_NAMES = {"LICENSE", "LICENSE.TXT", "THIRD-PARTY-NOTICES", "THIRD-PARTY-NOTICES.TXT"}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def install(assets_path, output):
    assets = json.loads(assets_path.read_text(encoding="utf-8-sig"))
    versions = {}
    for framework in assets["project"]["frameworks"].values():
        for dependency in framework.get("downloadDependencies", []):
            package = dependency["name"].lower()
            if package not in RUNTIMES:
                continue
            exact = re.fullmatch(r"\[([^,\]]+),\s*([^,\]]+)\]", dependency["version"])
            if not exact or exact[1] != exact[2]:
                raise ValueError(f"Runtime version is not an exact NuGet resolution: {dependency}")
            if package in versions and versions[package] != exact[1]:
                raise ValueError(f"Conflicting runtime versions for {package}")
            versions[package] = exact[1]

    manifest = {"schema_version": 1, "purpose": "Original notices for runtimes embedded in geomodelbridgeGUI.exe", "packages": []}
    prepared = []
    for package in RUNTIMES:
        if package not in versions:
            raise ValueError(f"Self-contained GUI runtime is absent from NuGet restore: {package}")
        version = versions[package]
        candidates = [Path(folder) / package / version for folder in assets["packageFolders"]]
        directory = next((path for path in candidates if path.is_dir()), None)
        if directory is None:
            raise FileNotFoundError(f"Restored package not found: {package}/{version}")
        notices = sorted(path for path in directory.rglob("*") if path.is_file() and path.name.upper() in NOTICE_NAMES)
        if not any(path.name.upper() in {"LICENSE", "LICENSE.TXT"} for path in notices):
            raise FileNotFoundError(f"Runtime package has no original license: {directory}")
        if package == RUNTIMES[0] and not any(path.name.upper().startswith("THIRD-PARTY-NOTICES") for path in notices):
            raise FileNotFoundError(f".NET runtime third-party notices are missing: {directory}")

        package_archive = directory / f"{package}.{version}.nupkg"
        package_sha512 = directory / f"{package}.{version}.nupkg.sha512"
        expected_sha512 = package_sha512.read_text(encoding="ascii").strip()
        actual_sha512 = base64.b64encode(hashlib.sha512(package_archive.read_bytes()).digest()).decode("ascii")
        if expected_sha512 != actual_sha512:
            raise ValueError(f"NuGet package SHA-512 mismatch: {package_archive}")
        metadata = json.loads((directory / ".nupkg.metadata").read_text(encoding="utf-8-sig"))
        ns = {"n": "http://schemas.microsoft.com/packaging/2011/08/nuspec.xsd"}
        spec = ET.parse(directory / f"{package}.nuspec")
        repo = spec.find("n:metadata/n:repository", ns)
        notice_records = []
        with zipfile.ZipFile(package_archive) as archive:
            for source in notices:
                package_relative = source.relative_to(directory)
                if archive.read(package_relative.as_posix()) != source.read_bytes():
                    raise ValueError(f"Cached notice differs from original NuGet package: {source}")
                relative = Path(package) / version / package_relative
                notice_records.append({"path": relative.as_posix(), "bytes": source.stat().st_size, "sha256": sha256(source)})
                prepared.append((source, output / relative))
        manifest["packages"].append({
            "id": package, "version": version,
            "nuget_source": metadata.get("source"),
            "nuget_package_url": f"https://api.nuget.org/v3-flatcontainer/{package}/{version}/{package}.{version}.nupkg",
            "package_sha512_base64": actual_sha512,
            "repository": repo.attrib if repo is not None else {},
            "third_party_notice_present": any(source.name.upper().startswith("THIRD-PARTY-NOTICES") for source in notices),
            "original_files": notice_records,
        })

    for source, target in prepared:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        if source.read_bytes() != target.read_bytes():
            raise OSError(f"Original notice copy did not preserve bytes: {target}")
    output.mkdir(parents=True, exist_ok=True)
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Installed {len(prepared)} original .NET runtime notice files: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", required=True, type=Path, help="Published GUI's obj/project.assets.json")
    parser.add_argument("--output", required=True, type=Path, help="Distribution licenses/dotnet directory")
    args = parser.parse_args()
    install(args.assets.resolve(), args.output.resolve())
