# Library view pipeline redesign — 7 October 2026

Source review baseline: `bef3068`. The implementation is in the commit containing this report. This is a structural follow-up to the scrolling and history passes: it changes how the application prepares library views and decides which work needs refreshing. Tests and benchmarks were not run, as requested.

## Ownership and preparation

The UI and startup worker now use `LibraryViewProjection`: ordered library indices, optional availability bytes, and group summaries. The UI no longer materializes a `LibraryItemProjection` with its own ID, title, path, status, and playback fields for every matching record. Visible cards continue reading live `MediaItem` records. The extra UI/startup visible-index lists and their parallel invalidation flags are removed. The owning `Query` API remains available and materializes current details only when requested.

The query engine keeps one unfiltered, uncollapsed, unlimited ordering plan for the active grouping/sort configuration. That plan includes manual-playlist exclusion, group classification, episode parsing/order, and duplicate detection. Search, status filtering, group expansion, pinning, and warm capped queries derive compact views from it. They do not repeat those preparation steps unless a relevant library revision or ordering option changes.

Normalized search fields are built lazily on the first text search and reused for subsequent queries within the catalog revision. Display title, original title, full path, and series override remain separate fields so matching cannot accidentally cross a field boundary. Folder order and collapsed-group keys use immutable snapshots; unchanged frame requests compare their shared identities instead of copying their contents.

For an ordinary warm search, the work changes from filtering plus grouping/sorting/materialization to a linear eligibility pass and compact row/group projection. Unfiltered expansion/collapse copies visible index ranges and reuses group totals, taking work proportional to the groups and emitted rows. An unchanged view skips even those passes. These are source-level work reductions, not measured latency claims.

Cold capped queries retain the direct builder's partial-sort/early-termination path rather than preparing an unnecessary unlimited ordering. Filesystem-sensitive queries retain the direct probing path. A rare series override equal to the reserved standalone key falls back to the direct builder when filtering changes that group's special classification.

## Change tracking and consumers

The persistence/conflict version is retained. Derived views additionally use four revision domains:

| Domain | Changes | Main consumers |
|---|---|---|
| Catalog | Membership, source order, paths, titles/overrides, duration and file metadata | Query plans, search fields, media lookup-dependent caches |
| Playback | Visible progress, watched state and recency | Progress/recency/duplicate ordering, status filters, pinning, smart playlists and statistics |
| Playlists | Playlist membership, order, names and rules | Flat-view exclusion and playlist views/statistics |
| History | History membership, progress, timestamps and rewritten paths | Timeline grouping and global-search metadata |

A playlist-only edit no longer refreshes global-search text/metadata or folder/series ordering. A history-only edit no longer rebuilds media ordering. Ordinary title-sorted grouped views read updated progress from live records without rebuilding their compact projection. Preferences and bookmarks retain their persistence updates without retiring these derived views.

Layout uses the projection's shape generation. When refreshed indices and group geometry are unchanged, the generation stays stable and row offsets are reused. Pin-height changes have their own explicit geometry check.

Smart-playlist path snapshots publish a generation whenever re-evaluated, including availability expiry or explicit cache clearing. The playlist UI resolves that generation before using cached row indices. Manual playlists retain a cheap cache hit without scanning playlist membership or looking up the active playlist again. Statistics/resume selection retain the existing availability deadline. Snapshot adoption and undo explicitly reset media, history, playlist, missing-file, and global-search caches because replacement objects can have coincident revision counters.

Public record edits still require notification. `NotifyPlaylistEdited` is for playlist-only changes; the legacy `NotifyPlaylistChanged` conservatively advances all derived domains. Membership/path changes also require `RebuildPathIndex`. Repository mutators and direct application writes were reviewed against this contract. Persistence snapshot comparisons continue detecting direct edits independently of these notifications.

## Behavior and lifetime details

Compact indices are borrowed from the engine and valid against the matching live library. Callers must reacquire them after a query mutation or library replacement. Startup transfers the compact result by move; the UI also seeds its ordering plan from that result, avoiding another parse/sort when initial pinning is applied.

Group actions now request expanded membership. Playing, revealing, or removing a collapsed series therefore resolves its members instead of interpreting the next visible group's rows as its own. Menu labels/path values are captured before focus handling can refresh the engine cache.

Grouped rows now use original source index as the last tie-breaker when all existing sort keys tie. This stabilizes equal records and manual grouped queries so filtering a prepared ordering agrees with directly sorting the matching subset. Ordinary unique-ID ordering is unchanged. Existing filtered counts, cap behavior, manual-playlist exclusion, recency tie priority, group labels and totals are preserved by the projection code.

## Validation and limitations

- Source review covered ownership/lifetimes, query dependencies, model mutation notifications, replacement/undo, group actions, and playlist availability refresh.
- Regression source covers direct-versus-prepared query results across grouping/sorting/filtering/caps, cache rebuild counters, live metadata materialization, startup plan adoption, cold bounded queries, smart-playlist path generations, and the standalone-key collision. Existing folder-order fixtures and the benchmark harness were adapted to immutable order snapshots. These sources have not been executed.
- MSVC x64 Release application compilation and linking passed with `build.ps1 -SkipDependencyCheck`. The executable was written outside the workspace to `%LOCALAPPDATA%\Codex\tools\scratch\aperture-view-pipeline-20261007\Aperture.exe`. The final log (`compile-verified.log` in that directory) reported no compiler warnings/errors. `git diff --check` passed.
- No tests, benchmarks, app launches, installation, packaging, publication, or runtime profiling were performed. Runtime behavior and the size of any speedup remain unverified.

The plan and lazy normalized strings deliberately retain memory to reduce repeated work. Only one ordering plan is retained; changing grouping/sort options can still rebuild it. Catalog edits still require preparation, and search still scans candidate records. Duplicate ordering must refresh when playback affects keeper priority. Saving and the playback engine were not redesigned in this pass; no whole-app speedup percentage is claimed.

For user validation, focus on large-library searches and group toggles; pinning and watched-status changes; playlist moves/imports and availability-based smart lists; collapsed-group play/reveal/removal; and undo/reload while a search is open. Check row geometry, selection, and results as well as responsiveness.
