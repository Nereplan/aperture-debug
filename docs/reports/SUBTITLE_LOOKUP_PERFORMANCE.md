# Subtitle lookup performance — 5 October 2026

Baseline: `0373c27`. Implementation and regression/benchmark harness: `3c046a6`. Version remains `2.0.0.0`; builds, tests, and packaging run locally.

## Change

The rendered-text cache already avoids recomputing identical frames, but every cache miss walked all cues before the playback position. Later subtitle boundaries and seeks therefore scanned progressively more expired cues, including when only one cue remained visible.

Selected tracks now build a compact maximum-end-time tree with 32 cues per leaf. Lookup prunes expired subtrees and future cues, then visits remaining cues in original order. Long-running overlapping cues do not force scanning every intervening expired cue. Tracks of at most 64 cues keep the direct scan. Duplicate detection borrows cue text instead of allocating another string copy.

The index survives seeks and delay changes. Track identity, generation, cue storage, or cue-count changes invalidate it; in-place edits require the existing generation counter to advance. Clearing the engine also releases both text/index caches. Inclusive cue ends, empty/duplicate text, output limits, primary/secondary delays, and cue ordering retain their behavior.

## Measurements

Both versions use the same current `subtitle_lookup_benchmark.cpp`, MSVC 14.44.35207 x64, `/O2`, `/MT`, and C++17 on the same Windows machine. Both compile before measurement. Processes use the same logical processor; one discarded round precedes seven measured rounds with alternating version order. No application build or test ran during timing.

Each warm workload makes 2,000 calls through the real subtitle engine, mixing active cues, inclusive ends, and gaps. Seeks use a deterministic sequence; nested playback adds a cue spanning the whole track. Setup, the initial index construction, and a separate linear reference pass are excluded from warm timings. Every sample must match that reference and its corresponding baseline output hash. All 64 rows per version matched.

| Workload | Cues | Before ms | After ms | Time reduction |
|---|---:|---:|---:|---:|
| Small track | 500 | 0.6921 | 0.4304 | 37.8% |
| Typical-sized track | 5,000 | 4.4464 | 0.6211 | 86.0% |
| Large track | 250,000 | 310.1516 | 1.2786 | 99.6% |
| Large-track seeks | 250,000 | 343.1175 | 1.5422 | 99.6% |
| Nested playback | 250,000 | 333.5670 | 1.8748 | 99.4% |
| Repeated cached frames | 250,000 | 0.1490 | 0.1501 | -0.7% |

These are medians for the entire 2,000-call workload, not per-frame timings. The tiny cached-frame difference is consistent with measurement noise; no cached-frame gain is claimed.

Index construction is a deliberate one-time tradeoff per selected slot: the first lookup at time zero rises from 0.0010 to **0.0199 ms** for 5,000 cues and from 0.0021 to **1.0267 ms** for 250,000 cues. The trees contain 4 KiB and 128 KiB of doubles, respectively. Two selected maximum-size tracks can retain 256 KiB of index storage. Switching tracks may retain vector capacity until engine clear.

Warm typical-track allocations fall from 6,501 to 5,001; nested-playback allocations fall from 12,501 to 9,001 because duplicate detection no longer copies strings. Warm allocation peaks exclude the retained tree and input cues; cold rows include index construction. Requested C++ heap measurements exclude allocator overhead, process memory, operating-system allocations, and GPU resources.

This synthetic benchmark establishes subtitle lookup savings, not a whole-app CPU percentage, video frame-rate improvement, or decoding improvement. Highly overlapping output still requires visiting relevant cues, subject to the existing 64-cue/text-size caps.

Raw evidence: [before](../evidence/subtitle-lookup-evidence/before.csv), [after](../evidence/subtitle-lookup-evidence/after.csv), [comparison](../evidence/subtitle-lookup-evidence/comparison.json).

## Validation and delivery

- All 11 focused subtitle cases pass, including over 3,000 independent linear-reference comparisons across forward playback, exact ends, gaps, randomized backward seeks, and independent primary/secondary delays. Separate cases cover output caps, duplicate expiry, source mutation, track replacement, and engine clear.
- The final local build at `e44f2a9` passes all 20 PR regression suites: 389 unit cases, zero failures/skips, and zero suite timeouts. Static analysis introduces no new warnings.
- A 10,000-iteration subtitle fuzz run with seed `20261005` validates 186,058 cues, with zero invariant failures, crashes, or hangs.
- The generated ZIP's ten payload files match their declared sizes/hashes and both installed copies. `release/` and `debug/` receive the optimized Release executable while retaining their saved data. Binary SHA-256: `fa39f146fab74fb4db17094c33a1c166adc4b9ed856a3bfaa45ae63c5dbcbb0b`.
- The additional real-app playback/restart test is blocked by an already-open Aperture instance from another checkout. The harness exits before touching that session; no smoke-test pass is claimed. No GitHub release is published.

The local-release fixture also needed a one-line fix (`0f5b928`) to select the first Git executable when more than one is on PATH. The fixed fixture and final gate pass. Shared [validation evidence](../evidence/ui-refresh-evidence/validation.json) records the final source and package identity.

## Reproduce

Export baseline `0373c27` into an external scratch directory, then run:

```powershell
./tests/benchmark/run_subtitle_benchmark.ps1 -BaselineSourceDirectory '<baseline>/src' -OutputDirectory '<external-results>'
./tests/unit/run_tests.ps1 -Suite SubtitlesDual
./build_and_release.ps1 -FailFast
```

The benchmark compiles both source versions with the same harness, checks outputs, saves CSV/JSON evidence, and removes its temporary binaries after completion or failure.
