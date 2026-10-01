# Backend optimization — 1 October 2026

Baseline: commit `502f648`. Six Luna agents worked in two waves, covering persistence, queries, caches, JSON serialization, batch imports and playback workers. All builds and verification run locally.

## Changes

- JSON serialization appends into an existing string, copies unescaped string runs together, and formats integers without a stream. Fractional values keep the existing stream precision. Pretty output, escapes, sorted keys and nonfinite values retain their representations.
- Library saves serialize ordinary scalar media records directly into the store buffer. Records with unknown fields, preferences, overrides or bookmarks use the existing object overlay. Every save still observes public record mutations, compares the complete output, rotates backups and replaces files atomically.
- Cache writes use their actual atomic temporary file as the writability check, removing a separate create/close/delete cycle. Explicit writability probes, read-only checks, budgets and access-denied error categories remain available.
- Import completion moves staged media rather than copying whole vectors. Large playlist batches use sorted indices and an ordinal case-insensitive merge, preserving existing entries and the first incoming spelling/order. Small drops retain a linear scan. A repeated candidate group is scanned once.
- The player waits indefinitely for mpv events when the optional `mpv_wakeup` export exists. Shutdown wakes the thread before joining and unloading the DLL. DLLs without that export retain the previous 50 ms wait.
- Custom folder ordering uses the already-normalized group key instead of normalizing it again.

## Measurements

Raw evidence is kept in `scratch/backend-optimization/` and `diagnostics/thumbnail-storage-benchmark.json`. CPU benchmarks use MSVC `/O2`, the same workloads and one logical processor. Allocation probes count C++ heap allocation requests, not process RAM or GPU memory.

The save harness uses real isolated profile writes, unchanged-write checks and backups, with seven samples after a warmup. The controlled comparison ran the saved baseline and optimized executables sequentially after other compilation/measurement activity had finished. All four stores had identical byte counts and content hashes for each corresponding workload. Unknown nested fields, escapes, Unicode, large file identities and mutated progress were also verified by reloading.

| 25,000-entry workload | Before | After | Reduction |
|---|---:|---:|---:|
| First save | 495.168 ms | 149.843 ms | 69.7% |
| Unchanged save | 452.209 ms | 110.298 ms | 75.6% |
| Progress save | 473.954 ms | 127.907 ms | 73.0% |
| Load | 392.262 ms | 392.291 ms | unchanged |

Progress saves made **1,098,385 → 524,986 C++ allocation requests (52.2% fewer)**. Peak live C++ allocation stayed around 24.8 MB for this workload; first-save peak increased about 3.2% due to buffer growth. No process-RAM reduction is claimed. The 500-entry progress-save median was 17.271 → 7.798 ms (54.8% lower).

The cache storage benchmark alternated old/new order for three runs of 400 synthetic 8 KiB files. Median write time was **1,169.45 → 937.05 ms (19.9% lower)**. Every read reproduced the original bytes and the final file count was checked. Read timing varied; no read-speed improvement is claimed.

The playlist benchmark alternated old/new order for seven measured samples after a warmup. Each sample compared the complete result vector with the former sequential membership scan.

| Membership workload | Sequential | Batch | Speedup |
|---|---:|---:|---:|
| Empty playlist, 2,000 new paths | 23.535 ms | 0.704 ms | 33.4× |
| 10,000 existing paths, 1,000 candidates, half new | 127.104 ms | 3.316 ms | 38.3× |
| 10,000 existing paths, one new path | 0.262 ms | 0.242 ms | effectively unchanged |
| 10,000 existing paths, 2,000 identical new candidates | 250.117 ms | 2.835 ms | 88.2× |

These measurements isolate playlist membership; metadata probing and actual end-to-end import times are excluded.

The event wait removes the player thread's 20 empty polling wakeups per second on the bundled mpv API. This is a control-flow change, not a measured claim about total application CPU or battery usage.

## Rejected change and limits

An attempted replacement of flat-query `stable_sort` with `sort` reduced one scratch allocation but slowed the 25,000-entry workload: three alternating process pairs showed medians of 15.559 → 20.248 ms. It was reverted. Query benchmarks also cover capped/manual/near-unlimited projections, cold index construction, broad/multiple-token/rejected searches and media extension checks, comparing item counts, group/match counts and result-order hashes. No broad search-speed or startup/load-speed improvement is claimed.

The final query comparison used three alternating process pairs, seven measured samples per process. All 13 workloads retained the same signatures. The 500-folder workload eliminated 500 allocations and 24,000 requested bytes from redundant normalization; its timing was effectively unchanged (22.751 → 22.675 ms).

Library persistence still scans and serializes the full library to honor publicly mutable records. There is no new asynchronous writer or storage format. Decoder/presentation behavior and UI rendering are outside these changes.

## Behavioral verification

The unit suite passed **334 tests**, including a seeded sequential membership oracle for both small and large playlist batches and Unicode casing/decomposition cases. Persistence passed **106 tests**, covering unknown fields, portable relocation, recovery/backups and read-only cache-write rejection. Seeded library workflows also checked scalar/general save-path byte equivalence.

The JSON mutation harness passed **10,101 cases**, including 4,102 successful round trips. The real application passed 102 automation commands covering load, advancing playback, pause, seek, queue navigation, stop/reopen and three restart cycles with saved resume positions. These runs used generated media and isolated profiles.

The initial full gate could not start its Python boundary validator through the Windows Store alias in the sandbox. The final run puts the installed interpreter first on the test process's `PATH`; application code and security checks were unchanged to resolve that environment issue.

The local regression gate passed **19/19 suites**, including corpus, IPC, DLL lifecycle, updater, package/data retention, boundary validation, static analysis and 20 asynchronous shutdown stress cases. `build_and_release.ps1` completed package creation, validation, installation and temporary-directory cleanup. The packaged binary was installed in both `release/` and `debug/`; both copies and the real-app-tested build have SHA-256 `9110830675820ff8936ae5a40cbd4d64e17cb8d98420f22ff5fa4de713c40f56`. All **52 existing profile/config files** retained their original hashes. The local portable ZIP is `dist/Aperture-2.0.0.0-windows-x64.zip`; its executable hash matches, and it excludes profiles, caches, backups and debug symbols. No GitHub release was published.

## Reproduce

Export the baseline with `git archive --format=zip --output=baseline.zip 502f648 src`, expand it into a separate directory, then use:

```powershell
tests/benchmark/run_save_benchmark.ps1 -SourceDirectory <baseline>/src -OutputDirectory <before> -KeepBuild
tests/benchmark/run_save_benchmark.ps1 -OutputDirectory <after> -KeepBuild
tests/benchmark/compare_save_benchmark.ps1 -BeforeCsv <before>/results.csv -AfterCsv <after>/results.csv -OutputJson <comparison>.json
tests/benchmark/run_query_benchmark.ps1 -SourceDirectory <baseline>/src -OutputDirectory <before-query> -KeepBuild
tests/benchmark/run_query_benchmark.ps1 -OutputDirectory <after-query> -KeepBuild
tests/benchmark/compare_query_benchmark.ps1 -BeforeCsv <before-query>/results.csv -AfterCsv <after-query>/results.csv -OutputJson <query-comparison>.json
tests/benchmark/run_playlist_benchmark.ps1
tests/thumbnails/benchmark_storage.ps1 -Corpus <bin-file-corpus> -BaselineHeaderDirectory <baseline>/src
build_and_release.ps1
tests/fuzz/run_json_fuzz.ps1 -Iterations 10000
tests/workflows/run_tests.ps1 -AppOnly -Executable release/Aperture.exe
```
