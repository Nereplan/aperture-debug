#pragma once

#include "Common.h"
#include "Json.h"
#include "MediaLibrary.h"
#include "PlaybackTarget.h"
#include <optional>
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>

namespace Aperture {

enum class PlaybackState { Closed, Initializing, Playing, Paused, Error };

enum class PresentationMode { Windowed, Fullscreen, PictureInPicture };

enum class QueueMode { DirectOpen, Queue };

struct PiPGeometry {
    int x = -1;
    int y = -1;
    int width = 0;
    int height = 0;
};

class PlaybackSession {
public:
    PlaybackSession();
    ~PlaybackSession() = default;
    static constexpr int kCurrentSessionSchemaVersion = 1;
    static constexpr size_t kMaxQueueSize = 1000;
    static bool MigrateSchema(JsonValue& root, int fromVersion, int targetVersion = kCurrentSessionSchemaVersion);
    StorageStatus Load(const std::wstring& customPath = L"", const std::wstring& baseDir = GetExeDir());
    StorageStatus Save(const std::wstring& customPath = L"", const std::wstring& baseDir = GetExeDir());
    bool WasRecoveredFromBackup() const { return recoveredFromBackup; }
    int GetLoadedSchemaVersion() const { return loadedSchemaVersion; }
    StorageStatus GetLastLoadStatus() const { return lastLoadStatus; }
    void Reset();
    // State Transitions
    bool OpenDirect(const std::wstring& filePath, double startPosSec = 0.0);
    bool OpenQueue(const std::vector<std::wstring>& items, size_t startIndex = 0);
    bool OpenPreserveQueue(const std::wstring& filePath, double startPosSec = 0.0);
    bool OnPlayerOpened(bool isPlaying = true);
    bool OnPlayerError(const std::wstring& errorMsg);
    bool SetPlayState(bool isPlaying);
    bool OnEof(bool autoplayEnabled, std::wstring& outNextPath, bool& outIsQueue);
    void Close();
    // Queue Navigation
    bool HasQueue() const { return queueMode == QueueMode::Queue && !playbackQueue.empty(); }
    const std::vector<std::wstring>& GetQueue() const { return playbackQueue; }
    size_t GetQueueIndex() const { return currentQueueIndex; }
    size_t GetQueueSize() const { return playbackQueue.size(); }
    void SetQueue(const std::vector<std::wstring>& queue, size_t index = 0);
    bool AppendToQueue(const std::vector<std::wstring>& items);
    std::wstring GetNextQueuePath();
    std::wstring GetPrevQueuePath(bool loop);
    std::wstring GetFirstQueuePath();
    bool UpdatePathReferences(const std::wstring& oldPath, const std::wstring& newPath);
    // Presentation Mode & Window Placement
    void SetReduceMotion(bool enabled) { reduceMotion = enabled; }
    PresentationMode GetPresentationMode() const { return presentation; }
    bool IsFullscreen() const { return presentation == PresentationMode::Fullscreen; }
    bool IsPiP() const { return presentation == PresentationMode::PictureInPicture; }
    bool IsPiPPinned() const { return IsPiP() && pipPinned; }
    bool IsWindowed() const { return presentation == PresentationMode::Windowed; }
    bool EnterFullscreen(HWND hWnd);
    bool ExitFullscreen(HWND hWnd);
    bool ToggleFullscreen(HWND hWnd);
    bool EnterPiP(HWND hWnd, const PiPGeometry& savedGeometry = {});
    bool ExitPiP(HWND hWnd);
    bool TogglePiP(HWND hWnd, const PiPGeometry& savedGeometry = {});
    bool TogglePiPPinned(HWND hWnd);
    bool TransitionPiPToFullscreen(HWND hWnd);
    void ResetPresentation(HWND hWnd);
    PresentationMode GetPriorPresentationMode() const { return pipPriorPresentation; }
    const PiPGeometry& GetLastPiPGeometry() const { return lastPiPGeometry; }
    void SetLastPiPGeometry(const PiPGeometry& geom) { lastPiPGeometry = geom; }
    // Queries & State Inspection
    PlaybackState GetState() const { return state; }
    QueueMode GetQueueMode() const { return queueMode; }
    bool IsOpen() const { return state != PlaybackState::Closed && state != PlaybackState::Error; }
    bool IsClosed() const { return state == PlaybackState::Closed; }
    bool IsInitializing() const { return state == PlaybackState::Initializing; }
    bool IsPlaying() const { return state == PlaybackState::Playing; }
    bool IsPaused() const { return state == PlaybackState::Paused; }
    bool IsError() const { return state == PlaybackState::Error; }
    bool IsQueuePlayback() const { return queueMode == QueueMode::Queue; }
    const std::wstring& GetCurrentFilePath() const { return currentFilePath; }
    const std::wstring& GetCurrentError() const { return currentError; }
    double GetStartPos() const { return startPositionSec; }
    double GetPlaybackPosition() const { return playbackPositionSec; }
    void SetPlaybackPosition(double posSec);
    // Track & Media Properties State Cache
    int GetLastActiveSubtitleTrackId() const { return lastActiveSubtitleTrackId; }
    void SetLastActiveSubtitleTrackId(int id) { lastActiveSubtitleTrackId = id; }
    const std::wstring& GetLastActiveSubtitleTrackTitle() const { return lastActiveSubtitleTrackTitle; }
    void SetLastActiveSubtitleTrackTitle(const std::wstring& title) { lastActiveSubtitleTrackTitle = title; }
    int GetLastActiveAudioTrackId() const { return lastActiveAudioTrackId; }
    void SetLastActiveAudioTrackId(int id) { lastActiveAudioTrackId = id; }
    const std::wstring& GetLastActiveAudioTrackTitle() const { return lastActiveAudioTrackTitle; }
    void SetLastActiveAudioTrackTitle(const std::wstring& title) { lastActiveAudioTrackTitle = title; }
    // Active Media Playback Preferences Cache
    const std::optional<MediaPlaybackPreferences>& GetActivePreferences() const { return activePreferences; }
    void SetActivePreferences(const std::optional<MediaPlaybackPreferences>& prefs) { activePreferences = prefs; }
    void ClearActivePreferences() { activePreferences.reset(); }
    double GetPriorUnmutedVolume() const { return priorUnmutedVolume; }
    void SetPriorUnmutedVolume(double vol) { if (vol > 0.01) priorUnmutedVolume = vol; }
    // Private Playback Session Mode
    bool IsPrivateMode() const { return isPrivateMode; }
    PlaybackPersistencePolicy PersistencePolicy() const { return {isPrivateMode}; }
    bool CanPersistPlayback() const { return PersistencePolicy().CanWritePlayback(); }
    bool CanPersistLocalTarget(const std::wstring& path) const { return PersistencePolicy().CanWriteLocalTarget(path); }
    bool CanPersistRecentStreams(bool optedIn) const { return PersistencePolicy().CanWriteRecentStreams(optedIn); }
    void SetPrivateMode(bool enabled) { isPrivateMode = enabled; }
    void TogglePrivateMode() { isPrivateMode = !isPrivateMode; }
    // Helpers
    static bool SameFilePath(const std::wstring& a, const std::wstring& b);

private:
    void RestoreWindowedPlacement(HWND hWnd);
    PlaybackState state = PlaybackState::Closed;
    PresentationMode presentation = PresentationMode::Windowed;
    QueueMode queueMode = QueueMode::DirectOpen;
    std::wstring currentFilePath, currentError;
    double startPositionSec = 0.0, playbackPositionSec = 0.0;
    // Serialization & Storage State
    std::string lastSavedJson;
    JsonValue persistedFields;
    int loadedSchemaVersion = 0;
    bool recoveredFromBackup = false;
    StorageStatus lastSaveStatus;
    StorageStatus lastLoadStatus;
    bool savesAllowed = true;
    std::vector<std::wstring> playbackQueue;
    size_t currentQueueIndex = 0;
    // Track Cache
    int lastActiveSubtitleTrackId = 0;
    std::wstring lastActiveSubtitleTrackTitle;
    int lastActiveAudioTrackId = 0;
    std::wstring lastActiveAudioTrackTitle;
    double priorUnmutedVolume = 1.0;
    std::optional<MediaPlaybackPreferences> activePreferences;
    bool isPrivateMode = false, pipPinned = false;
    // Saved Window Configuration for Presentation Transitions
    WINDOWPLACEMENT savedPlacement = { sizeof(WINDOWPLACEMENT) };
    RECT savedWindowRect = { 0 };
    DWORD savedStyle = 0, savedExStyle = 0;
    bool hasSavedWindowInfo = false;
    PresentationMode pipPriorPresentation = PresentationMode::Windowed;
    PiPGeometry lastPiPGeometry{};
    bool reduceMotion = false;
};

} // namespace Aperture
