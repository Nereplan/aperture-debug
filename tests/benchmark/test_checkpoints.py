"""Focused tests for checkpoint ingestion, integrity and historical evidence migration."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("checkpoints", ROOT / "scripts/checkpoints.py")
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)


class CheckpointsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.html, cls.data = api.load_html(api.DEFAULT_HTML)

    def bundle(self):
        cp = copy.deepcopy(self.data["checkpoints"][-1])
        cp["id"] = "test-new-checkpoint"
        cp["measuredAt"] = "2026-10-17T21:00:00+03:00"
        cp["results"] = []
        return {"checkpoints": [cp]}

    def test_saved_dataset_is_valid(self):
        api.validate(self.data)

    def test_complete_app_values_match_raw_summary(self):
        summary = json.loads((ROOT / "docs/evidence/backend-balance-20261017/application-summary.json").read_text())
        cp = next(c for c in self.data["checkpoints"] if c["id"] == "backend-balance-20261017-after")
        expected = summary["app-after"]["Aperture"]
        fields = {"visible-ms": ("visible_ms", 1), "private-mib": ("private_bytes", 1048576), "gpu-mib": ("gpu_dedicated_bytes", 1048576), "cpu-core": ("cpu_one_core_percent", 1), "runtime-mib": ("runtime_bytes", 1048576)}
        for metric, (field, divisor) in fields.items():
            r = next(r for r in cp["results"] if r["metric"] == metric)
            self.assertEqual(r["value"], expected[field] / divisor)
            ref = self.data["references"][r["reference"]]
            b = next(x for x in ref["results"] if api.identity(x) == api.identity(r))
            self.assertEqual(b["value"], summary["app-after"]["VLC"][field] / divisor)

    def test_archived_reports_match_preserved_sources(self):
        for report in self.data["reports"]:
            if report["source"].startswith("docs/reports/"):
                self.assertEqual(report["text"], (ROOT / report["source"]).read_text(encoding="utf-8-sig"))

    def test_startup_history_values_match_original_capture_files(self):
        fields = {"shell-ms": "startupMs", "populated-ms": "populatedMs",
                  "startup-private-bytes": "privateBytes", "startup-ws-bytes": "workingSetBytes",
                  "startup-window-ms": "windowShownMs"}
        count = 0
        for cp in self.data["checkpoints"]:
            for r in cp["results"]:
                if r.get("historySeries") != "empty-profile-startup":
                    continue
                raw = json.loads((ROOT / r["evidence"]).read_text(encoding="utf-8-sig"))
                field = fields[r["metric"]]
                expected = raw["metrics"][field]["median"] if "metrics" in raw else raw["shownMs" if field == "windowShownMs" else field]
                self.assertEqual(r["value"], expected)
                count += 1
        self.assertEqual(count, 35)

    def test_append_preserves_historical_data(self):
        original = copy.deepcopy(self.data)
        result = api.append_bundle(self.data, self.bundle())
        self.assertEqual(result["checkpoints"][:-1], original["checkpoints"])
        self.assertEqual(self.data, original)

    def test_duplicate_id_and_observation_rejected(self):
        with self.assertRaisesRegex(ValueError, "Duplicate checkpoint"):
            api.append_bundle(self.data, {"checkpoints": [self.data["checkpoints"][-1]]})
        bundle = self.bundle()
        r = copy.deepcopy(self.data["checkpoints"][-1]["results"][0])
        bundle["checkpoints"][0]["results"] = [r, r]
        with self.assertRaisesRegex(ValueError, "Duplicate metric"):
            api.append_bundle(self.data, bundle)

    def test_incompatible_reference_rejected(self):
        bundle = self.bundle()
        r = copy.deepcopy(self.data["checkpoints"][-1]["results"][0])
        r["session"] = "new-session-with-no-new-vlc-measurement"
        bundle["checkpoints"][0]["results"] = [r]
        with self.assertRaisesRegex(ValueError, "Reference conditions/session"):
            api.append_bundle(self.data, bundle)

    def test_invalid_values_and_missing_reasons_rejected(self):
        for value in [-1, float("inf"), float("nan"), True, None]:
            with self.subTest(value=value):
                bundle = self.bundle()
                r = copy.deepcopy(self.data["checkpoints"][-1]["results"][0])
                r["value"] = value
                bundle["checkpoints"][0]["results"] = [r]
                with self.assertRaises(ValueError):
                    api.append_bundle(self.data, bundle)

    def test_zero_and_explicit_unavailable_values_are_preserved(self):
        for value in [0, None]:
            bundle = self.bundle()
            r = copy.deepcopy(self.data["checkpoints"][-1]["results"][0])
            r["value"] = value
            if value is None:
                r["missingReason"] = "GPU counter unavailable"
            bundle["checkpoints"][0]["results"] = [r]
            updated = api.append_bundle(self.data, bundle)
            self.assertIs(updated["checkpoints"][-1]["results"][0]["value"], value)

    def test_timezone_and_calendar_validation(self):
        for date in ["2026-02-30", "2026-10-17T21:00:00"]:
            bundle = self.bundle()
            bundle["checkpoints"][0]["measuredAt"] = date
            with self.assertRaises(ValueError):
                api.append_bundle(self.data, bundle)

    def test_invalid_cpu_capacity_denominators_rejected(self):
        for count in [0, -1, True, 24.5]:
            data = copy.deepcopy(self.data)
            data["environments"]["ryzen3900x-4070super"]["logicalProcessors"] = count
            with self.assertRaisesRegex(ValueError, "Invalid logical processor count"):
                api.validate(data)

    def test_reversed_run_ranges_rejected(self):
        bundle = self.bundle()
        r = copy.deepcopy(self.data["checkpoints"][-1]["results"][0])
        r["runRange"] = {"min": 200, "max": 100, "runs": 3, "scope": "Measured launches"}
        bundle["checkpoints"][0]["results"] = [r]
        with self.assertRaisesRegex(ValueError, "Invalid run range"):
            api.append_bundle(self.data, bundle)

    def test_historical_definitions_cannot_be_redefined(self):
        bundle = self.bundle()
        bundle["metrics"] = {"visible-ms": {"label": "Seconds", "unit": "ms", "direction": "higher"}}
        with self.assertRaisesRegex(ValueError, "Cannot redefine"):
            api.append_bundle(self.data, bundle)

    def test_history_group_requires_declared_protocol_and_workload(self):
        r = copy.deepcopy(self.data["checkpoints"][-1]["results"][0])
        bundle = self.bundle()
        bundle["historySeries"] = {"test-history": {
            "label": "Same endpoint history", "method": "Recorded readings of the same workload",
            "workload": r["workload"], "protocols": [r["protocol"]]}}
        r["historySeries"] = "test-history"
        bundle["checkpoints"][0]["results"] = [r]
        api.append_bundle(self.data, bundle)
        r["workload"] = "Different workload"
        with self.assertRaisesRegex(ValueError, "Incompatible history series"):
            api.append_bundle(self.data, bundle)

    def test_unknown_history_group_rejected(self):
        bundle = self.bundle()
        r = copy.deepcopy(self.data["checkpoints"][-1]["results"][0])
        r["historySeries"] = "not-a-declared-history"
        bundle["checkpoints"][0]["results"] = [r]
        with self.assertRaisesRegex(ValueError, "Unknown history series"):
            api.append_bundle(self.data, bundle)

    def test_unsafe_payload_roundtrips_without_breaking_script(self):
        data = copy.deepcopy(self.data)
        data["checkpoints"][-1]["notes"] = '</script><script>alert("test")</script>&<>&'
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "checkpoints.html"
            api.write_html(path, self.html, data)
            text, saved = api.load_html(path)
            self.assertEqual(saved, data)
            self.assertNotIn(data["checkpoints"][-1]["notes"], text)
            self.assertEqual(api.BLOCK.sub("DATA", text), api.BLOCK.sub("DATA", self.html))

    def test_invalid_write_leaves_file_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "checkpoints.html"
            path.write_text(self.html, encoding="utf-8")
            before = path.read_bytes()
            data = copy.deepcopy(self.data)
            data["checkpoints"][-1]["pairedWith"] = "not-a-checkpoint"
            with self.assertRaises(ValueError):
                api.write_html(path, self.html, data)
            self.assertEqual(path.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
