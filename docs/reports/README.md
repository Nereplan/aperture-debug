# Performance and Evaluation Reports

Use the [checkpoint dashboard](../checkpoints.html) for current comparison and recording. It embeds these historical reports, measured checkpoint values, VLC baselines and validation. Add future performance work through [the checkpoint workflow](../CHECKPOINTS.md), keeping raw evidence in `docs/evidence/`.

Historical engineering reports for Aperture, recording measured optimizations, architectural evaluations, regression validation, and subsystem benchmarks.

Reports capture empirical observations on specific machines and source revisions. Timings, allocation counts, and line numbers are historical records; refer to the most recent report for current subsystem status.

## Navigation by subsystem

### Backend, storage and persistence
- [Backend performance balance (17 Oct 2026)](BACKEND_BALANCE_20261017.md) — Speed/resource/size measurements, all eight research dispositions, local build and regression evidence.
- [Library record memory (14 Oct 2026)](LIBRARY_RECORD_MEMORY_20261014.md) — On-demand playback preferences, independent snapshot ownership, and sparse/dense memory and CPU measurements.
- [JSON array ownership during loading (14 Oct 2026)](JSON_ARRAY_GROWTH_20261014.md) — Explicit parser ownership transfer, library-load measurements and array-boundary regressions.
- [Player comparison and preview work reduction (14 Oct 2026)](PLAYER_COMPARISON_20261014.md) — Local VLC/MPC-HC resource measurements, source review, and one-shot seekbar extraction.
- [Backend resource optimization (13 Oct 2026)](BACKEND_RESOURCES_20261013.md) — JSON loading, streamed cache accounting, single-path index ownership, and heap eviction.
- [Backend validation and benchmarks (9 Oct 2026)](BACKEND_VALIDATION_20261009.md) — Source `df21d93` vs `bd5db22`. Measured 77.6% faster progress saves (108 ms to 24 ms for 25k items), save-retry read-only flag fix, 20/20 PR suites passed, startup budget verification. Raw evidence: [backend-validation-20261009](../evidence/backend-validation-20261009/).
- [Remaining backend opportunities (8–9 Oct 2026)](BACKEND_REMAINING_WORK.md) — Follow-up to backend work reduction. Background folder rediscovery on cancellable worker, persisted fingerprints (`metadataFileWriteTime`) for duplicate imports, incremental durable saving (`library.changes` log with checkpoint barriers), background watch registration mailbox, and shared background availability.
- [Backend work reduction (7 Oct 2026)](BACKEND_WORK_REDUCTION.md) — Source `bd5db22`. Save serialization reuse, bounded candidate sets for search, prepared smart playlist queries, background watched-folder reconciliation, single-pass metadata backfill.
- [Backend optimization baseline (1 Oct 2026)](BACKEND_OPTIMIZATION_REPORT.md) — Initial backend pass covering JSON serialization, playlist membership caching, cache I/O, and player synchronization.

### UI, rendering and interaction
- [Library view pipeline redesign (7 Oct 2026)](LIBRARY_VIEW_PIPELINE.md) — Source `bef3068`. Compact UI projections (`LibraryViewItem`), reusable query plans, domain-specific refresh scoping, stable layout generations, and smart-playlist membership tracking.
- [History and playlist optimization (7 Oct 2026)](HISTORY_AND_PLAYLIST_OPTIMIZATION.md) — Source `3fee131`. Virtualized visible history row lookup, empty-viewport handling, cached playlist resume selection with expiry.
- [Scrolling and text optimization (7 Oct 2026)](SCROLL_AND_TEXT_OPTIMIZATION.md) — Source `508a96c`. Elimination of offscreen playlist work, group-header lookup caching, wrapped text layout reuse.
- [UI refresh performance (5 Oct 2026)](UI_REFRESH_PERFORMANCE.md) — Source `e44f2a9` vs `0373c27`. Accessibility snapshot ownership, elimination of retired accessibility tree copies, exact chapter-boundary caching. Raw evidence: [ui-refresh-evidence](../evidence/ui-refresh-evidence/).

### Playback engine, subtitles and media
- [Subtitle lookup performance (5 Oct 2026)](SUBTITLE_LOOKUP_PERFORMANCE.md) — Source `3c046a6` vs `0373c27`. Indexed cue ranges for playback and seek, preserved overlap/seek behavior, CPU and memory tradeoff evaluation. Raw evidence: [subtitle-lookup-evidence](../evidence/subtitle-lookup-evidence/).
- [libmpv size build integration (30 Sep 2026)](LIBMPV_INTEGRATION_REPORT.md) — Lean libmpv compilation recipe, shader chain `-Os` optimization, binary footprint reduction, playback validation.

### Maintenance, code cleanup and test efficiency
- [Bulk removal and cache eviction follow-up (4 Oct 2026)](MAINTENANCE_FOLLOWUP.md) — Source `79840ed` vs `1e89ccd`. Elimination of quadratic UI cleanup scans, borrowed eviction records, redundant subtitle work elimination, shared allocation probe harness. Raw evidence: [removal-eviction-evidence](../evidence/removal-eviction-evidence/).
- [Test efficiency and disk usage (4 Oct 2026)](TEST_AND_STORAGE_OPTIMIZATION.md) — Source `4b30177` vs `ea0a340`. Removal of ineffective tests, shared native suite compilation, adaptive thumbnail encoding, deduplicated rollback storage. Raw evidence: [size-evidence](../evidence/size-evidence/).
- [Measured performance improvements (4 Oct 2026)](PERFORMANCE_OPTIMIZATION.md) — Source `17ed90a`. Search tokens and scoring optimization, duplicate connectivity graphs, filtering and grouping improvements. Raw evidence: [performance-evidence](../evidence/performance-evidence/).
- [Dead code and duplicate detection cleanup (4 Oct 2026)](DEAD_CODE_AND_DUPLICATE_OPTIMIZATION.md) — Removal of 17 unused view-model fields, unread thumbnail counters, unused render state, initial duplicate scan optimization. Raw evidence: [duplicate-detection-evidence](../evidence/duplicate-detection-evidence/).
- [Initial application optimization baseline (30 Sep 2026)](OPTIMIZATION_REPORT.md) — Initial baseline comparison vs `e9b3a79`. JSON ownership, overlay work, deferred metadata, incremental builds.

### Architecture evaluations and implementations
- [Evaluation implementation tracking (3 Oct 2026)](EVALUATION_IMPLEMENTATION.md) — Follow-up disposition and verification for all 30 actionable findings (F01–F30) identified in the 2 October evaluation. Raw evidence: [evaluation-implementation-evidence](../evidence/evaluation-implementation-evidence/).
- [Aperture implementation evaluation (2 Oct 2026)](EVALUATION_2026-10-02.md) — 30-point source review covering playback persistence, dirty tracking, overlay rendering, transport, watcher, settings, and native lifecycle boundaries. Raw evidence: [evaluation-evidence-2026-10-02](../evidence/evaluation-evidence-2026-10-02/).

---

## Chronological index

| Date | Report | Baseline | Primary focus | Raw evidence |
|:---|:---|:---|:---|:---|
| 2026-10-14 | [Library record memory](LIBRARY_RECORD_MEMORY_20261014.md) | `4fcf456` | On-demand preferences and record memory/CPU tradeoffs | [library-record-memory-20261014](../evidence/library-record-memory-20261014/) |
| 2026-10-14 | [JSON array ownership during loading](JSON_ARRAY_GROWTH_20261014.md) | `fbb9eb4` | Library-load speed and allocation reductions | [json-array-growth-20261014](../evidence/json-array-growth-20261014/) |
| 2026-10-14 | [Player comparison and preview work reduction](PLAYER_COMPARISON_20261014.md) | `cee51e3` | VLC/MPC-HC resources and one-shot seekbar extraction | [player-comparison-20261014](../evidence/player-comparison-20261014/) |
| 2026-10-09 | [Backend validation and benchmarks](BACKEND_VALIDATION_20261009.md) | `bd5db22` | Save/load regression validation, save-retry fix, startup checks | [backend-validation-20261009](../evidence/backend-validation-20261009/) |
| 2026-10-08 | [Remaining backend opportunities](BACKEND_REMAINING_WORK.md) | `39e853a` | Folder rediscovery, import fingerprints, incremental saving, availability | — |
| 2026-10-07 | [Backend work reduction](BACKEND_WORK_REDUCTION.md) | `bd5db22` | Save serialization reuse, bounded search candidates, folder reconciliation | — |
| 2026-10-07 | [Library view pipeline redesign](LIBRARY_VIEW_PIPELINE.md) | `bef3068` | Compact view projections, reusable query plans, layout generations | — |
| 2026-10-07 | [History and playlist optimization](HISTORY_AND_PLAYLIST_OPTIMIZATION.md) | `3fee131` | Virtualized history rows, cached playlist resume selection | — |
| 2026-10-07 | [Scrolling and text optimization](SCROLL_AND_TEXT_OPTIMIZATION.md) | `508a96c` | Offscreen playlist work elimination, group-header lookup, text layout reuse | — |
| 2026-10-05 | [UI refresh performance](UI_REFRESH_PERFORMANCE.md) | `0373c27` | Accessibility snapshot ownership, chapter-boundary caching | [ui-refresh-evidence](../evidence/ui-refresh-evidence/) |
| 2026-10-05 | [Subtitle lookup performance](SUBTITLE_LOOKUP_PERFORMANCE.md) | `0373c27` | Subtitle cue indexing, seek performance, CPU/memory balance | [subtitle-lookup-evidence](../evidence/subtitle-lookup-evidence/) |
| 2026-10-04 | [Bulk removal and cache eviction follow-up](MAINTENANCE_FOLLOWUP.md) | `1e89ccd` | Removal scan quadratic cost elimination, borrowed eviction records | [removal-eviction-evidence](../evidence/removal-eviction-evidence/) |
| 2026-10-04 | [Test efficiency and disk usage](TEST_AND_STORAGE_OPTIMIZATION.md) | `ea0a340` | Test suite pruning, shared compilation, thumbnail & snapshot compaction | [size-evidence](../evidence/size-evidence/) |
| 2026-10-04 | [Measured performance improvements](PERFORMANCE_OPTIMIZATION.md) | `17ed90a` | Search scoring, duplicate connectivity, comparative benchmarks | [performance-evidence](../evidence/performance-evidence/) |
| 2026-10-04 | [Dead code and duplicate detection cleanup](DEAD_CODE_AND_DUPLICATE_OPTIMIZATION.md) | `17ed90a` | View-model field cleanup, unread counters, duplicate scan speedup | [duplicate-detection-evidence](../evidence/duplicate-detection-evidence/) |
| 2026-10-03 | [Evaluation implementation tracking](EVALUATION_IMPLEMENTATION.md) | `70c271c` | 30/30 evaluation findings implemented and verified | [evaluation-implementation-evidence](../evidence/evaluation-implementation-evidence/) |
| 2026-10-02 | [Aperture implementation evaluation](EVALUATION_2026-10-02.md) | `70c271c` | Comprehensive 30-finding architectural and performance review | [evaluation-evidence-2026-10-02](../evidence/evaluation-evidence-2026-10-02/) |
| 2026-10-01 | [Backend optimization baseline](BACKEND_OPTIMIZATION_REPORT.md) | `502f648` | Save serialization, playlist queries, cache I/O, player synchronization | — |
| 2026-09-30 | [libmpv size build integration](LIBMPV_INTEGRATION_REPORT.md) | — | Lean libmpv compilation flags, binary footprint, playback verification | — |
| 2026-09-30 | [Initial application optimization baseline](OPTIMIZATION_REPORT.md) | `e9b3a79` | Application baseline: JSON ownership, overlay work, deferred metadata | — |

---

## Historical report convention (superseded by checkpoints)

For future measurements, follow [the checkpoint workflow](../CHECKPOINTS.md) instead. The convention below describes the preserved historical reports.

Historical reports use descriptive uppercase filenames, record evaluated source/baseline revisions and measurement bounds, and retain raw CSV samples, JSON summaries and logs in `../evidence/<topic-evidence>/`. The indexes above preserve their original navigation; new checkpoint data is maintained in the dashboard.
