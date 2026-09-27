"""Generate the tiny project-authored glTF 2.0 GLB acceptance asset."""
import json
from pathlib import Path
import struct

FIXTURES = Path(__file__).resolve().parent / "fixtures"


def make_glb(document=None, binary=None, *, image=True):
    if binary is None:
        points = [0, 0, 0, 2, 0, 0, 2, 0, -3, 0, 0, -3]
        normals = [0, 1, 0] * 4
        uvs = [0, 0, 1, 0, 1, 1, 0, 1]
        binary = struct.pack("<12f12f8f6H", *points, *normals, *uvs, 0, 1, 2, 0, 2, 3)
        if image:
            binary += (FIXTURES / "checker.png").read_bytes()
    if document is None:
        document = {
            "asset": {"version": "2.0", "generator": "GeoModelBridge project-authored fixture"},
            "extensionsUsed": ["KHR_materials_unlit", "KHR_texture_transform"],
            "extensionsRequired": ["KHR_materials_unlit"],
            "scene": 0, "scenes": [{"nodes": [0]}],
            "nodes": [{"name": "Quad", "mesh": 0, "translation": [10, 1, 2]}],
            "meshes": [{"name": "Textured quad", "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                "indices": 3, "material": 0
            }]}],
            "buffers": [{"byteLength": len(binary)}],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": 48},
                {"buffer": 0, "byteOffset": 48, "byteLength": 48},
                {"buffer": 0, "byteOffset": 96, "byteLength": 32},
                {"buffer": 0, "byteOffset": 128, "byteLength": 12},
                {"buffer": 0, "byteOffset": 140, "byteLength": len(binary)-140},
            ],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
                 "min": [0, 0, -3], "max": [2, 0, 0]},
                {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
                {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
                {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
            ],
            "images": [{"bufferView": 4, "mimeType": "image/png", "name": "Checker"}],
            "textures": [{"source": 0}],
            "materials": [{"name": "Unlit checker", "extensions": {"KHR_materials_unlit": {}},
                           "alphaMode": "BLEND", "doubleSided": True,
                           "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.6, 0.4, 0.75],
                                                    "baseColorTexture": {"index": 0, "extensions": {
                                                        "KHR_texture_transform": {"offset": [0.25, 0.125],
                                                                                  "scale": [0.5, 0.5]}}}}}],
        }
    json_bytes = json.dumps(document, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    json_bytes += b" " * (-len(json_bytes) % 4)
    binary += b"\0" * (-len(binary) % 4)
    return (struct.pack("<4sII", b"glTF", 2, 28+len(json_bytes)+len(binary)) +
            struct.pack("<I4s", len(json_bytes), b"JSON") + json_bytes +
            struct.pack("<I4s", len(binary), b"BIN\0") + binary)


if __name__ == "__main__":
    (FIXTURES / "textured_quad.glb").write_bytes(make_glb())
