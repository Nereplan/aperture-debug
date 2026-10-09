# Backend validation and benchmarks — 9 October 2026

Validated source: `df21d93` (optimization series through `c07d5a5`, plus fixes found by this run). Comparison baseline: `bd5db22`, immediately before the backend work-reduction series. This is a local Windows x64 Release/MSVC run.

The main measured improvement is a 25,000-item progress save: **108.119 ms to 24.179 ms (77.64% less time)**. Results are mixed elsewhere; the query regressions below remain. Startup met every checked-in budget. These measurements do not establish universal speedups.

## Tests and fixes

The initial `tests/run_tests.ps1 -Tier pr` run passed 18 of 20 suites in 455.45 seconds, with no timeouts. It found one failing unit case and one new static-analysis warning.

- `IncrementalSaving::FailedDeltaAndCheckpointDoNotAcknowledgeNewState`: copying a read-only source also made its recovery copy read-only, causing later backup rotation and save retries to fail. Newly created recovery copies now have that inherited flag cleared. Source protection is preserved.
- Static analysis found a shadowed `headerY` in the history fallback renderer. The inner variable was renamed.

After those fixes, the complete unit suite passed **443/443**, persistence passed **105/105**, and static analysis passed with no new warnings. These three suites were rerun together (213.43 seconds); the other 17 passing suites were not repeated. Thus all 20 selected suites have passing results across the initial run and targeted recheck. The standalone native application also built successfully.

The unit pass includes incremental save/recovery, import fingerprints, folder rediscovery, background watcher configuration, and background availability. The initial gate also passed seeded library workflows (1,520,907 checks), shutdown stress, media-probe isolation, thumbnail workflows, IPC, loader, diagnostics, updater, package boundaries and manifest checks. No release was published.

## Method

Query and save benchmarks used the same checked-in harness compiled against both source versions. An initial seven-sample comparison was followed by three alternating before/after rounds using already compiled binaries, with no concurrent builds or tests. Each process discards a warmup per workload and records seven samples: the tables use the median of **21 samples per version**. Query result/order hashes and save-content hashes matched throughout. Save workloads include real isolated disk writes, backups and reload verification.

Maintenance uses seven measured alternating samples after a warmup, with matching result hashes. Playlist measurements compare the current batched algorithm against the harness's sequential reference implementation, not against the Git baseline. Startup uses the standard isolated **empty profile**, one warmup and five measured processes; OS caches were not flushed.

## Save results

| Items | Workload | Before ms | After ms | Time change |
|---:|---|---:|---:|---:|
| 500 | First save | 15.709 | 15.471 | -1.52% |
| 500 | Unchanged save | 0.360 | 0.315 | -12.50% |
| 500 | Progress save | 5.667 | 4.185 | -26.15% |
| 500 | Load | 26.856 | 28.735 | +7.00% |
| 25,000 | First save | 139.644 | 144.469 | +3.46% |
| 25,000 | Unchanged save | 7.668 | 7.592 | -0.99% |
| 25,000 | Progress save | 108.119 | 24.179 | -77.64% |
| 25,000 | Load | 377.421 | 365.257 | -3.22% |

These drivers use the full-save path, **not the app's incremental writer**. They therefore measure serialization reuse and related changes, not incremental-save latency. Peak tracked C++ heap for a 25,000-item progress save rose from 20,138,546 to 20,538,955 bytes (about 2%); load allocations were nearly unchanged. This allocation probe does not measure total process memory.

## Query results

All workloads use 25,000 candidate records; capped results and search results return fewer items. Positive time change means slower.

| Workload | Before ms | After ms | Time change |
|---|---:|---:|---:|
| Flat view | 14.209 | 13.587 | -4.38% |
| Folder view | 21.892 | 22.422 | +2.42% |
| Series view | 26.896 | 28.020 | +4.18% |
| Capped view | 0.747 | 0.697 | -6.69% |
| Manual playlist | 6.679 | 6.611 | -1.02% |
| Limited smart playlist | 0.449 | 0.574 | +27.84% |
| Broad search | 0.421 | 0.486 | +15.44% |
| Nearly unlimited view | 12.946 | 13.294 | +2.69% |
| Extension filtering | 2.028 | 2.577 | +27.07% |
| Search index build | 43.018 | 43.830 | +1.89% |
| Multi-token path search | 5.627 | 5.787 | +2.84% |
| Multi-token title search | 3.543 | 3.938 | +11.15% |
| Search miss | 2.393 | 0.001 | -99.96% |

The largest relative regressions were the limited smart playlist and extension filtering (about 28% and 27% slower); their absolute increases were about 0.12 ms and 0.55 ms. Multi-token title search increased by about 0.39 ms. These remain observed regressions rather than being dismissed as noise. Most query allocation peaks were unchanged; search-index construction added 200,000 tracked bytes. Search misses improved to the harness's 0.001 ms reporting resolution, so the apparent multiplier should not be overinterpreted.

## Maintenance and playlist measurements

| Maintenance workload | Before ms | After ms |
|---|---:|---:|
| remove-500-half | 1.4120 | 1.3702 |
| remove-25000-sparse | 70.2866 | 71.6722 |
| remove-25000-half | 75.8328 | 77.3533 |
| evict-25000-one | 2.8262 | 2.7763 |
| evict-25000-100 | 7.6777 | 7.1924 |

Maintenance was broadly similar to this baseline. Playlist reference comparisons:

| Workload | Sequential reference ms | Batched ms |
|---|---:|---:|
| Add 2,000 paths to empty playlist | 22.082 | 0.661 |
| Add 1,000 overlapping paths to 10,000 entries | 120.505 | 3.138 |
| Add one path to 10,000 entries | 0.277 | 0.238 |
| Add 2,000 repeated paths to 10,000 entries | 241.161 | 2.733 |

## Startup and process resources

The startup regression gate passed all budgets. Medians for the empty isolated profile:

| Metric | Measured |
|---|---:|
| GPU shell ready | 34.13 ms |
| Window shown | 21.00 ms |
| Populated library frame ready | 254.67 ms |
| Working set | 55.04 MiB |
| Peak working set | 55.88 MiB |
| Private bytes | 58.93 MiB |
| Peak private bytes | 59.63 MiB |
| GDI / User objects | 18 / 17 |

The populated frame is observed, not enforced against a checked-in timing cap. This is not a populated-user-library startup benchmark or a before/after startup comparison.

## Evidence and remaining limits

Raw CSV samples, comparison JSON, regression logs and startup output are retained in [backend-validation-20261009](../evidence/backend-validation-20261009/). The recorded repeat script contains machine-local temporary paths; substitute your before/after binary directories when reproducing. Original runners are in `tests/benchmark`; the application gate is `scripts/run_benchmark.ps1`.

Incremental-save speed, repeated-import speed, stalled/network filesystem responsiveness, large-profile startup, playback frame rate and visual fidelity were not benchmarked here. Runtime regression cases now cover the new background/persistence behavior, but those passing tests are not timing measurements. The manual visual, fuzz and special-fixture suites were not part of this run.
