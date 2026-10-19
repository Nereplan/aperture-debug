"""Reject invalid decode comparisons and keep warmups out of summaries."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location("compare_native_decode", Path(__file__).with_name("compare_native_decode.py"))
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


class DecodeComparisonTests(unittest.TestCase):
    def run_comparison(self, root, mismatch=False, incomplete=False):
        baseline, candidate, media = (root / f for f in ("baseline.exe", "candidate.exe", "media.mp4"))
        for path in (baseline, candidate, media):
            path.write_bytes(b"test fixture only")
        manifest = root / "cases.json"
        manifest.write_text(json.dumps([{"id": "fixture", "path": str(media), "kind": "video", "frames": 3}]))
        count = {str(baseline): 0, str(candidate): 0}

        def fake_probe(command, **kwargs):
            name = command[0]
            warmup = count[name] == 0
            count[name] += 1
            result = {"kind": "video", "decoder": "h264", "threads": 1,
                      "hardware": False, "frames": 2 if incomplete else 3,
                      "packets": 5, "units": 1800,
                      "decodedSha256": ("b" if mismatch and name == str(candidate) else "a") * 64,
                      "elapsedSeconds": 100 if warmup else (1 if name == str(baseline) else .5),
                      "processCpuSeconds": 10 if warmup else .25,
                      "processCycles": 1000000, "avcodecVersion": 62000, "windowsCreated": 0}
            return subprocess.CompletedProcess(command, 0, json.dumps(result).encode(), b"")

        argv = ["compare_native_decode", "--baseline", str(baseline), "--candidate", str(candidate),
                "--manifest", str(manifest), "--out", str(root / "out"), "--iterations", "1"]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(BENCH, "no_competing_benchmarks"), \
                mock.patch.object(BENCH.subprocess, "run", side_effect=fake_probe), contextlib.redirect_stdout(io.StringIO()):
            BENCH.main()

    def test_warmups_are_excluded_from_the_measured_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.run_comparison(root)
            result = json.loads((root / "out/results.json").read_text())
            self.assertEqual(result["status"], "passed")
            row = result["cases"][0]
            self.assertEqual(len(row["runs"]), 4)
            self.assertEqual(row["summary"]["baseline"]["elapsedSeconds"]["median"], 1)
            self.assertEqual(row["summary"]["candidate"]["elapsedSeconds"]["median"], .5)
            self.assertEqual(row["summary"]["baseline"]["elapsedSeconds"]["runs"], 1)

    def test_pixel_mismatch_rejects_the_comparison_and_retains_raw_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(RuntimeError, "Decoded output mismatch"):
                self.run_comparison(root, mismatch=True)
            result = json.loads((root / "out/results.json").read_text())
            self.assertEqual(result["status"], "failed")
            self.assertNotIn("summary", result["cases"][0])
            self.assertTrue((root / "out/fixture/0-candidate/stdout.txt").is_file())

    def test_short_decode_cannot_produce_a_successful_timing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(RuntimeError, "Invalid/incomplete"):
                self.run_comparison(root, incomplete=True)
            result = json.loads((root / "out/results.json").read_text())
            self.assertEqual(result["status"], "failed")


if __name__ == "__main__":
    unittest.main()
