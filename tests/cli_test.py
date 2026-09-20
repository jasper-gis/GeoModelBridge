"""End-to-end checks using the built executable and Python standard library only."""

import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def invoke(exe, *args, expect_success=True):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if expect_success and result.returncode:
        raise AssertionError(f"Command failed ({result.returncode}): {args}\n{result.stdout}\n{result.stderr}")
    if not expect_success and not result.returncode:
        raise AssertionError(f"Command unexpectedly succeeded: {args}")
    return result


def decode_fixture_png(data):
    """An independent decoder verifies generated PNG CRCs, zlib stream, and pixels."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    position, compressed, header, ended = 8, bytearray(), None, False
    while position < len(data):
        size = struct.unpack_from(">I", data, position)[0]
        kind = data[position + 4:position + 8]
        payload = data[position + 8:position + 8 + size]
        expected_crc = struct.unpack_from(">I", data, position + 8 + size)[0]
        assert zlib.crc32(kind + payload) & 0xFFFFFFFF == expected_crc, "PNG chunk checksum failed"
        position += 12 + size
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", payload)
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            ended = True
            break
    assert ended and position == len(data), "Truncated PNG or unexpected trailing bytes"
    assert header == (128, 128, 8, 6, 0, 0, 0), f"Unexpected fixture image format: {header}"
    raw = zlib.decompress(compressed)
    assert len(raw) == 128 * (1 + 128 * 4)
    pixels = bytearray()
    for y in range(128):
        row = raw[y * 513:(y + 1) * 513]
        assert row[0] == 0, "Fixture encoder changed; update independent decoder"
        pixels.extend(row[1:])
    return pixels


def read_bundle(directory):
    manifest = json.loads((directory / "scene.json").read_text(encoding="utf-8"))
    assert manifest["schema_version"] == 1
    assert manifest["generator"] == "GeoModelBridge"
    assert manifest["coordinates"]["unit"] == "meter"
    assert manifest["coordinates"]["up_axis"] == "Z"
    for texture in manifest["textures"]:
        relative = Path(texture["path"])
        assert not relative.is_absolute() and ".." not in relative.parts
        data = (directory / relative).read_bytes()
        assert len(data) == texture["byte_length"]
        assert hashlib.sha256(data).hexdigest() == texture["sha256"]
        assert texture["mime_type"] == "image/png"
        decode_fixture_png(data)
    for mesh in manifest["meshes"]:
        for triangle in mesh["triangles"]:
            assert len(triangle["indices"]) == 3
            assert all(0 <= index < len(mesh["vertices"]) for index in triangle["indices"])
            assert 0 <= triangle["material"] < len(manifest["materials"])
    return manifest


def main():
    exe = Path(sys.argv[1]).resolve()
    assert exe.is_file(), f"Executable not found: {exe}"
    assert "0.1.5" in invoke(exe, "--version").stdout
    doctor = json.loads(invoke(exe, "doctor").stdout)
    assert "native_filegdb" in doctor and "arcgis_pro" not in doctor
    assert "arcgis-pro" not in invoke(exe, "--help").stdout
    with tempfile.TemporaryDirectory(prefix="geomodelbridge-cli-") as scratch:
        root = Path(scratch)
        # Removed backends must fail before input parsing or writer dispatch.
        for backend in ["arcgis-pro", "arcgis-pro-corehost", "unknown"]:
            output = root / (backend + ".gdb")
            result = invoke(exe, "convert", root / "missing.fbx", "--output", output,
                            "--backend", backend, "--wkid", 32650, "--origin", 0, 0, 0,
                            expect_success=False)
            assert result.returncode == 4 and "Only native-filegdb" in result.stderr
            assert not output.exists()
            report = json.loads(Path(str(output) + ".report.json").read_text(encoding="utf-8"))
            assert report["status"] == "failed" and report["diagnostics"][0]["code"] == "BACKEND_UNAVAILABLE"
        fake_dll = root / "writer.dll"
        fake_dll.write_bytes(b"not a native executable")
        result = invoke(exe, "convert", Path(sys.argv[2]) / "textured_quad.fbx",
                        "--output", root / "managed.gdb", "--writer", fake_dll,
                        "--wkid", 32650, "--origin", 0, 0, 0, expect_success=False)
        assert result.returncode == 2 and not (root / "managed.gdb").exists()
        all_fixtures = root / "reference-fixtures"
        invoke(exe, "fixture", "all", "--output", all_fixtures)
        names = {"color-cube", "uv-plane", "mixed-materials", "alpha-plane", "seam-cube"}
        assert names <= {path.name for path in all_fixtures.iterdir() if path.is_dir()}
        manifests = {name: read_bundle(all_fixtures / name) for name in names}
        for manifest in manifests.values():
            assert manifest["coordinates"]["space"] == "local"
            assert manifest["coordinates"]["wkid"] == 0
            assert manifest["coordinates"]["origin_explicit"] is False
        color = manifests["color-cube"]
        assert len(color["textures"]) == 0
        assert len({tuple(material["color"]) for material in color["materials"]}) == 6
        assert len(color["meshes"][0]["triangles"]) == 12
        mixed = manifests["mixed-materials"]
        assert len(mixed["textures"]) == 2 and len(mixed["materials"]) == 3
        assert {triangle["material"] for triangle in mixed["meshes"][0]["triangles"]} == {0, 1, 2}
        assert sum(material["texture"] == -1 for material in mixed["materials"]) == 1
        seam = manifests["seam-cube"]["meshes"][0]
        assert len(seam["vertices"]) == 24
        assert len({tuple(vertex["position"]) for vertex in seam["vertices"]}) == 8
        assert len({tuple(vertex["normal"]) for vertex in seam["vertices"]}) == 6
        alpha = manifests["alpha-plane"]["textures"][0]
        alpha_pixels = decode_fixture_png((all_fixtures / "alpha-plane" / alpha["path"]).read_bytes())
        assert set(alpha_pixels[3::4]) == {0, 128, 255}, "Alpha cutout was flattened or incomplete"
        marker = all_fixtures / "user-file.txt"
        marker.write_text("do not replace", encoding="utf-8")
        before = {path.relative_to(all_fixtures): path.read_bytes() for path in all_fixtures.rglob("*") if path.is_file()}
        invoke(exe, "fixture", "all", "--output", all_fixtures, expect_success=False)
        after = {path.relative_to(all_fixtures): path.read_bytes() for path in all_fixtures.rglob("*") if path.is_file()}
        assert after == before, "Rejected overwrite changed existing output"
        placed = root / "placed"
        invoke(exe, "fixture", "uv-plane", "--output", placed, "--wkid", 32650, "--origin", 500000, 3000000, 100)
        shifted = read_bundle(placed)
        assert shifted["coordinates"]["origin_explicit"] is True
        assert shifted["coordinates"]["wkid"] == 32650
        assert shifted["meshes"][0]["vertices"][0]["position"] == [500000, 3000000, 100]
        assert shifted["meshes"][0]["vertices"][0]["uv"] == [0, 0]
        invoke(exe, "fixture", "does-not-exist", "--output", root / "unknown", expect_success=False)
        assert not (root / "unknown").exists()
        invoke(exe, "prepare", root / "missing.fbx", "--output", root / "missing", expect_success=False)
        assert not (root / "missing").exists()
        invalid = root / "invalid.fbx"
        invalid.write_bytes(b"this is not an FBX document")
        invoke(exe, "inspect", invalid, expect_success=False)
        invoke(exe, "prepare", invalid, "--output", root / "invalid", expect_success=False)
        assert not (root / "invalid").exists()
    print("PASS CLI, bundle integrity, original image bytes, PNG alpha, placement, and overwrite protection")


if __name__ == "__main__":
    main()
