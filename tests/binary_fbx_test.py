"""Offline CLI acceptance of unchanged, attributed exporter-produced binary FBX."""

from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import sys
import tempfile

from cli_test import invoke


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def main():
    exe = Path(sys.argv[1]).resolve()
    fixture_root = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).parent / "fixtures"
    upstream = fixture_root / "upstream"
    manifest = read_json(upstream / "manifest.json")
    assert manifest["revision"] == "fcc5d6ba444cfd3eb80677dba5e37e493941abe5"
    for item in manifest["files"]:
        payload = (upstream / item["file"]).read_bytes()
        assert len(payload) == item["bytes"], item["file"]
        assert hashlib.sha256(payload).hexdigest() == item["sha256"], item["file"]
        if item["file"].endswith(".fbx"):
            assert payload[:23] == b"Kaydara FBX Binary  \0\x1a\0"
            assert struct.unpack_from("<I", payload, 23)[0] == 7400

    with tempfile.TemporaryDirectory(prefix="geomodelbridge-binary-") as scratch:
        root = Path(scratch)
        # Unicode source names exercise Windows-wide file opening for binary FBX.
        source = root / "二进制立方体.fbx"
        shutil.copyfile(upstream / "blender_293_half_smooth_cube_7400_binary.fbx", source)
        inspect_report = root / "inspect.json"
        invoke(exe, "inspect", source, "--report", inspect_report)
        inspected = read_json(inspect_report)
        assert inspected["status"] == "inspected"
        assert inspected["counts"]["meshes"] == 1
        assert inspected["counts"]["triangles"] == 12
        assert not any(d["severity"] == "error" for d in inspected["diagnostics"])
        assert "DEFAULT_MATERIAL_ASSIGNED" in {d["code"] for d in inspected["diagnostics"]}

        output = root / "prepared"
        report_path = root / "prepare.json"
        invoke(exe, "prepare", source, "--output", output, "--report", report_path)
        report = read_json(report_path)
        assert report["status"] == "prepared"
        assert report["fidelity"]["strict_validation_passed"] is True
        assert report["fidelity"]["gdb_written"] is False
        scene = read_json(output / "scene.json")
        assert scene["coordinates"]["unit"] == "meter"
        assert scene["coordinates"]["up_axis"] == "Z"
        assert len(scene["materials"]) == 1
        assert scene["materials"][0]["color"] == [1, 1, 1, 1]
        assert not scene["textures"]
        mesh = scene["meshes"][0]
        assert len(mesh["triangles"]) == 12
        assert len(mesh["vertices"]) == 36
        normals, uv_sets = defaultdict(set), defaultdict(set)
        for vertex in mesh["vertices"]:
            position = tuple(round(v, 5) for v in vertex["position"])
            assert all(abs(abs(v) - 1.0) < 1e-5 for v in vertex["position"])
            assert len(vertex["normal"]) == 3
            assert math.isclose(sum(v * v for v in vertex["normal"]), 1.0, abs_tol=1e-10)
            assert len(vertex["uv"]) == 2
            normals[position].add(tuple(round(v, 5) for v in vertex["normal"]))
            uv_sets[position].add(tuple(round(v, 5) for v in vertex["uv"]))
        assert len(normals) == 8, "Binary cube position topology changed"
        assert any(len(values) > 1 for values in normals.values()), "Hard normal seams were welded"
        assert any(len(values) > 1 for values in uv_sets.values()), "UV seams were welded"
        for triangle in mesh["triangles"]:
            assert triangle["material"] == 0
            corners = [mesh["vertices"][i] for i in triangle["indices"]]
            a, b, c = [corner["position"] for corner in corners]
            u, v = [b[i] - a[i] for i in range(3)], [c[i] - a[i] for i in range(3)]
            cross = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            assert all(sum(cross[i] * corner["normal"][i] for i in range(3)) > 0 for corner in corners)

        # Maya serializes an empty animation stack and neutral rendering defaults.
        # These are metadata, not an active animation or displacement effect.
        # V0.1.2 rejected this original fixture; neither policy should now do so.
        maya_scenes = []
        for profile in ("strict", "gis-static"):
            maya_output, maya_report = root / ("maya-" + profile), root / ("maya-" + profile + ".json")
            invoke(exe, "prepare", upstream / "maya_cube_7400_binary.fbx",
                   "--output", maya_output, "--report", maya_report, "--profile", profile)
            report = read_json(maya_report)
            assert report["status"] == "prepared" and report["conversion_profile"] == profile
            assert report["counts"]["meshes"] == 1 and report["counts"]["triangles"] == 12
            assert not any(d["severity"] == "error" for d in report["diagnostics"])
            assert "EMPTY_ANIMATION_IGNORED" in {d["code"] for d in report["diagnostics"]}
            assert report["fidelity"]["strict_validation_passed"] is (profile == "strict")
            assert report["fidelity"]["validation_passed"] is True
            assert report["fidelity"]["compatibility_adjustments"] is False
            assert report["fidelity"]["gdb_written"] is False
            scene = read_json(maya_output / "scene.json")
            assert len(scene["materials"]) == 1 and not scene["textures"]
            assert len(scene["meshes"]) == 1 and len(scene["meshes"][0]["triangles"]) == 12
            positions = [vertex["position"] for vertex in scene["meshes"][0]["vertices"]]
            assert all(all(math.isfinite(v) for v in point) for point in positions)
            maya_scenes.append(scene)
        assert maya_scenes[0]["meshes"] == maya_scenes[1]["meshes"], "An empty animation container changed the geometry between policies"
        assert maya_scenes[0]["materials"] == maya_scenes[1]["materials"], "Neutral default properties changed the material between policies"

    print("PASS upstream binary FBX: verified original bytes, Unicode input, geometry, normals, UV seams, and neutral exporter defaults")


if __name__ == "__main__":
    main()
