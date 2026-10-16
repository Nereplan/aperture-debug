# Backend loading performance — 16 October 2026

This pass changes only the JSON and media-library backend source files. The
starting tree was `9a053d3` plus the existing uncommitted work, captured before
editing. Existing frontend source files are unchanged. No runtime dependency,
store-format migration, GitHub workflow or published release was added.

## Pipeline change

Ordinary library checkpoints now decode one JSON record at a time directly into
the typed catalog, instead of retaining a complete JSON tree. A bounded capacity
hint avoids repeatedly reallocating the catalog. Short records use explicit moves;
unused capacity and partially decoded records are released. Unknown fields,
Unicode, duplicate-path handling, preferences and bookmarks retain their behavior.

Versioned wrappers, incremental journals and backup recovery retain the existing
compatibility loader. A journal appearing during a streamed read forces a reload.
Malformed suffixes cannot publish partial records or overwrite unrecovered data.
Parser depth, string-size, input-size and element-count limits remain enforced.

## Measurements

Optimized Windows x64 MSVC builds used the same save/load harness, one warmup
and seven measured samples per workload. Results below compare the starting source
snapshot with this change. All saved store sizes and content hashes match across
all six recorded runs. Source hashes and raw samples are in
[the evidence directory](../evidence/loading-20261016/).

| 25,000-item library load | Before | After |
|---|---:|---:|
| Peak tracked C++ heap, initial matched-size harness | 55,853,970 B | 30,507,580 B |
| Retained tracked C++ heap, same harness | 25,588,179 B | 25,588,179 B |
| C++ allocation calls | 1,008,775 | 929,115 |
| Total allocated C++ bytes, same harness | 116,121,051 B | 104,560,905 B |
| Serialized stores | 9,024,568 B | 9,024,568 B |
| Initial baseline / final-build latency medians | 238.261 ms | 257.123 ms |
| Consecutive reversed-order latency medians | 224.930 ms | 207.247 ms |
| Consecutive forward-order latency medians | 276.527 ms | 227.509 ms |

Peak tracked heap falls **45.4%** and allocation calls fall **7.9%**. Timing varies:
the consecutive pairs improve, while the earlier comparison regresses. No reliable
wall-clock speedup is claimed. The 500-item workload also varies, from roughly
30 ms in the initial batch to 6.4 ms in the reversed batch. All samples, including
the regressions, are retained. Small allocation differences between rerun directories
come from different executable-path lengths.

Metadata-heavy records have less room for savings because the retained model
dominates memory. The resource regression test therefore checks both sparse and
fully populated records: 6,000-record peaks are 8,887,618 versus 9,874,788 bytes
for sparse records, and 17,997,628 versus 18,255,588 bytes for dense records
(streaming versus compatibility loading).

The actual Release startup gate passed with an isolated empty profile, one warmup
and five measured fresh processes: median window shown 21.86 ms, first GPU shell
38.46 ms, ready library frame 290.72 ms, working set 57,790,464 B and private bytes
60,989,440 B. This is a current-build budget check, not a matched startup comparison
or a reboot-cold measurement. C++ heap probes are not whole-process RAM or VRAM.
Video/decoder settings are unchanged; no VRAM reduction is claimed.

Persisted disk bytes are unchanged, and no new caches or runtime files are needed.
The rebuilt portable archive is 13,415,767 bytes. Existing cache ceilings remain
in effect. Store writes and test installations stay inside their respective
portable roots; unwritable-root checks reject external fallback storage.

## Tests and delivery

- Added [eight independent loading tests](../../tests/loading/README.md) covering
  actual restart/relocation, capacity growth, syntax limits, corruption/backup
  recovery, incremental replay/checkpointing, future-schema protection and heap bounds.
- Restored individual selection/reporting for 17 existing save-worker scenarios;
  removed assertions that only checked whether function addresses were non-null.
- Both deliberate production defects were detected by their specific assertions:
  accepting trailing JSON and disabling streaming. Compiler failures do not count.
- `build_and_release.ps1 -FailFast`: **22/22 suites passed**, including native static
  analysis, shutdown stress, portable storage, malformed-input and thumbnail workflows.
  The final added growth case was then included in a separate **8/8** loading run.
- The actual startup budget gate passed. Release and local debug-directory runtime
  copies were updated by the standard local workflow, retaining saved data.
  No GitHub release was published.

Reproduce with `tests/loading/run_tests.ps1`,
`tests/loading/verify_mutations.ps1`, and
`tests/benchmark/run_save_benchmark.ps1 -SourceDirectory <src> -OutputDirectory <outside-checkout> -KeepBuild`.
For the application budget check, run
`scripts/run_benchmark.ps1 -BinaryPath release/Aperture.exe -SkipBuild`.
