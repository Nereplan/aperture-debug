"""Manual storage experiment, not an Aperture startup benchmark.

Compares equivalent JSON records with an indexed SQLite container. Python
timings/heap estimates cannot be compared with native app measurements. The
full-hydration control exposes the cost if callers still demand every record.
All databases, journals and outputs stay in the explicitly supplied directory;
SQLite temporary sorting uses memory. No production format is modified.
"""
import argparse
import gc
import hashlib
import json
from pathlib import Path
import sqlite3
import statistics
import time
import tracemalloc


def connect(path):
    db = sqlite3.connect(path)
    db.execute("PRAGMA temp_store=MEMORY")
    db.execute("PRAGMA cache_size=-2048")
    return db


def measure(operation):
    timings = []
    digest = None
    for sample in range(8):
        gc.collect()
        start = time.perf_counter()
        result = operation()
        elapsed = (time.perf_counter() - start) * 1000
        hashed = hashlib.sha256(json.dumps(result, sort_keys=True, ensure_ascii=False).encode()).hexdigest()
        assert digest in (None, hashed)
        digest = hashed
        if sample:
            timings.append(elapsed)
        del result
    gc.collect()
    tracemalloc.start()
    result = operation()
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    return dict(median_ms=statistics.median(timings), samples_ms=timings,
                peak_python_bytes=peak, count=len(result), sha256=digest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    database = args.output / "catalog.sqlite"
    if database.exists():
        raise SystemExit("Use a fresh output directory; existing databases are preserved.")
    records = json.loads(args.source.read_text(encoding="utf-8-sig"))
    if not isinstance(records, list):
        raise SystemExit("Source must be an isolated array fixture, not an application profile.")
    with connect(database) as db:
        db.execute("PRAGMA journal_mode=DELETE")
        db.execute("PRAGMA synchronous=FULL")
        db.execute("CREATE TABLE records(slot INTEGER PRIMARY KEY,title TEXT NOT NULL,payload TEXT NOT NULL)")
        db.executemany("INSERT INTO records VALUES(?,?,?)", (
            (slot, str(record.get("title", "")), json.dumps(record, ensure_ascii=False, separators=(",", ":")))
            for slot, record in enumerate(records)))
        db.execute("CREATE INDEX initial_order ON records(title,slot)")
    db.close()
    expected_page = sorted(enumerate(records), key=lambda pair: (str(pair[1].get("title", "")), pair[0]))[:50]
    expected_page = [record for _, record in expected_page]
    del records

    def json_full():
        return json.loads(args.source.read_text(encoding="utf-8-sig"))

    def json_page():
        return sorted(json_full(), key=lambda record: str(record.get("title", "")))[:50]

    def sql_read(page):
        db = connect(database)
        try:
            query = "SELECT payload FROM records ORDER BY title,slot LIMIT 50" if page else "SELECT payload FROM records ORDER BY slot"
            return [json.loads(row[0]) for row in db.execute(query)]
        finally:
            db.close()

    assert sql_read(True) == expected_page
    results = {"scope": "Python container experiment; excludes native model/query/migration and SQLite C heap",
               "json_bytes": args.source.stat().st_size, "sqlite_bytes": database.stat().st_size,
               "sqlite_version": sqlite3.sqlite_version,
               "json_full": measure(json_full), "sqlite_full": measure(lambda: sql_read(False)),
               "json_first_50": measure(json_page), "sqlite_first_50": measure(lambda: sql_read(True))}
    assert results["json_full"]["sha256"] == results["sqlite_full"]["sha256"]
    assert results["json_first_50"]["sha256"] == results["sqlite_first_50"]["sha256"]
    (args.output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
