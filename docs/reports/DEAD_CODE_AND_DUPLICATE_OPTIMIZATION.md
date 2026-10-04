# Dead code and duplicate detection cleanup — 4 October 2026

Removed 17 unused view-model fields and their preparation on redraw, including discarded text copies and redundant PiP/group-header geometry. Removed unread thumbnail counters and their atomic updates, two unused channel-policy wrappers, and unused rendering/hover flags. Production source has a net reduction of 98 lines; tests and measurement tools are counted separately.

Duplicate detection now shares one keeper-order implementation and sorts original indices directly. Equal-size candidates are ordered by duration so pairs outside the 0.5-second tolerance are skipped. When a bucket consists of videos at least five seconds long with one known resolution, adjacent durations produce the same transitive groups as all-pairs comparisons. Text normalization is deferred until it is needed. Iterative, weighted disjoint sets avoid recursive traversal, and singleton clusters are not materialized. Exact file identities and path aliases retain precedence. Keeper rules and the existing input-order behavior at the five-second threshold are preserved; groups with equal titles/keys now have a path tie-break.

## Measurements

Baseline source: `e1fbe5d3ab3dca1325db8f73e815b4cd14506008`. Both versions use the current benchmark harness, MSVC x64 `/O2`, C++17, Visual Studio 2022 17.14.37, and an AMD Ryzen 9 3900X. Each process is pinned to one logical processor. Values are medians of seven samples after one warmup; no project builds or test runners ran during the recorded measurements.

| Workload | Before ms | After ms | Time reduction | Before peak MiB | After peak MiB |
|---|---:|---:|---:|---:|---:|
| 25,000 videos, separated duration pairs | 820.09 | 58.89 | 92.82% | 20.71 | 15.35 |
| 4,000 videos, one dense metadata group | 233.99 | 4.74 | 97.98% | 1.91 | 0.97 |
| 25,000 entries, 500 exact file identities | 6.39 | 4.71 | 26.27% | 1.04 | 0.94 |
| 25,000 videos, unknown resolution | 815.61 | 85.06 | 89.57% | 19.71 | 20.00 |

Every sample preserves the duplicate-entry count, group count, and hash of the ordered group memberships. Peak MiB measures live requested C++ allocations owned by the scan, excluding allocator overhead, process RSS, GPU memory, and pre-existing library records. These are synthetic CPU/heap measurements, not whole-application timings.

The unknown-resolution workload uses about 1.5% more peak temporary heap for weighted component tracking. Dense incomplete-metadata buckets can still require quadratic comparisons within the duration window; the fast adjacent-duration path applies when the full metadata predicate suffices.

Raw evidence: [before samples](../evidence/duplicate-detection-evidence/before.csv), [after samples](../evidence/duplicate-detection-evidence/after.csv), [comparison and allocation counts](../evidence/duplicate-detection-evidence/comparison.json).

## Validation

- `tests/unit/run_tests.ps1`: all 395 tests passed, including 32 seeded libraries checked against an independent pairwise graph oracle and transitive/threshold boundary cases.
- `tests/unit/run_tests.ps1 -Suite DuplicateDetection`: all 10 tests passed after adding the collision-order assertion.
- `build_and_release.ps1`: local Release build, all 20 PR-tier suites, packaging, and installation into `release`/`debug` passed with saved data retained. The gate includes static analysis, persistence, import workflows, malformed-input corpus, async shutdown, media probe isolation, and thumbnail caching.
- Benchmark comparison rejects changed counts/order hashes; `git diff --check` passed.

## Reproduce

Run these from the updated repository in PowerShell, using a fresh temporary worktree path. The current harness is used for both source versions.

```powershell
$baseline = Join-Path $env:TEMP 'Aperture-duplicate-before'
$results = Join-Path $env:TEMP 'Aperture-duplicate-results'
git worktree add --detach $baseline e1fbe5d3ab3dca1325db8f73e815b4cd14506008
./tests/benchmark/run_query_benchmark.ps1 -SourceDirectory "$baseline/src" -OutputDirectory "$results/before" -DuplicatesOnly
./tests/benchmark/run_query_benchmark.ps1 -OutputDirectory "$results/after" -DuplicatesOnly
./tests/benchmark/compare_query_benchmark.ps1 -BeforeCsv "$results/before/results.csv" -AfterCsv "$results/after/results.csv" -OutputJson "$results/comparison.json" -DuplicatesOnly
```
