"""Recompute the saved comparison figures; no app launches or source-data edits."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import statistics

ROOT = Path(__file__).resolve().parents[2]
SOURCES = {
    "oct14": "docs/evidence/player-comparison-20261014/comparison.json",
    "oct17-before": "docs/evidence/backend-balance-20261017/app-before.json",
    "oct17-after": "docs/evidence/backend-balance-20261017/app-after.json",
}


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def spread(values):
    return {"median": statistics.median(values), "min": min(values), "max": max(values)}


def run_cpu(run):
    first, last = run["samples"][0], run["samples"][-1]
    elapsed = last["elapsedSeconds"] - first["elapsedSeconds"]
    cpu = last["cpuSeconds"] - first["cpuSeconds"]
    if elapsed <= 0 or cpu < 0:
        raise ValueError("Invalid CPU sample interval")
    return cpu / elapsed * 100, elapsed


def summarize(report, processors):
    result = {}
    for name in sorted({r["player"] for r in report["results"]}):
        rows = [r for r in report["results"] if r["player"] == name and not r["warmup"]]
        if any(r["status"] != "sampled" or r["playbackVerification"] != "advancing-playback-verified" for r in rows):
            raise ValueError("An unsuccessful/unverified run needs review, not silent exclusion")
        cpu = [run_cpu(r)[0] for r in rows]
        result[name] = {
            "runs": len(rows),
            "windowMs": spread([r["processToVisibleWindowMs"] for r in rows]),
            "cpuOneCorePercent": spread(cpu),
            "cpuMachineCapacityPercent": spread([value / processors for value in cpu]),
            "cpuObservationSeconds": spread([run_cpu(r)[1] for r in rows]),
            "firstSamplePlaybackSeconds": spread([r["samples"][0]["playback"]["positionSec"] for r in rows]),
            "lastSamplePlaybackSeconds": spread([r["samples"][-1]["playback"]["positionSec"] for r in rows]),
            "runtimeMiB": rows[0]["runtimeBytes"] / 2**20,
        }
        for field, label in [("privateBytes", "privateMiB"), ("workingSetBytes", "workingSetMiB"),
                             ("dedicatedGpuBytes", "dedicatedGpuMiB"), ("sharedGpuBytes", "sharedGpuMiB")]:
            result[name][label] = spread([statistics.median(s[field] for s in r["samples"]) / 2**20 for r in rows])
    return result


def replay_summary(replay):
    rows = [r for r in replay["results"] if not r["warmup"]]
    if any(r["exitCode"] != 0 for r in rows):
        raise ValueError("Startup replay contains an unsuccessful run")
    groups = {name: [r for r in rows if r["variant"] == name] for name in sorted({r["variant"] for r in rows})}
    result = {"method": replay["method"], "createdUtc": replay["createdUtc"], "variants": {}, "pairedComparisons": []}
    for name, runs in groups.items():
        result["variants"][name] = {"runs": len(runs), "windowMs": spread([r["firstVisibleMs"] for r in runs]),
                                    "observationIntervalMs": spread([r["observationIntervalMs"] for r in runs])}
    rng = random.Random(20261017)
    for before in ["oct14", "oct17-before"]:
        a = {r["round"]: r["firstVisibleMs"] for r in groups[before]}
        b = {r["round"]: r["firstVisibleMs"] for r in groups["oct17-after"]}
        if a.keys() != b.keys():
            raise ValueError("Unmatched replay rounds")
        changes = [b[i] - a[i] for i in a]
        boot = sorted(statistics.median(rng.choices(changes, k=len(changes))) for _ in range(20000))
        result["pairedComparisons"].append({"before": before, "after": "oct17-after", "pairedMedianDifferenceMs": statistics.median(changes),
                                            "bootstrap95PercentileMs": [boot[500], boot[19499]],
                                            "interpretation": "Within-round resampling, descriptive only; not a code-level causal attribution or hard latency bound."})
    return result


def audit(replay):
    hardware = load(ROOT / "docs/evidence/player-comparison-20261014/hardware.json")
    machine = load(ROOT / "docs/evidence/backend-balance-20261017/machine.json")
    processors = hardware["logicalProcessors"]
    if processors != machine["cpu"][0]["NumberOfLogicalProcessors"]:
        raise ValueError("Different processor counts require separate normalization")
    reports = {name: load(ROOT / file) for name, file in SOURCES.items()}
    if len({r["mediaSha256"].lower() for r in reports.values()}) != 1:
        raise ValueError("Playback fixture hashes differ")
    sessions = {name: summarize(report, processors) for name, report in reports.items()}
    aperture, vlc = sessions["oct17-after"]["Aperture"], sessions["oct17-after"]["VLC"]
    return {
        "logicalProcessors": processors,
        "sources": {file: hashlib.sha256((ROOT / file).read_bytes()).hexdigest() for file in SOURCES.values()},
        "sessions": sessions,
        "currentCpuComparison": {
            "ratio": aperture["cpuOneCorePercent"]["median"] / vlc["cpuOneCorePercent"]["median"],
            "relativeIncreasePercent": (aperture["cpuOneCorePercent"]["median"] / vlc["cpuOneCorePercent"]["median"] - 1) * 100,
            "machineCapacityPercentagePointDifference": aperture["cpuMachineCapacityPercent"]["median"] - vlc["cpuMachineCapacityPercent"]["median"],
        },
        "startupReplay": replay_summary(replay),
        "findings": [
            "Legacy first-visible-window observations use requested 20 ms polling and only three measured launches per build. They are not first-frame measurements or proof of a code regression.",
            "Aperture shows its bootstrap shell before app.Initialize in src/main.cpp. A visible top-level window does not establish equivalent completed playback initialization across players; the large VLC window-opening delta is not a first-video speedup.",
            "Fresh-copy replay of all three saved EXEs used 15 measured launches each, rotated/reversed order and tighter recorded observation intervals. The historical ~30 ms slowdown did not reproduce consistently. Original EXE hashes match; Oct14 DLL identity has the retained-runtime limitation recorded in replay inputs.",
            "CPU is cumulative root-process CPU-time delta divided by wall time, percent of one logical processor. Dividing by 24 gives total machine capacity, approximately comparable with a whole-machine utilization display, not an exact promise about every Task Manager counter.",
            "The +501% ratio is arithmetically correct but corresponds to approximately 0.92% versus 0.15% of total CPU capacity, a difference of approximately 0.77 percentage points.",
            "Playback comparisons share a generated 60-second 1080p30 H.264/yuv420p clip without audio/subtitles, 1280x800 outer windows, warm OS caches, fresh portable profiles and verified advancing positions. They do not establish equal decoders, video viewport, picture quality or presentation cadence. Root counters exclude harness, DWM, drivers and unexpected child-process work.",
            "CPU rates span only about six seconds per run after about ten seconds of settling, with three measured runs and control-interface overhead. Short runs and substantial ranges limit precise sustained-use claims. The Oct17 baseline block was not interleaved with the candidate block.",
            "The same file was not sampled at aligned media positions: final Aperture starts near 10.6 s and ends near 16.77 s, while VLC starts near 7 s and ends near 13 s. Different startup readiness and control overhead produce different observed segments. Advancing positions do not verify equal presented frames, quality or frame drops.",
            "The display-vdrop to audio-clock change is supported directionally by controlled engine-only tests. Those are separate from the VLC full-app comparison. The no-audio engine series contains a 0.000% CPU sample without saved endpoint CPU ticks; it must remain flagged, not presented as verified zero-cost playback.",
            "Private bytes, working set and GPU counters are distinct overlapping accounting categories. Recorded private-memory/GPU gaps are local configuration results; no stack/allocation trace identifies the remaining component-level cause. Do not sum them or label them exclusive physical memory.",
            "Runtime size compares complete supplied distributions, including VLC plugins/languages/tools, against Aperture's supplied payload. It is not feature-matched efficiency, download size or memory usage.",
        ],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--startup-replay", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = audit(load(args.startup_replay))
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(f"Audited three historical sessions and {sum(v['runs'] for v in result['startupReplay']['variants'].values())} startup replays.")
