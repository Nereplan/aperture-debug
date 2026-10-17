"""Check actual direct-launch overlay counters captured by profile_player.

Use run_player_profile.ps1 -StateOnly with the no-subtitle test fixture, no input,
and matching Release PDBs. This checks live app behavior, not source text.
"""
import argparse
import json
from pathlib import Path


def read(path):
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    if not data.get("stateOnly") or not data.get("appFieldsAvailable"):
        raise ValueError("Requires a successful, non-suspending PDB state capture")
    points = data["snapshots"]
    if len(points) < 5 or any(p is None for p in points):
        raise ValueError("Missing live counter samples")
    span = points[-1]["elapsedSeconds"] - points[0]["elapsedSeconds"]
    if span < 5 or any(b["elapsedSeconds"] <= a["elapsedSeconds"] for a, b in zip(points, points[1:])):
        raise ValueError("Insufficient or nonmonotonic observation window")
    if any(p["controlsOpacity"] != 0 for p in points):
        raise ValueError("Controls were visible; this is not an empty-overlay observation")
    return points, span


def verify(candidate_path, baseline_path=None):
    points, span = read(candidate_path)
    delta = points[-1]["overlayFramesDrawn"] - points[0]["overlayFramesDrawn"]
    if delta != 0:
        raise ValueError(f"Empty playback overlay drew {delta} frames in {span:.2f}s")
    if baseline_path:
        before, before_span = read(baseline_path)
        if any(p["hubPresented"] for p in before + points):
            raise ValueError("The unpresented-hub direct-launch regression was not exercised")
        before_delta = before[-1]["overlayFramesDrawn"] - before[0]["overlayFramesDrawn"]
        if before_delta < before_span * 20:
            raise ValueError("Baseline did not reproduce continuous empty-overlay redraws")
        print(f"PASS: reproduced {before_delta / before_span:.1f} empty redraws/s before; 0 frames in {span:.2f}s after.")
    else:
        print(f"PASS: empty playback overlay drew 0 frames over {span:.2f}s.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    verify(args.candidate, args.baseline)
