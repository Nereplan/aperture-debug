#include "App.h"
#include "AppInternals.h"
#include "AppLayout.h"
#include "CollectionAlgorithms.h"
#include "IpcProtocol.h"
#include "LibraryQuery.h"
#include "MediaFoundation.h"
#include "OnlineSubtitles.h"
#include "Reorder.h"
#include "Updater.h"
#include "Version.h"
#include "BuildInfo.h"
#include "ChannelPolicy.h"
#include "Localization.h"
#include "SettingsLayout.h"
#include "Presentation.h"
#include "WindowGeometry.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <imm.h>
#include <numeric>

namespace Aperture {


void App::PreSizeVideoSurfaceForPresentation() {
    // Resize the mpv child surface to the presentation target while the window is still
    // PiP-sized: if the surface is resized after the window expands, the old small frame
    // stays composited over the fullscreen window until mpv's next present, which reads
    // as the video sitting at its old bounds and then "expanding".
    if (!hWnd || !hVideoWnd || !playbackSession.IsPiP()) return;
    RECT windowRect{};
    if (!GetWindowRect(hWnd, &windowRect)) return;
    HMONITOR hMon = MonitorFromRect(&windowRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfo(hMon, &mi)) return;
    player.SetVideoBounds(0, 0, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
}


void App::PromoteLastWatchedMedia() {
    if (library.mediaItems.empty()) return;
    isLastWatchedCleared = false;
    if (!library.PromoteMostRecentlyWatched()) return;
    m_mediaQueryEngine.InvalidateCache();
    InvalidateMediaLayoutCache();
    rowAnimStates.clear();
    selectedMediaIndices.clear();
    selectedMediaIndex = -1;
}


bool App::EnsurePlayerInitialized() {
    if (playerInitialized) return true;
    if (playerInitAttempted) return false;
    playerInitAttempted = true;
    Diagnostics::RecordStartupMilestone(L"player-initialize-start", false);
    if (!player.Initialize(hWnd, hVideoWnd)) { playerInitialized = false; return false; }
    Diagnostics::RecordStartupMilestone(L"player-initialize-ready", false);
    // All callbacks and persisted mpv properties are installed exactly once,
    // immediately after the first successful main-player initialization.
    player.SetVolume(settings.volume);
    playbackSession.SetPriorUnmutedVolume(settings.volume > 0.01 ? settings.volume : 1.0);
    player.SetMuted(settings.muted);
    player.SetPlaybackRate(settings.defaultSpeed);
    player.SetLoop(settings.loop);
    player.SetAspectRatio(settings.lastAspectRatio);
    player.SetAudioDelay((double)settings.audioDelayMs / 1000.0);
    player.SetSubDelay((double)settings.subDelayMs / 1000.0);
    player.SetVideoVisible(false);
    ApplyPlayerPreferences();
    playerCallbacksDisabled.store(false, std::memory_order_release);
    player.SetOnEnded([this]() { QueuePlayerEvent(PlayerEventEnded); });
    player.SetOnTimeUpdated([this](double curTime) {
        const double last = lastQueuedPlayerTime.load(std::memory_order_relaxed);
        if (std::abs(curTime - last) < 0.25 && curTime >= last) return;
        lastQueuedPlayerTime.store(curTime, std::memory_order_relaxed);
        pendingPlayerTime.store(curTime, std::memory_order_release);
        QueuePlayerEvent(PlayerEventTime);
    });
    player.SetOnDurationChanged([this](double) {
        QueuePlayerEvent(PlayerEventDuration);
    });
    player.SetOnTracksChanged([this]() { QueuePlayerEvent(PlayerEventTracks); });
    player.SetOnChaptersChanged([this]() { QueuePlayerEvent(PlayerEventChapters); });
    player.SetOnPlayStateChanged([this](bool) { QueuePlayerEvent(PlayerEventPlayState); });
    player.SetOnError([this](const std::wstring& err) {
        { std::lock_guard<std::mutex> lock(pendingPlayerErrorMutex); pendingPlayerErrors.push_back(AppError::Playback(err)); }
        QueuePlayerEvent(PlayerEventError);
    });
    player.SetOnWarning([this](const std::wstring& err) {
        { std::lock_guard<std::mutex> lock(pendingPlayerErrorMutex);
          if (pendingPlayerWarnings.size() < 64) pendingPlayerWarnings.push_back(AppError::Playback(err)); }
        QueuePlayerEvent(PlayerEventWarning);
    });
    playerInitialized = true;
    return true;
}


bool App::EnsurePreviewInitialized() {
    if (renderer.AreDeviceWorkersRunning()) return false;
    if (previewInitialized) return true;
    if (previewInitAttempted) return false;
    // A direct launch can keep the hub GPU device unallocated. Real preview
    // intent still needs its D2D bitmap context; do not trade the feature away.
    if (!renderer.EnsureGraphicsContext()) return false;
    previewInitAttempted = true;
    // The preview owns an independent mpv handle and loads the package-local DLL on its own when needed (see MpvPreviewPlayer::Initialize
    // nullptr path). It must NOT force main-player initialization: an idle hub with hover previews enabled must stay free of the 45 MiB DLL
    // mapping, main mpv instance, and event threads until real playback or a real hover. Reuse the main player's DLL mapping only when it
    // already exists.
    HMODULE sharedDll = playerInitialized ? player.GetMpvDll() : nullptr;
    previewInitialized = previewPlayer.Initialize(renderer.GetD2DContext(), sharedDll);
    return previewInitialized;
}


bool App::EnsureOverlayReady() {
    if (overlayReady) return true;
    if (!hOverlayWnd) return false;
    RECT rc;
    GetClientRect(hWnd, &rc);
    UINT w = std::max(1u, (UINT)(rc.right - rc.left));
    UINT h = std::max(1u, (UINT)(rc.bottom - rc.top));
    // Reuse the main renderer's parsed private font set instead of building a second DirectWrite collection from the same TTF files.
    if (!overlayRenderer.Initialize(renderer.GetD2DFactory(), renderer.GetDWriteFactory(),
                                    renderer.GetDpiScale(), renderer.GetCustomFontCollection())) {
        return false;
    }
    overlayRenderer.SetTextScale(systemVisualPreferences.Get().textScaleFactor);
    overlayRenderer.Resize(w, h);
    if (!settings.uiFontFamily.empty()) { overlayRenderer.UpdateFontFamily(settings.uiFontFamily); }
    overlayReady = true;
    return true;
}


void App::SetEcoThrottling(bool enable) {
    // PROCESS_POWER_THROTTLING_EXECUTION_SPEED lets the scheduler move an idle process to efficiency cores / lower clocks (EcoQoS). It is
    // only applied while minimized and not playing so playback is unaffected.
    PROCESS_POWER_THROTTLING_STATE state = {};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = enable ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}


void App::ArmPlayerEventMessage() {
    if (playerCallbacksDisabled.load(std::memory_order_acquire) || !hWnd) return;
    bool expected = false;
    if (!playerEventPosted.compare_exchange_strong( expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) { return; }
    if (!PostMessageW(hWnd, kPlayerEventMessage, 0, 0)) {
        // A failed post normally means the window is already leaving the message loop. Release the gate so a later callback cannot leave a
        // stale "posted" state behind.
        playerEventPosted.store(false, std::memory_order_release);
    }
}


void App::QueuePlayerEvent(uint32_t flags) {
    if (flags == PlayerEventNone || playerCallbacksDisabled.load(std::memory_order_acquire)) { return; }
    // Time and duration are latest-value channels. Only the bit is queued;
    // the UI thread consumes the newest atomic value once per message.
    pendingPlayerEvents.fetch_or(flags, std::memory_order_release);
    if (playerCallbacksDisabled.load(std::memory_order_acquire)) return;
    ArmPlayerEventMessage();
}


void App::ClearPendingPlayerEvents() {
    pendingPlayerEvents.store(PlayerEventNone, std::memory_order_release);
    pendingPlayerTime.store(0.0, std::memory_order_release);
    playerEventPosted.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(pendingPlayerErrorMutex);
    pendingPlayerErrors.clear();
    pendingPlayerWarnings.clear();
}


void App::HandlePlayerEvents() {
    if (playerCallbacksDisabled.load(std::memory_order_acquire) || shutdownComplete) { ClearPendingPlayerEvents(); return; }
    const uint32_t events = pendingPlayerEvents.exchange(PlayerEventNone, std::memory_order_acq_rel);
    // The observed time/duration channels already cover playback, pause and
    // seeks. Update durable progress once per coalesced event, rather than
    // also polling and copying the path on every UI frame.
    if ((events & (PlayerEventTime | PlayerEventDuration)) && playbackSession.CanPersistPlayback()) {
        const auto path = player.GetCurrentFilePath();
        if (playbackSession.CanPersistLocalTarget(path)) library.UpdateProgress(path, player.GetCurrentTime(), player.GetDuration());
    }
    if (events & PlayerEventTime) {
        const double currentTime = pendingPlayerTime.load(std::memory_order_acquire);
        if (player.IsLoaded()) {
            int curChap = player.GetCurrentChapter();
            const std::vector<MpvChapterInfo>& chapters = GetCachedChapters();
            if (curChap < 0 && !chapters.empty() && player.GetDuration() > 0.0) {
                for (int i = (int)chapters.size() - 1; i >= 0; --i) {
                    if (chapters[i].time <= currentTime + 0.25) { curChap = i; break; }
                }
            }
            if (curChap >= 0 && curChap < (int)chapters.size()) {
                if (lastAnnouncedChapterIndex != -1 && curChap != lastAnnouncedChapterIndex) {
                    std::wstring title = chapters[curChap].title;
                    std::wstring announce = title.empty() ? (L"Chapter " + std::to_wstring(curChap + 1) + L" of " + std::to_wstring(chapters.size())) : (L"Chapter " + std::to_wstring(curChap + 1) + L": " + title);
                    AnnounceStatus(announce);
                }
                lastAnnouncedChapterIndex = curChap;
            } else if (chapters.empty()) { lastAnnouncedChapterIndex = -1; }
        }
        UpdateMediaTransport(false);
    }
    if (events & PlayerEventDuration) {
        UpdateMediaTransport(true);
    }
    if (events & PlayerEventEnded) {
        if (stopAfterCurrent) { stopAfterCurrent = false; ClosePlayer(); UpdateMediaTransport(true); return; }
        // Keep Web semantics on the UI thread: explicit queue navigation
        // takes precedence over autoplay; ordinary library navigation is
        // gated by autoplay. MpvPlayer owns loop-current and suppresses EOF.
        std::wstring nextPath;
        bool isQueue = false;
        if (playbackSession.OnEof(settings.autoplay, nextPath, isQueue)) {
            if (isQueue) {
                if (!nextPath.empty()) { OpenAndPlayMedia(nextPath, 0.0, true); }
            } else {
                NextEpisodeSafeAction episode = LibraryQueryEngine::ProjectNextEpisodeAction(
                    player.GetCurrentFilePath(), library, settings);
                if (episode.canExecuteAuto && !episode.nextFilePath.empty()) {
                    OpenAndPlayMedia(episode.nextFilePath);
                } else { PlayNextLibraryMedia(); }
            }
        } else { ShowOSD(IconType::Play, L"Playback Finished"); }
        UpdateMediaTransport(true);
    }
    if (events & PlayerEventWarning) {
        std::deque<AppError> warnings;
        { std::lock_guard<std::mutex> lock(pendingPlayerErrorMutex); warnings.swap(pendingPlayerWarnings); }
        for (const auto& warning : warnings) Diagnostics::Log(warning);
        if (!warnings.empty()) ShowOSD(IconType::VolumeMuted, warnings.back().userMessage, 4.0f);
    }
    if (events & PlayerEventError) {
        std::deque<AppError> errors;
        { std::lock_guard<std::mutex> lock(pendingPlayerErrorMutex); errors.swap(pendingPlayerErrors); }
        const auto failedPath = playbackSession.GetCurrentFilePath();
        const auto failedQueue = playbackSession.GetQueue();
        const auto failedIndex = playbackSession.GetQueueIndex();
        const double failedPosition = std::max(0.0, player.GetCurrentTime());
        if (!errors.empty() && playbackSession.IsOpen()) ClosePlayer();
        if (!shutdownComplete && !errors.empty()) {
            for (const auto& error : errors) Diagnostics::Log(error);
            const auto explanation = Loc::Tr(NetworkStreamValidator::IsValid(failedPath) ? "ux.playback_network" : "ux.playback_local");
            playbackSession.OnPlayerError(explanation);
            ShowPlaybackRecovery(failedPath, explanation, failedQueue, failedIndex, failedPosition);
        }
        UpdateMediaTransport(true);
    }
    if (events & (PlayerEventEnded | PlayerEventError | PlayerEventPlayState)) { UpdateDisplayPowerRequest(); }
    if (events & PlayerEventPlayState) {
        const bool isPlaying = player.IsPlaying();
        if (!hasAnnouncedPlayingState || isPlaying != lastAnnouncedPlayingState) {
            lastAnnouncedPlayingState = isPlaying;
            hasAnnouncedPlayingState = true;
            AnnounceStatus(isPlaying ? L"Playing" : L"Paused");
        }
        UpdateMediaTransport(true);
    }
    if (events & PlayerEventTracks) { ApplyTrackPreferences(); InvalidateRect(hWnd, NULL, FALSE); }
    if (events & PlayerEventChapters) { InvalidateRect(hWnd, NULL, FALSE); }
    playerEventPosted.store(false, std::memory_order_release);
    if (pendingPlayerEvents.load(std::memory_order_acquire) != PlayerEventNone) { ArmPlayerEventMessage(); }
}


bool App::IsQualifyingJumpAheadChapter(size_t chapterIndex, double& outTargetTime) const {
    outTargetTime = 0.0;
    if (!player.IsLoaded()) return false;
    const std::vector<MpvChapterInfo>& chapters = GetCachedChapters();
    if (chapters.empty() || chapterIndex >= chapters.size()) return false;

    double duration = player.GetDuration();
    if (duration <= 0.0) return false;

    double chStart = chapters[chapterIndex].time;
    double chEnd = (chapterIndex + 1 < chapters.size()) ? chapters[chapterIndex + 1].time : duration;
    double chDuration = chEnd - chStart;

    // Only for sections above 1 minute (60s) as requested, ignoring small bumpers/eyecatches
    if (chDuration <= 60.0) return false;

    if (chapterIndex + 1 < chapters.size()) {
        outTargetTime = chEnd;
    } else {
        outTargetTime = std::max(0.0, chEnd - 1.0);
    }
    if (outTargetTime <= chStart) return false;

    // Classifies one chapter without the cold-open structural rule below.
    // negative:   the title marks story content ("Prologue", cold opens) — never skippable,
    //             even when the duration looks like a theme song.
    // byTitle:    an explicitly marked theme song / credits / preview / recap section.
    // byDuration: an untitled 60-150s section (<= 40% of the episode, episode >= 5 min),
    //             the standard anime OP/ED window.
    auto classify = [chapters, duration](size_t index, bool& negative, bool& byTitle, bool& byDuration) {
        negative = byTitle = byDuration = false;
        if (index >= chapters.size()) return;
        const std::wstring title = chapters[index].title;
        std::wstring lower;
        lower.reserve(title.size());
        for (wchar_t c : title) lower.push_back(towlower(c));

        static const std::vector<std::wstring> negativeKeywords = { L"prologue", L"cold open", L"coldopen" };
        for (const auto& kw : negativeKeywords) {
            if (lower.find(kw) != std::wstring::npos) { negative = true; return; }
        }

        static const std::vector<std::wstring> keywords = {
            L"opening", L"ending", L"intro", L"outro", L"theme",
            L"credit", L"credits", L"preview", L"recap",
            L"ncop", L"nced"
        };
        for (const auto& kw : keywords) {
            if (lower.find(kw) != std::wstring::npos) { byTitle = true; return; }
        }
        auto hasToken = [](const std::wstring& str, const std::wstring& token) {
            size_t pos = 0;
            while ((pos = str.find(token, pos)) != std::wstring::npos) {
                bool leftBound = (pos == 0 || !iswalnum(str[pos - 1]));
                bool rightBound = (pos + token.size() >= str.size() || !iswalnum(str[pos + token.size()]));
                if (leftBound && rightBound) return true;
                pos += token.size();
            }
            return false;
        };
        if (hasToken(lower, L"op") || hasToken(lower, L"ed") ||
            hasToken(lower, L"op1") || hasToken(lower, L"op2") ||
            hasToken(lower, L"ed1") || hasToken(lower, L"ed2")) {
            byTitle = true;
            return;
        }

        double start = chapters[index].time;
        double end = (index + 1 < chapters.size()) ? chapters[index + 1].time : duration;
        double len = end - start;
        byDuration = duration >= 300.0 && len > 60.0 && len <= 150.0 && (len / duration) <= 0.40;
    };

    bool negative = false, byTitle = false, byDuration = false;
    classify(chapterIndex, negative, byTitle, byDuration);
    if (negative) return false;
    if (byTitle) return true;
    if (!byDuration) return false;

    // Cold-open structural rule: an untitled duration-qualified chapter immediately
    // followed by another qualifying chapter is a pre-opening story segment in front of
    // the real theme song ([cold open][OP][part A]) — the button belongs to the later
    // one only. Title-marked chapters skip this suppression, so a recap or an OP
    // always keeps its button.
    if (chapterIndex + 1 < chapters.size()) {
        bool nextNegative = false, nextByTitle = false, nextByDuration = false;
        classify(chapterIndex + 1, nextNegative, nextByTitle, nextByDuration);
        if (!nextNegative && (nextByTitle || nextByDuration)) return false;
    }
    return true;
}


double App::TimelineClickTarget(float clickX, float barLeft, float barWidth, double rawTime, std::wstring& outSnappedChapterTitle, bool ctrlHeld) const {
    outSnappedChapterTitle.clear();
    // Ctrl+click opts out of the chapter magnet entirely.
    if (ctrlHeld) return rawTime;
    const double duration = player.GetDuration();
    if (duration <= 0.0 || barWidth <= 0.0f) return rawTime;
    const std::vector<MpvChapterInfo>& chapters = GetCachedChapters();
    if (chapters.empty()) return rawTime;

    // Spatial magnet: a click within a few pixels of a chapter start snaps to the
    // chapter's exact beginning. Pixels, not seconds, so the gesture is identical at
    // any window size.
    const float snapRadiusPx = 12.0f * renderer.GetDpiScale();
    double best = rawTime;
    float bestDist = snapRadiusPx;
    const std::wstring* bestTitle = nullptr;
    for (const auto& ch : chapters) {
        if (ch.time <= 0.0 || ch.time >= duration) continue;
        const float chX = barLeft + static_cast<float>((ch.time / duration) * static_cast<double>(barWidth));
        const float dist = std::abs(clickX - chX);
        if (dist <= bestDist) {
            bestDist = dist;
            best = ch.time;
            bestTitle = &ch.title;
        }
    }
    if (bestTitle) outSnappedChapterTitle = *bestTitle;
    return best;
}


void App::UpdateMediaTransport(bool force) {
    if (shutdownComplete) { mediaTransport.Clear(); return; }
    if (!playbackSession.IsOpen()) { mediaTransport.Clear(); return; }
    if (!mediaTransport.IsInitialized() && hWnd) { mediaTransport.EnsureInitialized(hWnd); }
    MediaTransportSnapshot snapshot = MediaTransport::ProjectState( playbackSession, library, settings, player.GetCurrentTime(),
        player.GetDuration() );
    mediaTransport.Update(snapshot, force);
}


void App::ApplyPlayerPreferences() {
    // The player setters are intentionally no-ops until libmpv is available, so this can also be used during initialization before the
    // first file is opened.
    player.SetHardwareAcceleration(settings.hwAccel);
    player.SetPreferredAudioLanguage(settings.audioLang);
    player.SetPreferredSubtitleLanguage(settings.subLang);
    const auto& activePrefs = playbackSession.GetActivePreferences();
    const bool hasPerMediaSubtitleVisibility = activePrefs.has_value() && (activePrefs->subtitlesVisible.has_value() ||
         (activePrefs->subtitleTrackId.has_value() && *activePrefs->subtitleTrackId == 0));
    if (!hasPerMediaSubtitleVisibility) { player.SetSubtitleVisibility(settings.subtitlesEnabled); }
    player.SetSubtitleStyle(settings.subFontSize, settings.subColorHex, settings.subBgStyle, settings.subPosition, settings.subSafeAreaX, settings.subSafeAreaY);
    player.SetColorPreferences(settings.hdrMode, settings.toneMappingAlgorithm, settings.targetPeak, settings.gamutMapping, settings.iccProfilePath);
    player.SetAudioPreferences(settings.audioDevice, settings.audioLayout, settings.audioChannelMix, settings.audioNormalizeDownmix, settings.nightMode, settings.audioBalance, settings.audioChannelMap);
    player.SetCustomMpvConfigEnabled(settings.customMpvConfigEnabled);
    player.SetCustomMpvConfigPath(settings.customMpvConfigPath);
    if (settings.customMpvConfigEnabled)
        player.LoadAndApplyCustomMpvConfig(Settings::ResolveMpvConfigPath(settings.customMpvConfigPath));
}


bool App::UpdateMediaPathReferences(const std::wstring& oldPath, const std::wstring& newPath) {
    if (oldPath.empty() || newPath.empty()) return false;
    auto normalizePath = [](std::wstring value) {
        for (auto& c : value) { if (c == L'/') c = L'\\'; c = (wchar_t)towlower(c); }
        while (value.size() > 3 && !value.empty() && value.back() == L'\\') value.pop_back();
        return value;
    };
    auto samePath = [&](const std::wstring& a, const std::wstring& b) { return normalizePath(a) == normalizePath(b); };
    bool changed = false;
    for (auto& item : library.mediaItems) {
        if (samePath(item.filePath, oldPath)) { item.filePath = newPath; changed = true; }
    }
    for (auto& playlist : library.playlists) {
        for (auto& path : playlist.itemPaths) {
            if (samePath(path, oldPath)) { path = newPath; changed = true; }
        }
    }
    for (auto& record : library.history) {
        if (samePath(record.filePath, oldPath)) { record.filePath = newPath; changed = true; }
    }
    if (playbackSession.UpdatePathReferences(oldPath, newPath)) { changed = true; SaveSession(); }
    if (changed) { library.RebuildPathIndex(); library.InvalidatePlaylistStatsCache(); }
    return changed;
}


std::wstring App::RelocateMissingMedia(const std::wstring& missingPath) {
    if (missingPath.empty()) return {};
    const auto title = Loc::Tr("ux.missing_title");
    const auto body = Loc::Tr("ux.missing_body") + L"\n\n" + missingPath;
    const auto locate = Loc::Tr("ux.locate_file");
    TASKDIALOG_BUTTON action{IDYES, locate.c_str()};
    TASKDIALOGCONFIG dialog{sizeof(dialog)};
    dialog.hwndParent = hWnd;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.pszWindowTitle = L"Aperture";
    dialog.pszMainInstruction = title.c_str();
    dialog.pszContent = body.c_str();
    dialog.cButtons = 1;
    dialog.pButtons = &action;
    dialog.nDefaultButton = IDYES;
    int choice = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&dialog, &choice, nullptr, nullptr)))
        choice = MessageBoxW(hWnd, body.c_str(), title.c_str(), MB_ICONWARNING | MB_YESNO);
    if (choice != IDYES) return {};
    std::vector<wchar_t> selected(Ipc::MAX_IPC_PATH_CHARS, L'\0');
    OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = selected.data();
    ofn.nMaxFile = static_cast<DWORD>(selected.size());
    ofn.lpstrFilter = L"Media Files\0*.mp4;*.mkv;*.avi;*.mov;*.webm;*.wmv;*.flv;*.m4v;*.mp3;*.wav;*.flac;*.aac;*.m4a;*.ogg;*.opus\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;
    // comdlg32 is delay-loaded (see DelaySafeGetOpenFileNameW): a missing DLL acts as user-cancel, never a crash.
    if (!DelaySafeGetOpenFileNameW(&ofn)) return {};
    const std::wstring replacement = Ipc::ResolveFullPathLong(selected.data());
    if (!FileExists(replacement) || !MediaLibrary::IsSupportedMediaExtension(replacement)) {
        MessageBoxW(hWnd, L"Please choose an existing supported media file.", L"Invalid Media", MB_ICONERROR | MB_OK);
        return {};
    }
    if (UpdateMediaPathReferences(missingPath, replacement)) {
        if (MediaItem* item = library.FindByPath(replacement)) {
            // Keep progress and user metadata on the existing record while refreshing only the filesystem fact that changed.
            item->fileSizeBytes = GetFileSizeInBytes(replacement);
        } else {
            library.ImportPaths({ replacement });
        }
        SaveLibrary();
        ShowOSD(IconType::Refresh, L"Media location updated");
        return replacement;
    }
    // A history-only or stale playlist reference still needs to be indexed.
    library.ImportPaths({ replacement });
    SaveLibrary();
    return replacement;
}


bool App::ExecuteAutomationCommand(const Ipc::AutomationCommand& cmd, Ipc::AutomationStatus* outStatus) {
    if (closeRequested) return false;
    switch (cmd.op) {
    case Ipc::AutomationOp::Play: {
        if (playbackSession.IsOpen()) {
            player.Play();
            playbackSession.SetPlayState(true);
            TriggerPlayPauseBadge(true);
            RequestRender();
            return true;
        } else if (playbackSession.HasQueue()) { PlayFirstInQueue(); return true; }
        return true;
    }
    case Ipc::AutomationOp::Pause: {
        if (playbackSession.IsOpen()) {
            player.Pause();
            playbackSession.SetPlayState(false);
            TriggerPlayPauseBadge(false);
            RequestRender();
        }
        return true;
    }
    case Ipc::AutomationOp::Stop: { ClosePlayer(); return true; }
    case Ipc::AutomationOp::Next: {
        if (playbackSession.IsQueuePlayback()) { PlayNextInQueue(); } else { PlayNextLibraryMedia(); }
        return true;
    }
    case Ipc::AutomationOp::Previous: {
        if (playbackSession.IsQueuePlayback()) { PlayPrevInQueue(); }
        return true;
    }
    case Ipc::AutomationOp::Seek: {
        if (playbackSession.IsOpen()) {
            if (cmd.seekRelative) {
                player.SeekRelative(cmd.seekPositionSec);
                TriggerDoubleTapFeedback(cmd.seekPositionSec >= 0, static_cast<int>(std::abs(cmd.seekPositionSec)));
            } else { double targetPos = std::max(0.0, cmd.seekPositionSec); player.Seek(targetPos); }
            RequestRender();
            return true;
        }
        return false;
    }
    case Ipc::AutomationOp::Volume: {
        double vol0to100 = std::clamp(cmd.volume, 0.0, 100.0);
        double vol0to1 = vol0to100 / 100.0;
        player.SetVolume(vol0to1);
        player.SetMuted(false);
        settings.volume = vol0to1;
        settings.muted = false;
        if (vol0to1 > 0.01) { playbackSession.SetPriorUnmutedVolume(vol0to1); }
        ShowOSD(vol0to1 <= 0.01 ? IconType::VolumeMuted : IconType::VolumeHigh, std::to_wstring(static_cast<int>(std::round(vol0to100))) + L"%");
        RequestRender();
        return true;
    }
    case Ipc::AutomationOp::Open: {
        if (cmd.mediaTargets.empty()) { return false; }
        PlaybackCommandLineOptions opts;
        opts.mediaTargets = cmd.mediaTargets;
        if (cmd.startPositionSec > 0.0) { opts.startPositionSec = cmd.startPositionSec; }
        opts.paused = cmd.paused;
        opts.fullscreen = cmd.fullscreen;
        ExecuteCommandLineOptions(opts);
        return true;
    }
    case Ipc::AutomationOp::Enqueue: {
        if (cmd.mediaTargets.empty()) { return false; }
        if (playbackSession.IsOpen()) {
            playbackSession.AppendToQueue(cmd.mediaTargets);
            StartImportJob(cmd.mediaTargets, "", false);
            ShowOSD(IconType::FolderPlaylist, L"Enqueued " + std::to_wstring(cmd.mediaTargets.size()) + L" media");
        } else {
            playbackSession.OpenQueue(cmd.mediaTargets, 0);
            OpenAndPlayMedia(cmd.mediaTargets.front(), 0.0, true);
            StartImportJob(cmd.mediaTargets, "", false);
        }
        return true;
    }
    case Ipc::AutomationOp::QueryStatus: {
        if (outStatus) {
            PlaybackState st = playbackSession.GetState();
            switch (st) {
            case PlaybackState::Closed: outStatus->state = Ipc::AutomationPlaybackState::Closed; break;
            case PlaybackState::Initializing: outStatus->state = Ipc::AutomationPlaybackState::Initializing; break;
            case PlaybackState::Playing: outStatus->state = Ipc::AutomationPlaybackState::Playing; break;
            case PlaybackState::Paused: outStatus->state = Ipc::AutomationPlaybackState::Paused; break;
            case PlaybackState::Error: outStatus->state = Ipc::AutomationPlaybackState::Error; break;
            default: outStatus->state = Ipc::AutomationPlaybackState::Closed; break;
            }
            outStatus->positionSec = player.GetCurrentTime();
            outStatus->durationSec = player.GetDuration();
            outStatus->volume = player.GetVolume() * 100.0;
            outStatus->isMuted = player.IsMuted();
            outStatus->isLoop = player.GetLoop();
            outStatus->isFullscreen = playbackSession.IsFullscreen();
            outStatus->currentFilePath = playbackSession.GetCurrentFilePath();
        }
        return true;
    }
    default: return false;
    }
}


void App::SyncOverlayPosition() {
    if (!hWnd || !hOverlayWnd) return;

    // Presentation changes synchronously send WM_SIZE/WM_MOVE while the
    // top-level window is being resized. Keep the layered overlay out of
    // those intermediate bounds and place it once the transition completes.
    if (inPresentationTransition) return;

    if (IsIconic(hWnd) || !IsWindowVisible(hWnd)) { ShowWindow(hOverlayWnd, SW_HIDE); return; }
    RECT clientScreenRect{};
    const HWND insertAfter = playbackSession.IsPiPPinned() ? HWND_TOPMOST : HWND_NOTOPMOST;
    const UINT positionFlags = SWP_NOACTIVATE | SWP_NOREDRAW |
        (playbackSession.IsOpen() ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    if (PlaceWindowOverClient(hWnd, hOverlayWnd, insertAfter, positionFlags, &clientScreenRect)) {
        int w = clientScreenRect.right - clientScreenRect.left;
        int h = clientScreenRect.bottom - clientScreenRect.top;
        overlayRenderer.Resize(w, h);
    }
}


void App::ShowPlaybackRecovery(const std::wstring& path, const std::wstring& explanation,
    const std::vector<std::wstring>& queue, size_t index, double position) {
    ShowOSD(IconType::MediaFilm, explanation, 6.0f);
    if (path.empty() || activeModal != ModalType::None || shutdownComplete) return;
    ShowConfirmationModal(Loc::Tr("ux.playback_title"), explanation, Loc::Tr("ux.retry"), [this, path, queue, index, position]() {
            if (!queue.empty()) playbackSession.OpenQueue(queue, std::min(index, queue.size() - 1));
            OpenAndPlayMedia(path, position, !queue.empty());
        }, nullptr, false);
}


void App::OpenAndPlayMedia(const std::wstring& filePath, double startPosSec, bool preserveQueue) {
    ++playbackRequestGeneration;
    if (closeRequested) return;
    if (renderer.AreDeviceWorkersRunning()) {
        pendingPlaybackOpen = PendingPlaybackOpen{filePath, startPosSec, preserveQueue};
        return;
    }
    StopAutoscroll(true);
    playerKeyboardFocusId = 0;
    isKeyboardNavActive = false;
    std::wstring resolvedPath = filePath;
    const bool isNetworkStream = NetworkStreamValidator::IsValid(resolvedPath);
    if (!isNetworkStream && !FileExists(resolvedPath)) {
        resolvedPath = RelocateMissingMedia(resolvedPath);
        if (resolvedPath.empty() || !FileExists(resolvedPath)) return;
    }
    // Hub pumping stops during playback, so its idle timeout cannot release
    // these decoders then. Drop them before allocating the main decoder.
    hubExtractors[0].Shutdown();
    hubExtractors[1].Shutdown();
    if (!EnsurePlayerInitialized()) {
        const auto retryQueue = playbackSession.GetQueue();
        const auto retryIndex = playbackSession.GetQueueIndex();
        playbackSession.Close();
        if (hVideoWnd) ShowWindow(hVideoWnd, SW_HIDE);
        if (hOverlayWnd) ShowWindow(hOverlayWnd, SW_HIDE);
        Diagnostics::Log(AppError::Playback(L"mpv failed to initialize"));
        ShowPlaybackRecovery(resolvedPath, Loc::Tr("ux.playback_engine"), retryQueue, retryIndex, startPosSec);
        return;
    }
    startupPlayerPrepared = false;
    abLoopA = -1.0;
    abLoopB = -1.0;
    player.ClearABLoop();
    if (preserveQueue) {
        playbackSession.OpenPreserveQueue(resolvedPath, startPosSec);
    } else { playbackSession.OpenDirect(resolvedPath, startPosSec); }
    // Private playback reads existing preferences but creates no durable media record and does not update last-watched metadata.
    MediaItem* item = isNetworkStream ? nullptr : library.FindByPath(resolvedPath);
    if (playbackSession.CanPersistLocalTarget(resolvedPath)) {
        // Metadata uses the owned isolated worker even when a batch is already
        // active; its completion chains any newly eligible records.
        item = library.AddOrUpdateMedia(resolvedPath, false);
        // The active import already owns this probe. Starting backfill here
        // would open a second source reader while the first video warms up.
        if (item && !(importJobActive && importEarlyPlaybackPath == resolvedPath)) ScheduleMetadataBackfill();
    }
    if (playbackSession.CanPersistLocalTarget(resolvedPath)) {
        PromoteLastWatchedMedia();
        // Promotion swaps records in the vector: preserve the target identity,
        // not the address returned before the mutation.
        item = library.FindByPath(resolvedPath);
    }
    // Cache active preferences in playback session
    playbackSession.SetActivePreferences(item ? item->preferences : std::nullopt);
    // Apply per-media preferences or fallback to Settings defaults
    double targetSpeed = settings.defaultSpeed;
    if (item && item->preferences.has_value() && item->preferences->playbackSpeed.has_value() &&
        MediaPlaybackPreferences::IsValidPlaybackSpeed(*item->preferences->playbackSpeed)) {
        targetSpeed = *item->preferences->playbackSpeed;
    }
    player.SetPlaybackRate(targetSpeed);
    std::string targetAspect = settings.lastAspectRatio;
    if (item && item->preferences.has_value() && item->preferences->aspectRatio.has_value() &&
        MediaPlaybackPreferences::IsValidAspectRatio(*item->preferences->aspectRatio)) {
        targetAspect = *item->preferences->aspectRatio;
    }
    player.SetAspectRatio(targetAspect);
    int targetAudioDelay = settings.audioDelayMs;
    if (item && item->preferences.has_value() && item->preferences->audioDelayMs.has_value() &&
        MediaPlaybackPreferences::IsValidAudioDelayMs(*item->preferences->audioDelayMs)) {
        targetAudioDelay = *item->preferences->audioDelayMs;
    }
    player.SetAudioDelay((double)targetAudioDelay / 1000.0);
    int targetSubDelay = settings.subDelayMs;
    if (item && item->preferences.has_value() && item->preferences->subDelayMs.has_value() &&
        MediaPlaybackPreferences::IsValidSubDelayMs(*item->preferences->subDelayMs)) {
        targetSubDelay = *item->preferences->subDelayMs;
    }
    player.SetSubDelay((double)targetSubDelay / 1000.0);
    bool targetSubVisible = settings.subtitlesEnabled;
    if (item && item->preferences.has_value() && item->preferences->subtitlesVisible.has_value()) {
        targetSubVisible = *item->preferences->subtitlesVisible;
    } else if (item && item->preferences.has_value() && item->preferences->subtitleTrackId.has_value() && *item->preferences->subtitleTrackId == 0) {
        targetSubVisible = false;
    }
    player.SetSubtitleVisibility(targetSubVisible);
    int targetRotate = settings.videoRotate;
    if (item && item->preferences.has_value() && item->preferences->videoRotate.has_value() &&
        MediaPlaybackPreferences::IsValidVideoRotate(*item->preferences->videoRotate)) {
        targetRotate = *item->preferences->videoRotate;
    }
    player.SetVideoRotate(targetRotate);
    int targetBrightness = settings.brightness;
    if (item && item->preferences.has_value() && item->preferences->brightness.has_value() &&
        MediaPlaybackPreferences::IsValidBrightness(*item->preferences->brightness)) {
        targetBrightness = *item->preferences->brightness;
    }
    player.SetBrightness(targetBrightness);
    int targetContrast = settings.contrast;
    if (item && item->preferences.has_value() && item->preferences->contrast.has_value() &&
        MediaPlaybackPreferences::IsValidContrast(*item->preferences->contrast)) {
        targetContrast = *item->preferences->contrast;
    }
    player.SetContrast(targetContrast);
    int targetSaturation = settings.saturation;
    if (item && item->preferences.has_value() && item->preferences->saturation.has_value() &&
        MediaPlaybackPreferences::IsValidSaturation(*item->preferences->saturation)) {
        targetSaturation = *item->preferences->saturation;
    }
    player.SetSaturation(targetSaturation);
    int targetGamma = settings.gamma;
    if (item && item->preferences.has_value() && item->preferences->gamma.has_value() &&
        MediaPlaybackPreferences::IsValidGamma(*item->preferences->gamma)) {
        targetGamma = *item->preferences->gamma;
    }
    player.SetGamma(targetGamma);
    int targetHue = settings.hue;
    if (item && item->preferences.has_value() && item->preferences->hue.has_value() &&
        MediaPlaybackPreferences::IsValidHue(*item->preferences->hue)) {
        targetHue = *item->preferences->hue;
    }
    player.SetHue(targetHue);
    double targetBalance = settings.audioBalance;
    if (item && item->preferences.has_value() && item->preferences->audioBalance.has_value() &&
        MediaPlaybackPreferences::IsValidAudioBalance(*item->preferences->audioBalance)) {
        targetBalance = *item->preferences->audioBalance;
    }
    std::string targetChannelMap = settings.audioChannelMap;
    if (item && item->preferences.has_value() && item->preferences->audioChannelMap.has_value() &&
        MediaPlaybackPreferences::IsValidAudioChannelMap(*item->preferences->audioChannelMap)) {
        targetChannelMap = *item->preferences->audioChannelMap;
    }
    player.SetAudioPreferences(settings.audioDevice, settings.audioLayout, settings.audioChannelMix, settings.audioNormalizeDownmix, settings.nightMode, targetBalance, targetChannelMap);
    // Resume position if enabled
    double seekPos = startPosSec;
    if (seekPos <= 0.0 && settings.rememberPos && item && item->lastPosSec > 0.5 && item->status != "done") { seekPos = item->lastPosSec; }
    RECT rc;
    GetClientRect(hWnd, &rc);
    player.SetVideoBounds(0, 0, rc.right - rc.left, rc.bottom - rc.top);
    player.SetVideoVisible(true);
    if (player.Open(resolvedPath, seekPos)) {
        playbackSession.OnPlayerOpened(true);
        mediaGenerationToken++;
        previewPlayer.StopPreview();
        previewPlayer.ResetSeekbarPreview();
        controlsOpacity = 1.0f;
        controlsIdleTimer = 0.0f;
        lastPlayerMouseX = -1.0f;
        lastPlayerMouseY = -1.0f;
        isOverlayMouseTracking = false;
        isVideoMouseTracking = false;
        if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
        isSettingsFlyoutOpen = false;
        isContextMenuOpen = false;
        flyoutClosing = false; contextMenuClosing = false;
        flyoutAnim.SnapTo(0.0f); contextMenuAnim.SnapTo(0.0f);
        isJumpAheadVisible = false;
        jumpAheadTimer = 0.0f;
        jumpAheadActiveChapterIndex = -1;
        jumpAheadTargetTime = 0.0;
        isJumpAheadHovered = false;
        isJumpAheadPressed = false;
        jumpAheadAnim.SnapTo(0.0f);
        EnsureOverlayReady();
        ShowWindow(hVideoWnd, SW_SHOW);
        ShowWindow(hOverlayWnd, SW_SHOW);
        SyncOverlayPosition();
        if (playbackSession.CanPersistLocalTarget(resolvedPath))
            library.AddToHistory(resolvedPath, seekPos, item ? item->durationSec : 0.0);
        std::wstring title = item ? item->title : GetFileTitleFromPath(resolvedPath);
        SetWindowTextW(hWnd, (title + L" - " + std::wstring(Version::ProductNameWide)).c_str());
        Render();
        AnnounceStatus(L"Playing: " + title);
        if (seekPos > 0.0) { playbackSession.SetPlaybackPosition(seekPos); }
        lastAnnouncedChapterIndex = -1;
        SaveSession();
        UpdateDisplayPowerRequest();
        UpdateMediaTransport(true);
    }
}


void App::ClosePlayer() {
    ++playbackRequestGeneration;
    StopAutoscroll(true);
    playerKeyboardFocusId = 0;
    isKeyboardNavActive = false;
    if (playbackSession.IsOpen()) {
        isOverlayMouseTracking = false;
        isVideoMouseTracking = false;
        lastPlayerMouseX = -1.0f;
        lastPlayerMouseY = -1.0f;
        mediaGenerationToken++;
        previewPlayer.StopPreview();
        previewPlayer.ResetSeekbarPreview();
        if (playbackSession.CanPersistLocalTarget(player.GetCurrentFilePath())) {
            double curTime = player.GetCurrentTime();
            if (curTime >= 0.0 && std::isfinite(curTime)) { playbackSession.SetPlaybackPosition(curTime); }
            library.UpdateProgress(player.GetCurrentFilePath(), player.GetCurrentTime(), player.GetDuration());
            PromoteLastWatchedMedia();
            SaveLibrary();
        }
        SaveSession();
        lastAnnouncedChapterIndex = -1;
        isJumpAheadVisible = false;
        jumpAheadTimer = 0.0f;
        jumpAheadActiveChapterIndex = -1;
        jumpAheadTargetTime = 0.0;
        isJumpAheadHovered = false;
        isJumpAheadPressed = false;
        jumpAheadAnim.SnapTo(0.0f);
        player.Close();
        player.SetVideoVisible(false);
        if (hOverlayWnd) ShowWindow(hOverlayWnd, SW_HIDE);
        if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
        inPresentationTransition = true;
        playbackSession.ResetPresentation(hWnd);
        inPresentationTransition = false;
        RECT rc;
        GetClientRect(hWnd, &rc);
        renderer.Resize(rc.right - rc.left, rc.bottom - rc.top);
        // A stopped libmpv handle still owns demuxer state, decoder surfaces,
        // audio buffers, and its event thread.  Closing a session must release
        // those media-only resources instead of keeping them attached to the
        // hub for the rest of the process lifetime.  Fence callbacks while the
        // event thread is being joined; the flag is restored once no callback
        // can reference the player anymore so folder watching/import remain available on the hub.
        playerCallbacksDisabled.store(true, std::memory_order_release);
        ClearPendingPlayerEvents();
        if (previewInitialized || previewInitAttempted) { previewPlayer.Shutdown(); }
        previewInitialized = false;
        previewInitAttempted = false;
        if (playerInitialized || playerInitAttempted) { player.Shutdown(); }
        player.ClearCallbacks();
        playerInitialized = false;
        playerInitAttempted = false;
        ClearPendingPlayerEvents();
        playerCallbacksDisabled.store(false, std::memory_order_release);
        // The transparent overlay has a full-window DIB and a separate D2D
        // render target.  It is lazy, so drop it with the media session and
        // recreate it on the next open instead of charging it to idle hub RAM.
        overlayRenderer.Shutdown();
        overlayReady = false;
        playbackSession.Close();
        SaveSession();
        mediaTransport.Clear();
        playerTooltips.SetActive(false);
        playerTooltips.HideForKeyboard();
        playerTooltipSignature = 0;
        isSettingsFlyoutOpen = false;
        isContextMenuOpen = false;
        flyoutClosing = false; contextMenuClosing = false;
        flyoutAnim.SnapTo(0.0f); contextMenuAnim.SnapTo(0.0f);
        SetWindowTextW(hWnd, std::wstring(Version::ProductNameWide).c_str());
        InvalidateRect(hWnd, NULL, FALSE);
        UpdateDisplayPowerRequest();
        AnnounceStatus(L"Player closed");
    }
}


void App::PersistCurrentPlaybackProgress() {
    const std::wstring path = player.GetCurrentFilePath();
    if (!playbackSession.CanPersistLocalTarget(path)) return;
    library.UpdateProgress(path, player.GetCurrentTime(), player.GetDuration());
    SaveLibrary();
}


void App::PlayFirstInQueue() {
    PersistCurrentPlaybackProgress();
    std::wstring path = playbackSession.GetFirstQueuePath();
    if (!path.empty()) { OpenAndPlayMedia(path, 0.0, true); }
}


void App::PlayNextInQueue() {
    PersistCurrentPlaybackProgress();
    std::wstring path = playbackSession.GetNextQueuePath();
    if (!path.empty()) { OpenAndPlayMedia(path, 0.0, true); }
}


void App::PlayNextLibraryMedia() {
    if (library.mediaItems.empty()) return;
    const std::wstring currentPath = player.GetCurrentFilePath();
    size_t currentIndex = library.mediaItems.size();
    for (size_t i = 0; i < library.mediaItems.size(); ++i) {
        if (CompareStringOrdinal(library.mediaItems[i].filePath.c_str(), -1, currentPath.c_str(), -1, TRUE) == CSTR_EQUAL) {
            currentIndex = i;
            break;
        }
    }
    const size_t nextIndex = currentIndex == library.mediaItems.size() ? 0 : (currentIndex + 1) % library.mediaItems.size();
    PersistCurrentPlaybackProgress();
    OpenAndPlayMedia(library.mediaItems[nextIndex].filePath, 0.0, false);
}


void App::PlayPrevInQueue() {
    PersistCurrentPlaybackProgress();
    std::wstring path = playbackSession.GetPrevQueuePath(settings.loop);
    if (!path.empty()) { OpenAndPlayMedia(path, 0.0, true); }
}


void App::PlayPlaylist(const std::string& playlistId, size_t startIndex) {
    Playlist* pl = library.FindPlaylist(playlistId);
    if (!pl) return;
    std::vector<std::wstring> paths = library.GetPlaylistPaths(*pl);
    if (paths.empty()) return;
    playbackSession.OpenQueue(paths, startIndex);
    OpenAndPlayMedia(playbackSession.GetCurrentFilePath(), 0.0, true);
}


void App::ToggleFullscreen() {
    if (!hWnd || inPresentationTransition) return;
    if (playbackSession.IsPiP()) {
        PersistPiPGeometry();
        PreSizeVideoSurfaceForPresentation();
        isPiPPotentialWindowDrag = false;
        isPiPDraggingWindow = false;
        isPiPDragOccurred = false;
    }
    playbackSession.SetReduceMotion(IsReducedMotionEnabled());
    inPresentationTransition = true;
    if (hOverlayWnd) ShowWindow(hOverlayWnd, SW_HIDE);
    playbackSession.ToggleFullscreen(hWnd);
    inPresentationTransition = false;
    RECT rc;
    GetClientRect(hWnd, &rc);
    renderer.Resize(rc.right - rc.left, rc.bottom - rc.top);
    if (hVideoWnd) {
        SetWindowPos(hVideoWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
        player.SetVideoBounds(0, 0, rc.right - rc.left, rc.bottom - rc.top);
    }
    if (playbackSession.IsOpen() && hOverlayWnd) {
        ShowWindow(hOverlayWnd, SW_SHOW);
    }
    SyncOverlayPosition();
    Render();
}


void App::TogglePiP() {
    if (!hWnd || inPresentationTransition) return;
    isPiPVolumeSliderOpen = false;
    isPiPVolumeDragging = false;
    pipVolumeAnim.SnapTo(0.0f);
    isPiPPotentialWindowDrag = false;
    isPiPDraggingWindow = false;
    isPiPDragOccurred = false;
    playerKeyboardFocusId = 0;
    isKeyboardNavActive = false;
    if (playbackSession.IsPiP()) {
        PersistPiPGeometry();
        PreSizeVideoSurfaceForPresentation();
    }
    playbackSession.SetReduceMotion(IsReducedMotionEnabled());
    inPresentationTransition = true;
    if (hOverlayWnd) ShowWindow(hOverlayWnd, SW_HIDE);
    PiPGeometry savedGeom{ settings.pipX, settings.pipY, settings.pipWidth, settings.pipHeight };
    playbackSession.TogglePiP(hWnd, savedGeom);
    if (playbackSession.IsPiP()) {
        PersistPiPGeometry();
    } else {
        const auto& lastGeom = playbackSession.GetLastPiPGeometry();
        if (lastGeom.width > 0 && lastGeom.height > 0) {
            settings.pipX = lastGeom.x;
            settings.pipY = lastGeom.y;
            settings.pipWidth = lastGeom.width;
            settings.pipHeight = lastGeom.height;
        }
    }
    inPresentationTransition = false;
    RECT rc;
    GetClientRect(hWnd, &rc);
    renderer.Resize(rc.right - rc.left, rc.bottom - rc.top);
    if (hVideoWnd) {
        SetWindowPos(hVideoWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
        player.SetVideoBounds(0, 0, rc.right - rc.left, rc.bottom - rc.top);
    }
    if (playbackSession.IsOpen() && hOverlayWnd) {
        ShowWindow(hOverlayWnd, SW_SHOW);
    }
    SyncOverlayPosition();
    Render();
}


void App::ShowOSD(IconType icon, const std::wstring& text, float durationSec) {
    if (text.empty()) return;
    PendingNotification next{ icon, text.substr(0, 512), std::clamp(durationSec, 0.5f, 8.0f) };
    auto isVolumeIcon = [](IconType t) {
        return t == IconType::VolumeHigh || t == IconType::VolumeLow || t == IconType::VolumeMuted;
    };
    if (isVolumeIcon(next.icon)) {
        notificationQueue.erase(
            std::remove_if(notificationQueue.begin(), notificationQueue.end(),
                [&isVolumeIcon](const PendingNotification& item) { return isVolumeIcon(item.icon); }),
            notificationQueue.end());
    }
    if (osdTimer > 0.0f) {
        if (osdIcon == icon || (isVolumeIcon(osdIcon) && isVolumeIcon(icon))) {
            osdIcon = next.icon;
            osdText = next.text;
            osdTimer = osdDuration = next.durationSec;
        } else {
            auto existing = std::find_if(notificationQueue.begin(), notificationQueue.end(),
                [icon](const PendingNotification& item) { return item.icon == icon; });
            if (existing != notificationQueue.end()) *existing = next;
            else { if (notificationQueue.size() >= 4) notificationQueue.pop_front(); notificationQueue.push_back(next); }
        }
    } else { osdIcon = next.icon; osdText = next.text; osdTimer = osdDuration = next.durationSec; }
    InvalidateRect(hWnd, nullptr, FALSE);
    AnnounceStatus(next.text);
}


void App::TriggerDoubleTapFeedback(bool isForward, int deltaSeconds) {
    // Mirrors youtube_double_seek.html: repeated taps accumulate the number, the sign/number pop once per tap, a fresh chevron slides in
    // from the number side over the standing one, and switching direction fast-fades the opposite badge instead of cutting it.
    if (doubleTapTimer > 0.0f && doubleTapIsForward != isForward) {
        doubleTapFadeValid = true;
        doubleTapFadeForward = doubleTapIsForward;
        doubleTapFadeSeconds = doubleTapAccumSec;
        doubleTapFadeProgress = 0.0f;
        doubleTapFadeExitStart = std::clamp( 1.0f - doubleTapTimer / kSeekOsdFadeSec, 0.0f, 1.0f);
        doubleTapFadeAppear = doubleTapAppear;
        doubleTapFadeSignPop = doubleTapSignPop;
        doubleTapFadeNumberPop = doubleTapNumberPop;
        doubleTapFadeChevronSlides = doubleTapChevronSlides;
    }
    if (doubleTapTimer > 0.0f && doubleTapIsForward == isForward) { doubleTapAccumSec += deltaSeconds; doubleTapPresses++; } else {
        doubleTapIsForward = isForward;
        doubleTapAccumSec = deltaSeconds;
        doubleTapPresses = 1;
        doubleTapAppear = 0.0f;
        doubleTapChevronSlides.clear();
    }
    doubleTapTimer = kSeekOsdHoldSec + kSeekOsdFadeSec;
    doubleTapSignPop = 0.0f;
    doubleTapNumberPop = 0.0f;
    // HTML appends a new chevron for every press and lets older slides finish.
    // Keep the collection bounded against synthetic key-repeat storms.
    if (doubleTapChevronSlides.size() >= 8) { doubleTapChevronSlides.erase(doubleTapChevronSlides.begin()); }
    doubleTapChevronSlides.push_back(0.0f);
    InvalidateRect(hWnd, NULL, FALSE);
    AnnounceStatus(isForward ? (L"Seek forward " + std::to_wstring(deltaSeconds) + L" seconds") : (L"Seek backward " + std::to_wstring(deltaSeconds) + L" seconds"));
}


void App::TriggerPlayPauseBadge(bool isPlay) {
    playPauseAnimIsPlay = isPlay;
    playPauseAnimTimer = MotionTokens::kPlayPauseBadge;
    InvalidateRect(hWnd, NULL, FALSE);
    AnnounceStatus(isPlay ? L"Playing" : L"Paused");
}

} // namespace Aperture
