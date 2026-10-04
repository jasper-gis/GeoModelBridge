"""STL grammar, exact binary framing, normals, placement and safe rejection."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

exe, fixtures = (Path(p).resolve() for p in sys.argv[1:3])
base = (fixtures / "mesh_quad.stl").read_text(encoding="ascii")
points = ((0, 0, 0), (2, 0, 0), (2, 3, 0), (0, 3, 0))
facets = [((0, 0, 1), (points[0], points[1], points[2]), 0),
          ((0, 0, 1), (points[0], points[2], points[3]), 0)]
cases = 0


def binary(records=facets, header=b"GeoModelBridge STL fixture", count=None):
    assert len(header) <= 80
    return header.ljust(80, b"\0") + struct.pack("<I", len(records) if count is None else count) + b"".join(
        struct.pack("<12fH", *normal, *(value for p in triangle for value in p), attribute)
        for normal, triangle, attribute in records)


with tempfile.TemporaryDirectory(prefix="gmb-stl-") as directory:
    root = Path(directory)

    def prepare(name, content=base, code=None, options=()):
        global cases
        cases += 1
        source = root / (name + ".STL")
        source.write_bytes(content.encode("ascii") if isinstance(content, str) else content)
        output, report = root / (name + "-bundle"), root / (name + ".json")
        result = subprocess.run([str(exe), "prepare", str(source), "--output", str(output),
                                 "--report", str(report), *map(str, options)], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=60)
        assert (result.returncode == 0) == (code is None), (name, result.returncode, result.stdout, result.stderr)
        report_data = json.loads(report.read_text(encoding="utf-8"))
        assert report_data["fidelity"]["gdb_written"] is False
        if code:
            assert result.returncode == 3 and not output.exists(), (name, result.returncode)
            assert code in {d["code"] for d in report_data["diagnostics"]}, (name, report_data)
            assert report_data["geometry_bounds"] is None
            return report_data
        scene = json.loads((output / "scene.json").read_text(encoding="utf-8"))
        assert not scene["textures"]
        return scene

    placed = prepare("中文 & ascii", options=("--wkid", 32650, "--origin", 500000, 3000000, 100))
    mesh = placed["meshes"][0]
    assert len(mesh["vertices"]) == 6 and len(mesh["triangles"]) == 2
    assert {tuple(v["position"]) for v in mesh["vertices"]} == {
        (500000, 3000000, 100), (500002, 3000000, 100), (500002, 3000003, 100), (500000, 3000003, 100)}
    assert all(v["normal"] == [0, 0, 1] and v["uv"] is None for v in mesh["vertices"])
    assert placed["coordinates"]["origin"] == [500000, 3000000, 100]
    assert placed["materials"] == [{"name": "STL default white", "color": [1, 1, 1, 1],
                                    "texture": -1, "double_sided": True}]
    assert {d["code"] for d in placed["diagnostics"]} == {"STL_COORDINATE_ASSUMPTION", "STL_MATERIAL_ASSUMPTION"}

    for label, header in (("binary", b"fixture"), ("binary-solid", b"solid binary header")):
        decoded = prepare(label, binary(header=header))
        assert decoded["meshes"][0]["triangles"] == mesh["triangles"]
        assert {tuple(v["position"]) for v in decoded["meshes"][0]["vertices"]} == set(points)
        assert all(v["normal"] == [0, 0, 1] for v in decoded["meshes"][0]["vertices"])

    # ASCII case and CRLF filenames are independent from exact grammar keywords.
    prepare("crlf", base.replace("\n", "\r\n"))
    prepare("signed-exponent", base.replace("vertex 2", "vertex +2e0"))
    prepare("negative-position", base.replace("vertex 2", "vertex -2"))
    unnamed = base.replace("solid Quad", "solid")
    prepare("unnamed-solid", unnamed)
    prepare("unnamed-end", base.replace("endsolid Quad", "endsolid"))
    doubled = prepare("multiple-solids", base + "\n" + base.replace("Quad", "Second"))
    assert len(doubled["meshes"]) == 2 and len(doubled["nodes"]) == 2
    assert [n["meshes"] for n in doubled["nodes"]] == [[0], [1]]

    y_up = prepare("y-up-centimetres", options=("--mesh-up-axis", "Y", "--mesh-unit-meters", .01))
    assert {tuple(v["position"]) for v in y_up["meshes"][0]["vertices"]} == {
        (0, 0, 0), (.02, 0, 0), (.02, 0, .03), (0, 0, .03)}
    assert all(v["normal"] == [0, -1, 0] for v in y_up["meshes"][0]["vertices"])
    assert "Y-up" in next(d["message"] for d in y_up["diagnostics"] if d["code"] == "STL_COORDINATE_ASSUMPTION")
    long_normal = prepare("normalize", base.replace("normal 0 0 1", "normal 0 0 9e200"))
    assert all(v["normal"] == [0, 0, 1] for v in long_normal["meshes"][0]["vertices"])
    distinct = prepare("normal-boundary", base.replace("normal 0 0 1", "normal 0 1 1", 1))
    assert len(distinct["meshes"][0]["vertices"]) == 6
    assert distinct["meshes"][0]["vertices"][0]["normal"] != distinct["meshes"][0]["vertices"][3]["normal"]

    for label, content in (
        ("binary-short-header", b"binary"), ("binary-short", binary()[:-1]),
        ("binary-trailing", binary() + b"x"), ("binary-count", binary(count=1)),
        ("binary-huge-count", binary(count=0xffffffff)),
        ("binary-solid-short", binary(header=b"solid")[:-1]),
        ("bad-keyword", base.replace("outer loop", "inner loop")),
        ("missing-normal", base.replace("normal 0 0 1", "")),
        ("missing-end", base.replace("endsolid Quad", "")),
        ("missing-vertex", base.replace("      vertex 2 0 0\n", "")),
        ("extra-vertex", base.replace("    endloop", "      vertex 1 2 3\n    endloop")),
        ("name-mismatch", base.replace("endsolid Quad", "endsolid Other")),
        ("trailing-directive", base + "color 1 0 0"),
        ("unknown-render-directive", base.replace("    outer loop", "    color 1 0 0\n    outer loop")),
        ("multiple-signs", base.replace("vertex 2", "vertex +-2")),
        ("hex-number", base.replace("vertex 2", "vertex 0x1p1")),
        ("range-overflow", base.replace("vertex 2", "vertex 2e999")),
        ("token-limit", base.replace("vertex 2", "vertex " + "1" * 257)),
        ("name-limit", base.replace("solid Quad", "solid " + "a" * 4097)),
        ("nonascii", base.encode("ascii") + b"\xff"),
        ("embedded-nul", base.encode("ascii") + b"\0"),
    ):
        prepare(label, content, "INVALID_STL")
    prepare("empty-binary", binary([]), "EMPTY_MESH")
    prepare("empty-ascii", "solid Empty\nendsolid Empty\n", "EMPTY_MESH")
    prepare("partial-after-valid-solid", base + "solid Empty\nendsolid Empty\n", "EMPTY_MESH")
    for attribute in (1, 0x1f, 0x8000, 0xffff):
        records = [(facets[0][0], facets[0][1], attribute), facets[1]]
        for profile in ("strict", "gis-static"):
            prepare("attribute-" + str(attribute) + "-" + profile, binary(records),
                    "UNSUPPORTED_STL_ATTRIBUTE", ("--profile", profile))
    for i, header in enumerate((b"COLOR=\xff\x00\x00\xff", b"MATERIAL=anything", b"TEXTURE=x", b"color=red")):
        prepare("header-extension-" + str(i), binary(header=header), "UNSUPPORTED_STL_COLOR")

    for label, normal in (("zero", "0 0 0"), ("nan", "nan 0 1"), ("infinity", "0 inf 1")):
        changed = base.replace("normal 0 0 1", "normal " + normal, 1)
        prepare("normal-" + label, changed, "INVALID_NORMAL")
        repaired = prepare("repair-" + label, changed, options=("--profile", "gis-static"))
        assert all(v["normal"] == [0, 0, 1] for v in repaired["meshes"][0]["vertices"])
        diagnostic = next(d for d in repaired["diagnostics"] if d["code"] == "NORMALS_REPAIRED")
        assert "3 invalid corner normals across 1 triangles" in diagnostic["message"]
    repair_y = prepare("repair-y-up", base.replace("normal 0 0 1", "normal 0 0 0"),
                       options=("--profile", "gis-static", "--mesh-up-axis", "Y"))
    assert all(v["normal"] == [0, -1, 0] for v in repair_y["meshes"][0]["vertices"])
    partial_normal = prepare("repair-preserves-valid", base.replace("normal 0 0 1", "normal 0 0 0", 1)
                             .replace("normal 0 0 1", "normal 0 1 1"), options=("--profile", "gis-static"))
    assert partial_normal["meshes"][0]["vertices"][0]["normal"] == [0, 0, 1]
    assert partial_normal["meshes"][0]["vertices"][3]["normal"][1] > .7
    for label, normal in (("zero", (0, 0, 0)), ("nan", (float("nan"), 0, 1)),
                           ("infinity", (0, float("inf"), 1))):
        changed = binary([(normal, facets[0][1], 0), facets[1]])
        prepare("binary-normal-" + label, changed, "INVALID_NORMAL")
        repaired = prepare("binary-repair-" + label, changed, options=("--profile", "gis-static"))
        assert all(v["normal"] == [0, 0, 1] for v in repaired["meshes"][0]["vertices"])
    for profile in ("strict", "gis-static"):
        prepare("position-nan-" + profile, base.replace("vertex 2", "vertex nan", 1),
                "NONFINITE_VERTEX", ("--profile", profile))
        prepare("position-infinity-" + profile, binary([
            (facets[0][0], ((float("inf"), 0, 0), points[1], points[2]), 0), facets[1]]),
                "NONFINITE_VERTEX", ("--profile", profile))
        prepare("unit-overflow-" + profile, options=("--profile", profile, "--mesh-unit-meters", "1e308"),
                code="NONFINITE_VERTEX")
        underflow = base.replace("vertex 2 0 0", "vertex 1e-200 0 0", 1).replace(
            "vertex 2 3 0", "vertex 0 1e-200 0", 1).replace("vertex 2 3 0", "vertex 2e200 3e200 0").replace(
            "vertex 0 3 0", "vertex 0 3e200 0")
        prepare("unit-underflow-" + profile, underflow, "UNREPRESENTABLE_COORDINATE",
                ("--profile", profile, "--mesh-unit-meters", "1e-200"))
        prepare("area-overflow-" + profile, base.replace("vertex 2", "vertex 2e200"),
                "DEGENERATE_TRIANGLE", ("--profile", profile))
        prepare("tiny-nonzero-" + profile, base.replace("vertex 2", "vertex 2e-200").replace("3 0", "3e-200 0"),
                "DEGENERATE_TRIANGLE", ("--profile", profile))
        # Products round equal in binary64, but the exact determinant is 2**-104.
        # Normalizing both edges first also makes this triangle appear collinear.
        # It must fail, never be quietly removed while the second face survives.
        rounded_zero = base.replace("vertex 2 0 0", "vertex 1 1.0000000000000002 0", 1).replace(
            "vertex 2 3 0", "vertex 0.9999999999999998 1 0", 1)
        prepare("rounded-nonzero-" + profile, rounded_zero, "DEGENERATE_TRIANGLE", ("--profile", profile))

    degenerate = base.replace("vertex 2 0 0", "vertex 0 0 0", 1)
    prepare("degenerate", degenerate, "DEGENERATE_TRIANGLE")
    retained = prepare("remove-degenerate", degenerate, options=("--profile", "gis-static"))
    assert len(retained["meshes"][0]["triangles"]) == 1 and len(retained["meshes"][0]["vertices"]) == 3
    assert any(d["code"] == "DEGENERATE_TRIANGLES_REMOVED" for d in retained["diagnostics"])
    discarded = prepare("discard-degenerate-normal", degenerate.replace("normal 0 0 1", "normal nan 0 1", 1),
                        options=("--profile", "gis-static"))
    assert any(d["code"] == "DEGENERATE_NORMALS_DISCARDED" and "3 invalid normals" in d["message"]
               for d in discarded["diagnostics"])
    assert not any(d["code"] == "NORMALS_REPAIRED" for d in discarded["diagnostics"])
    prepare("all-degenerate", base.replace("vertex 2", "vertex 0"), "EMPTY_MESH", ("--profile", "gis-static"))

    report = root / "inspection.json"
    result = subprocess.run([str(exe), "inspect", str(fixtures / "mesh_quad.stl"), "--report", str(report)],
                            capture_output=True, timeout=60)
    assert result.returncode == 0, result.stderr
    inspected = json.loads(report.read_text(encoding="utf-8"))
    assert inspected["geometry_bounds"] == {"min": [0, 0, 0], "max": [2, 3, 0]}
    assert inspected["counts"] == {"meshes": 1, "triangles": 2, "corner_vertices": 6,
                                    "materials": 1, "textures": 0, "texture_bytes": 0}
    assert inspected["fidelity"]["validation_passed"] is True
    assert inspected["fidelity"]["strict_validation_passed"] is True
    assert inspected["fidelity"]["compatibility_adjustments"] is False
    sentinel = report.read_bytes()
    assert subprocess.run([str(exe), "inspect", str(fixtures / "mesh_quad.stl"), "--report", str(report)],
                          capture_output=True, timeout=60).returncode != 0
    assert report.read_bytes() == sentinel
    bundle = root / "binary-bundle"
    sentinel = (bundle / "scene.json").read_bytes()
    assert subprocess.run([str(exe), "prepare", str(fixtures / "mesh_quad.stl"), "--output", str(bundle)],
                          capture_output=True, timeout=60).returncode != 0
    assert (bundle / "scene.json").read_bytes() == sentinel

print(f"PASS STL: {cases} prepare cases, ASCII/binary, exact framing, corner normals, placement, rejection and output safety")
