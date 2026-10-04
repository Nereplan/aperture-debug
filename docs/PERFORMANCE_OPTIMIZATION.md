# Measured performance improvements

Baseline: `17ed90a` (the previous duplicate optimization). Measurements below isolate this additional pass; they do not reuse the earlier, slower baseline.

## Method

Both versions used the current `query_resource_benchmark.cpp` harness and MSVC 14.44.35207, x64, `/O2`, C++17, on the same Windows machine. Each of 26 workloads ran once to warm up and seven times for measurement. Results are medians. Before/after binaries ran sequentially, with no build or test running during these measurements.

The harness creates 25,000 synthetic library records; dense duplicate workloads use 4,000 records. Search-with-history adds 1,000 history records, including 500 existing media paths and 500 history-only paths. File-existence checks are disabled. Allocation measurements cover C++ heap allocations made inside the measured operation, including the returned projection; they exclude input records, operating-system allocations, GPU memory, and process working set.

For every sample and version the comparison gate required identical result counts, group counts, and ordered output hashes. This is a performance experiment, not a statistical estimate for every real library. Small changes in unchanged paths, especially sub-millisecond operations, remain susceptible to scheduling and CPU-frequency variation.

## Timing and allocation results

| Workload | Before ms | After ms | Time reduction | Allocations before → after | Peak C++ heap before → after (MiB) |
|---|---:|---:|---:|---:|---:|
| flat | 12.496 | 12.559 | -0.50% | 50,008 → 50,008 | 8.637 → 8.637 |
| folder | 20.212 | 19.460 | 3.72% | 110,009 → 110,009 | 9.208 → 9.208 |
| series | 36.039 | 26.351 | 26.88% | 357,965 → 182,965 | 9.100 → 9.100 |
| capped | 0.652 | 0.746 | -14.42% | 107 → 107 | 0.780 → 0.780 |
| manual | 6.597 | 6.462 | 2.05% | 50,007 → 50,007 | 8.637 → 8.637 |
| smart playlist | 0.387 | 0.387 | 0.00% | 52 → 52 | 0.196 → 0.196 |
| search | 0.524 | 0.363 | 30.73% | 153 → 153 | 0.015 → 0.015 |
| nearly unlimited | 11.920 | 12.418 | -4.18% | 48,008 → 48,008 | 8.333 → 8.333 |
| extension | 1.997 | 1.978 | 0.95% | 0 → 0 | 0.000 → 0.000 |
| index build | 84.660 | 40.910 | 51.68% | 573,004 → 150,003 | 29.542 → 16.126 |
| multi token path | 6.699 | 5.279 | 21.20% | 158 → 158 | 0.015 → 0.015 |
| multi token title | 7.648 | 3.526 | 53.90% | 156 → 156 | 0.016 → 0.016 |
| search miss | 7.440 | 2.120 | 71.51% | 5 → 5 | 0.001 → 0.001 |
| duplicates duration windows | 57.751 | 49.917 | 13.57% | 303,804 → 191,256 | 15.350 → 9.838 |
| duplicates dense metadata | 4.549 | 4.279 | 5.94% | 20,095 → 8,060 | 0.974 → 0.512 |
| duplicates file identity | 4.690 | 4.487 | 4.33% | 9,528 → 3,544 | 0.943 → 0.876 |
| duplicates unknown resolution | 81.916 | 65.394 | 20.17% | 478,505 → 265,956 | 19.998 → 9.092 |
| collapsed folders | 14.200 | 14.374 | -1.23% | 58,605 → 58,605 | 1.312 → 1.312 |
| capped folders | 14.001 | 9.201 | 34.28% | 57,051 → 57,051 | 1.242 → 1.242 |
| index with history | 87.210 | 55.953 | 35.84% | 585,005 → 210,005 | 30.317 → 16.843 |
| dense unknown resolution | 189.378 | 9.671 | 94.89% | 53,567 → 24,433 | 1.845 → 0.935 |
| dense mixed resolution | 42.715 | 4.560 | 89.32% | 20,177 → 8,089 | 0.967 → 0.512 |
| mixed threshold duplicates | 69.911 | 11.353 | 83.76% | 49,676 → 21,589 | 1.715 → 0.898 |
| library search miss | 23.762 | 13.505 | 43.17% | 50,008 → 8 | 0.765 → 0.765 |
| metadata refresh | 2.576 | 2.287 | 11.22% | 153 → 153 | 0.015 → 0.015 |
| forced text rebuild | 85.637 | 41.888 | 51.09% | 573,156 → 150,155 | 22.104 → 9.656 |

The largest result is the dense unknown-resolution duplicate workload: 189.378 ms → 9.671 ms (19.6× faster). Its peak C++ heap falls 49.34%. The 25,000-record unknown-resolution pair workload now uses 54.53% less peak heap; this removes the previous pass's temporary-memory increase. Cold index construction falls 51.68%, with 45.41% less peak heap and 73.82% fewer allocation calls.

Unchanged flat/manual/extension/smart-playlist paths show modest timing variation, with identical allocation counts and output. The capped flat case rose from 0.652 to 0.746 ms in this run; its sort/filter algorithm and allocations are unchanged. Collapsed groups retain their sorting because aggregate-duration addition order must remain compatible. The capped grouped path improves by deferring sorting of groups that will never be emitted.

## Why the implementation is faster

### Search index and scoring

- Token caches store offset/length ranges into owned normalized title/path text instead of a separately allocated string for every token. Ranges survive entry moves, including small-string moves, because they contain offsets rather than pointers.
- Raw and normalized filenames borrow suffixes of the corresponding owned paths. The normalized suffix offset is computed during index construction; title matches do not touch filename data until the title checks fail. Projected filenames are copied only for retained results.
- Only playback-history paths require the temporary merge map. Libraries without history avoid per-media path-key normalization and the map entirely. When histories share aliases with multiple media records, the last media record still receives the merge.
- A necessary-condition check rejects multi-word queries when a token appears in neither normalized title nor path. Qualifying entries still receive the original full relevance score and ordering.
- Embedded NULs retain separate lowercase token backing text and bypass the prefilter. This preserves the old distinction between normalization, which trims boundary NULs, and tokenization, which preserves them. Unicode classification and case folding continue using the existing wide-character functions.

### Duplicate graph

- Exact file identities and normalized path aliases use sorted flat arrays. Each phase frees temporary keys before the content phase, rather than retaining a hash node/vector for every unique identity or path.
- Content candidates are ordered by size and duration. A gap greater than 0.5 seconds partitions independent components; two-item components use a direct comparison. This prevents the text-key sweeps from slowing sparse, separated pairs.
- Known resolutions are handled separately. For records at or above five seconds, adjacent duration edges span the same components as all matching pairs. Across the five-second boundary, the original rule remains asymmetric: metadata alone connects a short record only when the qualifying longer record appeared earlier in the input.
- Remaining candidates are grouped independently by equal nonempty stem, normalized title, and normalized filename. Within a compatible resolution, adjacent durations suffice. Unknown resolutions act as wildcard bridges: each known record joins the nearest eligible unknown predecessor and successor. Unknown components are separated by gaps greater than 0.5, so these neighbors cover all components intersecting the eligible one-second interval.
- Union-find stores negative component sizes and nonnegative parent indices in one signed array, with weighted union and iterative path compression. Only nonsingleton components are materialized, using another flat array.
- Keeper preference, exact-identity/path priority, displayed keys, original-item pointers, and deterministic final group order retain their previous semantics.

For a size/duration run of `k` candidates, the connectivity phase uses sorting and neighbor searches rather than examining `k(k−1)/2` pairs. Text comparison costs still depend on string length. Temporary text keys are scoped to that run, and output construction still necessarily costs space/time proportional to the reported duplicate entries.

### Library filtering and grouping

- Library filtering borrows the display title, avoids normalizing the same raw title twice, and removes the redundant filename pass because every normalized filename hit is also a normalized full-path hit. Its reusable inline buffer initializes only the characters actually written and still supports long heap-backed strings.
- Series grouping parses episode numbers and stems without copying raw path, filename, and directory strings. The public episode API retains all source fields and next-episode behavior. Directory parsing remains available for the empty-stem fallback.
- Series-stem normalization lowers characters as it builds the result, removing the intermediate lowercase string and an extra copied raw stem.
- Grouped queries sort each reached group immediately before projection, allowing a result cap to stop before sorting later groups. Emitted order, collapsed-group summaries, and complete-group aggregates remain compatible.

## Validation

- The unit suite passes all 402 tests, including a frozen owning-string search scorer and exhaustive ordering/count/result-limit comparisons across 384 records, 13 queries, index rebuilding, Unicode, and embedded NULs.
- Token delimiter behavior is checked against the old implementation for all 65,536 Windows wide-character code units.
- Duplicate components are compared against an independent brute-force graph traversal for 128 deterministic randomized libraries of 96 records. The cases include dense/sparse durations, partial or missing resolutions, empty titles, the five-second boundary, and transitive chains. Explicit tests cover an unknown-resolution bridge connecting two duration components and different known resolutions.
- Library filtering is checked against the previous owning traversal, including long strings, overrides, embedded NULs, and Unicode. Grouped caps are checked against full visible prefixes in both sort directions, with collapsed groups and summary totals. Public episode source fields and grouping keys are checked together.
- All 26 benchmark comparisons pass result-count, group-count, and ordered-hash equivalence checks. The full local Release build, 20-suite PR regression gate, package validation, and installation complete through `build_and_release.ps1`.

## Evidence and reproduction

Raw CSVs and comparison JSONs are in [performance-evidence](performance-evidence). Workloads are exposed through `run_query_benchmark.ps1` as the default set, `-DuplicatesOnly`, `-ExtendedOnly`, and `-ProgressOnly`. `compare_query_benchmark.ps1` accepts the corresponding switches. Both scripts reject conflicting workload selections.

Use the current harness against a separate source checkout of `17ed90a` and the current source. Keep harness output and checkouts outside the project workspace:

```powershell
$baselineSource = "C:\Temp\Aperture-before\src" # A checkout of 17ed90a
$benchmarkScratch = "$env:LOCALAPPDATA\Codex\tools\scratch\aperture-comparison"
.\tests\benchmark\run_query_benchmark.ps1 -SourceDirectory $baselineSource -OutputDirectory "$benchmarkScratch\before" -KeepBuild
.\tests\benchmark\run_query_benchmark.ps1 -OutputDirectory "$benchmarkScratch\after" -KeepBuild
.\tests\benchmark\compare_query_benchmark.ps1 -BeforeCsv "$benchmarkScratch\before\results.csv" -AfterCsv "$benchmarkScratch\after\results.csv" -OutputJson "$benchmarkScratch\default.json"

# Reuse the same compiled binaries for the extended workloads.
& "$benchmarkScratch\before\query-benchmark.exe" --extended | Set-Content "$benchmarkScratch\before-extended.csv"
& "$benchmarkScratch\after\query-benchmark.exe" --extended | Set-Content "$benchmarkScratch\after-extended.csv"
.\tests\benchmark\compare_query_benchmark.ps1 -BeforeCsv "$benchmarkScratch\before-extended.csv" -AfterCsv "$benchmarkScratch\after-extended.csv" -OutputJson "$benchmarkScratch\extended.json" -ExtendedOnly
```

Run timing comparisons without concurrent compilation/tests. Ordinary application validation and packaging use `build_and_release.ps1 -FailFast`. These measurements concern library CPU work and C++ heap use; they do not establish a playback-FPS, decoder, or GPU improvement.
