# Backend resource optimization — 13 October 2026

Baseline: `cc006c5`. Local Windows x64 / MSVC Release measurements. Existing uncommitted frontend changes were preserved byte for byte; this change modifies only backend JSON and cache code, regression tests, and benchmark documentation.

## Changes

- JSON parsing copies ordinary UTF-8 spans and parses numbers directly from the input. Sorted object keys use an insertion hint; unordered keys and duplicate-key replacement still work. Rare subnormal/range cases retain CRT handling. Decoded Unicode escapes now obey the same byte ceiling as literal text.
- Cache accounting stores each path once. Scanning passes owned records directly into accounting and computes storage totals without collecting every file into a temporary vector.
- Cache eviction builds a min-heap and visits only the oldest required candidates. Thumbnail tiers, replacement exclusion, locked-file handling, and disk ceilings are preserved. A missing access timestamp falls back to that file's write timestamp, giving a consistent ordering.

Durable writes, backups, thumbnail quality, playback queues, and portable paths are unchanged. No persistent index, dependency, or new on-disk format was added.

## Measurements

| 25,000-file workload | Before | After | Change |
|---|---:|---:|---:|
| Index construction | 80.672 ms | 74.570 ms | 7.6% lower |
| Evict one file | 3.401 ms | 2.191 ms | 35.6% lower |
| Evict 100 files | 12.248 ms | 11.101 ms | 9.4% lower |
| Index construction peak tracked heap | 17,768,542 bytes | 10,350,304 bytes | 41.7% lower |
| Index construction allocations | 125,003 | 75,003 | 40.0% lower |

An earlier implementation searched the index twice for insertion and regressed construction time. The final implementation uses one insertion search. The memory benchmark includes input ownership and the production record transfer, avoiding an artificial advantage from moving untracked inputs.

## Validation

- `build_and_release.ps1`: successful optimized build, all **20/20** PR suites passed in 480.91 seconds, package verified and installed into workspace `release/` and `debug/` with saved data retained. Includes **443 unit tests**, **108 persistence tests**, **8,524 thumbnail assertions**, and the static-analysis gate.
- JSON fuzz: seed 1337, 10,101 total cases, 4,102 accepted/round-tripped and 5,999 rejected; all invariants passed.
- Startup/resource gate: one warmup plus five measured processes using isolated empty profiles; all budgets passed. Medians: GPU shell 45.05 ms, window shown 27.73 ms, library-ready frame 296.40 ms, working set 57,589,760 bytes (54.92 MiB), private bytes 61,575,168 (58.72 MiB). This is a current-build budget check, not a before/after startup measurement.
- `git diff --check` passed for the changed files. Existing frontend source remained byte-identical to the initial snapshot.

The local package is `dist/Aperture-2.0.0.0-windows-x64.zip` (13,357,874 bytes); runtime payload is 29,765,477 bytes including the 3,279,872-byte executable. These are current artifact sizes, not a measured package-size reduction. No release was published. Existing compressed thumbnail storage and the 160 MiB total disposable-cache ceiling remain in effect.

## Reproduction and scope

The save/load comparison uses `tests/benchmark/run_save_benchmark.ps1` against baseline and updated source directories, one warmup and seven measured samples for each workload. The 25,000-item load median was 421.365 ms before and 380.887 ms after (9.6% lower); 500-item load was 31.622 ms versus 32.104 ms (1.5% higher). These are sequential before/after runs with warm OS caches, not an end-to-end startup comparison. Load peak tracked C++ memory was unchanged at 68,215,717 bytes for 25,000 items. The identical 9,024,568 serialized bytes and matching content hashes verify unchanged durable storage representation. All save/load workloads are retained in the evidence, including small timing regressions.

The maintenance runner compiles the same harness against both versions, alternates execution order, discards one warmup, and compares seven measured samples per version with matching result hashes. Index construction includes input-path allocations and transfers each scanned record once, as production does. Eviction measures the real accounting/deletion path against absent files in isolated roots; it excludes a filesystem rescan and does not predict disk-dependent end-to-end latency.

C++ allocation tracking is not process RSS or VRAM. Playback, decoding, and GPU allocation behavior were not changed. No playback/VRAM improvement is claimed. App-owned data remains beneath the executable directory; test harness temporary directories are separate development artifacts.

Raw measurements: [backend-resources-20261013](../evidence/backend-resources-20261013/).
