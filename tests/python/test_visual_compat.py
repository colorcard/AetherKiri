import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[2] / "tools" / "visual_compat.py"
SPEC = importlib.util.spec_from_file_location("visual_compat", MODULE_PATH)
visual_compat = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(visual_compat)


def ppm(path, width, height, pixels):
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + bytes(pixels))


def layers(path, values):
    path.write_text(json.dumps({"layers": values}), encoding="utf-8")


class VisualCompatTests(unittest.TestCase):
    @staticmethod
    def checkpoint(root, reference_layer, actual_layer, reference_pixels=None,
                   actual_pixels=None):
        reference, actual = root / "reference", root / "actual"
        default = [20, 30, 40] * 64
        ppm(reference.with_suffix(".ppm"), 8, 8,
            reference_pixels or default)
        ppm(actual.with_suffix(".ppm"), 8, 8, actual_pixels or default)
        layers(reference.with_suffix(".layers.json"), [reference_layer])
        layers(actual.with_suffix(".layers.json"), [actual_layer])
        return reference, actual

    def test_small_uniform_color_shift_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference, actual = root / "reference.ppm", root / "actual.ppm"
            ppm(reference, 16, 16, [100, 110, 120] * 256)
            ppm(actual, 16, 16, [102, 111, 119] * 256)
            self.assertTrue(visual_compat.image_metrics(reference, actual)["pass"])

    def test_missing_background_layer_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference, actual = root / "reference", root / "actual"
            pixels = [80, 90, 100] * 16
            ppm(reference.with_suffix(".ppm"), 4, 4, pixels)
            ppm(actual.with_suffix(".ppm"), 4, 4, pixels)
            background = {"path": "root/bg", "parent": "root", "order": 1,
                          "visible": True, "node_visible": True, "opacity": 255,
                          "type": "opaque", "rect": {}, "clip": {},
                          "image": {"present": True, "width": 4, "height": 4}}
            layers(reference.with_suffix(".layers.json"), [background])
            layers(actual.with_suffix(".layers.json"), [])
            self.assertFalse(visual_compat.compare_checkpoint(reference, actual,
                                                               exact=False)["pass"])

    def test_layer_order_change_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference, actual = root / "reference", root / "actual"
            pixels = [20, 30, 40] * 16
            ppm(reference.with_suffix(".ppm"), 4, 4, pixels)
            ppm(actual.with_suffix(".ppm"), 4, 4, pixels)
            base = {"path": "root/character", "parent": "root", "order": 2,
                    "visible": True, "node_visible": True, "opacity": 255,
                    "type": "alpha", "rect": {}, "clip": {},
                    "image": {"present": True, "width": 4, "height": 4}}
            layers(reference.with_suffix(".layers.json"), [base])
            layers(actual.with_suffix(".layers.json"), [base | {"order": 3}])
            self.assertFalse(visual_compat.compare_checkpoint(reference, actual,
                                                               exact=False)["pass"])

    def test_opacity_zero_and_empty_clip_fail(self):
        base = {"path": "root/character", "parent": "root", "order": 2,
                "visible": True, "node_visible": True, "opacity": 255,
                "type": "alpha", "rect": {"width": 8, "height": 8},
                "clip": {"width": 8, "height": 8},
                "image": {"present": True, "width": 8, "height": 8}}
        for mutation in ({"opacity": 0}, {"clip": {}}):
            with self.subTest(mutation=mutation), \
                 tempfile.TemporaryDirectory() as directory:
                reference, actual = self.checkpoint(
                    Path(directory), base, base | mutation)
                self.assertFalse(visual_compat.compare_checkpoint(
                    reference, actual, exact=False)["pass"])

    def test_one_pixel_antialiasing_change_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference, actual = root / "reference.ppm", root / "actual.ppm"
            pixels = [0, 0, 0] * (64 * 64)
            changed = list(pixels)
            changed[(32 * 64 + 32) * 3:(32 * 64 + 32) * 3 + 3] = [8, 8, 8]
            ppm(reference, 64, 64, pixels)
            ppm(actual, 64, 64, changed)
            self.assertTrue(visual_compat.image_metrics(reference, actual)["pass"])

    def test_resource_fingerprint_changes_with_archive_content(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "data.xp3"
            archive.write_bytes(b"first")
            first, _ = visual_compat.resource_fingerprint(root)
            archive.write_bytes(b"second")
            second, _ = visual_compat.resource_fingerprint(root)
            self.assertNotEqual(first, second)

    def test_synthetic_fingerprint_uses_plain_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "startup.tjs"
            source.write_text("first", encoding="utf-8")
            first, resources = visual_compat.resource_fingerprint(root, True)
            self.assertEqual(resources[0]["name"], "startup.tjs")
            source.write_text("second", encoding="utf-8")
            second, _ = visual_compat.resource_fingerprint(root, True)
            self.assertNotEqual(first, second)

    def test_plain_files_require_synthetic_opt_in(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "startup.tjs").write_text("test", encoding="utf-8")
            with self.assertRaises(visual_compat.CompatError):
                visual_compat.resource_fingerprint(root)

    def test_source_audit_detects_source_write(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "data.xp3"
            source.write_bytes(b"original")
            before = visual_compat.source_audit(root)
            source.write_bytes(b"modified")
            self.assertNotEqual(before, visual_compat.source_audit(root))


if __name__ == "__main__":
    unittest.main()
