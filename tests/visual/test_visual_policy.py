#!/usr/bin/env python3
"""
Unit and Policy Tests for Aperture Visual Regression Gate and Baseline Manifest.

Validates visual baseline schema integrity, scene definitions, cryptographic hashes,
pixel comparison tolerances, lossless PNG decoding/encoding, and native probe hook integration.
"""

import json
import os
import re
import unittest

from png_util import (
    compute_file_sha256,
    decode_png,
    read_png_dimensions,
    write_png,
    compare_images,
)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
BASELINE_PATH = os.path.join(SCRIPT_DIR, "visual_baseline.json")
SCHEMA_PATH = os.path.join(SCRIPT_DIR, "visual_baseline.schema.json")
MAIN_CPP_PATH = os.path.join(REPO_ROOT, "src", "main.cpp")
IPC_PROTOCOL_H_PATH = os.path.join(REPO_ROOT, "src", "IpcProtocol.h")
RENDERER_H_PATH = os.path.join(REPO_ROOT, "src", "Renderer.h")
RENDERER_CPP_PATH = os.path.join(REPO_ROOT, "src", "Renderer.cpp")
APP_H_PATH = os.path.join(REPO_ROOT, "src", "App.h")
APP_CPP_PATH = os.path.join(REPO_ROOT, "src", "App.cpp")
CMAKELISTS_PATH = os.path.join(REPO_ROOT, "CMakeLists.txt")
SUITES_JSON_PATH = os.path.join(REPO_ROOT, "tests", "suites.json")


class TestVisualPolicy(unittest.TestCase):
    """Authoritative visual regression policy, baseline, and probe verification suite."""

    def test_baseline_json_exists_and_parses(self):
        self.assertTrue(os.path.isfile(BASELINE_PATH), f"Missing baseline at {BASELINE_PATH}")
        with open(BASELINE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.assertIsInstance(data, dict)
        self.assertGreaterEqual(data.get("version", 0), 1)
        self.assertEqual(data.get("platform"), "windows-x64")
        self.assertEqual(data.get("configuration"), "Release")
        self.assertIn("environment", data)
        self.assertIn("tolerances", data)
        self.assertIn("scenes", data)

    def test_schema_json_exists_and_parses(self):
        self.assertTrue(os.path.isfile(SCHEMA_PATH), f"Missing schema at {SCHEMA_PATH}")
        with open(SCHEMA_PATH, "r", encoding="utf-8") as f:
            schema = json.load(f)
        self.assertIsInstance(schema, dict)
        self.assertEqual(schema.get("type"), "object")
        props = schema.get("properties", {})
        self.assertIn("environment", props)
        self.assertIn("tolerances", props)
        self.assertIn("scenes", props)

    def test_environment_consistency(self):
        with open(BASELINE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        env = data.get("environment", {})
        self.assertEqual(env.get("dpi"), 96.0)
        self.assertEqual(env.get("dpiScale"), 1.0)
        self.assertEqual(env.get("theme"), "default")
        self.assertEqual(env.get("fontFamily"), "Inter")
        self.assertTrue(env.get("reduceMotion"))
        self.assertGreaterEqual(env.get("windowWidth", 0), 640)
        self.assertGreaterEqual(env.get("windowHeight", 0), 420)

    def test_scenes_and_tolerances_bounds(self):
        with open(BASELINE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        scenes = data.get("scenes", [])
        self.assertGreaterEqual(len(scenes), 1, "Must declare at least one visual regression scene")

        scene_ids = set()
        for s in scenes:
            sid = s.get("id")
            self.assertIsNotNone(sid, "Scene missing 'id'")
            self.assertNotIn(sid, scene_ids, f"Duplicate scene id: {sid}")
            scene_ids.add(sid)

            self.assertIn(s.get("tab"), ["media", "playlists", "history", "settings"])
            self.assertGreater(s.get("width", 0), 0)
            self.assertGreater(s.get("height", 0), 0)
            self.assertTrue(0.0 <= s.get("maxMismatchedPixelPercent", -1.0) <= 100.0)
            self.assertTrue(0 <= s.get("perPixelColorDeltaThreshold", -1) <= 255)
            self.assertTrue(0 <= s.get("maxPeakColorDelta", -1) <= 255)

            sha = s.get("sha256", "")
            self.assertTrue(bool(re.match(r"^[a-fA-F0-9]{64}$", sha)), f"Invalid sha256 in scene {sid}: {sha}")

    def test_baseline_files_exist_and_match_hashes_and_dimensions(self):
        with open(BASELINE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        scenes = data.get("scenes", [])

        for s in scenes:
            sid = s["id"]
            rel_path = s["baselineFile"]
            full_path = os.path.join(SCRIPT_DIR, rel_path)
            self.assertTrue(os.path.isfile(full_path), f"Scene {sid} baseline file not found: {full_path}")

            w, h = read_png_dimensions(full_path)
            self.assertEqual(w, s["width"], f"Scene {sid} width mismatch ({w} != {s['width']})")
            self.assertEqual(h, s["height"], f"Scene {sid} height mismatch ({h} != {s['height']})")

            actual_sha = compute_file_sha256(full_path)
            self.assertEqual(
                actual_sha.lower(),
                s["sha256"].lower(),
                f"Scene {sid} SHA-256 mismatch (actual {actual_sha} != expected {s['sha256']})",
            )

    def test_pure_python_png_decode_encode_roundtrip(self):
        tmp_png = os.path.join(SCRIPT_DIR, "_tmp_test_roundtrip.png")
        try:
            w, h = 32, 24
            pixels = bytearray(w * h * 4)
            for y in range(h):
                for x in range(w):
                    idx = (y * w + x) * 4
                    pixels[idx] = (x * 7) % 256
                    pixels[idx + 1] = (y * 11) % 256
                    pixels[idx + 2] = ((x + y) * 5) % 256
                    pixels[idx + 3] = 255

            write_png(w, h, pixels, tmp_png)
            rw, rh, rpixels = decode_png(tmp_png)
            self.assertEqual(w, rw)
            self.assertEqual(h, rh)
            self.assertEqual(bytes(pixels), bytes(rpixels))
        finally:
            if os.path.exists(tmp_png):
                os.remove(tmp_png)

    def test_image_comparison_math_and_thresholds(self):
        tmp_base = os.path.join(SCRIPT_DIR, "_tmp_base.png")
        tmp_actual = os.path.join(SCRIPT_DIR, "_tmp_actual.png")
        tmp_diff = os.path.join(SCRIPT_DIR, "_tmp_diff.png")

        try:
            w, h = 10, 10
            base_px = bytearray(w * h * 4)
            actual_px = bytearray(w * h * 4)
            for i in range(w * h * 4):
                base_px[i] = 40
                actual_px[i] = 40

            # 1. Identical comparison
            write_png(w, h, base_px, tmp_base)
            write_png(w, h, actual_px, tmp_actual)
            res = compare_images(tmp_base, tmp_actual, tmp_diff, tolerance_per_pixel=2, max_mismatch_pct=0.0, max_peak_delta=0)
            self.assertTrue(res["passed"])
            self.assertEqual(res["mismatched_pixels"], 0)
            self.assertEqual(res["peak_delta"], 0)

            # 2. Difference within per-pixel tolerance
            actual_px[0] = 42  # delta 2 (allowed with tolerance 2)
            write_png(w, h, actual_px, tmp_actual)
            res = compare_images(tmp_base, tmp_actual, tmp_diff, tolerance_per_pixel=2, max_mismatch_pct=0.0, max_peak_delta=5)
            self.assertTrue(res["passed"])
            self.assertEqual(res["mismatched_pixels"], 0)
            self.assertEqual(res["peak_delta"], 2)

            # 3. Difference exceeding per-pixel tolerance
            actual_px[0] = 55  # delta 15 (exceeds tolerance 2)
            write_png(w, h, actual_px, tmp_actual)
            res = compare_images(tmp_base, tmp_actual, tmp_diff, tolerance_per_pixel=2, max_mismatch_pct=0.0, max_peak_delta=5)
            self.assertFalse(res["passed"])
            self.assertEqual(res["mismatched_pixels"], 1)
            self.assertEqual(res["peak_delta"], 15)
            self.assertTrue(os.path.isfile(tmp_diff), "Diff file should be emitted on comparison failure")
        finally:
            for p in (tmp_base, tmp_actual, tmp_diff):
                if os.path.exists(p):
                    os.remove(p)

    def test_source_code_probe_hooks(self):
        with open(MAIN_CPP_PATH, "r", encoding="utf-8") as f:
            main_code = f.read()
        with open(IPC_PROTOCOL_H_PATH, "r", encoding="utf-8") as f:
            ipc_code = f.read()
        with open(RENDERER_H_PATH, "r", encoding="utf-8") as f:
            renderer_h = f.read()
        with open(RENDERER_CPP_PATH, "r", encoding="utf-8") as f:
            renderer_cpp = f.read()
        with open(APP_H_PATH, "r", encoding="utf-8") as f:
            app_h = f.read()
        with open(APP_CPP_PATH, "r", encoding="utf-8") as f:
            app_cpp = f.read()

        self.assertIn("visualCaptureProbe", main_code)
        self.assertIn("CaptureClientContent", main_code)
        self.assertIn("visualCaptureProbe", ipc_code)
        self.assertIn("visualCaptureScene", ipc_code)
        self.assertIn("visualCaptureOutputFile", ipc_code)
        self.assertIn("CaptureBackBufferToPng", renderer_h)
        self.assertIn("CaptureBackBufferToPng", renderer_cpp)
        self.assertIn("CaptureClientContent", app_h)
        self.assertIn("CaptureClientContent", app_cpp)

    def test_suites_json_registration(self):
        self.assertTrue(os.path.isfile(SUITES_JSON_PATH), f"Missing {SUITES_JSON_PATH}")
        with open(SUITES_JSON_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        suite_ids = [s["id"] for s in data.get("suites", [])]
        self.assertIn("visual", suite_ids, "visual suite must be registered in tests/suites.json")


if __name__ == "__main__":
    unittest.main(verbosity=2)
