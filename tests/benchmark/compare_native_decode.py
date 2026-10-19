"""Compare CPU-only decode probes; never launches a media player or window."""
import argparse
import csv
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def no_competing_benchmarks():
    if os.name != "nt":
        return
    process = subprocess.run(["tasklist.exe", "/FO", "CSV", "/NH"],
                             text=True, capture_output=True, check=True,
                             creationflags=subprocess.CREATE_NO_WINDOW)
    blocked = {"aperture.exe", "vlc.exe", "mpv_playback_test.exe", "gcc.exe",
               "cc1.exe", "cc1plus.exe", "lto1.exe", "ninja.exe", "make.exe"}
    active = sorted({r[0] for r in csv.reader(process.stdout.splitlines())
                     if r and r[0].lower() in blocked})
    if active:
        raise RuntimeError("Timing deferred while these processes are active: " + ", ".join(active))


def summarize(runs):
    result = {}
    for variant in ("baseline", "candidate"):
        selected = [r["result"] for r in runs if r["variant"] == variant and not r["warmup"]]
        result[variant] = {}
        for field in ("elapsedSeconds", "processCpuSeconds", "processCycles"):
            values = [r[field] for r in selected]
            result[variant][field] = {"median": statistics.median(values),
                                      "min": min(values), "max": max(values),
                                      "runs": len(values)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--iterations", type=int, default=5)
    args = parser.parse_args()
    if not 1 <= args.iterations <= 20:
        parser.error("iterations must be 1..20")
    probes = {k: getattr(args, k).resolve(strict=True) for k in ("baseline", "candidate")}
    cases = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
    if not isinstance(cases, list) or not cases:
        parser.error("manifest must contain at least one case")
    for case in cases:
        if (not isinstance(case.get("id"), str) or not case["id"].replace("-", "").isalnum()
                or case["kind"] not in ("video", "audio") or type(case["frames"]) is not int
                or not 1 <= case["frames"] <= 100000):
            parser.error("invalid case")
        case["path"] = str(Path(case["path"]).resolve(strict=True))
    if len({c["id"] for c in cases}) != len(cases):
        parser.error("case IDs must be unique")
    no_competing_benchmarks()
    args.out.mkdir(parents=True, exist_ok=False)
    report = {"createdUtc": now(), "status": "running",
              "scope": "CPU-only demux/software decode. No renderer, hardware decode, audio output or player windows. Not a whole-player benchmark.",
              "method": "One discarded process per variant/case, followed by rotated measured processes. Each process first hashes decoded output in a separate warmup, then initializes a fresh decoder and times fixed-count demux/decode/frame release. Open, stream discovery and codec initialization excluded. One decoder thread, warm file cache, no affinity change; raw process cycles and CPU time retained without converting to CPU utilization. Exact decoded buffers and frame metadata must match across both variants and all launches.",
              "environment": {"platform": platform.platform(), "machine": platform.machine(),
                              "logicalProcessors": os.cpu_count()},
              "probes": {k: {"path": str(p), "sha256": sha(p)} for k, p in probes.items()},
              "cases": []}

    def save():
        (args.out / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    save()
    try:
        for case in cases:
            row = {**case, "fixtureSha256": sha(case["path"]), "runs": [], "status": "running"}
            report["cases"].append(row)
            expected_output = None
            for round_number in range(args.iterations + 1):
                no_competing_benchmarks()
                order = ["baseline", "candidate"] if round_number % 2 == 0 else ["candidate", "baseline"]
                for variant in order:
                    no_competing_benchmarks()
                    directory = args.out / case["id"] / f"{round_number}-{variant}"
                    directory.mkdir(parents=True)
                    command = [str(probes[variant]), case["path"], case["kind"], str(case["frames"])]
                    run = {"variant": variant, "round": round_number, "warmup": round_number == 0,
                           "startedUtc": now(), "arguments": command}
                    process = subprocess.run(command, cwd=directory, capture_output=True, timeout=300,
                                             creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                    (directory / "stdout.txt").write_bytes(process.stdout)
                    (directory / "stderr.txt").write_bytes(process.stderr)
                    run.update(endedUtc=now(), exitCode=process.returncode)
                    row["runs"].append(run)
                    save()
                    if process.returncode:
                        raise RuntimeError(f"{case['id']}/{variant} failed; inspect retained raw logs")
                    result = json.loads(process.stdout)
                    run["result"] = result
                    if (result["frames"] != case["frames"] or result["threads"] != 1
                            or result["hardware"] or result["windowsCreated"] != 0
                            or result["elapsedSeconds"] <= 0 or result["processCycles"] <= 0):
                        raise RuntimeError("Invalid/incomplete decode result")
                    output = tuple(result[k] for k in ("decodedSha256", "decoder", "frames", "units", "packets", "avcodecVersion"))
                    expected_output = expected_output or output
                    if output != expected_output:
                        raise RuntimeError(f"Decoded output mismatch in {case['id']}/{variant}")
                    save()
            row["summary"] = summarize(row["runs"])
            row["status"] = "passed"
            save()
            baseline = row["summary"]["baseline"]["elapsedSeconds"]["median"]
            candidate = row["summary"]["candidate"]["elapsedSeconds"]["median"]
            print(f"{case['id']}: output matches; median decode time {(candidate / baseline - 1) * 100:+.1f}%", flush=True)
        report.update(status="passed", completedUtc=now())
        save()
    except Exception as error:
        report.update(status="failed", failure=str(error), completedUtc=now())
        save()
        raise


if __name__ == "__main__":
    main()
