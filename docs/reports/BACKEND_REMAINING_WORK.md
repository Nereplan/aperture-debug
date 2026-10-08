# Remaining backend opportunities

8 October 2026. Starting source: `39e853a`. Follow-up to [Backend work reduction](BACKEND_WORK_REDUCTION.md).

The requested scope is the remaining opportunities in that report: background folder rediscovery, persisted fingerprints for duplicate imports, incremental durable saving, and the remaining synchronous availability work. Tests, benchmarks and application launches remain excluded by the user's instruction. Compilation and source review do not establish runtime speedups.

| Workstream | Current state |
|---|---|
| Folder rediscovery | Discovery runs on an owned, cancellable worker; guarded application remains on the UI thread. App and regression source compile. |
| Duplicate imports | Pending: persist validated file fingerprints and reuse metadata without copying user-owned records into the import worker. |
| Durable incremental saving | Pending: reduce full-document writes while preserving recovery, backups, unknown fields and portability. |
| Availability and watch registration | Pending: shared background availability and the remaining synchronous watch-handle registration. |

## Folder rediscovery

Startup, folder-view entry and manual rediscovery now capture file facts and schedule the same background planner. Cancellation interrupts supported synchronous I/O; replacement and undo invalidate old completions. Shutdown waits through the existing close pump before joining the owned job.

The planner groups missing records, shares parent-directory enumeration and indexes candidate filenames. Cached directory listings are limited to 128 directories and 65,536 matched file records; an oversized directory is processed without retaining an additional cached copy. Equal-confidence candidates are left unresolved. Known conflicting identities on the same volume no longer authorize relocation merely because sizes match.

The UI checks captured source and destination file facts against the current library. It applies accepted path changes, duplicate merges and playlist/history references in one batch with one lookup rebuild. Current playback and user metadata remain live. A stale observation requests a new scan. Watch reconciliation defers removals while rediscovery is active so it cannot discard a missing record before reconnection is attempted.

The synchronous library API uses the same planner and merge for callers that need it. A duplicate merge now honors `saveChanges = false`; the former nested removal could save despite that flag. Missing destination duration can be recovered from the surviving source's metadata.

Validation: the full native app compiled and linked locally; all registered unit-test source passed MSVC syntax-only compilation. Added unrun cases cover concurrent user edits, stale file facts, ambiguous destinations, duplicate merging without saving, directory-list reuse and cancellation. No tests or benchmarks were executed.
