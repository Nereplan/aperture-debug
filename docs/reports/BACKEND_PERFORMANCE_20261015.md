# Backend performance and behavioral tests — 15 October 2026

Baseline: initial source snapshot at `9a053d3`, including the user's existing
uncommitted changes. Those files were preserved byte for byte. This work changes
backend storage, JSON and settings code; it adds no frontend changes or runtime
dependencies. Measurements use local Windows x64 optimized MSVC builds.

## Changes

- Cache scans classify each directory once, instead of repeatedly resolving
  class roots for every file. Per-file containment checks remain. Scanning and
  cleanup skip directory links, including a linked cache root, to avoid walking
  outside the portable cache.
- JSON number formatting uses a stack buffer instead of allocating streams.
  The existing six significant digits and ordinary serialized bytes are retained.
  Formatting is independent of the process locale. The positive `2^63` boundary
  no longer undergoes an out-of-range signed integer cast.
- Library loading consumes nested preference, bookmark and override objects in
  place, releases processed top-level DOMs sooner, and drops empty extension maps.
  Unknown fields still survive saves and relocation. Empty maps no longer retain
  an MSVC map sentinel per record or copy those allocations into save snapshots.
- Reset/fresh settings now take cache defaults from `CacheBudgetConfig`. The old
  reset implementation silently restored 512 MiB despite the documented 160 MiB
  policy. Defaults are now 160 MiB total: 64 thumbnails, 64 shaders, 16 subtitles,
  16 generic. Explicit saved user budgets are preserved. Existing caches are
  reduced by the ordinary eviction policy; no media or durable stores are purged.

## Measurements

The 25,000-item library benchmark uses Unicode paths/titles, nested extensions,
playback preferences, playlists and history. One warmup and seven measured samples
were run per workload, then both versions were run again in reversed order.

| 25,000-item workload | Before | After | Interpretation |
|---|---:|---:|---|
| First save, forward run | 146.438 ms | 117.931 ms | 19.5% faster |
| First save, reversed run | 159.758 ms | 117.561 ms | 26.4% faster |
| Load, forward run | 246.982 ms | 248.503 ms | 0.6% slower |
| Load, reversed run | 236.275 ms | 225.751 ms | 4.5% faster |
| Retained C++ heap after load, forward run | 28,250,579 B | 25,588,179 B | 9.4% lower |
| Peak tracked C++ heap during load, forward run | 55,854,466 B | 55,853,970 B | Essentially unchanged |
| Serialized stores | 9,024,568 B | 9,024,568 B | Identical bytes and hashes |

No consistent library-load latency improvement is claimed. Smaller workloads also
show timing variation: in the reversed run, 500-item unchanged saves rose from
0.395 to 0.482 ms, progress saves from 5.655 to 6.275 ms, and loads from 5.623 to
5.960 ms. Raw samples include these results rather than reporting only wins.

C++ heap measurements are not whole-process RAM or VRAM. GPU/decoder settings and
video quality are unchanged; no VRAM reduction is claimed. Startup measurements
are a separate current-build budget check, not a before/after startup comparison.

## Tests

The new [backend suite](../../tests/backend/README.md) runs production load/save
and cache code against real isolated files. It verifies settings defaults and
custom budgets, nested extensions, preferences, bookmarks, relocation, history
ordering, locked-file eviction, byte ceilings, traversal rejection and junction
containment. Numeric cases compare against an independent classic-locale oracle,
exercise boundary values/a comma locale, and enforce allocation-free formatting.

Ten weak tests were removed, including copied persistence implementations, a
manually populated load-result test and a scheduler test that tested its own
local guard. Three more tests now call the actual loader and writer for invalid
preferences and partial corruption. Four unused fixture files were removed.
Useful migration, recovery, import, concurrency, codec and engine tests remain.

The manual mutation suite independently breaks bookmark extension preservation,
settings extension preservation and cache-budget enforcement in isolated source
copies. Each must fail its specific behavioral assertion; compiler failures and
unrelated test failures are rejected as invalid evidence.

## Reproduction and evidence

- `./tests/run_tests.ps1 -Id backend`
- `./tests/backend/verify_mutations.ps1`
- `./tests/backend/run_benchmark.ps1 -OutputDirectory <output>` (also supports `-SourceDirectory <baseline/src>`)
- `./tests/benchmark/run_save_benchmark.ps1 -OutputDirectory <output> -KeepBuild` (same baseline option; retained executables permit reversed runs without recompilation)
- `./build_and_release.ps1 -FailFast`

[Raw samples and source provenance](../evidence/backend-20261015/) include
matching output hashes and both save/load run orders. Test installations, baseline
sources, compiler products and large logs are development artifacts outside the
checkout. Each tested application writes its data inside its own installation.
No GitHub release or workflow was created.
