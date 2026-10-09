# Remaining backend opportunities

8 October 2026. Starting source: `39e853a`. Follow-up to [Backend work reduction](BACKEND_WORK_REDUCTION.md).

The requested scope is the remaining opportunities in that report: background folder rediscovery, persisted fingerprints for duplicate imports, incremental durable saving, and the remaining synchronous availability work. Tests, benchmarks and application launches remain excluded by the user's instruction. Compilation and source review do not establish runtime speedups.

| Workstream | Current state |
|---|---|
| Folder rediscovery | Discovery runs on an owned, cancellable worker; guarded application remains on the UI thread. App and regression source compile. |
| Duplicate imports | Fingerprints persist with media records; the worker reuses matching metadata and the UI skips no-op rebuilds/saves. App and regression source compile. |
| Durable incremental saving | Bounded atomic changes and full-checkpoint barriers implemented. App, unit-test source and persistence-test source compile. |
| Availability and watch registration | Pending: shared background availability and the remaining synchronous watch-handle registration. |

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
