# Documentation

Use the guides below for the current workflow. Reports describe a particular source revision and machine; their timings, test counts, source line numbers, and remaining-work lists are historical observations. Follow-up reports can supersede earlier findings.

## Guides and contracts

- [Build and release](BUILD_AND_RELEASE.md): local builds, packaging, publication, updates, and recovery.
- [Testing](TESTING.md): suite selection, focused checks, behavioral coverage, and benchmark reproduction.
- [Portable data and migrations](PORTABLE_DATA_MIGRATIONS.md): durable stores, compatibility, containment, and recovery.
- [Threat boundaries](THREAT_BOUNDARIES.md): trust boundaries and input/lifecycle policies.
- [Custom libmpv builds](CUSTOM_MPV_BUILD.md): engine configuration and local playback validation.

The [portable-data manifest](portable_data_manifest.json) and [threat-boundary manifest](threat_boundaries_manifest.json), with its [schema](threat_boundaries_manifest.schema.json), stay beside their contract documents. Build and test scripts consume these paths.

## Reports

Read the most recent report relevant to the subsystem. Measurements with different baselines or workloads cannot be added together into a single performance percentage.

| Report | Scope |
|---|---|
| [Remaining backend opportunities](reports/BACKEND_REMAINING_WORK.md) | 8 October, in progress: background rediscovery, duplicate import fingerprints, incremental durable saving and background availability |
| [Backend work reduction](reports/BACKEND_WORK_REDUCTION.md) | 7 October: save serialization reuse, bounded search candidates, prepared smart playlists, background watched-folder reconciliation and linear metadata backfill; compile-only validation |
| [Library view pipeline redesign](reports/LIBRARY_VIEW_PIPELINE.md) | 7 October: compact UI projections, reusable query plans, domain-specific refresh, stable layout generations and smart-playlist membership tracking; tests/benchmarks deferred |
| [History and playlist optimization](reports/HISTORY_AND_PLAYLIST_OPTIMIZATION.md) | 7 October follow-up: visible history row lookup, empty-viewport handling, and cached playlist resume selection with smart-playlist expiry; compile-only validation |
| [Scrolling and text optimization](reports/SCROLL_AND_TEXT_OPTIMIZATION.md) | 7 October: offscreen playlist work, group-header lookup, and wrapped text reuse; compile-only validation, with tests and measurements deferred |
| [UI refresh performance](reports/UI_REFRESH_PERFORMANCE.md) | 5 October: accessibility snapshot ownership and exact chapter-boundary caching, with CPU/allocation measurements |
| [Subtitle lookup performance](reports/SUBTITLE_LOOKUP_PERFORMANCE.md) | 5 October: indexed expired cue ranges, preserved overlap/seek behavior, and measured CPU/index-memory tradeoffs |
| [Bulk removal and cache eviction follow-up](reports/MAINTENANCE_FOLLOWUP.md) | 4 October: removed quadratic UI cleanup scans, borrowed eviction records, removed redundant subtitle work, and shared benchmark probes |
| [Test efficiency and disk usage](reports/TEST_AND_STORAGE_OPTIMIZATION.md) | 4 October: removed ineffective tests, shared native compilation, adaptive thumbnail encoding, and deduplicated rollback storage |
| [Measured performance improvements](reports/PERFORMANCE_OPTIMIZATION.md) | 4 October: search tokens/scoring, duplicate connectivity, filtering, grouping, and comparative CPU/heap evidence |
| [Dead code and duplicate detection](reports/DEAD_CODE_AND_DUPLICATE_OPTIMIZATION.md) | 4 October: removed unused render state and first duplicate-scan optimization |
| [Evaluation implementation](reports/EVALUATION_IMPLEMENTATION.md) | Follow-up disposition and validation of all 30 findings in the 2 October evaluation |
| [Implementation evaluation](reports/EVALUATION_2026-10-02.md) | 2 October source review and original probes; read with the implementation follow-up |
| [Backend optimization](reports/BACKEND_OPTIMIZATION_REPORT.md) | 1 October: serialization, playlist membership, cache I/O, and player waits |
| [libmpv integration](reports/LIBMPV_INTEGRATION_REPORT.md) | Engine size/build integration, playback validation, and cleanup |
| [Initial optimization](reports/OPTIMIZATION_REPORT.md) | 30 September application baseline: JSON ownership, overlay work, deferred metadata, and incremental builds |

## Evidence

Small measurement records and historical probe sources live under [evidence](evidence), linked from their reports. These files preserve what was measured at the recorded revision. Historical evaluation probes can assert the old defects and are not current regression gates. Use [the registered suites](../tests/suites.json) to validate current behavior.

Generated build output, full diagnostic runs, personal profiles, thumbnail corpora, and media stay outside tracked documentation. Keep new reports in `reports/`, link to their evidence, and record the source revision, workload, toolchain, validation, and measurement limits.
