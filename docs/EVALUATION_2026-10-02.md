# Aperture implementation evaluation — 2026-10-02

Evaluated source: `70c271ce17c270762c9b3f574ee3f0f749326bc4`, version 2.0.0.0. This is an implementation review with local component probes and resource benchmarks, not a redesign proposal. Production source, user profiles, release tags, and published assets were not changed.

## Assessment and priorities

The best near-term improvements are data preservation, consistent playback identity and privacy rules, and avoiding full-library work on the UI thread. Aperture already has useful foundations: asynchronous startup snapshots with generation checks, bounded renderer caches, coalesced player events, direct D3D11VA playback, independent preview decoders, atomic file replacement, backup recovery, native shell/media integration, and extensive component tests. Replacing these systems wholesale would discard useful work.

The review identifies 30 findings. Five are P1; the remainder are P2 or P3. P1 means fix first because of persistent data loss, privacy violations, incorrect playback, or an input-triggered exception. P2 means consequential correctness, responsiveness, recovery, or Windows behavior. P3 means lower-impact resource waste, observability debt, or a targeted optimization opportunity. No P0 vulnerability was established.

Evidence labels:

- **Reproduced:** observed in locally compiled production components using isolated temporary profiles. This does not imply an end-to-end GUI reproduction.
- **Confirmed in code:** the implementation and caller establish the behavior. A particular hardware, network, or OS failure may still be needed to observe it interactively.
- **Opportunity:** a concrete implementation cost or duplication exists, but the proposed performance benefit needs measurement. Conditional consequences are explicitly identified.

| Order | Finding | Priority | Practical consequence |
|---|---|---|---|
| 1 | F01: unavailable watched roots remove records | P1 | Lost progress, preferences, bookmarks, and playlist membership |
| 2 | F02: private playback still saves history | P1 | Private media paths and viewing timestamps persist |
| 3 | F03: reordering changes the identity of a borrowed item | P1 | Wrong file's resume position, title, and preferences |
| 4 | F04: malformed smart rules throw during evaluation | P1 | User-editable or persisted input can escape into UI execution |
| 5 | F05: invalid store structure is accepted as empty | P1 | Valid JSON with the wrong schema can become an empty saved library |
| 6 | F06: volume scale and persistence are inconsistent | P2 | Requested intermediate volume can become 100%; saved volume resets |
| 7 | F07: synchronous whole-library saves | P2 | Large-library UI stalls every five seconds during playback |
| 8 | F08: successful zero-duration probes remain eligible | P2 | Repeated metadata jobs without retry backoff |
| 9 | F09: UIA root requests never reach the provider | P2 | Rich accessibility patterns are not exposed through the intended path |
| 10 | F10–F18 | P2 | Overaggressive playback recovery, network/UI blocking, persistence and device recovery defects |

## Evidence and measurement limits

Read and traced application orchestration, player initialization/events/commands, import and backfill workers, library/query/search models, renderer and overlay lifecycle, settings/session persistence, folder watching, IPC, online subtitles, updating, accessibility, Windows placement, build hardening, and relevant tests. Findings below cite source files, functions, and line numbers at the evaluated commit.

Ran the existing save and query resource benchmarks locally and compiled a small probe executable against the real library/settings implementations. Probe executables and their profiles lived in fresh temporary directories and were removed by the runner. There were no real media decoders, external network requests, registry changes, or live profile writes in these probes.

The benchmarks use MSVC `/O2`, one CPU affinity bit, one warmup, and seven measured samples. The application Release configuration instead selects `/O1` plus LTCG: see [CMakeLists.txt](../CMakeLists.txt), lines 325–337. Consequently these are measured costs of the production algorithms in the benchmark configuration, not exact shipped-app frame timings. Allocation totals mean cumulative tracked C++ allocations; peak live values exclude pre-existing allocations, libmpv, COM, driver memory, and process RSS. No cold-start wall-clock, GPU utilization, frame-drop, power, or working-set claims are made from these measurements. A full application build/regression run was not necessary for this read-only evaluation and was not performed.

At 25,000 synthetic media items:

| Workload | Median ms | Allocation calls | Cumulative allocated MiB | Peak tracked live MiB |
|---|---:|---:|---:|---:|
| First library save | 147.373 | 525,206 | 83.96 | 18.22 |
| Unchanged library save | 109.053 | 524,929 | 55.08 | 8.61 |
| Save after one progress change | 128.523 | 524,984 | 77.68 | 23.67 |
| Load library stores | 434.860 | 2,165,962 | 214.23 | 65.05 |
| Full flat projection | 14.560 | 50,008 | 9.02 | 8.64 |
| Full folder projection | 22.532 | 110,009 | 13.63 | 9.21 |
| Full series projection | 38.285 | 357,965 | 23.53 | 9.10 |
| Flat projection capped at 50 | 0.747 | 107 | 0.78 | 0.78 |
| Smart playlist capped at 50 | 0.430 | 52 | 0.20 | 0.20 |
| Warm global search, simple query | 0.720 | 153 | 0.015 | 0.015 |
| Global search index construction | 99.823 | 573,004 | 45.16 | 28.78 |

Save workloads also include playlists, history, and a subset of records with preferences/extra fields. The aggregate serialized store size was 9,024,568 bytes. At 500 items, unchanged save was 3.342 ms and changed save 7.430 ms: F07 is primarily a scaling problem. Query mode 9 times index construction alone; query modes 10–12 are multi-token/rejected searches with filesystem existence checks disabled. They must not be interpreted as disk-existence benchmarks.

Raw evidence: [save CSV](evaluation-evidence-2026-10-02/save-results.csv), [query CSV](evaluation-evidence-2026-10-02/query-results.csv), [probe output](evaluation-evidence-2026-10-02/probes-results.txt), [probe source](evaluation-evidence-2026-10-02/probes.cpp), and [probe runner](evaluation-evidence-2026-10-02/run_probes.ps1). Existing benchmark drivers: [save](../tests/benchmark/run_save_benchmark.ps1) and [query](../tests/benchmark/run_query_benchmark.ps1). Their result-verification checks completed successfully.

## 1. Architecture & Code Organization

### F03 — A borrowed `MediaItem*` can become another media item before use

**P1 · Reproduced at the model boundary; confirmed application caller.** [AppPlayback.cpp](../src/AppPlayback.cpp), `OpenAndPlayMedia`, lines 657–752, obtains a pointer from `FindByPath`/`AddOrUpdateMedia`, calls `PromoteLastWatchedMedia` at 668, then reads preferences and resume position through that pointer. [MediaLibrary.cpp](../src/MediaLibrary.cpp), `PromoteMostRecentlyWatched`, lines 331–348, swaps the most recently watched record with the front element.

New records are inserted at the front with no watched timestamp (`AddOrUpdateMediaCore`, lines 887–923). If another item was previously watched, promotion swaps that old item into the address returned for the new item. The address remains valid but its identity changes. Speed, aspect ratio, subtitle/audio delay, color adjustments, resume position, history duration, and window title can therefore come from the old file. This is a logical identity bug, not a demonstrated dangling-pointer access.

The probe retrieved `new-file`, promoted the previous file, and the same pointer then reported `previous-file` with resume position 42. Preserve the target ID/path across mutations and reacquire afterward, or copy the target preferences before reordering. A narrow fix needs no UI change.

The same review exposes an architectural boundary worth tightening: library mutators both change model state and perform persistence, while App also owns saving and error presentation. `RemoveMedia` ignores its callers' deferred-save intent (F01); opening media can save more than once (F07); privacy is checked by callers rather than enforced at a single playback-persistence boundary (F02). Small explicit mutation results and a common persistence policy would address these concrete defects without a broad App rewrite.

## 2. Performance & Startup

### F07 — Disk-write suppression happens after expensive full serialization on the UI thread

**P2 · Measured and confirmed in code.** [App.cpp](../src/App.cpp), `Update`, lines 1258–1259, calls `SaveLibrary` and `SaveSession` every five seconds while playing. [AppPersistence.cpp](../src/AppPersistence.cpp), `SaveLibrary`, line 35, calls the synchronous library save. [MediaLibrary.cpp](../src/MediaLibrary.cpp), `Save`, lines 2115–2271, performs a writability check and constructs/stringifies the complete library, playlists, history, and watched-folder documents. Only `saveDocument` at 2256 compares the finished JSON against the last saved string.

An unchanged 25,000-item save still took 109.053 ms and 524,929 allocations. A single progress update took 128.523 ms. This work blocks input and overlay rendering even though mpv can continue presenting independently. Atomic replacement and unchanged-byte suppression already reduce disk traffic, but do not avoid this CPU/allocation cost.

Introduce per-store dirty generations so unchanged stores return before serialization. Coalesce playback progress saves and serialize/write immutable snapshots on a serialized worker if dirty-save cost remains material. Preserve atomic writes, latest-generation ordering, shutdown flush behavior, and error reporting. Do not move reads of mutable `mediaItems` onto a worker unsynchronized. Also eliminate duplicate saving at domain-mutator/App boundaries; for example `AddOrUpdateMedia` saves at 929 and `AddToHistory` saves at 1270/1282 during one open.

### F23 — Search invalidation is expensive for progress updates, yet incomplete for history edits

**P2 · Measured construction cost; confirmed invalidation behavior.** [GlobalSearch.cpp](../src/GlobalSearch.cpp), `IsDirty`, line 104, keys the whole index to `library.GetVersion`; `Query`, line 254, rebuilds synchronously whenever dirty. `RebuildIndex`, lines 108–174, recopies paths/titles and regenerates normalized strings and token vectors for every entry. [MediaLibrary.cpp](../src/MediaLibrary.cpp), `UpdateProgress`, lines 1212–1215, advances this version on second-level progress/timestamp changes. The next search after playback therefore rebuilds unchanged lexical content. The 25,000-item index rebuild measured 99.823 ms, 573,004 allocations, and 28.78 MiB peak tracked live memory, versus 0.720 ms for a simple warm query.

Conversely `AddToHistory` (1257–1282) and `RemoveFromHistory` (1285–1289) save without incrementing the version. The UI's history removal handlers ([AppMenus.cpp](../src/AppMenus.cpp):471 and [App.cpp](../src/App.cpp):3243) do not invalidate the global index. A previously indexed history-only entry can therefore remain searchable after removal until another version-changing mutation occurs. This stale-result scenario was established from the code, not separately exercised in the probe.

Separate searchable-content generation from progress/ranking generation and history membership. Refresh mutable result metadata without regenerating title/path tokens; invalidate membership when history rows change. Apply similar restraint to full library projections: measured full series construction is 38.285 ms, but cached projections and capped selection are already useful optimizations and should remain.

### F28 — Startup milestones can claim a rendered frame after rendering failed

**P3 · Confirmed in code.** [App.cpp](../src/App.cpp), `Render`, lines 1559–1565, discards the Boolean returned by `renderer.EndDraw()` and immediately records `first-gpu-shell`/`first-populated-frame`. [Renderer.cpp](../src/Renderer.cpp), `EndDraw`, lines 492–507, returns false for missing resources, draw failures, and device loss. [main.cpp](../src/main.cpp), benchmark pumping at 315–338, trusts the App populated-frame flag.

A failure can make the populated-frame benchmark report readiness without a successful populated presentation. Gate these flags on successful rendering/presentation and distinguish attempted, occluded, and visible frames. This improves the reliability of later performance measurements. Async D3D prewarm and background library snapshots are already implemented; the measured library load cost is not evidence that it blocks first paint.

## 3. Memory, CPU & GPU Efficiency

### F19 — Visible playback controls require a full-window layered surface update

**P2 · Confirmed resource path; optimization benefit needs profiling.** [OverlayRenderer.cpp](../src/OverlayRenderer.cpp), `RecreateDIB`, lines 197–214, allocates a full-window 32-bit DIB. `BeginDraw`, line 232, clears the full surface; `EndDrawAndPresent`, line 252, submits it through `UpdateLayeredWindow`. [App.cpp](../src/App.cpp):1251–1257 requests continuous redraw while playing with visible controls, diagnostics, flyouts, or context menus. [App.h](../src/App.h):67 removes the hub frame-latency handle during playback; [main.cpp](../src/main.cpp):469 uses an 8 ms wait on the redraw path.

A 3840×2160 DIB is 31.64 MiB. At 60 updates/second, its surface-size arithmetic is 1.85 GiB/second; this is not measured bus traffic or a claim that every byte crosses the GPU bus. The code has no explicit display-rate deadline for this overlay path. Microsoft confirms that [UpdateLayeredWindow updates the entire window](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-updatelayeredwindow).

First measure visible-controls versus hidden-controls CPU, update rate, and power. Cap overlay animation updates to an appropriate display cadence and skip frames whose content has not changed. A smaller surface, dirty-region-capable presentation, or DirectComposition may offer further savings, but requires a visual/performance comparison before changing the rendering architecture. Preserve transparency, native hit testing, and the existing layout. Hidden controls already avoid continuous empty-overlay uploading; do not characterize normal hidden-controls playback as continuously copying this DIB.

### F25 — HTTP downloads temporarily hold two complete response bodies

**P3 · Confirmed in code; size-dependent impact.** [HttpTransport.cpp](../src/HttpTransport.cpp):104–127 builds `responseBody`, then copies it into `response.body`. A response near the updater's 150 MiB ceiling ([Updater.h](../src/Updater.h):76) temporarily requires about 300 MiB for the two logical bodies, before capacity growth, read buffers, and application memory. This concerns the native updater fallback and other transport consumers; normal packaged updating prefers the external script (F12).

Move the completed string into the response and use a reusable fixed-size read buffer. For large update packages, stream to the staging file while hashing, retaining all size, signature, and hash checks. The copy is a straightforward saving; a streaming transport is a larger opportunity and should be evaluated separately.

### F27 — Removed folder watches retain their 64 KiB buffers until watcher shutdown

**P3 · Confirmed in code.** [FolderWatcher.cpp](../src/FolderWatcher.cpp), `RemoveWatch`/`ClearWatches`, lines 72–105, move watch objects into `retiredWatches`. The worker checks `retired` but never removes completed retired objects. [FolderWatcher.h](../src/FolderWatcher.h):66–68 deliberately retains them for completion-key safety; each watch has a 64 KiB buffer (`kBufferSize`, line 88). Repeated add/remove operations grow memory with the total number ever removed during the session, not current watches.

The retention protects against use-after-free and must not be replaced with immediate deletion. Track outstanding I/O/completion ownership and reclaim a retired watch once its final completion has been consumed. Low priority for ordinary small watch lists; useful for long sessions or watch-list churn.

## 4. Playback Pipeline & Media Handling

### F06 — Volume is normalized in the player but treated as a percentage by two callers

**P2 · Persistence reproduced; call-site mismatch confirmed.** [Settings.h](../src/Settings.h):131 defines `volume` in 0..1. [MpvPlayer.cpp](../src/MpvPlayer.cpp), `SetVolume`, lines 817–833, clamps that normalized range. [App.cpp](../src/App.cpp), `ExecuteCommandLineOptions`, lines 1142–1145, correctly calls `SetVolume(vol/100)` but stores rounded 0..100 in `settings.volume`; automation does the same in [AppPlayback.cpp](../src/AppPlayback.cpp):533–540.

On first lazy initialization, `EnsurePlayerInitialized` reapplies `settings.volume` directly at line 62. Thus `--volume 50` supplied before the first media open becomes a stored value of 50 and is clamped to 1.0 when initialization reapplies it. Existing initialized-player commands can appear to work, which makes the defect easy to miss.

Separately [Settings.cpp](../src/Settings.cpp) resets volume to 1.0 at 118 and migration creates a `volume` key at 394, but Load does not read that key and Save does not update it. The probe saved 0.25 and loaded 1.0. Keep one normalized representation in Settings, explicitly read/write it, and convert only at CLI/automation/OSD boundaries. Verify zero, 25, 50, and 100 both before lazy initialization and after initialization.

### F20 — Stream URLs enter filesystem-only library, history, and session paths

**P2 · Confirmed in code; no live stream opened during review.** `OpenAndPlayMedia` correctly recognizes streams and skips local item creation at [AppPlayback.cpp](../src/AppPlayback.cpp):631–668. However it unconditionally calls `AddToHistory` at 784. `HandlePlayerEvents`, lines 178–180, sends every non-private current path to `library.UpdateProgress`, whose fallback creates a media item if absent ([MediaLibrary.cpp](../src/MediaLibrary.cpp):1193–1197).

Both library methods call `ResolveAppPath`. [PathFileHelpers.h](../src/PathFileHelpers.h):99–123 normalizes slash separators and treats URLs as relative filesystem paths. Session Save also calls `ToRelocatablePath` ([PlaybackSession.cpp](../src/PlaybackSession.cpp):161–166), and Load resolves saved targets as file paths at 213. This can pollute the local library with a fabricated path, break stream history/restoration, and retain raw sensitive query parameters. It bypasses the separately implemented recent-stream policy, which defaults off and redacts tokens ([NetworkStream.h](../src/NetworkStream.h):74; [NetworkStream.cpp](../src/NetworkStream.cpp):598–609).

Carry a local-file/stream distinction through progress, history, and session persistence. Preserve stream URL spelling/case for playback, redact persistent display/history data, and respect the existing recent-stream opt-in and private mode. Simply forcing filesystem normalization to accept URLs would leave downstream file existence/probing assumptions unresolved.

### F21 — Smart playlists with `status = missing` cannot match missing files

**P2 · Reproduced.** [MediaLibrary.cpp](../src/MediaLibrary.cpp), `EvaluateSmartPlaylist`, line 1667, invokes `criteria.Matches(item)` without supplying existence state. The default is true; `SmartPlaylistRule::Matches`, lines 2481–2484, implements missing as `!existsOnDisk`. A nonexistent fixture returned zero matching items.

Pass an accurate, error-aware existence result for rules that require it. Avoid adding synchronous filesystem checks for every item in ordinary smart playlists; determine whether criteria require existence and use a cached availability snapshot where practical. Treat unavailable storage carefully alongside F01.

The main playback path already requests D3D11 direct decoding, bounds demuxer/subtitle caches, and tears down hub extractors before allocating the main decoder ([MpvPlayer.cpp](../src/MpvPlayer.cpp):208–249; [AppPlayback.cpp](../src/AppPlayback.cpp):636–639). Software previews use small output buffers and limited decoder threads. No evidence here justifies a general replacement of libmpv or automatic enlargement of every cache.

## 5. Rendering, D3D11 & Direct2D

### F18 — Renderer device recovery does not invalidate externally owned preview bitmaps

**P2 · Confirmed lifecycle omission; actual device-reset consequence needs a runtime test.** [Renderer.cpp](../src/Renderer.cpp), `ReleaseDeviceResources`, lines 190–209, drops the renderer's D2D/D3D device and cached resources. `RecreateDeviceResources`, lines 409–412, creates a fresh device. `MpvPreviewPlayer` independently retains `previewBitmap`/`seekbarBitmap`; they are reset during shutdown or output-size changes ([MpvPlayer.cpp](../src/MpvPlayer.cpp):2209–2210, 2244–2245), not renderer device recovery.

`UpdateFrame`, lines 2457–2462, creates a bitmap from the supplied context only when the existing pointer is null, then ignores `CopyFromMemory`'s HRESULT. After replacement of the renderer device, an old bitmap can remain associated with the old resource domain and be handed back to the new renderer. Microsoft documents [ID2D1Bitmap as device-dependent and requiring recreation with its rendering target](https://learn.microsoft.com/en-us/windows/win32/api/d2d1/nn-d2d1-id2d1bitmap).

Expose a renderer device generation and invalidate preview GPU resources when it changes, keeping CPU pixels and decoder state where possible. Check upload failures before declaring a frame usable. Verify a forced device recreation while a hover preview is active. The renderer already clears its own device resources and falls back to WARP on device creation failure; the omission is at the external-owner boundary.

F19 describes the playback overlay's separate DC/layered path. F28 describes ignored presentation results. Neither establishes that normal hardware video frames are copied through the overlay: mpv presents video independently.

## 6. Concurrency, Threading & Lifecycle

### F08 — Successful metadata probes with no duration can chain forever

**P2 · Reproduced eligibility defect; real problematic media not used.** [MediaLibrary.cpp](../src/MediaLibrary.cpp), `ProbeMediaMetadata`, lines 287–314, returns success when a source reader exists even if duration retrieval fails and remains zero. `BackfillMissingMetadata` treats `durationSec <= 0` as eligible at 2287, but on probe success stores zero and resets both retry count and retry timestamp at 2315–2325.

[AppImportJobs.cpp](../src/AppImportJobs.cpp), `FinishBackfillJob`, lines 312–320, immediately chains another batch whenever eligible items remain. A zero-duration success remains perpetually eligible and bypasses retry backoff. The probe completed the same zero-duration item twice with attempt count still zero. In the App this can repeatedly create worker jobs, scan candidates, probe files, invalidate UI state, announce completion, and defer the final chained-batch save.

Represent “probed successfully, duration unavailable” separately from “not yet probed,” or apply bounded retry/backoff whenever required metadata remains unresolved. Share the eligibility predicate currently repeated in `HasEligibleBackfillItems`, snapshot collection, and `BackfillMissingMetadata`; this prevents future disagreement without changing batching or visuals.

### F15 — Media Foundation startup/shutdown accounting races across threads

**P2 · Confirmed interleaving; not stress-reproduced.** [MediaFoundation.cpp](../src/MediaFoundation.cpp), `Acquire`, lines 33–42, checks initialization under a mutex, releases it, calls `MFStartup`, then reacquires it. Two first callers can both see false and both call `MFStartup`, while application Release calls `MFShutdown` only once when the shared count reaches zero. `Release`, lines 45–52, also clears the initialized flag before the shutdown call, allowing a new startup to overlap that transition.

Import and backfill use separate workers and each holds a `ScopedAcquire`; the class must support concurrent clients. Microsoft requires [one MFShutdown for every MFStartup](https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfstartup). The current local reference count does not guarantee that pairing. Actual retained platform resources were not measured.

Serialize initializing/stopping transitions with a state machine and condition variable, or use a suitably scoped process lifetime owner. Preserve the delay-load failure handling; removing all synchronization or shutdown is not a fix. A focused two-caller test with instrumented startup/shutdown gates is more useful than repeated full-app runs.

### F22 — Cancellation cannot interrupt synchronous external calls, but close joins them

**P2 · Confirmed blocking contract; hangs are conditional.** [AppImportJobs.cpp](../src/AppImportJobs.cpp), `CancelImportJob` at 164 and `CancelBackfillJob` at 328, join workers on the UI thread. Cancellation is checked between probes, not inside the synchronous MF source-reader operation ([MediaLibrary.cpp](../src/MediaLibrary.cpp):302). [Renderer.cpp](../src/Renderer.cpp):84 joins thumbnail workers that may be in Shell `GetImage` extraction at 1322.

A stalled media source or Shell thumbnail provider can therefore keep close/cancel blocked. Moving work onto a worker prevents normal UI blocking during processing, but does not establish bounded shutdown. No deliberately hung Shell extension or remote media fixture was exercised.

Make cancellation and worker lifetime explicit. For APIs without effective cancellation, isolate risky extraction or use an orderly asynchronous close that keeps owned state alive until completion. Do not detach a worker that captures `this`, nor free an OVERLAPPED completion target early. Existing callback fences and repeated-shutdown guards are useful and should remain.

## 7. Reliability, Error Handling & Recovery

### F10 — Recoverable command failures are treated as fatal playback failures

**P2 · Confirmed in code.** [MpvPlayer.cpp](../src/MpvPlayer.cpp), `ReportError`/`ReportErrorMessage`, lines 114–131, route all errors through one callback. This includes missing external subtitles (`LoadExternalSubtitle`, lines 1642–1657), seek failure (753), and property failures such as volume (831). [AppPlayback.cpp](../src/AppPlayback.cpp), `HandlePlayerEvents`, lines 226–238, closes the player whenever the error deque is nonempty and shows the local/network playback recovery dialog.

A subtitle disappearing between selection and loading can terminate otherwise healthy video playback. An unsupported seek on a live stream can produce the same whole-playback recovery flow. These are different from decode/open failures. Carry an error category or recoverability flag and show existing OSD/error UI for nonfatal operations while keeping playback active. Keep fatal recovery for failed open/decode/output loss and retain queue/position information.

### F11 — Mid-response WinHTTP failures are returned as success

**P2 · Confirmed in code; not fault-injected over a real connection.** [HttpTransport.cpp](../src/HttpTransport.cpp):107–128 exits the read loop when `WinHttpQueryDataAvailable` fails or `WinHttpReadData` fails, closes handles, then unconditionally sets success and returns the accumulated body. It does not distinguish failure from clean end-of-response.

The caller can parse/store a partial subtitle file as if the download succeeded, or display a misleading JSON/parser failure for an interrupted manifest. The updater's package SHA-256 verification at [Updater.cpp](../src/Updater.cpp):993–996 still rejects an incomplete package; this is not evidence of arbitrary update execution. Microsoft explicitly distinguishes [WinHttpReadData failure from a successful zero-byte end-of-response](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpreaddata).

Return the actual timeout/network/cancellation status on query/read failure, preserve `GetLastError`, and expose success only on a clean completion. Validate expected body length where available. One deterministic transport interruption test is sufficient to cover the contract.

### F29 — Critical mpv initialization options are not checked

**P3 · Confirmed observability gap; compatibility impact is conditional.** [MpvPlayer.cpp](../src/MpvPlayer.cpp), `Initialize`, lines 195–257, ignores return values for window attachment and options controlling decoder, GPU API/context, cache limits, fonts, and subtitle behavior. Preview/extractor initialization follows the same pattern. An option rejected by a changed/custom libmpv build can silently leave defaults in effect while the application assumes the intended resource policy.

Use a small checked option helper. Fail initialization only for essential embedding/output requirements; log or fall back for optional tuning. Validate options against the bundled build rather than claiming an unsupported option has already been observed. This is a better maintenance target than changing established playback tuning without representative codec/hardware evidence.

## 8. Persistence, Configuration & Data Integrity

### F01 — An unavailable watched folder is interpreted as removal of all its media

**P1 · Reproduced.** [MediaLibrary.cpp](../src/MediaLibrary.cpp), `ReconcileRescan`, lines 1498–1513, treats every invalid `GetFileAttributesW` result as permission to remove all records under the root. It does not distinguish a disconnected drive/share, access denial, transient I/O failure, and confirmed deletion. [AppLibrary.cpp](../src/AppLibrary.cpp), `RescanAllWatchedFolders`, lines 447–454, calls this directly; watcher overflow reconciliation can also invoke rescanning.

`RemoveMedia`, lines 1167–1189, removes library records and playlist paths, then always calls Save. Thus even `ReconcileRescan(root, false)` persists removal. The isolated probe removed its media record and playlist membership and wrote `[]` despite `saveChanges=false`. Per-record progress, preferences, bookmarks, and metadata overrides disappear with the record; media files themselves are not deleted.

Preserve records when a root is unavailable and publish an availability/error state. Only remove records after a successful, sufficiently complete reconciliation under an explicit deletion policy. The post-enumeration path at 1544–1553 also uses a Boolean `FileExists` after an incomplete/permission-skipping scan, so it needs the same error-aware treatment. Pass deferred-save intent through removal APIs and perform one final checked save.

### F05 — Syntactically valid but structurally invalid JSON is accepted as a successful empty store

**P1 · Reproduced.** [MediaLibrary.cpp](../src/MediaLibrary.cpp), `LoadFromRoot`/`loadStore`, lines 1736–1752, validates only “array or object.” Its backup validator uses the same test. The typed load at 1771–1774 finds no items when an object has the wrong `items` type but does not mark the store failed.

The fixture `{"schemaVersion":2,"items":{"unexpected":true}}` returned success with zero media items. Because App accepts a successful snapshot, subsequent Save can overwrite the source with an empty array. An unrelated JSON object is also accepted; for older versions `MigrateArraySchema`, lines 241–253, can replace a wrong/missing collection with an empty one. The background snapshot's publication fence does not protect against a schema validator that calls invalid state successful.

Validate each store's required structure and supported schema before publication and use that validator for recovery copies. Permit deliberate legacy-array support and per-record salvage where intended, but report malformed required collections. Future versions are currently accepted (`fromVersion >= targetVersion` at 242) while Save reconstructs root documents; protect unknown future formats from destructive downgrade instead of assuming compatibility. This future-schema concern is code-based, not a separately reproduced migration loss.

### F13 — Failed settings/session loads do not prevent later replacement with defaults

**P2 · Settings replacement reproduced; App integration confirmed.** [main.cpp](../src/main.cpp):177–178 ignores the status of startup settings Load. [App.cpp](../src/App.cpp):1027 adopts that copy, 1028 ignores fallback Load status, and 1047 ignores session Load status. `Settings::Load` resets defaults before reading and returns failure when no valid backup exists ([Settings.cpp](../src/Settings.cpp):509–532). Normal shutdown calls `SaveAll` at App.cpp:895, and Settings Save has no failed-load preservation fence at 899–905.

The probe loaded malformed settings unsuccessfully, then saved successfully and replaced the malformed source with valid default-based JSON. This masks the underlying problem and destroys the original live file even if a recovery copy happens to remain. A temporary read/sharing failure can be more serious: a later writable save can replace valid but unread data.

Report failed startup loads and retain the source until an explicit reset or successful recovery. Apply a per-store load-success/read-only fence, allowing the app to use in-memory defaults without silently replacing unrecovered state. Preserve current unknown-field retention and backup recovery for valid files.

### F14 — Backup and live-file ceilings can leave large libraries without usable recovery

**P2 · Backup deletion reproduced; read/write mismatch confirmed.** [BackupFiles.h](../src/BackupFiles.h), `CreateRotatingBackup`, lines 49–78, caps the entire backups directory at 32 MiB after creating the newest backup. A single source over 32 MiB causes even its newly created recovery copy to be deleted, yet the function returns `Ok(bak1)`. The 33 MiB probe returned success and a nonexistent backup path.

Rotation also uses file last-write timestamps inherited from copied sources as its pruning order, and rotation move results are ignored. Save callers ignore the backup status altogether ([MediaLibrary.cpp](../src/MediaLibrary.cpp):2257; [Settings.cpp](../src/Settings.cpp):902). The user therefore has no indication that a claimed recovery copy is absent.

Separately [PathFileHelpers.h](../src/PathFileHelpers.h), `ReadTextFile`, line 332, and [Json.h](../src/Json.h):21 reject input over 64 MiB, while normal serialization/atomic-write paths do not enforce a corresponding output ceiling. A sufficiently large saved store can become unreadable by this application and lack backup recovery. A 64+ MiB library roundtrip was not run; this is a confirmed contract mismatch, not a measured capacity threshold in item counts.

Guarantee at least one recoverable snapshot per durable store or return a clear backup-capacity failure. Align output/read limits before committing data, or adopt a scalable store format through a tested migration. Do not simply remove all size guards. The four store files are individually atomic rather than one multi-file transaction; a generation manifest is a possible later improvement if cross-store recovery evidence warrants it.

## 9. Windows Integration & Native Behavior

### F16 — Single-instance timeout falls back to an unbounded synchronous send

**P2 · Confirmed in code.** [main.cpp](../src/main.cpp):155 uses a five-second `SendMessageTimeoutW`, then on failure calls plain `SendMessageW` at 156. A hung first instance can therefore hang the second indefinitely, defeating the timeout. If the first command eventually completes after a timeout, retrying the same open can also create duplicate processing; that race was not reproduced.

Retain bounded delivery and surface a useful failure/result. Use a request ID and acknowledgement if retry is required. The existing-instance branch also exits successfully after its one-second window search even when no receiver window was found (146–164), so slow initialization can silently drop the launch target. Prefer a bounded readiness handshake rather than unbounded sending or an unconditional success exit.

### F17 — Restoring window placement rejects valid negative monitor coordinates

**P2 · Confirmed in code.** [AppPersistence.cpp](../src/AppPersistence.cpp), `PersistWindowPlacement`, lines 189–193, stores physical screen coordinates, including valid negative coordinates on monitors left of/above the primary display. [main.cpp](../src/main.cpp):205 requires both coordinates to be nonnegative before selecting the saved monitor and lines 223–224 separately recenter negative axes.

The app therefore loses placement on common multi-monitor arrangements even though Settings permits negative coordinates. The default `-1` sentinel ([Settings.h](../src/Settings.h):141) is indistinguishable from a legitimate coordinate. Use an explicit placement-valid flag or optional placement record, then validate/clamp the actual saved rectangle against current monitors. Preserve current DPI scaling and caption-visibility behavior. No live monitor rearrangement was performed for this review.

Native taskbar/transport controls, display-power requests, Shell thumbnails, per-monitor DPI handling, and focus restoration already have dedicated implementations. `RegisterFileAssociations` deliberately returns false under documented strict portable containment ([AppFileOps.cpp](../src/AppFileOps.cpp):204–208); it should not be reported as an accidental broken registry feature.

## 10. UI, UX & Accessibility

### F09 — The implemented UI Automation provider is unreachable through WM_GETOBJECT

**P2 · Confirmed dispatch defect; assistive-technology behavior not interactively tested.** [App.cpp](../src/App.cpp), WM_GETOBJECT, lines 1655–1661, forwards only `OBJID_CLIENT`. [Accessibility.cpp](../src/Accessibility.cpp), `AccessibilityTree::HandleGetObject`, lines 514–520, contains a separate `UiaRootObjectId` path calling `UiaReturnRawElementProvider`. That path cannot be reached from the application handler.

Microsoft documents [OBJID_CLIENT as the MSAA request and UiaRootObjectId as the UIA-provider request](https://learn.microsoft.com/en-us/windows/win32/winauto/wm-getobject). MSAA and the system bridge can still provide some accessibility; the finding is not that the entire app is inaccessible. The custom rich UIA provider and its patterns are not exposed through their intended native entry point.

Forward both object IDs, refresh the tree, and verify custom Value/RangeValue/Selection/Scroll patterns with an actual UIA client. The provider already opts into COM threading at Accessibility.cpp:822/1109, so the presence of callbacks directly mutating App state is not sufficient evidence of a cross-thread race by itself.

### F12 — Online subtitle work blocks the UI; the native updater fallback does too

**P2 · Confirmed in code; no external requests made.** [AppMenus.cpp](../src/AppMenus.cpp), `OpenOnlineSubtitlesContextMenu`, line 1236, performs synchronous Search while building a context menu. Its command callback synchronously downloads at 1252, then recursively reopens the menu at 1263, performing another search. [OnlineSubtitles.cpp](../src/OnlineSubtitles.cpp):358/395 sets five-/ten-second transport timeouts, not a worker-based UI completion path.

Menu opening and selection can freeze input, repaint, accessibility interaction, and close for network delays. mpv video can continue independently. Return the existing loading/error UI immediately, perform retrieval on a worker, and apply results with a media generation token. Reuse search results when reopening the menu after download. Preserve the opt-in and current menu presentation.

[AppModals.cpp](../src/AppModals.cpp), `CheckForUpdates`, lines 526–539, normally launches the packaged external PowerShell updater and returns. Only when that script is absent does the native fallback synchronously check at 544 and download at 576; download timeout configuration is 60 seconds. Deferred auto-checking after first paint improves the first-frame metric but does not make this fallback asynchronous. Scope any worker fix to that real fallback path; normal script-based updates are not established as UI-blocking.

## 11. Security, Input Validation & Hardening

### F02 — Private playback writes persistent history despite skipping other playback persistence

**P1 · Confirmed application path.** [AppPlayback.cpp](../src/AppPlayback.cpp), `OpenAndPlayMedia`, lines 656–668, guards record creation and watched promotion with private mode, but line 784 unconditionally calls `library.AddToHistory`. [MediaLibrary.cpp](../src/MediaLibrary.cpp):1257–1282 writes the media path, position, duration, and watched timestamp and immediately saves. `TogglePrivateMode` ([AppLibrary.cpp](../src/AppLibrary.cpp):458–464) configures session and thumbnail components, not this history call.

`PlaybackSession::Save` correctly skips private sessions at [PlaybackSession.cpp](../src/PlaybackSession.cpp):152, but cannot prevent a separate library history save. A private open can therefore persist history, including for a file not previously in the library. No real private playback was opened during the audit; the missing guard and unconditional durable call are directly established.

Guard history recording and route every playback-derived durable mutation through one explicit privacy policy. A narrow end-to-end isolated-profile test should assert no changes to history/library/session/cache files when privately opening both a new and an existing file. Ordinary intentional settings changes need not be blocked by a blanket “never save anything” switch.

### F04 — Malformed smart-playlist values can escape as exceptions or invalid numeric operations

**P1 · Exceptions reproduced; application consequence inferred from unguarded call chain.** [MediaLibrary.cpp](../src/MediaLibrary.cpp), `ParseSecondsValue`, lines 2364–2384, catches only its final bare-number conversion; colon and unit branches call `std::stod` outside that catch. `ParseTimestampValue`, lines 2409–2422, similarly leaves relative-day conversions unguarded. Probes with `badm` and `bad days` threw `std::invalid_argument` from real rule evaluation.

The editor accepts rule strings without typed validation ([AppModals.cpp](../src/AppModals.cpp):428–435), and smart playlist evaluation at MediaLibrary.cpp:1663/1653 does not catch these errors. A malformed stored or edited rule can escape through a UI render/query path. An end-to-end process termination was not induced.

`ParseResolutionValue` also calls `s.back()` on potentially empty input at 2432. Byte/time conversions accept nonfinite, negative, or out-of-range numeric values before integer casts in nearby helpers. These additional edge cases were identified statically, not executed as undefined-behavior tests.

Parse and validate rules once when editing/loading, using typed values and checked finite/range conversions. Return a localized validation error without replacing valid criteria. Keep a defensive evaluation failure boundary so a corrupt persisted rule cannot bring down unrelated playback/UI. This also removes repeated string-to-number parsing for every item.

### F24 — Configured HTTP redirect limits and final-host visibility are not implemented

**P3 · Confirmed policy omission; no exploit demonstrated.** [HttpTransport.h](../src/HttpTransport.h):22 exposes `maxRedirects=3`, but [HttpTransport.cpp](../src/HttpTransport.cpp) never applies it. It configures HTTPS-to-HTTP redirect rejection at 70–71, while `response.finalUrl` remains the original request URL set at 13. [Updater.cpp](../src/Updater.cpp):835–842 checks allowed hosts for the declared download URL, with no final redirected-host validation.

Configured redirect-count policy is therefore ineffective, and callers cannot inspect the actual final host. If cross-host redirects are intentionally required, their policy needs to be explicit rather than appearing enforced by the original-host allowlist. Apply the limit, retrieve/validate redirect destinations where required, and keep the HTTPS downgrade restriction. Package hashing, signed manifest verification, and Authenticode checks remain significant protections; this finding does not establish code execution or a signature bypass.

Security work already present deserves preservation: bounded JSON input/depth/container/string sizes ([Json.h](../src/Json.h):21–24), bounded IPC envelopes, URL scheme/control-character validation, package-local DLL loading with constrained search flags ([DllLoader.h](../src/DllLoader.h)), custom-mpv-config restrictions, and compiler/linker hardening ([CMakeLists.txt](../CMakeLists.txt):321 onward). The missing rule validation and privacy boundary are higher-value fixes than adding generic hardening checklists. This review is not an exhaustive fuzzing or dependency vulnerability audit.

## 12. Dead Code, Technical Debt & Optimization Opportunities

### F26 — Seekbar thumbnail production machinery is implemented and tested but not wired into App

**P3 · Confirmed reference audit; intended product behavior needs a decision before removal.** `RequestSeekbarThumbnail` exists at [MpvPlayer.cpp](../src/MpvPlayer.cpp):2300 and in the header, but no App source calls it. `HasSeekbarThumbnail` has no production App consumer. App calls `UpdateFrame` only in its hub branch at [App.cpp](../src/App.cpp):1363; playback does not pump this software preview path. App resets the seekbar state on transitions but never requests an active seekbar frame.

The supporting request/seek/cache/extraction code is therefore unreachable from the current production UI, despite [TestSeekbarThumbnailPreview.h](../tests/unit/TestSeekbarThumbnailPreview.h) exercising component behavior. Preview initialization still starts a disk-save worker at [MpvPlayer.cpp](../src/MpvPlayer.cpp):2177 and creates a second seekbar bitmap at 2181–2182. In the reachable hover-only path, the preview disk-save queue is used by the unwired seekbar extraction branch, leaving that worker idle.

First decide whether seekbar previews are a promised feature to wire in or dormant code to retire. Independently, start its save worker and allocate its bitmap only when a real seekbar request needs them. Preserve hub hover previews. The observation supports a concrete idle-thread/resource saving; it is not permission to remove visible functionality merely because a component appears old.

### F30 — DLL binding and mpv setup policies are duplicated across three player classes

**P3 · Opportunity grounded in implementation.** [MpvPlayer.cpp](../src/MpvPlayer.cpp), main initialization at 178, preview initialization at 2094, and hub extraction initialization around 2523 each manage DLL references, symbol lookup, handle creation, options, and cleanup through parallel code paths. Different rendering/decoder policies and handles are justified; repeating low-level ownership and binding rules increases the chance of inconsistent failure handling and option checking (F29).

Extract a small typed DLL/API owner and a checked option-application helper while retaining independent main/preview/extractor handles and their distinct tuning. Only pursue this after correctness fixes or when changing libmpv integration; a broad player rewrite has no measured benefit here.

The unit driver [run_tests.ps1](../tests/unit/run_tests.ps1):45–72 builds components but not App orchestration. That explains why good component coverage does not establish correct private-mode history, volume sequencing, WM_GETOBJECT routing, or borrowed-pointer handling. Add a few integration tests at these boundaries instead of duplicating implementation-shaped unit tests. Existing local workflow and release regression gates should stay local under AGENTS.md; no Actions workflow is warranted.

## Recommended work sequence and focused validation

1. Fix F01–F05 first. Reuse isolated profiles to check offline roots preserve records/playlists, private opens do not persist history, newly opened media keeps its own preferences/resume, invalid rule values fail safely, and structurally invalid stores recover or stay protected.
2. Fix normalized volume and its roundtrip, then error categorization and zero-duration backfill. These are small changes with visible correctness/resource benefits.
3. Put per-store dirty detection before serialization. Compare the same save benchmark once before/after and record an actual UI-thread duration with a large profile; verify failure handling and final progress durability. Keep the capped-query improvements already measured.
4. Repair UIA dispatch and negative-coordinate restore; use one UIA client session and representative left/above-primary placement cases. Make subtitle retrieval asynchronous with stale-result rejection.
5. Address MF transition accounting, device-generation invalidation, and bounded shutdown with focused failure injection. Measure overlay cadence/CPU before choosing a rendering migration.
6. Clean up unused seekbar resources and duplicate API ownership only after verifying intended functionality. Any production source fixes should be separate descriptive commits; a future release should use the existing local version/build/test/publish scripts.

Acceptance should preserve existing playback formats, queues, per-file preferences, progress/bookmarks, portable data compatibility, keyboard/pointer/touch behavior, transparency, themes, and layout. Most high-priority fixes are model, persistence, dispatch, or lifetime changes and need no visual redesign. Avoid a full codec matrix or repeated end-to-end runs for every small fix; reserve targeted codec/hardware tests for changes to decoder/rendering policy.
