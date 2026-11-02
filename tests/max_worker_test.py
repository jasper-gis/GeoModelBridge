"""Filesystem/render policy checks without pymxs; not real Max validation."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
import zlib
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("max_worker", ROOT / "adapters/3dsmax/worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class WorkerPolicy(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gmb-max-worker-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source.max"
        self.source.write_bytes(b"test only")

    def test_version(self):
        self.assertEqual(worker.VERSION, (ROOT / "VERSION").read_text().strip())

    def test_manifest_keeps_face_assets_and_records_unused_slots(self):
        materials = [dict(name=name, texture_sha256=image) for name, image in
                     [("used", "shared"), ("unused-shared", "shared"),
                      ("unused-only", "other"), ("zero-area-only", None)]]
        textures = {image: dict(sha256=image) for image in ("shared", "other")}
        nodes = [dict(face_materials={"used": 5, "zero-area-only": 1})]
        result = worker.referenced_assets(nodes, materials, textures)
        self.assertEqual([m["name"] for m in result["materials"]], ["used", "zero-area-only"])
        self.assertEqual([m["name"] for m in result["unreferenced_materials"]],
                         ["unused-shared", "unused-only"])
        self.assertEqual(result["textures"], [dict(sha256="shared")])
        self.assertEqual(result["unreferenced_textures"], [dict(sha256="other")])
        # Missing referenced assets must not be hidden by the partition.
        for bad_materials, bad_textures in [(materials[1:], textures), (materials, {})]:
            with self.assertRaises(worker.Rejected) as error:
                worker.referenced_assets(nodes, bad_materials, bad_textures)
            self.assertEqual(error.exception.code, "MAX_EXPORT_MISMATCH")

    def test_resolution(self):
        extra = self.root / "extra"
        extra.mkdir()
        image = extra / "中文 image.png"
        image.write_bytes(b"original")
        self.assertEqual(worker.resolve_image(image.name, self.source, [extra]), image)
        self.assertIsNone(worker.resolve_image("missing.png", self.source, [extra]))

    def test_stale_absolute_path_finds_model_sibling_before_extra_directory(self):
        sibling = self.root / "中文 image.png"
        sibling.write_bytes(b"model sibling")
        extra = self.root / "extra"
        extra.mkdir()
        (extra / sibling.name).write_bytes(b"alternate")
        stale = self.root / "absent-old-computer" / sibling.name
        self.assertEqual(worker.resolve_image(str(stale), self.source, [extra]), sibling)
        declared = extra / sibling.name
        self.assertEqual(worker.resolve_image(str(declared), self.source, []), declared)

    def test_invalid_declared_parent_never_uses_model_sibling(self):
        sibling = self.root / "image.png"
        sibling.write_bytes(b"must not mask an invalid selected path")
        parent = self.root / "not-a-directory"
        parent.write_bytes(b"file")
        with self.assertRaises(worker.Rejected) as error:
            worker.resolve_image(str(parent / "nested" / sibling.name), self.source, [])
        self.assertEqual(error.exception.code, "TEXTURE_READ_ERROR")

    def test_unreadable_path_metadata_preserves_read_error(self):
        with patch.object(Path, "lstat", side_effect=PermissionError("denied metadata")):
            with self.assertRaises(worker.Rejected) as error:
                worker.resolve_image(str(self.source), self.source, [])
        self.assertEqual(error.exception.code, "TEXTURE_READ_ERROR")

    def test_uv_repair_manifest_identifies_original_bindings(self):
        materials = {"image": dict(source_name="原材质", texture_sha256="normalized"),
                     "color": dict(source_name="Color", texture_sha256=None)}
        textures = {"normalized": dict(source="原图.png", original_sha256="original")}
        result = worker.missing_uv_record("原节点", 9, {"image": 5, "color": 4}, materials, textures)
        self.assertEqual(result["mesh_triangles"], 9)
        self.assertEqual(result["textured_triangles"], 5)
        self.assertEqual(result["required_uv_channel"], 1)
        self.assertEqual(result["bindings"], [dict(source_material="原材质", triangles=5,
                         source_image="原图.png", source_image_sha256="original")])

    def test_invalid_selected_path_never_falls_back(self):
        extra = self.root / "extra"
        extra.mkdir()
        (extra / "image.png").write_bytes(b"alternate")
        (self.root / "image.png").mkdir()
        with self.assertRaises(worker.Rejected) as error:
            worker.resolve_image("image.png", self.source, [extra])
        self.assertEqual(error.exception.code, "TEXTURE_READ_ERROR")
        parent = self.root / "not-a-folder"
        parent.write_bytes(b"file")
        with self.assertRaises((worker.Rejected, NotADirectoryError)):
            worker.resolve_image(str(parent / "image.png"), self.source, [extra])

    def test_permission_error_never_falls_back(self):
        with patch.object(Path, "open", side_effect=PermissionError("denied")):
            with self.assertRaises(worker.Rejected) as error:
                worker.resolve_image(str(self.source), self.source, [])
        self.assertEqual(error.exception.code, "TEXTURE_READ_ERROR")

    def test_digest_limit_and_links(self):
        with self.assertRaises(worker.Rejected):
            worker.digest(self.source, 1)
        link = self.root / "linked.max"
        try:
            link.symlink_to(self.source)
        except OSError:
            return  # Windows may deny unprivileged symlink creation.
        with self.assertRaises(worker.Rejected):
            worker.digest(link, 100)

    def test_png_alpha_rejection(self):
        for color_type in (2, 4, 6):
            path = self.root / (str(color_type) + ".png")
            ihdr = struct.pack(">IIBBBBB", 1, 1, 8, color_type, 0, 0, 0)
            path.write_bytes(b"\x89PNG\r\n\x1a\n" + struct.pack(">I4s", 13, b"IHDR") + ihdr + b"0000" + struct.pack(">I4s", 0, b"IDAT"))
            if color_type == 2:
                worker.opaque_image(path)
            else:
                with self.assertRaises(worker.Rejected) as error:
                    worker.opaque_image(path)
                self.assertEqual(error.exception.code, "MAX_IMAGE_ALPHA_UNSUPPORTED")

    def test_render_semantics_fail_closed(self):
        for value in (0.5, float("nan"), float("inf")):
            with self.assertRaises(worker.Rejected):
                worker.neutral(SimpleNamespace(U_Tiling=value), {"U_Tiling": 1}, "map")
        with self.assertRaises(AttributeError):
            worker.neutral(SimpleNamespace(), {"unsupported_property": False}, "map")

    def test_scalar_opacity_and_invalid_color(self):
        material = SimpleNamespace(name="test", diffuse=SimpleNamespace(r=255, g=127.5, b=0), opacity=25)
        self.assertEqual(worker.rgba(material), [1, .5, 0, .25])
        material.opacity = float("nan")
        with self.assertRaises(worker.Rejected):
            worker.rgba(material)

    def test_inactive_mental_ray_requires_scanline_static_and_exact_plugin(self):
        known = ["mrmaterialattribs.gup - mental ray: material custom attribute"]
        worker.check_missing_plugins(known, "gis-static", "Default_Scanline_Renderer")
        worker.check_missing_plugins([], "strict", "other")
        for plugins, profile, renderer in [
            (known, "strict", "Default_Scanline_Renderer"),
            (known, "gis-static", "mental_ray_renderer"),
            (known, "gis-static", "Arnold"),
            (known + ["missing.dlo - Geometry"], "gis-static", "Default_Scanline_Renderer"),
            (["mrmaterialattribs.gup - mental ray: Displacement Custom Attributes"], "gis-static", "Default_Scanline_Renderer"),
        ]:
            with self.assertRaises(worker.Rejected) as error:
                worker.check_missing_plugins(plugins, profile, renderer)
            self.assertEqual(error.exception.code, "MAX_MISSING_PLUGIN")

    def test_standard_alpha_is_not_implicitly_surface_opacity(self):
        rgba = bytes((200, 40, 80, 128, 10, 20, 30, 255, 0, 0, 0, 0))
        original = bytes(rgba)
        # Independently confirmed with Max 2024 Scanline, full-strength diffuse,
        # green base, gamma-1 source: (200,167,80), opaque without opacity map.
        self.assertEqual(worker.standard_pixels(rgba, (0, 1, 0), True, True, False),
                         bytes((200, 167, 80, 255, 10, 20, 30, 255, 0, 255, 0, 255)))
        self.assertEqual(worker.standard_pixels(rgba, (0, 1, 0), False, True, True), rgba)
        self.assertEqual(worker.standard_pixels(rgba[:4], (0, 1, 0), True, False, True),
                         bytes((100, 147, 40, 128)))
        self.assertEqual(rgba, original)

    def test_multimaterial_wrap_and_unassigned_holes(self):
        # Independent Max Scanline samples are red/green for these bindings.
        self.assertEqual(worker.effective_material_id(3, [1]), 1)
        self.assertEqual(worker.effective_material_id(4, [1, 2]), 2)
        self.assertEqual(worker.effective_material_id(6, [1, 3]), 3)
        self.assertEqual(worker.effective_material_id(6, [2, 4]), 2)
        for face, ids in [(2, [1, 3]), (5, [2, 4]), (0, [1]), (65537, [1]), (1, [])]:
            with self.assertRaises(worker.Rejected):
                worker.effective_material_id(face, ids)
        self.assertIsNone(worker.effective_material_id(2, [1, 3], True))
        with self.assertRaises(worker.Rejected):
            worker.effective_material_id(0, [1], True)

    def test_crop_pixel_centers_and_bounds(self):
        pixels = bytes((10,20,30,40, 50,60,70,80, 90,100,110,120, 130,140,150,160))
        self.assertEqual(worker.crop_pixels(2,2,pixels,(0,0,1,1)), (2,2,pixels))
        self.assertEqual(worker.crop_pixels(2,2,pixels,(.5,0,.5,1)), (1,2,pixels[4:8]+pixels[12:16]))
        self.assertEqual(worker.crop_pixels(2,2,pixels,(.25,.25,.5,.5)), (1,1,bytes((70,80,90,100))))
        for crop in [(0,0,0,1),(-.1,0,1,1),(0,0,1.1,1),(0,float('nan'),1,1)]:
            with self.assertRaises(worker.Rejected):
                worker.crop_pixels(2,2,pixels,crop)

    def test_nested_multimaterial_uses_original_face_id(self):
        tree = ([1,2], [([1,2,3], ['red','green','blue']), 'yellow'])
        self.assertEqual([worker.resolve_material_tree(tree, i) for i in range(1,8)],
                         ['red','yellow','blue','yellow','green','yellow','red'])
        self.assertEqual(list(worker.material_leaves(tree)), ['red','green','blue','yellow'])

    def test_baked_uvs_preserve_material_corner_boundaries(self):
        def point(x,y,z):
            return SimpleNamespace(x=x,y=y,z=z)
        original = [point(0,0,0),point(1,0,0),point(0,1,0)]
        faces, vertices = {}, {}
        ops = SimpleNamespace(getNumMapVerts=lambda m,c: 3,
            getMapVert=lambda m,c,i: original[i-1],getMapFace=lambda m,c,i: point(1,2,3),
            setNumMapVerts=lambda *a,**kw: None,
            setMapVert=lambda m,c,i,v: vertices.update({i:v}),
            setMapFace=lambda m,c,i,v: faces.update({i:v}))
        rt = SimpleNamespace(meshop=ops,Point3=point)
        self.assertTrue(worker.bake_corner_uvs(rt,None,[(.25,.25,True),(2,2,False)],'test'))
        self.assertEqual(len(vertices),6)
        self.assertEqual([faces[1].x,faces[2].x],[1,4])
        self.assertEqual([(vertices[i].x,vertices[i].y) for i in range(1,7)],
                         [(0,0),(.25,0),(0,.25),(-.5,-.5),(1.5,-.5),(-.5,1.5)])
        ops.getMapFace=lambda m,c,i: point(0,2,3)
        with self.assertRaises(worker.Rejected):
            worker.bake_corner_uvs(rt,None,[(.25,.25,True)],'invalid')

    def test_exporter_name_wrapper(self):
        class MaxName:
            def __str__(self):
                return 'z'
        self.assertTrue(worker.exporter_parameter_matches(MaxName(), 'Z'))
        self.assertFalse(worker.exporter_parameter_matches(MaxName(), 'Y'))
        self.assertFalse(worker.exporter_parameter_matches(False, True))

    def test_normalized_png_pixels_and_corruption(self):
        pixels = bytes((0, 1, 2, 0, 200, 40, 80, 128, 255, 254, 253, 255, 4, 5, 6, 255))
        encoded = worker.encode_rgba(2, 2, pixels)
        path = self.root / "surface.png"
        path.write_bytes(encoded)
        self.assertEqual(worker.png_rgba(path, 1000), (2, 2, pixels))
        for bad in (encoded[:-1], encoded + b"extra", encoded[:44] + bytes([encoded[44] ^ 1]) + encoded[45:]):
            path.write_bytes(bad)
            with self.assertRaises(worker.Rejected) as error:
                worker.png_rgba(path, 1000)
            self.assertEqual(error.exception.code, "TEXTURE_READ_ERROR")

    def test_png_all_filters_16bit_and_decoded_limit(self):
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
        # Each PNG filter encodes the same two pixels in two equal rows.
        row = bytes((0x80, 0x12, 0xff, 0xff, 0x20, 0x01, 0x00, 0x00))  # gray+alpha16
        for mode in range(5):
            rows = bytearray()
            prev = bytes(len(row))
            for _ in range(2):
                rows.append(mode)
                for i, value in enumerate(row):
                    a, b, c = (row[i-4] if i >= 4 else 0), prev[i], (prev[i-4] if i >= 4 else 0)
                    p = a+b-c
                    distances = [abs(p-a), abs(p-b), abs(p-c)]
                    predictors = [0, a, b, (a+b)//2, [a,b,c][distances.index(min(distances))]]
                    rows.append((value-predictors[mode]) & 255)
                prev = row
            path = self.root / ("filter-%s.png" % mode)
            path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2,2,16,4,0,0,0))
                             + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
            expected = bytes((128,128,128,255,32,32,32,0))*2
            self.assertEqual(worker.png_rgba(path, 1000), (2,2,expected))
            with self.assertRaises(worker.Rejected):
                worker.png_rgba(path, 15)


if __name__ == "__main__":
    unittest.main()
