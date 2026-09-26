"""OBJ/MTL input contract through the same CLI and Scene Bundle as FBX."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
fixtures = Path(sys.argv[2]).resolve()


def run(*args, success=True):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True,
                            text=True, encoding="utf-8", errors="replace")
    assert (result.returncode == 0) == success, (result.returncode, result.stdout, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix="gmb-obj-") as directory:
    root = Path(directory)
    source = root / "textured_quad.obj"
    material = root / "textured_quad.mtl"
    image = root / "checker.png"
    for name in (source.name, material.name, image.name):
        shutil.copyfile(fixtures / name, root / name)
    bundle = root / "prepared"
    run("prepare", source, "--output", bundle, "--wkid", 32650,
        "--origin", 500000, 3000000, 100)
    scene = json.loads((bundle / "scene.json").read_text(encoding="utf-8"))
    assert scene["source"] == str(source)
    assert scene["coordinates"]["origin"] == [500000, 3000000, 100]
    assert len(scene["meshes"]) == 1 and len(scene["meshes"][0]["triangles"]) == 2
    assert scene["materials"][0]["color"] == [0.8, 0.6, 0.4, 0.75]
    assert scene["materials"][0]["texture"] == 0 and len(scene["textures"]) == 1
    vertices = scene["meshes"][0]["vertices"]
    assert {tuple(v["uv"]) for v in vertices} == {(0, 0), (1, 0), (1, 1), (0, 1)}
    assert all(v["normal"] is not None and v["uv"] is not None for v in vertices)
    assert {tuple(v["position"]) for v in vertices} == {
        (500000, 3000000, 100), (500002, 3000000, 100),
        (500002, 3000002, 100), (500000, 3000002, 100)}
    assert any(d["code"] == "OBJ_COORDINATE_ASSUMPTION" for d in scene["diagnostics"])
    run("prepare", source, "--output", bundle, success=False)
    assert json.loads((bundle / "scene.json").read_text(encoding="utf-8")) == scene

    y_bundle = root / "y-up"
    run("prepare", source, "--output", y_bundle, "--obj-up-axis", "Y",
        "--obj-unit-meters", "0.5")
    y_scene = json.loads((y_bundle / "scene.json").read_text(encoding="utf-8"))
    assert (0, 0, 1) in {tuple(v["position"]) for v in y_scene["meshes"][0]["vertices"]}

    material.write_text(material.read_text(encoding="utf-8").replace("d 0.75", "Tr 0.25"), encoding="utf-8")
    tr_bundle = root / "tr-opacity"
    run("prepare", source, "--output", tr_bundle)
    assert json.loads((tr_bundle / "scene.json").read_text(encoding="utf-8"))["materials"][0]["color"][3] == 0.75

    image.unlink()
    report = root / "fallback.json"
    run("inspect", source, "--report", report)
    fallback = json.loads(report.read_text(encoding="utf-8"))
    assert any(d["code"] == "MISSING_TEXTURE_FALLBACK" for d in fallback["diagnostics"])
    assert run("inspect", source, "--missing-textures", "error", success=False).returncode == 3
    shutil.copyfile(fixtures / image.name, image)

    material.write_text("newmtl Checker\nKd 1 1 1\nmap_d checker.png\n", encoding="utf-8")
    rejected = root / "unsupported.json"
    assert run("inspect", source, "--report", rejected, success=False).returncode == 3
    assert any(d["code"] == "UNSUPPORTED_MTL_DIRECTIVE" for d in
               json.loads(rejected.read_text(encoding="utf-8"))["diagnostics"])
    material.write_text("newmtl Checker\nKd 1 1 1\nKs 0.3 0.3 0.3\n", encoding="utf-8")
    assert run("inspect", source, success=False).returncode == 3
    static = root / "static.json"
    run("inspect", source, "--profile", "gis-static", "--report", static)
    assert any(d["code"] == "MATERIAL_CHANNEL_OMITTED" for d in
               json.loads(static.read_text(encoding="utf-8"))["diagnostics"])
    material.write_text("newmtl Another\nKd 1 1 1\n", encoding="utf-8")
    missing_material = root / "missing-material.json"
    assert run("inspect", source, "--report", missing_material, success=False).returncode == 3
    assert any(d["code"] == "MISSING_MTL_MATERIAL" for d in
               json.loads(missing_material.read_text(encoding="utf-8"))["diagnostics"])
    source.write_text(source.read_text(encoding="utf-8").replace("textured_quad.mtl", "../escape.mtl"), encoding="utf-8")
    assert run("inspect", source, success=False).returncode != 0

print("PASS OBJ geometry, material, texture, placement, policies and path safety")
