#include "Localization.h"
#include "Diagnostics.h"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <regex>
#include <string_view>

namespace Aperture {

namespace {

struct StringEntryView { std::string_view key; std::string_view value; };

struct PluralFormView {
    std::string_view zero;
    std::string_view one;
    std::string_view two;
    std::string_view few;
    std::string_view many;
    std::string_view other;
};

struct PluralEntryView { std::string_view key; PluralFormView forms; };

inline std::wstring Utf8ToUtf16View(std::string_view sv) {
    if (sv.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), NULL, 0);
    if (size <= 0) return L"";
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), &result[0], size);
    return result;
}

// Compile-time canonical English strings dictionary
constexpr StringEntryView kCanonicalStringEntries[] = {
    { "ux.autoplay_description", "Queues continue automatically in their chosen order." }, { "ux.queue_position", "Queue {0} of {1}" },
    { "ux.queue_next", "Next in queue" }, { "ux.queue_restart", "Restart queue" }, { "ux.library_next", "Next in library" },
    { "ux.storage_title", "Your data needs attention" },
    { "ux.storage_full", "There is not enough space to save your changes. Free up disk space, then try again." },
    { "ux.storage_read", "Aperture could not read your saved data. Check that the drive is connected. Your existing files have been kept." },
    { "ux.storage_permission", "Aperture cannot save changes in this folder. Move the app folder to a location you can write to, then reopen it." },
    { "ux.storage_retry", "Your changes could not be saved. Check that the drive is connected and has free space, then try again." },
    { "ux.open_folder", "Open folder" }, { "ux.playback_title", "This media could not be played" },
    { "ux.playback_local", "Check that the file is available and opens in another player, then try again." },
    { "ux.playback_network", "Check your connection and whether the stream address still works, then try again." },
    { "ux.playback_engine", "A playback component could not start. Reopen Aperture or restore the app files, then try again." },
    { "ux.smart_rule_invalid", "Enter a valid smart playlist rule" },
    { "ux.retry", "Try again" }, { "ux.back_library", "Back to library" }, { "ux.queue_single", "Start a one-item queue" },
    { "ux.queue_selected", "Start queue ({0} items)" }, { "ux.missing_title", "File unavailable" },
    { "ux.missing_body", "The file may have moved, or its drive may be disconnected. Reconnect the drive or locate the file to keep your progress." },
    { "ux.locate_file", "Locate file" },
    // Navigation & General App
    { "app.name", "Aperture" }, { "app.description", "Aperture media player" }, { "app.status_live_region", "Application Status" },
    { "nav.media", "Media" }, { "nav.playlists", "Playlists" }, { "nav.history", "History" }, { "nav.settings", "Settings" },
    { "nav.open_file", "+ Open File" }, { "nav.drop_to_playlist", "+ Drop to Playlist" },
    // Media Hub & Toolbar
    { "media.all_media", "All Media" }, { "media.folders", "Folders" }, { "media.series", "Series" }, { "media.duplicates", "Duplicates" },
    { "media.search_placeholder", "Search media library..." }, { "media.missing", "MISSING" }, { "media.done", "Done" },
    { "media.duration_unavailable", "Unknown duration" }, { "media.empty.title", "Your Media Library is empty" },
    { "media.empty.body", "Open video files or add a folder to watch. You can also drag and drop media files anywhere into the window." },
    { "media.empty.open_file", "Open File" }, { "media.empty.add_folder", "Add Folder" },
    { "media.empty_filter.title", "No matching media found" },
    { "media.empty_filter.body", "No media items match the current view filter. Reset grouping to view all media files." },
    { "media.empty_duplicates.title", "No duplicate media found" },
    { "media.empty_duplicates.body", "No duplicate videos were detected in your library. Reset grouping to view all media files." },
    { "media.empty.reset_filter", "View All Media" }, { "media.card_tooltip", "{0} ({1})" },
    // Playlists Tab
    { "playlist.create_new", "+ Create New Playlist" }, { "playlist.name_placeholder", "Playlist name" }, { "playlist.empty", "Empty" },
    { "playlist.select_prompt", "Select or create a playlist from the left rail to view its contents." },
    { "playlist.play_all", "Play All" }, { "playlist.drag_drop_title", "Drag and drop video files here to add them to this playlist" },
    { "playlist.drag_drop_subtitle", "or drag cards from the Media tab, or right-click any video to add" },
    { "playlist.continue_watching", "CONTINUE WATCHING" }, { "playlist.start_watching", "START WATCHING" },
    { "playlist.filter_placeholder", "Filter episodes\xE2\x80\xA6" }, { "playlist.drop_banner", "+ Drop files here to add into \"{0}\"" },
    { "playlist.episode_number", "Episode {0}" }, { "playlist.smart_prefix", "Smart Playlist" },
    { "playlist.meta_summary", "{0} · {1} · {2} total" }, { "playlist.meta_watched", " · {0} watched" },
    // History Tab
    { "history.title", "History" }, { "history.clear_history", "Clear History" }, { "history.empty.title", "No watch history yet" },
    { "history.empty.body", "Media you play will show up here automatically with your saved playback positions and timestamps." },
    { "history.empty.open_file", "Open File" }, { "history.group.today", "Today" }, { "history.group.yesterday", "Yesterday" },
    { "history.group.this_week", "This Week" }, { "history.group.earlier", "Earlier" }, { "history.watched_at", "Watched at {0}" },
    // Settings Categories
    { "settings.category.appearance", "Appearance" }, { "settings.category.playback", "Playback" },
    { "settings.category.subtitles", "Audio & Subtitles" }, { "settings.category.shortcuts", "Keybindings" },
    { "settings.category.system", "System & Storage" },
    { "settings.page.appearance", "Set the look of the interface and the display language." },
    { "settings.page.playback", "Default behavior, seek granularity, and advanced mpv configuration." },
    { "settings.page.subtitles", "Choose default tracks and how subtitles look during playback." },
    { "settings.subtitles.preview_title", "Subtitle preview" }, { "settings.subtitles.preview_text", "This is how subtitles will look" },
    { "settings.page.shortcuts", "Customizable keyboard shortcuts." },
    { "settings.page.system", "Updates, preferences, and your saved data." },
    // Settings: Appearance
    { "settings.appearance.theme_palette", "THEME" }, { "settings.appearance.display_language", "DISPLAY LANGUAGE" },
    { "theme.default", "Default" }, { "theme.oled_dark", "OLED Dark" }, { "theme.cyberpunk", "Cyberpunk" }, { "theme.nord", "Nord" },
    { "theme.solarized", "Solarized" }, { "theme.tokyo_night", "Tokyo Night" }, { "theme.gruvbox", "Gruvbox" },
    { "theme.dracula", "Dracula" }, { "theme.catppuccin", "Catppuccin" }, { "theme.monokai", "Monokai" }, { "font.inter", "Inter" },
    { "font.space_grotesk", "Space Grotesk" }, { "font.jetbrains_mono", "JetBrains Mono" },
    // Settings: Playback
    { "settings.playback.behavior", "PLAYBACK BEHAVIOR" }, { "settings.playback.autoplay", "Autoplay from library" },
    { "settings.playback.remember_pos", "Remember last playback position" },
    { "settings.playback.hw_accel", "Hardware accelerated video decoding" }, { "settings.playback.hw_accel_desc", "D3D11VA" },
    { "settings.playback.loop", "Loop playback" }, { "settings.playback.loop_desc", "Repeat current track" },
    { "settings.playback.seek_interval", "ARROW KEY SEEK INTERVAL" }, { "settings.playback.seek_step_title", "Seek Step Duration" },
    { "settings.playback.seek_step_desc", "Time jumped forward or backward per Left / Right arrow key press" },
    { "settings.playback.custom", "Custom..." }, { "settings.playback.frame_step_header", "FRAME STEP SIZE" },
    { "settings.playback.frame_step_title", "Frame Step Size" },
    { "settings.playback.frame_step_desc", "Number of video frames advanced or reversed per frame-step command" },
    { "settings.playback.default_speed", "Default playback speed" },
    { "settings.playback.mpv_conf_header", "CUSTOM MPV CONFIGURATION (mpv.conf)" },
    { "settings.playback.mpv_conf_enable", "Enable Custom MPV Configuration" },
    { "settings.playback.mpv_conf_desc", "Load root-local mpv.conf for custom video/audio rendering options (safe options only)" },
    { "settings.playback.mpv_edit_title", "Open / Edit mpv.conf" },
    { "settings.playback.mpv_edit_desc", "Explore and edit root-local configuration in text editor" },
    { "settings.playback.mpv_edit_btn", "Edit File" }, { "settings.playback.mpv_reset_title", "Reset to Default Template" },
    { "settings.playback.mpv_reset_desc", "Overwrite mpv.conf with clean safe defaults (creates rotating backup)" },
    { "settings.playback.mpv_reset_btn", "Reset" }, { "settings.playback.mpv_revert_title", "Revert to Last Known Good" },
    { "settings.playback.mpv_revert_desc", "Restore mpv.conf from last verified working startup configuration" },
    { "settings.playback.mpv_revert_btn", "Revert" },
    // Settings: Audio & Subtitles
    { "settings.audio.preferred_track", "Preferred audio track" }, { "settings.subtitles.preferred_track", "Preferred subtitle track" },
    { "settings.subtitles.font_size", "Subtitle size" }, { "settings.subtitles.text_color", "Subtitle color" },
    { "settings.subtitles.bg_style", "Background style" }, { "settings.subtitles.position", "Vertical position" },
    { "settings.subtitles.size_small", "Small (14px)" }, { "settings.subtitles.size_medium", "Medium (18px)" },
    { "settings.subtitles.size_large", "Large (24px)" }, { "settings.subtitles.size_xlarge", "Extra Large (32px)" },
    { "settings.subtitles.color_white", "White" }, { "settings.subtitles.color_yellow", "Yellow" },
    { "settings.subtitles.color_green", "Green" }, { "settings.subtitles.color_cyan", "Cyan" },
    { "settings.subtitles.color_amber", "Amber" }, { "settings.subtitles.color_coral", "Coral" },
    { "settings.subtitles.bg_vlc", "Outline" }, { "settings.subtitles.bg_translucent", "Translucent box" },
    { "settings.subtitles.bg_solid", "Solid box" }, { "settings.subtitles.bg_clean", "Clean text" },
    { "settings.subtitles.pos_bottom", "Bottom" }, { "settings.subtitles.pos_elevated", "Elevated (above HUD)" },
    { "settings.subtitles.pos_top", "Top" }, { "lang.english", "English" }, { "lang.japanese", "Japanese" }, { "lang.french", "French" },
    { "lang.german", "German" }, { "lang.spanish", "Spanish" },
    // Settings: Keybindings
    { "settings.shortcuts.header", "CUSTOMIZABLE KEYBOARD SHORTCUTS" }, { "settings.shortcuts.reset", "Reset Shortcuts" },
    { "settings.shortcuts.press_key", "[ Press Key... ]" }, { "settings.shortcuts.unassigned", "Unassigned" },
    { "settings.shortcuts.rebind_prompt", "Press any key combination to reassign '{0}'. Press Escape to cancel." },
    // Shortcut Action Display Names
    { "shortcut.togglePlay", "Play / Pause Video" }, { "shortcut.seekForward", "Seek Forward (5s)" },
    { "shortcut.seekBackward", "Seek Backward (5s)" }, { "shortcut.seekForwardLarge", "Seek Forward (10s)" },
    { "shortcut.seekBackwardLarge", "Seek Backward (10s)" }, { "shortcut.volumeUp", "Volume Up" }, { "shortcut.volumeDown", "Volume Down" },
    { "shortcut.toggleMute", "Mute / Unmute Audio" }, { "shortcut.toggleFullscreen", "Toggle Fullscreen" },
    { "shortcut.cycleSubtitles", "Cycle Subtitle Tracks" }, { "shortcut.cycleAudio", "Cycle Audio Tracks" },
    { "shortcut.togglePiP", "Picture-in-Picture Mode" }, { "shortcut.frameStepForward", "Next Video Frame" },
    { "shortcut.frameStepBackward", "Previous Video Frame" }, { "shortcut.speedUp", "Increase Playback Speed" },
    { "shortcut.speedDown", "Decrease Playback Speed" }, { "shortcut.speedReset", "Reset Playback Speed" },
    { "shortcut.playNext", "Play Next Video in Queue" }, { "shortcut.playPrevious", "Play Previous Video in Queue" },
    { "shortcut.nextChapter", "Next Chapter" }, { "shortcut.previousChapter", "Previous Chapter" },
    { "shortcut.restartVideo", "Restart Video" }, { "shortcut.cycleAspectRatio", "Cycle Aspect Ratio" },
    { "shortcut.audioDelayPlus", "Increase Audio Delay (+50ms)" }, { "shortcut.audioDelayMinus", "Decrease Audio Delay (-50ms)" },
    { "shortcut.audioDelayReset", "Reset Audio Delay" }, { "shortcut.subDelayPlus", "Increase Subtitle Delay (+100ms)" },
    { "shortcut.subDelayMinus", "Decrease Subtitle Delay (-100ms)" }, { "shortcut.subDelayReset", "Reset Subtitle Delay" },
    { "shortcut.togglePlaybackDiagnostics", "Toggle Playback Diagnostics" }, { "shortcut.takeScreenshot", "Take Screenshot" },
    { "shortcut.togglePrivateMode", "Toggle Private Session Mode" }, { "shortcut.closePlayer", "Exit Player" },
    { "settings.system.portable_desc", "Your library and preferences stay with this copy of Aperture." },
    // Settings: System & Storage
    { "settings.system.about_header", "APERTURE" }, { "settings.system.actions_header", "PREFERENCES & STORAGE" },
    { "settings.system.open_data", "Saved Data" },
    { "settings.system.open_data_desc", "Open the folder containing your library, playlists, and preferences." },
    { "settings.system.clear_disk_cache", "Clear Cache" },
    { "settings.system.clear_disk_cache_desc", "Free temporary storage. Your library and watch history are kept." },
    { "settings.system.clear_history", "Clear Watch History" },
    { "settings.system.clear_history_desc", "Remove watch history and saved playback positions." },
    { "settings.system.reset_defaults", "Reset Preferences" },
    { "settings.system.reset_defaults_desc", "Restore default appearance, shortcuts, and playback preferences." },
    { "settings.system.hover_previews", "Hover Video Previews" },
    { "settings.system.hover_previews_on", "Play a preview when you hover over a video." },
    { "settings.system.hover_previews_off", "Disabled" }, { "settings.system.reduce_motion", "Reduce Motion" },
    { "settings.system.reduce_motion_on", "Use fewer animations throughout the app." },
    { "settings.system.reduce_motion_off", "Follow Windows accessibility preferences" },
    { "settings.system.check_updates", "Update Aperture" },
    { "settings.system.check_updates_desc", "Install the latest release while keeping your library and settings." }, { "settings.system.smart_series", "Smart Series Detection" },
    { "settings.system.smart_series_on", "Group videos into series and seasons using their filenames." },
    { "settings.system.smart_series_off", "Group videos by the folders they are stored in." },
    { "settings.system.btn.open_data", "Open Data" },
    { "settings.system.btn.clear_disk", "Clear Cache" }, { "settings.system.btn.clear_history", "Clear History" },
    { "settings.system.btn.reset_defaults", "Reset Defaults" }, { "settings.system.btn.enabled", "Enabled" },
    { "settings.system.btn.disabled", "Disabled" }, { "settings.system.btn.check_now", "Update" },
    // Player Overlay & Controls
    { "player.play", "Play" }, { "player.pause", "Pause" }, { "player.stop", "Stop" }, { "player.next", "Next" },
    { "player.previous", "Previous" }, { "player.mute", "Mute" }, { "player.unmute", "Unmute" }, { "player.volume", "Volume" },
    { "player.subtitles", "Subtitles" }, { "player.audio", "Audio" }, { "player.settings", "Settings" },
    { "player.pip", "Picture-in-Picture" }, { "player.fullscreen", "Fullscreen" }, { "player.exit_pip", "Exit PiP" },
    { "player.exit_fullscreen", "Exit Fullscreen" }, { "player.loading_preview", "Loading preview..." },
    { "player.diagnostics_header", "PLAYBACK DIAGNOSTICS" }, { "player.diagnostics_toggle", "[I] Toggle" }, { "player.back", "Back" },
    // Context Menus
    { "menu.play", "Play" }, { "menu.play_item", "Play Video" }, { "menu.pause", "Pause" }, { "menu.resume", "Resume" },
    { "menu.restart", "Restart" }, { "menu.play_in_queue", "Play in Queue" }, { "menu.play_in_queue_multi", "Play in Queue ({0} Videos)" },
    { "menu.mark_watched", "Mark as watched" }, { "menu.mark_watched_multi", "Mark as watched ({0} Videos)" },
    { "menu.mark_unwatched", "Mark as unwatched" }, { "menu.mark_unwatched_multi", "Mark as unwatched ({0} Videos)" },
    { "menu.refresh_metadata", "Refresh Metadata" }, { "menu.refresh_metadata_multi", "Refresh Metadata ({0} Videos)" },
    { "menu.edit_overrides", "Edit Overrides..." }, { "menu.reset_overrides", "Reset Overrides" },
    { "menu.reset_overrides_multi", "Reset Overrides ({0} Videos)" }, { "menu.add_to_playlist", "Add to {0}" },
    { "menu.add_items_to_playlist", "Add {0} items to {1}" }, { "menu.new_playlist", "+ New Playlist..." },
    { "menu.new_smart_playlist", "+ New Smart Playlist..." }, { "menu.select_all", "Select All" },
    { "menu.show_in_explorer", "Show in Explorer" }, { "menu.remove_from_library", "Remove from Library..." },
    { "menu.remove_selected_library", "Remove Selected ({0} Files)..." }, { "menu.open_file", "Open File..." },
    { "menu.add_watched_folder", "Add Watched Folder..." }, { "menu.relocate_missing", "Relocate Missing Media..." },
    { "menu.detect_duplicates", "Detect Duplicate Media..." }, { "menu.rescan_watched", "Rescan Watched Folders" },
    { "menu.stop_watching", "Stop Watching: {0}" }, { "menu.refresh", "Refresh" }, { "menu.settings", "Settings" },
    { "menu.open_playlist", "Open Playlist" }, { "menu.play_all", "Play All" }, { "menu.delete_playlist", "Delete Playlist" },
    { "menu.rename", "Rename" }, { "menu.edit_rules", "Edit Rules..." }, { "menu.remove_from_playlist", "Remove from Playlist" },
    { "menu.remove_from_playlist_multi", "Remove Selected ({0} Videos)" },
    { "menu.remove_from_history_multi", "Remove Selected ({0} Records)" }, { "menu.locate_missing", "Locate Missing File\xE2\x80\xA6" },
    { "menu.next_chapter", "Next Chapter" }, { "menu.prev_chapter", "Previous Chapter" }, { "menu.chapters", "Chapters ({0})..." },
    { "menu.no_chapters", "Chapters (None)" }, { "menu.add_bookmark", "Add Bookmark..." }, { "menu.bookmarks", "Bookmarks ({0})..." },
    { "menu.delete_bookmark", "Delete Bookmark..." }, { "menu.clear_all_bookmarks", "Clear All Bookmarks" },
    { "menu.set_ab_start", "Set A-B Start" }, { "menu.set_ab_end", "Set A-B End" }, { "menu.clear_ab_repeat", "Clear A-B Repeat" },
    { "menu.loop", "Loop: {0}" }, { "menu.content_speed", "Content Speed Presets..." }, { "menu.sleep_timer", "Sleep Timer..." },
    { "menu.stop_in_minutes", "Stop in {0} minutes" }, { "menu.stop_after_current", "Stop After Current" },
    { "menu.cancel_sleep_timer", "Cancel Sleep Timer" }, { "menu.audio_delay", "Audio Delay ({0} ms)..." },
    { "menu.audio_balance", "Audio Balance ({0})..." }, { "menu.channel_mapping", "Channel Mapping ({0})..." },
    { "menu.subtitle_delay", "Subtitle Delay ({0} ms)..." }, { "menu.subtitles_menu", "Subtitles & Dual Subtitles..." },
    { "menu.open_subtitle", "Open Subtitle File..." }, { "menu.cycle_audio", "Cycle Audio Track" },
    { "menu.aspect_ratio", "Aspect Ratio: {0}" }, { "menu.rotation", "Rotation: {0}°" },
    { "menu.video_adjustments", "Video Adjustments..." }, { "menu.take_screenshot", "Take Screenshot" },
    { "menu.show_last_screenshot", "Show Last Screenshot" }, { "menu.exit_player", "Exit Player" }, { "menu.back", "← Back" },
    { "menu.increase", "+{0} (Increase)" }, { "menu.decrease", "−{0} (Decrease)" }, { "menu.reset", "Reset ({0})" },
    { "menu.reset_all", "Reset All Adjustments" },
    // Modals & Dialogs
    { "modal.ok", "OK" }, { "modal.cancel", "Cancel" }, { "modal.apply", "Apply" }, { "modal.save", "Save" }, { "modal.delete", "Delete" },
    { "modal.clear", "Clear" }, { "modal.create", "Create" }, { "modal.rename", "Rename" }, { "modal.new_playlist.title", "New Playlist" },
    { "modal.new_playlist.prompt", "Enter playlist name:" }, { "modal.rename_playlist.title", "Rename Playlist" },
    { "modal.rename_playlist.prompt", "Enter new playlist name:" }, { "modal.delete_playlist.title", "Delete Playlist" },
    { "modal.delete_playlist.message", "Are you sure you want to delete '{0}'?" },
    { "modal.new_smart_playlist.title", "New Smart Playlist" }, { "modal.edit_smart_playlist.title", "Edit Smart Playlist" },
    { "modal.seek_step.title", "Custom Seek Step" }, { "modal.seek_step.prompt", "Enter seek step in seconds (0.5 to 300):" },
    { "modal.frame_step.title", "Custom Frame Seek Step" }, { "modal.frame_step.prompt", "Enter number of frames to jump (1 to 240):" },
    { "modal.add_bookmark.title", "Add Bookmark" }, { "modal.add_bookmark.prompt", "Enter bookmark label ({0}):" },
    { "modal.edit_metadata.title", "Edit Metadata Overrides" },
    { "modal.edit_metadata.prompt", "Enter overrides (Title | Year | Series or Title):" },
    { "modal.clear_history.title", "Clear Watch History" },
    { "modal.clear_history.message", "Erase all watch history and playback progress? You can undo this afterward." },
    { "modal.remove_library.title", "Remove from Library" },
    { "modal.remove_library.message", "Remove {0} selected video(s) from your library? (Source files on disk will NOT be deleted)." },
    { "modal.save_sub_profile.title", "Save Subtitle Style Profile" }, { "modal.save_sub_profile.prompt", "Enter style profile name:" },
    { "modal.rename_sub_profile.title", "Rename Subtitle Style Profile" },
    { "modal.rename_sub_profile.prompt", "Enter new style profile name:" },
    // Global Search
    { "search.placeholder", "Search media, playlists, and history..." }, { "search.no_results", "No results found" },
    { "search.hint", "Press Enter to play, Esc to close" }, { "search.category_media", "Media Files" },
    { "search.category_playlists", "Playlists" }, { "search.category_history", "Watch History" },
    // OSD Badges & Status Announcements
    { "osd.volume", "Volume: {0}%" }, { "osd.muted", "Muted" }, { "osd.unmuted", "Unmuted" }, { "osd.play", "Playing" },
    { "osd.pause", "Paused" }, { "osd.seek", "Seek: {0}" }, { "osd.speed", "Speed: {0}x" }, { "osd.loop_on", "Loop: On" },
    { "osd.loop_off", "Loop: Off" }, { "osd.aspect_ratio", "Aspect Ratio: {0}" }, { "osd.rotate", "Rotation: {0}°" },
    { "osd.audio_track", "Audio: {0}" }, { "osd.sub_track", "Subtitle: {0}" }, { "osd.audio_delay", "Audio Delay: {0} ms" },
    { "osd.sub_delay", "Subtitle Delay: {0} ms" }, { "osd.screenshot_saved", "Screenshot saved to {0}" },
    { "osd.screenshot_failed", "Failed to capture screenshot" }, { "osd.theme_applied", "Theme: {0}" },
    { "osd.language_applied", "Language: {0}" }, { "osd.bookmark_added", "Bookmark added at {0}" }, { "osd.seek_step", "Seek Step: {0}s" },
    { "osd.frame_seek", "Frame Seek: {0} frames" }
};

constexpr PluralEntryView kCanonicalPluralEntries[] = {{"plural.videos", {"", "{0} video", "", "", "", "{0} videos"}},
    { "plural.watched_videos", { "", "{0} watched", "", "", "", "{0} watched" } },
    { "plural.items", { "", "{0} item", "", "", "", "{0} items" } }, { "plural.seconds", { "", "{0} sec", "", "", "", "{0} sec" } },
    { "plural.frames", { "", "{0} frame", "", "", "", "{0} frames" } }, { "plural.rules", { "", "{0} rule", "", "", "", "{0} rules" } },
    { "plural.files", { "", "{0} file", "", "", "", "{0} files" } }, { "plural.records", { "", "{0} record", "", "", "", "{0} records" } },
    { "plural.minutes", { "", "{0} minute", "", "", "", "{0} minutes" } }, { "plural.hours", { "", "{0} hour", "", "", "", "{0} hours" } },
    { "plural.days", { "", "{0} day", "", "", "", "{0} days" } }
};

} // namespace

Localization& Localization::Instance() { static Localization instance; return instance; }

Localization::Localization() {
    rootDir_ = GetExeDir();
    constexpr size_t stringCount = sizeof(kCanonicalStringEntries) / sizeof(kCanonicalStringEntries[0]);
    fallbackUtf8Strings_.reserve(stringCount);
    fallbackWideStrings_.reserve(stringCount);
    for (const auto& entry : kCanonicalStringEntries) {
        fallbackUtf8Strings_.emplace(std::string(entry.key), std::string(entry.value));
        fallbackWideStrings_.emplace(std::string(entry.key), Utf8ToUtf16View(entry.value));
    }
    constexpr size_t pluralCount = sizeof(kCanonicalPluralEntries) / sizeof(kCanonicalPluralEntries[0]);
    fallbackPlurals_.reserve(pluralCount);
    for (const auto& entry : kCanonicalPluralEntries) {
        fallbackPlurals_.emplace(std::string(entry.key),
                                 PluralEntry{std::string(entry.forms.zero), std::string(entry.forms.one), std::string(entry.forms.two),
                                             std::string(entry.forms.few), std::string(entry.forms.many), std::string(entry.forms.other)});
    }
    activeUtf8Strings_ = fallbackUtf8Strings_;
    activeWideStrings_ = fallbackWideStrings_;
    activePlurals_ = fallbackPlurals_;
}

const std::unordered_map<std::string, std::string>& Localization::GetCanonicalEnglishCatalog() {
    static const auto* catalog = []() {
        auto* map = new std::unordered_map<std::string, std::string>();
        constexpr size_t stringCount = sizeof(kCanonicalStringEntries) / sizeof(kCanonicalStringEntries[0]);
        map->reserve(stringCount);
        for (const auto& entry : kCanonicalStringEntries) { map->emplace(std::string(entry.key), std::string(entry.value)); }
        return map;
    }();
    return *catalog;
}

const std::unordered_map<std::string, PluralEntry>& Localization::GetCanonicalEnglishPlurals() {
    static const auto* plurals = []() {
        auto* map = new std::unordered_map<std::string, PluralEntry>();
        constexpr size_t pluralCount = sizeof(kCanonicalPluralEntries) / sizeof(kCanonicalPluralEntries[0]);
        map->reserve(pluralCount);
        for (const auto& entry : kCanonicalPluralEntries) {
            map->emplace(std::string(entry.key),
                         PluralEntry{std::string(entry.forms.zero), std::string(entry.forms.one), std::string(entry.forms.two),
                                     std::string(entry.forms.few), std::string(entry.forms.many), std::string(entry.forms.other)});
        }
        return map;
    }();
    return *plurals;
}
bool Localization::IsRtl() const { std::lock_guard<std::mutex> lock(mutex_); return currentLocaleInfo_.direction == "rtl"; }

std::wstring Localization::BidiIsolateLtr(const std::wstring& str) { if (str.empty()) return str; return L"\x2066" + str + L"\x2069"; }

std::wstring Localization::BidiIsolateRtl(const std::wstring& str) { if (str.empty()) return str; return L"\x2067" + str + L"\x2069"; }

std::string Localization::BidiIsolateLtrUtf8(const std::string& str) {
    if (str.empty()) return str;
    return "\xE2\x81\xA6" + str + "\xE2\x81\xA9";
}
bool Localization::Initialize(const std::wstring& rootDir, const std::string& preferredLocale) {
    std::lock_guard<std::mutex> lock(mutex_);
    rootDir_ = rootDir.empty() ? GetExeDir() : rootDir;
    return LoadLocaleInternal(preferredLocale);
}

std::wstring Localization::ResolveLocalesDir() const { return rootDir_ + L"\\locales"; }

bool Localization::IsValidLocaleTag(const std::string& tag) {
    if (tag.empty() || tag.size() > 32) return false;
    // Disallow directory traversal or illegal path characters
    if (tag.find("..") != std::string::npos || tag.find('/') != std::string::npos || tag.find('\\') != std::string::npos ||
        tag.find(':') != std::string::npos || tag.find('?') != std::string::npos || tag.find('*') != std::string::npos) {
        return false;
    }
    for (char c : tag) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') { return false; }
    }
    return true;
}

std::string Localization::NormalizeLocaleTag(const std::string& raw) {
    if (raw.empty()) return "en-US";
    std::string tag = raw;
    // Replace underscores with hyphens
    std::replace(tag.begin(), tag.end(), '_', '-');
    size_t hyphen = tag.find('-');
    if (hyphen != std::string::npos) {
        std::string lang = tag.substr(0, hyphen);
        std::string region = tag.substr(hyphen + 1);
        for (char& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : region) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return lang + "-" + region;
    } else {
        std::string lang = tag;
        for (char& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lang == "en") return "en-US";
        if (lang == "es") return "es-ES";
        if (lang == "de") return "de-DE";
        if (lang == "fr") return "fr-FR";
        if (lang == "ar") return "ar-SA";
        if (lang == "he") return "he-IL";
        return lang;
    }
}

bool Localization::IsSafeLocalePath(const std::wstring& path, const std::wstring& localesDir) {
    if (path.empty() || localesDir.empty()) return false;
    wchar_t fullPath[MAX_PATH] = { 0 };
    wchar_t fullLocales[MAX_PATH] = { 0 };
    if (!_wfullpath(fullPath, path.c_str(), MAX_PATH)) return false;
    if (!_wfullpath(fullLocales, localesDir.c_str(), MAX_PATH)) return false;
    size_t baseLen = wcslen(fullLocales);
    if (_wcsnicmp(fullPath, fullLocales, baseLen) != 0) return false;
    if (fullPath[baseLen] != L'\\' && fullPath[baseLen] != L'/' && fullPath[baseLen] != L'\0') return false;
    return true;
}

bool Localization::ParseCatalogJson( const std::string& jsonContent, LocaleInfo& outInfo,
    std::unordered_map<std::string, std::string> &outStrings,
    std::unordered_map<std::string, PluralEntry> &outPlurals, std::string &outError) {
    JsonValue root;
    if (!JsonValue::parse(jsonContent, root) || !root.is_object()) { outError = "Malformed JSON syntax in locale file"; return false; }
    outInfo.tag = root.get("tag").as_string(root.get("locale").as_string(""));
    if (outInfo.tag.empty()) { outError = "Missing required 'tag' or 'locale' field"; return false; }
    outInfo.tag = NormalizeLocaleTag(outInfo.tag);
    outInfo.displayName = root.get("displayName").as_string(outInfo.tag);
    outInfo.englishName = root.get("englishName").as_string(outInfo.displayName);
    std::string dirVal = root.has_key("direction") ? root.get("direction").as_string("ltr") : "ltr";
    std::string normDir;
    for (char c : dirVal) { normDir.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c)))); }
    if (normDir.empty()) { normDir = "ltr"; }
    if (normDir != "ltr" && normDir != "rtl") { outError = "Invalid direction metadata: must be 'ltr' or 'rtl'"; return false; }
    outInfo.direction = normDir;
    outInfo.isLoaded = true;
    outStrings.clear();
    outPlurals.clear();
    if (root.has_key("strings") && root.get("strings").is_object()) {
        const auto& strObj = root.get("strings").as_object();
        for (const auto& kv : strObj) {
            if (kv.second.is_string()) { outStrings[kv.first] = kv.second.as_string(); } else if (kv.second.is_object()) {
                // Plural entry inside strings
                PluralEntry pe;
                pe.zero = kv.second.get("zero").as_string("");
                pe.one = kv.second.get("one").as_string("");
                pe.two = kv.second.get("two").as_string("");
                pe.few = kv.second.get("few").as_string("");
                pe.many = kv.second.get("many").as_string("");
                pe.other = kv.second.get("other").as_string("");
                outPlurals[kv.first] = pe;
                if (!pe.other.empty()) { outStrings[kv.first] = pe.other; } else if (!pe.one.empty()) { outStrings[kv.first] = pe.one; }
            }
        }
    }
    if (root.has_key("plurals") && root.get("plurals").is_object()) {
        const auto& plObj = root.get("plurals").as_object();
        for (const auto& kv : plObj) {
            if (kv.second.is_object()) {
                PluralEntry pe;
                pe.zero = kv.second.get("zero").as_string("");
                pe.one = kv.second.get("one").as_string("");
                pe.two = kv.second.get("two").as_string("");
                pe.few = kv.second.get("few").as_string("");
                pe.many = kv.second.get("many").as_string("");
                pe.other = kv.second.get("other").as_string("");
                outPlurals[kv.first] = pe;
            }
        }
    }
    return true;
}

bool Localization::LoadLocaleInternal(const std::string& requestedTag, bool /*isFallbackChain*/) {
    hadFallback_ = false;
    lastDiagnostic_.clear();
    if (!IsValidLocaleTag(requestedTag)) {
        lastDiagnostic_ = "Invalid locale tag: '" + requestedTag + "', falling back to en-US";
        hadFallback_ = true;
    }
    std::string tag = IsValidLocaleTag(requestedTag) ? NormalizeLocaleTag(requestedTag) : "en-US";
    std::wstring localesDir = ResolveLocalesDir();
    // 1. Try loading en-US disk file into fallback catalog first if available
    std::wstring enPath = localesDir + L"\\en-US.json";
    if (FileExists(enPath) && IsSafeLocalePath(enPath, localesDir)) {
        std::string enContent;
        if (ReadTextFile(enPath, enContent).is_ok()) {
            LocaleInfo enInfo;
            std::unordered_map<std::string, std::string> enStrings;
            std::unordered_map<std::string, PluralEntry> enPlurals;
            std::string err;
            if (ParseCatalogJson(enContent, enInfo, enStrings, enPlurals, err)) {
                for (const auto& kv : enStrings) {
                    fallbackUtf8Strings_[kv.first] = kv.second;
                    fallbackWideStrings_[kv.first] = Utf8ToUtf16(kv.second);
                }
                for (const auto& kv : enPlurals) { fallbackPlurals_[kv.first] = kv.second; }
            }
        }
    }
    // 2. If requested tag is canonical English and loaded, set active
    if (tag == "en-US") {
        currentLocale_ = "en-US";
        currentLocaleInfo_ = { "en-US", "English (US)", "English (US)", "ltr", true };
        activeUtf8Strings_ = fallbackUtf8Strings_;
        activeWideStrings_ = fallbackWideStrings_;
        activePlurals_ = fallbackPlurals_;
        return !hadFallback_;
    }
    // 3. Look for requested catalog file on disk (e.g. "es-ES.json" or "es.json")
    std::wstring targetPath = localesDir + L"\\" + Utf8ToUtf16(tag) + L".json";
    if (!FileExists(targetPath)) {
        size_t dash = tag.find('-');
        if (dash != std::string::npos) {
            std::wstring shortPath = localesDir + L"\\" + Utf8ToUtf16(tag.substr(0, dash)) + L".json";
            if (FileExists(shortPath)) { targetPath = shortPath; }
        }
    }
    if (!FileExists(targetPath) || !IsSafeLocalePath(targetPath, localesDir)) {
        lastDiagnostic_ = "Locale file not found: " + tag + ", falling back to en-US";
        hadFallback_ = true;
        currentLocale_ = "en-US";
        currentLocaleInfo_ = { "en-US", "English (US)", "English (US)", "ltr", true };
        activeUtf8Strings_ = fallbackUtf8Strings_;
        activeWideStrings_ = fallbackWideStrings_;
        activePlurals_ = fallbackPlurals_;
        return false;
    }
    std::string content;
    StorageStatus st = ReadTextFile(targetPath, content);
    if (!st.is_ok()) {
        lastDiagnostic_ = "Failed to read locale file: " + tag + " (" + st.message + "), falling back to en-US";
        hadFallback_ = true;
        currentLocale_ = "en-US";
        currentLocaleInfo_ = { "en-US", "English (US)", "English (US)", "ltr", true };
        activeUtf8Strings_ = fallbackUtf8Strings_;
        activeWideStrings_ = fallbackWideStrings_;
        activePlurals_ = fallbackPlurals_;
        return false;
    }
    LocaleInfo info;
    std::unordered_map<std::string, std::string> loadedStrings;
    std::unordered_map<std::string, PluralEntry> loadedPlurals;
    std::string parseErr;
    if (!ParseCatalogJson(content, info, loadedStrings, loadedPlurals, parseErr)) {
        lastDiagnostic_ = "Failed to parse locale file: " + tag + " (" + parseErr + "), falling back to en-US";
        hadFallback_ = true;
        currentLocale_ = "en-US";
        currentLocaleInfo_ = { "en-US", "English (US)", "English (US)", "ltr", true };
        activeUtf8Strings_ = fallbackUtf8Strings_;
        activeWideStrings_ = fallbackWideStrings_;
        activePlurals_ = fallbackPlurals_;
        return false;
    }
    currentLocale_ = info.tag;
    currentLocaleInfo_ = info;
    // Start with fallback baseline to guarantee completeness
    activeUtf8Strings_ = fallbackUtf8Strings_;
    activeWideStrings_ = fallbackWideStrings_;
    activePlurals_ = fallbackPlurals_;
    // Overlay loaded locale strings
    for (const auto& kv : loadedStrings) {
        activeUtf8Strings_[kv.first] = kv.second;
        activeWideStrings_[kv.first] = Utf8ToUtf16(kv.second);
    }
    for (const auto& kv : loadedPlurals) { activePlurals_[kv.first] = kv.second; }
    return true;
}

bool Localization::SetLocale(const std::string& tag) { std::lock_guard<std::mutex> lock(mutex_); return LoadLocaleInternal(tag); }

std::string Localization::GetCurrentLocale() const { std::lock_guard<std::mutex> lock(mutex_); return currentLocale_; }

std::vector<LocaleInfo> Localization::GetAvailableLocales() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return DiscoverLocales(ResolveLocalesDir());
}

std::vector<LocaleInfo> Localization::DiscoverLocales(const std::wstring& localesDir) const {
    std::vector<LocaleInfo> result;
    // Always include built-in / canonical English
    LocaleInfo enDefault{ "en-US", "English (US)", "English (US)", "ltr", true };
    result.push_back(enDefault);
    if (!localesDir.empty() && DirectoryExists(localesDir)) {
        WIN32_FIND_DATAW fd;
        std::wstring pattern = localesDir + L"\\*.json";
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::wstring fName = fd.cFileName;
                    std::wstring fullPath = localesDir + L"\\" + fName;
                    if (IsSafeLocalePath(fullPath, localesDir)) {
                        std::string jsonText;
                        if (ReadTextFile(fullPath, jsonText).is_ok()) {
                            LocaleInfo info;
                            std::unordered_map<std::string, std::string> sMap;
                            std::unordered_map<std::string, PluralEntry> pMap;
                            std::string err;
                            if (ParseCatalogJson(jsonText, info, sMap, pMap, err)) {
                                if (info.tag != "en-US") {
                                    // Check if already in list
                                    bool exists = false;
                                    for (const auto& existing : result) {
                                        if (existing.tag == info.tag) { exists = true; break; }
                                    }
                                    if (!exists) { result.push_back(info); }
                                } else {
                                    // Update display name for en-US if disk catalog provides one
                                    result[0] = info;
                                }
                            }
                        }
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }
    // Sort deterministically: en-US first, then alphabetically by tag
    std::stable_sort(result.begin(), result.end(), [](const LocaleInfo& a, const LocaleInfo& b) {
        if (a.tag == "en-US") return true;
        if (b.tag == "en-US") return false;
        return a.tag < b.tag;
    });
    return result;
}

std::wstring Localization::Get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = activeWideStrings_.find(key);
    if (it != activeWideStrings_.end()) { return it->second; }
    auto fb = fallbackWideStrings_.find(key);
    if (fb != fallbackWideStrings_.end()) { return fb->second; }
    return Utf8ToUtf16(key);
}

std::string Localization::GetUtf8(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = activeUtf8Strings_.find(key);
    if (it != activeUtf8Strings_.end()) { return it->second; }
    auto fb = fallbackUtf8Strings_.find(key);
    if (fb != fallbackUtf8Strings_.end()) { return fb->second; }
    return key;
}

std::wstring Localization::FormatPattern(const std::wstring& pattern, const std::vector<std::wstring>& args) {
    std::wstring result;
    result.reserve(pattern.size() + args.size() * 16);
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == L'{' && i + 1 < pattern.size()) {
            if (pattern[i + 1] == L'{') {
                // Escaped brace '{{' -> '{'
                result.push_back(L'{');
                ++i;
                continue;
            }
            // Parse argument index {0}, {1}, etc.
            size_t closePos = pattern.find(L'}', i + 1);
            if (closePos != std::wstring::npos) {
                std::wstring numStr = pattern.substr(i + 1, closePos - i - 1);
                bool allDigits = !numStr.empty();
                for (wchar_t ch : numStr) {
                    if (!iswdigit(ch)) { allDigits = false; break; }
                }
                if (allDigits) {
                    size_t argIdx = static_cast<size_t>(_wtoi(numStr.c_str()));
                    if (argIdx < args.size()) { result.append(args[argIdx]); } else {
                        // Argument index out of bounds; retain placeholder safely
                        result.append(pattern.substr(i, closePos - i + 1));
                    }
                    i = closePos;
                    continue;
                }
            }
        } else if (pattern[i] == L'}' && i + 1 < pattern.size() && pattern[i + 1] == L'}') {
            // Escaped brace '}}' -> '}'
            result.push_back(L'}');
            ++i;
            continue;
        }
        result.push_back(pattern[i]);
    }
    return result;
}

std::string Localization::FormatPatternUtf8(const std::string& pattern, const std::vector<std::string>& args) {
    std::string result;
    result.reserve(pattern.size() + args.size() * 16);
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 1 < pattern.size()) {
            if (pattern[i + 1] == '{') {
                result.push_back('{');
                ++i;
                continue;
            }
            size_t closePos = pattern.find('}', i + 1);
            if (closePos != std::string::npos) {
                std::string numStr = pattern.substr(i + 1, closePos - i - 1);
                bool allDigits = !numStr.empty();
                for (char ch : numStr) {
                    if (!isdigit(static_cast<unsigned char>(ch))) { allDigits = false; break; }
                }
                if (allDigits) {
                    size_t argIdx = static_cast<size_t>(std::atoi(numStr.c_str()));
                    if (argIdx < args.size()) { result.append(args[argIdx]); } else { result.append(pattern.substr(i, closePos - i + 1)); }
                    i = closePos;
                    continue;
                }
            }
        } else if (pattern[i] == '}' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
            result.push_back('}');
            ++i;
            continue;
        }
        result.push_back(pattern[i]);
    }
    return result;
}

std::wstring Localization::FormatArgs(const std::string& key, const std::vector<std::wstring>& args) const {
    std::wstring pattern = Get(key);
    return FormatPattern(pattern, args);
}

std::string Localization::FormatArgsUtf8(const std::string& key, const std::vector<std::string>& args) const {
    std::string pattern = GetUtf8(key);
    return FormatPatternUtf8(pattern, args);
}

std::string Localization::SelectPluralForm(const PluralEntry& entry, int64_t count) const {
    if (count == 0 && !entry.zero.empty()) return entry.zero;
    if (count == 1 && !entry.one.empty()) return entry.one;
    if (count == 2 && !entry.two.empty()) return entry.two;
    if (!entry.other.empty()) return entry.other;
    if (!entry.one.empty()) return entry.one;
    return "";
}

std::wstring Localization::FormatPluralArgs(const std::string& key, int64_t count, const std::vector<std::wstring>& args) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string pattern;
    auto it = activePlurals_.find(key);
    if (it != activePlurals_.end()) { pattern = SelectPluralForm(it->second, count); }
    if (pattern.empty()) {
        auto fb = fallbackPlurals_.find(key);
        if (fb != fallbackPlurals_.end()) { pattern = SelectPluralForm(fb->second, count); }
    }
    if (pattern.empty()) {
        auto sit = activeUtf8Strings_.find(key);
        if (sit != activeUtf8Strings_.end()) { pattern = sit->second; }
    }
    if (pattern.empty()) { pattern = key; }
    std::vector<std::wstring> actualArgs = args;
    if (actualArgs.empty()) { actualArgs.push_back(std::to_wstring(count)); }
    return FormatPattern(Utf8ToUtf16(pattern), actualArgs);
}

std::string Localization::FormatPluralArgsUtf8(const std::string& key, int64_t count, const std::vector<std::string>& args) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string pattern;
    auto it = activePlurals_.find(key);
    if (it != activePlurals_.end()) { pattern = SelectPluralForm(it->second, count); }
    if (pattern.empty()) {
        auto fb = fallbackPlurals_.find(key);
        if (fb != fallbackPlurals_.end()) { pattern = SelectPluralForm(fb->second, count); }
    }
    if (pattern.empty()) {
        auto sit = activeUtf8Strings_.find(key);
        if (sit != activeUtf8Strings_.end()) { pattern = sit->second; }
    }
    if (pattern.empty()) { pattern = key; }
    std::vector<std::string> actualArgs = args;
    if (actualArgs.empty()) { actualArgs.push_back(std::to_string(count)); }
    return FormatPatternUtf8(pattern, actualArgs);
}

std::string Localization::GetLastDiagnostic() const { std::lock_guard<std::mutex> lock(mutex_); return lastDiagnostic_; }

bool Localization::HadFallback() const { std::lock_guard<std::mutex> lock(mutex_); return hadFallback_; }
} // namespace Aperture
