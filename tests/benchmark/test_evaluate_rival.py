"""Adversarial evidence checks; never launches a media player or opens a window."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(__file__).with_name("evaluate_rival.py")
spec = importlib.util.spec_from_file_location("evaluate_rival", SCRIPT)
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
MIB = 1048576


def visible_fixture():
    return {"safe": True, "reason": None, "dpi": 96,
            "screenCanvas": {"left": 40, "top": 40, "right": 1320, "bottom": 760}}


def fixture():
    """Synthetic counters with known answers, following the real runner schema."""
    report = {"schemaVersion": 1, "mediaPath": "C:/fixtures/clip.mp4", "mediaSha256": "a" * 64,
              "mediaBytes": 1000, "iterations": 3, "warmupIterations": 1,
              "environment": {"osVersion": "Windows fixture", "cpuName": "CPU fixture",
                              "logicalProcessors": 24, "gpuName": "GPU fixture",
                              "gpuDriverVersion": "fixture driver"},
              "videoCanvasTarget": [1280, 720], "players": [], "results": []}
    for name, offset in (("Aperture", 0), ("VLC", 10)):
        player = {"name": name, "exeHash": ("b" if offset else "c") * 64,
                  "runtimeManifestSha256": ("d" if offset else "e") * 64,
                  "version": "1.0.0.0", "runtimeBytes": (100 + offset) * MIB,
                  "runtimeFileCount": 5}
        report["players"].append(player)
        for round_id in range(4):
            directory = f"C:/scratch/{round_id}-{name}"
            cpu_percent = 5 + round_id + offset
            report["results"].append({
                "player": name, "variant": name, "round": round_id, "warmup": round_id == 0,
                "status": "sampled", "failure": None, "playbackReason": None,
                "playbackVerification": "advancing-playback-verified",
                "runtimeBytes": player["runtimeBytes"], "runtimeFileCount": 5,
                "executableHash": player["exeHash"], "runtimeManifestSha256": player["runtimeManifestSha256"],
                "version": player["version"], "runDirectory": directory,
                "arguments": ([report["mediaPath"]] if name == "Aperture" else
                              [f"--config={directory}/profile/vlcrc",
                               f"--rc-host=127.0.0.1:{15000 + round_id}", report["mediaPath"]]),
                "initialConfig": "fresh", "configPath": f"{directory}/profile/config",
                "childEnvironment": {key: f"{directory}/profile/{key}"
                                     for key in ("APPDATA", "LOCALAPPDATA", "TEMP", "TMP")},
                "videoCanvas": {"clientWidth": 1280, "clientHeight": 720, "dpi": 96},
                "processToVisibleWindowMs": 100 + offset,
                "samples": [{"elapsedSeconds": 10 + i * 5, "cpuSeconds": 1 + i * 5 * cpu_percent / 100,
                             "visibleCanvasBefore": visible_fixture(), "visibleCanvasAfter": visible_fixture(),
                             "cpuCycles": 1000 + i * cpu_percent * 1000,
                             "privateBytes": (100 + offset * 10) * MIB,
                             "workingSetBytes": (90 + offset * 10) * MIB,
                             "dedicatedGpuBytes": (80 + offset * 10) * MIB,
                             "sharedGpuBytes": 5 * MIB,
                             "playback": {"positionSec": 10 + i * 5,
                                          "state": 2 if name == "Aperture" else None, "reason": None}}
                            for i in range(7)]})
    return report


class RivalTests(unittest.TestCase):
    def setUp(self):
        self.report = fixture()
        self.run = self.report["results"][1]

    def reject(self, pattern):
        with self.assertRaisesRegex(ValueError, pattern):
            api.evaluate(self.report)

    def test_hub_entry_requires_consistent_owned_media_open(self):
        self.report["launchMode"] = "HubThenOpen"
        for run in self.report["results"]:
            run["arguments"].remove(self.report["mediaPath"])
            run.update(launchMode="HubThenOpen", openedMediaPath=self.report["mediaPath"],
                       mediaOpenRequestedMs=3000)
        self.assertTrue(api.evaluate(self.report)["qualifiedResourceEvidence"])
        self.run["launchMode"] = "Direct"
        self.reject("Mixed launch modes")
        self.run["launchMode"] = "HubThenOpen"
        self.run["openedMediaPath"] = "other.mp4"
        self.reject("Hub media differs")
        self.run["openedMediaPath"] = self.report["mediaPath"]
        self.run["mediaOpenRequestedMs"] = None
        self.reject("media open time")

    def test_known_endpoint_cpu_and_unit_normalization(self):
        result = api.evaluate(self.report)
        self.assertTrue(result["qualifiedResourceEvidence"])
        for actual, expected in zip(result["rows"]["cpu"]["candidate"]["values"], (6, 7, 8)):
            self.assertAlmostEqual(actual, expected)
        self.assertAlmostEqual(result["rows"]["capacity"]["candidate"]["median"], 7 / 24)
        self.assertAlmostEqual(result["rows"]["capacity"]["medianDifference"], -10 / 24)
        self.assertAlmostEqual(result["rows"]["cpu"]["relativeDifferencePercent"], (7 / 17 - 1) * 100)
        self.assertEqual(result["rows"]["capacity"]["differenceUnit"], "percentage points of machine capacity")
        self.assertEqual(result["rows"]["cpu"]["status"], "candidate-better")
        self.assertEqual(result["rows"]["cycles"]["unit"], "cycles/s")

    def test_visibility_must_hold_at_both_sample_guards(self):
        for key in ("visibleCanvasBefore", "visibleCanvasAfter"):
            with self.subTest(key=key):
                original = self.run["samples"][0].pop(key)
                self.reject("visibility guard")
                self.run["samples"][0][key] = original
                original["safe"] = False
                self.reject("visibility guard")
                original["safe"] = True
                original["screenCanvas"]["right"] += 1
                self.reject("Sampled canvas changed")
                original["screenCanvas"]["right"] -= 1

    def test_no_overall_win_from_all_resource_advantages(self):
        result = api.evaluate(self.report)
        self.assertFalse(result["overallWin"])
        for key in ("quality", "readiness", "workflows", "featureMatchedStorage"):
            self.assertEqual(result["rows"][key]["status"], "unproven")
        self.assertIn("DiagnosticOnly", next(k for k in result if k.startswith("shell")))

    def test_full_window_delta_not_average_of_unequal_intervals(self):
        for run in self.report["results"]:
            for i, sample in enumerate(run["samples"]):
                sample["elapsedSeconds"] = 10 + [0, 4, 10.5, 14.5, 20, 26, 30][i]
                sample["playback"]["positionSec"] = sample["elapsedSeconds"]
                sample["cpuSeconds"] = [1, 1.5, 1.6, 1.7, 1.8, 1.9, 2.5][i]
        self.assertEqual(api.evaluate(self.report)["rows"]["cpu"]["candidate"]["median"], 5)

    def test_memory_uses_median_per_run_not_pooled_snapshots(self):
        for run in self.report["results"]:
            if run["player"] == "Aperture" and not run["warmup"]:
                values = [10] * 4 + [1000] * 3 if run["round"] < 3 else [1000] * 7
                for sample, value in zip(run["samples"], values):
                    sample["privateBytes"] = value * MIB
        self.assertEqual(api.evaluate(self.report)["rows"]["private"]["candidate"]["median"], 10)

    def test_zero_ram_cannot_be_a_memory_win(self):
        for sample in self.run["samples"]:
            sample.update(privateBytes=0, workingSetBytes=0)
        result = api.evaluate(self.report)
        for key in ("private", "workingSet"):
            self.assertEqual(result["rows"][key]["status"], "unproven")

    def test_controlled_visible_seek_is_not_resource_evidence(self):
        self.run.update(status="sentinel-validated", samples=[],
                        playbackVerification="visible-sentinel-and-seek-verified")
        self.reject("failed|unverified")

    def test_equal_count_with_clustered_counter_snapshots_is_rejected(self):
        for sample, time in zip(self.run["samples"], (10, 10.1, 10.2, 10.3, 10.4, 39.9, 40)):
            sample["elapsedSeconds"] = time
            sample["playback"]["positionSec"] = time
        self.reject("counter-sampling schedule")

    def test_overlap_and_touching_ranges_cannot_win(self):
        for advantage in (1, 2):
            report = fixture()
            for run in report["results"]:
                percent = run["round"] + (advantage if run["player"] == "VLC" else 0)
                for i, sample in enumerate(run["samples"]):
                    sample["cpuSeconds"] = 1 + i * 5 * percent / 100
            self.assertEqual(api.evaluate(report)["rows"]["cpu"]["status"],
                             "inconclusive-overlapping-ranges")

    def test_failed_or_unverified_runs_are_rejected_not_dropped(self):
        for field, value in (("failure", "crash"), ("status", "failed"),
                             ("playbackVerification", "playback-unverified")):
            with self.subTest(field=field):
                report = fixture()
                report["results"][1][field] = value
                with self.assertRaisesRegex(ValueError, "failed or unverified"):
                    api.evaluate(report)

    def test_failed_warmup_is_not_silently_excluded(self):
        self.report["results"][0]["status"] = "failed"
        self.reject("failed or unverified")

    def test_round_omission_duplication_and_extra_are_rejected(self):
        for action in ("omit", "duplicate", "extra"):
            with self.subTest(action=action):
                report = fixture()
                if action == "omit":
                    del report["results"][1]
                else:
                    extra = copy.deepcopy(report["results"][1])
                    extra["round"] = 4 if action == "extra" else 1
                    report["results"].append(extra)
                with self.assertRaisesRegex(ValueError, "rounds"):
                    api.evaluate(report)

    def test_warmup_flag_cannot_hide_a_measured_run(self):
        self.run["warmup"] = True
        self.reject("Warmup flags")

    def test_snapshots_cannot_substitute_for_repeat_runs(self):
        self.report["iterations"] = 1
        self.reject("Insufficient measured rounds")
        with self.assertRaisesRegex(ValueError, "At least two"):
            api.evaluate(fixture(), min_runs=1)

    def test_mixed_runtime_including_same_exe_changed_dll_is_rejected(self):
        for field in ("executableHash", "runtimeManifestSha256", "version", "runtimeBytes"):
            with self.subTest(field=field):
                report = fixture()
                report["results"][1][field] = "f" * 64 if "Hash" in field or "Sha256" in field else 999
                with self.assertRaisesRegex(ValueError, "Mixed runtime"):
                    api.evaluate(report)

    def test_missing_full_runtime_hash_cannot_qualify(self):
        del self.report["players"][0]["runtimeManifestSha256"]
        self.reject("runtime manifest SHA256")

    def test_changed_configuration_or_experimental_environment_is_rejected(self):
        for field in ("arguments", "initialConfig", "childEnvironment"):
            with self.subTest(field=field):
                report = fixture()
                run = report["results"][1]
                if field == "arguments":
                    run[field].insert(0, "--no-audio")
                elif field == "initialConfig":
                    run[field] = "hwdec=no"
                else:
                    run[field]["APERTURE_TEST_ROUND_WAITS"] = "floor"
                with self.assertRaisesRegex(ValueError, "mixed launch configurations"):
                    api.evaluate(report)

    def test_per_run_owned_directories_and_rc_ports_are_normalized(self):
        self.assertTrue(api.evaluate(self.report)["qualifiedResourceEvidence"])

    def test_profile_redirection_must_be_owned(self):
        self.run["childEnvironment"]["APPDATA"] = "C:/Users/user/AppData/Roaming"
        self.reject("Unisolated child APPDATA")

    def test_canvas_dpi_or_dimensions_cannot_be_mixed(self):
        for field, pattern in (("dpi", "DPI"), ("clientHeight", "canvas differs")):
            with self.subTest(field=field):
                report = fixture()
                report["results"][1]["videoCanvas"][field] = 144
                with self.assertRaisesRegex(ValueError, pattern):
                    api.evaluate(report)

    def test_media_argument_hash_and_environment_cannot_be_mixed(self):
        for field in ("arguments", "mediaSha256", "environment"):
            with self.subTest(field=field):
                report = fixture()
                report["results"][1][field] = {
                    "arguments": ["C:/fixtures/other.mp4"], "mediaSha256": "f" * 64,
                    "environment": {"logicalProcessors": 8}}[field]
                with self.assertRaisesRegex(ValueError, "media|environment"):
                    api.evaluate(report)

    def test_unknown_environment_and_fake_processor_count_are_rejected(self):
        del self.report["environment"]
        self.reject("environment identity")
        for processors in (0, True, 24.5):
            report = fixture()
            report["environment"]["logicalProcessors"] = processors
            with self.assertRaises(ValueError):
                api.evaluate(report)

    def test_raw_status_overrules_forged_verified_summary(self):
        for field, value in (("positionSec", 10), ("state", 1), ("reason", "probe failed")):
            report = fixture()
            report["results"][1]["samples"][3]["playback"][field] = value
            with self.assertRaisesRegex(ValueError, "Nonadvancing|nonplaying"):
                api.evaluate(report)

    def test_decreasing_counters_or_time_are_rejected(self):
        for field in ("elapsedSeconds", "cpuSeconds", "cpuCycles"):
            report = fixture()
            report["results"][1]["samples"][3][field] = 0
            with self.assertRaisesRegex(ValueError, "Nonadvancing|decreased"):
                api.evaluate(report)

    def test_nonfinite_counters_are_not_admitted(self):
        for value in (float("nan"), float("inf"), -1, True):
            self.run["samples"][0]["cpuSeconds"] = value
            self.reject("CPU seconds")

    def test_short_or_unmatched_media_windows_are_rejected(self):
        for sample in self.run["samples"]:
            sample["elapsedSeconds"] = 10 + (sample["elapsedSeconds"] - 10) / 5
        self.reject("too short")
        self.report = fixture()
        for sample in self.report["results"][1]["samples"]:
            sample["playback"]["positionSec"] += 5
        self.reject("Unmatched sampled media segments")

    def test_missing_gpu_or_zero_sampled_cpu_is_unproven_never_zero_cost(self):
        for sample in self.run["samples"]:
            sample["dedicatedGpuBytes"] = None
            sample["cpuSeconds"] = 1
        result = api.evaluate(self.report)
        self.assertEqual(result["rows"]["dedicatedGpu"]["status"], "unproven")
        self.assertEqual(result["rows"]["cpu"]["status"], "unproven")

    def test_saved_real_report_is_not_upgraded_into_qualified_evidence(self):
        report = json.loads((ROOT / "docs/evidence/balanced-frame-resources-20261018/h264/comparison.json")
                            .read_text(encoding="utf-8-sig"))
        with self.assertRaisesRegex(ValueError, "environment identity"):
            api.evaluate(report, "Aperture-balanced-candidate", "VLC")

    def test_real_counter_schema_matches_original_independent_summaries(self):
        folder = ROOT / "docs/evidence/balanced-frame-resources-20261018"
        report = json.loads((folder / "h264/comparison.json").read_text(encoding="utf-8-sig"))
        # Synthetic metadata ONLY for parser coverage; never saved as measured evidence.
        report["environment"] = fixture()["environment"]
        for index, player in enumerate(report["players"]):
            player["runtimeManifestSha256"] = str(index) * 64
            for run in report["results"]:
                if (run.get("variant") or run["player"]) == (player.get("variant") or player["name"]):
                    run["runtimeManifestSha256"] = player["runtimeManifestSha256"]
                    for sample in run["samples"]:
                        sample.update(visibleCanvasBefore=visible_fixture(), visibleCanvasAfter=visible_fixture())
        result = api.evaluate(report, "Aperture-balanced-candidate", "VLC")
        original = json.loads((folder / "review.json").read_text(encoding="utf-8-sig"))["summary"]
        for side, name in (("candidate", "Aperture-balanced-candidate"), ("rival", "VLC")):
            for metric, source in (("cpu", "legacy-cpu-core"), ("private", "private-mib")):
                for stat in ("median", "min", "max", "runs"):
                    self.assertAlmostEqual(result["rows"][metric][side][stat], original[name][source][stat])

    def test_evaluation_is_read_only_and_does_not_trust_claimed_winner(self):
        self.report["overallWin"] = True
        before = copy.deepcopy(self.report)
        self.assertFalse(api.evaluate(self.report)["overallWin"])
        self.assertEqual(self.report, before)

    def test_cli_reports_rejection_and_never_overwrites_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, output = Path(temporary) / "input.json", Path(temporary) / "result.json"
            source.write_text(json.dumps(self.report), encoding="utf-8")
            command = [sys.executable, str(SCRIPT), str(source), "--output", str(output)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            saved = output.read_bytes()
            self.assertFalse(json.loads(saved)["overallWin"])
            self.assertEqual(len(json.loads(saved)["sourceReport"]["sha256"]), 64)
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), saved)
            source.write_text("{}", encoding="utf-8")
            result = subprocess.run(command[:-2], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertFalse(json.loads(result.stdout)["qualifiedResourceEvidence"])


class CalibrationTests(unittest.TestCase):
    def setUp(self):
        self.report = fixture()
        self.environment = self.report["environment"]
        self.environment["lastBootUpTimeUtc"] = "2026-10-20T08:00:00Z"
        self.raw = {"supported": True, "invariant_tsc": True, "cpu": "CPU fixture",
                    "logical_processors": 24, "qpc_frequency": 10000000,
                    "tsc_ticks_per_second": 4000000000, "relative_rate_spread": 0,
                    "busy_wall_seconds": .25, "thread_busy_ratio": .99, "process_busy_ratio": .99,
                    "thread_cycles": 990000000, "process_cycles": 990000000, "uptime_ms": 3600000,
                    "samples": [{"tsc_delta": 1000000000, "qpc_delta": 2500000,
                                 "ticks_per_second": 4000000000} for _ in range(5)]}
        self.attestation = {"recordedAtUtc": "2026-10-20T09:00:01Z", "environment": copy.deepcopy(self.environment),
                            "sourceSha256": "1" * 64, "executableSha256": "2" * 64, "exitCode": 0}

    def evaluate(self):
        with tempfile.TemporaryDirectory() as folder:
            raw, identity = Path(folder) / "raw.json", Path(folder) / "identity.json"
            raw.write_text(json.dumps(self.raw), encoding="utf-8")
            self.attestation.setdefault("calibrationSha256", hashlib.sha256(raw.read_bytes()).hexdigest())
            identity.write_text(json.dumps(self.attestation), encoding="utf-8")
            return api.evaluate(self.report, calibration=raw, calibration_identity=identity)

    def test_calibration_preserves_raw_cpu_and_uses_verified_capacity(self):
        baseline = api.evaluate(self.report)
        result = self.evaluate()
        for key in ("cpu", "capacity", "cycles"):
            self.assertEqual(result["rows"][key], baseline["rows"][key])
        for side in ("candidate", "rival"):
            expected = baseline["rows"]["cycles"][side]["median"] / 4000000000 * 100
            self.assertAlmostEqual(result["rows"]["calibratedCpu"][side]["median"], expected)
            self.assertAlmostEqual(result["rows"]["calibratedCapacity"][side]["median"], expected / 24)
        self.assertFalse(result["overallWin"])
        self.assertEqual(result["rows"]["calibratedCpu"]["status"], "candidate-better")
        self.assertEqual(result["cycleCalibration"]["sourceSha256"], "1" * 64)

    def test_no_calibration_means_no_converted_cpu_claim(self):
        result = api.evaluate(self.report)
        self.assertNotIn("calibratedCpu", result["rows"])
        self.assertIsNone(result["cycleCalibration"])

    def test_both_input_files_are_required(self):
        for kwargs in ({"calibration": Path("missing")}, {"calibration_identity": Path("missing")}):
            with self.assertRaisesRegex(ValueError, "Supply both"):
                api.evaluate(self.report, **kwargs)

    def test_changed_raw_file_cannot_reuse_attestation(self):
        self.attestation["calibrationSha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "recorded hash"):
            self.evaluate()

    def test_rejects_wrong_hardware_driver_or_boot(self):
        for field in ("cpuName", "logicalProcessors", "lastBootUpTimeUtc", "osVersion", "gpuName", "gpuDriverVersion"):
            with self.subTest(field=field):
                original = self.attestation["environment"][field]
                self.attestation["environment"][field] = "different"
                with self.assertRaisesRegex(ValueError, "environment differs"):
                    self.evaluate()
                self.attestation["environment"][field] = original

    def test_native_cpu_or_count_cannot_disagree_with_sidecar(self):
        for field, value in (("cpu", "other CPU"), ("logical_processors", 12), ("logical_processors", True)):
            with self.subTest(field=field, value=value):
                original = self.raw[field]
                self.raw[field] = value
                with self.assertRaises(ValueError): self.evaluate()
                self.raw[field] = original
                self.attestation.pop("calibrationSha256", None)

    def test_bad_uptime_or_naive_timestamp_is_rejected(self):
        for time in ("2026-10-20T09:10:01Z", "2026-10-20T09:00:01", None):
            self.attestation["recordedAtUtc"] = time
            with self.assertRaisesRegex(ValueError, "uptime|UTC"):
                self.evaluate()

    def test_failure_or_noninvariant_counter_cannot_be_calibrated(self):
        for field in ("supported", "invariant_tsc"):
            self.raw[field] = False
            with self.assertRaisesRegex(ValueError, "Unsupported"):
                self.evaluate()
            self.raw[field] = True
            self.attestation.pop("calibrationSha256", None)
        self.attestation["exitCode"] = 4
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            self.evaluate()

    def test_rate_must_be_recomputed_from_raw_deltas(self):
        self.raw["samples"][2]["ticks_per_second"] += 1000000
        with self.assertRaisesRegex(ValueError, "raw deltas"):
            self.evaluate()

    def test_stable_claim_cannot_hide_unstable_samples(self):
        self.raw["samples"][0].update(tsc_delta=1100000000, ticks_per_second=4400000000)
        self.raw["relative_rate_spread"] = .1
        with self.assertRaisesRegex(ValueError, "Unstable"):
            self.evaluate()

    def test_sample_count_duration_and_finite_counters_are_validated(self):
        for samples in ([], self.raw["samples"][:4],
                        [{"tsc_delta": 4000000, "qpc_delta": 10000, "ticks_per_second": 4000000000}] * 5,
                        [{"tsc_delta": float("inf"), "qpc_delta": 2500000, "ticks_per_second": 4000000000}] * 5):
            self.raw["samples"] = samples
            with self.assertRaises(ValueError): self.evaluate()
            self.attestation.pop("calibrationSha256", None)

    def test_busy_check_must_agree_with_real_counter_deltas(self):
        for field, value in (("thread_busy_ratio", 1), ("process_cycles", 1000), ("busy_wall_seconds", .01)):
            original = self.raw[field]
            self.raw[field] = value
            with self.assertRaisesRegex(ValueError, "busy"):
                self.evaluate()
            self.raw[field] = original
            self.attestation.pop("calibrationSha256", None)

    def test_attestation_requires_source_and_binary_identity(self):
        for field in ("sourceSha256", "executableSha256"):
            original = self.attestation.pop(field)
            with self.assertRaisesRegex(ValueError, "SHA|Sha"):
                self.evaluate()
            self.attestation[field] = original


if __name__ == "__main__":
    unittest.main()
