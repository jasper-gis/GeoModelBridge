"""Synthetic FBX regressions for explicit static-GIS compatibility policy.

All input data is derived from the project's own small quad, never from a user's
model. The executable parses the generated ASCII FBX, not a mocked scene.
"""

import json
import hashlib
import math
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import tempfile

from cli_test import invoke


def insert_objects(source, objects, connections=""):
    source = source.replace("\nConnections: {", "\nConnections: {\n" + connections, 1)
    return source.replace("\n}\nConnections:", "\n" + objects + "\n}\nConnections:", 1)


def properties(source, additions):
    marker = '   P: "DiffuseColor",'
    assert source.count(marker) == 1
    return source.replace(marker, additions + "\n" + marker, 1)


def material_value(source, name, value):
    lines = source.splitlines()
    marker = f'P: "{name}",'
    matches = [i for i, line in enumerate(lines) if marker in line]
    assert len(matches) == 1, name
    index = matches[0]
    fields = lines[index].split(",", 4)
    lines[index] = ",".join(fields[:4]) + ", " + value
    return "\n".join(lines) + "\n"


def geometry(source, positions, triangles):
    indices = []
    for a, b, c in triangles:
        indices.extend((a, b, -c - 1))
    vertices = ",".join(str(value) for point in positions for value in point)
    normals = ",".join("0,0,1" for _ in indices)
    body = f''' Geometry: 100, "Geometry::Regression", "Mesh" {{
  Vertices: *{len(positions) * 3} {{ a: {vertices} }}
  PolygonVertexIndex: *{len(indices)} {{ a: {','.join(map(str, indices))} }}
  LayerElementNormal: 0 {{
   Version: 101
   Name: ""
   MappingInformationType: "ByPolygonVertex"
   ReferenceInformationType: "Direct"
   Normals: *{len(indices) * 3} {{ a: {normals} }}
  }}
  LayerElementMaterial: 0 {{
   Version: 101
   Name: ""
   MappingInformationType: "AllSame"
   ReferenceInformationType: "IndexToDirect"
   Materials: *1 {{ a: 0 }}
  }}
  Layer: 0 {{
   Version: 100
   LayerElement: {{ Type: "LayerElementNormal"
    TypedIndex: 0 }}
   LayerElement: {{ Type: "LayerElementMaterial"
    TypedIndex: 0 }}
  }}
 }}'''
    start = source.index(" Geometry: 100,")
    end = source.index("\n Model:", start)
    return source[:start] + body + source[end:]


EMPTY_STACK = ''' AnimationStack: 600, "AnimStack::Empty", "" {
  Properties70: {
   P: "LocalStart", "KTime", "Time", "",0
   P: "LocalStop", "KTime", "Time", "",46186158000
  }
 }
 AnimationLayer: 601, "AnimLayer::Empty", "" { }
'''
ANIMATION = EMPTY_STACK + ''' AnimationCurveNode: 602, "AnimCurveNode::T", "" {
  Properties70: {
   P: "d|X", "Number", "", "A", 100
   P: "d|Y", "Number", "", "A", 0
   P: "d|Z", "Number", "", "A", 0
  }
 }
 AnimationCurve: 603, "AnimCurve::X", "" {
  Default: 100
  KeyVer: 4008
  KeyTime: *2 { a: 0,46186158000 }
  KeyValueFloat: *2 { a: 100,200 }
  KeyAttrFlags: *1 { a: 24836 }
  KeyAttrDataFloat: *4 { a: 0,0,0,0 }
  KeyAttrRefCount: *1 { a: 2 }
 }
'''
ANIMATION_CONNECTIONS = ''' C: "OO",601,600
 C: "OO",602,601
 C: "OP",602,200,"Lcl Translation"
 C: "OP",603,602,"d|X"
'''


def main():
    exe = Path(sys.argv[1]).resolve()
    fixtures = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).parent / "fixtures"
    base = (fixtures / "colored_quad.fbx").read_text(encoding="utf-8")
    textured = (fixtures / "textured_quad.fbx").read_text(encoding="utf-8")
    cases = []
    with tempfile.TemporaryDirectory(prefix="geomodelbridge-profile-") as scratch:
        root = Path(scratch)
        shutil.copyfile(fixtures / "checker.png", root / "checker.png")

        def run(name, source, profile, accepted, code=None):
            path = root / (name + ".fbx")
            output = root / (name + "-bundle")
            report = root / (name + ".json")
            path.write_text(source, encoding="utf-8")
            args = [] if profile is None else ["--profile", profile]
            result = invoke(exe, "prepare", path, "--output", output, "--report", report,
                            *args, expect_success=accepted)
            data = json.loads(report.read_text(encoding="utf-8"))
            diagnostics = data["diagnostics"]
            effective_profile = profile or "strict"
            assert data["conversion_profile"] == effective_profile, (name, data)
            compatibility_codes = {"STATIC_POSE_USED", "MATERIAL_CHANNEL_OMITTED", "DEGENERATE_TRIANGLES_REMOVED", "JPEG_CONTAINER_NORMALIZED"}
            adjusted = any(d["code"] in compatibility_codes and d["severity"] == "warning" for d in diagnostics)
            assert data["fidelity"]["compatibility_adjustments"] is adjusted
            assert data["fidelity"]["validation_passed"] is accepted
            if accepted:
                assert data["status"] == "prepared", (name, data)
                assert not any(d["severity"] == "error" for d in diagnostics), (name, diagnostics)
                scene = json.loads((output / "scene.json").read_text(encoding="utf-8"))
                assert scene["conversion_profile"] == effective_profile
                assert data["fidelity"]["strict_validation_passed"] is (effective_profile == "strict")
                assert scene["meshes"] and all(mesh["triangles"] for mesh in scene["meshes"])
                for mesh in scene["meshes"]:
                    for vertex in mesh["vertices"]:
                        assert all(math.isfinite(v) for v in vertex["position"])
                if code:
                    assert any(d["code"] == code and d["severity"] == "warning" for d in diagnostics), (name, diagnostics)
            else:
                assert result.returncode != 0 and data["status"] in {"failed", "rejected"}, (name, data)
                assert not output.exists(), name + " left a partial bundle"
                assert data["fidelity"]["strict_validation_passed"] is False
                if code:
                    assert any(d["code"] == code and d["severity"] == "error" for d in diagnostics), (name, diagnostics)
                scene = None
            cases.append(name)
            print("PASS", name)
            return scene, data

        neutral = properties(base, '''   P: "Reflectivity", "Number", "", "A", 0
   P: "DisplacementColor", "Color", "", "A", 0,0,0
   P: "VectorDisplacementColor", "Color", "", "A", 0,0,0''')
        for profile in ("strict", "gis-static"):
            run("neutral-exporter-defaults-" + profile, neutral, profile, True)
            dormant = properties(base, '''   P: "DisplacementColor", "Color", "", "A", 1,2,3
   P: "DisplacementFactor", "Number", "", "A", 0
   P: "VectorDisplacementColor", "Color", "", "A", 3,2,1
   P: "VectorDisplacementFactor", "Number", "", "A", 0''')
            run("zero-factor-displacement-is-inactive-" + profile, dormant, profile, True)
            run("empty-animation-stack-" + profile,
                insert_objects(base, EMPTY_STACK, ' C: "OO",601,600\n'), profile, True,
                "EMPTY_ANIMATION_IGNORED")

        saved_pose = material_value(base, "Lcl Translation", "7,8,9")
        animated = insert_objects(saved_pose, ANIMATION, ANIMATION_CONNECTIONS)
        run("active-animation-strict", animated, "strict", False, "UNSUPPORTED_ANIMATION")
        scene, _ = run("active-animation-gis-static", animated, "gis-static", True, "STATIC_POSE_USED")
        positions = [v["position"] for v in scene["meshes"][0]["vertices"]]
        assert [min(p[i] for p in positions) for i in range(3)] == [7, 8, 9], "Evaluated a keyframe instead of using the saved pose"

        for channel in ("Ambient", "Specular", "Reflection"):
            if channel == "Ambient":
                active = material_value(material_value(base, "AmbientColor", "0.2,0.3,0.4"), "AmbientFactor", "0.5")
            else:
                active = properties(base, f'   P: "{channel}Color", "Color", "", "A", 0.2,0.3,0.4\n'
                                         f'   P: "{channel}Factor", "Number", "", "A", 0.5')
            run(channel.lower() + "-strict", active, "strict", False, "UNSUPPORTED_MATERIAL_CHANNEL")
            scene, _ = run(channel.lower() + "-gis-static", active, "gis-static", True, "MATERIAL_CHANNEL_OMITTED")
            assert scene["materials"][0]["color"] == [0.4, 0.1, 0.05, 0.75], "Lighting omission changed diffuse color or alpha"
            texture_source = textured.replace(' C: "OP",400,300,"DiffuseColor"',
                                              f' C: "OP",400,300,"DiffuseColor"\n C: "OP",400,300,"{channel}Color"')
            run(channel.lower() + "-texture-strict", texture_source, "strict", False)
            scene, _ = run(channel.lower() + "-texture-gis-static", texture_source, "gis-static", True, "MATERIAL_CHANNEL_OMITTED")
            assert len(scene["textures"]) == 1, "Lost the shared base-color image when omitting lighting texture"

        # Default CLI behavior must retain strict fidelity.
        run("omitted-profile-is-strict", active, None, False, "UNSUPPORTED_MATERIAL_CHANNEL")
        reflectivity = properties(base, '   P: "Reflectivity", "Number", "", "A", 0.5')
        run("active-reflectivity-strict", reflectivity, "strict", False, "UNSUPPORTED_MATERIAL_CHANNEL")
        run("active-reflectivity-gis-static", reflectivity, "gis-static", True, "MATERIAL_CHANNEL_OMITTED")
        hazardous = {
            "emission": (fixtures / "unsupported_emission.fbx").read_text(encoding="utf-8"),
            "active-displacement": properties(base, '   P: "DisplacementColor", "Color", "", "A", 1,0,0'),
            "active-vector-displacement": properties(base, '   P: "VectorDisplacementColor", "Color", "", "A", 0,1,0'),
            "unknown-roughness": properties(base, '   P: "CustomRoughness", "Number", "", "A", 0.5'),
            "pbr-shader": base.replace('ShadingModel: "lambert"', 'ShadingModel: "physical"'),
            "conflicting-opacity": properties(base, '   P: "Opacity", "Number", "", "A", 0.1'),
            "colored-transparency": material_value(base, "TransparentColor", "1,0,0"),
            "normal-map": textured.replace(' C: "OP",400,300,"DiffuseColor"', ' C: "OP",400,300,"NormalMap"'),
            "opacity-map": textured.replace(' C: "OP",400,300,"DiffuseColor"', ' C: "OP",400,300,"TransparentColor"'),
        }
        for name, source in hazardous.items():
            for profile in ("strict", "gis-static"):
                run(name + "-" + profile, source, profile, False)

        # Generated project-owned 3x2 JPEG, with deliberately varied metadata.
        # Preserve the complete original compressed stream when normalizing.
        jpeg = (fixtures / "project_rgb.jpg").read_bytes()
        assert jpeg[:6] == b"\xff\xd8\xff\xe0\x00\x10" and jpeg[6:11] == b"JFIF\0"
        jfif_segment = jpeg[2:20]
        without_jfif = jpeg[:2] + jpeg[20:]

        def marker(code, payload):
            return bytes((255, code)) + struct.pack(">H", len(payload) + 2) + payload

        def adobe(transform):
            return marker(0xEE, b"Adobe" + struct.pack(">HHHB", 100, 0, 0, transform))

        def exif_tags(fields, endian="<"):
            # Construct original TIFF IFD0 tags, including out-of-line rationals.
            header = (b"II" if endian == "<" else b"MM") + struct.pack(endian + "HIH", 42, 8, len(fields))
            entries, values = bytearray(), bytearray()
            value_offset = 8 + 2 + 12 * len(fields) + 4
            for tag, kind, value in fields:
                entries.extend(struct.pack(endian + "HHI", tag, kind, 1))
                if kind == 3:
                    entries.extend(struct.pack(endian + "HH", value, 0))
                else:
                    assert kind == 5
                    entries.extend(struct.pack(endian + "I", value_offset + len(values)))
                    values.extend(struct.pack(endian + "II", *value))
            tiff = header + entries + struct.pack(endian + "I", 0) + values
            return marker(0xE1, b"Exif\0\0" + tiff)

        def exif_orientation(value):
            return exif_tags([(0x0112, 3, value)])

        def jpeg_source(name, payload):
            (root / (name + ".jpg")).write_bytes(payload)
            return textured.replace("checker.png", name + ".jpg")

        for name, payload in [("jpeg-jfif-original", jpeg),
                              ("jpeg-adobe-ycbcr", without_jfif[:2] + adobe(1) + without_jfif[2:]),
                              ("jpeg-adobe-orientation-one", without_jfif[:2] + exif_orientation(1) + adobe(1) + without_jfif[2:]),
                              ("jpeg-big-endian-equal-density", without_jfif[:2] + exif_tags([
                                  (0x0112, 3, 1), (0x0213, 3, 1), (0x011a, 5, (72, 1)), (0x011b, 5, (144, 2))], ">") + adobe(1) + without_jfif[2:])]:
            source = jpeg_source(name, payload)
            if name == "jpeg-jfif-original":
                for profile in ("strict", "gis-static"):
                    scene, _ = run(name + "-" + profile, source, profile, True)
                    actual = (root / (name + "-" + profile + "-bundle") / scene["textures"][0]["path"]).read_bytes()
                    assert actual == payload, "An already compatible JPEG changed"
            else:
                run(name + "-strict", source, "strict", False, "JPEG_CONTAINER_REQUIRES_NORMALIZATION")
                scene, report = run(name + "-gis-static", source, "gis-static", True, "JPEG_CONTAINER_NORMALIZED")
                actual = (root / (name + "-gis-static-bundle") / scene["textures"][0]["path"]).read_bytes()
                assert len(actual) == len(payload) + 18 and actual[:2] == payload[:2]
                assert actual[2:20] == jfif_segment, "Unexpected JPEG metadata insertion"
                assert actual[20:] == payload[2:], "Original JPEG markers or compressed bytes were rewritten"
                diagnostic = next(d for d in report["diagnostics"] if d["code"] == "JPEG_CONTAINER_NORMALIZED")
                assert hashlib.sha256(payload).hexdigest() in diagnostic["message"]
                assert hashlib.sha256(actual).hexdigest() in diagnostic["message"]
                assert (root / (name + ".jpg")).read_bytes() == payload, "Source image was modified"

        unusual_ids = bytearray(without_jfif)
        sof = unusual_ids.index(b"\xff\xc0")
        sos = unusual_ids.index(b"\xff\xda")
        for index, value in enumerate(b"RGB"):
            unusual_ids[sof + 10 + 3 * index] = value
            unusual_ids[sos + 5 + 2 * index] = value
        safe_adobe = without_jfif[:2] + adobe(1) + without_jfif[2:]
        scan_marker = safe_adobe.index(b"\xff\xda")
        scan_start = scan_marker + 2 + struct.unpack_from(">H", safe_adobe, scan_marker + 2)[0]
        for name, payload in [
                ("jpeg-adobe-rgb-ambiguous", without_jfif[:2] + adobe(0) + without_jfif[2:]),
                ("jpeg-missing-colorspace-metadata", without_jfif),
                ("jpeg-orientation-six", without_jfif[:2] + exif_orientation(6) + adobe(1) + without_jfif[2:]),
                ("jpeg-nonsquare-density", without_jfif[:2] + exif_tags([(0x011a, 5, (72, 1)), (0x011b, 5, (144, 1))]) + adobe(1) + without_jfif[2:]),
                ("jpeg-incomplete-density", without_jfif[:2] + exif_tags([(0x011a, 5, (72, 1))]) + adobe(1) + without_jfif[2:]),
                ("jpeg-cosited-chroma", without_jfif[:2] + exif_tags([(0x0213, 3, 2)]) + adobe(1) + without_jfif[2:]),
                ("jpeg-unusual-component-ids", unusual_ids[:2] + adobe(1) + unusual_ids[2:]),
                ("jpeg-truncated-scan-header", safe_adobe[:scan_marker + 2]),
                ("jpeg-empty-scan", safe_adobe[:scan_start] + b"\xff\xd9"),
                ("jpeg-missing-end-marker", safe_adobe[:-2])]:
            source = jpeg_source(name, payload)
            for profile in ("strict", "gis-static"):
                run(name + "-" + profile, source, profile, False, "JPEG_CONTAINER_UNSUPPORTED")

        quad = [(0, 0, 0), (2, 0, 0), (2, 3, 0), (0, 3, 0)]
        degenerates = geometry(base, quad, [(0, 1, 2), (0, 0, 1), (0, 2, 3), (1, 1, 1)])
        run("zero-area-strict", degenerates, "strict", False, "DEGENERATE_TRIANGLE")
        scene, report = run("zero-area-gis-static", degenerates, "gis-static", True, "DEGENERATE_TRIANGLES_REMOVED")
        assert len(scene["meshes"][0]["triangles"]) == 2
        removal = [d for d in report["diagnostics"] if d["code"] == "DEGENERATE_TRIANGLES_REMOVED"]
        assert len(removal) == 1, "Repeated triangle warnings were not summarized per mesh"
        assert "2" in removal[0]["message"], removal
        run("all-degenerate-not-empty-success", geometry(base, quad, [(0, 0, 1)]), "gis-static", False)
        tiny = geometry(base, [(0, 0, 0), (1e-12, 0, 0), (0, 1e-12, 0)], [(0, 1, 2)])
        scene, report = run("tiny-nonzero-triangle-retained", tiny, "gis-static", True)
        assert len(scene["meshes"][0]["triangles"]) == 1
        assert not any(d["code"] == "DEGENERATE_TRIANGLES_REMOVED" for d in report["diagnostics"])
        overflow = geometry(base, [(0, 0, 0), (1e154, 0, 0), (0, 1e154, 0)], [(0, 1, 2)])
        run("overflow-area-is-error-not-cleanup", overflow, "gis-static", False)

        # Scaling extreme axes can round a real nonzero area to zero. It may be
        # refused as numerically unsafe, but must never become a cleanup success.
        imbalanced_source = root / "imbalanced.fbx"
        imbalanced_output = root / "imbalanced-bundle"
        imbalanced_report = root / "imbalanced.json"
        imbalanced_source.write_text(geometry(base, [(0, 0, 0), (1e308, 1e-308, 0), (1e308, 0, 0)], [(0, 1, 2)]), encoding="utf-8")
        result = subprocess.run([str(exe), "prepare", str(imbalanced_source), "--output", str(imbalanced_output),
                                 "--report", str(imbalanced_report), "--profile", "gis-static"],
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        imbalanced = json.loads(imbalanced_report.read_text(encoding="utf-8"))
        assert not any(d["code"] == "DEGENERATE_TRIANGLES_REMOVED" for d in imbalanced["diagnostics"]), "An extreme but nonzero triangle was silently removed"
        if result.returncode == 0:
            imbalanced_scene = json.loads((imbalanced_output / "scene.json").read_text(encoding="utf-8"))
            assert sum(len(mesh["triangles"]) for mesh in imbalanced_scene["meshes"]) == 1
        else:
            assert not imbalanced_output.exists() and imbalanced["status"] in {"failed", "rejected"}
        cases.append("imbalanced-nonzero-area-is-never-cleaned")
        print("PASS", cases[-1])

        invalid_output = root / "invalid-profile-output"
        invoke(exe, "prepare", fixtures / "colored_quad.fbx", "--output", invalid_output,
               "--profile", "silently-ignore-everything", expect_success=False)
        assert not invalid_output.exists()
        cases.append("invalid-profile-rejected-before-output")
    print(f"PASS {len(cases)} static-GIS profile cases: defaults, animation, lighting-only omission, strict safety, exact-zero cleanup")


if __name__ == "__main__":
    main()
