"""Qualify compare_players.ps1 evidence; resource observations never prove overall superiority."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
from statistics import median


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value, label, *, integer=False):
    require(type(value) in (int, float) and math.isfinite(value) and value >= 0,
            f"Invalid {label}")
    require(not integer or type(value) is int, f"Invalid integer {label}")
    return value


def sha(value, label):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-fA-F]{64}", value),
            f"Missing/invalid {label}")
    return value.lower()


def identity(player):
    return player.get("variant") or player.get("name")


def configuration(run, media, launch_mode):
    """Ignore only harness-owned per-run directories and the owned RC port."""
    directory = run.get("runDirectory")
    require(isinstance(directory, str) and directory, "Missing run directory")
    directory = directory.replace("\\", "/").rstrip("/").casefold()

    def normalized(value):
        require(isinstance(value, str), "Non-string launch configuration")
        path = value.replace("\\", "/")
        if path.casefold().startswith(directory + "/"):
            return "<run>/" + path[len(directory) + 1:]
        return value

    args = run.get("arguments")
    require(isinstance(args, list), "Missing launch arguments")
    require(run.get("launchMode", "Direct") == launch_mode, "Mixed launch modes")
    if launch_mode == "Direct":
        require(media in args, "Run media differs from batch media")
    else:
        require(media not in args and run.get("openedMediaPath") == media,
                "Hub media differs from batch media")
        require(number(run.get("mediaOpenRequestedMs"), "media open time") > 0,
                "Missing hub media open time")
    normalized_args = []
    for arg in args:
        require(isinstance(arg, str), "Non-string launch argument")
        if arg.startswith("--config="):
            arg = "--config=" + normalized(arg[len("--config="):])
        elif re.fullmatch(r"--rc-host=127\.0\.0\.1:[0-9]+", arg):
            arg = "--rc-host=127.0.0.1:<owned-port>"
        normalized_args.append(arg)
    env = run.get("childEnvironment")
    require(isinstance(env, dict), "Missing child environment")
    for key in ("APPDATA", "LOCALAPPDATA", "TEMP", "TMP"):
        require(isinstance(env.get(key), str) and normalized(env[key]).startswith("<run>/"),
                f"Unisolated child {key}")
    require(isinstance(run.get("initialConfig"), str), "Missing initial configuration")
    return (normalized_args, run["initialConfig"],
            {key: normalized(value) for key, value in env.items()},
            normalized(run.get("configPath")))


def summarize(values):
    return {"median": median(values), "min": min(values), "max": max(values),
            "runs": len(values), "values": values}


def comparison(label, unit, groups, metric):
    if any(any(value is None for value in group[metric]) for group in groups):
        return {"label": label, "unit": unit, "status": "unproven",
                "reason": "Counters unavailable in at least one measured run; no run was discarded."}
    a, b = [summarize(group[metric]) for group in groups]
    if a["max"] < b["min"]:
        status = "candidate-better"
    elif b["max"] < a["min"]:
        status = "rival-better"
    elif a["min"] == a["max"] == b["min"] == b["max"]:
        status = "equal-observed-values"
    else:
        status = "inconclusive-overlapping-ranges"
    return {"label": label, "unit": unit, "status": status, "candidate": a, "rival": b,
            "medianDifference": a["median"] - b["median"],
            "differenceUnit": unit.replace("% of", "percentage points of"),
            "relativeDifferencePercent": ((a["median"] / b["median"] - 1) * 100
                                          if b["median"] else None)}


def evaluate(report, candidate="Aperture", rival="VLC", min_runs=3, min_seconds=25):
    require(type(min_runs) is int and min_runs >= 2, "At least two measured runs are required")
    require(number(min_seconds, "minimum seconds") > 0, "Minimum seconds must be positive")
    require(isinstance(report, dict) and type(report.get("schemaVersion")) is int and report["schemaVersion"] == 1,
            "Unsupported compare_players report schema")
    media_hash = sha(report.get("mediaSha256"), "media SHA256")
    require(number(report.get("mediaBytes"), "media bytes", integer=True) > 0, "Empty media")
    media = report.get("mediaPath")
    require(isinstance(media, str) and media, "Missing media path")
    launch_mode = report.get("launchMode", "Direct")
    require(launch_mode in ("Direct", "HubThenOpen"), "Unsupported launch mode")
    environment = report.get("environment")
    require(isinstance(environment, dict), "Missing recorded environment identity")
    for field in ("osVersion", "cpuName", "gpuName", "gpuDriverVersion"):
        require(isinstance(environment.get(field), str) and environment[field].strip(),
                f"Missing environment {field}")
    processors = number(environment.get("logicalProcessors"), "logical processors", integer=True)
    require(processors > 0, "Logical processor count must be positive")
    iterations = number(report.get("iterations"), "iterations", integer=True)
    warmups = number(report.get("warmupIterations"), "warmups", integer=True)
    require(iterations >= min_runs, "Insufficient measured rounds; samples are not independent runs")
    canvas = report.get("videoCanvasTarget")
    require(isinstance(canvas, list) and len(canvas) == 2 and
            all(type(x) is int and x > 0 for x in canvas), "Missing actual video-canvas target")
    players, results = report.get("players"), report.get("results")
    require(isinstance(players, list) and isinstance(results, list), "Missing players/results")
    require(all(isinstance(p, dict) for p in players + results), "Invalid player/run record")
    names = [identity(player) for player in players]
    require(all(isinstance(name, str) and name for name in names), "Missing player identities")
    require(len(names) == len(set(names)), "Duplicate player identities")
    require(candidate != rival and candidate in names and rival in names,
            "Select distinct exact candidate/rival variant identities")
    require(all(identity({"name": r.get("player"), "variant": r.get("variant")}) in names
                for r in results), "Unknown player run")
    grouped, dpi, segments, sample_counts, sources = [], set(), [], set(), {}
    for selected in (candidate, rival):
        player = players[names.index(selected)]
        executable = sha(player.get("exeHash"), f"{selected} EXE SHA256")
        runtime = sha(player.get("runtimeManifestSha256"), f"{selected} runtime manifest SHA256")
        require(isinstance(player.get("version"), str) and player["version"], "Missing runtime version")
        runtime_bytes = number(player.get("runtimeBytes"), "runtime bytes", integer=True)
        file_count = number(player.get("runtimeFileCount"), "runtime files", integer=True)
        require(runtime_bytes > 0 and file_count > 0, "Empty runtime")
        runs = [r for r in results if (r.get("variant") or r.get("player")) == selected]
        rounds = [r.get("round") for r in runs]
        require(all(type(r) is int for r in rounds) and
                sorted(rounds) == list(range(warmups + iterations)),
                f"{selected}: missing, duplicated or extra rounds")
        runs.sort(key=lambda run: run["round"])
        config = None
        group = {key: [] for key in ("cpu", "capacity", "cycles", "private", "workingSet",
                                     "dedicatedGpu", "sharedGpu", "runtime", "seconds", "shell")}
        for run in runs:
            require(run.get("player") == player.get("name"), "Mixed player identity")
            require(type(run.get("warmup")) is bool and run["warmup"] == (run["round"] < warmups),
                    "Warmup flags do not match declared rounds")
            require(run.get("status") == "sampled" and not run.get("failure") and
                    run.get("playbackVerification") == "advancing-playback-verified" and
                    not run.get("playbackReason"), f"{selected}: failed or unverified run")
            require(sha(run.get("executableHash"), "run EXE SHA256") == executable and
                    sha(run.get("runtimeManifestSha256"), "run runtime manifest SHA256") == runtime and
                    run.get("runtimeBytes") == runtime_bytes and run.get("runtimeFileCount") == file_count and
                    run.get("version") == player.get("version"), "Mixed runtime identity")
            require(sha(run.get("mediaSha256", media_hash), "run media SHA256") == media_hash and
                    run.get("mediaBytes", report["mediaBytes"]) == report["mediaBytes"] and
                    run.get("environment", environment) == environment, "Mixed media/environment")
            signature = configuration(run, media, launch_mode)
            require(config is None or signature == config, f"{selected}: mixed launch configurations")
            config = signature
            viewport = run.get("videoCanvas") or {}
            require(isinstance(viewport, dict), "Invalid video canvas")
            require([viewport.get("clientWidth"), viewport.get("clientHeight")] == canvas,
                    "Actual canvas differs from target")
            dpi.add(number(viewport.get("dpi"), "canvas DPI", integer=True))
            samples = run.get("samples")
            require(isinstance(samples, list) and len(samples) >= 3, "Missing raw counter snapshots")
            times, cpu, cycles, positions = [], [], [], []
            for sample in samples:
                require(isinstance(sample, dict), "Invalid sample record")
                for key in ("visibleCanvasBefore", "visibleCanvasAfter"):
                    visible = sample.get(key)
                    require(isinstance(visible, dict) and visible.get("safe") is True and
                            not visible.get("reason"), "Missing/rejected sampled visibility guard")
                    rect = visible.get("screenCanvas") or {}
                    require(isinstance(rect, dict) and
                            all(type(rect.get(edge)) is int for edge in ("left", "right", "top", "bottom")),
                            "Invalid visible canvas bounds")
                    require([rect["right"] - rect["left"], rect["bottom"] - rect["top"]] == canvas,
                            "Sampled canvas changed")
                    require(visible.get("dpi") == viewport["dpi"], "Sampled canvas DPI changed")
                times.append(number(sample.get("elapsedSeconds"), "sample time"))
                cpu.append(number(sample.get("cpuSeconds"), "CPU seconds"))
                cycles.append(number(sample.get("cpuCycles"), "CPU cycles", integer=True))
                playback = sample.get("playback") or {}
                require(isinstance(playback, dict), "Invalid playback status")
                positions.append(number(playback.get("positionSec"), "media position"))
                require(not playback.get("reason") and playback.get("state") in (None, 2) and
                        (player.get("name") != "Aperture" or playback.get("state") == 2),
                        "Invalid/nonplaying raw status")
            require(all(y > x for x, y in zip(times, times[1:])) and
                    all(y > x for x, y in zip(positions, positions[1:])), "Nonadvancing time/media")
            require(all(y >= x for values in (cpu, cycles) for x, y in zip(values, values[1:])),
                    "CPU counter decreased")
            elapsed = times[-1] - times[0]
            require(elapsed >= min_seconds, "CPU window is too short")
            require(all(abs((time - times[0]) - elapsed * i / (len(times) - 1)) <= 1.1
                        for i, time in enumerate(times)), "Clustered/irregular counter-sampling schedule")
            require(abs((positions[-1] - positions[0]) - elapsed) <= max(1.1, elapsed * .05),
                    "Media did not advance at the measured playback rate")
            if run["warmup"]:
                continue
            segments.append((positions[0], positions[-1]))
            sample_counts.add(len(samples))
            group["seconds"].append(elapsed)
            rate = (cpu[-1] - cpu[0]) / elapsed * 100
            require(rate <= processors * 100 * 1.01, "Impossible CPU-time rate")
            group["cpu"].append(rate if rate > 0 else None)
            group["capacity"].append(rate / processors if rate > 0 else None)
            group["cycles"].append((cycles[-1] - cycles[0]) / elapsed if cycles[-1] > cycles[0] else None)
            for key, counter in (("private", "privateBytes"), ("workingSet", "workingSetBytes"),
                                 ("dedicatedGpu", "dedicatedGpuBytes"), ("sharedGpu", "sharedGpuBytes")):
                values = [s.get(counter) for s in samples]
                if any(v is None for v in values) or (key.endswith("Gpu") and
                        any(s.get("gpuUnavailableReason") for s in samples)) or (
                        key in ("private", "workingSet") and any(v == 0 for v in values)):
                    group[key].append(None)
                else:
                    group[key].append(median(number(v, counter, integer=True) for v in values) / 1048576)
            group["runtime"].append(runtime_bytes / 1048576)
            group["shell"].append(number(run.get("processToVisibleWindowMs"), "visible shell time"))
        sources[selected] = {"version": player["version"], "executableSha256": executable,
                             "runtimeManifestSha256": runtime,
                             "launchConfigurationSha256": hashlib.sha256(json.dumps(
                                 config, sort_keys=True, ensure_ascii=False).encode("utf-8")).hexdigest()}
        grouped.append(group)
    require(len(dpi) == 1 and next(iter(dpi)) > 0, "Mixed/invalid canvas DPI")
    require(len(sample_counts) == 1, "Mixed counter-sampling schedules")
    require(all(max(s[i] for s in segments) - min(s[i] for s in segments) <= 1.1
                for i in (0, 1)), "Unmatched sampled media segments")
    rows = {key: comparison(label, unit, grouped, key) for key, label, unit in (
        ("cpu", "Sampled CPU time / wall time", "% of one logical processor"),
        ("capacity", "Sampled CPU / verified machine capacity", "% of machine capacity"),
        ("cycles", "Raw process cycles / wall time (not CPU-time percent)", "cycles/s"),
        ("private", "Private committed RAM", "MiB"), ("workingSet", "Resident working set", "MiB"),
        ("dedicatedGpu", "Dedicated GPU allocation (not utilization)", "MiB"),
        ("sharedGpu", "Shared GPU allocation", "MiB"),
        ("runtime", "Complete supplied runtime (feature inventories may differ)", "MiB"))}
    for key, label in (("quality", "Output quality and presentation cadence"),
                       ("readiness", "First visible video and seek readiness"),
                       ("workflows", "Core workflows and reliability"),
                       ("featureMatchedStorage", "Feature-matched distribution storage")):
        rows[key] = {"label": label, "status": "unproven",
                     "reason": "Not established by compare_players resource/status counters."}
    return {"schemaVersion": 1, "qualifiedResourceEvidence": True, "overallWin": False,
            "launchMode": launch_mode,
            "overallStatus": "unproven", "candidate": candidate, "rival": rival,
            "runtimeIdentities": sources,
            "qualificationThresholds": {"minimumMeasuredRuns": min_runs,
                                        "minimumCpuWindowSeconds": min_seconds,
                                        "matchedPositionToleranceSeconds": 1.1},
            "environment": environment, "mediaSha256": media_hash, "videoCanvas": canvas,
            "measuredRunsPerPlayer": iterations, "discardedWarmupsPerPlayer": warmups,
            "samplingSeconds": [summarize(g["seconds"]) for g in grouped], "rows": rows,
            "shellVisibleMsDiagnosticOnly": [summarize(g["shell"]) for g in grouped],
            "limits": ["Run ranges are descriptive, not confidence intervals or causal attribution.",
                       "Overlapping ranges cannot establish a winner; snapshots are not independent runs.",
                       "CPU uses each complete endpoint delta; RAM uses a median per run, then across runs.",
                       "Machine CPU differences are percentage points; relative percent is separate.",
                       "Cycles/s is not converted to time using CPU MHz or an unverified calibration.",
                       "Root-process counters exclude shared services; memory accounts must not be added."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--candidate", default="Aperture")
    parser.add_argument("--rival", default="VLC")
    parser.add_argument("--min-runs", type=int, default=3)
    parser.add_argument("--min-seconds", type=float, default=25)
    parser.add_argument("--output", type=Path, help="New JSON evidence file; existing files are never replaced")
    args = parser.parse_args()
    try:
        source = args.report.read_bytes()
        result = evaluate(json.loads(source.decode("utf-8-sig")),
                          args.candidate, args.rival, args.min_runs, args.min_seconds)
        result["sourceReport"] = {"path": str(args.report.resolve()),
                                  "sha256": hashlib.sha256(source).hexdigest()}
        code = 0
    except (OSError, ValueError, TypeError, KeyError, OverflowError) as error:
        result = {"qualifiedResourceEvidence": False, "overallWin": False,
                  "overallStatus": "unproven", "invalidReason": str(error)}
        code = 2
    rendered = json.dumps(result, indent=2, allow_nan=False) + "\n"
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(rendered)
    else:
        print(rendered, end="")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
