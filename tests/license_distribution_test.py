"""Check license rejection and real archive contents with synthetic native probes.

No SDK, compiled programs or real GDB verification is used by this test.
"""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "scripts"))
version = (root / "VERSION").read_text(encoding="utf-8").strip()
notices = ("LICENSE", "THIRD_PARTY_NOTICES.md")


class LicenseDistributionTest(unittest.TestCase):
    def test_missing_or_stale_installed_notices(self):
        for name in notices:
            for stale in (False, True):
                with self.subTest(name=name, stale=stale), tempfile.TemporaryDirectory() as directory:
                    install = Path(directory)
                    for other in notices:
                        shutil.copyfile(root / other, install / other)
                    if stale:
                        (install / name).write_bytes(b"outdated notice\n")
                    else:
                        (install / name).unlink()
                    result = subprocess.run(
                        [sys.executable, str(root / "scripts/package.py"), "--check-only",
                         "--install-dir", str(install)], capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("Installed license/notice is missing or stale: " + name, result.stderr)

    def test_archive_notices_on_both_platforms(self):
        # Use the real source enumeration, but replace native release probes only
        # within this test. Creating a synthetic archive is not release evidence.
        listing = subprocess.check_output(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=root)
        for target in ("windows-x64", "linux-x64"):
            for suffix in (".zip", ".tar.gz"):
                with self.subTest(target=target, suffix=suffix), tempfile.TemporaryDirectory() as directory:
                    work = Path(directory)
                    install, examples = work / "dist", work / "examples"
                    install.mkdir()
                    examples.mkdir()
                    cli = "bin/geomodelbridge" + (".exe" if target == "windows-x64" else "")
                    native = "bin/native-filegdb/GeoModelBridge.NativeWriter" + (".exe" if target == "windows-x64" else "")
                    runtime_names = (["bin64/FileGDBAPI.dll"] if target == "windows-x64"
                                     else ["lib/libFileGDBAPI.so", "lib/libfgdbunixrtl.so"])
                    for name in (cli, native, *("bin/native-filegdb/" + Path(p).name for p in runtime_names)):
                        path = install / name
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(b"synthetic program: never executed\n")
                    copies = {name: name for name in notices}
                    for name in ("textured_quad.fbx", "textured_quad.obj", "textured_quad.mtl",
                                 "textured_quad.glb", "textured_quad.gltf", "textured_quad.bin",
                                 "textured_quad.wrl", "textured_quad.dae", "colored_quad.ply",
                                 "mesh_quad.stl", "textured_quad.x3d", "checker.png"):
                        copies["bin/demo/" + name] = "tests/fixtures/" + name
                    copies["bin/max-adapter/worker.py"] = "adapters/3dsmax/worker.py"
                    copies["licenses/tinyxml2/LICENSE.txt"] = "third_party/tinyxml2/LICENSE.txt"
                    copies["licenses/ufbx/LICENSE"] = "third_party/ufbx/LICENSE"
                    for relative in ("geomodelbridge/__init__.py", "geomodelbridge/_version.py",
                                     "geomodelbridge/client.py", "examples/convert_fbx.py"):
                        copies["python/" + relative] = "python/" + relative
                    for destination, source in copies.items():
                        path = install / destination
                        path.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(root / source, path)
                    sdk_notice = install / "licenses/filegdb-api/nested/test-notice.txt"
                    sdk_notice.parent.mkdir(parents=True)
                    sdk_notice.write_bytes(b"synthetic SDK notice for recursive collection test\n")
                    names = ("color-cube", "uv-plane", "mixed-materials", "alpha-plane", "seam-cube",
                             "colored_quad", "textured_quad", "embedded_quad", "uv_transform",
                             "instanced_mirror", "multi_material", "no_material", "sloped_normals", "binary_blender")
                    (examples / "verification-summary.json").write_text(json.dumps({
                        "version": version, "backend": "native-filegdb", "status": "passed",
                        "standalone_copy_verified": True,
                        "gdb_cases": [{"name": name, "passed": True} for name in names]}), encoding="utf-8")
                    (examples / "reports").mkdir()
                    for name in names:
                        (examples / "reports" / (name + ".json")).write_text(json.dumps({
                            "version": version, "backend": "native-filegdb"}), encoding="utf-8")

                    def probe(command, **kwargs):
                        if command[0] == "git":
                            return listing
                        if command == [str(install / cli), "--version"]:
                            return f"GeoModelBridge V{version}\n"
                        if command == [str(install / native), "--probe"]:
                            return json.dumps({"version": version, "status": "available",
                                               "backend": "native-filegdb", "arcgis_pro_required": False})
                        raise AssertionError("Unexpected probe: " + str(command))

                    archive_path = work / ("synthetic" + suffix)
                    argv = [str(root / "scripts/package.py"), "--install-dir", str(install),
                            "--examples-dir", str(examples), "--output", str(archive_path)]
                    with patch.object(sys, "argv", argv), patch("gmb_platform.platform_name", return_value=target), \
                            patch("gmb_platform.executable_names", return_value=(cli, native)), \
                            patch("gmb_platform.sdk_manifest", return_value={"runtime_files": runtime_names}), \
                            patch("subprocess.check_output", side_effect=probe), \
                            patch("subprocess.run", return_value=subprocess.CompletedProcess([], 0)), \
                            contextlib.redirect_stdout(io.StringIO()):
                        runpy.run_path(str(root / "scripts/package.py"), run_name="__main__")
                    if suffix == ".zip":
                        with zipfile.ZipFile(archive_path) as archive:
                            self.assertIsNone(archive.testzip())
                            content = {name: archive.read(name) for name in archive.namelist()}
                    else:
                        with tarfile.open(archive_path) as archive:
                            content = {member.name: archive.extractfile(member).read()
                                       for member in archive.getmembers() if member.isfile()}
                    for name in notices:
                        for prefix in ("GeoModelBridge/", "GeoModelBridge/dist/"):
                            self.assertEqual(content[prefix + name], (root / name).read_bytes())
                    for name, source in copies.items():
                        if name.startswith("licenses/"):
                            self.assertEqual(content["GeoModelBridge/dist/" + name], (root / source).read_bytes())
                    self.assertEqual(content["GeoModelBridge/dist/licenses/filegdb-api/nested/test-notice.txt"], sdk_notice.read_bytes())
                    self.assertEqual(content["GeoModelBridge/tests/fixtures/upstream/LICENSE"],
                                     (root / "tests/fixtures/upstream/LICENSE").read_bytes())
                    digest = hashlib.sha256(archive_path.read_bytes()).hexdigest()
                    self.assertEqual(archive_path.with_suffix(archive_path.suffix + ".sha256").read_text().split()[0], digest)


if __name__ == "__main__":
    unittest.main()
