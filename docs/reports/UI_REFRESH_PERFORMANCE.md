# UI refresh performance — 5 October 2026

Baseline: `0373c27`. Implementation: `e44f2a9`. This follows [subtitle lookup optimization](SUBTITLE_LOOKUP_PERFORMANCE.md) and targets repeated work in the UI instead of adding background workers or changing visual quality.

## Changes

Accessibility refresh previously copied the entire retired control tree, including all strings, relationship vectors, and callback captures, then assigned the new tree while holding the reader mutex. The replacement now validates IDs once, prepares the incoming copy outside the lock, and swaps ownership inside the lock. The retired snapshot remains owned through provider disconnection and change notification. Incoming data still belongs to the caller; no borrowed lifetime is introduced. Invalid trees leave the current tree and its providers intact.

Chapter markers previously rebuilt every quarter second, allocating a vector and copying up to 500 titles. An immutable snapshot now lasts until an actual chapter boundary, version change, or duration change. Backward seeks use the same exact interval boundaries, including unsorted and duplicate chapter times. The existing 500-marker and 80-character title limits remain. This also removes stale passed-state display when a chapter starts within a quarter-second interval. The cache uses the version of the copied chapter list so an asynchronous player update cannot label an old list with a newer version.

## Measurements

MSVC 14.44.35207, x64, `/O2`, `/MT`, C++17. Both binaries compile first and run sequentially on the same logical processor. One discarded round precedes seven measured rounds; version order alternates. No build or tests ran during timing. Values are medians of complete workloads, not per-frame time.

| Workload | Calls | Before ms | After ms | Time reduction | Allocation requests before → after |
|---|---:|---:|---:|---:|---:|
| Refresh 64 accessible controls | 200 | 18.5684 | 13.0499 | 29.7% | 91,656 → 78,189 |
| Refresh 4,096 accessible selected rows | 20 | 170.5773 | 118.0112 | 30.8% | 602,280 → 516,201 |
| Empty chapter list | 36,000 | 0.6750 | 0.6242 | 7.5% | 2,399 → 0 |
| 12 chapter markers | 36,000 | 3.5386 | 1.4483 | 59.1% | 33,586 → 154 |
| 500 chapter markers | 36,000 | 70.1188 | 2.0960 | 97.0% | 1,204,298 → 9,538 |
| Random chapter seeks, 500 markers | 4,000 | 113.2658 | 110.2158 | 2.7% | 2,006,996 → 1,906,596 |

The small random-seek difference should not be generalized. Most seeks cross boundaries and still need a new snapshot. Empty-list timings are also too small for a strong CPU claim; the eliminated allocations are deterministic.

Accessibility measurements invoke the actual baseline/current `AccessibilityTree::ReplaceNodes` with identical stable control trees. They exclude App's node construction, a real HWND, assistive-technology clients, and OS event delivery. The harness verifies stored names, descriptions, help text, automation IDs, values, selection, relationships, bounds, and ordering after the measured refreshes. Heap accounting includes initial tree publication in both versions because the baseline reuses its original allocations while the new implementation replaces storage. Timing excludes that initial publication. Peak requested C++ heap remains unchanged: 277,758 bytes for 64 nodes and 17,760,549 bytes for 4,096 nodes. No memory reduction is claimed for this change.

Chapter measurements use the frozen baseline getter body and the current production `ChapterMarkerCache`, isolated from the window/player. Sequential calls simulate ten minutes at 60 queries per second. Chapters start 30 seconds apart; random seeks cover the same ten-minute window. The initial snapshot is excluded. Every query checks a deterministic marker sample's text/passed-state hash against an independent reference; full boundary, ordering, title, position, invalid-time, and ownership behavior is covered by unit tests. Chapter boundaries are quarter-second aligned so the output remains comparable; the sub-quarter boundary correction is tested separately.

All 48 rows per version match output hashes. Both marker implementations retain one snapshot; the 500-marker warm allocation peak remains 136,158 bytes. Allocation accounting excludes pre-existing inputs, allocator overhead, OS resources, and GPU memory. These results isolate UI housekeeping and do not establish whole-app CPU or playback frame-rate percentages.

Raw evidence: [before](../evidence/ui-refresh-evidence/before.csv), [after](../evidence/ui-refresh-evidence/after.csv), [comparison](../evidence/ui-refresh-evidence/comparison.json).

## Validation and delivery

All 14 focused UI cases pass. Added cases require exact forward/backward chapter boundaries, immutable previous-frame snapshots, title truncation, normalized position updates, empty/500-marker limits, invalid playback times, and version changes. Accessibility coverage requires independent ownership of strings/relationships/callbacks, provider continuity across valid replacement, atomic rejection of invalid trees, and provider disconnection when nodes disappear.

The final `e44f2a9` local build passes all 20 PR suites, including all 389 unit cases, static analysis without new warnings, and shutdown/cache checks. Both installed copies retain saved data and match all ten packaged payload files. Binary SHA-256 is `fa39f146fab74fb4db17094c33a1c166adc4b9ed856a3bfaa45ae63c5dbcbb0b`. The additional 10,000-case subtitle fuzz test passes. The separate real-app playback/restart check is **blocked**: another checkout's Aperture instance is open, and the harness refuses to disturb it. This is not recorded as a passing smoke test.

[Validation evidence](../evidence/ui-refresh-evidence/validation.json) records suite results, fuzz counts, the source commit, and package hashes. Changes are committed locally; version remains `2.0.0.0` and no GitHub release is published.

## Reproduce

Export baseline `0373c27` into an external scratch directory, then run:

```powershell
./tests/benchmark/run_ui_benchmark.ps1 -BaselineSourceDirectory '<baseline>/src' -OutputDirectory '<external-results>'
./tests/unit/run_tests.ps1 -Suite RenderViewModel
./tests/unit/run_tests.ps1 -Suite Accessibility
./build_and_release.ps1 -FailFast
```

The runner removes its isolated temporary binaries after completion or failure.
