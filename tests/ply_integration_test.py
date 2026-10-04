"""PLY typed data, topology, render preservation and optional native readback."""
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

exe, fixtures = (Path(value).resolve() for value in sys.argv[1:3])
writer = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else None
base = (fixtures / "colored_quad.ply").read_text(encoding="utf-8")
cases = []

with tempfile.TemporaryDirectory(prefix="gmb-ply-") as directory:
    root = Path(directory)

    def run(*args):
        return subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, encoding="utf-8")

    def prepare(name, contents=base, code=None, options=()):
        source = root / (name + ".PLY")
        source.write_bytes(contents.encode("utf-8") if isinstance(contents, str) else contents)
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        output, report = root / (name + "-bundle"), root / (name + ".json")
        process = run("prepare", source, "--output", output, "--report", report, *options)
        assert process.returncode == (3 if code else 0), (name, process.returncode, process.stdout, process.stderr)
        result = json.loads(report.read_text(encoding="utf-8"))
        assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
        cases.append(name)
        if code:
            assert result["status"] == "rejected" and not output.exists(), (name, result)
            assert code in {entry["code"] for entry in result["diagnostics"]}, (name, result)
            assert result["geometry_bounds"] is None
            return result
        assert result["status"] == "prepared" and result["fidelity"]["gdb_written"] is False
        return json.loads((output / "scene.json").read_text(encoding="utf-8"))

    def binary(endian, *, aliases=False, face_first=False):
        header = base[:base.index("end_header\n") + len("end_header\n")]
        header = header.replace("format ascii", "format binary_" + ("little" if endian == "<" else "big") + "_endian")
        if aliases:
            header = header.replace("property float ", "property float32 ").replace("property uchar ", "property uint8 ")
            header = header.replace("property list uchar int ", "property list uint8 int32 ")
        vertices = b"".join(struct.pack(endian + "6f", *p, 0, 0, 1) for p in [(0, 0, 0), (2, 0, 0), (2, 3, 0), (0, 3, 0)])
        faces = b"".join(struct.pack(endian + "B3i4B", 3, *f, 200, 80, 20, 128) for f in [(0, 1, 2), (0, 2, 3)])
        if face_first:
            start, v = header.split("element vertex", 1)
            vertex, face = v.split("element face", 1)
            face = face.removesuffix("end_header\n")
            header = start + "element face" + face + "element vertex" + vertex + "end_header\n"
        return header.encode("ascii") + (faces + vertices if face_first else vertices + faces)

    scene = prepare("中文 颜色", options=("--wkid", "32650", "--origin", "500000", "3000000", "100"))
    mesh = scene["meshes"][0]
    assert len(scene["meshes"]) == len(scene["materials"]) == 1 and not scene["textures"]
    assert len(mesh["vertices"]) == 6 and len(mesh["triangles"]) == 2
    assert all(v["normal"] == [0, 0, 1] for v in mesh["vertices"])
    assert {tuple(v["position"]) for v in mesh["vertices"]} == {
        (500000, 3000000, 100), (500002, 3000000, 100), (500002, 3000003, 100), (500000, 3000003, 100)}
    assert scene["materials"][0]["color"] == [200 / 255, 80 / 255, 20 / 255, 128 / 255]
    assert scene["materials"][0]["double_sided"] is True
    assert scene["coordinates"]["origin"] == [500000, 3000000, 100]
    assert {entry["code"] for entry in scene["diagnostics"]} >= {"PLY_COORDINATE_ASSUMPTION", "PLY_COLOR_MAPPED", "PLY_METADATA_OMITTED"}
    baseline = prepare("ascii-local")
    for name, contents in [("binary-le", binary("<")), ("binary-be", binary(">")),
                           ("binary-aliases", binary(">", aliases=True)), ("binary-face-first", binary("<", face_first=True)),
                           ("crlf", base.replace("\n", "\r\n")), ("cr", base.replace("\n", "\r")),
                           ("no-final-newline", base.rstrip()), ("singular-indices", base.replace("vertex_indices", "vertex_index"))]:
        other = prepare(name, contents)
        for key in ("vertices", "triangles"):
            assert other["meshes"][0][key] == baseline["meshes"][0][key], name
        assert other["materials"] == baseline["materials"], name
    inspect = root / "inspection.json"
    process = run("inspect", root / "ascii-local.PLY", "--report", inspect)
    assert process.returncode == 0, process.stderr
    report = json.loads(inspect.read_text(encoding="utf-8"))
    assert report["status"] == "inspected" and report["backend"] == "none"
    assert report["counts"] == dict(meshes=1, triangles=2, corner_vertices=6, materials=1, textures=0, texture_bytes=0)
    assert report["geometry_bounds"] == dict(min=[0, 0, 0], max=[2, 3, 0])
    cases.append("inspect-counts-bounds")
    converted = prepare("y-up-centimeters", options=("--mesh-up-axis", "Y", "--mesh-unit-meters", "0.01"))
    assert {tuple(v["position"]) for v in converted["meshes"][0]["vertices"]} == {(0, 0, 0), (.02, 0, 0), (.02, 0, .03), (0, 0, .03)}
    assert all(v["normal"] == [0, -1, 0] for v in converted["meshes"][0]["vertices"])

    # A color seam becomes separate materials and complete emitted corners.
    split = prepare("face-colors", base.replace("3 0 2 3 200 80 20 128", "3 0 2 3 0 128 255 255"))
    assert len(split["materials"]) == 2 and len(split["meshes"][0]["vertices"]) == 6
    assert split["meshes"][0]["triangles"][0]["material"] != split["meshes"][0]["triangles"][1]["material"]
    without_normals = base.replace("property float nx\nproperty float ny\nproperty float nz\n", "")
    for value in ("0 0 0", "2 0 0", "2 3 0", "0 3 0"):
        without_normals = without_normals.replace(value + " 0 0 1\n", value + "\n")
    flat = prepare("generated-normals", without_normals)
    assert any(d["code"] == "PLY_DEFAULT_NORMALS_GENERATED" for d in flat["diagnostics"])
    assert all(v["normal"] == [0, 0, 1] for v in flat["meshes"][0]["vertices"])
    face_normals = without_normals.replace("property list uchar int vertex_indices\n", "property list uchar int vertex_indices\nproperty float nx\nproperty float ny\nproperty float nz\n")
    face_normals = face_normals.replace("3 0 1 2 200", "3 0 1 2 0 0 1 200").replace("3 0 2 3 200", "3 0 2 3 0 0 1 200")
    assert all(v["normal"] == [0, 0, 1] for v in prepare("face-normals", face_normals)["meshes"][0]["vertices"])

    plain_header = "ply\nformat ascii 1.0\nelement vertex {count}\nproperty double x\nproperty double y\nproperty double z\nelement face {faces}\nproperty list uchar int vertex_indices\nend_header\n"
    concave = plain_header.format(count=5, faces=1) + "0 0 0\n2 0 0\n2 2 0\n1 1 0\n0 2 0\n5 0 1 2 3 4\n"
    concave_scene = prepare("concave", concave)
    assert len(concave_scene["meshes"][0]["triangles"]) == 3
    area = 0
    for triangle in concave_scene["meshes"][0]["triangles"]:
        a, b, c = (concave_scene["meshes"][0]["vertices"][i]["position"] for i in triangle["indices"])
        area += ((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])) / 2
    assert math.isclose(area, 3)
    prepare("nonplanar", concave.replace("2 2 0", "2 2 1"), "INVALID_PLY_FACE")
    prepare("crossing", concave.replace("5 0 1 2 3 4", "5 0 2 1 3 4"), "INVALID_PLY_FACE")
    prepare("repeated-corner", concave.replace("5 0 1 2 3 4", "5 0 1 1 3 4"), "INVALID_PLY_FACE")

    vertex_colors = (plain_header.format(count=4, faces=2).replace("element face 2", "property uchar red\nproperty uchar green\nproperty uchar blue\nproperty uchar alpha\nelement face 2")
        + "0 0 0 200 80 20 128\n2 0 0 200 80 20 128\n2 3 0 200 80 20 128\n0 3 0 200 80 20 128\n3 0 1 2\n3 0 2 3\n")
    uniform = prepare("uniform-vertex-colors", vertex_colors)
    assert uniform["materials"][0]["color"] == baseline["materials"][0]["color"]
    prepare("vertex-gradient", vertex_colors.replace("2 0 0 200", "2 0 0 199"), "UNSUPPORTED_PLY_VERTEX_COLOR")
    prepare("static-vertex-gradient", vertex_colors.replace("2 0 0 200", "2 0 0 199"), "UNSUPPORTED_PLY_VERTEX_COLOR", ("--profile", "gis-static"))
    prepare("vertex-alpha-gradient", vertex_colors.replace("2 0 0 200 80 20 128", "2 0 0 200 80 20 127"), "UNSUPPORTED_PLY_VERTEX_COLOR")
    both_colors = vertex_colors.replace("property list uchar int vertex_indices\n", "property list uchar int vertex_indices\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nproperty uchar alpha\n")
    both_colors = both_colors.replace("3 0 1 2\n", "3 0 1 2 200 80 20 128\n").replace("3 0 2 3\n", "3 0 2 3 200 80 20 128\n")
    prepare("consistent-color-bindings", both_colors)
    prepare("conflicting-color-bindings", both_colors.replace("3 0 2 3 200", "3 0 2 3 201"), "UNSUPPORTED_PLY_COLOR")

    for token in ("0", "nan", "inf"):
        broken = base.replace("0 0 0 0 0 1\n", "0 0 0 0 0 " + token + "\n")
        prepare("strict-normal-" + token, broken, "INVALID_NORMAL")
        repaired = prepare("repair-normal-" + token, broken, options=("--profile", "gis-static"))
        repairs = [d for d in repaired["diagnostics"] if d["code"] == "NORMALS_REPAIRED"]
        assert len(repairs) == 1 and "2 invalid" in repairs[0]["message"] and "2 triangles" in repairs[0]["message"]
        assert all(v["normal"] == [0, 0, 1] for v in repaired["meshes"][0]["vertices"])
    unused = base.replace("element vertex 4", "element vertex 5").replace("3 0 1 2 200", "9 9 9 0 0 0\n3 0 1 2 200")
    for profile in ("strict", "gis-static"):
        prepare("unused-invalid-normal-" + profile, unused, "INVALID_NORMAL", ("--profile", profile))
        prepare("unused-nonfinite-position-" + profile, unused.replace("9 9 9 0 0 0", "nan 9 9 0 0 1"), "NONFINITE_VERTEX", ("--profile", profile))
    valid_unused = prepare("unused-valid", unused.replace("9 9 9 0 0 0", "9 9 9 0 0 1"))
    assert any(d["code"] == "PLY_UNUSED_VERTICES" for d in valid_unused["diagnostics"])
    degenerate = base.replace("element face 2", "element face 3") + "3 0 0 1 200 80 20 128\n"
    prepare("strict-degenerate", degenerate, "DEGENERATE_TRIANGLE")
    dropped = prepare("static-degenerate", degenerate, options=("--profile", "gis-static"))
    assert len(dropped["meshes"][0]["triangles"]) == 2
    assert any(d["code"] == "DEGENERATE_TRIANGLES_REMOVED" for d in dropped["diagnostics"])
    discarded_normal = degenerate.replace("0 0 0 0 0 1\n", "0 0 0 0 0 0\n")
    dropped = prepare("static-degenerate-normal", discarded_normal, options=("--profile", "gis-static"))
    assert {d["code"] for d in dropped["diagnostics"]} >= {"NORMALS_REPAIRED", "DEGENERATE_NORMALS_DISCARDED"}
    tiny = plain_header.format(count=3, faces=1) + "0 0 0\n1e-100 0 0\n0 1e-100 0\n3 0 1 2\n"
    tiny_report = prepare("nonzero-area-underflow", tiny, "DEGENERATE_TRIANGLE", ("--profile", "gis-static"))
    assert not any(d["code"] == "DEGENERATE_TRIANGLES_REMOVED" for d in tiny_report["diagnostics"])
    for profile in ("strict", "gis-static"):
        prepare("unit-underflow-" + profile, tiny, "UNREPRESENTABLE_COORDINATE", ("--profile", profile, "--mesh-unit-meters", "1e-300"))
    for endian in ("<", ">"):
        payload = bytearray(binary(endian))
        data_start = payload.index(b"end_header\n") + len(b"end_header\n")
        payload[data_start:data_start+4] = struct.pack(endian + "f", float("nan"))
        prepare("binary-nan-position-" + ("le" if endian == "<" else "be"), bytes(payload), "NONFINITE_VERTEX")
        payload = bytearray(binary(endian))
        payload[data_start+20:data_start+24] = struct.pack(endian + "f", float("nan"))
        prepare("binary-nan-normal-" + ("le" if endian == "<" else "be"), bytes(payload), "INVALID_NORMAL")
        repaired = prepare("binary-normal-repair-" + ("le" if endian == "<" else "be"), bytes(payload), options=("--profile", "gis-static"))
        assert any(d["code"] == "NORMALS_REPAIRED" for d in repaired["diagnostics"])

    for name, contents, code in [
        ("trailing-ascii", base + "unexpected\n", "INVALID_PLY"),
        ("extra-field", base.replace("0 0 0 0 0 1\n", "0 0 0 0 0 1 42\n"), "INVALID_PLY"),
        ("fractional-index", base.replace("3 0 1 2 200", "3 0 1.5 2 200"), "INVALID_PLY"),
        ("out-of-range-index", base.replace("3 0 1 2 200", "3 0 8 2 200"), "INVALID_PLY"),
        ("negative-list", base.replace("property list uchar", "property list char").replace("3 0 1 2 200", "-3 0 1 2 200"), "INVALID_PLY_FACE"),
        ("oversized-color", base.replace("200 80 20 128", "256 80 20 128"), "INVALID_PLY"),
        ("fractional-color", base.replace("200 80 20 128", "200.5 80 20 128"), "INVALID_PLY"),
        ("floating-color", base.replace("property uchar red", "property float red"), "UNSUPPORTED_PLY_COLOR"),
        ("partial-color", base.replace("property uchar blue\n", ""), "INVALID_PLY"),
        ("partial-normal", base.replace("property float nz\n", ""), "INVALID_PLY"),
        ("two-normal-bindings", base.replace("property list uchar int vertex_indices\n", "property list uchar int vertex_indices\nproperty float nx\nproperty float ny\nproperty float nz\n"), "UNSUPPORTED_PLY_NORMAL_BINDING"),
        ("duplicate-property", base.replace("property float x\n", "property float x\nproperty float x\n"), "INVALID_PLY"),
        ("duplicate-element", base.replace("end_header", "element vertex 4\nend_header"), "INVALID_PLY"),
        ("unknown-element", base.replace("end_header", "element edge 0\nend_header"), "UNSUPPORTED_PLY_ELEMENT"),
        ("unknown-property", base.replace("property float nz", "property float reflectance"), "UNSUPPORTED_PLY_PROPERTY"),
        ("texcoord-property", base.replace("property float nz", "property float texture_u"), "UNSUPPORTED_PLY_PROPERTY"),
        ("texture-comment", base.replace("comment GeoModelBridge", "comment TextureFile checker.png\ncomment GeoModelBridge"), "UNSUPPORTED_PLY_METADATA"),
        ("texture-obj-info", base.replace("comment GeoModelBridge", "obj_info texture_file checker.png\ncomment GeoModelBridge"), "UNSUPPORTED_PLY_METADATA"),
        ("wrong-version", base.replace("ascii 1.0", "ascii 1.1"), "INVALID_PLY"),
        ("floating-indices", base.replace("list uchar int", "list uchar float"), "INVALID_PLY"),
        ("oversized-count", base.replace("element vertex 4", "element vertex 10000001"), "INVALID_PLY"),
        ("count-overflow", base.replace("element vertex 4", "element vertex 9999999999999999999999"), "INVALID_PLY"),
        ("truncated-ascii", base.rsplit("\n", 2)[0] + "\n", "INVALID_PLY"),
        ("truncated-binary", binary("<")[:-1], "INVALID_PLY"),
        ("trailing-binary", binary(">") + b"\0", "INVALID_PLY"),
        ("ascii-nul", base.replace("2 0 0", "2\0 0 0", 1), "INVALID_PLY"),
    ]:
        prepare(name, contents, code)
    over_face = plain_header.format(count=3, faces=1).replace("list uchar", "list uint") + "0 0 0\n1 0 0\n0 1 0\n1025\n"
    prepare("face-complexity-cap", over_face, "INVALID_PLY_FACE")
    prepare("obj-info", base.replace("comment GeoModelBridge", "obj_info author Example\ncomment GeoModelBridge"))
    changed_order = base.replace("property float x\nproperty float y\nproperty float z", "property float z\nproperty float y\nproperty float x")
    reordered = prepare("reordered-properties", changed_order)
    assert {tuple(v["position"]) for v in reordered["meshes"][0]["vertices"]} == {(0,0,0),(0,0,2),(0,3,2),(0,3,0)}
    # Explicit valid normals remain source-authored, even when not perpendicular to a face.
    assert all(v["normal"] == [0, 0, 1] for v in reordered["meshes"][0]["vertices"])
    preserved = (root / "ascii-local-bundle/scene.json").read_bytes()
    assert run("prepare", root / "ascii-local.PLY", "--output", root / "ascii-local-bundle").returncode != 0
    assert (root / "ascii-local-bundle/scene.json").read_bytes() == preserved
    cases.append("existing-output-preserved")

    if writer is not None:
        output, report = root / "native-colors.gdb", root / "native-colors.json"
        process = run("convert", fixtures / "colored_quad.ply", "--output", output, "--report", report,
                      "--writer", writer, "--wkid", "3857", "--origin", "100", "100", "100")
        assert process.returncode == 0, (process.returncode, process.stdout, process.stderr)
        result = json.loads(report.read_text(encoding="utf-8"))
        assert result["status"] == "written_and_readback_verified"
        assert result["verification"]["feature_count"] == 1
        assert result["coordinates"]["origin"] == [100, 100, 100]
        copied, copied_report = root / "independent-copy.gdb", root / "independent-copy.json"
        shutil.copytree(output, copied)
        subprocess.run([str(writer), "--verify-gdb", str(copied), "--expected-report", str(report), "--report", str(copied_report)],
                       check=True, stdout=subprocess.DEVNULL)
        assert json.loads(copied_report.read_text(encoding="utf-8"))["status"] == "standalone_copy_verified"
        cases.append("native-color-alpha-copy-readback")

print(f"PASS PLY: {len(cases)} cases; ASCII/both binary orders, colors/alpha, normals, concave topology, strict limits and rejection")
