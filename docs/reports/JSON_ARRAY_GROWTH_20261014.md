# JSON array ownership during loading — 14 October 2026

Baseline: `fbb9eb4`. Backend-only parser change on Windows x64 with MSVC
14.44.35207, `/O2`, C++17.
All ten pre-existing modified frontend source files remained byte-identical to
their initial SHA-256 hashes.

## Change

MSVC's `std::map` move constructor can allocate its empty sentinel. Consequently,
`JsonValue` is not nothrow-move-constructible, and ordinary `std::vector` growth
copies existing JSON objects and all their nested fields. Large library arrays
repeat those deep copies at each capacity boundary.

The parser now explicitly moves its unpublished array entries into a replacement
buffer when capacity is exhausted. It keeps 1.5x growth, capped at the existing
one-million-element limit. An allocation exception still abandons the entire
parse; partially moved parser state is never published. Ordinary public JSON
copy semantics and array insertion APIs are unchanged.

This adds no dependency, persistent index or storage format. Portable paths,
backup recovery, unknown fields, duplicate-key behavior and parser limits remain
in place. No playback or GPU code changes are included.

## Measurements

| Load workload | Before | After | Reduction |
|---|---:|---:|---:|
| 500 items, median load time | 30.384 ms | 28.509 ms | 6.2% |
| 25,000 items, median load time | 382.792 ms | 240.505 ms | 37.2% |
| 500 items, peak tracked C++ heap | 1,731,573 bytes | 1,401,955 bytes | 19.0% |
| 25,000 items, peak tracked C++ heap | 68,215,717 bytes | 65,854,066 bytes | 3.5% |
| 25,000 items, allocation calls | 2,165,977 | 1,077,721 | 50.2% |
| 25,000 items, cumulative allocated bytes | 224,897,851 | 132,371,435 | 41.1% |

The large-library peak falls less than allocation traffic because the parsed
document still coexists with the reserved typed-library storage during conversion.
These are requested C++ allocation bytes, excluding allocator overhead, process
RSS and VRAM. This is a library-load benchmark, not a measurement of window
creation or first-frame latency.

`tests/benchmark/run_save_benchmark.ps1` compiled the same harness against a
snapshot of the original source and the updated source. Each workload discarded
one warmup and retained seven samples, pinned to one available logical processor,
with warm OS file caches. Baseline and updated runs were sequential, so timing
changes in the unchanged save paths should be treated as noise, not benefits of
this parser change. No heavy build ran during these measurements.

Both versions wrote identical content hashes and byte counts for all workloads.
The 25,000-item profile contains 9,024,568 serialized bytes. No data-size or VRAM
reduction is claimed. Raw samples and the comparison are in
[json-array-growth-20261014](../evidence/json-array-growth-20261014/).

## Validation

- Added nested-array coverage across many growth boundaries, including all JSON
  payload types, escaped Unicode, duplicate keys, long strings, independent deep
  copies, round trips, input lifetime and malformed trailing input.
- Added exact one-million-element acceptance and limit-plus-one rejection.
- JSON mutation fuzz passed seed 1337: 10,101 cases, 4,102 accepted and
  round-tripped, 5,999 rejected; all invariants passed.
- `build_and_release.ps1 -FailFast` built successfully and passed all **20/20**
  regression suites in **444.4 seconds**, including **443 unit tests**, **110
  persistence tests**, **61 corpus fixtures**, **8,524 thumbnail assertions**,
  static analysis, portable containment and shutdown stress.
- The optimized package was verified and installed into workspace `release/` and
  `debug/`, retaining their saved data. The executable remains 3,280,896 bytes;
  the local ZIP is 13,358,826 bytes. No GitHub release was published.
- `scripts/run_benchmark.ps1 -SkipBuild -BinaryPath ./release/Aperture.exe
  -Iterations 5 -WarmupIterations 1 -Json` passed all startup/resource budgets.
  Empty-profile medians: GPU shell 36.98 ms, window shown 22.39 ms, populated
  library frame 254.10 ms, working set 58,105,856 bytes, private bytes 61,739,008.
  This is a current-build budget check, not a before/after startup comparison.
- `git diff --check` passed for all changed files.

The compiler probe measured `sizeof(JsonValue) == 40` and
`std::is_nothrow_move_constructible_v<JsonValue> == false`, confirming the
allocation mechanism on the tested toolchain.
