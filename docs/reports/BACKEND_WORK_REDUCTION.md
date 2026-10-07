# Backend work reduction

7 October 2026. Source baseline: `bd5db22` (library view pipeline redesign).

This pass reduces repeated work in saving, searching, smart playlists, watched-folder updates and metadata backfill. It does not claim measured latency or memory improvements: the user requested no tests or benchmarks. The metadata follow-up starts from `91b9ccd`.

## Saving

Save requests now coalesce before the UI captures its immutable snapshot: 100 ms of quiet, bounded by 500 ms of continuous requests. Close and reload barriers force capture. The writer flushes an admitted snapshot without another debounce interval.

The writer retains spans into its last successful JSON document and shared ownership of those records. Unchanged records copy their existing bytes; changed records use the existing serializer. A temporary pointer lookup handles displaced records after insertion, deletion or reordering. Failed writes do not advance the span mapping. Unknown fields, relocatable paths, atomic replacement and backups retain their existing behavior.

Snapshot capture still compares public mutable records, and publishing still assembles and atomically writes a full document. This avoids silently losing callers' direct edits. A journal or controlled mutation API would be a separate persistence design change.

## Search

A lazy trigram candidate index narrows queries before the existing exact scorer. Hash collisions only admit extra candidates. Missing, common or omitted buckets fall back to exhaustive scoring. The index keeps original input order and leaves ranking, match counts, source filtering and final existence checks intact.

The index starts at 256 entries, declines builds above 16 million text code units and retains at most 4 million posting integers plus 65,536 bucket records (about 16.5 MiB). Very short or broad queries can still scan the catalog. The first eligible query pays for index construction; this tradeoff needs workload measurements.

Metadata revisions prepare bounded history overlays and refresh media details only for actual scored matches. Playback changes no longer revisit every cached search entry. Result selection reserves space against the candidate count rather than the full catalog.

## Smart playlists

Each evaluation validates and prepares rule constants once. Title/path normalization is shared between rules for each item, and impossible matches are rejected before disk checks. Playlists share short-lived file-availability observations without extending their expiry. Explicit evaluation still performs fresh checks.

Playback-independent memberships survive unrelated progress edits. Relative date rules use one clock per evaluation and expire at the next possible membership boundary, including recovery from a backward clock adjustment. Previously, passing time alone could leave these memberships stale.

Availability checks remain synchronous for eligible candidates; moving them into a shared background availability service is a further opportunity.

## Watched folders

Filesystem enumeration, existence checks and file-identity reads run on an owned worker. Its mailbox holds at most 1,024 events across 64 roots; overflow becomes a rescan. The UI captures only file facts, validates those facts against current records and applies a batch with one index rebuild. Playback progress, bookmarks and overrides remain owned by the live model.

Cancellation and generations discard results from replaced libraries. Newer queued events defer an older delta. Incomplete scans and unavailable roots do not authorize removals. Rename updates preserve playlist/history references, and completed rescans reconnect unique file identities when a notification was lost. Ambiguous hard-link identities do not authorize reconnection. Removing media retains watch history.

Continuous filesystem changes can defer publication until a current scan completes. Manual/startup folder rediscovery outside the watcher pipeline still uses its existing synchronous implementation.

## Metadata backfill

Chained metadata batches now share a forward cursor. A stable catalog is examined once across the pass, rather than repeatedly scanning its processed prefix for eligibility, collection and continuation. External catalog edits restart the cursor. Only the next bounded batch (32 records by default) is copied into the worker.

Completion merges only file/probe metadata against captured baseline facts. It rejects removed, replaced, renamed or otherwise superseded records and retains live playback, bookmarks and overrides. Metadata changes advance the catalog revision without rebuilding ID/path maps or search text. Retry-only bookkeeping remains durable without invalidating catalog, playlist or history views.

Long passes request a save checkpoint every five seconds and at completion. Each eligible record is attempted once per stable pass; later scheduling retains the existing retry limits and backoff rules. Regression source covers a complete multi-batch pass, external insertion, stale results, preservation of live edits and retry-only invalidation.

## Validation

- Native application compiled and linked locally with MSVC using `build.ps1 -SkipDependencyCheck`; no application launch.
- Regression source passed MSVC syntax-only compilation, including the metadata follow-up. The final syntax check had no diagnostics; malformed path literals from the previous view tests were corrected.
- Added regression cases for save-byte reuse and failed retry, search candidate counts and lazy metadata, prepared smart-rule semantics and time boundaries, bounded watch queues, batch application, stale observations, rename recovery and cancellation.
- Tests, benchmarks and runtime measurements were **not run**. The changes require the user's behavioral and performance validation before release.

## Remaining candidates

Further architectural work could move the separate folder-rediscovery operation off the UI thread, introduce persisted file fingerprints to avoid safe-to-skip duplicate import probes, and replace full-document saving with a journal. These change larger behavior or storage contracts and need workload measurements and regression execution before choosing a design.
