#!/usr/bin/env python3
"""
Authoritative validator for Aperture parser and media container corpus manifest.
Validates manifest schema, SHA-256 hashes, file sizes, category constraints,
duplicate IDs, missing files, and orphaned fixture files.
"""

import sys
import os
import json
import hashlib

def validate_corpus(corpus_dir: str = None) -> bool:
    if not corpus_dir:
        corpus_dir = os.path.dirname(os.path.abspath(__file__))
    manifest_path = os.path.join(corpus_dir, "manifest.json")
    fixtures_dir = os.path.join(corpus_dir, "fixtures")

    if not os.path.isfile(manifest_path):
        print(f"[ERROR] Manifest not found: {manifest_path}", file=sys.stderr)
        return False

    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except Exception as ex:
        print(f"[ERROR] Failed to parse manifest JSON: {ex}", file=sys.stderr)
        return False

    errors = []
    seen_ids = set()
    registered_paths = set()

    categories = manifest.get("categories", [])
    if not categories:
        errors.append("Manifest missing 'categories' list")
    valid_categories = set(categories)

    fixtures = manifest.get("fixtures", [])
    if not fixtures:
        errors.append("Manifest contains no fixtures")

    declared_count = manifest.get("fixtureCount", 0)
    if declared_count != len(fixtures):
        errors.append(f"Declared fixtureCount ({declared_count}) != actual fixtures array length ({len(fixtures)})")

    for f in fixtures:
        fid = f.get("id")
        if not fid:
            errors.append("Fixture missing 'id'")
            continue
        if fid in seen_ids:
            errors.append(f"Duplicate fixture ID detected: '{fid}'")
        seen_ids.add(fid)

        cat = f.get("category")
        if not cat or cat not in valid_categories:
            errors.append(f"Fixture '{fid}' has invalid category '{cat}'. Valid: {sorted(list(valid_categories))}")

        outcome = f.get("expectedOutcome")
        if outcome not in ["accept", "reject"]:
            errors.append(f"Fixture '{fid}' has invalid expectedOutcome '{outcome}'")

        reason = f.get("expectedReason")
        if not reason or not isinstance(reason, str):
            errors.append(f"Fixture '{fid}' missing valid expectedReason string")

        rel_path = f.get("path")
        if not rel_path:
            errors.append(f"Fixture '{fid}' missing 'path'")
            continue

        if rel_path.startswith("/") or rel_path.startswith("\\") or ":" in rel_path:
            errors.append(f"Fixture '{fid}' path must be relative: '{rel_path}'")
            continue

        full_path = os.path.normpath(os.path.join(corpus_dir, rel_path.replace("/", os.sep)))
        if not full_path.startswith(corpus_dir):
            errors.append(f"Fixture '{fid}' path escapes corpus root: '{rel_path}'")
            continue

        registered_paths.add(full_path.lower())

        if not os.path.isfile(full_path):
            errors.append(f"Fixture '{fid}' file not found on disk: {rel_path} (resolved: {full_path})")
            continue

        try:
            with open(full_path, "rb") as fp:
                content = fp.read()
        except Exception as ex:
            errors.append(f"Fixture '{fid}' failed to read: {ex}")
            continue

        actual_size = len(content)
        expected_size = f.get("sizeBytes")
        if expected_size is None or actual_size != expected_size:
            errors.append(f"Fixture '{fid}' size mismatch: expected {expected_size} bytes, got {actual_size} bytes")

        actual_hash = hashlib.sha256(content).hexdigest()
        expected_hash = f.get("sha256")
        if expected_hash is None or actual_hash.lower() != expected_hash.lower():
            errors.append(f"Fixture '{fid}' SHA-256 mismatch: expected {expected_hash}, got {actual_hash}")

    if os.path.isdir(fixtures_dir):
        for root, _, files in os.walk(fixtures_dir):
            for fname in files:
                fpath = os.path.normpath(os.path.join(root, fname)).lower()
                if fpath not in registered_paths:
                    errors.append(f"Orphan fixture file on disk not registered in manifest: {fpath}")

    if errors:
        print(f"[FAIL] Corpus manifest validation failed with {len(errors)} error(s):", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return False

    category_counts = {}
    for f in fixtures:
        c = f.get("category", "unknown")
        category_counts[c] = category_counts.get(c, 0) + 1

    print(f"[PASS] Corpus manifest verified: {len(fixtures)} fixtures across {len(category_counts)} categories.")
    for c, cnt in sorted(category_counts.items()):
        print(f"  - {c}: {cnt} fixtures")
    return True

if __name__ == "__main__":
    target_dir = sys.argv[1] if len(sys.argv) > 1 else None
    if not validate_corpus(target_dir):
        sys.exit(1)
    sys.exit(0)
