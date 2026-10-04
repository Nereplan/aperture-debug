# Bulk removal and cache eviction follow-up — 4 October 2026

Baseline: `1e89ccd`. Production change: `79840ed`. Benchmark harness and shared allocation probe: `69e974b`. Subtitle fuzz repair: `5fecf8d`. Version remains `2.0.0.0`; all work and release preparation run locally.

## Remaining costs found

Bulk removal prepared thumbnail cleanup by scanning the full media vector for each selected ID. It scanned the remaining vector again for each cleanup candidate. Removing half of a large library therefore did quadratic work in the UI callback, even though the model's actual erasure already used a set of IDs.

Cache eviction copied every indexed file record, including its full path, into a candidate vector. It then searched the path map again for every candidate in both eviction phases to identify records already erased. The borrowed index already owns these records throughout the locked eviction pass.

## Changes

- `MediaLibrary::RemoveMedia` optionally returns owned paths/durations for every record it actually erases. The UI uses that output after erasure, eliminating both repeated library scans. Existing callers, skipped counts, history retention, playlist cleanup, save dispatch, and thumbnail-producer draining remain compatible. Duplicate IDs on different records now produce cleanup metadata for all erased records. Empty/no-op calls clear a previously populated output.
- Eviction sorts pointers to indexed records, reserves candidate storage once, and clears each erased pointer before another phase can revisit it. Failed/locked deletions keep their candidates. File-age ordering, read-only handling, replacement exclusion, class/tier protection, and accounting retain their policies. Seek-thumbnail classification borrows a filename view instead of allocating a substring.
- Subtitle text lookup removes a redundant backward walk over every earlier cue before its existing forward scan. Sorted cues before `upper_bound` all start at or before the adjusted playback time, so that backward walk always reached the beginning. The forward scan remains necessary for nested/overlapping cues; cold misses still have linear cost. No subtitle speed percentage is claimed.
- Query, save, and maintenance benchmarks share their identical allocation instrumentation in `AllocationProbe.h`. It remains confined to one translation unit per benchmark executable.
- The subtitle fuzz runner now links its production discovery and JSON dependencies through the shared native build helper. Its malformed-timestamp checks require the parser's `-1` rejection sentinel and verify that rejected timestamps cannot produce SRT cues. The previous runner failed to link; its old negative-timestamp assertion also treated that rejection sentinel as an error.

## Measurements

Both source versions use the same current maintenance harness, MSVC 14.44.35207 x64, `/O2`, `/MT`, C++17, on the same Windows machine. Each process is pinned to one logical processor. Both versions compile before timing. One discarded round precedes seven measured rounds, alternating version order; each round uses fresh processes. No test/build ran during these timing rounds.

| Workload | Before ms | After ms | Time reduction | Before peak C++ MiB | After peak C++ MiB |
|---|---:|---:|---:|---:|---:|
| Remove 250 of 500 media records | 1.774 | 1.411 | 20.42% | 0.239 | 0.231 |
| Remove 50 of 25,000 records | 83.851 | 81.291 | 3.05% | 8.026 | 8.025 |
| Remove 12,500 of 25,000 records | 877.864 | 92.173 | 89.50% | 11.849 | 11.468 |
| Evict one of 25,000 indexed files | 108.724 | 3.581 | 96.71% | 13.169 | 0.193 |
| Evict 100 of 25,000 indexed files | 115.483 | 11.497 | 90.04% | 13.169 | 0.193 |

Values are medians. The sparse-removal difference is small and should not be generalized. Large-removal cost still includes real vector erasure, playlist-reference cleanup, and rebuilding path/ID indexes. The baseline harness reproduces the former UI caller's two scans around the actual baseline `RemoveMedia`; the current harness uses the production metadata output. UI confirmation, undo capture, decoder draining, and actual thumbnail deletion are excluded.

Eviction uses the production indexed-eviction function with 25,000 synthetic long-path records beneath an isolated temporary cache directory. Payload files are absent: deletion still exercises native path validation and the supported already-missing-file accounting path. These results isolate candidate preparation and indexed eviction; they do not predict the latency of deleting real files on a particular storage device. The real-file regression covers both eviction phases, a locked file, and a read-only target.

For one-file eviction, allocation requests fall from **25,035 to 10**. Peak requested C++ heap falls from **13,808,231 to 201,895 bytes**, excluding the pre-existing index, allocator overhead, process memory, and GPU resources. There is no process-RAM or whole-app frame-rate claim.

Every sample checks exact survivor order and playlist membership, cleanup count/path-duration hashes, eviction membership/order, and accounting. Before/after comparisons also reject changed counts or result hashes. Cleanup order itself is allowed to follow library order because each removed file is independently purged.

Raw evidence: [before](../evidence/removal-eviction-evidence/before.csv), [after](../evidence/removal-eviction-evidence/after.csv), [comparison](../evidence/removal-eviction-evidence/comparison.json).

## Validation

- Focused `BulkOperations`: 7 cases pass with zero failures/skips, including duplicate/stale/empty IDs, output ownership after vector erasure, and clearing metadata on no-op calls.
- Thumbnail suite: 8,519 assertions pass. The added fixture requires both target-class and total-budget eviction phases, clears a read-only target, preserves a locked payload, and checks that erased candidates are not revisited.
- The shared query probe retains all 91 recorded sample result counts, ordered hashes, allocation counts, and peak bytes from the prior current-source default dataset. All 28 save-capture samples pass their existing content/reload checks.
- Nested subtitle coverage checks duplicate/empty text, instantaneous cues, inclusive ends, the next representable instant, delays, and backward seeks through the actual text cache.
- The local application build and all 20 PR regression suites pass, with zero failures/timeouts. The unit suite has 383 passing cases and zero skips. The gate includes static analysis, shutdown stress, process isolation, and thumbnail-cache checks. The application source is unchanged by the subsequent fuzz-runner repair.
- The repaired subtitle fuzz suite passes 10,000 mutations with seed `20261004`: 198,490 SRT/ASS cues validated, zero invariant failures, and zero crashes/hangs. Valid seeds, input limits, malformed-timestamp rejection, overlap queries, and delays also pass.
- The generated ZIP passes manifest/file hashes, x64 architecture, dependency closure, and isolated startup/shutdown verification. Staging is cleaned and the original package data is untouched. The package, release install, and debug install share binary SHA-256 `d8adad8f526a9dd6a7c9d65fcff8301832ba037020086b4099806d80e7dc3d04`; both installs retain saved data.

Recorded suite results and package identity: [validation evidence](../evidence/removal-eviction-evidence/validation.json).

## Reproduce

Use a separate clean baseline checkout and an external results directory. Run the benchmark after other compilation/tests finish.

```powershell
$scratch = Join-Path $env:LOCALAPPDATA 'Codex/tools/scratch/aperture-maintenance-repro'
$baseline = Join-Path $scratch 'baseline'
git worktree add --detach $baseline 1e89ccd
./tests/benchmark/run_maintenance_benchmark.ps1 -BaselineSourceDirectory "$baseline/src" -OutputDirectory "$scratch/results"
```

The benchmark removes its isolated build/cache directory after success or failure. Retain only small CSV/JSON evidence in documentation. For current application validation, use `./build_and_release.ps1 -FailFast` and the suite-selection guide in [Testing](../TESTING.md).

Reproduce the additional subtitle campaign through the bounded suite runner:

```powershell
./tests/run_tests.ps1 -Id fuzz_subtitle -PassThruArgs @('-Seed', '20261004', '-Iterations', '10000') -FailFast
```
