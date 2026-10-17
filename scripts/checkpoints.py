#!/usr/bin/env python3
"""Maintain the data embedded in docs/checkpoints.html (Python standard library only)."""
import argparse
import datetime as dt
import json
import math
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_HTML = ROOT / "docs" / "checkpoints.html"
BLOCK = re.compile(r'(<script id="checkpoint-data" type="application/json">\s*)(.*?)(\s*</script>)', re.S)
CATEGORIES = {"Playback", "Startup", "Storage", "Library", "Rendering", "Subtitles", "Maintenance"}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0


def identity(result):
    return tuple(result[k] for k in ("metric", "protocol", "workload", "environment"))


def timestamp(value):
    require(isinstance(value, str), "measuredAt must be an ISO date or timestamp")
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", value):
        dt.date.fromisoformat(value)
    else:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
        require(parsed.tzinfo is not None, "Exact measurement timestamps need a timezone")


def validate(data):
    require(isinstance(data, dict), "Data must be an object")
    require(data.get("schemaVersion") == 1, "Unsupported schemaVersion")
    for name in ("metrics", "protocols", "environments", "references"):
        require(isinstance(data.get(name), dict), f"{name} must be an object")
    for key, metric in data["metrics"].items():
        require(isinstance(metric, dict), f"Metric must be an object: {key}")
        require(metric.get("unit") in {"ms", "MiB", "bytes", "%", "count"}, f"Invalid unit: {key}")
        require(metric.get("direction") in {"lower", "higher", "neutral"}, f"Invalid direction: {key}")
        require(bool(metric.get("label")), f"Missing metric label: {key}")
    for key, protocol in data["protocols"].items():
        require(isinstance(protocol, dict), f"Protocol must be an object: {key}")
        require(protocol.get("category") in CATEGORIES, f"Invalid category: {key}")
        require(bool(protocol.get("label")) and bool(protocol.get("method")), f"Missing protocol description: {key}")
    for key, env in data["environments"].items():
        require(isinstance(env, dict), f"Environment must be an object: {key}")
        require(bool(env.get("label")) and bool(env.get("details")), f"Missing environment: {key}")
        if "logicalProcessors" in env:
            count = env["logicalProcessors"]
            require(isinstance(count, int) and not isinstance(count, bool) and count > 0, f"Invalid logical processor count: {key}")
    history = data.get("historySeries", {})
    require(isinstance(history, dict), "historySeries must be an object")
    for key, series in history.items():
        require(isinstance(series, dict), f"History series must be an object: {key}")
        require(all(isinstance(series.get(k), str) and series[k] for k in ("label", "method", "workload")), f"Incomplete history series: {key}")
        require(isinstance(series.get("protocols"), list) and bool(series["protocols"]), f"History series needs protocols: {key}")
        require(all(p in data["protocols"] for p in series["protocols"]), f"Unknown history protocol: {key}")

    def check_result(r, context):
        require(isinstance(r, dict), f"Result must be an object: {context}")
        require("value" in r and "samples" in r, f"Result needs explicit value and samples fields: {context}")
        for field, table in (("metric", "metrics"), ("protocol", "protocols"), ("environment", "environments")):
            require(r.get(field) in data[table], f"Unknown {field} in {context}: {r.get(field)}")
        for field in ("workload", "session", "evidence"):
            require(isinstance(r.get(field), str) and bool(r[field].strip()), f"Missing {field} in {context}")
        if r.get("value") is None:
            require(bool(r.get("missingReason")), f"Null value needs missingReason in {context}")
        else:
            require(finite(r["value"]), f"Value must be finite and nonnegative in {context}")
        n = r.get("samples")
        require(n is None or (isinstance(n, int) and not isinstance(n, bool) and n > 0), f"Invalid sample count in {context}")
        require(n is not None or bool(r.get("samplingNote")), f"Unknown samples need samplingNote in {context}")
        if "runRange" in r:
            interval = r["runRange"]
            require(isinstance(interval, dict) and finite(interval.get("min")) and finite(interval.get("max")) and interval["min"] <= interval["max"], f"Invalid run range in {context}")
            runs = interval.get("runs")
            require(isinstance(runs, int) and not isinstance(runs, bool) and runs > 0 and bool(interval.get("scope")), f"Incomplete run range in {context}")
        if "measuredAt" in r:
            timestamp(r["measuredAt"])
        if r.get("historySeries"):
            require(r["historySeries"] in history, f"Unknown history series in {context}")
            series = history[r["historySeries"]]
            require(r["protocol"] in series["protocols"] and r["workload"] == series["workload"], f"Incompatible history series in {context}")

    for key, ref in data["references"].items():
        require(isinstance(ref, dict), f"Reference must be an object: {key}")
        require(bool(ref.get("player")) and bool(ref.get("version")), f"Missing reference player/version: {key}")
        timestamp(ref.get("measuredAt"))
        require(isinstance(ref.get("results"), list), f"Reference results must be a list: {key}")
        seen = set()
        for r in ref["results"]:
            check_result(r, key)
            require(identity(r) not in seen, f"Duplicate result in {key}")
            seen.add(identity(r))

    require(isinstance(data.get("checkpoints"), list) and bool(data["checkpoints"]), "No checkpoints")
    ids = set()
    for cp in data["checkpoints"]:
        require(isinstance(cp, dict), "Checkpoint must be an object")
        key = cp.get("id")
        require(isinstance(key, str) and re.fullmatch(r"[a-z0-9][a-z0-9-]*", key), "Invalid checkpoint id")
        require(key not in ids, f"Duplicate checkpoint: {key}")
        ids.add(key)
        timestamp(cp.get("measuredAt"))
        require(bool(cp.get("label")) and bool(cp.get("notes")), f"Missing label/notes: {key}")
        require(cp.get("commit") is None or bool(re.fullmatch(r"[0-9a-f]{7,40}", cp["commit"])), f"Invalid commit: {key}")
        require(cp.get("dirty") is None or isinstance(cp["dirty"], bool), f"Invalid dirty flag: {key}")
        require(bool(cp.get("sourceNote")), f"Missing sourceNote: {key}")
        require(isinstance(cp.get("results"), list), f"Results must be a list: {key}")
        seen = set()
        for r in cp["results"]:
            check_result(r, key)
            require(identity(r) not in seen, f"Duplicate metric/workload in {key}: {identity(r)}")
            seen.add(identity(r))
            if r.get("reference"):
                require(r["reference"] in data["references"], f"Unknown reference in {key}")
                ref = data["references"][r["reference"]]
                matches = [b for b in ref["results"] if identity(b) == identity(r) and b["session"] == r["session"]]
                require(len(matches) == 1, f"Reference conditions/session do not match in {key}")
        for test in cp.get("tests", []):
            require(isinstance(test, dict), f"Test must be an object: {key}")
            require(test.get("status") in {"pass", "fail", "partial", "not-run"}, f"Invalid test status: {key}")
            require(bool(test.get("name")) and bool(test.get("evidence")), f"Missing test evidence: {key}")
            passed, total = test.get("passed"), test.get("total")
            require((passed is None and total is None) or (type(passed) is int and type(total) is int and 0 <= passed <= total and total > 0), f"Invalid test counts: {key}")
            require(test.get("status") != "pass" or total is None or passed == total, f"Passing test has incomplete counts: {key}")
            require(test.get("durationSeconds") is None or finite(test["durationSeconds"]), f"Invalid test duration: {key}")
    for cp in data["checkpoints"]:
        require(not cp.get("pairedWith") or (cp["pairedWith"] in ids and cp["pairedWith"] != cp["id"]), f"Unknown/self paired checkpoint: {cp['id']}")
    report_ids = set()
    for report in data.get("reports", []):
        require(report.get("id") not in report_ids and bool(report.get("id")), "Duplicate/missing report id")
        report_ids.add(report["id"])
        require(all(isinstance(report.get(k), str) and report[k] for k in ("title", "source", "text")), "Incomplete report")
    for cp in data["checkpoints"]:
        require(not cp.get("report") or cp["report"] in report_ids, f"Unknown report: {cp['id']}")
    return data


def load_html(path):
    html = path.read_text(encoding="utf-8")
    match = BLOCK.search(html)
    require(match is not None, "HTML data block not found")
    return html, json.loads(match.group(2))


def write_html(path, html, data):
    validate(data)
    payload = json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    updated = BLOCK.sub(lambda m: m.group(1) + payload + m.group(3), html, count=1)
    require(updated != html or BLOCK.search(updated) is not None, "Data replacement failed")
    # Validate completely before an atomic replacement. Preserve all frontend bytes.
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", dir=path.parent, suffix=".tmp", delete=False) as handle:
        temporary = Path(handle.name)
        handle.write(updated)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def append_bundle(data, bundle):
    require(isinstance(bundle, dict), "Bundle must be an object")
    result = json.loads(json.dumps(data))
    require(isinstance(bundle.get("checkpoints"), list) and bundle["checkpoints"], "Bundle needs a checkpoints list")
    for name in ("metrics", "protocols", "environments", "references", "historySeries"):
        result.setdefault(name, {})
        for key, value in bundle.get(name, {}).items():
            require(key not in result[name] or result[name][key] == value, f"Cannot redefine historical {name}: {key}")
            result[name][key] = value
    result["checkpoints"].extend(bundle["checkpoints"])
    result.setdefault("reports", []).extend(bundle.get("reports", []))
    result["updatedAt"] = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    return validate(result)


def template(data):
    return {"checkpoints": [{
        "id": "replace-with-unique-id", "label": "Describe the optimization",
        "measuredAt": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "commit": None, "dirty": None,
        "sourceNote": "REPLACE: measured commit, dirty state, executable and source hashes",
        "notes": "REPLACE: change, measurement conditions, tradeoffs and limitations",
        "results": [{"metric": "visible-ms", "protocol": "playback-1080p30",
                     "environment": "ryzen3900x-4070super", "workload": "1080p30 H.264 / no audio / 1280×800",
                     "session": "REPLACE-with-new-session-id", "value": None, "missingReason": "Not measured yet",
                     "samples": None, "samplingNote": "REPLACE: measured runs, warmups, aggregation",
                     "evidence": "REPLACE: docs/evidence/<session>/summary.json"}],
        "tests": [{"name": "Local regression gate", "status": "not-run", "passed": None,
                   "total": None, "durationSeconds": None, "evidence": "REPLACE: evidence path"}]
    }]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--html", type=Path, default=DEFAULT_HTML)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--check", action="store_true")
    actions.add_argument("--list", action="store_true")
    actions.add_argument("--export", type=Path)
    actions.add_argument("--template", type=Path)
    actions.add_argument("--append", type=Path, help="Append a JSON bundle; never silently edit old observations")
    args = parser.parse_args(argv)
    try:
        html, data = load_html(args.html)
        validate(data)
        if args.check:
            count = sum(len(cp["results"]) for cp in data["checkpoints"])
            print(f"Valid: {len(data['checkpoints'])} checkpoints, {count} observations, {len(data.get('reports', []))} embedded reports")
        elif args.list:
            for cp in sorted(data["checkpoints"], key=lambda c: c["measuredAt"]):
                print(f"{cp['id']:44} {cp['measuredAt']:25} {cp.get('commit') or 'unknown':10} {len(cp['results']):4} results  {cp['label']}")
        elif args.export or args.template:
            path = args.export or args.template
            require(not path.exists(), f"Output already exists: {path}")
            path.write_text(json.dumps(data if args.export else template(data), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            print(f"Wrote {path}")
        else:
            bundle = json.loads(args.append.read_text(encoding="utf-8-sig"))
            require("REPLACE" not in json.dumps(bundle) and "replace-with-unique-id" not in json.dumps(bundle), "Fill template placeholders before appending")
            updated = append_bundle(data, bundle)
            write_html(args.html, html, updated)
            print(f"Appended {len(bundle['checkpoints'])} checkpoints to {args.html}")
    except (ValueError, KeyError, TypeError, OSError) as exc:
        print(f"Checkpoint error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
