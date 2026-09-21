"""Validate or package native-only source, installed programs and verified examples."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile
import tarfile
from gmb_platform import executable_names, sdk_manifest, platform_name

root = Path(__file__).resolve().parents[1]
version = (root / "VERSION").read_text(encoding="utf-8").strip()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, help="New .zip or .tar.gz outside the project")
parser.add_argument("--install-dir", type=Path, default=root / "dist")
parser.add_argument("--examples-dir", type=Path, default=root / "examples" / f"V{version}")
parser.add_argument("--check-only", action="store_true", help="Validate without creating an archive")
args = parser.parse_args()
install = args.install_dir.resolve()
examples = args.examples_dir.resolve()
output = args.output.resolve() if args.output else None
if not args.check_only and output is None:
    parser.error("--output is required unless --check-only is used")
if output is not None and (output.exists() or output.with_suffix(output.suffix + ".sha256").exists() or root == output or root in output.parents):
    raise SystemExit("Archive and checksum must be new paths outside the project")
if (install / "bin/arcgis-pro").exists():
    raise SystemExit("Install directory contains a removed backend; use a new native-only installation")
cli_name, native_name = executable_names()
cli, native = install / cli_name, install / native_name
runtimes = [native.with_name(Path(name).name) for name in sdk_manifest()["runtime_files"]]
if not all(p.is_file() for p in (cli, native, *runtimes)):
    raise SystemExit("Build the native release and include the official FileGDB runtime first")
subprocess.run([sys.executable, str(root / "scripts/check_version.py")], check=True)
subprocess.run([sys.executable, str(root / "scripts/verify_dependencies.py"), "--install-dir", str(install)], check=True)
actual = subprocess.check_output([str(cli), "--version"], text=True).strip()
if actual != f"GeoModelBridge V{version}":
    raise SystemExit("CLI and source version do not match")
native_probe = json.loads(subprocess.check_output([str(native), "--probe"], text=True))
if (native_probe.get("version") != version or native_probe.get("status") != "available"
        or native_probe.get("backend") != "native-filegdb" or native_probe.get("arcgis_pro_required") is not False):
    raise SystemExit("Native writer version/runtime does not match the release")
summary_path = examples / "verification-summary.json"
if not summary_path.is_file():
    raise SystemExit("Generate native release examples with scripts/generate_examples.py first")
summary = json.loads(summary_path.read_text(encoding="utf-8"))
cases = summary.get("gdb_cases", [])
expected_names = {"color-cube", "uv-plane", "mixed-materials", "alpha-plane", "seam-cube",
                  "colored_quad", "textured_quad", "embedded_quad", "uv_transform", "instanced_mirror",
                  "multi_material", "no_material", "sloped_normals", "binary_blender"}
if (summary.get("version") != version or summary.get("backend") != "native-filegdb"
        or summary.get("status") != "passed" or summary.get("standalone_copy_verified") is not True
        or len(cases) != 14 or {c.get("name") for c in cases} != expected_names
        or not all(c.get("passed") is True for c in cases)):
    raise SystemExit("All 14 native examples and standalone copy must pass for the current release")
# Reopen the actual GDBs. A stale or edited summary alone is not release evidence.
import tempfile
with tempfile.TemporaryDirectory(prefix="gmb-package-check-") as work:
    for name in sorted(expected_names):
        expected = examples / "reports" / (name + ".json")
        report = json.loads(expected.read_text(encoding="utf-8"))
        if report.get("version") != version or report.get("backend") != "native-filegdb":
            raise SystemExit("Example report does not match this native release: " + name)
        subprocess.run([str(native), "--verify-gdb", str(examples / "filegdb" / (name + ".gdb")),
                        "--expected-report", str(expected), "--report", str(Path(work) / (name + ".json"))],
                       check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(native), "--verify-gdb", str(examples / "standalone/alpha-plane-copy.gdb"),
                    "--expected-report", str(examples / "reports/alpha-plane.json"),
                    "--report", str(Path(work) / "standalone.json")], check=True, stdout=subprocess.DEVNULL)
# Enumerate source through Git, never by recursively sweeping local results or older releases.
source_roots = {"src", "include", "backends", "apps", "tests", "scripts", "third_party", "docs", ".github", "python"}
source_files = {"AGENTS.md", "README.md", "VERSION", "CHANGELOG.md", "THIRD_PARTY_NOTICES.md", "CMakeLists.txt", "CMakePresets.json", ".gitignore", ".gitattributes", "examples/README.md"}
files = {}
listing = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=root).decode("utf-8").split("\0")
for name in sorted(set(listing)):
    rel = Path(name)
    if not name or (name not in source_files and rel.parts[0] not in source_roots):
        continue
    if {"bin", "obj", "build", "__pycache__"}.intersection(rel.parts):
        continue
    path = root / rel
    if path.is_file(): files[rel.as_posix()] = path
binary_files = [cli_name, native_name, *[p.relative_to(install).as_posix() for p in runtimes], "bin/demo/textured_quad.fbx", "bin/demo/checker.png"]
# Explicit SDK files: fail on missing/stale installed libraries instead of shipping
# a release whose Python client silently targets a different executable version.
for relative in ("geomodelbridge/__init__.py", "geomodelbridge/_version.py", "geomodelbridge/client.py", "examples/convert_fbx.py"):
    name = "python/" + relative
    path = install / name
    if not path.is_file() or path.read_bytes() != (root / name).read_bytes():
        raise SystemExit("Installed Python client is missing or stale: " + name)
    binary_files.append(name)
if platform_name() == "windows-x64":
    binary_files.append("bin/geomodelbridgeGUI.exe")
for name in binary_files:
    path = install / name
    if path.is_file(): files["dist/" + name] = path
for directory, prefix in [(install / "licenses", "dist/licenses"), (examples, "examples/V" + version)]:
    for path in sorted(directory.rglob("*")):
        if path.is_file() and not path.name.endswith(".lock"):
            files[prefix + "/" + path.relative_to(directory).as_posix()] = path
if args.check_only:
    print(f"Validated native-only V{version}: {len(files)} package files; no archive created")
    raise SystemExit(0)
output.parent.mkdir(parents=True, exist_ok=True)
if output.name.endswith(".tar.gz"):
    with output.open("xb") as stream, tarfile.open(fileobj=stream, mode="w:gz") as archive:
        for relative, path in sorted(files.items()):
            archive.add(path, arcname="GeoModelBridge/" + relative, recursive=False)
elif output.suffix == ".zip":
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for relative, path in sorted(files.items()):
            archive.write(path, "GeoModelBridge/" + relative)
    with zipfile.ZipFile(output) as archive:
        if problem := archive.testzip(): raise SystemExit(f"Archive integrity check failed: {problem}")
else:
    raise SystemExit("Use .tar.gz (preserves Linux executable permissions) or .zip")
with output.open("rb") as file:
    digest = hashlib.file_digest(file, "sha256").hexdigest()
with output.with_suffix(output.suffix + ".sha256").open("x", encoding="utf-8") as file:
    file.write(f"{digest}  {output.name}\n")
print(f"Packaged {len(files)} files: {output}\nSHA256 {digest}")
