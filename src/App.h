#pragma once

#include "Common.h"
#include "Settings.h"
#include "MediaLibrary.h"
#include "ImportPlaybackIntent.h"
#include "Subtitles.h"
#include "FolderWatcher.h"
#include "WatchReconcile.h"
#include "FolderRediscovery.h"
#include "MpvPlayer.h"
#include "Renderer.h"
#include "UIComponents.h"
#include "TextInputModel.h"
#include "Accessibility.h"
#include "SystemVisualPreferences.h"
#include "TooltipManager.h"
#include "PlaybackSession.h"
#include "PlaybackFlyoutLayout.h"
#include "CommandDispatcher.h"
#include "MediaTransport.h"
#include "Error.h"
#include "Diagnostics.h"
#include "GlobalSearch.h"
#include "TaskbarController.h"
#include "NetworkStream.h"
#include "PlaylistIO.h"
#include "LibraryQuery.h"
#include "LibrarySnapshot.h"
#include "LibrarySaveWorker.h"
#include "MetadataBackfillCursor.h"
#include "OnlineSubtitleJobs.h"
#include "InternetRadio.h"
#include "Podcasts.h"
#include "AsyncOwnedJob.h"
#include "BackgroundPathAvailability.h"
#include "IpcProtocol.h"
#include "Updater.h"
#include "TouchInput.h"
#include "SettingsLayout.h"
#include "ChapterMarkerCache.h"
#include "FeatureGate.h"
#include <memory>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Aperture {
class App : public ICommandContext {
    friend class ApertureDropTarget;
    friend struct PlaybackUiTestAccess;
public:
    App();
    ~App();
    // startupSettings (already loaded in wWinMain) can be adopted to avoid a second settings.json read/parse during Initialize.
    bool Initialize(HWND hWnd, HWND hVideoWnd, HINSTANCE hInstance, const PlaybackCommandLineOptions& startupOptions, const Settings* startupSettings = nullptr);
    void Shutdown();
    void BeginClose();
    bool PollClose();
    bool IsClosing() const { return closeRequested || shutdownComplete; }
    bool IsLibraryReady() const { return libraryReady; }
    bool IsLibraryLoadComplete() const { return libraryReady || libraryLoadFailed; }
    bool HasRenderedLibrarySnapshot() const { return librarySnapshotFrameRendered; }
    void ExecuteCommandLineOptions(const PlaybackCommandLineOptions& options);
    bool ExecuteAutomationCommand(const Ipc::AutomationCommand& cmd, Ipc::AutomationStatus* outStatus = nullptr);
    // Message Handlers
    LRESULT HandleMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleOverlayMessage(HWND hOverlay, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleVideoMessage(HWND hVideo, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void SyncOverlayPosition();
    bool HandleGlobalKeyDown(WPARAM wParam, LPARAM lParam);
    bool HandleModalKeyDown(WPARAM wParam);
    void Render();
    bool Update(float dt);
    bool UpdateHoverStates(float mouseX, float mouseY);
    void OnMouseLeave();
    // Bootstrap shell: pre-Initialize theme for GDI first pixels,
    // and GPU-present state for the shell→GPU handoff and milestone timing.
    void SetBootstrapColors(COLORREF bg, COLORREF text, COLORREF subtext);
    bool HasPresentedFrame() const;
    HANDLE GetFrameLatencyWaitableObject() const { return playbackSession.IsOpen() ? overlayRenderer.GetFrameLatencyWaitableObject() : renderer.GetFrameLatencyWaitableObject(); }
    bool HasPendingPlaybackFrame() const { return playbackSession.IsOpen() && overlayRenderer.HasPendingFrame(); }
    DWORD GetPlaybackFrameWaitMs() const { return overlayRenderer.GetFrameWaitMs(); }
    bool RequiresSubtitleTick() const { return playbackSession.IsOpen() && player.IsPlaying() && subtitles.GetActiveTrack(); }
    bool IsOccluded() const { return playbackSession.IsOpen() ? IsIconic(hWnd) != FALSE : renderer.IsOccluded(); }
    // ICommandContext Implementation
    Settings& GetSettings() override { return settings; }
    MpvPlayer& GetPlayer() override { return player; }
    PlaybackSession& GetPlaybackSession() override { return playbackSession; }
    MediaLibrary& GetLibrary() override { return library; }
    void ShowOSD(IconType icon, const std::wstring& text, float durationSec = 1.5f) override;
    void TriggerDoubleTapFeedback(bool isForward, int deltaSeconds = 5) override;
    void TriggerPlayPauseBadge(bool isPlay) override;
    bool SaveSettings() override;
    bool SaveLibrary() override;
    bool SaveSession();
    void OpenAndPlayMedia(const std::wstring& filePath, double startPosSec = 0.0, bool preserveQueue = false) override;
    void ClosePlayer() override;
    void PlayFirstInQueue() override;
    void PlayNextInQueue() override;
    void PlayPrevInQueue() override;
    void PlayNextLibraryMedia() override;
    void PersistCurrentPlaybackProgress();
    void PlayPlaylist(const std::string& playlistId, size_t startIndex = 0);
    void ToggleFullscreen() override;
    void TogglePiP() override;
    void OpenFileDialog() override;
    void OpenFolderDialog();
    void OpenSubtitleFileDialog() override;
    void OpenOnlineSubtitlesDialog() override;
    void OpenInExplorer(const std::wstring& filePath) override;
    bool TakeScreenshot() override;
    void DismissPopups() override;
    bool ArePopupsOpen() const override;
    void RequestRender() override;
    void TogglePlaybackDiagnostics() override;
    bool IsPlaybackDiagnosticsVisible() const override;
    void TogglePrivateMode() override;
    void ToggleAlwaysOnTop() override;
    void ShowJumpToTimeModal() override;
    double GetSubSyncAudioPoint() override { return subSyncAudioPointSec; }
    void SetSubSyncAudioPoint(double sec) override { subSyncAudioPointSec = sec; }
    void ShowCustomAspectModal();
    void ShowCustomRotationModal();
    bool IsPrivateMode() const { return playbackSession.IsPrivateMode(); }
    // State Queries
    bool IsFullscreen() const { return playbackSession.IsFullscreen(); }
    bool IsPiP() const { return playbackSession.IsPiP(); }
    // File Operations
    void HandleDroppedFiles(HDROP hDrop);
    // Visual Capture & Automation
    bool CaptureClientContent(const std::wstring& outputPath);
    void PrepareVisualCaptureScene(const std::wstring& scene);
    void SetTab(TabView tab);
    void SetSettingsCategory(SettingsCategory cat);
    // Snapshot & View-Model Derivation
    PlayerRenderViewModel BuildPlayerRenderViewModel() const;
    HubRenderViewModel BuildHubRenderViewModel();
    ModalViewModel BuildModalViewModel() const;
private:
    bool RenderHubFrame();
    HWND hWnd = NULL;
    HINSTANCE hInstance = NULL;
    Settings settings;
    MediaLibrary library;
    BackgroundPathAvailability pathAvailability;
    LibrarySaveWorker librarySaveWorker{WriteLibrarySaveSnapshot, true};
    LibrarySaveCaptureSchedule librarySaveCaptureSchedule;
    std::shared_ptr<const MediaLibrarySaveSnapshot> submittedLibrarySnapshot, libraryReloadBaseline;
    uint64_t closeSaveSerial = 0;
    bool librarySaveRequested = false;
    std::wstring playbackSavePath;
    MediaLibrarySaveResult QueueLibrarySave();
    void QueuePlaybackProgressSave();
    MediaLibrarySaveResult SubmitPendingLibrarySave(bool force = true);
    bool PumpLibrarySaveResults();
    bool ReportLibrarySaveResult(const MediaLibrarySaveResult& result);
    OnlineSubtitleJobs onlineSubtitleJobs;
    RadioDirectoryJobs radioJobs;
    PodcastJobs podcastJobs;
    PodcastSubscriptions podcastSubscriptions;
    AsyncOwnedJob<UpdateCheckResult> updateCheckJob;
    AsyncOwnedJob<UpdateDownloadResult> updateDownloadJob;
    bool updateCheckManual = true, updateCheckPersistState = false, updateJobsPrivateMode = false;
    std::string updateJobChannel, updateJobUrl;
    bool PumpOnlineSubtitleJobs();
    bool PumpRadioDirectoryJobs();
    void OpenInternetRadioContextMenu();
    bool PumpPodcastJobs();
    void OpenPodcastsContextMenu();
    void OpenPodcastEpisodesContextMenu(size_t feedIndex);
    void ShowAddPodcastModal();
    bool PumpUpdateJobs();
    void CancelNetworkJobs();
    bool AreNetworkJobsRunning() const;
    SubtitleEngine subtitles;
    MpvPlayer player;
    MpvPreviewPlayer previewPlayer;
    Renderer renderer;
    OverlayRenderer overlayRenderer;
    TooltipManager hubTooltips;
    TooltipManager playerTooltips;
    AccessibilityTree accessibilityTree;
    SystemVisualPreferences systemVisualPreferences;
    PlaybackSession playbackSession;
    CommandDispatcher commandDispatcher;
    MediaTransport mediaTransport;
    TaskbarController taskbarController;
    // System tray state (Settings > Playback rows 10-11). trayIconAdded tracks
    // whether NIM_ADD succeeded; isMinimizedToTray marks a window hidden after WM_SIZE minimize.
    NOTIFYICONDATAW trayIconData{};
    bool trayIconAdded = false, isMinimizedToTray = false;
    HICON hTrayIcon = nullptr;
    // Success count from the last ApplyGlobalHotkeys pass, used by the toggle OSD.
    int registeredGlobalHotkeys = 0;
    NetworkStreamModel networkStreamModel;
    HWND hOverlayWnd = NULL;
    HWND hVideoWnd = NULL;
    // Wallpaper mode state: the child's pre-reparent owner and its placement in
    // that parent's client coordinates (captured before SetParent).
    bool wallpaperModeActive = false;
    HWND wallpaperOriginalParent = NULL;
    RECT wallpaperOriginalChildRect = { 0, 0, 0, 0 };
    // WM_CLOSE and the destructor can both reach cleanup. Keep resource teardown idempotent and only balance COM/OLE initialization once.
    bool shutdownComplete = false, closeRequested = false;
    // Media Foundation is reference-counted and lazy (see MediaFoundation):
    // acquired for import/metadata work, never for an idle hub. No App-owned
    // flag is needed; Shutdown balances via ShutdownIfInitialized after
    // workers have joined. Set before teardown begins. MpvPlayer callbacks may be running on its event thread, so this atomic is the
    // callback-side lifetime fence.
    std::atomic<bool> playerCallbacksDisabled{ false };
    bool oleInitialized = false, appInitialized = false;
    // libmpv is intentionally lazy: the hub can render and respond without loading the main playback handle or the preview render context.
    bool playerInitAttempted = false, playerInitialized = false, previewInitAttempted = false, previewInitialized = false;
    bool startupPlayerPrepared = false;
    // Preview initialization is driven by hover intent; there is no idle
    // prewarm state to maintain alongside the lazy initialization flags.
    // Dedicated mpv instances that extract hub thumbnails for files the Windows shell cannot thumbnail. Run parallel to hover previews.
    // Two handles double worst-case fill throughput for MKV-heavy folders.
    MpvHubExtractor hubExtractors[2];
    // Position bucket (0 = auto) of the request each extractor is serving,
    // replayed into InjectThumbnail when its frame lands.
    uint32_t hubExtractorBucket[2] = {};
    // Throttled snapshot of on-screen file paths feeding extraction priority and overscan prefetch.
    std::vector<std::wstring> cachedVisibleVideoPaths;
    uint64_t lastVisibleCollectMs = 0;
    uint64_t lastPrefetchSig = 0;
    // The overlay surface (DC target, DIB, overlay fonts) is equally lazy and is only built when the player is first opened.
    bool overlayReady = false;
    // Tracks SIZE_MINIMIZED for background power throttling.
    bool isMinimized = false;
    bool inPresentationTransition = false;
    bool isUserSizingMoving = false;
    uint64_t mediaGenerationToken = 0;
    struct PendingPlaybackOpen { std::wstring path; double position; bool preserveQueue; };
    std::optional<PendingPlaybackOpen> pendingPlaybackOpen;
    // Theme-colored brush used for the pre-first-frame erase and the GDI bootstrap shell. Same color as the hub canvas so the GDI→GPU
    // handoff has no visible jump.
    HBRUSH hStartupBgBrush = nullptr;
    // Set once when the GDI bootstrap shell first paints (in-memory
    // milestone only; flushed to diagnostics/perf/ on benchmark probes).
    bool bootstrapShellPainted = false;
    // Pre-Initialize theme for the bootstrap shell (from wWinMain's startupSettings so GDI shell and GPU frame share exact colors).
    bool bootstrapColorsSet = false;
    COLORREF bootstrapBg = RGB(20, 14, 10);
    COLORREF bootstrapText = RGB(242, 244, 248);
    COLORREF bootstrapSubtext = RGB(160, 170, 190);
    // GDI-only first pixels: theme background + app title/loading hint. Uses USER32/GDI32 only so it never waits on D3D/DWrite/MF/COM.
    void PaintBootstrapShell(HDC hdc, const RECT& rc);
    // Deferred cold-path services: created on first real use, destroyed symmetrically in Shutdown (safe when never created).
    bool EnsureHubTooltips();
    bool EnsurePlayerTooltips();
    // The initial portable library is built on one worker and published as a generation-tagged move-only package. Only the UI thread
    // mutates the live MediaLibrary after adoption.
    std::thread libraryLoadWorker;
    std::atomic<bool> libraryLoadCancelRequested{ false }, libraryLoadWorkerDone{ true };
    LibrarySnapshotMailbox librarySnapshotMailbox;
    bool libraryLoadInProgress = false, libraryReady = false, libraryLoadFailed = false, librarySnapshotFrameRendered = false;
    // The startup update check performs a synchronous network request; it is
    // deferred until the first GPU shell has been presented (see Update()).
    bool startupUpdateCheckPending = false;
    bool firstGpuShellRecorded = false, deferredLibraryReadyWorkPending = false;
    StorageStatus libraryLoadStatus = StorageStatus::Ok();
    static constexpr size_t kMaxDeferredLibraryCommands = 64;
    std::deque<PlaybackCommandLineOptions> deferredLibraryCommands;
    void StartLibraryLoad(LibraryGroupMode groupMode, bool preserveExisting = false);
    void FinishInitialLibraryLoad(uint64_t generation);
    void CancelInitialLibraryLoad();
    void RunDeferredLibraryReadyWork();
    static constexpr UINT kAccessibilityActionMessage = WM_APP + 0x4A, kPlayerEventMessage = WM_APP + 0x4B;
    static constexpr UINT kFolderWatchEventMessage = WM_APP + 0x4C, kBackfillEventMessage = WM_APP + 0x4D;
    static constexpr UINT kAccessibilityAnnouncementMessage = WM_APP + 0x4F, kLibrarySnapshotReadyMessage = WM_APP + 0x51;
    // Free offset: 0x4A-0x51 are taken (App.h/MediaTransport.h/Renderer.h); 0x52 is unused.
    static constexpr UINT kTrayCallbackMessage = WM_APP + 0x52;
    static constexpr UINT kGlobalHotkeyBase = 0x9000;
    static constexpr UINT kGlobalHotkeyCount = 4;
    // Fixed action set driven by their CURRENT bound chords in settings.shortcuts.
    static const char* const kGlobalHotkeyActions[kGlobalHotkeyCount];
    static constexpr LONG kStatusLiveRegionId = 9999;
    enum PlayerEventFlags : uint32_t {
        PlayerEventNone = 0, PlayerEventEnded = 1u << 0, PlayerEventTime = 1u << 1, PlayerEventDuration = 1u << 2,
        PlayerEventError = 1u << 3, PlayerEventPlayState = 1u << 4, PlayerEventTracks = 1u << 5, PlayerEventChapters = 1u << 6,
        PlayerEventWarning = 1u << 7, };
    std::atomic<uint32_t> pendingPlayerEvents{ PlayerEventNone };
    std::atomic<double> pendingPlayerTime{ 0.0 };
    // time-pos fires up to frame rate; the timeline reads GetCurrentTime()
    // per frame directly, so queued event work (progress push, chapter
    // announce, SMTC refresh) is pulsed at ~4 Hz instead.
    std::atomic<double> lastQueuedPlayerTime{ -1.0e9 };
    std::mutex pendingPlayerErrorMutex;
    std::deque<AppError> pendingPlayerErrors;
    std::deque<AppError> pendingPlayerWarnings;
    std::atomic<bool> playerEventPosted{ false };
    bool accessibilityDirty = true;
    ULONGLONG accessibilityLastRefreshMs = 0;
    std::unordered_map<LONG, std::function<void()>> accessibilityActions;
    DWORD mainThreadId = 0;
    std::wstring currentLiveAnnouncement, lastAnnouncedText;
    ULONGLONG lastAnnouncementTimeMs = 0;
    std::wstring pendingCoalescedAnnouncement;
    ULONGLONG lastVolumeOrSeekAnnounceMs = 0;
    std::mutex pendingAnnouncementMutex;
    std::vector<std::wstring> pendingAnnouncements;
    bool lastAnnouncedPlayingState = false, hasAnnouncedPlayingState = false;
    int lastAnnouncedChapterIndex = -1;
    unsigned long long hubTooltipSignature = 0;
    // Descriptions hidden by the compact Settings layout, captured from the
    // last rendered frame so tooltips can surface them on hover.
    std::vector<SettingsDescTool> settingsDescTools;
    unsigned long long playerTooltipSignature = 0;
    bool displayPowerRequestActive = false;
    std::wstring lastReportedStorageError;
    ULONGLONG lastStorageErrorReportTimeMs = 0;
    // Import work runs against an isolated MediaLibrary snapshot. The UI thread joins the worker before merging accepted items into the
    // live library, so no background thread ever touches App::library.
    std::thread importWorker;
    std::atomic<bool> importCancelRequested{ false };
    std::atomic<bool> importDone{ false };
    std::atomic<size_t> importProcessed{ 0 };
    std::atomic<size_t> importTotal{ 0 };
    bool importJobActive = false, importLaunchFirst = false, importCancelHovered = false;
    bool importPopoverOpen = false, importPillHovered = false;
    // Listen panel state (podcasts + radio directory mirrors, UI-thread only).
    std::vector<PodcastFeed> listenFeeds;
    std::vector<RadioStation> listenStations;
    int listenExpandedFeed = -1;
    float listenScrollY = 0.0f, listenContentHeight = 0.0f, listenViewportHeight = 0.0f;
    int listenHoveredFeed = -1, listenHoveredEpisodeFeed = -1, listenHoveredEpisode = -1, listenHoveredStation = -1;
    bool listenAddPodcastHovered = false;
    bool listenJobsKicked = false;
    std::wstring listenRadioErrorCached;
    // Island morph: width/height spring between a 6 DIP dot and the content
    // pill; the dot fades out only after the collapse finishes.
    AnimatedFloat importIslandWidthDip, importIslandHeightDip, importIslandFade;
    float importSpinAngle = 0.0f;
    ImportPlaybackIntent importFirstPlayback;
    std::wstring importEarlyPlaybackPath;
    uint64_t playbackRequestGeneration = 0, importPlaybackGeneration = 0, importStartPlaybackGeneration = 0;
    std::string importTargetPlaylistId;
    std::wstring importCreatedPlaylistName;
    MediaImportResult importResult;
    std::vector<MediaItem> stagedImportItems;
    std::vector<std::optional<MetadataBackfillBaseline>> stagedImportBaselines;
    // Metadata backfill work runs against an isolated MediaLibrary snapshot. The UI thread joins the worker before merging updated metadata
    // into the live library, so no background thread ever touches App::library.
    std::thread backfillWorker;
    std::atomic<bool> backfillCancelRequested{ false };
    std::atomic<bool> backfillDone{ false };
    std::atomic<size_t> backfillProcessed{ 0 };
    std::atomic<size_t> backfillTotal{ 0 };
    bool backfillJobActive = false;
    MetadataBackfillResult backfillResult;
    std::vector<MediaItem> stagedBackfillItems;
    std::vector<MetadataBackfillBaseline> backfillBaseline;
    MetadataBackfillCursor backfillCursor;
    MetadataBackfillOptions backfillOptions;
    bool backfillPassDirty = false;
    ULONGLONG backfillLastSaveTick = 0;
    WatchReconcileWorker watchReconcileWorker;
    AsyncOwnedJob<PreparedFolderRediscovery> folderRediscoveryJob;
    bool folderRediscoveryActive = false, folderRediscoveryFeedback = false, folderRediscoveryAgain = false;
    FolderWatcher folderWatcher;
    ULONGLONG watchRetryAfterTick = 0;
    bool watchRescanFeedbackPending = false;
    std::atomic<bool> watchEventPosted{ false };
    void HandleFolderWatchEvents();
    void ResolvePlaylistHover(float x, float y);
    void RestorePlaylistItemsToLibrary();
    // Listen panel (Media tab, Listen mode): hover resolution, stream playback
    // (validation + recent-stream bookkeeping + open), and right-click menus.
    void ResolveListenHover(float x, float y);
    void PlayListenStream(const std::wstring& url);
    void OpenListenFeedContextMenu(size_t feedIndex, float x, float y);
    void OpenListenStationContextMenu(size_t stationIndex, float x, float y);
    struct ListenPress {
        int target = 0; // 0 none, 1 feed, 2 episode, 3 station, 4 add-podcast
        int a = -1, b = -1;
    } listenPress;
    // Import status island: pill in the History<->Settings nav gap; the
    // popover anchors below it. Single geometry source for draw + hit-tests.
    D2D1_RECT_F ImportPillBounds(float dp);
    float ImportIslandTargetWidthDip();
    D2D1_RECT_F ImportPopoverBounds(float dp);
    D2D1_RECT_F ImportCancelBounds(float dp);
    D2D1_RECT_F ContextMenuBounds(float dp) const;
    void OpenAddToPlaylistContextMenu(float x, float y, std::vector<std::wstring> paths);
    // A modal file dialog pumps messages on the UI thread; these guards keep
    // nested invocations (watch events, a second dialog) off that stack.
    bool handlingFolderWatchEvents = false;
    bool systemDialogActive = false;
    void AddWatchedFolderPrompt();
    void RescanAllWatchedFolders();
    void StartImportJob(const std::vector<std::wstring>& paths, const std::string& targetPlaylistId = "", bool launchFirst = false,
                        const std::wstring& createdPlaylistName = L"");
    // Folder drops on the playlist rail create one playlist per folder; their imports run sequentially through this queue because the
    // import pipeline supports a single active job.
    struct PendingDropImport { std::vector<std::wstring> paths; std::string targetPlaylistId; std::wstring createdPlaylistName; };
    std::deque<PendingDropImport> pendingDropImports;
    void PumpPendingDropImports();
    void FinishImportJob();
    void PumpImportFirstPlayback();
    void CancelImportJob();
    void ScheduleMetadataBackfill(const MetadataBackfillOptions& options = {});
    void StartNextMetadataBackfillBatch();
    void FinishBackfillJob();
    void CancelBackfillJob();
    bool EnsurePlayerInitialized();
    bool EnsurePreviewInitialized();
    // Representative start position for hub fallback extraction (resume pos else 10% capped at 10s, mirroring the hover preview rule).
    double HubExtractStartSecForPath(const std::wstring& path);
    // Dedicated hub extractor scheduling (visible-first, cancel-stale).
    static bool IsHubExtractVideoPath(const std::wstring& path);
    void CollectVisibleVideoPaths();
    bool HubExtractPathStillWanted(const std::wstring& path);
    bool DriveHubExtraction();
    void MaybePrefetchVisibleThumbnails();
    void QueuePlayerEvent(uint32_t flags);
    void HandlePlayerEvents();
    void ClearPendingPlayerEvents();
    void ArmPlayerEventMessage();
    void ApplyPlayerPreferences();
    void ApplyAlwaysOnTop();
    // Desktop wallpaper mode: the video child is temporarily reparented onto
    // the shell's WorkerW so mpv renders behind the desktop icons. mpv's wid
    // stays bound to the same HWND, so SetParent alone is safe.
    void SetWallpaperMode(bool enabled);
    // Silent force-disable used by the close/PiP/fullscreen entry paths; the
    // video child must never remain owned by the shell.
    void DisableWallpaperMode();
    void ApplyExperienceModeSelection(ExperienceMode mode, bool forkFromPreview = false);
    void ResolveSettingsHover(float x, float y);
    void ToggleExperienceFeature(const std::string& key);
    // System-wide hotkeys (Settings > Playback, row 9). Unregisters all four
    // fixed actions then registers the CURRENT bound chord of each action from
    // settings.shortcuts. Bindings are skipped when vk==0 (unassigned) or when
    // no modifier is held: a bare global key would swallow system-wide typing.
    int ApplyGlobalHotkeys();
    // Tray icon + minimize-to-tray (Settings > Playback, rows 10-11).
    void UpdateTrayIcon();
    void RemoveTrayIcon();
    void RestoreFromTray();
    // Jump list (recent media) publication. History writes only mark a dirty
    // flag; Update() pumps it rate-limited and BeginClose publishes at once.
    void MarkJumpListDirty();
    void PumpJumpListUpdate(bool force = false);
    static std::vector<std::wstring> RecentMediaPathsForJumpList(const std::vector<HistoryRecord>& history, size_t maxCount);
    bool jumpListDirty = false, jumpListSeeded = false;
    uint64_t lastJumpListPublishMs = 0;
    static constexpr uint32_t kJumpListRebuildMinIntervalMs = 30000;
    void ShowTrayContextMenu(int screenX, int screenY);
    // In-app viewer for the last lines of the diagnostics log (player context).
    void ToggleLogViewer() override;
    bool IsLogViewerVisible() const { return showLogViewer; }
    bool IsLogViewerCloseHit(float x, float y) const;
    std::wstring RelocateMissingMedia(const std::wstring& missingPath);
    bool UpdateMediaPathReferences(const std::wstring& oldPath, const std::wstring& newPath);
    void UpdateMediaTransport(bool force = false);
    // View States
    TabView currentTab = TabView::Media;
    TabView hoveredTab = (TabView)-1;
    SettingsCategory currentSettingsCat = SettingsCategory::Appearance;
    SettingsCategory hoveredSettingsCat = (SettingsCategory)-1;
    bool isDragActive = false;
    // Player Overlay State
    float controlsOpacity = 1.0f, controlsIdleTimer = 0.0f, lastPlayerMouseX = -1.0f, lastPlayerMouseY = -1.0f;
    // Progress-push throttle: library progress is only updated when the
    // player's position, duration, or file actually changed meaningfully.
    float progressSaveTimerSec = 0.0f;
    uint64_t hiddenLibrarySinceMs = 0;
    bool hiddenLibraryCachesTrimmed = false;
    bool isCursorHidden = false, isScrubbing = false;
    double scrubTargetTime = -1.0, lastMpvSeekTime = -1.0;
    std::chrono::steady_clock::time_point lastMpvSeekTimestamp{};
    bool isVolumeScrubbing = false, isVolumeHovered = false, isTimelineHovered = false, isBackHovered = false;
    // Ctrl+drag pan on the video surface (grab semantics; session-only state).
    bool isVideoPanDragging = false;
    float panDragLastX = 0.0f, panDragLastY = 0.0f;
    int hoveredPlayerButtonId = 0;
    LONG playerKeyboardFocusId = 0;
    bool showPlaybackDiagnostics = false;
    // In-app diagnostics log viewer (player context, cloned from the
    // playback-diagnostics overlay pattern).
    bool showLogViewer = false;
    std::vector<std::wstring> logViewerLines;
    // Settings Flyout State
    bool isSettingsFlyoutOpen = false;
    std::string currentFlyoutMenu = "main";
    int hoveredFlyoutOption = -1;
    size_t queueFlyoutFirstVisible = 0;
    TextInputModel playbackSpeedInput;
    bool isPlaybackSpeedDragging = false;
    double playbackSpeedDragValue = 1.0;
    D2D1_RECT_F PlaybackSpeedEditorBounds() const;
    bool CommitPlaybackSpeedInput();
    void UpdatePlaybackSpeedDrag(float clientX);
    bool HandlePlaybackSpeedDragMessage(UINT message, LPARAM position);
    void UpdateFlyoutHitTarget(float mouseX, float mouseY, float winW, float winH, float dp, float bottomBarH);
    void HandleFlyoutClick(float localX, float localY);
    float GetFlyoutHeight() const;
    int GetFlyoutOptionCount() const;
    // OSD State
    IconType osdIcon = IconType::VolumeHigh;
    std::wstring osdText;
    float osdTimer = 0.0f, osdDuration = 1.5f;
    struct PendingNotification { IconType icon = IconType::SettingsGear; std::wstring text; float durationSec = 1.5f; };
    std::deque<PendingNotification> notificationQueue;
    // Animations
    float doubleTapTimer = 0.0f;
    bool doubleTapIsForward = true;
    // YouTube-style accumulated seek OSD (mirrors youtube_double_seek.html): repeated presses grow the number, sign/number pop once per
    // press, a fresh chevron slides in over the standing one, and switching direction fast-fades the opposite badge.
    static constexpr float kSeekOsdHoldSec = 0.85f, kSeekOsdFadeSec = 0.22f, kSeekOsdFastFadeSec = 0.20f;
    // Per-element progress ramps inside the seek OSD: chevron slide, sign pop, number pop (lands on the standard moderate pace), badge entrance.
    static constexpr float kSeekOsdChevronSec = 0.26f, kSeekOsdSignSec = 0.24f, kSeekOsdNumberSec = MotionTokens::kModerate, kSeekOsdAppearSec = 0.16f;
    double doubleTapAccumSec = 0.0;
    int doubleTapPresses = 0;
    float doubleTapSignPop = 1.0f;    // 0 -> 1 over 0.24s
    float doubleTapNumberPop = 1.0f;  // 0 -> 1 over 0.22s
    std::vector<float> doubleTapChevronSlides; // one 0 -> 1 track per press
    float doubleTapAppear = 1.0f;     // 0 -> 1 badge entrance on first press
    bool doubleTapFadeValid = false;  // opposite-side snapshot while crossfading
    bool doubleTapFadeForward = true;
    double doubleTapFadeSeconds = 0.0;
    float doubleTapFadeProgress = 1.0f, doubleTapFadeExitStart = 0.0f, doubleTapFadeAppear = 1.0f, doubleTapFadeSignPop = 1.0f;
    float doubleTapFadeNumberPop = 1.0f;
    std::vector<float> doubleTapFadeChevronSlides;
    float playPauseAnimTimer = 0.0f;
    bool playPauseAnimIsPlay = true;
    // Jump Ahead State (Intro/Outro skip pill)
    bool isJumpAheadVisible = false;
    float jumpAheadTimer = 0.0f;
    AnimatedFloat jumpAheadAnim;
    int jumpAheadActiveChapterIndex = -1;
    double jumpAheadTargetTime = 0.0;
    bool isJumpAheadHovered = false;
    bool isJumpAheadPressed = false;
    D2D1_RECT_F jumpAheadBounds = { 0, 0, 0, 0 };
    bool IsQualifyingJumpAheadChapter(size_t chapterIndex, double& outTargetTime) const;
    double TimelineClickTarget(float clickX, float barLeft, float barWidth, double rawTime, std::wstring& outSnappedChapterTitle, bool ctrlHeld) const;
    // Scroll States
    float mediaScrollY = 0.0f, playlistRailScrollY = 0.0f, playlistDetailScrollY = 0.0f, historyScrollY = 0.0f;
    float settingsScrollY = 0.0f;
    // Media Library Grouping State
    LibraryGroupMode libraryGroupMode = LibraryGroupMode::Flat;
    LibrarySortField librarySortField = LibrarySortField::Title;
    SortDirection librarySortDirection = SortDirection::Ascending;
    int hoveredGroupBtn = -1; // -1: None, 0: Flat, 1: Folder/Series, 2: Listen
    int hoveredMediaToolbarAction = -1; // -1: None, 0: Search, 1: Sort
    D2D1_RECT_F mediaSearchBtnBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F mediaSearchClearBtnBounds = { 0, 0, 0, 0 };
    bool isMediaSearchClearHovered = false, isMediaSearchFocused = false;
    TextInputModel mediaSearchInput;
    D2D1_RECT_F mediaSortBtnBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F mediaFlatBtnBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F mediaSeriesBtnBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F mediaListenBtnBounds = { 0, 0, 0, 0 };
    // Cached Listen-panel hit rects, refreshed during view-model build.
    std::vector<D2D1_RECT_F> listenFeedBounds, listenEpisodeBounds, listenStationBounds;
    std::vector<int> listenEpisodeFeedIndex;
    D2D1_RECT_F listenAddPodcastBounds = { 0, 0, 0, 0 };
    LibraryQueryEngine m_mediaQueryEngine;
    // Borrow the compact query result; visible cards read the live records.
    const LibraryViewProjection& GetCurrentMediaProjection(bool includeCollapsed = false);
    std::shared_ptr<const std::vector<std::wstring>> m_folderOrderSnapshot;
    const std::vector<size_t>& GetVisibleMediaIndices();
    void SetLibraryGroupMode(LibraryGroupMode mode);
    void SetLibrarySort(LibrarySortField field, SortDirection direction);
    void OpenMediaSortContextMenu(float x, float y);
    std::wstring GetLibrarySortLabel(bool compact = false) const;
    void ClearMediaSearch();
    void FocusMediaSearch();
    // Middle-Button Autoscroll State
    enum class AutoscrollMode { Inactive, Pending, Holding, Toggle };
    enum class AutoscrollPane { None, Media, PlaylistRail, PlaylistDetail, History, Settings };
    struct AutoscrollState {
        AutoscrollMode mode = AutoscrollMode::Inactive;
        AutoscrollPane pane = AutoscrollPane::None;
        D2D1_POINT_2F anchorPoint = { 0.0f, 0.0f };
        D2D1_POINT_2F currentPoint = { 0.0f, 0.0f };
        float velocityDIPPerSecond = 0.0f;
        bool suppressNextClick = false;
    };
    AutoscrollState autoscroll;
    bool IsAutoscrolling() const { return autoscroll.mode != AutoscrollMode::Inactive; }
    void StartAutoscroll(AutoscrollPane pane, float mouseX, float mouseY);
    void StopAutoscroll(bool releaseCapture = true);
    void UpdateAutoscroll(float dt);
    AutoscrollPane HitTestAutoscrollPane(float mouseX, float mouseY, float& outMaxScroll);
    float GetSettingsContentHeight(float dp) const;
    float SettingsContentWidth(float dp) const;
    float GetMaxScrollForPane(AutoscrollPane pane);
    float GetMaxScrollForPaneInViewport(AutoscrollPane pane, float winW, float winH, float dp);
    float* GetScrollOffsetPtr(AutoscrollPane pane);
    // ---- Touch (WM_POINTER) gestures -----------------------------------------
    // Pure gesture math lives in TouchInput::Engine; zone semantics (what a
    // tap, double-tap, long-press, or pan means per window) live in AppTouch.cpp.
    Touch::Engine touchHub;      // main window while the hub is frontmost
    Touch::Engine touchPlayer;   // overlay + video child while playback is open
    enum class HubTouchMode { Passthrough, PanCandidate, Deferred };
    HubTouchMode hubTouchMode = HubTouchMode::Passthrough;
    bool hubTouchBlocked = false;           // modal/menu/search open at press time
    AutoscrollPane touchPanPane = AutoscrollPane::None;
    float hubTouchPanMaxScroll = 0.0f;      // captured at touch-down for the panned pane
    float touchPanLastY = 0.0f;             // last panned finger Y, DIPs
    float touchPanVelocity = 0.0f;          // inertia, DIP/s; positive scrolls down
    bool touchSpringBack = false;           // rubber-band is settling into bounds
    bool touchPressSuppressesHoverExpand = false;
    double touchHubTapSettleAt = -1.0;      // pending post-tap hover clear
    enum class PlayerTouchMode { Passthrough, VideoEdge, QueueFlyout };
    struct PlayerTouchState {
        PlayerTouchMode mode = PlayerTouchMode::Passthrough;
        bool onVideo = false;               // press started in the video area
        bool seekScrub = false;             // horizontal pan drives the timeline
        bool volumeDrag = false;            // right-side vertical pan drives volume
        float volumeStart = 0.0f;
        float downXPx = 0.0f, downYPx = 0.0f;
        float lastYPx = 0.0f;               // queue flyout row accumulator input
        float flyoutAccumPx = 0.0f;
        double deliverAt = -1.0;            // pending deferred tap delivery
        float deliverXPx = 0.0f, deliverYPx = 0.0f;
    };
    PlayerTouchState playerTouch;
    double playerHoverClearAt = -1.0;
    bool HandleHubTouchPointer(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    bool HandlePlayerTouchPointer(HWND hTarget, UINT uMsg, WPARAM wParam, LPARAM lParam);
    bool UpdateTouchGestures(float dt, double nowSec);
    void CancelTouchGestures();
    void BeginTouchInertia();
    bool TouchPressArmedHubControl() const;
    void ClearHubPressVisualState();
    void ClearPlayerHoverState();
    void DeliverTouchMouse(HWND hTarget, UINT mouseMessage, WPARAM wParam, float xPx, float yPx);
    void DeliverSyntheticTap(HWND hTarget, float xPx, float yPx);
    void NudgeTouchControlTarget(float xPx, float yPx, float winW, float winH, float dp, float& outX, float& outY);
    void ScrubTimelineToPx(float xPx, float winW, float dp);
    void ScrubTimelineBetween(float xPx, float barLeft, float barW);
    void StartTouchSeekScrub(float xPx, float winW, float dp);
    void FinishTouchSeekScrub(bool seekFinal);
    void ApplyTouchVolume(float volume);
    // Direct2D playlist toolbar input state.  The WebView keeps this in an
    // HTML input/localStorage; native keeps the active query for the session
    // and redraws the value/caret in UIComponents.
    TextInputModel playlistSearchInput;
    bool playlistGridView = false, isPlaylistSearchFocused = false;
    std::string cachedVisiblePlaylistId;
    std::wstring cachedVisibleSearchQuery;
    LibraryRevisions cachedVisibleRevisions;
    uint64_t cachedVisiblePathsGeneration = 0;
    bool cachedVisiblePlaylistIsSmart = false;
    std::vector<size_t> cachedVisiblePlaylistIndices;
    std::string cachedSearchKeyPlaylistId;
    LibraryRevisions cachedSearchKeyRevisions;
    uint64_t cachedSearchPathsGeneration = 0;
    std::vector<std::wstring> cachedPlaylistSearchKeys;
    enum class TextInputField { None, PlaylistSearch, Modal, InlineRename, GlobalSearch, MediaSearch, PlaybackSpeed };
    TextInputModel modalInput;
    TextInputModel inlineRenameInput;
    TextInputField activeTextInputField = TextInputField::None;
    bool isInlineRenaming = false;
    std::string inlineRenamePlaylistId;
    GlobalSearchModel globalSearchModel;
    int hoveredGlobalSearchIndex = -1;
    bool isGlobalSearchCloseHovered = false;
    double abLoopA = -1.0, abLoopB = -1.0;
    double subSyncAudioPointSec = -1.0; // Guided subtitle-sync mark point (-1 = unset)
    float sleepTimerRemainingSec = 0.0f;
    bool stopAfterCurrent = false, textInputCaretVisible = true, textInputDragging = false;
    bool exitAfterPlayback = false;
    std::size_t textInputDragAnchor = 0;
    bool hasPendingHighSurrogate = false;
    wchar_t pendingHighSurrogate = 0;
    // Some IMEs deliver GCS_RESULTSTR followed by an equivalent WM_CHAR sequence. Consume that echo so committed text is never duplicated.
    TextInputModel::Text pendingImeEcho;
    // Focus & Multi-Selection Tracking
    enum class FocusPane { TopNavBar, MediaList, PlaylistRail, PlaylistDetail, HistoryList, SettingsCategoryList, SettingsContent };
    FocusPane activeFocusPane = FocusPane::TopNavBar;
    int topNavFocusedIndex = 0; // 0: Media, 1: Playlists, 2: History, 3: Settings, 4: + Open File
    enum class TransientSurfaceType {
        None, Modal, ContextMenu, SettingsFlyout, GlobalSearch, ShortcutOverlay, ShortcutCapture, InlineRename, SystemDialog
    };
    struct FocusSnapshot {
        TransientSurfaceType surfaceType = TransientSurfaceType::None;
        TabView tab = TabView::Media;
        FocusPane pane = FocusPane::TopNavBar;
        int topNavIndex = 0, keyboardIndex = -1, keyboardSubIndex = 0;
        TextInputField textInputField = TextInputField::None;
        LONG accessibilityId = 0;
        std::wstring entityPath;
        std::string entityId;
        uint64_t libraryVersion = 0;
        bool playerActive = false;
        int playerButtonId = -1;
    };
    static constexpr size_t kMaxFocusStackDepth = 16;
    std::vector<FocusSnapshot> focusRestoreStack;
    void PushFocusSnapshot(TransientSurfaceType surfaceType);
    bool PopAndRestoreFocus(TransientSurfaceType expectedSurfaceType = TransientSurfaceType::None);
    void RestoreLogicalFocus(const FocusSnapshot& snapshot);
    FocusSnapshot CaptureCurrentFocus(TransientSurfaceType surfaceType) const;
    void ActivateCreatedPlaylist(const std::string& playlistId);
    void DismissActiveModal(bool confirmed = false);
    void CloseContextMenu();
    void RepositionContextMenu(float x, float y);
    float contextMenuPreferredWidth = 220.0f;
    void CloseSettingsFlyout();
    std::unordered_set<size_t> selectedMediaIndices;
    std::unordered_set<size_t> selectedPlaylistIndices;
    std::unordered_set<size_t> selectedEpisodeIndices;
    std::unordered_set<size_t> selectedHistoryIndices;
    int selectionAnchorIndex = -1, keyboardFocusedIndex = -1, keyboardFocusedSubIndex = 0;
    bool isKeyboardNavActive = false;
    // Toggle switch animations in settings
    AnimatedFloat autoplayToggleAnim;
    AnimatedFloat rememberPosToggleAnim;
    AnimatedFloat hwAccelToggleAnim;
    AnimatedFloat loopToggleAnim;
    AnimatedFloat customMpvConfigToggleAnim;
    // Experience settings rows: per-feature toggle progress toward
    // enabled ? 1 : 0 (19 catalog features, 40 slots of headroom). Targets are
    // re-derived from the experience mode every frame like the System rows.
    AnimatedFloat experienceToggleAnim[40];
    // System & Storage rows: per-row hover/press progress so feedback eases
    // in and out instead of snapping between flat states. Press itself is an
    // instant boolean (settingsPressState); the eased progress only softens
    // the release, matching the nav-bar press model.
    AnimatedFloat systemRowHoverAnim[SettingsLayout::kSystemRowCount];
    AnimatedFloat systemRowPressAnim[SettingsLayout::kSystemRowCount];
    void HandleItemSelection(FocusPane pane, size_t index, bool isShift, bool isCtrl, size_t totalCount);
    void SelectAllInCurrentPane();
    void ClearSelectionInCurrentPane();
    void DeleteSelectedInCurrentPane();
    void ConfirmDeleteSelectedMedia();
    void PruneOrphanedHubThumbnailCache();
    struct LibraryUndoEntry {
        std::wstring label;
        std::function<bool()> apply;
        std::unique_ptr<MediaLibrary> snapshot;
        size_t retainedBytes = 0;
    };
    std::deque<LibraryUndoEntry> libraryUndoHistory;
    void PushLibraryUndo(LibraryUndoEntry entry);
    void ArmLibraryUndo(const std::wstring& label);
    void ArmLibraryUndo(const std::wstring& label, std::function<bool()> edit, size_t retainedBytes = 0);
    bool UndoLastLibraryAction();
    size_t SetWatchedWithUndo(const std::vector<std::wstring>& paths, bool watched, size_t* skipped = nullptr);
    size_t AddToPlaylistWithUndo(const std::string& id, const std::vector<std::wstring>& paths, size_t* skipped = nullptr);
    void ArmCreatedPlaylistUndo(const std::string& id);
    void BulkSetWatched(bool watched);
    void BulkAddToPlaylist(const std::string& playlistId);
    void PlaySelectedInCurrentPane();
    void AutoScrollToFocusedItem();
    int hoveredMediaIndex = -1, selectedMediaIndex = -1;
    std::string activePlaylistId = "pl_favorites";
    int hoveredPlaylistRailIndex = -1, hoveredPlaylistDetailItemIndex = -1, hoveredPlaylistDetailDeleteIndex = -1, hoveredHistoryIndex = -1;
    int hoveredHistoryDeleteIndex = -1, hoveredSettingsRow = -1, hoveredShortcutIndex = -1;
    bool isResetShortcutsHovered = false;
    std::string rebindingActionId = "";
    std::string rebindingActionName = "";
    int rebindingOriginRow = 1, rebindingOriginSubIndex = 0;
    std::wstring lastAnnouncedModifierChord = L"";
    struct RowAnimationState {
        AnimatedFloat height;             // rest 76 -> expanded 160 (kPreview); pinned rows hold their rest height
        AnimatedFloat liftY;              // 0 -> 2 px lift on hover (kReveal)
        AnimatedFloat previewOpacity;     // hover preview video backdrop fade (kPreview)
        float hoverEnterTime = -1.0f;     // hover must persist kHoverRevealDelay (0.35 s) before the card expands
        bool isHovering = false, inActiveAnimList = false;
    };
    std::vector<RowAnimationState> rowAnimStates;
    AnimatedFloat navMediaTabHover;
    AnimatedFloat navPlaylistTabHover;
    AnimatedFloat navHistoryTabHover;
    AnimatedFloat navSettingsTabHover;
    AnimatedFloat navSettingsRotation;
    AnimatedFloat navOpenFileLift;
    // Sliding selection pill of the segmented nav: tab index as a float (0..2).
    AnimatedFloat navIndicator;
    bool navIndicatorReady = false;
    // Sliding selection pill of the Settings sidebar category: category index as a float (0..4).
    AnimatedFloat settingsCatIndicator;
    bool settingsCatIndicatorReady = false;
    // Sliding selection pill of the Playlist left rail: playlist index as a float.
    AnimatedFloat playlistRailIndicator;
    bool playlistRailIndicatorReady = false;
    // Sliding selection pill of the Media grouping mode: (0.0 = Standalone, 1.0 = Folders/Series).
    AnimatedFloat groupModeIndicator;
    bool groupModeIndicatorReady = false;
    // Sliding selection pill of the Playlist view mode: (0.0 = List, 1.0 = Grid).
    AnimatedFloat playlistViewModeIndicator;
    bool playlistViewModeIndicatorReady = false;
    // 0..2 = nav tabs, 3 = settings, 4 = open file; -1 = none.
    // Set on mouse-down, dispatched on mouse-up so the click has a visible press and release.
    int pressedNavIndex = -1;
    bool isNavSettingsHovered = false, isNavOpenHovered = false;
    // Idle-teardown bookkeeping for the lazy mpv consumers (plan §4.1.5).
    double hubExtractorLastBusy[2] = { 0.0, 0.0 };
    double previewLastActivity = 0.0;
    // Playlist "Continue Watching" card hover preview.
    std::wstring resumePreviewPath;
    double resumeResumePos = 0.0, resumeHoverEnter = -1.0;
    bool resumePreviewStarted = false;
    AnimatedFloat resumePreviewAnim;   // preview backdrop fade (kFast in, kResponsive out)
    // Playlist card hover expansion (1s hover intent reveals full title and hides duration)
    int playlistCardHoverIndex = -1;
    float playlistCardHoverEnterTime = -1.0f;
    static constexpr float kPlaylistCardHoverDelay = 1.0f;
    std::unordered_map<size_t, AnimatedFloat> playlistCardExpands;
    void ClearPlaylistCardHover();
    bool isPlaylistAddHovered = false, isPlaylistAddFilesHovered = false, isPlaylistPlayAllHovered = false;
    bool isPlaylistDeleteHovered = false, isPlaylistResumeHovered = false;
    bool isPlaylistDropTargetActive = false, isRailFolderDropActive = false, isClearHistoryHovered = false, isClearHistoryPressed = false;
    // Snapshot taken at OLE DragEnter: true when the CF_HDROP payload carries at least one folder, so drag-over can preview
    // "create playlist from folder" before the drop happens.
    bool oleDragPayloadHasFolder = false;
    std::vector<D2D1_RECT_F> mediaEmptyBtnBounds;
    std::vector<D2D1_RECT_F> historyEmptyBtnBounds;
    int hoveredMediaEmptyBtn = -1, pressedMediaEmptyBtn = -1, hoveredHistoryEmptyBtn = -1, pressedHistoryEmptyBtn = -1;
    SettingsPressState settingsPressState;
    // Live hub pointer position, used by draw code that resolves hover from layout it alone knows (settings rows).
    float hubMouseX = -1.0f, hubMouseY = -1.0f;
    bool subtitleSizeDragging = false, isPinnedCloseHovered = false, isLastWatchedCleared = false;
    bool isPiPVolumeSliderOpen = false, isPiPVolumeDragging = false;
    bool isPiPPotentialWindowDrag = false, isPiPDraggingWindow = false, isPiPDragOccurred = false;
    POINT pipWindowDragStartPoint = { 0, 0 };
    POINT pipWindowDragStartWindowPos = { 0, 0 };
    AnimatedFloat pipVolumeAnim;
    int hoveredMediaGroupHeader = -1, hoveredFolderDeleteIndex = -1;
    std::unordered_set<std::wstring> m_collapsedGroupKeys;
    // Canonical shared snapshot for query options: rebuilt only when the key
    // set changes (generation counter bumps at each mutation), so building
    // per-frame LibraryQueryOptions never deep-copies the set.
    uint64_t m_collapsedGroupsGeneration = 0;
    uint64_t m_collapsedGroupsSnapshotGeneration = 0;
    std::shared_ptr<const std::unordered_set<std::wstring>> m_collapsedGroupsSnapshot;
    static const std::shared_ptr<const std::unordered_set<std::wstring>>& EmptyCollapsedGroups() {
        static const std::shared_ptr<const std::unordered_set<std::wstring>> kEmpty =
            std::make_shared<const std::unordered_set<std::wstring>>();
        return kEmpty;
    }
    const std::shared_ptr<const std::unordered_set<std::wstring>>& CollapsedGroupsSnapshot() {
        if (m_collapsedGroupKeys.empty()) return EmptyCollapsedGroups();
        if (m_collapsedGroupsSnapshotGeneration != m_collapsedGroupsGeneration || !m_collapsedGroupsSnapshot) {
            m_collapsedGroupsSnapshot = std::make_shared<const std::unordered_set<std::wstring>>(m_collapsedGroupKeys);
            m_collapsedGroupsSnapshotGeneration = m_collapsedGroupsGeneration;
        }
        return m_collapsedGroupsSnapshot;
    }
    std::unordered_set<std::wstring> selectedFolderKeys;
    bool isMouseTracking = false, isOverlayMouseTracking = false, isVideoMouseTracking = false;
    // Unified Drag & Drop State
    enum class DragType { None, MediaList, PlaylistRail, PlaylistItem, MediaFolder };
    DragType activeDragType = DragType::None;
    DragType pendingDragType = DragType::None;
    float dragStartX = 0;
    D2D1_RECT_F dragSourceRect = {};
    std::vector<std::wstring> dragPathSnapshot;
    std::string dragPlaylistId;
    std::vector<std::string> dragRailSnapshot;
    std::unordered_map<int, std::pair<D2D1_POINT_2F, D2D1_POINT_2F>> dragOffsets;
    void ArmCardDrag(DragType type, int index, float x, float y);
    void CancelCardDrag();
    D2D1_POINT_2F CardDragOffset(int index, float x, float y);
    int dragFromIndex = -1, dragToIndex = -1;
    float dragStartY = 0.0f, dragCurrentY = 0.0f, dragCurrentX = 0.0f;
    bool isPlaylistsNavDropHovered = false;
    // Context Menu State
    bool isContextMenuOpen = false;
    bool isSplitIrisMenu = false;
    D2D1_POINT_2F contextMenuOrigin = { 0, 0 };
    D2D1_POINT_2F irisAnchorCursor = { 0, 0 };
    std::vector<ContextMenuItem> contextMenuItems;
    int hoveredContextMenuItem = -1;
    IrisShoulderButton irisLeftShoulder;
    IrisShoulderButton irisRightShoulder;
    int hoveredIrisShoulder = 0; // 0: none, 1: left, 2: right
    int activeIrisDialIndex = -1;
    std::array<float, 5> irisDialApertures = { 0.56f, 0.56f, 0.56f, 0.56f, 0.56f };
    float irisTargetWedgeAngle = 0.0f;
    float irisCurrentWedgeAngle = 0.0f;
    float irisApertureTarget = 0.22f;
    float irisApertureCurrent = 0.06f;
    AnimatedFloat contextMenuAnim;
    AnimatedFloat flyoutAnim;
    // Menus fade out on close: hit-testing stops as soon as the open flag drops; the closing flag only extends drawing until the fade ends.
    bool contextMenuClosing = false, flyoutClosing = false;
    AnimatedFloat timelineHoverAnim;
    // Screenshot State
    std::wstring lastScreenshotPath;
    std::mutex screenshotMutex;
    // Modal State
    ModalType activeModal = ModalType::None;
    ModalType modalFocusOwner = ModalType::None;
    int modalKeyboardFocus = 0;
    std::wstring modalTitle, modalPrompt, modalMessage, modalConfirmText;
    bool modalIsDestructive = true;
    void ShowPlaybackRecovery(const std::wstring& path, const std::wstring& explanation,
        const std::vector<std::wstring>& queue, size_t index, double position);
    std::function<void()> modalOnConfirm;
    std::function<void()> modalOnCancel;
    bool isModalOkHovered = false, isModalCancelHovered = false, isModalOkPressed = false, isModalCancelPressed = false;
    std::string renamePlaylistId, editingSmartPlaylistId;
    static constexpr UINT_PTR kTextInputCaretTimer = 0xA712;
    static constexpr UINT_PTR kTextInputImeEchoTimer = 0xA713;
    static constexpr UINT_PTR kThumbnailRefreshTimer = 0xA714;
    static constexpr UINT_PTR kHeartbeatTimer = 0xA715;
    bool thumbnailRefreshTimerPending = false;
    // Resource-efficiency helper: EcoQoS while minimized and idle.
    void SetEcoThrottling(bool enable);
    bool EnsureOverlayReady();
    // OLE Drag & Drop Hover Target
    IDropTarget* pOleDropTarget = nullptr;
    void OnOleDragEnter(IDataObject* pDataObj, POINTL pt);
    void OnOleDragOver(POINTL pt);
    void OnOleDragLeave();
    void OnOleDrop(IDataObject* pDataObj, POINTL pt);
    void ProcessDroppedFiles(const std::vector<std::wstring>& droppedList, POINT dropPt);
    // Centralized Portable Persistence Save Wrappers
    bool SaveAll();
    void ReportStorageFailure(const StorageStatus& status, const std::wstring& context);
    void ReportAppError(const AppError& error, float durationSec = 4.0f);
    // Display / System Power Request Management
    void UpdateDisplayPowerRequest();
    void SetDisplayPowerRequest(bool enable);
    void ApplyWindowTheme();
    void ApplyTooltipTheme();
    void RefreshHubTooltips(bool force = false);
    void RefreshPlayerTooltips(bool force = false);
    void HideTooltipKeyboardFocus();
    void ShowHubTooltipForKeyboardFocus();
    bool IsReducedMotionEnabled() const;
    void AnimateVisual(AnimatedFloat& animation, float target, float duration, const CubicBezier* curve);
    void SetNavHoverTargets(bool media, bool playlists, bool history, bool settings, bool openFile);
    void SnapVisualAnimations();
    void ConfigureAccessibleScroll(AccessibilityNode& node, float App::*offset, float maximum,
                                   float viewHeight, float contentHeight, float lineHeight, float dp);
    void RefreshAccessibilityTree(bool force = false);
    void QueueAccessibilityAction(LONG actionId);
    void InvokeAccessibilityAction(LONG actionId);
    void MarkAccessibilityDirty() { accessibilityDirty = true; }
    void AnnounceStatus(const std::wstring& text, bool force = false);
    void ProcessPendingAnnouncements();
    void RefreshHubHitTargets(float mouseX, float mouseY);
    void RefreshOverlayHitTargets(float mouseX, float mouseY);
    bool IsPlaybackDiagnosticsCloseHit(float x, float y) const;
    void ClampScrollOffsets();
    void PersistWindowPlacement();
    void PersistPiPGeometry();
    void PreSizeVideoSurfaceForPresentation();
    void PromoteLastWatchedMedia();
    void TriggerShortcut(const std::string& actionId);
    enum class PlayerMenuReveal { None, Controls, Cursor };
    void PresentHubContextMenu(float x, float y, float width);
    void PresentPlayerContextMenu(float x, float y, float width, bool animate, PlayerMenuReveal reveal);
    void ApplyExperienceMenuFilter();
    void PrepareContextMenu(float& x, float& y, float width, float dp, bool reposition = false);
    void PrepareSplitIrisContextMenu(float x, float y, float dp, bool resetIrisAnimation);
    void SetIrisShoulders(IrisShoulderButton left, IrisShoulderButton right);
    void SetActiveIrisDial(int index);
    void StepIrisDial(int index, int dir);
    void ResetIrisDial(int index);
    std::array<IrisDialSlotViewModel, 5> BuildIrisDialViewModels() const;
    void PopulateContextMenuViewModel(ContextMenuViewModel& out) const;
    void UpdateContextMenuHitTarget(float mouseX, float mouseY, float dp);
    bool HandleContextMenuClick(float mouseX, float mouseY, float dp);
    bool HandleContextMenuWheel(short delta);
    bool TickSplitIrisAnimation(float dt);
    void OpenMediaContextMenu(const MediaItem& item, float x, float y);
    void OpenFolderContextMenu(int groupIndex, float x, float y);
    void RemoveSelectedFolders();
    void RunFolderRediscovery(bool showFeedback = true);
    void FinishFolderRediscovery();
    void OpenPlaylistContextMenu(const Playlist& playlist, float x, float y);
    void OpenRailBackgroundContextMenu(float x, float y);
    void OpenPlaylistItemContextMenu(size_t itemIndex, float x, float y);
    void OpenHistoryContextMenu(const HistoryRecord& item, float x, float y);
    void OpenBackgroundContextMenu(float x, float y);
    void OpenPlayerContextMenu(float x, float y);
    void OpenPlayerAdvancedContextMenu(float x, float y);
    void CycleABRepeat();
    void OpenSleepTimerContextMenu(float x, float y);
    void OpenAudioDelayContextMenu(float x, float y);
    void OpenAudioBalanceContextMenu(float x, float y);
    void OpenAudioChannelMapContextMenu(float x, float y);
    void OpenNightModeContextMenu(float x, float y);
    void OpenReplayGainContextMenu(float x, float y);
    void OpenDeinterlaceContextMenu(float x, float y);
    void OpenAudioDeviceContextMenu(float x, float y);
    void OpenSubtitleDelayContextMenu(float x, float y);
    void OpenVideoAdjustmentsContextMenu(float x, float y);
    void OpenJumpSizeContextMenu(float x, float y);
    void OpenAdjustmentContextMenu(const std::string& type, float x, float y);
    void OpenSubtitlesContextMenu(float x, float y);
    void OpenSubtitleSafeAreaContextMenu(float x, float y);
    void OpenSubtitleCodepageContextMenu(float x, float y);
    void OpenSubtitleFontContextMenu(float x, float y);
    void OpenDualSubtitleStyleContextMenu(float x, float y);
    void OpenSubtitleProfilesContextMenu(float x, float y);
    void OpenOnlineSubtitlesContextMenu(float x, float y);
    void OpenRenameSubtitleProfileContextMenu(float x, float y);
    void OpenDeleteSubtitleProfileContextMenu(float x, float y);
    void ShowSaveSubtitleProfileModal();
    void ShowRenameSubtitleProfileModal(const std::string& profileName);
    void ShowDeleteSubtitleProfileModal(const std::string& profileName);
    void ApplySubtitleStyleProfile(const std::string& profileName);
    void RestoreSubtitleStyleDefault();
    void OpenChaptersContextMenu(float x, float y);
    void OpenBookmarksContextMenu(float x, float y);
    void OpenDeleteBookmarksContextMenu(float x, float y);
    void ShowAddBookmarkModal();
    void PromptClearAllBookmarks();
    void OpenShortcutOverlay();
    void ToggleSubtitles();
    void ApplyTrackPreferences();
    const std::vector<size_t>& GetVisiblePlaylistIndices();
    void ShowNewPlaylistModal();
    void OpenFileDialogForPlaylist(const std::string& playlistId);
    void OpenFileDialogForTarget(const std::string& targetPlaylistId, bool launchSingle);
    void ShowEditSmartPlaylistModal(const std::string& playlistId);
    void ShowNewSmartPlaylistModal();
    void ShowEditMetadataModal(const std::wstring& mediaPath);
    // Cover art: pick an image that replaces the item's extracted thumbnail.
    void SetCoverArtFromFile(const MediaItem& item);
    void ClearCoverArt(const MediaItem& item);
    // Rebuilds the renderer cover-art map after a library snapshot is adopted.
    void SyncCoverArtThumbnails();
    void ShowRenamePlaylistModal(const std::string& playlistId);
    void StartInlinePlaylistRename(const std::string& playlistId);
    void CommitInlinePlaylistRename();
    void CancelInlinePlaylistRename();
    void ShowDeletePlaylistModal(const std::string& playlistId);
    void ShowConfirmationModal(const std::wstring& title, const std::wstring& message, const std::wstring& confirmText, std::function<void()> onConfirm, std::function<void()> onCancel = nullptr, bool isDestructive = true);
    void StartShortcutCapture(const std::string& actionId, const std::string& actionName, int originRow = 1, int originSubIndex = 0);
    void CancelShortcutCapture();
    // Shared completion path for shortcut capture: keyboard (AppKeyboard) and
    // mouse pseudo-keys (Feature 4 routing) both land here.
    void CompleteShortcutCapture(UINT vkCode, bool ctrl, bool shift, bool alt);
    // Returns true and completes the active capture when one is in progress;
    // otherwise returns false so the message keeps its normal behavior.
    bool HandleShortcutCaptureMouse(UINT vkCode);
    // Player-context mouse binding execution: looks up a bound action for a
    // mouse pseudo-key (see CommandDispatcher::FindMouseBinding) and executes it.
    bool TryExecutePlayerMouseBinding(UINT vkCode);
    std::wstring GetCurrentCaptureChordText() const;
    void ShowCustomSeekStepModal();
    void ShowOpenNetworkStreamModal();
    void ImportStandardPlaylist();
    void ExportActivePlaylist(PlaylistFormat format = PlaylistFormat::M3U8);
    void ShowCustomFrameSeekModal();
    void OpenSeekStepContextMenu(float x, float y);
    void OpenFrameSeekContextMenu(float x, float y);
    void OpenNetworkForwardCacheContextMenu(float x, float y);
    void OpenNetworkBackCacheContextMenu(float x, float y);
    void ShowCustomNetworkForwardCacheModal();
    void ShowCustomNetworkBackCacheModal();
    void ToggleCustomMpvConfig();
    void EditCustomMpvConfigFile();
    void ResetCustomMpvConfigToDefault();
    void RevertCustomMpvConfigToLastKnownGood();
    void CheckForUpdates(bool isManual = true);
    void TriggerUpdateDownloadAndApply(const ReleaseManifest& manifest);
    TextInputModel* ActiveTextInputModel();
    void OpenGlobalSearch();
    void CloseGlobalSearch();
    void ToggleGlobalSearch();
    void ExecuteGlobalSearchAction(const GlobalSearchActionResult& action);
    const TextInputModel* ActiveTextInputModel() const;
    void FocusTextInput(TextInputField field);
    void BlurTextInput();
    void ResetTextInputCaret();
    void StopTextInputCaretTimer();
    void MarkTextInputEdit(bool committed);
    TextInputModel::ClipboardCallbacks MakeClipboardCallbacks();
    bool HandleTextInputKeyDown(WPARAM wParam, LPARAM lParam);
    bool HandleTextInputChar(wchar_t ch);
    bool HandleTextInputIme(UINT message, WPARAM wParam, LPARAM lParam);
    bool HitTestTextInput(float x, float y, std::size_t& outIndex);
    void SetTextInputCaretFromMouse(float x, float y, bool extendSelection);
    void UpdateImeCandidateWindow();
    // Media Virtualized Layout Cache (in DIP)
    struct MediaLayoutCache {
        std::vector<float> rowOffsetsDIP;
        std::vector<float> groupHeaderOffsetsDIP;
        float contentHeightDIP = 0.0f;
        uint64_t projectionGeneration = 0;
        size_t itemCount = 0;
        LibraryGroupMode groupMode = LibraryGroupMode::Flat;
        bool lastWatchedCleared = false, hasPinned = false;
        uint64_t collapsedGroupsGeneration = 0;
        bool dirty = true;
        size_t dirtyFromIndex = 0;
    };
    MediaLayoutCache m_mediaLayoutCache;
    std::vector<size_t> m_activeAnimRowIndices;
    void EnsureMediaLayoutCache();
    void InvalidateMediaLayoutCache(size_t fromIndex = 0);
    std::pair<size_t, size_t> GetVisibleMediaRowRange(float scrollYDIP, float viewportHDIP, size_t totalCount, int overscan = 2);
    int HitTestMediaRow(float yDIP, size_t totalCount);
    int HitTestSystemSettingsRow(const D2D1_RECT_F& contentBounds, float contentY, float x, float y);
    int HitTestMediaGroupHeader(float yDIP, size_t groupCount);
    float GetMediaRowTopDIP(size_t index);
    float GetMediaRowHeightDIP(size_t index);
    void EnqueueActiveRowAnimation(size_t index);
    // File observations are shared with smart playlists and refreshed on workers.
    bool IsMediaFileMissing(const std::wstring& path);
    // Borrowed-frame chapter list cache, keyed on MpvPlayer's version counter.
    mutable std::vector<MpvChapterInfo> m_cachedChapters;
    mutable uint64_t m_chaptersCacheVersion = (std::numeric_limits<uint64_t>::max)();
    const std::vector<MpvChapterInfo>& GetCachedChapters() const;
    // Track lists change only on mpv track-list events; version-gated caches
    // keep the per-frame view-model build from re-copying them under stateMutex.
    mutable std::vector<MpvTrackInfo> m_cachedSubTracks, m_cachedAudioTracks;
    mutable uint64_t m_subTracksCacheVersion = (std::numeric_limits<uint64_t>::max)();
    mutable uint64_t m_audioTracksCacheVersion = (std::numeric_limits<uint64_t>::max)();
    const std::vector<MpvTrackInfo>& GetCachedSubtitleTracks() const;
    const std::vector<MpvTrackInfo>& GetCachedAudioTracks() const;
    mutable ChapterMarkerCache m_chapterMarkersCache;
    std::shared_ptr<const std::vector<TimelineChapterMarker>> GetChapterMarkersSnapshot() const;
    // Cached history timeline projection (group buckets + ordering). History
    // ordering only changes when history changes or the day rolls over.
    HistoryTimelineProjection m_historyProjection;
    uint64_t m_historyProjectionVersion = 0;
    int64_t m_historyProjectionDay = -1;
    size_t m_historyProjectionSize = 0;
    const HistoryTimelineProjection& GetHistoryProjection();
    void InvalidateLibraryDerivedState();
};

} // namespace Aperture
