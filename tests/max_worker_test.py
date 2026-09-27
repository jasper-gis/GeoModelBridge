"""Filesystem/render policy checks without pymxs; not real Max validation."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
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

    def test_resolution(self):
        extra = self.root / "extra"
        extra.mkdir()
        image = extra / "中文 image.png"
        image.write_bytes(b"original")
        self.assertEqual(worker.resolve_image(image.name, self.source, [extra]), image)
        self.assertIsNone(worker.resolve_image("missing.png", self.source, [extra]))

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


if __name__ == "__main__":
    unittest.main()
