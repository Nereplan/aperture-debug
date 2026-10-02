#include "PlaybackSession.h"
#include "JsonStoreValidation.h"

namespace Aperture {

bool PlaybackSession::SameFilePath(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

PlaybackSession::PlaybackSession() { savedPlacement.length = sizeof(WINDOWPLACEMENT); }

bool PlaybackSession::OpenDirect(const std::wstring& filePath, double startPosSec) {
    if (filePath.empty()) return false;
    state = PlaybackState::Initializing;
    queueMode = QueueMode::DirectOpen;
    playbackQueue.clear();
    currentQueueIndex = 0;
    currentFilePath = filePath;
    currentError.clear();
    startPositionSec = (startPosSec >= 0.0 && std::isfinite(startPosSec)) ? startPosSec : 0.0;
    playbackPositionSec = startPositionSec;
    lastActiveSubtitleTrackId = 0;
    lastActiveSubtitleTrackTitle.clear();
    lastActiveAudioTrackId = 0;
    lastActiveAudioTrackTitle.clear();
    activePreferences.reset();
    return true;
}

bool PlaybackSession::OpenQueue(const std::vector<std::wstring>& items, size_t startIndex) {
    if (items.empty()) return false;
    state = PlaybackState::Initializing;
    queueMode = QueueMode::Queue;
    playbackQueue = items;
    currentQueueIndex = std::min(startIndex, playbackQueue.size() - 1);
    currentFilePath = playbackQueue[currentQueueIndex];
    currentError.clear();
    startPositionSec = 0.0;
    playbackPositionSec = 0.0;
    lastActiveSubtitleTrackId = 0;
    lastActiveSubtitleTrackTitle.clear();
    lastActiveAudioTrackId = 0;
    lastActiveAudioTrackTitle.clear();
    activePreferences.reset();
    return true;
}

bool PlaybackSession::OpenPreserveQueue(const std::wstring& filePath, double startPosSec) {
    if (filePath.empty()) return false;
    if (queueMode == QueueMode::Queue && !playbackQueue.empty()) {
        bool inCurrentQueue = false;
        size_t matchIndex = 0;
        if (currentQueueIndex < playbackQueue.size() && SameFilePath(playbackQueue[currentQueueIndex], filePath)) {
            inCurrentQueue = true;
            matchIndex = currentQueueIndex;
        } else {
            for (size_t i = 0; i < playbackQueue.size(); ++i) {
                if (SameFilePath(playbackQueue[i], filePath)) { inCurrentQueue = true; matchIndex = i; break; }
            }
        }
        if (inCurrentQueue) {
            currentQueueIndex = matchIndex;
            state = PlaybackState::Initializing;
            currentFilePath = filePath;
            currentError.clear();
            startPositionSec = (startPosSec >= 0.0 && std::isfinite(startPosSec)) ? startPosSec : 0.0;
            playbackPositionSec = startPositionSec;
            lastActiveSubtitleTrackId = 0;
            lastActiveSubtitleTrackTitle.clear();
            lastActiveAudioTrackId = 0;
            lastActiveAudioTrackTitle.clear();
            return true;
        }
    }
    // Fail closed to a direct open when requested file is not in the queue
    return OpenDirect(filePath, startPosSec);
}

bool PlaybackSession::OnPlayerOpened(bool isPlaying) {
    if (state == PlaybackState::Closed) return false;
    state = isPlaying ? PlaybackState::Playing : PlaybackState::Paused;
    return true;
}

bool PlaybackSession::OnPlayerError(const std::wstring& errorMsg) { state = PlaybackState::Error; currentError = errorMsg; return true; }

bool PlaybackSession::SetPlayState(bool isPlaying) {
    if (state == PlaybackState::Closed) return false;
    state = isPlaying ? PlaybackState::Playing : PlaybackState::Paused;
    return true;
}

bool PlaybackSession::OnEof(bool autoplayEnabled, std::wstring& outNextPath, bool& outIsQueue) {
    if (state == PlaybackState::Closed) return false;
    if (queueMode == QueueMode::Queue && !playbackQueue.empty()) {
        currentQueueIndex = (currentQueueIndex + 1) % playbackQueue.size();
        outNextPath = playbackQueue[currentQueueIndex];
        outIsQueue = true;
        return true;
    }
    if (autoplayEnabled) { outNextPath.clear(); outIsQueue = false; return true; }
    state = PlaybackState::Paused;
    return false;
}

void PlaybackSession::Close() {
    state = PlaybackState::Closed;
    queueMode = QueueMode::DirectOpen;
    playbackQueue.clear();
    currentQueueIndex = 0;
    currentFilePath.clear();
    currentError.clear();
    startPositionSec = 0.0;
    playbackPositionSec = 0.0;
    lastActiveSubtitleTrackId = 0;
    lastActiveSubtitleTrackTitle.clear();
    lastActiveAudioTrackId = 0;
    lastActiveAudioTrackTitle.clear();
    activePreferences.reset();
}

void PlaybackSession::SetPlaybackPosition(double posSec) { playbackPositionSec = (posSec >= 0.0 && std::isfinite(posSec)) ? posSec : 0.0; }

void PlaybackSession::Reset() {
    savesAllowed = true;
    Close();
    playbackPositionSec = 0.0;
    lastSavedJson.clear();
    persistedFields = JsonValue();
    loadedSchemaVersion = 0;
    recoveredFromBackup = false;
    lastSaveStatus = StorageStatus();
    lastLoadStatus = StorageStatus();
}

bool PlaybackSession::MigrateSchema(JsonValue& root, int fromVersion, int targetVersion) {
    if (!root.is_object()) return false;
    if (fromVersion >= targetVersion) return true;
    for (int v = fromVersion; v < targetVersion; ++v) {
        if (v == 0) {
            if (!root.has_key("queueMode")) root["queueMode"] = "direct";
            if (!root.has_key("currentFilePath")) root["currentFilePath"] = "";
            if (!root.has_key("currentQueueIndex")) root["currentQueueIndex"] = 0;
            if (!root.has_key("playbackPositionSec")) root["playbackPositionSec"] = 0.0;
            if (!root.has_key("queue") || !root.get("queue").is_array()) root["queue"] = JsonValue::Array();
            root["schemaVersion"] = 1;
        }
    }
    return true;
}

StorageStatus PlaybackSession::Save(const std::wstring& customPath, const std::wstring& baseDir) {
    if (!savesAllowed) { lastSaveStatus = lastLoadStatus; return lastSaveStatus; }
    if (isPrivateMode) { lastSaveStatus = StorageStatus::Ok(); return lastSaveStatus; }
    const std::wstring targetPath = customPath.empty() ? (GetDataDir() + L"\\session.json") : customPath;
    if (customPath.empty()) {
        StorageStatus writability = CheckDataDirectoryWritability();
        if (!writability.is_ok()) { lastSaveStatus = writability; return lastSaveStatus; }
    }
    JsonValue root = persistedFields.is_object() ? persistedFields : JsonValue::Object();
    root["schemaVersion"] = kCurrentSessionSchemaVersion;
    root["queueMode"] = (queueMode == QueueMode::Queue) ? "queue" : "direct";
    const bool localCurrent = CanPersistLocalTarget(currentFilePath);
    root["currentFilePath"] = localCurrent ? Utf16ToUtf8(ToRelocatablePath(currentFilePath, baseDir)) : "";
    root["playbackPositionSec"] = (localCurrent && playbackPositionSec >= 0.0 && std::isfinite(playbackPositionSec)) ? playbackPositionSec : 0.0;
    JsonValue qArr = JsonValue::Array();
    size_t count = std::min(playbackQueue.size(), kMaxQueueSize);
    size_t localIndex = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!CanPersistLocalTarget(playbackQueue[i])) continue;
        if (i < currentQueueIndex) ++localIndex;
        qArr.push_back(JsonValue(Utf16ToUtf8(ToRelocatablePath(playbackQueue[i], baseDir))));
    }
    root["currentQueueIndex"] = static_cast<int>(localIndex);
    root["queue"] = qArr;
    const std::string json = root.stringify(true);
    if (json != lastSavedJson || !FileExists(targetPath)) {
        StorageStatus backup = CreateRotatingBackup(targetPath, 3);
        if (!backup.is_ok()) { lastSaveStatus = backup; return lastSaveStatus; }
        StorageStatus writeRes = AtomicWriteTextFile(targetPath, json);
        if (!writeRes.is_ok()) { lastSaveStatus = writeRes; return lastSaveStatus; }
        lastSavedJson = json;
    }
    lastSaveStatus = StorageStatus::Ok(targetPath);
    return lastSaveStatus;
}

StorageStatus PlaybackSession::Load(const std::wstring& customPath, const std::wstring& baseDir) {
    recoveredFromBackup = false;
    loadedSchemaVersion = 0;
    savesAllowed = false;
    if (customPath.empty()) {
        const auto rootStatus = CheckPortableRootAvailability(GetExeDir());
        if (!rootStatus.is_ok()) { lastLoadStatus = rootStatus; return lastLoadStatus; }
    }
    std::string jsonStr;
    const std::wstring targetPath = customPath.empty() ? (GetDataDir() + L"\\session.json") : customPath;
    savesAllowed = false;
    DWORD pathError = ERROR_SUCCESS;
    const auto availability = QueryPathAvailability(targetPath, &pathError);
    if (availability == PathAvailability::Missing) { savesAllowed = true; lastLoadStatus = StorageStatus::Ok(targetPath); return lastLoadStatus; }
    if (availability == PathAvailability::Unavailable) {
        lastLoadStatus = StorageStatus::Error(StorageErrorCode::FileReadFailed, pathError, targetPath, "Session file is unavailable");
        return lastLoadStatus;
    }
    StorageStatus readStatus = ReadTextFile(targetPath, jsonStr);
    JsonValue root;
    const bool parsed = readStatus.is_ok() && JsonValue::parse(jsonStr, root);
    if (parsed && HasFutureStoreSchema(root, kCurrentSessionSchemaVersion)) {
        lastLoadStatus = StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_REVISION_MISMATCH, targetPath, "Session requires a newer application version");
        return lastLoadStatus;
    }
    bool parseOk = parsed && HasSupportedStoreSchema(root, kCurrentSessionSchemaVersion) &&
        (!root.has_key("queue") || root.get("queue").is_array());
    if (!parseOk) {
        std::string backupText;
        StorageStatus backupStatus = RestoreFromNewestValidBackup(targetPath, backupText, [](const std::string& t) {
            JsonValue j;
            return JsonValue::parse(t, j) && HasSupportedStoreSchema(j, kCurrentSessionSchemaVersion) &&
                (!j.has_key("queue") || j.get("queue").is_array());
        });
        if (backupStatus.is_ok() && JsonValue::parse(backupText, root) && root.is_object()) {
            recoveredFromBackup = true;
            parseOk = true;
        } else {
            lastLoadStatus = StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_INVALID_DATA, targetPath, "Malformed JSON in session.json and no valid backup");
            return lastLoadStatus;
        }
    }
    int fileVersion = root.get("schemaVersion").as_int(0);
    if (fileVersion > kCurrentSessionSchemaVersion) {
        lastLoadStatus = StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_REVISION_MISMATCH, targetPath, "Session requires a newer application version");
        return lastLoadStatus;
    }
    loadedSchemaVersion = fileVersion;
    if (fileVersion < kCurrentSessionSchemaVersion) { MigrateSchema(root, fileVersion, kCurrentSessionSchemaVersion); }
    persistedFields = root;
    std::string modeStr = root.get("queueMode").as_string("direct");
    bool isQueue = (modeStr == "queue");
    double rawPos = root.get("playbackPositionSec").as_number(0.0);
    if (std::isnan(rawPos) || std::isinf(rawPos) || rawPos < 0.0) { rawPos = 0.0; }
    playbackPositionSec = rawPos;
    startPositionSec = rawPos;
    std::wstring savedCurrent = root.get("currentFilePath").as_wstring();
    std::wstring resolvedCurrent = ClassifyPlaybackTarget(savedCurrent) == PlaybackTargetKind::LocalFile ? ResolveAppPath(savedCurrent, baseDir) : L"";
    int savedIndex = root.get("currentQueueIndex").as_int(0);
    if (savedIndex < 0) savedIndex = 0;
    std::vector<std::wstring> validQueue;
    if (root.has_key("queue") && root.get("queue").is_array()) {
        const auto& qArr = root.get("queue").as_array();
        size_t cap = std::min(qArr.size(), kMaxQueueSize);
        for (size_t i = 0; i < cap; ++i) {
            if (!qArr[i].is_string()) continue;
            std::wstring rawItem = qArr[i].as_wstring();
            if (ClassifyPlaybackTarget(rawItem) != PlaybackTargetKind::LocalFile) continue;
            std::wstring resolvedItem = ResolveAppPath(rawItem, baseDir);
            if (resolvedItem.empty()) continue;
            if (FileExists(resolvedItem)) { validQueue.push_back(resolvedItem); }
        }
    }
    if (isQueue && !validQueue.empty()) {
        queueMode = QueueMode::Queue;
        playbackQueue = std::move(validQueue);
        bool foundCurrent = false;
        size_t matchIndex = 0;
        if (!resolvedCurrent.empty()) {
            for (size_t i = 0; i < playbackQueue.size(); ++i) {
                if (SameFilePath(playbackQueue[i], resolvedCurrent)) { foundCurrent = true; matchIndex = i; break; }
            }
        }
        if (foundCurrent) { currentQueueIndex = matchIndex; currentFilePath = playbackQueue[matchIndex]; } else {
            currentQueueIndex = std::min((size_t)savedIndex, playbackQueue.size() - 1);
            currentFilePath = playbackQueue[currentQueueIndex];
        }
    } else if (isQueue && validQueue.empty()) {
        queueMode = QueueMode::DirectOpen;
        playbackQueue.clear();
        currentQueueIndex = 0;
        currentFilePath.clear();
        playbackPositionSec = 0.0;
        startPositionSec = 0.0;
    } else {
        queueMode = QueueMode::DirectOpen;
        playbackQueue.clear();
        currentQueueIndex = 0;
        if (!resolvedCurrent.empty() && FileExists(resolvedCurrent)) {
            currentFilePath = resolvedCurrent;
        } else { currentFilePath.clear(); playbackPositionSec = 0.0; startPositionSec = 0.0; }
    }
    state = PlaybackState::Closed;
    lastSavedJson.clear();
    lastLoadStatus = StorageStatus::Ok(targetPath);
    savesAllowed = true;
    return lastLoadStatus;
}

void PlaybackSession::SetQueue(const std::vector<std::wstring>& queue, size_t index) {
    playbackQueue = queue;
    queueMode = playbackQueue.empty() ? QueueMode::DirectOpen : QueueMode::Queue;
    currentQueueIndex = playbackQueue.empty() ? 0 : std::min(index, playbackQueue.size() - 1);
}

bool PlaybackSession::AppendToQueue(const std::vector<std::wstring>& items) {
    if (items.empty()) return false;
    if (queueMode != QueueMode::Queue) {
        queueMode = QueueMode::Queue;
        playbackQueue.clear();
        if (!currentFilePath.empty()) { playbackQueue.push_back(currentFilePath); currentQueueIndex = 0; }
    }
    for (const auto& item : items) {
        if (!item.empty() && playbackQueue.size() < kMaxQueueSize) { playbackQueue.push_back(item); }
    }
    return true;
}

std::wstring PlaybackSession::GetNextQueuePath() {
    if (queueMode != QueueMode::Queue || playbackQueue.empty()) return {};
    currentQueueIndex = (currentQueueIndex + 1) % playbackQueue.size();
    return playbackQueue[currentQueueIndex];
}

std::wstring PlaybackSession::GetPrevQueuePath(bool loop) {
    if (queueMode != QueueMode::Queue || playbackQueue.empty()) return {};
    if (currentQueueIndex > 0) { currentQueueIndex--; } else if (loop) {
        currentQueueIndex = playbackQueue.size() - 1;
    } else { currentQueueIndex = 0; }
    return playbackQueue[currentQueueIndex];
}

std::wstring PlaybackSession::GetFirstQueuePath() {
    if (queueMode != QueueMode::Queue || playbackQueue.empty()) return {};
    currentQueueIndex = 0;
    return playbackQueue.front();
}

bool PlaybackSession::UpdatePathReferences(const std::wstring& oldPath, const std::wstring& newPath) {
    if (oldPath.empty() || newPath.empty()) return false;
    bool changed = false;
    if (SameFilePath(currentFilePath, oldPath)) { currentFilePath = newPath; changed = true; }
    for (auto& path : playbackQueue) {
        if (SameFilePath(path, oldPath)) { path = newPath; changed = true; }
    }
    return changed;
}

bool PlaybackSession::EnterFullscreen(HWND hWnd) {
    if (!hWnd) return false;
    if (presentation == PresentationMode::PictureInPicture) {
        return TransitionPiPToFullscreen(hWnd);
    }
    if (presentation != PresentationMode::Fullscreen) {
        savedStyle = ((DWORD)GetWindowLong(hWnd, GWL_STYLE) | WS_OVERLAPPEDWINDOW) & ~(WS_MAXIMIZE | WS_MINIMIZE);
        savedExStyle = (DWORD)GetWindowLong(hWnd, GWL_EXSTYLE);
        if (savedStyle == 0) savedStyle = (WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS) & ~(WS_MAXIMIZE | WS_MINIMIZE);
        if (savedExStyle == 0) savedExStyle = WS_EX_APPWINDOW;
        savedPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hWnd, &savedPlacement);
        GetWindowRect(hWnd, &savedWindowRect);
        hasSavedWindowInfo = true;
        HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (GetMonitorInfo(hMon, &mi)) {
            SetWindowLong(hWnd, GWL_STYLE, (savedStyle & ~(WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_MAXIMIZE | WS_MINIMIZE)) | WS_POPUP | WS_VISIBLE);
            SetWindowLong(hWnd, GWL_EXSTYLE, (savedExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE)));
            SetWindowPos(hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            presentation = PresentationMode::Fullscreen;
            return true;
        }
    }
    return false;
}

void PlaybackSession::RestoreWindowedPlacement(HWND hWnd) {
    // Restore the saved target directly. Normal-first followed by maximize
    // exposes a smaller window and plays a second composition transition.
    BOOL transitionsDisabled = TRUE;
    DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED,
        &transitionsDisabled, sizeof(transitionsDisabled));
    DWORD restStyle = (savedStyle ? (savedStyle | WS_OVERLAPPEDWINDOW) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS)) & ~(WS_MAXIMIZE | WS_MINIMIZE);
    // SetWindowPlacement restores the normal bounds before maximizing unless
    // the window already carries the maximized state. Preserve that target
    // state through the style swap so even WM_SIZE sees only the final bounds.
    if (savedPlacement.length == sizeof(WINDOWPLACEMENT) &&
        savedPlacement.rcNormalPosition.right > savedPlacement.rcNormalPosition.left &&
        savedPlacement.showCmd == SW_SHOWMAXIMIZED) restStyle |= WS_MAXIMIZE;
    DWORD restExStyle = (savedExStyle ? savedExStyle : WS_EX_APPWINDOW) & ~WS_EX_TOPMOST;
    SetWindowLong(hWnd, GWL_STYLE, restStyle);
    SetWindowLong(hWnd, GWL_EXSTYLE, restExStyle);
    if (savedPlacement.length == sizeof(WINDOWPLACEMENT) && savedPlacement.rcNormalPosition.right > savedPlacement.rcNormalPosition.left) {
        WINDOWPLACEMENT restorePlacement = savedPlacement;
        restorePlacement.showCmd = savedPlacement.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        SetWindowPlacement(hWnd, &restorePlacement);
    } else if (savedWindowRect.right > savedWindowRect.left && savedWindowRect.bottom > savedWindowRect.top) {
        SetWindowPos(hWnd, HWND_NOTOPMOST, savedWindowRect.left, savedWindowRect.top, savedWindowRect.right - savedWindowRect.left,
            savedWindowRect.bottom - savedWindowRect.top, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    }
    // Refresh the frame only after the target bounds are in place.
    SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    transitionsDisabled = FALSE;
    DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED,
        &transitionsDisabled, sizeof(transitionsDisabled));
}

bool PlaybackSession::ExitFullscreen(HWND hWnd) {
    if (!hWnd || presentation != PresentationMode::Fullscreen) return false;
    RestoreWindowedPlacement(hWnd);
    presentation = PresentationMode::Windowed;
    pipPriorPresentation = PresentationMode::Windowed;
    return true;
}

bool PlaybackSession::ToggleFullscreen(HWND hWnd) {
    if (presentation == PresentationMode::Fullscreen) { return ExitFullscreen(hWnd); } else { return EnterFullscreen(hWnd); }
}

bool PlaybackSession::EnterPiP(HWND hWnd, const PiPGeometry& savedGeometry) {
    if (!hWnd) return false;
    if (presentation == PresentationMode::PictureInPicture) return false;

    pipPriorPresentation = presentation;
    bool wasMaximizedOnEntry = false;

    if (pipPriorPresentation != PresentationMode::Fullscreen) {
        savedStyle = ((DWORD)GetWindowLong(hWnd, GWL_STYLE) | WS_OVERLAPPEDWINDOW) & ~(WS_MAXIMIZE | WS_MINIMIZE);
        savedExStyle = (DWORD)GetWindowLong(hWnd, GWL_EXSTYLE);
        if (savedStyle == 0) savedStyle = (WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS) & ~(WS_MAXIMIZE | WS_MINIMIZE);
        if (savedExStyle == 0) savedExStyle = WS_EX_APPWINDOW;
        savedPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hWnd, &savedPlacement);
        GetWindowRect(hWnd, &savedWindowRect);
        hasSavedWindowInfo = true;
        wasMaximizedOnEntry = IsZoomed(hWnd) != FALSE;
    }

    presentation = PresentationMode::PictureInPicture;
    pipPinned = true;

    HMONITOR hMon = nullptr;
    RECT savedRect{};
    PiPGeometry effectiveGeometry = savedGeometry;
    if (effectiveGeometry.width <= 0 || effectiveGeometry.height <= 0) {
        if (lastPiPGeometry.width > 0 && lastPiPGeometry.height > 0) {
            effectiveGeometry = lastPiPGeometry;
        }
    }
    bool hasSavedRect = (effectiveGeometry.width > 0 && effectiveGeometry.height > 0);
    if (hasSavedRect) {
        savedRect.left = effectiveGeometry.x;
        savedRect.top = effectiveGeometry.y;
        savedRect.right = effectiveGeometry.x + effectiveGeometry.width;
        savedRect.bottom = effectiveGeometry.y + effectiveGeometry.height;
        hMon = MonitorFromRect(&savedRect, MONITOR_DEFAULTTONEAREST);
    }
    if (!hMon) {
        hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfo(hMon, &mi)) {
        hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfo(hMon, &mi)) {
            mi.rcWork = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
            mi.rcMonitor = mi.rcWork;
        }
    }

    UINT dpi = 96;
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto getDpi = user32 ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
    if (getDpi) dpi = getDpi(hWnd);
    if (dpi == 0) dpi = 96;

    const int workW = static_cast<int>(mi.rcWork.right - mi.rcWork.left);
    const int workH = static_cast<int>(mi.rcWork.bottom - mi.rcWork.top);

    // Default dimensions: 640x360 logical pixels scaled by current DPI
    int defaultW = MulDiv(640, (int)dpi, 96);
    int defaultH = MulDiv(360, (int)dpi, 96);

    // Minimum dimensions: 300x169 logical pixels scaled by current DPI
    int minW = MulDiv(300, (int)dpi, 96);
    int minH = MulDiv(169, (int)dpi, 96);
    int maxW = std::max(minW, workW);
    int maxH = std::max(minH, workH);

    int pipW = defaultW;
    int pipH = defaultH;
    bool isDefaultPosition = !hasSavedRect || (effectiveGeometry.x == -1 && effectiveGeometry.y == -1);

    if (hasSavedRect) {
        pipW = std::clamp(effectiveGeometry.width, minW, maxW);
        pipH = std::clamp(effectiveGeometry.height, minH, maxH);
    } else {
        pipW = std::clamp(defaultW, minW, maxW);
        pipH = std::clamp(defaultH, minH, maxH);
    }

    int x = 0;
    int y = 0;
    if (isDefaultPosition) {
        x = mi.rcWork.right - pipW;
        y = mi.rcWork.bottom - pipH;
    } else {
        x = effectiveGeometry.x;
        y = effectiveGeometry.y;
        if (x + pipW > mi.rcWork.right) x = mi.rcWork.right - pipW;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y + pipH > mi.rcWork.bottom) y = mi.rcWork.bottom - pipH;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }

    SetWindowLong(hWnd, GWL_STYLE, WS_POPUP | WS_THICKFRAME | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    SetWindowLong(hWnd, GWL_EXSTYLE, (savedExStyle | WS_EX_APPWINDOW | WS_EX_TOPMOST) & ~WS_EX_TOOLWINDOW);
    if (wasMaximizedOnEntry) {
        // Un-maximize straight into the PiP rect. Restoring first plays a visible DWM
        // animation that parks the window at its centered normal rect before the PiP
        // placement applies, so PiP appears to open in the middle of the screen.
        WINDOWPLACEMENT pipPlacement = savedPlacement;
        pipPlacement.showCmd = SW_SHOWNORMAL;
        pipPlacement.rcNormalPosition = { x, y, x + pipW, y + pipH };
        SetWindowPlacement(hWnd, &pipPlacement);
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowPos(hWnd, HWND_TOPMOST, x, y, pipW, pipH, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    }

    // Compensate for DWM invisible resize borders so visible bounds are aligned flush
    RECT extRect = {};
    if (SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &extRect, sizeof(extRect)))) {
        if (isDefaultPosition) {
            // Align visible right and bottom exactly flush with rcWork right/bottom without taskbar-height guesses
            int diffRight = mi.rcWork.right - extRect.right;
            int diffBottom = mi.rcWork.bottom - extRect.bottom;
            if (diffRight != 0 || diffBottom != 0) {
                x += diffRight;
                y += diffBottom;
                SetWindowPos(hWnd, HWND_TOPMOST, x, y, pipW, pipH, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
            }
        } else {
            // Ensure visible bounds stay within the active work area
            int adjX = x;
            int adjY = y;
            if (extRect.right > mi.rcWork.right) {
                adjX -= (extRect.right - mi.rcWork.right);
            }
            if (extRect.left < mi.rcWork.left) {
                adjX += (mi.rcWork.left - extRect.left);
            }
            if (extRect.bottom > mi.rcWork.bottom) {
                adjY -= (extRect.bottom - mi.rcWork.bottom);
            }
            if (extRect.top < mi.rcWork.top) {
                adjY += (mi.rcWork.top - extRect.top);
            }
            if (adjX != x || adjY != y) {
                x = adjX;
                y = adjY;
                SetWindowPos(hWnd, HWND_TOPMOST, x, y, pipW, pipH, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
            }
        }
    }
    // The style swap above can make the system's frame bookkeeping rewrite the window
    // rect once (observed: a one-time asymmetric offset on the first frame change).
    // Assert the requested rect so the placed, saved, and restored geometry all agree.
    RECT placedRect{};
    if (GetWindowRect(hWnd, &placedRect) &&
        (placedRect.left != x || placedRect.top != y ||
         placedRect.right - placedRect.left != pipW || placedRect.bottom - placedRect.top != pipH)) {
        SetWindowPos(hWnd, HWND_TOPMOST, x, y, pipW, pipH, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    RECT finalRect{};
    if (GetWindowRect(hWnd, &finalRect)) {
        lastPiPGeometry = { finalRect.left, finalRect.top, finalRect.right - finalRect.left, finalRect.bottom - finalRect.top };
    }
    return true;
}

bool PlaybackSession::TransitionPiPToFullscreen(HWND hWnd) {
    if (!hWnd || presentation != PresentationMode::PictureInPicture) return false;

    // Snapshot the PiP bounds first; every later step invalidates them. They are the
    // restore point for the next PiP session.
    RECT pipRect{};
    if (!GetWindowRect(hWnd, &pipRect)) return false;
    lastPiPGeometry = { pipRect.left, pipRect.top, pipRect.right - pipRect.left, pipRect.bottom - pipRect.top };

    // Commit the latest PiP frame before changing the window frame so the expand starts
    // from the window's live composed bounds, not a stale pre-drag composition.
    (void)DwmFlush();

    pipPinned = false;
    pipPriorPresentation = PresentationMode::Windowed;

    // Expand onto the monitor that currently holds the PiP window.
    HMONITOR hMon = MonitorFromRect(&pipRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfo(hMon, &mi)) {
        hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfo(hMon, &mi)) {
            mi.rcMonitor = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        }
    }
    const int targetW = mi.rcMonitor.right - mi.rcMonitor.left;
    const int targetH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    DWORD fsStyle = (savedStyle ? (savedStyle & ~(WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_MAXIMIZE | WS_MINIMIZE)) : 0) | WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    DWORD fsExStyle = (savedExStyle ? (savedExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE)) : WS_EX_APPWINDOW) & ~WS_EX_TOPMOST;

    // Apply the style change and the new bounds as ONE window operation with DWM
    // transitions forced off. With transitions left enabled this direction animates
    // position before size: the window visibly slides toward the monitor origin and
    // only then grows to fill it. The opposite direction (fullscreen -> PiP) keeps
    // its native animation.
    BOOL transitionsDisabled = TRUE;
    DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &transitionsDisabled, sizeof(transitionsDisabled));
    SetWindowLong(hWnd, GWL_STYLE, fsStyle);
    SetWindowLong(hWnd, GWL_EXSTYLE, fsExStyle);
    SetWindowPos(hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, targetW, targetH,
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    transitionsDisabled = FALSE;
    DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &transitionsDisabled, sizeof(transitionsDisabled));

    presentation = PresentationMode::Fullscreen;
    return true;
}

bool PlaybackSession::ExitPiP(HWND hWnd) {
    if (!hWnd || presentation != PresentationMode::PictureInPicture) return false;

    RECT pipRect{};
    if (GetWindowRect(hWnd, &pipRect)) {
        lastPiPGeometry = { pipRect.left, pipRect.top, pipRect.right - pipRect.left, pipRect.bottom - pipRect.top };
    }

    pipPinned = false;
    PresentationMode targetPresentation = pipPriorPresentation;
    pipPriorPresentation = PresentationMode::Windowed;

    if (targetPresentation == PresentationMode::Fullscreen) {
        return TransitionPiPToFullscreen(hWnd);
    }

    presentation = PresentationMode::Windowed;
    DWORD restStyle = (savedStyle ? (savedStyle | WS_OVERLAPPEDWINDOW) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS)) & ~(WS_MAXIMIZE | WS_MINIMIZE);
    DWORD restExStyle = (savedExStyle ? savedExStyle : WS_EX_APPWINDOW) & ~WS_EX_TOPMOST;

    // One composition: style swap and saved placement together, with DWM transitions
    // forced off. With the native maximize/restore animation running (or the frame
    // changes split into separate recompositions) the transition interpolates position
    // before size, which reads as the window jumping toward the monitor origin and then
    // growing across the work area in steps. The fullscreen exit target above commits
    // the same way.
    BOOL transitionsDisabled = TRUE;
    DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &transitionsDisabled, sizeof(transitionsDisabled));
    SetWindowLong(hWnd, GWL_STYLE, restStyle);
    SetWindowLong(hWnd, GWL_EXSTYLE, restExStyle);
    SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (savedPlacement.length == sizeof(WINDOWPLACEMENT) && savedPlacement.rcNormalPosition.right > savedPlacement.rcNormalPosition.left) {
        // Apply the saved placement in one step: restoring normal-first and then maximizing
        // would play two visible DWM animations instead of a single direct commit.
        WINDOWPLACEMENT restorePlacement = savedPlacement;
        restorePlacement.showCmd = savedPlacement.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        SetWindowPlacement(hWnd, &restorePlacement);
    } else if (savedWindowRect.right > savedWindowRect.left && savedWindowRect.bottom > savedWindowRect.top) {
        int width = savedWindowRect.right - savedWindowRect.left;
        int height = savedWindowRect.bottom - savedWindowRect.top;
        SetWindowPos(hWnd, HWND_NOTOPMOST, savedWindowRect.left, savedWindowRect.top, width, height, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    }
    SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    transitionsDisabled = FALSE;
    DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &transitionsDisabled, sizeof(transitionsDisabled));
    return true;
}

bool PlaybackSession::TogglePiP(HWND hWnd, const PiPGeometry& savedGeometry) {
    if (presentation == PresentationMode::PictureInPicture) { return ExitPiP(hWnd); } else { return EnterPiP(hWnd, savedGeometry); }
}

bool PlaybackSession::TogglePiPPinned(HWND hWnd) {
    if (!hWnd || presentation != PresentationMode::PictureInPicture) return false;
    pipPinned = !pipPinned;
    SetWindowLong(hWnd, GWL_EXSTYLE,
        pipPinned ? (GetWindowLong(hWnd, GWL_EXSTYLE) | WS_EX_TOPMOST) : (GetWindowLong(hWnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST));
    SetWindowPos(hWnd, pipPinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return true;
}

void PlaybackSession::ResetPresentation(HWND hWnd) {
    if (presentation == PresentationMode::Fullscreen || presentation == PresentationMode::PictureInPicture) {
        presentation = PresentationMode::Windowed;
        pipPinned = false;
        pipPriorPresentation = PresentationMode::Windowed;
        if (hWnd) RestoreWindowedPlacement(hWnd);
    }
    pipPinned = false;
    pipPriorPresentation = PresentationMode::Windowed;
}

} // namespace Aperture
