# Library record memory — 14 October 2026

Baseline: `4fcf456`, Windows x64, MSVC 14.44.35207, C++17 Release. Existing
uncommitted frontend edits are included in both app builds and preserved.

## Change and tradeoff

Playback preferences were stored inline in every media record, including records
without any preferences. They now use a small owning optional value that allocates
only when present. Each media record is **424 bytes instead of 824 bytes**. Copies
clone their preferences, preserving independent library copies, undo/save snapshots
and active playback settings. Moves transfer ownership. Metadata overrides stay
inline because display-title queries use them frequently.

The JSON representation, schema, portable paths and backup behavior are unchanged.
No dependency, persistent cache or additional application file is introduced.
The helper allocates ordinary memory and adds no long-lived allocation pool.

This trades one heap allocation per present preferences object for a smaller
record array. Fully populated records have no reduction in requested retained
bytes, incur additional allocator overhead and are slower to construct/copy.
The benefit depends on how many library items have custom preferences.

## Record benchmark

`run_media_record_benchmark.ps1` measures 500 and 25,000 synthetic records with
preferences and title overrides on 0%, 10% or 100% of items. The comparison uses
three alternating before/after process pairs. Each process discards one warmup
per workload and measures seven samples, giving 21 samples per version/workload.
Execution is pinned to one available logical processor; no heavy build runs
during measurement. Matching hashes verify display titles, progress and presence
flags. Save/load regression checks separately verify complete stored contents.

| 25,000 records | Before | After |
|---|---:|---:|
| Retained heap, no optional fields | 23,813,879 bytes | 13,813,879 bytes |
| Retained heap, 10% optional fields | 24,613,719 bytes | 15,613,719 bytes |
| Retained heap, 100% optional fields | 31,812,279 bytes | 31,812,279 bytes |
| Copy time, no optional fields | 8.2192 ms | 5.3339 ms |
| Copy time, 10% optional fields | 9.0494 ms | 6.7961 ms |
| Copy time, 100% optional fields | 16.4451 ms | 19.6112 ms |
| Title/progress scan, 10% optional fields | 0.6033 ms | 0.5397 ms |
| Title/progress scan, 100% optional fields | 0.7832 ms | 0.9471 ms |

At 10% occupancy, retained record memory falls **36.6%** and copying takes
**24.9% less time**. At 100%, copying takes **19.3% more time**, or 3.17 ms more
for all 25,000 records. Dense construction increases from 16.4243 to 19.7526 ms.
These requested C++ allocation totals include the record vector and its fields,
but exclude allocator overhead, library indexes, process RSS and VRAM. Scan
timings below one millisecond should not be treated as end-to-end response times.

## Save and load

The existing save benchmark uses 10% optional-field occupancy, one warmup and
seven measured samples per workload, with a baseline run followed by an updated
run. Every workload retains identical serialized byte counts and content hashes.
The 25,000-item durable profile remains **9,024,568 bytes**.

| 25,000-item workload | Before | After |
|---|---:|---:|
| First save, peak tracked heap | 44,749,144 bytes | 35,749,144 bytes |
| Load, peak tracked heap | 65,854,066 bytes | 55,854,466 bytes |
| First save, median time | 143.10 ms | 138.40 ms |
| Unchanged save, median time | 7.81 ms | 6.85 ms |
| Progress save, median time | 25.05 ms | 29.12 ms |
| Load, median time | 242.49 ms | 236.28 ms |

Large-library peak heap falls **20.1% during the first save** and **15.2% during
loading**. The measured progress-save regression is retained in the evidence;
sequential measurements include real disk publication and backup work. Small
500-item loads also rise from 26.62 to 27.69 ms while peak heap falls 14.2%.
This change is primarily a library-memory improvement, not a universal speedup.

A separate `--capture-only` run isolates snapshot capture from serialization and
disk publication. At 25,000 items, unchanged capture changes from 8.206 to 6.694 ms
and progress capture from 9.155 to 6.580 ms (seven measured samples after a
warmup). This shows the smaller records help snapshot comparison; it does not by
itself establish the cause of the full progress-save timing variation above.

Raw records, save measurements and summaries:
[library-record-memory-20261014](../evidence/library-record-memory-20261014/).

## Application comparison

The saved baseline executable and updated build ran with identical isolated
25,000-item profiles through `run_startup_comparison.ps1 -Samples 9
-LibrarySize 25000 -OutputRoot <task-scratch>`. Each variant discarded one warmup;
nine fresh processes per variant then alternated order with warm OS caches. The
fixture has no per-item preferences. Both binaries contain the same pre-existing
frontend edits; no other source changes were made between them.

| Median application metric | Before | After | Change |
|---|---:|---:|---:|
| Working set | 141,950,976 bytes | 132,096,000 bytes | 6.9% lower |
| Private bytes | 147,509,248 bytes | 138,170,368 bytes | 6.3% lower |
| Populated library frame | 294.83 ms | 284.20 ms | 3.6% sooner |
| Window shown | 22.47 ms | 22.55 ms | 0.08 ms later |

The first-populated-frame milestone measured from process entry independently
changes from 293.274 to 282.616 ms. These are warm-cache startup measurements,
not playback or VRAM measurements. Working set and private bytes overlap and
must not be added together. All samples reported library readiness.

## Validation

- `build_and_release.ps1 -FailFast`: optimized build and all **20/20** PR suites
  passed in **428.79 seconds**, including **445 unit tests**, **110 persistence
  tests**, **61 corpus fixtures**, static analysis, portable containment,
  shutdown stress and **8,524 thumbnail assertions**.
- New behavioral tests exercise independent library/save-snapshot values after
  nested edits, vector growth and record deletion, then persist and reload the
  snapshot. Active playback preferences are also checked for independent
  ownership and clearing.
- Both local app copies were updated with saved data retained. The executable
  is 3,281,920 bytes, **1,024 bytes larger**. The ZIP is 13,359,197 bytes,
  **371 bytes larger**. No GitHub release was published.
- The ordinary empty-profile startup/resource budget gate also passed one
  warmup plus five measured processes, with no reported regressions.
- The ten initially modified frontend source files retained their original
  SHA-256 hashes. Changed files passed `git diff --check`.
