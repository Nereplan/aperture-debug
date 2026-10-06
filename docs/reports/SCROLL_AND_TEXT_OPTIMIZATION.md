# Scrolling and text optimization — 7 October 2026

Source review baseline: `508a96c`. The implementation is in the commit containing this report. Tests and benchmarks were explicitly deferred to the user; the expected savings below describe removed work, not measured application speedups.

## Changes

| Path | Previous work | Updated work |
|---|---|---|
| Playlist sidebar | Reserved space for every playlist and fetched statistics/copied labels before checking visibility | Reserves space based on the viewport and prepares statistics/labels only for visible entries; stops below the viewport when no playlist is being dragged |
| Media group headers | Scanned every group on each projection and pointer hit check | Binary search locates the first visible header or the header under the pointer; projection visits only the visible range |
| Wrapped text | Changed wrapping and trimming on a cached layout, created an ellipsis, then reset the layout on every draw | Single-line, wrapped measurement, and wrapped/ellipsized drawing use separate cache keys; each layout is configured once |

Playlist row positions retain the existing accumulation order. Dragging still updates every row's animation target before culling, and the dragged clone still gets its statistics. Scrolling far down the sidebar still walks the preceding row positions, but no longer prepares their labels or statistics.

Group-header lookup uses the existing sorted layout offsets, including collapsed groups. Projection searches with the same pixel-space clipping expression, retaining headers that touch the viewport boundary. Pointer lookup keeps the existing inclusive top/exclusive bottom rule. These changes reduce header-specific work; series summary counting and other view-model work can still scan all groups.

Wrapped text keeps the existing wrapping, clipping, and ellipsis settings. Font, text scale, and DPI changes still clear the cache. Its 512-entry and estimated 3 MiB limits remain in place, shared across all layout modes. Rapidly changing time/progress strings retain their existing cache bypass.

## Validation performed

- Reviewed the changed source and the layout, drag, and font-cache call sites.
- `git diff --check` passed.
- MSVC x64 Release compilation and linking passed using `build.ps1 -SkipDependencyCheck` with an external output path. This was a compile-only build; the application was not launched or installed.
- No test suites, runtime/visual checks, benchmarks, or release packaging ran. Runtime behavior and performance gains remain unverified.

## Suggested user checks

- Scroll a long playlist sidebar, including smart playlists; drag items in both directions, autoscroll, and rename a visible item.
- Scroll and collapse grouped media, checking headers at viewport edges and clicks immediately above/below a header.
- Hover long playlist titles, resize the window, change font/text scale, and move between monitors with different DPI. Check wrapped messages and empty-state text as well.

## Further optimization candidates

- `App::BuildHubRenderViewModel` and history hover handling still walk history rows outside the viewport. A shared history layout index could narrow drawing and hit checks together while keeping scrolling, selection, and accessibility aligned.
- The playlist detail pane calls `MediaLibrary::GetPlaylistResumeIndex` on each view-model build. That scan repeatedly normalizes and looks up playlist paths. Caching the result could help large playlists, but must track status changes, reordering, and smart-playlist availability refreshes, which can occur without a library-version change.

Profile those paths with a representative library before expanding the optimization scope.
