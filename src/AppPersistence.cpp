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


bool App::SaveSettings() {
    StorageStatus status = settings.Save();
    if (!status.is_ok()) { ReportStorageFailure(status, L"settings"); return false; }
    return true;
}


bool App::SaveLibrary() {
    // Never serialize the placeholder library. If startup loading failed or a close arrived before publication, the valid/corrupt source
    // stores must remain untouched for recovery on the next launch.
    if (!libraryReady) return true;
    MediaLibrarySaveResult res = library.Save();
    if (!res.is_ok()) {
        if (!res.libraryStatus.is_ok()) {
            ReportStorageFailure(res.libraryStatus, L"media library");
        } else if (!res.playlistsStatus.is_ok()) {
            ReportStorageFailure(res.playlistsStatus, L"playlists");
        } else if (!res.historyStatus.is_ok()) { ReportStorageFailure(res.historyStatus, L"history"); } else {
            StorageStatus err = StorageStatus::Error(StorageErrorCode::FileOpenFailed, 0, GetDataDir(), "Failed to save library data");
            ReportStorageFailure(err, L"media library");
        }
        return false;
    }
    return true;
}


bool App::SaveSession() {
    if (playbackSession.IsOpen()) {
        if (!player.GetCurrentFilePath().empty()) {
            double curPos = player.GetCurrentTime();
            if (curPos >= 0.0 && std::isfinite(curPos)) { playbackSession.SetPlaybackPosition(curPos); }
        }
    }
    StorageStatus status = playbackSession.Save();
    if (!status.is_ok()) { ReportStorageFailure(status, L"playback session"); return false; }
    return true;
}


bool App::SaveAll() {
    bool okSettings = SaveSettings();
    bool okLibrary = SaveLibrary();
    bool okSession = SaveSession();
    return okSettings && okLibrary && okSession;
}


void App::ReportStorageFailure(const StorageStatus& status, const std::wstring& context) {
    const ULONGLONG now = GetTickCount64();
    const std::wstring errorKey = status.path + L":" + std::to_wstring(static_cast<int>(status.code)) + L":" + std::to_wstring(status.win32Error);
    if (errorKey == lastReportedStorageError && (now - lastStorageErrorReportTimeMs) < 4000) { return; }
    lastReportedStorageError = errorKey;
    lastStorageErrorReportTimeMs = now;
    std::wstring displayPath = status.path.empty() ? GetDataDir() : status.path;
    const std::wstring osdMsg = Presentation::StorageGuidance(status);
    ReportAppError(AppError::Storage( static_cast<int>(status.code), status.win32Error, osdMsg, Utf8ToUtf16(status.message), displayPath));
    // Do not replace a dialog in progress or interrupt active playback.
    if (appInitialized && !playbackSession.IsOpen() && activeModal == ModalType::None && !shutdownComplete) {
        ShowConfirmationModal(Loc::Tr("ux.storage_title"), osdMsg, Loc::Tr("ux.open_folder"), [this]() {
            const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(hWnd, L"open", GetExeDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32) ShowOSD(IconType::FolderPlaylist, Loc::Tr("ux.storage_retry"), 6.0f);
        }, nullptr, false);
    }
}


void App::ReportAppError(const AppError& error, float durationSec) {
    Diagnostics::Log(error);
    if (!error.IsValid() || shutdownComplete) return;
    std::wstring message = error.userMessage;
    if (message.empty()) { message = std::wstring(ErrorDomainName(error.domain)) + L" error"; }
    ShowOSD(error.severity == ErrorSeverity::Information ? IconType::Checkmark :
            error.domain == ErrorDomain::Storage ? IconType::FolderPlaylist : IconType::MediaFilm, message, durationSec);
}


void App::SetDisplayPowerRequest(bool enable) {
    if (displayPowerRequestActive != enable) {
        if (enable) {
            SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
            displayPowerRequestActive = true;
        } else { SetThreadExecutionState(ES_CONTINUOUS); displayPowerRequestActive = false; }
    }
}


void App::UpdateDisplayPowerRequest() {
    const bool shouldHold = playbackSession.IsOpen() && player.IsPlaying() && !isMinimized && !IsIconic(hWnd) && !shutdownComplete;
    SetDisplayPowerRequest(shouldHold);
}


void App::ApplyWindowTheme() {
    if (!hWnd) return;
    // Rebuild the persisted user palette first. This makes leaving high contrast restore the selected Settings theme/accent without
    // mutating either persisted setting.
    settings.UpdateThemeColors();
    const auto& visual = systemVisualPreferences.Get();
    if (visual.highContrast) {
        const auto opaque = [](COLORREF color) {
            return D2D1::ColorF( static_cast<float>(GetRValue(color)) / 255.0f, static_cast<float>(GetGValue(color)) / 255.0f,
                static_cast<float>(GetBValue(color)) / 255.0f, 1.0f);
        };
        // High contrast uses opaque system colors for every token consumed by Renderer/UIComponents. Avoid translucent blends that erase
        // the user's configured contrast ratio.
        settings.currentTheme.bg = opaque(visual.colors.background);
        settings.currentTheme.cardBg = opaque(visual.colors.background);
        settings.currentTheme.iconBg = opaque(visual.colors.background);
        settings.currentTheme.trackBg = opaque(visual.colors.border);
        settings.currentTheme.border = opaque(visual.colors.border);
        settings.currentTheme.accent = opaque(visual.colors.highlight);
        settings.currentTheme.accentDark = opaque(visual.colors.highlight);
        settings.currentTheme.accentSubtle = opaque(visual.colors.highlight);
        settings.currentTheme.text = opaque(visual.colors.text);
        settings.currentTheme.textMuted = opaque(visual.colors.disabled);
        settings.currentTheme.red = opaque(visual.colors.highlight);
        // Semantic surface tokens
        settings.currentTheme.surfaceElevated = opaque(visual.colors.background);
        settings.currentTheme.surfaceFloating = opaque(visual.colors.background);
        settings.currentTheme.surfaceModal = opaque(visual.colors.background);
        settings.currentTheme.scrim = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.85f);
        settings.currentTheme.focusRing = opaque(visual.colors.highlight);
        settings.currentTheme.borderSubtle = opaque(visual.colors.border);
        settings.currentTheme.surfaceHover = opaque(visual.colors.background);
        settings.currentTheme.surfaceSelected = opaque(visual.colors.highlight);
    }
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
    COLORREF captionColor = RGB( (BYTE)(settings.currentTheme.bg.r * 255.0f), (BYTE)(settings.currentTheme.bg.g * 255.0f),
        (BYTE)(settings.currentTheme.bg.b * 255.0f) );
    DwmSetWindowAttribute(hWnd, 35 /* DWMWA_CAPTION_COLOR */, &captionColor, sizeof(captionColor));
    COLORREF textColor = RGB( (BYTE)(settings.currentTheme.text.r * 255.0f), (BYTE)(settings.currentTheme.text.g * 255.0f),
        (BYTE)(settings.currentTheme.text.b * 255.0f) );
    DwmSetWindowAttribute(hWnd, 36 /* DWMWA_TEXT_COLOR */, &textColor, sizeof(textColor));
    // Keep the pre-first-frame erase brush in sync with the active theme so the opening never flashes a stale or white background.
    if (hStartupBgBrush) DeleteObject(hStartupBgBrush);
    hStartupBgBrush = CreateSolidBrush(D2DColorToColorRef(settings.currentTheme.bg));
    ApplyTooltipTheme();
}


void App::ApplyTooltipTheme() {
    const bool highContrast = systemVisualPreferences.Get().highContrast;
    const auto& bg = settings.currentTheme.bg;
    const float luminance = 0.2126f * bg.r + 0.7152f * bg.g + 0.0722f * bg.b;
    const bool darkMode = luminance < 0.52f;
    if (hubTooltips.IsCreated()) hubTooltips.SetTheme(darkMode, highContrast);
    if (playerTooltips.IsCreated()) playerTooltips.SetTheme(darkMode, highContrast);
}


void App::PersistWindowPlacement() {
    if (!hWnd || inPresentationTransition || playbackSession.IsFullscreen() || playbackSession.IsPiP()) return;
    WINDOWPLACEMENT placement = { sizeof(WINDOWPLACEMENT) };
    if (!GetWindowPlacement(hWnd, &placement)) return;
    const bool minimized = placement.showCmd == SW_SHOWMINIMIZED || placement.showCmd == SW_MINIMIZE ||
                           placement.showCmd == SW_FORCEMINIMIZE;
    settings.windowMaximized = placement.showCmd == SW_SHOWMAXIMIZED || IsZoomed(hWnd);
    if (minimized || settings.windowMaximized) return; // Preserve restore rect while maximized.
    RECT windowRect{};
    if (!GetWindowRect(hWnd, &windowRect)) return;
    settings.windowX = windowRect.left;
    settings.windowY = windowRect.top;
    settings.windowWidth = std::max(1L, windowRect.right - windowRect.left);
    settings.windowHeight = std::max(1L, windowRect.bottom - windowRect.top);
}


void App::PersistPiPGeometry() {
    if (!hWnd || inPresentationTransition || !playbackSession.IsPiP()) return;
    RECT windowRect{};
    if (!GetWindowRect(hWnd, &windowRect)) return;
    int w = windowRect.right - windowRect.left;
    int h = windowRect.bottom - windowRect.top;
    if (w > 0 && h > 0) {
        settings.pipX = windowRect.left;
        settings.pipY = windowRect.top;
        settings.pipWidth = w;
        settings.pipHeight = h;
        playbackSession.SetLastPiPGeometry({ settings.pipX, settings.pipY, settings.pipWidth, settings.pipHeight });
    }
}

} // namespace Aperture
