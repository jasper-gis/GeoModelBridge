"""Platform names and pinned SDK layout shared by build/release tools."""
import json
import platform
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def platform_name():
    if platform.machine().lower() not in ("amd64", "x86_64"):
        raise RuntimeError("The native SDK requires x86_64")
    if platform.system() == "Windows":
        return "windows-x64"
    if platform.system() == "Linux":
        return "linux-x64"
    raise RuntimeError("Native releases support Windows and Linux")


def sdk_manifest(target=None):
    target = target or platform_name()
    name = {"windows-x64": "sdk-sources.json", "linux-x64": "sdk-sources-linux.json"}[target]
    return json.loads((ROOT / "backends/native-filegdb" / name).read_text(encoding="utf-8-sig"))


def executable_names():
    suffix = ".exe" if platform_name() == "windows-x64" else ""
    return "bin/geomodelbridge" + suffix, "bin/native-filegdb/GeoModelBridge.NativeWriter" + suffix


def installed_sdk_files(manifest):
    result = {"sdk/" + name: "bin/native-filegdb/" + Path(name).name for name in manifest["runtime_files"]}
    result.update({"sdk/" + name: "licenses/filegdb-api/" + Path(name).name for name in manifest["notice_files"]})
    return result
