"""Exercise the actual FBX parser, canonical scene, and CLI failure reports."""

import hashlib
import json
import math
from pathlib import Path
import shutil
import sys
import tempfile

from cli_test import invoke


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def near(actual, expected):
    assert len(actual) == len(expected)
    assert all(math.isclose(a, b, rel_tol=1e-10, abs_tol=1e-10) for a, b in zip(actual, expected)), (actual, expected)


def bounds(mesh):
    positions = [vertex["position"] for vertex in mesh["vertices"]]
    return [min(p[axis] for p in positions) for axis in range(3)] + [max(p[axis] for p in positions) for axis in range(3)]


def prepare(exe, source, destination, *options):
    report = destination.with_suffix(".report.json")
    invoke(exe, "prepare", source, "--output", destination, "--report", report, *options)
    summary = read_json(report)
    assert summary["status"] == "prepared"
    assert summary["fidelity"]["strict_validation_passed"] is True
    assert summary["fidelity"]["gdb_written"] is False
    assert summary["fidelity"]["gdb_readback_verified"] is False
    scene = read_json(destination / "scene.json")
    assert len(scene["meshes"]) == summary["counts"]["meshes"]
    for texture in scene["textures"]:
        payload = (destination / texture["path"]).read_bytes()
        assert hashlib.sha256(payload).hexdigest() == texture["sha256"]
        assert len(payload) == texture["byte_length"]
    return scene


def winding_agrees_with_normals(mesh):
    for tri in mesh["triangles"]:
        corners = [mesh["vertices"][index] for index in tri["indices"]]
        a, b, c = [v["position"] for v in corners]
        u = [b[i] - a[i] for i in range(3)]
        v = [c[i] - a[i] for i in range(3)]
        cross = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        for corner in corners:
            assert sum(cross[i] * corner["normal"][i] for i in range(3)) > 0, "Mirroring inverted winding relative to normals"


def main():
    exe = Path(sys.argv[1]).resolve()
    fixtures = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).parent / "fixtures"
    with tempfile.TemporaryDirectory(prefix="geomodelbridge-reader-") as scratch:
        root = Path(scratch)
        inspect_report = root / "inspect.json"
        invoke(exe, "inspect", fixtures / "colored_quad.fbx", "--report", inspect_report)
        inspected = read_json(inspect_report)
        assert inspected["status"] == "inspected"
        assert inspected["counts"]["meshes"] == 1 and inspected["counts"]["triangles"] == 2
        color = prepare(exe, fixtures / "colored_quad.fbx", root / "color")
        near(color["materials"][0]["color"], [.4, .1, .05, .75])
        assert not color["textures"]
        near(bounds(color["meshes"][0]), [0, 0, 0, 2, 3, 0])
        winding_agrees_with_normals(color["meshes"][0])

        expected_png = (fixtures / "checker.png").read_bytes()
        for name, embedded in [("textured_quad", False), ("embedded_quad", True)]:
            destination = root / name
            scene = prepare(exe, fixtures / f"{name}.fbx", destination)
            assert len(scene["textures"]) == 1
            texture = scene["textures"][0]
            assert texture["embedded"] is embedded
            assert (destination / texture["path"]).read_bytes() == expected_png, "FBX image bytes changed"
            corners = scene["meshes"][0]["vertices"]
            assert {tuple(vertex["uv"]) for vertex in corners} == {(0, 0), (1, 0), (1, 1), (0, 1)}, "Selected the wrong UV set"
            for vertex in corners:
                near(vertex["uv"], [vertex["position"][0] / 2, vertex["position"][1] / 3])

        transformed = prepare(exe, fixtures / "uv_transform.fbx", root / "uv-transform")
        for vertex in transformed["meshes"][0]["vertices"]:
            u, v = vertex["position"][0] / 2, vertex["position"][1] / 3
            near(vertex["uv"], [(u - .25) / 2, (v - .5) / 3])

        mirrored = prepare(exe, fixtures / "instanced_mirror.fbx", root / "mirrored")
        assert len(mirrored["meshes"]) == 2
        ordered = sorted(mirrored["meshes"], key=lambda mesh: bounds(mesh)[0])
        near(bounds(ordered[0]), [0, 0, 5, 2, 3, 5])
        near(bounds(ordered[1]), [104, 220, 330, 108, 229, 330])
        for mesh in ordered:
            assert len(mesh["triangles"]) == 2
            winding_agrees_with_normals(mesh)
        colors = [mirrored["materials"][mesh["triangles"][0]["material"]]["color"] for mesh in ordered]
        near(colors[0], [0, 0, 1, 1])
        near(colors[1], [.4, .1, .05, .75])

        multiple = prepare(exe, fixtures / "multi_material.fbx", root / "multiple")
        assert len(multiple["meshes"]) == 1
        triangles = multiple["meshes"][0]["triangles"]
        assert len(triangles) == 2 and triangles[0]["material"] != triangles[1]["material"]
        near(multiple["materials"][triangles[0]["material"]]["color"], [.4, .1, .05, .75])
        near(multiple["materials"][triangles[1]["material"]]["color"], [0, 0, 1, 1])

        sloped = prepare(exe, fixtures / "sloped_normals.fbx", root / "sloped")
        near(bounds(sloped["meshes"][0]), [0, 0, 0, 2, 3, 4])
        for vertex in sloped["meshes"][0]["vertices"]:
            near(vertex["normal"], [0, -.8, .6])
        winding_agrees_with_normals(sloped["meshes"][0])

        no_material = prepare(exe, fixtures / "no_material.fbx", root / "default-material")
        assert len(no_material["materials"]) == 1
        near(no_material["materials"][0]["color"], [1, 1, 1, 1])
        default_report = read_json(root / "default-material.report.json")
        assert "DEFAULT_MATERIAL_ASSIGNED" in {d["code"] for d in default_report["diagnostics"] if d["severity"] == "warning"}

        # Test unit conversion independently by changing only the declared source unit.
        centimeters = root / "centimeters.fbx"
        source_text = (fixtures / "colored_quad.fbx").read_text(encoding="utf-8")
        needle = 'P: "UnitScaleFactor", "double", "Number", "", 100'
        assert source_text.count(needle) == 1
        centimeters.write_text(source_text.replace(needle, 'P: "UnitScaleFactor", "double", "Number", "", 1'), encoding="utf-8")
        metric = prepare(exe, centimeters, root / "metric")
        near(bounds(metric["meshes"][0]), [0, 0, 0, .02, .03, 0])

        # Source Y-up must become Z-up without flattening the vertical coordinate.
        y_up = root / "y-up.fbx"
        y_up.write_text(source_text.replace('P: "UpAxis", "int", "Integer", "", 2', 'P: "UpAxis", "int", "Integer", "", 1')
                        .replace('P: "FrontAxis", "int", "Integer", "", 1', 'P: "FrontAxis", "int", "Integer", "", 2'), encoding="utf-8")
        canonical = prepare(exe, y_up, root / "z-up")
        near(bounds(canonical["meshes"][0]), [0, 0, 0, 2, 0, 3])
        winding_agrees_with_normals(canonical["meshes"][0])

        # Models can be relocated with their textures in an explicit search directory.
        relocated_dir = root / "relocated"
        relocated_dir.mkdir()
        relocated = relocated_dir / "textured.fbx"
        shutil.copyfile(fixtures / "textured_quad.fbx", relocated)
        search = prepare(exe, relocated, root / "searched", "--texture-dir", fixtures)
        assert len(search["textures"]) == 1

        for name, expected_code in [("missing_texture", "MISSING_TEXTURE"), ("unsupported_emission", "UNSUPPORTED_MATERIAL_CHANNEL")]:
            destination = root / (name + "-rejected")
            report = root / (name + ".json")
            result = invoke(exe, "prepare", fixtures / f"{name}.fbx", "--output", destination, "--report", report, expect_success=False)
            assert result.returncode == 3, (result.returncode, result.stderr)
            assert not destination.exists(), "Rejected FBX left a partial bundle"
            rejected = read_json(report)
            assert rejected["status"] == "rejected"
            assert rejected["fidelity"]["strict_validation_passed"] is False
            assert expected_code in {d["code"] for d in rejected["diagnostics"] if d["severity"] == "error"}

        # A filesystem failure must not retain a success/prepared report.
        blocked_parent = root / "ordinary-file"
        blocked_parent.write_text("user content", encoding="utf-8")
        failure_report = root / "io-failure.json"
        invoke(exe, "prepare", fixtures / "colored_quad.fbx", "--output", blocked_parent / "bundle", "--report", failure_report, expect_success=False)
        assert read_json(failure_report)["status"] == "failed", "I/O failure retained a successful preparation report"
        assert blocked_parent.read_text(encoding="utf-8") == "user content"

    print("PASS actual FBX parsing, units, transforms, mirrors, materials, selected UVs, original external/embedded textures, diagnostics and failure reports")


if __name__ == "__main__":
    main()
