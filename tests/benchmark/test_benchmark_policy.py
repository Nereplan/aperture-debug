#!/usr/bin/env python3
"""
Unit and Policy Tests for Aperture Benchmark Gate and Budget Manifest.

Validates benchmark baseline schema integrity, budget bounds, regression detection math,
and source code probe hook integration using pure Python standard library modules.
"""

import json
import os
import re
import unittest


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
BASELINE_PATH = os.path.join(REPO_ROOT, "scripts", "benchmark_baseline.json")
SCHEMA_PATH = os.path.join(REPO_ROOT, "scripts", "benchmark_baseline.schema.json")
MAIN_CPP_PATH = os.path.join(REPO_ROOT, "src", "main.cpp")
IPC_PROTOCOL_H_PATH = os.path.join(REPO_ROOT, "src", "IpcProtocol.h")
CMAKELISTS_PATH = os.path.join(REPO_ROOT, "CMakeLists.txt")
SUITES_JSON_PATH = os.path.join(REPO_ROOT, "tests", "suites.json")


class TestBenchmarkPolicy(unittest.TestCase):
    """Authoritative benchmark policy and baseline verification suite."""

    def test_baseline_json_exists_and_parses(self):
        self.assertTrue(os.path.isfile(BASELINE_PATH), f"Missing baseline at {BASELINE_PATH}")
        with open(BASELINE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.assertIsInstance(data, dict)
        self.assertGreaterEqual(data.get("version", 0), 1)
        self.assertEqual(data.get("platform"), "windows-x64")
        self.assertEqual(data.get("configuration"), "Release")

    def test_schema_json_exists_and_parses(self):
        self.assertTrue(os.path.isfile(SCHEMA_PATH), f"Missing schema at {SCHEMA_PATH}")
        with open(SCHEMA_PATH, "r", encoding="utf-8") as f:
            schema = json.load(f)
        self.assertIsInstance(schema, dict)
        self.assertEqual(schema.get("type"), "object")
        self.assertIn("budgets", schema.get("properties", {}))
        self.assertIn("sampling", schema.get("properties", {}))
        self.assertIn("baseline", schema.get("properties", {}))

    def test_budgets_consistency_and_bounds(self):
        with open(BASELINE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        budgets = data.get("budgets", {})
        self.assertIsInstance(budgets, dict)

        required_keys = [
            "startupMaxMs", "startupTargetMs",
            "windowShownMaxMs", "windowShownTargetMs",
            "workingSetMaxBytes", "workingSetTargetBytes",
            "peakWorkingSetMaxBytes", "peakWorkingSetTargetBytes",
            "privateBytesMaxBytes", "privateBytesTargetBytes",
            "peakPrivateBytesMaxBytes", "peakPrivateBytesTargetBytes",
            "gdiObjectsMax", "userObjectsMax", "maxProcesses"
        ]
        for key in required_keys:
            self.assertIn(key, budgets, f"Missing budget key: {key}")
            self.assertGreater(budgets[key], 0, f"Budget key {key} must be > 0")

        # Target <= Max
        self.assertLessEqual(budgets["startupTargetMs"], budgets["startupMaxMs"])
        self.assertLessEqual(budgets["windowShownTargetMs"], budgets["windowShownMaxMs"])
        self.assertLessEqual(budgets["workingSetTargetBytes"], budgets["workingSetMaxBytes"])
        self.assertLessEqual(budgets["peakWorkingSetTargetBytes"], budgets["peakWorkingSetMaxBytes"])
        self.assertLessEqual(budgets["privateBytesTargetBytes"], budgets["privateBytesMaxBytes"])
        self.assertLessEqual(budgets["peakPrivateBytesTargetBytes"], budgets["peakPrivateBytesMaxBytes"])

    def test_baseline_medians_satisfy_budgets(self):
        with open(BASELINE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        budgets = data["budgets"]
        baseline = data["baseline"]

        self.assertLessEqual(baseline["startupMedianMs"], budgets["startupMaxMs"])
        self.assertLessEqual(baseline["windowShownMedianMs"], budgets["windowShownMaxMs"])
        self.assertLessEqual(baseline["workingSetMedianBytes"], budgets["workingSetMaxBytes"])
        self.assertLessEqual(baseline["peakWorkingSetMedianBytes"], budgets["peakWorkingSetMaxBytes"])
        self.assertLessEqual(baseline["privateBytesMedianBytes"], budgets["privateBytesMaxBytes"])
        self.assertLessEqual(baseline["peakPrivateBytesMedianBytes"], budgets["peakPrivateBytesMaxBytes"])
        self.assertLessEqual(baseline["gdiObjects"], budgets["gdiObjectsMax"])
        self.assertLessEqual(baseline["userObjects"], budgets["userObjectsMax"])
        self.assertEqual(baseline["processCount"], 1)

    def test_sampling_parameters(self):
        with open(BASELINE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        sampling = data.get("sampling", {})
        self.assertGreaterEqual(sampling.get("warmupIterations", -1), 0)
        self.assertGreaterEqual(sampling.get("measuredIterations", 0), 1)
        self.assertGreaterEqual(sampling.get("timeoutSecondsPerSample", 0), 1)

    def test_regression_detection_math(self):
        # Deterministic simulation of passing and regressing sample collections
        budgets = {
            "startupMaxMs": 1500.0,
            "workingSetMaxBytes": 157286400,
        }

        # Case 1: Pass
        pass_samples = [310.0, 320.0, 330.0, 315.0, 325.0]
        pass_samples.sort()
        pass_median = pass_samples[len(pass_samples) // 2]
        self.assertLessEqual(pass_median, budgets["startupMaxMs"])

        # Case 2: Regression
        fail_samples = [1600.0, 1550.0, 1620.0, 1580.0, 1590.0]
        fail_samples.sort()
        fail_median = fail_samples[len(fail_samples) // 2]
        self.assertGreater(fail_median, budgets["startupMaxMs"])

    def test_source_code_probe_hooks(self):
        self.assertTrue(os.path.isfile(MAIN_CPP_PATH))
        self.assertTrue(os.path.isfile(IPC_PROTOCOL_H_PATH))

        with open(MAIN_CPP_PATH, "r", encoding="utf-8") as f:
            main_code = f.read()
        with open(IPC_PROTOCOL_H_PATH, "r", encoding="utf-8") as f:
            ipc_code = f.read()

        # Check for benchmark-probe CLI option
        self.assertIn("benchmarkProbe", ipc_code)
        self.assertIn("benchmarkOutputFile", ipc_code)
        self.assertIn("benchmark-probe", ipc_code)

        # Check for Win32 memory measurement in main.cpp
        self.assertIn("GetProcessMemoryInfo", main_code)
        self.assertIn("PROCESS_MEMORY_COUNTERS_EX", main_code)
        self.assertIn("APERTURE_BENCHMARK_PROBE_READY", main_code)
        self.assertIn("benchmarkProbe", main_code)

    def test_cmakelists_psapi_link(self):
        self.assertTrue(os.path.isfile(CMAKELISTS_PATH))
        with open(CMAKELISTS_PATH, "r", encoding="utf-8") as f:
            cmake_code = f.read()
        self.assertIn("psapi", cmake_code)


if __name__ == "__main__":
    unittest.main()
