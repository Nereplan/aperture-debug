# Remaining backend opportunities

8-9 October 2026. Starting source: `39e853a`. Follow-up to [Backend work reduction](BACKEND_WORK_REDUCTION.md).

The requested scope is the remaining opportunities in that report: background folder rediscovery, persisted fingerprints for duplicate imports, incremental durable saving, and the remaining synchronous availability work. Tests, benchmarks and application launches remain excluded by the user's instruction. Compilation and source review do not establish runtime speedups.

| Workstream | Current state |
|---|---|
| Folder rediscovery | Discovery runs on an owned, cancellable worker; guarded application remains on the UI thread. App and regression source compile. |
| Duplicate imports | Fingerprints persist with media records; the worker reuses matching metadata and the UI skips no-op rebuilds/saves. App and regression source compile. |
| Durable incremental saving | Bounded atomic changes and full-checkpoint barriers implemented. App, unit-test source and persistence-test source compile. |
| Availability and watch registration | Watch registration and shared availability run on owned workers. The app and registered regression source compile. |

## Folder rediscovery

Startup, folder-view entry and manual rediscovery now capture file facts and schedule the same background planner. Cancellation interrupts supported synchronous I/O; replacement and undo invalidate old completions. Shutdown waits through the existing close pump before joining the owned job.

The planner groups missing records, shares parent-directory enumeration and indexes candidate filenames. Cached directory listings are limited to 128 directories and 65,536 matched file records; an oversized directory is processed without retaining an additional cached copy. Equal-confidence candidates are left unresolved. Known conflicting identities on the same volume no longer authorize relocation merely because sizes match.

The UI checks captured source and destination file facts against the current library. It applies accepted path changes, duplicate merges and playlist/history references in one batch with one lookup rebuild. Current playback and user metadata remain live. A stale observation requests a new scan. Watch reconciliation defers removals while rediscovery is active so it cannot discard a missing record before reconnection is attempted.

The synchronous library API uses the same planner and merge for callers that need it. A duplicate merge now honors `saveChanges = false`; the former nested removal could save despite that flag. Missing destination duration can be recovered from the surviving source's metadata.

Validation: the full native app compiled and linked locally; all registered unit-test source passed MSVC syntax-only compilation. Added unrun cases cover concurrent user edits, stale file facts, ambiguous destinations, duplicate merging without saving, directory-list reuse and cancellation. No tests or benchmarks were executed.

## Duplicate import fingerprints

`metadataFileWriteTime` stores the exact Windows file timestamp as a decimal string, alongside the existing size, volume and file identity. A record becomes eligible for reuse only after a successful probe sees the same fingerprint before and after probing. Old records without that evidence receive a fresh probe. Failed probes, changed file observations and uncertain fingerprints retire the cached timestamp.

The fingerprint lookup briefly opens the file for reading without sharing writes, then closes it before probing. Active write handles therefore disable reuse rather than relying on a potentially deferred write timestamp. The sharing behavior follows Microsoft's [CreateFile documentation](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew). File identity also distinguishes replacement files whose size and timestamp are unchanged.

The UI captures scalar metadata and ID/path strings; the worker builds the lookup and performs filesystem checks. Playback, bookmarks, preferences and overrides are not copied into that cache. Cached metadata does not start the media-probe helper. Explicit overwrite and metadata-refresh operations continue to probe.

Import completion checks source baselines and updates only file/probe fields. It retains useful duration and dimensions after a failed probe and preserves current user edits. Structural additions rebuild the path map once; a repeat import with no metadata or playlist changes does not rebuild indexes, invalidate views, request a save or launch a metadata sweep. Failed or superseded results do not overwrite newer records.

Fingerprints use filesystem metadata. Tools that rewrite a file in place and restore its timestamps can require an explicit metadata refresh. This approach avoids reading the entire contents of large videos just to validate the cache.

Validation: the native app compiled and linked, and unit-test source passed syntax-only compilation. Added unrun regression cases cover fingerprint persistence, repeat imports, same-size edits, replacement files, active writers, changes during a probe, failed probes and stale merges. Tests and benchmarks were not executed.

## Durable incremental saving

The app writer now keeps a full `library.json` checkpoint and writes cumulative record replacements to `library.changes`. Repeated playback updates replace the same small record entry instead of assembling, backing up and rewriting the full library. The changes file is capped at 512 records, 1 MiB and one quarter of the checkpoint size. Small libraries and larger edits use a full checkpoint. Snapshot capture still compares the public mutable records; that compatibility cost remains linear in library size.

The changes file is atomically replaced and checksummed. Replay validates the exact checkpoint digest, byte and count limits, unique sorted slots and fully populated appends before altering parsed records. Full checkpoints have a fresh identifier, so an obsolete file cannot become valid again when values revert. Unreadable, orphaned and unsupported changes protect the library from saving; corrupt changes can recover from a matching backup. Full-checkpoint backups preserve the latest associated changes.

Close and reload use an explicit checkpoint barrier, including when the writer is idle. Normal save flushes continue to publish small updates. Synchronous library callers retain full-document output. The portable-data documentation and manifest now describe the additional durable file and the interrupted-session downgrade requirement.

Atomic writes now check the result of [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers) before publishing. The SHA-256 helper checks each operation and uses the [CNG-managed hash allocation](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptcreatehash) available on supported Windows versions. These mechanisms have not been exercised in runtime tests in this task.

Validation: the native app compiled and linked with no compiler warnings in the final build. Registered unit and persistence regression sources passed MSVC syntax-only compilation. New unrun cases cover incremental progress, cumulative replacements, appends/removals, value reversion, large reorder fallback, failed writes, checksum damage, matching backup recovery, orphaned/future changes and an idle-worker checkpoint barrier. No tests, benchmarks or application launches were run.

## Background watch registration

The app publishes its complete desired watch list through a separate mailbox. The watcher normalizes paths, opens directory handles, registers notifications and retires removed handles on its worker. A newer list replaces a pending list without waiting on the worker's watch mutex. Startup no longer opens watched directories on the UI thread. Library reload/undo, folder relocation, adding and removing watched roots all use the same path.

Inactive roots retry every two seconds and request reconciliation when registration succeeds. The watch list is not rescanned on every 50 ms notification-coalescing tick. Removed watches retain their notification storage until canceled completions arrive.

Close now requests cancellation before the close pump waits for the watcher to finish; joining happens after that. Driver support still governs [Windows I/O cancellation](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations). The existing synchronous AddWatch/RemoveWatch compatibility APIs remain available, but the app no longer uses them.

Validation: the native app compiled and linked, and registered unit-test source passed syntax-only compilation. Added unrun cases cover replacement while the worker holds its watch mutex, stop requests during that same condition, empty profiles, and registering a previously missing root. No tests or benchmarks were executed.

## Shared background availability

Smart playlists and media-card missing-file badges now read the same demand-driven availability cache. Lookups normalize/deduplicate paths and enqueue work without accessing the filesystem. Up to two lazily started workers process 32 paths per batch, so one stalled batch does not stop the other worker or block the interface. Callbacks receive owned path snapshots rather than mutable library records.

Unobserved paths do not become confirmed missing items while waiting. Smart playlists exclude unknown/unavailable observations; cards retain their existing distinction between an observed file and an inaccessible or directory target. An expired observation remains visible while refreshing. Fresh observations retain their original two-second expiry across playlists; pending refreshes use a future deadline so a stalled drive cannot force full playlist reevaluation on every frame.

The cache holds at most the library's current item count plus 4,096 additional paths during steady state. Pending entries cannot be evicted. Shrinking a library prunes completed entries gradually, with bounded work per update. Exception failures keep the prior observation and back off. Cache reset cancels old generations; a late completion cannot repopulate a replaced library's cache. The close pump waits for active work before joining both owned workers.

Availability completions invalidate only availability-dependent smart memberships and their aggregates, then request a redraw. Independent title/rule playlists keep their cached membership. Standalone or copied library objects do not invoke the original UI provider; their explicit synchronous evaluation remains available. The app's ordinary library projection does not enable the engine's separate explicit disk-check query mode.

This adds memory for demanded path keys/observations and allows temporary pending or stale display states while I/O finishes. Filesystem cancellation remains subject to driver support. No runtime latency or memory improvement has been measured.

Validation: the final native app compiled and linked without compiler warnings. Registered unit and persistence regression sources passed syntax-only compilation. New unrun availability cases cover asynchronous lookup, path deduplication, stale refreshes, independent workers, cancellation/reset, cache bounds, exception backoff, view/aggregate invalidation, original observation expiry, copied models and real file/directory distinctions.

## Scope and verification audit

| Requested opportunity | Current source evidence | Validation boundary |
|---|---|---|
| Background folder rediscovery | `AppFileOps.cpp` schedules `PrepareFolderRediscovery`; startup and folder-view entry call that worker path. Application uses guarded `ApplyFolderRediscovery`. | Native build and registered regression source compile; no discovery operation executed. |
| Duplicate import fingerprints | `MediaImportProbeCache.h` verifies size, timestamp and identity; both media serializers/loaders preserve the stamp. `AppImportJobs.cpp` uses guarded `MergeImportedMedia` and skips unchanged saves. | Native build and unrun fingerprint regression source compile. |
| Durable incremental saving | `LibraryIncrementalStore.h` validates bounded changes and recovery; `WriteLibrarySaveSnapshot` selects delta/full publication. Close/reload call `RequestCheckpoint`. Portable manifest includes `library.changes`. | Native build plus unit and persistence syntax checks; crash recovery and disk-failure cases are unrun. |
| Background watch registration | App callers use `SetWatchedFoldersAsync`; directory registration and retirement occur on the worker. Close requests cancellation and waits for worker completion. | Native build and unrun concurrency regression source compile. |
| Shared background availability | `BackgroundPathAvailability.h` owns bounded batches; smart playlists and card badges use cached observations. Completion invalidates affected memberships and redraws. | Native build and unrun cache/concurrency regression source compile. |

All implementation work in this follow-up is complete. Tests, benchmarks and application launches were not run, as requested. Runtime correctness, workload timings and memory measurements remain for the user's validation; compilation is not evidence of measured speedups. No release was published.
