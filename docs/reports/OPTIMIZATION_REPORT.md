# Aperture optimization report — 30 September 2026

Baseline: commit `e9b3a79`, rebuilt with the same MSVC x64 toolchain. Both versions used the same libmpv DLL, assets, portable fixtures and media. MB means 1,000,000 bytes; memory uses MiB. Negative changes indicate reductions. Raw measurements and validation logs were collected under `diagnostics/optimization/`; commit preparation archived them in the sibling `Aperture Backups/commit-preparation-20260930-090430/archived/diagnostics/optimization/` directory.

## 1. Major problems found

Every JSON value carried a string, vector and map simultaneously, even for scalar values. Large library loads spent considerable memory and time constructing unused containers. Playback updated durable progress through both event callbacks and frame polling, and repainted an empty overlay after the controls disappeared. First-time playback synchronously probed metadata before opening the decoder. Inactive preview/extraction paths retained DLL references and buffer capacity. Compile-only builds discarded all incremental work.

## 2. Major optimizations performed

- Store only the active JSON payload in an owning `std::variant`. Use transparent object-key lookup and string references where copies are unnecessary. Keep parser limits, serialization, unknown-field preservation and normal value semantics.
- Update playback progress once per coalesced time/duration event. Preserve seeks, paused progress, resume data and private playback.
- Stop repainting the playback overlay once controls disappear. Visible controls, application subtitles, diagnostics, menus and transient animations continue to request redraws; mpv continues presenting video independently.
- Let the existing metadata worker probe new playback records while mpv opens the media. Retain synchronous probing when a worker already owns a captured batch, so new records cannot miss metadata work.
- Release inactive preview/extractor resources through one shutdown path, including DLL references and buffer capacity. Allocate extraction pixels on demand.
- Use MSVC's size-oriented Release profile with the existing link-time optimization and dead-code elimination.
- Reuse separate Release/Debug build caches, serialized by a file lock. `-Clean`, analysis and sanitizer builds remain isolated; reproducibility checks explicitly request clean builds. A repeat build required no compilation and finished in **4.10 seconds**, including validation/configuration/copying. No matched full-build timing was collected, so no build-speed percentage is claimed.

## 3. Dead/redundant systems removed

Removed the unused `ExternalTools` implementation/header and its build entry, unused application initialization overload, no-op association removal, uncalled thumbnail-cache/settings helpers, obsolete library path helpers, duplicate preview/extractor release implementations, redundant progress polling state and unused pending-duration storage. Actual playback, import, update, subtitle, association, thumbnail and library features remain.

## 4. Binary/distribution size reduction

| Metric | Before | After | Change |
|---|---:|---:|---:|
| Aperture.exe | 3.616256 MB | 2.947584 MB | −0.668672 MB / −18.49% |
| Complete portable runtime | 44.786548 MB | 44.117876 MB | −0.668672 MB / −1.49% |
| libmpv-2.dll | 39.929344 MB | 39.929344 MB | unchanged |

The runtime measurement includes the executable, engine, fonts, locale, updater and manifest. It excludes user data, caches, rollback copies and developer symbols, consistently for both packages. The final ZIP is **18,636,557 bytes**. Matching debug symbols are archived separately in `dist/symbols/` and are absent from the portable package.

libmpv accounts for **90.5%** of the optimized runtime. Its existing PE layout has no large removable padding/debug payload. A 30% runtime reduction would require rebuilding the engine or reducing supported functionality; deleting necessary codecs, fonts or features would violate the task.

## 5. Code-size reduction

Tracked production `src/*.cpp`, `src/*.h` and resource source: **53,844 → 53,678 physical lines**, a net removal of **166 lines / 0.31%**, with **two files deleted**. The changed production files alone total **17,981 → 17,815 lines / −0.92%**. Counts include comments and blank lines, and exclude vendor/generated code and test/report additions. Most of the source implements retained UI/features, so a 30% LOC cut was not forced.

## 6. RAM/VRAM changes

One matched run per binary used the same 60-second, 1280×720, 24 fps MJPEG video, bundled assets, eight-second warmup and six playback samples. Playback values are sample maxima. Idle is the settled hub after playback, hover preview and decoder teardown.

| Metric | Before | After | Observed change |
|---|---:|---:|---:|
| Idle working set | 111.36 MiB | 104.81 MiB | −5.89% |
| Idle private memory | 124.30 MiB | 101.58 MiB | −18.28% |
| Playback working set | 137.31 MiB | 136.75 MiB | −0.41% |
| Playback private memory | 188.27 MiB | 186.33 MiB | −1.03% |
| Idle dedicated GPU allocation | 62.98 MiB | 62.84 MiB | −0.22% |
| Playback dedicated GPU allocation | 86.27 MiB | 82.51 MiB | −4.36% |
| Peak C++ allocation, 25,000-entry load | 163.51 MB | 68.21 MB | −58.28% |
| Working set after 10,000-entry startup | 120.17 MiB | 94.28 MiB | −21.5% |
| Private memory after 10,000-entry startup | 126.41 MiB | 100.28 MiB | −20.7% |

Heap instrumentation measures live C++ allocations, not process RAM. GPU figures come from Windows GPU-process memory counters and are allocations rather than physical residency. Library allocation results are seven-sample medians; startup results are seven alternating fresh-process samples. Runtime RAM/VRAM results are limited to the single matched run and should not be generalized to every codec/device. The probe's legacy 15 MiB idle budget was exceeded by both versions; a successful measurement does not mean that budget passed.

## 7. Startup/loading/performance changes

| Metric | Before | After | Observed change |
|---|---:|---:|---:|
| Empty-library window-show milestone | 21.81 ms | 22.38 ms | +2.6% / +0.57 ms |
| Empty-library first GPU shell milestone | 216.667 ms | 216.993 ms | effectively unchanged |
| Empty-library populated probe completion | 222.70 ms | 222.78 ms | effectively unchanged |
| 10,000-entry first populated hub milestone | 269.646 ms | 233.997 ms | −13.2% |
| 10,000-entry populated probe completion | 826.01 ms | 773.26 ms | −6.4% |
| Load 500 entries | 34.456 ms | 27.869 ms | −19.12% |
| Load 25,000 entries | 585.563 ms | 371.969 ms | −36.48% |
| First save, 25,000 entries | 509.526 ms | 463.251 ms | −9.08% |
| Unchanged save, 25,000 entries | 464.934 ms | 442.958 ms | −4.73% |
| Progress save, 25,000 entries | 500.522 ms | 453.086 ms | −9.48% |
| Open With equivalent CLI → visible window | 93 ms | 79 ms | −15.1%; coarse sampling |
| CLI → loaded/playable | 469 ms | 453 ms | −3.4%; coarse sampling |
| Hub → loaded/playable, same-file reopen | 78 ms | 94 ms | +16 ms; below polling interval |
| Playback CPU, percent of one core | 15.91% | 3.82% | −76.0%; one matched run |
| Settled idle CPU, percent of one core | 0.44% | 0.22% | low/noisy; one short interval |
| CLI/Hub → first presented video frame | not reliably measured | not reliably measured | no claim |
| Media switching latency | not reliably measured | not reliably measured | functionality verified |
| Average presented FPS / frame-time stutter | not reliably measured | not reliably measured | no claim |

Startup medians use nine samples for the empty library and seven for 10,000 entries, with one warmup per version, alternating order, fresh processes and retained OS caches. Internal milestones start at the application timer, rather than external process creation. Probe completion includes work after the first populated render and differs from that render's milestone. None is a video-first-frame measurement.

CLI/hub medians use five alternating fresh profiles per version plus warmup, the exact same video and an external process-start timer. The harness invokes the same media argument used by file association launches, without timing Explorer itself. It checks a visible owned window, loaded paused media with positive duration, and an advancing playback clock. Window polling is 25 ms and status polling 40 ms, with ordinary Windows timer granularity. The 16 ms hub difference does not establish a reliable slowdown. These measurements support little loading improvement on this sample; they do not support a 30% launch/loading claim.

CPU uses process CPU-time deltas over approximately 30 seconds of settled playback; 100% represents one logical core. The machine has 24 logical processors. Reduced playback CPU is consistent with avoiding empty overlay repaints. The earlier shorter probe varied in the opposite direction, so the longer measurement is reported with its single-run limitation. The idle interval was only about seven seconds and cannot certify a 50% idle CPU improvement.

All save/load workloads produced the same JSON byte counts and content hashes for both versions.

## 8. Deliberately omitted optimizations and validation limits

Preserved the bundled playback engine/codecs, fonts, locales, rendering API, diagnostics, networking, settings and storage compatibility. Engine rebuilding needs its own reproducible toolchain and codec/device regression matrix. Decoder/GPU allocations dominate ordinary playback memory; speculative trimming would risk playback behavior. Did not remove supported functionality or introduce a new storage format solely to meet a percentage target.

Final validation: **18/18 PR regression suites**, including unit, persistence, IPC, workflows, loader, updater, package retention, dependency/runtime boundaries, static analysis, shutdown stress and thumbnail-cache coverage. Additional final checks passed **105 persistence tests**, **10,101 JSON fuzz cases**, and actual-app seek/pause/queue/resume/reopen workflows. Package verification passed file hashes, x64/runtime dependency checks and isolated startup/shutdown. Symbol archive integrity and binary/PDB matching passed. The installed `release/` and `debug/` executables match the validated binary; **27 personal files remained byte-identical** after installation.

The repository's checked-in visual goldens already fail against the original binary. Fixed test-only BOM/obsolete-store fixtures, then captured all four scenes from the unmodified baseline and compared the optimized binary: **zero differing pixels in every scene**. Checked-in golden images were preserved. This is a passing original-versus-optimized UI comparison, not a claim that the stale golden gate passes.

## 9. Follow-up opportunities at this baseline

The later [backend optimization report](BACKEND_OPTIMIZATION_REPORT.md) records the 1 October follow-up, including faster full-library saves, batch playlist membership, cache writes and event-driven player waits.

- At this baseline, periodic progress saves serialized the full library at about 453 ms for 25,000 entries. The later [evaluation implementation](EVALUATION_IMPLEMENTATION.md) records coalesced background saves and compact snapshots, including unknown-field preservation, atomic recovery, and shutdown draining.
- The follow-up libmpv rebuild is now integrated; see [LIBMPV_INTEGRATION_REPORT.md](LIBMPV_INTEGRATION_REPORT.md) for its separate measurements and playback validation. The earlier results above describe the application-only optimization stage.
- Add actual decoder/present timestamps and GPU frame tracing to measure video-first-frame latency and pacing across H.264/HEVC/AV1, 4K, subtitles and hardware/software decoding. Present quality should drive further pipeline changes.
- Profile existing backfill/import activity during large-library startup and first-time media open, especially when another metadata batch is already active.

The current executable and portable ZIP remain under `release/` and `dist/`. The duplicate debug runtime, matching symbols and raw measurement evidence were archived outside the workspace during commit preparation; live profile files remain in place. See [LIBMPV_INTEGRATION_REPORT.md](LIBMPV_INTEGRATION_REPORT.md) for the later engine change and cleanup record.
