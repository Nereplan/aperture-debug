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
    onlineSubtitleJobs.Cancel(); updateCheckJob.Cancel(); updateDownloadJob.Cancel();
}

bool App::AreNetworkJobsRunning() const {
    return onlineSubtitleJobs.IsRunning() || updateCheckJob.IsRunning() || updateDownloadJob.IsRunning();
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


void App::ShowOpenNetworkStreamModal() {
    PushFocusSnapshot(TransientSurfaceType::Modal);
    BlurTextInput();
    activeModal = ModalType::SeekStepInput;
    modalPrompt = L"Enter an HTTP, HTTPS, RTSP, or RTMP URL (Ctrl+L):";
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
    ofn.lpstrFilter = L"Playlists (*.m3u;*.m3u8;*.pls)\0*.m3u;*.m3u8;*.pls\0All Files\0*.*\0";
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


void App::ExportActivePlaylist() {
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
    PlaylistExportOptions options;
    options.format = PlaylistFormat::M3U8;
    options.basePath = exportDir;
    options.collisionSafe = true;
    PlaylistExportResult result = PlaylistIO::ExportPlaylistFile( exportDir + L"\\" + name + L".m3u8", *playlist, options);
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
