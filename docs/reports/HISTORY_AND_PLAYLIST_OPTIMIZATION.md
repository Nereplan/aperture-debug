# History and playlist optimization — 7 October 2026

Source review baseline: `3fee131`. The implementation is in the commit containing this report. This follows the [scrolling and text pass](SCROLL_AND_TEXT_OPTIMIZATION.md) and addresses its two follow-up candidates. Tests and benchmarks remain deferred at the user's request.

## History rows

History drawing, mouse hit checks, tooltip preparation, and accessible row construction previously walked every row above the viewport. Tooltip preparation also copied offscreen titles before its clipping check. Each group now uses a shared `HistoryGroupLayout`: row positions are calculated directly, and binary searches locate the intersecting range. Only those rows need per-row processing. The current projection has at most four time groups.

The same geometry defines group spacing, scroll limits, focus scrolling, and the renderer's standalone fallback. Row heights, gaps, and the existing viewport margins are retained. Accessibility still applies its final integer screen-coordinate intersection check, with a pixel of search margin for rounding. Header relationship lists still include every member of a visible group; their cost is not eliminated. Focus navigation still searches the compact index list for the focused record.

An explicit `hasProjectedLayout` flag distinguishes an empty viewport from an unprojected view model. Touch overscroll can otherwise make both visible-row and visible-header lists empty and trigger a fresh sort/copy of the full history inside the renderer. The application now honors its empty projection. Selected state is already present on each projected row, so the application also avoids copying the full selection set into every frame.

Positions now use multiplication within a group instead of repeated addition. All history consumers share that calculation; at unusual fractional display scales, rounding can differ from the previous accumulated positions. Visual behavior has not been checked at runtime.

## Playlist resume selection

The playlist detail pane previously called `GetPlaylistResumeIndex` on every frame, repeating path normalization and media lookups across the playlist. `PlaylistStats` now computes the resume index during its existing statistics pass. Unchanged detail views reuse that value through the existing cache.

The selection rule stays the same: the first in-progress item wins, otherwise the first new or missing library record, otherwise index zero. Direct resume actions retain the existing uncached API. Library edits, progress/status changes, item reordering, and explicit cache invalidation retire the cached result through the existing mechanisms.

Availability-based smart playlists can change membership without a library-version change. Their statistics now share the path snapshot's two-second deadline. A path refresh also removes the corresponding cached statistics and resume index. This prevents an earlier snapshot's resume position or totals from surviving a membership refresh.

## Validation and limits

- Reviewed geometry consumers, resume-selection priority, and library/cache invalidation paths.
- `git diff --check` passed.
- MSVC x64 Release compilation and linking passed with `build.ps1 -SkipDependencyCheck`, writing the executable outside the workspace. No compiler warnings or errors were reported in the build log.
- No tests, benchmarks, application launches, installation, packaging, or publication ran. Actual CPU/latency improvements and runtime behavior remain unmeasured and unverified.

The source eliminates full-history row walks on the listed hot paths and repeated resume scans on cache hits. It does not establish a whole-app speedup percentage. History projection construction and visible group relationship lists can still process the full history, and a cold playlist statistics scan still processes its members.

## Suggested user checks

- Scroll a long history across all four date groups; check row edges, gaps, hover/delete targets, tooltips, selection, keyboard focus scrolling, and accessibility bounds.
- Check touch overscroll, very small windows, empty history, and several display scales, including a custom fractional scale.
- Check playlist resume cards with in-progress, new, completed, and missing records, then reorder items and change their watched status.
- For availability-based smart playlists, add/remove a matching file and wait beyond two seconds; check the item count and resume card together.
