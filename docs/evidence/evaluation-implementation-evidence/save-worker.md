# Per-store snapshots and serialized background saving

F07 now compares typed persisted fields before serialization, including direct edits of public records and unknown JSON fields. Unchanged records and stores share immutable snapshot storage. Separate store generations and a load-series identity protect failed/future-schema stores, retry failed writes, and distinguish reloads with otherwise equal generation numbers. Explicit portable roots are used consistently for loading, saving and relocatable paths.

App mutator and command save requests coalesce on the UI pump. One owned writer serializes immutable snapshots, with one replaceable pending snapshot and a 100 ms debounce. Dirty JSON generation, backups and atomic replacement execute on that writer. Completion and failures return to the UI. Refresh waits for the save barrier on its worker and rejects a failed barrier or newer in-memory changes. Close captures the final state, requests a flush, continues dispatching messages until its ticket completes, then joins the owned writer. Standalone MediaLibrary callers retain synchronous Save behavior; copied models cannot invoke the originating App's save handler.

The resource fixture exercises Unicode, escaped strings, unknown fields, preferences, overrides, playlists and history, with seven measured samples after warmup at 500 and 25,000 records. JSON byte lengths and content hashes match the original baseline in all eight save/load workloads. The final measurement ran with other compiler and timing work paused.

| 25,000 records | Original median | Current median | What is measured |
| --- | ---: | ---: | --- |
| Unchanged synchronous save | 109.053 ms | 9.546 ms | Exact comparison and existing-file checks, without serialization |
| Dirty progress synchronous save | 128.523 ms | 117.477 ms | Total work, including serialization and checked disk writes |
| Unchanged capture | n/a | 9.323 ms | App's immutable capture work; 2 allocations / 168 bytes |
| One progress-change capture | n/a | 10.065 ms | App's immutable capture work; 17 allocations / 402,559 bytes |
| Initial synchronous save | 147.373 ms | 161.708 ms | First immutable copy plus serialization and writes |

The capture measurement excludes worker submission and thread scheduling. It proves the portion left on the UI thread, not end-to-end application frame time. The background writer removes the remaining dirty-save work from that thread; it does not eliminate that work. Measurements of baseline and current binaries were taken at different times, so small timing differences and the 500-record load reduction are not attributed to this change.

Initial snapshot ownership has a real memory cost: peak tracked C++ allocation during the large initial save increased from 19,107,512 to 44,148,719 bytes (about 24 MiB extra). It also makes initial saving slower. Subsequent captures share unchanged records; the one-record update allocates a pointer vector and one changed record, rather than duplicating the whole model. This is the tradeoff for safely serializing the existing publicly mutable model on another thread. The unchanged-save peak falls from 9,026,524 to 1,824 bytes. No claim is made about total process working set from this C++ allocation counter.

Evidence: [save CSV](save-results.csv), [save comparison](save-comparison.json), [capture CSV](save-capture-results.csv), [capture summary](save-capture-summary.json). Capture modes are available with `tests/benchmark/run_save_benchmark.ps1 -CaptureOnly`; the ordinary benchmark workloads remain unchanged.
