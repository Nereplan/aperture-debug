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


void App::TriggerShortcut(const std::string& actionId) { commandDispatcher.Execute(actionId); }


void App::OpenShortcutOverlay() {
    PushFocusSnapshot(TransientSurfaceType::ShortcutOverlay);
    contextMenuItems.clear();
    const auto shortcutFor = [this](const std::string& actionId, const wchar_t* fallback) {
        const auto it = settings.shortcuts.find(actionId);
        if (it == settings.shortcuts.end()) return std::wstring(fallback);
        const std::wstring formatted = Settings::FormatShortcutKey(it->second);
        return formatted.empty() ? std::wstring(fallback) : formatted;
    };
    const auto addHint = [this, &shortcutFor](const wchar_t* label, const std::string& actionId, const wchar_t* fallback, IconType icon) {
        contextMenuItems.push_back({ "shortcut_" + actionId, label, shortcutFor(actionId, fallback),
            icon, false, false, false, nullptr, true });
    };
    if (playbackSession.IsOpen()) {
        addHint(L"Play / Pause", "togglePlay", L"Space", IconType::Play);
        addHint(L"Seek Back / Forward", "seekForward", L"\u2190 / \u2192", IconType::Refresh);
        addHint(L"Mute", "toggleMute", L"M", IconType::VolumeMuted);
        addHint(L"Subtitles", "cycleSubtitles", L"S", IconType::Subtitles);
        addHint(L"Fullscreen", "toggleFullscreen", L"F", IconType::Fullscreen);
        addHint(L"Close Player", "closePlayer", L"Esc", IconType::CloseX);
    } else {
        addHint(L"Open File", "openFile", L"Ctrl+O", IconType::Plus);
        addHint(L"Search", "globalSearch", L"Ctrl+F", IconType::Search);
        addHint(L"Select All", "selectAll", L"Ctrl+A", IconType::Checkmark);
        addHint(L"Remove Selected", "deleteSelected", L"Delete", IconType::Trash);
        addHint(L"Switch Tabs", "switchTabs", L"Ctrl+1\u20134", IconType::FolderPlaylist);
    }
    RECT rc{};
    GetClientRect(playbackSession.IsOpen() && hOverlayWnd ? hOverlayWnd : hWnd, &rc);
    const float dp = playbackSession.IsOpen() ? overlayRenderer.GetDpiScale() : renderer.GetDpiScale();
    const float menuW = 220.0f * dp;
    const float menuH = (8.0f + contextMenuItems.size() * 32.0f) * dp;
    contextMenuOrigin = {std::max(8.0f * dp, (static_cast<float>(rc.right - rc.left) - menuW) * 0.5f),
                         std::max(8.0f * dp, (static_cast<float>(rc.bottom - rc.top) - menuH) * 0.5f)};
    hoveredContextMenuItem = -1;
    contextMenuAnim.SnapTo(0.0f);
    AnimateVisual(contextMenuAnim, 1.0f, MotionTokens::kFast, MotionTokens::Standard());
    isContextMenuOpen = true;
    contextMenuClosing = false;
    accessibilityDirty = true;
    if (playbackSession.IsOpen()) Render();
    else InvalidateRect(hWnd, NULL, FALSE);
}


// Centered overlay surface reusing the shortcut-sheet mechanism: plain
// contextMenuItems presented through the shared context-menu renderer, closed
// by the backdrop click / Esc handling that surface already owns.
void App::OpenInternetRadioContextMenu() {
    PushFocusSnapshot(TransientSurfaceType::ShortcutOverlay);
    contextMenuItems.clear();
    contextMenuItems.push_back({ "radio_title", L"Internet Radio (click to play)", L"", IconType::Play, false, false, false, nullptr, true });
    contextMenuItems.push_back({ "sep_radio1", L"", L"", IconType::Play, true });
    radioJobs.EnsureFetch();
    if (radioJobs.GetFetchState() == RadioDirectoryJobs::FetchState::Loading) {
        contextMenuItems.push_back({ "radio_loading", L"Loading stations...", L"", IconType::Play, false, false, false, nullptr, true });
    } else {
        const RadioDirectoryResult& result = radioJobs.GetResult();
        if (result.stale) ShowOSD(IconType::VolumeMuted, L"Radio directory unavailable");
        if (!result.success) {
            contextMenuItems.push_back({ "radio_err", L"Radio directory unavailable", L"", IconType::VolumeMuted, false, false, false, nullptr, true });
        } else {
            // The centered overlay menu has no scrolling; cap the visible
            // stations to the rows that fit the window height.
            RECT fitRc{};
            GetClientRect(hWnd, &fitRc);
            const float fitDp = renderer.GetDpiScale();
            const int maxVisible = std::max(4, static_cast<int>((static_cast<float>(fitRc.bottom - fitRc.top) * 0.9f / fitDp - 48.0f) / 32.0f));
            const size_t count = std::min(result.stations.size(), static_cast<size_t>(maxVisible));
            for (size_t i = 0; i < count; ++i) {
                const RadioStation& station = result.stations[i];
                std::wstring label = station.name;
                if (station.bitrateKbps > 0 || !station.codec.empty()) {
                    label += L" \u2014 ";
                    if (station.bitrateKbps > 0) label += std::to_wstring(station.bitrateKbps) + L" kbps";
                    if (!station.codec.empty()) {
                        if (station.bitrateKbps > 0) label += L" ";
                        label += station.codec;
                    }
                }
                contextMenuItems.push_back({ "radio_" + std::to_string(i), label, L"", IconType::Play, false, false, false,
                    [this, url = station.url]() {
                        // Same path the network-stream modal uses: URL validation, recent-stream
                        // bookkeeping, then the shared open-and-play entry point.
                        NetworkStreamUrl parsed = NetworkStreamValidator::ValidateAndParse(url);
                        if (!parsed.isValid) {
                            ShowOSD(IconType::VolumeMuted, NetworkStreamValidator::GetSafeErrorMessage(parsed.error, url), 4.0f);
                            return;
                        }
                        if (playbackSession.CanPersistRecentStreams(networkStreamModel.IsRecentStreamsEnabled())) {
                            networkStreamModel.AddRecentStream(parsed.rawUrl);
                            networkStreamModel.SaveRecentStreams(GetExeDir());
                        }
                        OpenAndPlayMedia(parsed.rawUrl);
                    }, false, false, true});
            }
        }
    }
    RECT rc{};
    GetClientRect(playbackSession.IsOpen() && hOverlayWnd ? hOverlayWnd : hWnd, &rc);
    const float dp = playbackSession.IsOpen() ? overlayRenderer.GetDpiScale() : renderer.GetDpiScale();
    const float menuW = 220.0f * dp;
    const float menuH = (8.0f + contextMenuItems.size() * 32.0f) * dp;
    contextMenuOrigin = {std::max(8.0f * dp, (static_cast<float>(rc.right - rc.left) - menuW) * 0.5f),
                         std::max(8.0f * dp, (static_cast<float>(rc.bottom - rc.top) - menuH) * 0.5f)};
    hoveredContextMenuItem = -1;
    contextMenuAnim.SnapTo(0.0f);
    AnimateVisual(contextMenuAnim, 1.0f, MotionTokens::kFast, MotionTokens::Standard());
    isContextMenuOpen = true;
    contextMenuClosing = false;
    accessibilityDirty = true;
    if (playbackSession.IsOpen()) Render();
    else InvalidateRect(hWnd, NULL, FALSE);
}


// Centered overlay surface for podcast subscriptions, reusing the internet-radio
// sheet mechanism: plain contextMenuItems presented through the shared
// context-menu renderer, closed by the backdrop click / Esc handling that
// surface already owns.
void App::OpenPodcastsContextMenu() {
    PushFocusSnapshot(TransientSurfaceType::ShortcutOverlay);
    contextMenuItems.clear();
    contextMenuItems.push_back({ "pod_title", L"Podcasts", L"", IconType::Play, false, false, false, nullptr, true });
    contextMenuItems.push_back({ "sep_pod1", L"", L"", IconType::Play, true });
    std::vector<std::wstring> feedUrls;
    for (const auto& feed : podcastSubscriptions.GetFeeds()) feedUrls.push_back(feed.url);
    // Subscriptions persist like playlists, but the network refresh is skipped
    // while private mode is active (an existing parsed result stays browsable).
    // Nothing to fetch with no subscriptions: skip the job so the empty surface
    // stays silent.
    if (!feedUrls.empty() && !playbackSession.IsPrivateMode()) podcastJobs.EnsureRefresh(feedUrls);
    if (podcastJobs.IsRunning()) {
        contextMenuItems.push_back({ "pod_loading", L"Refreshing feeds...", L"", IconType::Play, false, false, false, nullptr, true });
    } else {
        const auto& feeds = podcastSubscriptions.GetFeeds();
        if (feeds.empty()) {
            contextMenuItems.push_back({ "pod_none", L"No podcast subscriptions", L"", IconType::Play, false, false, false, nullptr, true });
        } else {
            // The centered overlay menu has no scrolling; cap the visible
            // feeds to the rows that fit the window height.
            RECT fitRc{};
            GetClientRect(hWnd, &fitRc);
            const float fitDp = renderer.GetDpiScale();
            const int maxVisible = std::max(4, static_cast<int>((static_cast<float>(fitRc.bottom - fitRc.top) * 0.9f / fitDp - 48.0f) / 32.0f));
            const size_t count = std::min(feeds.size(), static_cast<size_t>(maxVisible));
            for (size_t i = 0; i < count; ++i) {
                contextMenuItems.push_back({ "pod_" + std::to_string(i), feeds[i].title, L"", IconType::Play, false, false, false,
                    [this, i]() { OpenPodcastEpisodesContextMenu(i); }, false, false, true});
            }
        }
    }
    contextMenuItems.push_back({ "sep_pod2", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({ "pod_add", L"Add Podcast by URL...", L"", IconType::Plus, false, false, false,
        [this]() { ShowAddPodcastModal(); }, false, false, true});
    RECT rc{};
    GetClientRect(playbackSession.IsOpen() && hOverlayWnd ? hOverlayWnd : hWnd, &rc);
    const float dp = playbackSession.IsOpen() ? overlayRenderer.GetDpiScale() : renderer.GetDpiScale();
    const float menuW = 220.0f * dp;
    const float menuH = (8.0f + contextMenuItems.size() * 32.0f) * dp;
    contextMenuOrigin = {std::max(8.0f * dp, (static_cast<float>(rc.right - rc.left) - menuW) * 0.5f),
                         std::max(8.0f * dp, (static_cast<float>(rc.bottom - rc.top) - menuH) * 0.5f)};
    hoveredContextMenuItem = -1;
    contextMenuAnim.SnapTo(0.0f);
    AnimateVisual(contextMenuAnim, 1.0f, MotionTokens::kFast, MotionTokens::Standard());
    isContextMenuOpen = true;
    contextMenuClosing = false;
    accessibilityDirty = true;
    if (playbackSession.IsOpen()) Render();
    else InvalidateRect(hWnd, NULL, FALSE);
}


// Second-level page: the episodes of one subscription, parsed feeds coming from
// the shared refresh result (matched by URL). Clicking an episode plays it
// through the same path the network-stream modal uses.
void App::OpenPodcastEpisodesContextMenu(size_t feedIndex) {
    const auto& feeds = podcastSubscriptions.GetFeeds();
    if (feedIndex >= feeds.size()) return;
    const std::wstring feedTitle = feeds[feedIndex].title;
    const std::wstring feedUrl = feeds[feedIndex].url;
    const PodcastFeed* parsed = nullptr;
    for (const auto& feed : podcastJobs.GetResult().feeds) {
        if (feed.url == feedUrl) { parsed = &feed; break; }
    }
    PushFocusSnapshot(TransientSurfaceType::ShortcutOverlay);
    contextMenuItems.clear();
    contextMenuItems.push_back({ "pep_title", feedTitle, L"", IconType::Play, false, false, false, nullptr, true });
    contextMenuItems.push_back({ "sep_pep1", L"", L"", IconType::Play, true });
    if (!parsed || parsed->episodes.empty()) {
        contextMenuItems.push_back({ "pep_none", L"No episodes available", L"", IconType::VolumeMuted, false, false, false, nullptr, true });
    } else {
        // Same fit cap as the radio sheet: no scrolling on the overlay menu.
        RECT fitRc{};
        GetClientRect(hWnd, &fitRc);
        const float fitDp = renderer.GetDpiScale();
        const int maxVisible = std::max(4, static_cast<int>((static_cast<float>(fitRc.bottom - fitRc.top) * 0.9f / fitDp - 48.0f) / 32.0f));
        const size_t count = std::min(parsed->episodes.size(), static_cast<size_t>(maxVisible));
        for (size_t i = 0; i < count; ++i) {
            const PodcastEpisode& episode = parsed->episodes[i];
            std::wstring label = episode.title;
            if (episode.publishedSec > 0) {
                std::tm tmValue{};
                if (gmtime_s(&tmValue, &episode.publishedSec) == 0) {
                    wchar_t dateBuf[16];
                    swprintf_s(dateBuf, L"%04d-%02d-%02d", tmValue.tm_year + 1900, tmValue.tm_mon + 1, tmValue.tm_mday);
                    label += L" \u2014 ";
                    label += dateBuf;
                }
            }
            contextMenuItems.push_back({ "pep_" + std::to_string(i), label, L"", IconType::Play, false, false, false,
                [this, url = episode.url]() {
                    // Same path the network-stream modal uses: URL validation, recent-stream
                    // bookkeeping, then the shared open-and-play entry point.
                    NetworkStreamUrl parsedUrl = NetworkStreamValidator::ValidateAndParse(url);
                    if (!parsedUrl.isValid) {
                        ShowOSD(IconType::VolumeMuted, NetworkStreamValidator::GetSafeErrorMessage(parsedUrl.error, url), 4.0f);
                        return;
                    }
                    if (playbackSession.CanPersistRecentStreams(networkStreamModel.IsRecentStreamsEnabled())) {
                        networkStreamModel.AddRecentStream(parsedUrl.rawUrl);
                        networkStreamModel.SaveRecentStreams(GetExeDir());
                    }
                    OpenAndPlayMedia(parsedUrl.rawUrl);
                }, false, false, true});
        }
    }
    contextMenuItems.push_back({ "sep_pep2", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({ "pdel", L"Remove This Podcast", L"", IconType::Trash, false, false, false, [this, feedUrl]() {
        podcastSubscriptions.Remove(feedUrl);
        podcastSubscriptions.Save(GetDataDir());
        ShowOSD(IconType::Trash, L"Podcast removed");
        OpenPodcastsContextMenu();
    }, false, false, true});
    contextMenuItems.push_back({ "pback", L"Back", L"", IconType::ChevronLeft, false, false, false, [this]() {
        OpenPodcastsContextMenu();
    }, false, false, true});
    RECT rc{};
    GetClientRect(playbackSession.IsOpen() && hOverlayWnd ? hOverlayWnd : hWnd, &rc);
    const float dp = playbackSession.IsOpen() ? overlayRenderer.GetDpiScale() : renderer.GetDpiScale();
    const float menuW = 220.0f * dp;
    const float menuH = (8.0f + contextMenuItems.size() * 32.0f) * dp;
    contextMenuOrigin = {std::max(8.0f * dp, (static_cast<float>(rc.right - rc.left) - menuW) * 0.5f),
                         std::max(8.0f * dp, (static_cast<float>(rc.bottom - rc.top) - menuH) * 0.5f)};
    hoveredContextMenuItem = -1;
    contextMenuAnim.SnapTo(0.0f);
    AnimateVisual(contextMenuAnim, 1.0f, MotionTokens::kFast, MotionTokens::Standard());
    isContextMenuOpen = true;
    contextMenuClosing = false;
    accessibilityDirty = true;
    if (playbackSession.IsOpen()) Render();
    else InvalidateRect(hWnd, NULL, FALSE);
}


// Add-by-URL modal for podcast subscriptions. Confirmation validates the https
// prefix, then enqueues a single-feed fetch through the shared podcast job
// pipeline; PumpPodcastJobs subscribes the feed once the fetch parses.
void App::ShowAddPodcastModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::CustomSettingInput;
    modalTitle = L"Add Podcast";
    modalPrompt = L"Podcast feed URL (https:// only):";
    modalInput.SetText({});
    modalOnConfirm = [this]() {
        const std::wstring raw = Text16ToWideString(modalInput.GetText());
        const auto trimW = [](const std::wstring& s) {
            const size_t b = s.find_first_not_of(L" \t\r\n");
            if (b == std::wstring::npos) return std::wstring();
            const size_t e = s.find_last_not_of(L" \t\r\n");
            return s.substr(b, e - b + 1);
        };
        const std::wstring url = trimW(raw);
        // Keep the modal open on invalid input (network-stream modal idiom) so
        // the URL can be corrected without retyping the surface path.
        if (url.size() <= 8 || url.rfind(L"https://", 0) != 0) {
            ShowOSD(IconType::VolumeMuted, L"Podcast feed invalid", 4.0f);
            return;
        }
        // Private mode skips the network validation fetch, so the add cannot succeed.
        if (playbackSession.IsPrivateMode()) {
            ShowOSD(IconType::VolumeMuted, L"Podcast feed invalid", 4.0f);
            return;
        }
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
        podcastJobs.EnsureAddPreview(url);
        if (isContextMenuOpen && !contextMenuItems.empty() && contextMenuItems.front().id == "pod_title")
            OpenPodcastsContextMenu();
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


// Second-level page for the specialist controls that used to clutter the main
// player menu. Every entry remains reachable, one step further away.

void App::ShowAddBookmarkModal() {
    if (!playbackSession.IsOpen() || player.GetCurrentFilePath().empty()) return;
    PushFocusSnapshot(TransientSurfaceType::Modal);
    double curTime = player.GetCurrentTime();
    if (!std::isfinite(curTime) || curTime < 0.0) curTime = 0.0;
    double duration = player.GetDuration();
    if (duration > 0.0 && curTime > duration) curTime = duration;
    BlurTextInput();
    activeModal = ModalType::BookmarkInput;
    modalTitle = L"Add Bookmark";
    modalPrompt = L"Enter bookmark label (" + FormatDuration(curTime) + L"):";
    std::wstring defaultLabel = L"Bookmark at " + FormatDuration(curTime);
    modalInput.SetText(WideStringToText16(defaultLabel));
    modalInput.SelectAll();
    std::wstring path = player.GetCurrentFilePath();
    modalOnConfirm = [this, path, curTime]() {
        std::wstring label = Text16ToWideString(modalInput.GetText());
        if (label.empty()) { label = L"Bookmark at " + FormatDuration(curTime); }
        if (label.length() > MediaBookmark::kMaxLabelLength) { label = label.substr(0, MediaBookmark::kMaxLabelLength); }
        MediaBookmark bm;
        bm.timeSec = curTime;
        bm.label = label;
        bm.createdTimestamp = GetCurrentTimestampSec();
        bm.id = "bm_" + std::to_string(bm.createdTimestamp) + "_" + std::to_string(static_cast<int64_t>(curTime * 1000));
        if (library.AddBookmark(path, bm)) {
            SaveLibrary();
            ShowOSD(IconType::Checkmark, L"Bookmark: " + label);
        } else { ShowOSD(IconType::VolumeMuted, L"Failed to add bookmark"); }
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::PromptClearAllBookmarks() {
    if (!playbackSession.IsOpen() || player.GetCurrentFilePath().empty()) return;
    std::wstring path = player.GetCurrentFilePath();
    ShowConfirmationModal(L"Clear Bookmarks", L"Are you sure you want to remove all bookmarks for this media?", L"Clear All", [this, path]() {
        if (library.ClearBookmarks(path)) { SaveLibrary(); ShowOSD(IconType::Trash, L"All bookmarks cleared"); }
    });
}

void App::ApplySubtitleStyleProfile(const std::string& profileName) {
    if (settings.ApplySubtitleProfile(profileName)) {
        ApplyPlayerPreferences();
        SaveSettings();
        ShowOSD(IconType::Subtitles, L"Applied Style: " + Utf8ToUtf16(profileName));
    }
}


void App::RestoreSubtitleStyleDefault() {
    settings.ResetSubtitleStyleToDefault();
    ApplyPlayerPreferences();
    SaveSettings();
    ShowOSD(IconType::Subtitles, L"Subtitle Style Reset to Default");
}


void App::ShowSaveSubtitleProfileModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::SaveSubtitleProfile;
    modalTitle = L"Save Subtitle Style Profile";
    modalPrompt = L"Enter profile name:";
    std::string defName = "Style " + std::to_string(settings.subtitleProfiles.size() + 1);
    modalInput.SetText(WideStringToText16(Utf8ToUtf16(defName)));
    modalInput.SelectAll();
    modalOnConfirm = [this]() {
        const std::wstring val = Text16ToWideString(modalInput.GetText());
        std::string name = Utf16ToUtf8(val);
        std::string sanitized = Settings::SanitizeProfileName(name);
        if (Settings::IsValidProfileName(sanitized)) {
            auto profile = settings.GetCurrentSubtitleStyleAsProfile(sanitized);
            if (settings.AddOrUpdateSubtitleProfile(profile)) {
                settings.activeSubtitleProfile = profile.name;
                SaveSettings();
                ShowOSD(IconType::Subtitles, L"Profile Saved: " + Utf8ToUtf16(profile.name));
            } else { ShowOSD(IconType::VolumeMuted, L"Failed to save profile"); }
        }
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowRenameSubtitleProfileModal(const std::string& profileName) {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::RenameSubtitleProfile;
    modalTitle = L"Rename Subtitle Style Profile";
    modalPrompt = L"Enter new profile name for '" + Utf8ToUtf16(profileName) + L"':";
    modalInput.SetText(WideStringToText16(Utf8ToUtf16(profileName)));
    modalInput.SelectAll();
    modalOnConfirm = [this, profileName]() {
        const std::wstring val = Text16ToWideString(modalInput.GetText());
        std::string newName = Utf16ToUtf8(val);
        std::string sanitized = Settings::SanitizeProfileName(newName);
        if (Settings::IsValidProfileName(sanitized)) {
            if (settings.RenameSubtitleProfile(profileName, sanitized)) {
                SaveSettings();
                ShowOSD(IconType::Subtitles, L"Profile Renamed: " + Utf8ToUtf16(sanitized));
            } else { ShowOSD(IconType::VolumeMuted, L"Profile rename failed"); }
        }
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowDeleteSubtitleProfileModal(const std::string& profileName) {
    ShowConfirmationModal(L"Delete Style Profile", L"Are you sure you want to delete profile '" + Utf8ToUtf16(profileName) + L"'?",
        L"Delete", [this, profileName]() {
            if (settings.DeleteSubtitleProfile(profileName)) {
                SaveSettings();
                ShowOSD(IconType::Trash, L"Deleted Profile: " + Utf8ToUtf16(profileName));
            }
        });
}


void App::ToggleSubtitles() { commandDispatcher.Execute(AppCommand::ToggleSubtitles); }


void App::ApplyTrackPreferences() {
    if (!playbackSession.IsOpen()) return;
    const std::wstring currentPath = player.GetCurrentFilePath().empty() ? playbackSession.GetCurrentFilePath() : player.GetCurrentFilePath();
    const MediaItem* item = currentPath.empty() ? nullptr : library.FindByPath(currentPath);
    auto audioTracks = player.GetAudioTracks();
    if (!audioTracks.empty()) {
        int selectedAudioId = 0;
        for (const auto& t : audioTracks) {
            if (t.isSelected) { selectedAudioId = t.id; break; }
        }
        int targetAudioId = 0;
        std::wstring targetAudioTitle;
        // 1. Try per-media preference
        if (item && item->preferences.has_value()) {
            if (item->preferences->audioTrackId.has_value() && *item->preferences->audioTrackId > 0) {
                for (const auto& t : audioTracks) {
                    if (t.id == *item->preferences->audioTrackId) { targetAudioId = t.id; targetAudioTitle = t.title; break; }
                }
            }
            if (targetAudioId == 0 && item->preferences->audioTrackTitle.has_value() && !item->preferences->audioTrackTitle->empty()) {
                const std::wstring& prefTitle = *item->preferences->audioTrackTitle;
                for (const auto& t : audioTracks) {
                    if (t.title == prefTitle) { targetAudioId = t.id; targetAudioTitle = t.title; break; }
                }
            }
            if (targetAudioId == 0 && item->preferences->audioTrackLang.has_value() && !item->preferences->audioTrackLang->empty()) {
                std::wstring prefLang = Utf8ToUtf16(*item->preferences->audioTrackLang);
                for (const auto& t : audioTracks) {
                    if (!t.lang.empty() && (t.lang == prefLang || t.title.find(prefLang) != std::wstring::npos)) {
                        targetAudioId = t.id;
                        targetAudioTitle = t.title;
                        break;
                    }
                }
            }
        }
        // 2. Fall back to session / settings defaults
        if (targetAudioId == 0 && playbackSession.GetLastActiveAudioTrackId() > 0) {
            for (const auto& t : audioTracks) {
                if (t.id == playbackSession.GetLastActiveAudioTrackId()) { targetAudioId = t.id; targetAudioTitle = t.title; break; }
            }
        }
        if (targetAudioId == 0 && !settings.lastAudioTrackTitle.empty()) {
            std::wstring prefTitle = Utf8ToUtf16(settings.lastAudioTrackTitle);
            for (const auto& t : audioTracks) {
                if (t.title == prefTitle) { targetAudioId = t.id; targetAudioTitle = t.title; break; }
            }
        }
        if (targetAudioId == 0 && !settings.audioLang.empty() && settings.audioLang != "eng") {
            std::wstring prefLang = Utf8ToUtf16(settings.audioLang);
            for (const auto& t : audioTracks) {
                if (!t.lang.empty() && (t.lang == prefLang || t.title.find(prefLang) != std::wstring::npos)) {
                    targetAudioId = t.id;
                    targetAudioTitle = t.title;
                    break;
                }
            }
        }
        if (targetAudioId > 0 && targetAudioId != selectedAudioId) {
            player.SelectAudioTrack(targetAudioId);
            playbackSession.SetLastActiveAudioTrackId(targetAudioId);
            playbackSession.SetLastActiveAudioTrackTitle(targetAudioTitle);
        } else if (selectedAudioId > 0) {
            playbackSession.SetLastActiveAudioTrackId(selectedAudioId);
            for (const auto& t : audioTracks) {
                if (t.id == selectedAudioId) { playbackSession.SetLastActiveAudioTrackTitle(t.title); break; }
            }
        }
    }
    auto subTracks = player.GetSubtitleTracks();
    if (!subTracks.empty()) {
        int selectedSubId = 0;
        std::wstring selectedSubTitle;
        for (const auto& t : subTracks) {
            if (t.isSelected) { selectedSubId = t.id; selectedSubTitle = t.title; break; }
        }
        bool subEnabled = settings.subtitlesEnabled && (settings.subLang != "no");
        if (item && item->preferences.has_value()) {
            if (item->preferences->subtitlesVisible.has_value()) {
                subEnabled = *item->preferences->subtitlesVisible;
            } else if (item->preferences->subtitleTrackId.has_value() && *item->preferences->subtitleTrackId == 0) { subEnabled = false; }
        }
        if (!subEnabled) {
            if (selectedSubId != 0) { player.SelectSubtitleTrack(0); }
            player.SetSubtitleVisibility(false);
            playbackSession.SetLastActiveSubtitleTrackId(0);
            playbackSession.SetLastActiveSubtitleTrackTitle(L"");
        } else {
            player.SetSubtitleVisibility(true);
            int targetSubId = 0;
            std::wstring targetSubTitle;
            // 1. Try per-media preference
            if (item && item->preferences.has_value()) {
                if (item->preferences->subtitleTrackId.has_value() && *item->preferences->subtitleTrackId > 0) {
                    for (const auto& t : subTracks) {
                        if (t.id == *item->preferences->subtitleTrackId) { targetSubId = t.id; targetSubTitle = t.title; break; }
                    }
                }
                if (targetSubId == 0 && item->preferences->subtitleTrackTitle.has_value() && !item->preferences->subtitleTrackTitle->empty()) {
                    const std::wstring& prefTitle = *item->preferences->subtitleTrackTitle;
                    for (const auto& t : subTracks) {
                        if (t.title == prefTitle) { targetSubId = t.id; targetSubTitle = t.title; break; }
                    }
                }
                if (targetSubId == 0 && item->preferences->subtitleTrackLang.has_value() && !item->preferences->subtitleTrackLang->empty()) {
                    std::wstring prefLang = Utf8ToUtf16(*item->preferences->subtitleTrackLang);
                    for (const auto& t : subTracks) {
                        if (!t.lang.empty() && (t.lang == prefLang || t.title.find(prefLang) != std::wstring::npos)) {
                            targetSubId = t.id;
                            targetSubTitle = t.title;
                            break;
                        }
                    }
                }
            }
            // 2. Fall back to session / settings defaults
            if (targetSubId == 0 && playbackSession.GetLastActiveSubtitleTrackId() > 0) {
                for (const auto& t : subTracks) {
                    if (t.id == playbackSession.GetLastActiveSubtitleTrackId()) { targetSubId = t.id; targetSubTitle = t.title; break; }
                }
            }
            if (targetSubId == 0 && !settings.lastSubtitleTrackTitle.empty()) {
                std::wstring prefTitle = Utf8ToUtf16(settings.lastSubtitleTrackTitle);
                for (const auto& t : subTracks) {
                    if (t.title == prefTitle) { targetSubId = t.id; targetSubTitle = t.title; break; }
                }
            }
            if (targetSubId == 0 && !settings.subLang.empty() && settings.subLang != "no") {
                std::wstring prefLang = Utf8ToUtf16(settings.subLang);
                for (const auto& t : subTracks) {
                    if (!t.lang.empty() && (t.lang == prefLang || t.title.find(prefLang) != std::wstring::npos)) {
                        targetSubId = t.id;
                        targetSubTitle = t.title;
                        break;
                    }
                }
            }
            if (targetSubId > 0 && targetSubId != selectedSubId) {
                player.SelectSubtitleTrack(targetSubId);
                playbackSession.SetLastActiveSubtitleTrackId(targetSubId);
                playbackSession.SetLastActiveSubtitleTrackTitle(targetSubTitle);
            } else if (selectedSubId > 0) {
                playbackSession.SetLastActiveSubtitleTrackId(selectedSubId);
                playbackSession.SetLastActiveSubtitleTrackTitle(selectedSubTitle);
            }
        }
    }
}


void App::OpenGlobalSearch() {
    PushFocusSnapshot(TransientSurfaceType::GlobalSearch);
    BlurTextInput();
    globalSearchModel.Open();
    globalSearchModel.SetQuery(L"", library, true);
    FocusTextInput(TextInputField::GlobalSearch);
    MarkAccessibilityDirty();
    if (hWnd) InvalidateRect(hWnd, NULL, FALSE);
}


void App::CloseGlobalSearch() {
    if (!globalSearchModel.IsOpen()) return;
    globalSearchModel.Close();
    if (activeTextInputField == TextInputField::GlobalSearch) { BlurTextInput(); }
    PopAndRestoreFocus(TransientSurfaceType::GlobalSearch);
}


void App::ToggleGlobalSearch() {
    if (globalSearchModel.IsOpen()) { CloseGlobalSearch(); } else { OpenGlobalSearch(); }
}


void App::ExecuteGlobalSearchAction(const GlobalSearchActionResult& action) {
    if (action.action == GlobalSearchActionType::PlayMedia) {
        CloseGlobalSearch();
        OpenAndPlayMedia(action.filePath, action.startPosSec);
    } else if (action.action == GlobalSearchActionType::ReportMissingMedia) {
        ShowOSD(IconType::Play, L"Media file not found on disk");
    } else if (action.action == GlobalSearchActionType::OpenInExplorer) { CloseGlobalSearch(); OpenInExplorer(action.filePath); }
}


void App::ShowNewPlaylistModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::NewPlaylist;
    renamePlaylistId.clear();
    modalTitle = L"Create New Playlist";
    modalPrompt = L"Enter playlist name...";
    modalInput.SetText(WideStringToText16(L"My Playlist"));
    modalOnConfirm = [this]() {
        std::string createdPlaylistId;
        const std::wstring value = Text16ToWideString(modalInput.GetText());
        if (!value.empty()) { Playlist* pl = library.CreatePlaylist(Utf16ToUtf8(value)); if (pl) { createdPlaylistId = pl->id; ArmCreatedPlaylistUndo(pl->id); } }
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
        ActivateCreatedPlaylist(createdPlaylistId);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowEditSmartPlaylistModal(const std::string& playlistId) {
    Playlist* pl = library.FindPlaylist(playlistId);
    if (!pl || !pl->IsSmart()) return;
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::EditSmartPlaylist;
    modalTitle = L"Edit Smart Playlist Rules";
    modalPrompt = L"Enter rules e.g. status = new; duration > 10m";
    std::string ruleStr = pl->smartCriteria.ToRuleString();
    modalInput.SetText(WideStringToText16(Utf8ToUtf16(ruleStr)));
    modalOnConfirm = [this, playlistId]() {
        const std::wstring raw = Text16ToWideString(modalInput.GetText());
        std::string ruleStr = Utf16ToUtf8(raw);
        SmartPlaylistCriteria criteria = SmartPlaylistCriteria::FromRuleString(ruleStr);
        std::string validation;
        if (!criteria.Validate(&validation)) {
            ShowOSD(IconType::VolumeMuted, Loc::Tr("ux.smart_rule_invalid") + L": " + Utf8ToUtf16(validation), 4.0f);
            return;
        }
        Playlist* p = library.FindPlaylist(playlistId);
        std::string name = p ? p->name : "Smart Playlist";
        const auto before = p ? p->smartCriteria : SmartPlaylistCriteria{};
        if (library.UpdateSmartPlaylist(playlistId, name, criteria)) {
            ArmLibraryUndo(L"Smart playlist rules", [this, playlistId, name, before, expected = criteria.ToRuleString()]() {
                const auto* item = library.FindPlaylist(playlistId);
                if (!item || item->smartCriteria.ToRuleString() != expected) return false;
                return library.UpdateSmartPlaylist(playlistId, name, before);
            });
            ShowOSD(IconType::FolderPlaylist, L"Rules Updated!");
        }
        SaveLibrary();
        activeModal = ModalType::None;
        editingSmartPlaylistId.clear();
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowNewSmartPlaylistModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::NewSmartPlaylist;
    modalTitle = L"New Smart Playlist";
    modalPrompt = L"Name | rules e.g. My Unwatched | status = new";
    modalInput.SetText(WideStringToText16(L""));
    modalOnConfirm = [this]() {
        const std::wstring raw = Text16ToWideString(modalInput.GetText());
        const auto trimW = [](const std::wstring& s) {
            const size_t b = s.find_first_not_of(L" \t\r\n");
            if (b == std::wstring::npos) return std::wstring();
            const size_t e = s.find_last_not_of(L" \t\r\n");
            return s.substr(b, e - b + 1);
        };
        const size_t pipe = raw.find(L'|');
        std::wstring name = trimW(pipe == std::wstring::npos ? raw : raw.substr(0, pipe));
        std::wstring ruleStr = pipe == std::wstring::npos ? L"" : trimW(raw.substr(pipe + 1));
        if (name.empty()) {
            ShowOSD(IconType::VolumeMuted, Loc::Tr("ux.smart_rule_invalid") + L": Name required", 4.0f);
            return;
        }
        SmartPlaylistCriteria criteria = SmartPlaylistCriteria::FromRuleString(Utf16ToUtf8(ruleStr));
        std::string validation;
        if (!criteria.Validate(&validation)) {
            ShowOSD(IconType::VolumeMuted, Loc::Tr("ux.smart_rule_invalid") + L": " + Utf8ToUtf16(validation), 4.0f);
            return;
        }
        std::string createdPlaylistId;
        if (Playlist* pl = library.CreateSmartPlaylist(Utf16ToUtf8(name), criteria)) {
            createdPlaylistId = pl->id;
            ArmCreatedPlaylistUndo(pl->id);
            ShowOSD(IconType::FolderPlaylist, L"Smart Playlist Created");
        }
        SaveLibrary();
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
        ActivateCreatedPlaylist(createdPlaylistId);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowEditMetadataModal(const std::wstring& mediaPath) {
    const MediaItem* item = library.FindByPath(mediaPath);
    if (!item) return;
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::EditMetadataOverrides;
    modalTitle = L"Edit Metadata";
    modalPrompt = L"Title | Year | Series (empty a part to clear it)";
    std::wstring prefill;
    if (item->overrides) {
        std::wstring parts[3];
        if (item->overrides->title) parts[0] = *item->overrides->title;
        if (item->overrides->year.has_value()) parts[1] = std::to_wstring(*item->overrides->year);
        if (item->overrides->series) parts[2] = *item->overrides->series;
        int last = -1;
        for (int i = 0; i < 3; ++i) { if (!parts[i].empty()) last = i; }
        for (int i = 0; i <= last; ++i) {
            if (i > 0) prefill += L"|";
            prefill += parts[i];
        }
    }
    modalInput.SetText(WideStringToText16(prefill));
    modalOnConfirm = [this, mediaPath]() {
        const std::wstring raw = Text16ToWideString(modalInput.GetText());
        const auto trimW = [](const std::wstring& s) {
            const size_t b = s.find_first_not_of(L" \t\r\n");
            if (b == std::wstring::npos) return std::wstring();
            const size_t e = s.find_last_not_of(L" \t\r\n");
            return s.substr(b, e - b + 1);
        };
        std::wstring parts[3];
        size_t start = 0;
        for (int i = 0; i < 3; ++i) {
            const size_t pipe = raw.find(L'|', start);
            parts[i] = trimW(pipe == std::wstring::npos ? raw.substr(start) : raw.substr(start, pipe - start));
            if (pipe == std::wstring::npos) break;
            start = pipe + 1;
        }
        MediaMetadataOverrides overrides;
        if (!parts[0].empty()) overrides.title = parts[0];
        if (!parts[1].empty()) {
            int year = 0;
            try { year = std::stoi(parts[1]); } catch (...) { year = -1; }
            if (!MediaMetadataOverrides::IsValidYear(year)) {
                ShowOSD(IconType::VolumeMuted, Loc::Tr("ux.smart_rule_invalid") + L": Year must be 1888-2100", 4.0f);
                return;
            }
            overrides.year = year;
        }
        if (!parts[2].empty()) overrides.series = parts[2];
        // Cover art is managed by the file picker, not this pipe modal; carry
        // the existing override through so editing text fields cannot drop it.
        if (const MediaItem* current = library.FindByPath(mediaPath)) {
            if (current->overrides) overrides.coverArtPath = current->overrides->coverArtPath;
        }
        if (!library.SetMetadataOverrides(mediaPath, overrides)) {
            ShowOSD(IconType::VolumeMuted, Loc::Tr("ux.smart_rule_invalid") + L": Media not found", 4.0f);
            return;
        }
        SaveLibrary();
        InvalidateMediaLayoutCache();
        ShowOSD(IconType::MediaFilm, L"Metadata Updated");
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowRenamePlaylistModal(const std::string& playlistId) { StartInlinePlaylistRename(playlistId); }


void App::StartInlinePlaylistRename(const std::string& playlistId) {
    Playlist* playlist = library.FindPlaylist(playlistId);
    if (!playlist) return;
    PushFocusSnapshot(TransientSurfaceType::InlineRename);
    BlurTextInput();
    isInlineRenaming = true;
    activePlaylistId = playlistId;
    inlineRenamePlaylistId = playlistId;
    inlineRenameInput.SetText(WideStringToText16(Utf8ToUtf16(playlist->name)));
    inlineRenameInput.SelectAll();
    FocusTextInput(TextInputField::InlineRename);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::CommitInlinePlaylistRename() {
    if (!isInlineRenaming) return;
    Playlist* pl = library.FindPlaylist(inlineRenamePlaylistId);
    std::wstring value = Text16ToWideString(inlineRenameInput.GetText());
    if (pl && !value.empty() && pl->name != Utf16ToUtf8(value)) {
        const auto id = pl->id, before = pl->name, after = Utf16ToUtf8(value);
        ArmLibraryUndo(L"Playlist rename", [this, id, before, after]() {
            auto* item = library.FindPlaylist(id);
            if (!item || item->name != after) return false;
            item->name = before;
            library.NotifyPlaylistEdited();
            return true;
        });
        pl->name = after;
        library.NotifyPlaylistEdited();
        SaveLibrary();
        ShowOSD(IconType::FolderPlaylist, L"Playlist Renamed");
    }
    isInlineRenaming = false;
    inlineRenamePlaylistId.clear();
    activeTextInputField = TextInputField::None;
    StopTextInputCaretTimer();
    PopAndRestoreFocus(TransientSurfaceType::InlineRename);
}


void App::CancelInlinePlaylistRename() {
    isInlineRenaming = false;
    inlineRenamePlaylistId.clear();
    activeTextInputField = TextInputField::None;
    StopTextInputCaretTimer();
    PopAndRestoreFocus(TransientSurfaceType::InlineRename);
}


void App::ShowDeletePlaylistModal(const std::string& playlistId) {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::DeletePlaylist;
    Playlist* pl = library.FindPlaylist(playlistId);
    modalMessage = L"Are you sure you want to delete playlist '" + (pl ? Utf8ToUtf16(pl->name) : L"") + L"'?";
    modalConfirmText = L"Delete";
    modalOnConfirm = [this, playlistId]() {
        ArmLibraryUndo(L"Playlist deletion");
        library.DeletePlaylist(playlistId);
        SaveLibrary();
        if (!library.playlists.empty()) { activePlaylistId = library.playlists[0].id; }
        ShowOSD(IconType::Trash, L"Playlist deleted \u2022 Ctrl+Z to undo");
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowConfirmationModal(const std::wstring& title, const std::wstring& message, const std::wstring& confirmText, std::function<void()> onConfirm, std::function<void()> onCancel, bool isDestructive) {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::Confirmation;
    modalFocusOwner = ModalType::None;
    modalKeyboardFocus = 0;
    isModalOkHovered = false;
    isModalCancelHovered = false;
    modalTitle = title;
    modalMessage = message;
    modalConfirmText = confirmText;
    modalIsDestructive = isDestructive;
    modalOnConfirm = std::move(onConfirm);
    modalOnCancel = std::move(onCancel);
    AnnounceStatus(title + L". " + message, true);
    MarkAccessibilityDirty();
    InvalidateRect(hWnd, NULL, FALSE);
}

void App::CheckForUpdates(bool isManual) {
    const std::wstring updaterScript = GetExeDir() + L"\\update.ps1";
    if (FileExists(updaterScript)) {
        // The portable updater installs all runtime assets and snapshots the profile.
        // It closes this instance through WM_CLOSE only after verification succeeds.
        if (!isManual) return;
        wchar_t systemDirectory[MAX_PATH]{};
        if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return;
        const std::wstring powershell = std::wstring(systemDirectory) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        const std::wstring arguments = L"-NoProfile -ExecutionPolicy Bypass -File \"" + updaterScript + L"\" -Interactive";
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(hWnd, L"open", powershell.c_str(), arguments.c_str(), GetExeDir().c_str(), SW_HIDE));
        ShowOSD(result > 32 ? IconType::SettingsGear : IconType::VolumeMuted,
            result > 32 ? L"Checking for updates. Aperture will restart when the update is ready." : L"Could not start the updater.");
        return;
    }
    if (updateDownloadJob.IsRunning() || updateCheckJob.IsRunning()) {
        if (isManual) ShowOSD(IconType::SettingsGear, L"An update operation is already in progress.");
        return;
    }
    UpdaterConfig cfg(settings.updateChannel);
    if (!settings.updateCheckUrl.empty()) cfg.manifestUrl = settings.updateCheckUrl;
    updateJobChannel = settings.updateChannel;
    updateJobUrl = settings.updateCheckUrl;
    updateJobsPrivateMode = playbackSession.IsPrivateMode();
    updateCheckManual = isManual;
    updateCheckPersistState = !playbackSession.IsPrivateMode();
    const std::wstring root = GetExeDir();
    updateCheckJob.Start([cfg, root](const auto& cancellation) mutable {
        cfg.cancellation = cancellation;
        return Aperture::CheckForUpdates(cfg, nullptr, root);
    });
    if (isManual) ShowOSD(IconType::SettingsGear, L"Checking for updates...");
}

void App::TriggerUpdateDownloadAndApply(const ReleaseManifest& manifest) {
    if (updateDownloadJob.IsRunning()) return;
    updateCheckJob.Cancel();
    ShowOSD(IconType::SettingsGear, L"Downloading Aperture update...");
    UpdaterConfig cfg(settings.updateChannel);
    if (!settings.updateCheckUrl.empty()) cfg.manifestUrl = settings.updateCheckUrl;
    updateJobChannel = settings.updateChannel;
    updateJobUrl = settings.updateCheckUrl;
    updateJobsPrivateMode = playbackSession.IsPrivateMode();
    const std::wstring root = GetExeDir();
    updateDownloadJob.Start([manifest, cfg, root](const auto& cancellation) mutable {
        cfg.cancellation = cancellation;
        return DownloadAndStageUpdate(manifest, cfg, nullptr, nullptr, root);
    });
}

bool App::PumpUpdateJobs() {
    if (updateJobChannel != settings.updateChannel || updateJobUrl != settings.updateCheckUrl ||
        updateJobsPrivateMode != playbackSession.IsPrivateMode()) {
        updateCheckJob.Cancel(); updateDownloadJob.Cancel();
        updateJobChannel = settings.updateChannel; updateJobUrl = settings.updateCheckUrl;
        updateJobsPrivateMode = playbackSession.IsPrivateMode();
        return false;
    }
    bool changed = false;
    // Keep the single bounded completion queued while another transient surface
    // owns focus. An asynchronous update prompt must never replace its callbacks.
    if (activeModal == ModalType::None && !ArePopupsOpen()) {
    if (auto completion = updateCheckJob.TakeResult()) {
        changed = true;
        if (!completion->result) {
            if (updateCheckManual) ShowOSD(IconType::VolumeMuted, L"Update check failed: " + Utf8ToUtf16(completion->error));
        } else {
            const auto& res = *completion->result;
            if (updateCheckPersistState && !playbackSession.IsPrivateMode() && res.status != UpdaterStatusCode::Cancelled) {
                UpdaterState st;
                st.lastCheckUtc = FormatIso8601Utc(std::chrono::system_clock::now());
                st.lastCheckStatus = (res.status == UpdaterStatusCode::UpdateAvailable) ? "update_available" :
                                     (res.status == UpdaterStatusCode::UpToDate) ? "up_to_date" : "failed";
                st.lastAvailableVersion = res.manifest.version;
                st.lastError = res.message;
                const auto status = SaveUpdaterState(st, false);
                if (!status.is_ok()) ReportStorageFailure(status, L"Save update check");
            }
            if (res.status == UpdaterStatusCode::UpdateAvailable) {
                const std::wstring message = L"A new version of Aperture (" + Utf8ToUtf16(res.manifest.version) + L") is available to install.\n\n" +
                    (!res.manifest.releaseNotes.empty() ? Utf8ToUtf16(res.manifest.releaseNotes) + L"\n\n" : L"") +
                    L"Would you like to download and install this update now?";
                ShowConfirmationModal(L"Software Update Available", message, L"Update Now",
                    [this, manifest = res.manifest]() { TriggerUpdateDownloadAndApply(manifest); }, nullptr, false);
            } else if (res.status == UpdaterStatusCode::UpToDate && updateCheckManual) {
                ShowOSD(IconType::Checkmark, L"Aperture is up to date (" + Utf8ToUtf16(SemanticVersion::Current().ToString()) + L")");
            } else if (res.status != UpdaterStatusCode::Cancelled && updateCheckManual) {
                ShowOSD(IconType::VolumeMuted, GetUpdaterStatusMessage(res.status, res.message));
            }
        }
    }
    }
    if (auto completion = updateDownloadJob.TakeResult()) {
        changed = true;
        if (!completion->result) {
            ShowOSD(IconType::VolumeMuted, L"Update download failed: " + Utf8ToUtf16(completion->error));
        } else {
            const auto& result = *completion->result;
            if (result.status == UpdaterStatusCode::Success) {
                ShowOSD(IconType::Checkmark, L"Update staged. Launching updater helper...");
                const auto apply = LaunchHelperAndExit(result, true);
                if (apply.status == UpdaterStatusCode::Success) PostMessageW(hWnd, WM_CLOSE, 0, 0);
                else ShowOSD(IconType::VolumeMuted, GetUpdaterStatusMessage(apply.status, apply.message));
            } else if (result.status != UpdaterStatusCode::Cancelled) {
                ShowOSD(IconType::VolumeMuted, GetUpdaterStatusMessage(result.status, result.message));
            }
        }
    }
    return changed;
}

void App::CancelNetworkJobs() {
    onlineSubtitleJobs.Cancel(); updateCheckJob.Cancel(); updateDownloadJob.Cancel(); radioJobs.Cancel(); podcastJobs.Cancel();
}

bool App::AreNetworkJobsRunning() const {
    return onlineSubtitleJobs.IsRunning() || updateCheckJob.IsRunning() || updateDownloadJob.IsRunning() || radioJobs.IsRunning() ||
        podcastJobs.IsRunning();
}


void App::StartShortcutCapture(const std::string& actionId, const std::string& actionName, int originRow, int originSubIndex) {
    PushFocusSnapshot(TransientSurfaceType::ShortcutCapture);
    rebindingActionId = actionId;
    rebindingActionName = actionName;
    rebindingOriginRow = originRow;
    rebindingOriginSubIndex = originSubIndex;
    activeFocusPane = FocusPane::SettingsContent;
    keyboardFocusedIndex = originRow;
    keyboardFocusedSubIndex = originSubIndex;
    lastAnnouncedModifierChord.clear();
    const std::wstring displayName = Utf8ToUtf16(actionName.empty() ? actionId : actionName);
    AnnounceStatus(L"Rebinding shortcut for " + displayName + L". Press desired key combination with optional Ctrl, Shift, or Alt, or press Escape to cancel.", true);
    MarkAccessibilityDirty();
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::CancelShortcutCapture() {
    if (rebindingActionId.empty()) return;
    const std::wstring displayName = Utf8ToUtf16(rebindingActionName.empty() ? rebindingActionId : rebindingActionName);
    rebindingActionId.clear();
    rebindingActionName.clear();
    lastAnnouncedModifierChord.clear();
    ShowOSD(IconType::SettingsGear, L"Shortcut Rebinding Cancelled");
    AnnounceStatus(L"Shortcut rebinding cancelled for " + displayName + L".", true);
    PopAndRestoreFocus(TransientSurfaceType::ShortcutCapture);
}


std::wstring App::GetCurrentCaptureChordText() const {
    const bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool isAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    std::wstring text;
    if (isCtrl) text += L"Ctrl + ";
    if (isAlt) text += L"Alt + ";
    if (isShift) text += L"Shift + ";
    if (text.empty()) { return L"[ Press Key... ]"; }
    return text + L"...";
}


bool App::HandleShortcutCaptureMouse(UINT vkCode) {
    if (rebindingActionId.empty()) return false;
    const bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool isAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    CompleteShortcutCapture(vkCode, isCtrl, isShift, isAlt);
    return true;
}


void App::CompleteShortcutCapture(UINT vkCode, bool ctrl, bool shift, bool alt) {
    ShortcutBinding b;
    b.actionId = rebindingActionId;
    b.displayName = rebindingActionName.empty() ? rebindingActionId : rebindingActionName;
    b.vkCode = vkCode;
    b.ctrl = ctrl;
    b.shift = shift;
    b.alt = alt;
    const std::string targetId = rebindingActionId;
    const std::string targetDisplayName = b.displayName;
    const int originRow = rebindingOriginRow, originSub = rebindingOriginSubIndex;
    const std::wstring keyStr = Settings::FormatShortcutKey(b);
    auto conflict = settings.shortcuts.end();
    for (auto it = settings.shortcuts.begin(); it != settings.shortcuts.end(); ++it) {
        if (it->first != targetId && it->second.vkCode == b.vkCode &&
            it->second.ctrl == b.ctrl && it->second.shift == b.shift && it->second.alt == b.alt) {
            conflict = it;
            break;
        }
    }
    if (conflict != settings.shortcuts.end()) {
        const ShortcutBinding pending = b;
        const std::string conflictId = conflict->first;
        const std::wstring conflictName = Utf8ToUtf16(conflict->second.displayName);
        const std::wstring targetName = Utf8ToUtf16(targetDisplayName);
        rebindingActionId.clear();
        rebindingActionName.clear();
        lastAnnouncedModifierChord.clear();
        AnnounceStatus(L"Shortcut conflict: " + keyStr + L" is already assigned to " + conflictName + L". Confirmation required to replace binding.", true);
        ShowConfirmationModal( L"Shortcut Conflict", keyStr + L" is already assigned to " + conflictName + L". Replace that binding?",
            L"Replace", [this, targetId, pending, conflictId, keyStr, conflictName, targetName, originRow, originSub]() {
                auto old = settings.shortcuts.find(conflictId);
                if (old != settings.shortcuts.end()) old->second.vkCode = 0;
                settings.shortcuts[targetId] = pending;
                SaveSettings();
                ApplyGlobalHotkeys();
                ShowOSD(IconType::SettingsGear, Utf8ToUtf16(pending.displayName) + L" assigned to " + keyStr);
                AnnounceStatus(targetName + L" assigned to " + keyStr + L". Previous binding for " + conflictName + L" is now unassigned. Settings saved.", true);
                activeFocusPane = FocusPane::SettingsContent;
                keyboardFocusedIndex = originRow;
                keyboardFocusedSubIndex = originSub;
                MarkAccessibilityDirty();
                InvalidateRect(hWnd, nullptr, FALSE);
            }, [this, targetName, originRow, originSub]() {
                AnnounceStatus(L"Shortcut replacement cancelled. " + targetName + L" remains unchanged.", true);
                activeFocusPane = FocusPane::SettingsContent;
                keyboardFocusedIndex = originRow;
                keyboardFocusedSubIndex = originSub;
                MarkAccessibilityDirty();
                InvalidateRect(hWnd, nullptr, FALSE);
            });
        return;
    }
    settings.shortcuts[targetId] = b;
    SaveSettings();
    ApplyGlobalHotkeys();
    ShowOSD(IconType::SettingsGear, Utf8ToUtf16(b.displayName) + L" assigned to " + keyStr);
    AnnounceStatus(Utf8ToUtf16(b.displayName) + L" shortcut assigned to " + keyStr + L". Settings saved.", true);
    rebindingActionId.clear();
    rebindingActionName.clear();
    lastAnnouncedModifierChord.clear();
    PopAndRestoreFocus(TransientSurfaceType::ShortcutCapture);
}


void App::ShowOpenNetworkStreamModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::SeekStepInput;
    modalPrompt = L"Enter an HTTP, HTTPS, RTSP, RTMP, UDP, or RTP URL (Ctrl+L):";
    modalInput.SetText({});
    modalOnConfirm = [this]() {
        const std::wstring input = Text16ToWideString(modalInput.GetText());
        NetworkStreamUrl parsed = NetworkStreamValidator::ValidateAndParse(input);
        if (!parsed.isValid) {
            ShowOSD(IconType::VolumeMuted, NetworkStreamValidator::GetSafeErrorMessage(parsed.error, input), 4.0f);
            return;
        }
        if (playbackSession.CanPersistRecentStreams(networkStreamModel.IsRecentStreamsEnabled())) {
            networkStreamModel.AddRecentStream(parsed.rawUrl);
            networkStreamModel.SaveRecentStreams(GetExeDir());
        }
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
        OpenAndPlayMedia(parsed.rawUrl);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ImportStandardPlaylist() {
    PushFocusSnapshot(TransientSurfaceType::SystemDialog);
    std::vector<wchar_t> buffer(65536, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"Playlists (*.m3u;*.m3u8;*.pls;*.xspf;*.asx;*.wpl;*.xml;*.b4s;*.qtl)\0*.m3u;*.m3u8;*.pls;*.xspf;*.asx;*.wpl;*.xml;*.b4s;*.qtl\0All Files\0*.*\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!DelaySafeGetOpenFileNameW(&ofn)) { PopAndRestoreFocus(TransientSurfaceType::SystemDialog); return; }
    PlaylistParseResult result = PlaylistIO::ImportPlaylistFile(buffer.data());
    if (!result) {
        ShowOSD(IconType::VolumeMuted, L"Playlist import failed", 4.0f);
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    std::wstring displayName = result.playlistTitle.empty() ? GetFileTitleFromPath(buffer.data()) : result.playlistTitle;
    Playlist* playlist = library.CreatePlaylist(Utf16ToUtf8(displayName));
    if (!playlist) {
        ShowOSD(IconType::VolumeMuted, L"Unable to create playlist", 4.0f);
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    ArmCreatedPlaylistUndo(playlist->id);
    playlist->itemPaths.clear();
    for (const auto& entry : result.entries) playlist->itemPaths.push_back(entry.location);
    activePlaylistId = playlist->id;
    library.NotifyPlaylistEdited();
    SaveLibrary();
    ShowOSD(IconType::FolderPlaylist, L"Imported " + std::to_wstring(result.entries.size()) + L" playlist items");
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
}


void App::ExportActivePlaylist(PlaylistFormat format) {
    PushFocusSnapshot(TransientSurfaceType::SystemDialog);
    Playlist* playlist = library.FindPlaylist(activePlaylistId);
    if (!playlist && !library.playlists.empty()) playlist = &library.playlists.front();
    if (!playlist) {
        ShowOSD(IconType::VolumeMuted, L"No playlist to export");
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    const std::wstring exportDir = GetExeDir() + L"\\playlists";
    SHCreateDirectoryExW(nullptr, exportDir.c_str(), nullptr);
    const std::wstring name = SanitizeMediaStemForFilename(Utf8ToUtf16(playlist->name));
    const wchar_t* extension = (format == PlaylistFormat::XSPF) ? L".xspf" : L".m3u8";
    PlaylistExportOptions options;
    options.format = format;
    options.basePath = exportDir;
    options.collisionSafe = true;
    PlaylistExportResult result = PlaylistIO::ExportPlaylistFile( exportDir + L"\\" + name + extension, *playlist, options);
    if (!result) {
        ShowOSD(IconType::VolumeMuted, L"Playlist export failed", 4.0f);
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    ShowOSD(IconType::FolderPlaylist, L"Exported: playlists\\" + GetFileNameFromPath(result.exportedPath));
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
}


void App::ShowCustomSeekStepModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::SeekStepInput;
    modalPrompt = L"Enter seek duration in seconds (e.g. 3, 7.5, 20):";
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", settings.arrowSeekStepSec);
    if (wcslen(buf) > 2 && wcscmp(buf + wcslen(buf) - 2, L".0") == 0) buf[wcslen(buf) - 2] = L'\0';
    modalInput.SetText(WideStringToText16(buf));
    modalOnConfirm = [this]() {
        const std::wstring val = Text16ToWideString(modalInput.GetText());
        try {
            double sec = std::stod(val);
            if (sec >= 0.1 && sec <= 600.0) {
                settings.arrowSeekStepSec = sec;
                SaveSettings();
                ApplyPlayerPreferences();
                wchar_t sbuf[32];
                swprintf_s(sbuf, L"Seek Step: %.1fs", sec);
                if (wcslen(sbuf) > 3 && wcscmp(sbuf + wcslen(sbuf) - 3, L".0s") == 0) { swprintf_s(sbuf, L"Seek Step: %ds", (int)sec); }
                ShowOSD(IconType::SettingsGear, sbuf);
            }
        } catch (...) {}
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowCustomFrameSeekModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::FrameSeekInput;
    modalPrompt = L"Enter frame step (1 - 120 frames):";
    modalInput.SetText(WideStringToText16(std::to_wstring(settings.ctrlSeekFrames)));
    modalOnConfirm = [this]() {
        const std::wstring val = Text16ToWideString(modalInput.GetText());
        try {
            int frames = std::stoi(val);
            if (frames >= 1 && frames <= 120) {
                settings.ctrlSeekFrames = frames;
                SaveSettings();
                ApplyPlayerPreferences();
                ShowOSD(IconType::SettingsGear, L"Frame Seek: " + std::to_wstring(frames) + (frames == 1 ? L" frame" : L" frames"));
            }
        } catch (...) {}
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowCustomNetworkForwardCacheModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::CustomSettingInput;
    modalPrompt = L"Enter forward cache size in MB (8 - 512):";
    modalInput.SetText(WideStringToText16(std::to_wstring(settings.networkForwardCacheMB)));
    modalOnConfirm = [this]() {
        const std::wstring val = Text16ToWideString(modalInput.GetText());
        try {
            int mb = std::stoi(val);
            if (mb >= 8 && mb <= 512) {
                settings.networkForwardCacheMB = mb;
                SaveSettings();
                ApplyPlayerPreferences();
                ShowOSD(IconType::SettingsGear, L"Network Cache (Forward): " + std::to_wstring(mb) + L" MB");
            } else { ShowOSD(IconType::VolumeMuted, L"Invalid cache size (8 - 512 MB)"); }
        } catch (...) { ShowOSD(IconType::VolumeMuted, L"Invalid cache size (8 - 512 MB)"); }
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowCustomNetworkBackCacheModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::CustomSettingInput;
    modalPrompt = L"Enter backward cache size in MB (0 = off, 0 - 256):";
    modalInput.SetText(WideStringToText16(std::to_wstring(settings.networkBackCacheMB)));
    modalOnConfirm = [this]() {
        const std::wstring val = Text16ToWideString(modalInput.GetText());
        try {
            int mb = std::stoi(val);
            if (mb >= 0 && mb <= 256) {
                settings.networkBackCacheMB = mb;
                SaveSettings();
                ApplyPlayerPreferences();
                ShowOSD(IconType::SettingsGear, mb == 0 ? L"Network Cache (Back): Off"
                                                        : L"Network Cache (Back): " + std::to_wstring(mb) + L" MB");
            } else { ShowOSD(IconType::VolumeMuted, L"Invalid cache size (0 - 256 MB)"); }
        } catch (...) { ShowOSD(IconType::VolumeMuted, L"Invalid cache size (0 - 256 MB)"); }
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowJumpToTimeModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::JumpToTime;
    modalTitle = L"Go To Time";
    modalPrompt = L"Enter a time (h:mm:ss, m:ss, or seconds)";
    modalInput.SetText({});
    modalOnConfirm = [this]() {
        // No media open: close the modal silently.
        if (playbackSession.IsOpen()) {
            const std::wstring val = Text16ToWideString(modalInput.GetText());
            // Accepted: plain seconds ("90"), "m:ss" (":30", "1:30"), "h:mm:ss" ("1:02:03");
            // leading/trailing spaces are tolerated, everything else is rejected.
            std::wstring text = val;
            auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t'; };
            while (!text.empty() && isSpace(text.front())) text.erase(text.begin());
            while (!text.empty() && isSpace(text.back())) text.pop_back();
            double seconds = 0.0;
            bool valid = false;
            if (text.find(L':') == std::wstring::npos) {
                try {
                    size_t consumed = 0;
                    const double parsed = std::stod(text, &consumed);
                    if (consumed == text.size() && std::isfinite(parsed) && parsed >= 0.0) { seconds = parsed; valid = true; }
                } catch (...) {}
            } else {
                std::vector<std::wstring> parts;
                size_t start = 0;
                while (true) {
                    const size_t colon = text.find(L':', start);
                    if (colon == std::wstring::npos) { parts.push_back(text.substr(start)); break; }
                    parts.push_back(text.substr(start, colon - start));
                    start = colon + 1;
                }
                if (parts.size() == 2 || parts.size() == 3) {
                    bool allDigits = true;
                    for (size_t i = 0; i < parts.size(); ++i) {
                        // An empty leading component (":30") counts as zero minutes.
                        if (parts[i].empty() && i == 0) continue;
                        if (parts[i].empty() || parts[i].find_first_not_of(L"0123456789") != std::wstring::npos) { allDigits = false; break; }
                    }
                    try {
                        if (allDigits) {
                            const double h = parts.size() == 3 ? std::stod(parts[0]) : 0.0;
                            const double m = parts.size() == 3 ? std::stod(parts[1]) : std::stod(parts[0]);
                            const double s = std::stod(parts.back());
                            seconds = h * 3600.0 + m * 60.0 + s;
                            valid = true;
                        }
                    } catch (...) { valid = false; }
                }
            }
            if (valid) {
                const double duration = player.GetDuration();
                if (duration > 0.0 && std::isfinite(duration)) seconds = std::clamp(seconds, 0.0, duration);
                else seconds = std::max(0.0, seconds);
                player.Seek(seconds, true);
                ShowOSD(IconType::Refresh, L"Jumped to " + FormatDuration(seconds));
            }
        }
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowCustomAspectModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::CustomAspectInput;
    modalTitle = L"Custom Aspect Ratio";
    modalPrompt = L"e.g. 16:9, 2.35:1, 1.85";
    // Prefill the current value only when it is already a custom ratio.
    const bool currentIsCustom = settings.lastAspectRatio != "auto" && settings.lastAspectRatio != "16:9" &&
        settings.lastAspectRatio != "4:3" && settings.lastAspectRatio != "21:9" &&
        settings.lastAspectRatio != "fill" && settings.lastAspectRatio != "cover";
    modalInput.SetText(WideStringToText16(currentIsCustom ? Utf8ToUtf16(settings.lastAspectRatio) : std::wstring()));
    modalOnConfirm = [this]() {
        const std::wstring val = Text16ToWideString(modalInput.GetText());
        // Accepted: "W:H" with decimal W,H > 0, or a single decimal ratio > 0;
        // leading/trailing spaces are tolerated, everything else is rejected.
        std::wstring text = val;
        auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t'; };
        while (!text.empty() && isSpace(text.front())) text.erase(text.begin());
        while (!text.empty() && isSpace(text.back())) text.pop_back();
        const std::string aspect = Utf16ToUtf8(text);
        if (!IsValidCustomAspectString(aspect)) {
            ShowOSD(IconType::VolumeMuted, L"Invalid aspect ratio", 4.0f);
        } else {
            CommandArgs args;
            args.stringVal = aspect;
            commandDispatcher.Execute(AppCommand::SetAspectRatio, args);
        }
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ShowCustomRotationModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::CustomRotationInput;
    modalTitle = L"Custom Rotation";
    modalPrompt = L"Degrees clockwise (0-359)";
    modalInput.SetText(WideStringToText16(std::to_wstring(settings.videoRotate)));
    modalOnConfirm = [this]() {
        const std::wstring val = Text16ToWideString(modalInput.GetText());
        // Accepted: whole degrees 0-359; leading/trailing spaces are tolerated.
        std::wstring text = val;
        auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t'; };
        while (!text.empty() && isSpace(text.front())) text.erase(text.begin());
        while (!text.empty() && isSpace(text.back())) text.pop_back();
        bool valid = false;
        int degrees = 0;
        if (!text.empty() && text.find_first_not_of(L"0123456789") == std::wstring::npos) {
            try {
                size_t consumed = 0;
                const long long parsed = std::stoll(text, &consumed);
                if (consumed == text.size() && parsed >= 0 && parsed <= 359) { degrees = (int)parsed; valid = true; }
            } catch (...) {}
        }
        if (!valid) {
            ShowOSD(IconType::VolumeMuted, L"Invalid rotation", 4.0f);
        } else {
            player.SetVideoRotate(degrees);
            settings.videoRotate = degrees;
            SaveSettings();
            ShowOSD(IconType::AspectFrame, L"Rotation: " + std::to_wstring(degrees) + L"°");
        }
        activeModal = ModalType::None;
        BlurTextInput();
        PopAndRestoreFocus(TransientSurfaceType::Modal);
    };
    FocusTextInput(TextInputField::Modal);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ToggleCustomMpvConfig() {
    settings.customMpvConfigEnabled = !settings.customMpvConfigEnabled;
    AnimateVisual(customMpvConfigToggleAnim, settings.customMpvConfigEnabled ? 1.0f : 0.0f, MotionTokens::kToggle, MotionTokens::Standard());
    player.SetCustomMpvConfigEnabled(settings.customMpvConfigEnabled);
    if (settings.customMpvConfigEnabled) {
        std::wstring path = Settings::ResolveMpvConfigPath(settings.customMpvConfigPath);
        CustomMpvApplyResult res = player.LoadAndApplyCustomMpvConfig(path);
        if (!res.success) {
            settings.customMpvConfigEnabled = false;
            AnimateVisual(customMpvConfigToggleAnim, 0.0f, MotionTokens::kToggle, MotionTokens::Standard());
            player.SetCustomMpvConfigEnabled(false);
            ShowOSD(IconType::VolumeMuted, L"Custom MPV Config Failed: " + Utf8ToUtf16(res.errorMessage));
        } else {
            std::wstring msg = L"Custom MPV Config Enabled (" + std::to_wstring(res.appliedOptions.size()) + L" options applied)";
            if (!res.rejectedOptions.empty()) { msg += L" [" + std::to_wstring(res.rejectedOptions.size()) + L" unsafe rejected]"; }
            ShowOSD(IconType::Checkmark, msg);
        }
    } else { ShowOSD(IconType::SettingsGear, L"Custom MPV Configuration Disabled"); }
    SaveSettings();
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::EditCustomMpvConfigFile() {
    std::wstring path = Settings::ResolveMpvConfigPath(settings.customMpvConfigPath);
    if (!FileExists(path)) { Settings::ResetMpvConfigToDefault(path); }
    ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
    ShowOSD(IconType::SettingsGear, L"Opening mpv.conf");
}


void App::ResetCustomMpvConfigToDefault() {
    ShowConfirmationModal(L"Reset mpv.conf to Defaults", L"Are you sure you want to reset mpv.conf to the default template? A rotating backup will be created.", L"Reset", [this]() {
        std::wstring path = Settings::ResolveMpvConfigPath(settings.customMpvConfigPath);
        Settings::ResetMpvConfigToDefault(path);
        if (settings.customMpvConfigEnabled) { player.LoadAndApplyCustomMpvConfig(path); }
        ShowOSD(IconType::Refresh, L"Reset mpv.conf to defaults");
        InvalidateRect(hWnd, NULL, FALSE);
    });
}


void App::RevertCustomMpvConfigToLastKnownGood() {
    ShowConfirmationModal(L"Revert mpv.conf to Last Known Good", L"Are you sure you want to revert mpv.conf to the last known working configuration?", L"Revert", [this]() {
        std::wstring path = Settings::ResolveMpvConfigPath(settings.customMpvConfigPath);
        StorageStatus st = Settings::RevertMpvConfigToLastKnownGood(path);
        if (st.is_ok()) {
            if (settings.customMpvConfigEnabled) { player.LoadAndApplyCustomMpvConfig(path); }
            ShowOSD(IconType::Checkmark, L"Reverted mpv.conf to last known good copy");
        } else { ShowOSD(IconType::VolumeMuted, L"No last-known-good backup available"); }
        InvalidateRect(hWnd, NULL, FALSE);
    });
}

} // namespace Aperture
