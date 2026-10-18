# Thumbnail cache removal — 14 October 2026

Baseline: `9a053d3`, Windows x64, MSVC 14.44.35207, C++17 Release. Existing
uncommitted frontend edits are preserved. This pass changes backend thumbnail
keys and removal only; the application version is unchanged.

## Change

Removing a video previously attempted one filesystem delete per second of its
duration, plus the hub and fractional-end thumbnails. A two-hour video triggered
more than 7,200 deletion attempts even with no seekbar thumbnails on disk. Each
attempt also converted the same media path to UTF-8 and formatted a key through
a stream.

Removal now hashes the path once. For durations above 64 seconds it lists existing
seekbar cache names, sorts their numeric hashes and skips keys absent from that
inventory. Empty caches return immediately after the hub deletion attempt. The
inventory retains at most the smaller of the duration's integer bucket count and
16,384 hashes; listing stops at the first candidate over that bound. Incomplete enumeration or an exceeded bound falls back
to direct deletion. Short clips use direct deletion without listing the directory.

The shared key builder uses decimal conversion and direct hexadecimal formatting.
It retains the exact existing FNV key format, so cached thumbnails need no
migration. Inventory reads names rather than thumbnail headers, preserving cleanup
of empty and corrupt entries. Cache I/O remains under the existing mutex. Failed
deletions, including locked files, remain retryable.

No persistent index, dependency, new cache format or additional application file
is introduced. All application paths retain their existing portable-root handling.
The inventory is transient; this change does not target playback VRAM.

## Measurements

The comparison compiles both production implementations before measuring, then
alternates process order for one warmup and seven measured samples per version.
Each process runs on one available logical processor. Cache fixture creation,
verification and process startup are outside timing. Every sample checks the
exact removed count, absence of target entries and survival of unrelated entries.

| Workload | Target files removed | Unrelated files | Before median | After median |
|---|---:|---:|---:|---:|
| Empty two-hour cache | 0 | 0 | 846.16 ms | 0.70 ms |
| Sparse two-hour cache | 9 | 128 | 887.45 ms | 6.70 ms |
| Populated two-hour cache | 67 | 4,096 | 1,010.42 ms | 31.05 ms |
| Dense thirty-minute cache | 1,603 | 0 | 577.76 ms | 544.02 ms |
| 65-second clip, listing limit exceeded | 5 | 128 | 10.45 ms | 11.83 ms |
| Twenty-second clip in large cache | 5 | 4,096 | 4.41 ms | 4.26 ms |

Sparse two-hour removal takes **99.2% less time**; the populated case takes
**96.9% less time**. Dense deletion remains dominated by deleting actual files.
The bounded fallback case regresses by **1.38 ms (13.2%)** in this run because it
can pay for both listing and direct deletion. This is a tradeoff, not a claim that
every removal becomes faster.

| Requested C++ heap | Before | After |
|---|---:|---:|
| Sparse two-hour total allocated | 4,835,574 bytes | 9,630 bytes |
| Sparse two-hour allocation count | 50,432 | 37 |
| Sparse two-hour peak live | 1,174 bytes | 2,382 bytes |
| Populated two-hour peak live | 1,214 bytes | 72,516 bytes |
| Dense thirty-minute peak live | 1,174 bytes | 32,556 bytes |

All measured cases retain zero tracked heap after removal. The transient inventory
increases peak memory while sharply reducing allocation churn and filesystem
misses. Peak numbers include vector growth; the entry bound is not a promise of
exactly eight bytes of peak memory per entry.

These are local measurements with warm filesystem metadata and empty payloads,
including corrupt cache entries. They exclude Windows enumeration buffers,
allocator overhead, process RSS, GPU memory and whole-library removal work. They
do not establish cold-disk or network-share performance. Nonempty inventories
still require an O(duration) hash loop; extremely large durations are not made
constant-time. Concurrent external cache edits remain best-effort, while app
writers are serialized by the existing mutex.

Raw samples, executable hashes and medians:
[thumbnail-purge-20261014](../evidence/thumbnail-purge-20261014/).
Reproduce with `tests/benchmark/compare_thumbnail_purge.ps1`, supplying the baseline
source directory and a harness scratch output directory.

## Validation

- Focused thumbnail tests: **15 passed**, including legacy key compatibility,
  Unicode and long media paths, fractional and maximum supported timestamps,
  corrupt/empty entries, uppercase names, locked files and retries, unrelated
  entries, directories, inventory overflow, empty caches and unknown durations.
- Full local build and regression results are recorded with the evidence after
  the build completes.
