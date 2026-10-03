"""GLB static geometry, material, image, transform and rejection contract."""
import copy
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from glb_fixture import make_glb

exe = Path(sys.argv[1]).resolve()
fixture = Path(sys.argv[2]).resolve() / "textured_quad.glb"
payload = fixture.read_bytes()
json_size = struct.unpack_from("<I", payload, 12)[0]
base = json.loads(payload[20:20+json_size])
binary = payload[28+json_size:]


def run(*args, success=True):
    p = subprocess.run([str(exe), *map(str, args)], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    assert (p.returncode == 0) == success, (p.returncode, p.stdout, p.stderr)
    return p


def variation(root, name, mutate, content=None):
    doc = copy.deepcopy(base)
    mutate(doc)
    path = root / (name + ".glb")
    path.write_bytes(make_glb(doc, binary if content is None else content))
    return path


def rejected(source, code, root, *args):
    report = root / (source.stem + "-" + code + ".json")
    assert run("inspect", source, "--report", report, *args, success=False).returncode == 3
    assert code in [d["code"] for d in json.loads(report.read_text(encoding="utf-8"))["diagnostics"]]


with tempfile.TemporaryDirectory(prefix="gmb-glb-") as directory:
    root = Path(directory)
    source = root / "模型 & textured.glb"
    source.write_bytes(payload)
    bundle = root / "bundle"
    run("prepare", source, "--output", bundle, "--wkid", 32650,
        "--origin", 500000, 3000000, 100)
    scene = json.loads((bundle / "scene.json").read_text(encoding="utf-8"))
    assert scene["source"] == str(source)
    assert scene["coordinates"]["origin"] == [500000, 3000000, 100]
    assert scene["coordinates"]["wkid"] == 32650
    assert len(scene["meshes"]) == 1 and len(scene["meshes"][0]["triangles"]) == 2
    assert len(scene["nodes"]) == 1 and scene["nodes"][0]["source_world_transform"][12:15] == [10, 1, 2]
    vertices = scene["meshes"][0]["vertices"]
    assert {tuple(v["position"]) for v in vertices} == {
        (500010, 2999998, 101), (500012, 2999998, 101),
        (500012, 3000001, 101), (500010, 3000001, 101)}
    assert {tuple(v["uv"]) for v in vertices} == {
        (0.25, 0.875), (0.75, 0.875), (0.75, 0.375), (0.25, 0.375)}
    rotated = variation(root,"rotated-uv",lambda d: d["materials"][0]["pbrMetallicRoughness"]
                        ["baseColorTexture"]["extensions"]["KHR_texture_transform"].update(rotation=1.5707963267948966))
    rb = root / "rotated-bundle"
    run("prepare",rotated,"--output",rb)
    rotated_uvs = {tuple(round(x,5) for x in v["uv"]) for v in
                   json.loads((rb / "scene.json").read_text())["meshes"][0]["vertices"]}
    assert rotated_uvs == {(0.25,0.875),(0.25,0.375),(-0.25,0.375),(-0.25,0.875)}
    assert all(v["normal"] == [0, 0, 1] for v in vertices)
    sparse_bytes = binary + b"\0\0\0\0" + struct.pack("<3f",1,0,0)
    def sparse_position(d):
        d["buffers"][0]["byteLength"] = len(sparse_bytes)
        d["bufferViews"].extend([{"buffer":0,"byteOffset":len(binary),"byteLength":1},
                                 {"buffer":0,"byteOffset":len(binary)+4,"byteLength":12}])
        d["accessors"][0]["sparse"] = {"count":1,"indices":{"bufferView":5,"componentType":5121},
                                         "values":{"bufferView":6}}
    sparse = variation(root,"sparse",sparse_position,sparse_bytes)
    sb = root / "sparse-bundle"
    run("prepare",sparse,"--output",sb)
    sparse_scene = json.loads((sb / "scene.json").read_text())
    assert (11,-2,1) in {tuple(v["position"]) for v in sparse_scene["meshes"][0]["vertices"]}
    assert all(abs(x-y)<1e-6 for x,y in zip(scene["materials"][0]["color"], [0.8,0.6,0.4,0.75]))
    texture = scene["textures"][0]
    assert (bundle / texture["path"]).read_bytes() == (Path(sys.argv[2]) / "checker.png").read_bytes()
    assert any(d["code"] == "GLTF_COORDINATE_CONVENTION" for d in scene["diagnostics"])
    assert any(d["code"] == "UNLIT_SHADING_MAPPED" for d in scene["diagnostics"])
    prepared = json.loads((bundle / "report.json").read_text(encoding="utf-8"))
    assert prepared["geometry_bounds"] == {"min": [500010, 2999998, 101], "max": [500012, 3000001, 101]}
    assert prepared["fidelity"]["compatibility_adjustments"] is True
    assert prepared["fidelity"]["strict_validation_passed"] is False
    inspect_report = root / "inspection.json"
    run("inspect", source, "--report", inspect_report)
    inspected = json.loads(inspect_report.read_text(encoding="utf-8"))
    assert inspected["status"] == "inspected" and inspected["backend"] == "none"
    assert inspected["geometry_bounds"] == {"min": [10, -2, 1], "max": [12, 1, 1]}
    assert inspected["fidelity"]["validation_passed"] is True
    assert inspected["fidelity"]["compatibility_adjustments"] is True
    assert inspected["fidelity"]["strict_validation_passed"] is False
    assert inspected["fidelity"]["gdb_written"] is False and inspected["fidelity"]["gdb_readback_verified"] is False
    sole_scene = variation(root,"sole-scene",lambda d: d.pop("scene"))
    sole_report = root / "sole-scene.json"
    run("inspect",sole_scene,"--report",sole_report)
    assert any(d["code"] == "GLTF_SCENE_ASSUMPTION" for d in json.loads(sole_report.read_text())["diagnostics"])
    run("prepare", source, "--output", bundle, success=False)
    assert json.loads((bundle / "scene.json").read_text(encoding="utf-8")) == scene

    mirror = variation(root,"mirror",lambda d: (
        d["nodes"].append({"name":"Mirror","mesh":0,"translation":[20,1,2],"scale":[-1,1,1]}),
        d["scenes"][0]["nodes"].append(1)))
    mb = root / "mirror-bundle"
    run("prepare",mirror,"--output",mb)
    ms = json.loads((mb / "scene.json").read_text(encoding="utf-8"))
    assert len(ms["meshes"]) == 2 and ms["nodes"][1]["meshes"] == [1]
    assert {tuple(v["position"]) for v in ms["meshes"][1]["vertices"]} == {
        (20,-2,1),(18,-2,1),(18,1,1),(20,1,1)}
    assert all(v["normal"] == [0,0,1] for v in ms["meshes"][1]["vertices"])

    pbr = variation(root,"pbr",lambda d: (
        d["materials"][0].pop("extensions"), d.update(extensionsUsed=["KHR_texture_transform"],extensionsRequired=[])))
    rejected(pbr,"UNSUPPORTED_GLTF_MATERIAL",root)
    report = root / "pbr.json"
    run("inspect",pbr,"--profile","gis-static","--report",report)
    assert any(d["code"] == "MATERIAL_CHANNEL_OMITTED" for d in json.loads(report.read_text())["diagnostics"])
    masked = variation(root,"mask",lambda d: d["materials"][0].update(alphaMode="MASK",alphaCutoff=0.4))
    rejected(masked,"UNSUPPORTED_GLTF_ALPHA",root)
    opaque_png = variation(root,"opaque-png",lambda d: d["materials"][0].update(alphaMode="OPAQUE"))
    rejected(opaque_png,"UNSUPPORTED_GLTF_ALPHA",root)
    animated = variation(root,"animated",lambda d: d.update(animations=[{"samplers":[],"channels":[]}]))
    empty_report = root / "empty-animation.json"
    run("inspect",animated,"--report",empty_report)
    assert any(d["code"] == "EMPTY_ANIMATION_IGNORED" for d in json.loads(empty_report.read_text())["diagnostics"])
    animation_bytes = binary + struct.pack("<2f6f",0,1,10,1,2,20,1,2)
    def add_animation(d):
        d["buffers"][0]["byteLength"] = len(animation_bytes)
        d["bufferViews"].extend([{"buffer":0,"byteOffset":len(binary),"byteLength":8},
                                 {"buffer":0,"byteOffset":len(binary)+8,"byteLength":24}])
        d["accessors"].extend([{"bufferView":5,"componentType":5126,"count":2,"type":"SCALAR"},
                               {"bufferView":6,"componentType":5126,"count":2,"type":"VEC3"}])
        d["animations"] = [{"samplers":[{"input":4,"output":5,"interpolation":"LINEAR"}],
                            "channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}]
    active_animation = variation(root,"active-animation",add_animation,animation_bytes)
    rejected(active_animation,"UNSUPPORTED_ANIMATION",root)
    static_report = root / "static-animation.json"
    run("inspect",active_animation,"--profile","gis-static","--report",static_report)
    assert any(d["code"] == "STATIC_POSE_USED" for d in json.loads(static_report.read_text())["diagnostics"])
    vertex_colors = variation(root,"colors",lambda d: d["meshes"][0]["primitives"][0]["attributes"].update(COLOR_0=0))
    rejected(vertex_colors,"UNSUPPORTED_GLTF_ATTRIBUTE",root)
    unknown = variation(root,"unknown",lambda d: d.update(extensionsUsed=["EXT_unknown"]))
    rejected(unknown,"UNSUPPORTED_GLTF_EXTENSION",root)

    def external(d):
        d["images"] = [{"uri":"absent.png","name":"Absent"}]
    missing = variation(root,"missing",external)
    report = root / "missing.json"
    run("inspect",missing,"--report",report)
    assert any(d["code"] == "MISSING_TEXTURE_FALLBACK" for d in json.loads(report.read_text())["diagnostics"])
    rejected(missing,"MISSING_TEXTURE",root,"--missing-textures","error")
    (root / "checker image.png").write_bytes((Path(sys.argv[2]) / "checker.png").read_bytes())
    external = variation(root,"external-image",lambda d: d["images"].__setitem__(0,{"uri":"checker%20image.png"}))
    eb = root / "external-bundle"
    run("prepare",external,"--output",eb)
    es = json.loads((eb / "scene.json").read_text(encoding="utf-8"))
    assert len(es["textures"]) == 1 and es["textures"][0]["embedded"] is False
    (root / "checker image.png").unlink()
    extra_dir = root / "extra-textures"
    extra_dir.mkdir()
    (extra_dir / "checker image.png").write_bytes((Path(sys.argv[2]) / "checker.png").read_bytes())
    run("inspect",external,"--texture-dir",extra_dir,"--missing-textures","error")
    (root / "bad-parent").write_text("file",encoding="utf-8")
    bad_parent = variation(root,"bad-parent-image",lambda d: d["images"].__setitem__(0,{"uri":"bad-parent/image.png"}))
    rejected(bad_parent,"TEXTURE_READ_ERROR",root)
    escape = variation(root,"escape",lambda d: d["images"].__setitem__(0,{"uri":"../elsewhere.png"}))
    rejected(escape,"UNSAFE_GLTF_URI",root)
    bad_sampler = variation(root,"sampler",lambda d: (
        d.update(samplers=[{"wrapS":33071}]),d["textures"][0].update(sampler=0)))
    rejected(bad_sampler,"UNSUPPORTED_GLTF_SAMPLER",root)
    wrong_mime = variation(root,"mime",lambda d: d["images"][0].update(mimeType="image/jpeg"))
    rejected(wrong_mime,"UNSUPPORTED_TEXTURE_FORMAT",root)
    bad_view = variation(root,"short-view",lambda d: d["bufferViews"][0].update(byteLength=10))
    rejected(bad_view,"INVALID_GLTF_ACCESSOR",root)
    truncated = root / "truncated.glb"
    truncated.write_bytes(payload[:32])
    rejected(truncated,"INVALID_GLB",root)
    bad_normal_binary = bytearray(binary)
    struct.pack_into("<3f",bad_normal_binary,48,0,0,0)
    bad_normal = variation(root,"bad-normal",lambda d: None,bad_normal_binary)
    rejected(bad_normal,"INVALID_NORMAL",root)
    fixed = root / "fixed.json"
    run("inspect",bad_normal,"--profile","gis-static","--report",fixed)
    assert any(d["code"] == "NORMALS_REPAIRED" for d in json.loads(fixed.read_text())["diagnostics"])

print("PASS GLB static texture/UV/placement, mirrored instances, material profiles, fallback and semantic rejection")
