#include "../../src/App.h"
#include <fstream>
#include <stdexcept>
#include <iostream>
#include <thread>

using namespace Aperture;
static void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void Pump(unsigned milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    do { MSG m{}; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(5); } while (GetTickCount64() < end);
}
template<class Predicate> static void Until(Predicate predicate, const char* message) {
    const auto end = GetTickCount64() + 8000;
    while (!predicate() && GetTickCount64() < end) Pump(15);
    Require(predicate(), message);
}
static LPARAM Point(int x, int y) { return MAKELPARAM(x, y); }
static LRESULT CALLBACK HarnessProc(HWND, UINT, WPARAM, LPARAM);

namespace Aperture {
struct PlaybackUiTestAccess {
    static int passed;
    static std::wstring output;
    static App* current;
    static bool IsOverlay(const App& a, HWND window) { return window == a.hOverlayWnd; }
    static void NoVisibleWindows() {
        bool visible = false;
        EnumWindows([](HWND window, LPARAM arg) -> BOOL {
            DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(window)) {
                *reinterpret_cast<bool*>(arg) = true;
                wchar_t name[128]{}; GetClassNameW(window, name, 128);
                std::wcerr << L"Visible test window class: " << name << std::endl;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&visible));
        Require(!visible, "A test window became visible");
    }
    static void Pass(const char* name) { NoVisibleWindows(); ++passed; std::cout << "PASS " << name << std::endl; }
    static void Capture(App& a, const wchar_t* name) {
        a.flyoutAnim.SnapTo(a.isSettingsFlyoutOpen ? 1.0f : 0.0f); a.contextMenuAnim.SnapTo(a.isContextMenuOpen ? 1.0f : 0.0f);
        a.flyoutClosing = a.contextMenuClosing = false; a.osdTimer = 0;
        Pump(25); a.overlayRenderer.RequestRedraw(); a.Render();
        auto& r = a.overlayRenderer;
        Require(r.pBitmapBits && r.width && r.height, "No overlay pixels to capture");
        ComPtr<IWICImagingFactory> factory;
        Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");
        ComPtr<IWICBitmap> bitmap;
        Require(SUCCEEDED(factory->CreateBitmapFromMemory(r.width, r.height, GUID_WICPixelFormat32bppPBGRA, r.width * 4,
            r.width * r.height * 4, static_cast<BYTE*>(r.pBitmapBits), &bitmap)), "WIC bitmap");
        ComPtr<IWICFormatConverter> converter; factory->CreateFormatConverter(&converter);
        Require(SUCCEEDED(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)), "WIC conversion");
        ComPtr<IWICStream> stream; factory->CreateStream(&stream);
        Require(SUCCEEDED(stream->InitializeFromFilename((output + L"\\" + name + L".png").c_str(), GENERIC_WRITE)), "Capture output");
        ComPtr<IWICBitmapEncoder> encoder; factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        Require(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)), "PNG encoder");
        ComPtr<IWICBitmapFrameEncode> frame; encoder->CreateNewFrame(&frame, nullptr); frame->Initialize(nullptr); frame->SetSize(r.width, r.height);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA; frame->SetPixelFormat(&format);
        Require(SUCCEEDED(frame->WriteSource(converter.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()), "PNG write");
        NoVisibleWindows();
    }
    static void Key(App& a, UINT key, bool shift = false, bool ctrl = false) {
        BYTE previous[256]{}, state[256]{}; GetKeyboardState(previous);
        if (shift) state[VK_SHIFT] = 0x80;
        if (ctrl) state[VK_CONTROL] = 0x80;
        SetKeyboardState(state);
        a.HandleGlobalKeyDown(key, 0);
        SetKeyboardState(previous);
    }
    static void Text(App& a, const wchar_t* text) {
        for (const wchar_t* ch = text; *ch; ++ch) {
            const SHORT key = VkKeyScanW(*ch);
            if (key != -1) Key(a, LOBYTE(key), (HIBYTE(key) & 1) != 0);
            a.HandleMessage(a.hWnd, WM_CHAR, *ch, 0);
        }
    }
    static void Flyout(App& a, const char* menu) {
        a.CloseContextMenu(); a.contextMenuClosing = false;
        a.isSettingsFlyoutOpen = true; a.currentFlyoutMenu = menu;
        a.flyoutClosing = false; a.flyoutAnim.SnapTo(1); a.hoveredFlyoutOption = PlaybackFlyoutLayout::None;
    }
    static void Setup(App& a, const std::wstring& media) {
        current = &a;
        WNDCLASSW wc{}; wc.lpfnWndProc = HarnessProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ApertureHiddenPlaybackUiTests";
        RegisterClassW(&wc);
        a.hWnd = CreateWindowExW(0, wc.lpszClassName, L"Hidden playback test", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1024, 640, nullptr, nullptr, wc.hInstance, nullptr);
        Require(a.hWnd != nullptr, "Hidden owner creation");
        a.hVideoWnd = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1024, 640, a.hWnd, nullptr, wc.hInstance, nullptr);
        a.hOverlayWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Hidden overlay", WS_POPUP, 0, 0, 1024, 640, a.hWnd, nullptr, wc.hInstance, nullptr);
        SetWindowLongPtrW(a.hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&a));
        SetWindowLongPtrW(a.hOverlayWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&a));
        a.hInstance = wc.hInstance;
        a.settings.muted = true; a.settings.volume = 0;
        a.playbackSpeedInput.SetMaxLength(32);
        Require(a.renderer.Initialize(a.hWnd, a.settings.uiFontFamily, false), "CPU renderer initialization");
        Require(a.EnsureOverlayReady(), "Hidden overlay initialization");
        a.appInitialized = true;
        Require(a.player.Initialize(a.hWnd, a.hVideoWnd), "Player initialization");
        a.playerInitialized = true; a.player.SetMuted(true); a.player.SetVolume(0);
        MediaItem item; item.filePath = media; item.title = L"Playback UI fixture"; item.durationSec = 60;
        a.library.mediaItems.push_back(item); a.library.RebuildPathIndex();
        Require(a.playbackSession.OpenDirect(media), "Session initialization");
        Require(a.player.Open(media), "Media open");
        Until([&]() { return a.player.GetDuration() > 1 && a.player.IsPlaying(); }, "Media duration timeout");
        a.playbackSession.OnPlayerOpened(true);
        NoVisibleWindows();
        Pass("hidden owner, overlay and muted native player");
    }
    static void Controls(App& a) {
        a.playerKeyboardFocusId = 9006; a.isKeyboardNavActive = true;
        Key(a, VK_RETURN); Require(a.isSettingsFlyoutOpen, "Enter did not open focused settings");
        const bool before = a.player.IsPlaying();
        Key(a, VK_SPACE); Until([&]() { return a.player.IsPlaying() != before; }, "Space was captured by settings");
        Require(a.isSettingsFlyoutOpen, "Space activated a flyout option");
        Key(a, VK_ESCAPE); Require(!a.isSettingsFlyoutOpen, "Escape did not close flyout");
        const bool closedState = a.player.IsPlaying();
        Key(a, VK_SPACE); Until([&]() { return a.player.IsPlaying() != closedState; }, "Space stayed on the settings button");
        Require(!a.isSettingsFlyoutOpen, "Space reopened focused settings");
        a.player.Pause();
        Pass("Space remains play/pause with settings open and after closing");
        const float left = 1024 - 300, top = 640 - 86 - 340;
        for (const char* menu : { "queue", "aspect", "speed", "audio", "subtitles" }) {
            Flyout(a, menu);
            for (float y : { 2.0f, 46.0f, 330.0f }) {
                a.RefreshOverlayHitTargets(left + 140, top + y);
                Require(a.hoveredFlyoutOption == PlaybackFlyoutLayout::None, "Blank area hovered Back or a row");
                a.HandleFlyoutClick(140, y);
                Require(a.currentFlyoutMenu == menu, "Blank area changed flyout page");
            }
            a.RefreshOverlayHitTargets(left + 140, top + 25);
            Require(a.hoveredFlyoutOption == PlaybackFlyoutLayout::Back, "Actual Back header is not hittable");
        }
        Pass("blank flyout areas stay inert on all five pages");
        Flyout(a, "speed"); a.HandleFlyoutClick(30, 120);
        Require(a.currentFlyoutMenu == "speed", "Speed preset closed page");
        Until([&]() { return std::abs(a.player.GetPlaybackRate() - 0.25) < 0.001; }, "Preset speed not applied");
        Pass("speed presets apply while retaining the speed page");
        a.HandleFlyoutClick(140, 65); Text(a, L"1.37"); Key(a, VK_RETURN);
        Until([&]() { return std::abs(a.player.GetPlaybackRate() - 1.37) < 0.001; }, "Typed speed not applied");
        Require(a.currentFlyoutMenu == "speed" && a.activeTextInputField == App::TextInputField::None, "Typed speed did not finish in place");
        Pass("direct numeric speed entry");
        const double savedSpeed = *a.library.mediaItems[0].preferences->playbackSpeed;
        a.HandleFlyoutClick(140, 95);
        Require(a.isPlaybackSpeedDragging, "Slider did not capture drag");
        a.HandleOverlayMessage(a.hOverlayWnd, WM_MOUSEMOVE, MK_LBUTTON, Point(1200, 309));
        Require(a.isPlaybackSpeedDragging && std::abs(a.playbackSpeedDragValue - 3.0) < 0.001, "Captured slider does not clamp outside bounds");
        Require(*a.library.mediaItems[0].preferences->playbackSpeed == savedSpeed, "Drag persisted preferences during movement");
        a.HandleOverlayMessage(a.hOverlayWnd, WM_LBUTTONUP, 0, Point(864, 309));
        Require(!a.isPlaybackSpeedDragging && a.currentFlyoutMenu == "speed", "Drag did not finish in place");
        Require(std::abs(*a.library.mediaItems[0].preferences->playbackSpeed - a.playbackSpeedDragValue) < 0.001, "Final drag speed not persisted");
        Pass("captured speed drag previews without preference writes and commits on release");
        a.HandleFlyoutClick(140, 65); Text(a, L"nan"); const double valid = a.player.GetPlaybackRate(); Key(a, VK_RETURN);
        Require(a.activeTextInputField == App::TextInputField::PlaybackSpeed && a.player.GetPlaybackRate() == valid, "Invalid speed escaped validation");
        Key(a, VK_ESCAPE); Require(a.activeTextInputField == App::TextInputField::None, "Escape did not cancel speed editing");
        Capture(a, L"speed"); Pass("invalid speed remains editable and Escape cancels");
    }
    static void Subtitles(App& a) {
        const auto dir = GetExeDir();
        for (const auto* filename : { L"one.srt", L"two.srt" }) {
            std::wofstream f(dir + L"\\" + filename); f << L"1\n00:00:00,000 --> 00:00:59,000\nCaption fixture\n"; f.close();
            Require(a.player.LoadExternalSubtitle(dir + L"\\" + filename), "External subtitle load");
        }
        Until([&]() { return a.player.GetSubtitleTracks().size() >= 2; }, "Two subtitle tracks timeout");
        const auto track = a.player.GetSubtitleTracks().back();
        a.commandDispatcher.Execute(AppCommand::SelectSubtitleTrack, CommandArgs{ 0, track.id, "", track.title, track.lang });
        Until([&]() { return a.player.GetCurrentSubtitleTrackId() == track.id && a.player.GetSubtitleVisibility(); }, "Subtitle selection");
        a.CloseSettingsFlyout(); a.flyoutClosing = false;
        const auto controls = Presentation::Player(1024, 640, 1, false);
        a.HandleOverlayMessage(a.hOverlayWnd, WM_LBUTTONDOWN, MK_LBUTTON,
            Point(static_cast<int>((controls.subtitles.left + controls.subtitles.right) / 2), static_cast<int>((controls.subtitles.top + controls.subtitles.bottom) / 2)));
        Until([&]() { return !a.player.GetSubtitleVisibility(); }, "CC did not switch off");
        Require(a.player.GetCurrentSubtitleTrackId() == track.id, "CC cycled the selected track");
        a.commandDispatcher.Execute(AppCommand::ToggleSubtitles);
        Until([&]() { return a.player.GetSubtitleVisibility() && a.player.GetCurrentSubtitleTrackId() == track.id; }, "CC did not restore selected track");
        Pass("CC toggles visibility without cycling the selected subtitle track");
    }
    static void Bookmark(App& a) {
        a.CloseSettingsFlyout(); a.ShowAddBookmarkModal();
        Require(a.BuildPlayerRenderViewModel().modal.type == ModalType::BookmarkInput, "Playback does not include bookmark modal");
        const bool playing = a.player.IsPlaying(); Text(a, L"Scene A");
        Require(Text16ToWideString(a.modalInput.GetText()) == L"Scene A", "Space activated bookmark dialog instead of typing");
        Require(a.player.IsPlaying() == playing, "Typing a bookmark toggled playback");
        Capture(a, L"bookmark");
        a.HandleOverlayMessage(a.hOverlayWnd, WM_LBUTTONDOWN, MK_LBUTTON, Point(672, 384));
        a.HandleOverlayMessage(a.hOverlayWnd, WM_LBUTTONUP, 0, Point(672, 384));
        const auto* item = a.library.FindByPath(a.playbackSession.GetCurrentFilePath());
        Require(item && !item->bookmarks.empty() && item->bookmarks.back().label == L"Scene A", "Bookmark not added in playback");
        Require(a.activeModal == ModalType::None && a.playbackSession.IsOpen(), "Bookmark left playback");
        Pass("bookmark dialog renders, accepts spaces and saves without leaving playback");
    }
    static void RepeatAndMenu(App& a) {
        a.player.Seek(2, true); Until([&]() { return std::abs(a.player.GetCurrentTime() - 2) < 0.2; }, "Seek to A");
        Key(a, 'A', true); Require(a.abLoopA >= 0 && a.abLoopB < 0, "Shift+A did not set A");
        a.player.Seek(4, true); Until([&]() { return std::abs(a.player.GetCurrentTime() - 4) < 0.2; }, "Seek to B");
        Key(a, 'A', true); Require(a.abLoopB > a.abLoopA, "Shift+A did not set B");
        a.OpenPlayerAdvancedContextMenu(350, 250);
        Require(std::count_if(a.contextMenuItems.begin(), a.contextMenuItems.end(), [](const ContextMenuItem& i) { return i.id == "ab_cycle"; }) == 1, "A/B is not one menu action");
        Require(std::none_of(a.contextMenuItems.begin(), a.contextMenuItems.end(), [](const ContextMenuItem& i) { return i.id == "content_speed" || i.id == "ab_set_a" || i.id == "ab_set_b"; }), "Redundant menu action remains");
        const auto oldIds = a.contextMenuItems; const auto oldPosition = a.contextMenuOrigin;
        a.HandleOverlayMessage(a.hOverlayWnd, WM_RBUTTONDOWN, 0, Point(650, 300));
        Require(a.contextMenuOrigin.x != oldPosition.x, "Second right-click did not move menu");
        Require(a.contextMenuItems.size() == oldIds.size() && a.contextMenuItems[0].id == oldIds[0].id, "Second right-click changed submenu");
        auto ab = std::find_if(a.contextMenuItems.begin(), a.contextMenuItems.end(), [](const ContextMenuItem& i) { return i.id == "ab_cycle"; });
        Require(ab != a.contextMenuItems.end() && ab->shortcut == FormatDuration(a.abLoopA) + L"\u2013" + FormatDuration(a.abLoopB), "A/B times absent");
        a.commandDispatcher.Execute(AppCommand::SetSpeed, CommandArgs{ 1.0 });
        a.commandDispatcher.Execute(AppCommand::Play);
        double previous = a.player.GetCurrentTime(); bool wrapped = false;
        for (int i = 0; i < 24; ++i) {
            Pump(100); const double now = a.player.GetCurrentTime();
            if (now + 0.3 < previous) wrapped = true;
            Require(now <= a.abLoopB + 0.5, "A/B playback escaped the repeat interval");
            previous = now;
        }
        Require(wrapped, "Native playback did not repeat A/B interval");
        Pass("native A/B playback loops until cleared");
        Capture(a, L"repeat-menu"); Key(a, 'A', true); Require(a.abLoopA < 0 && a.abLoopB < 0, "Shift+A did not clear repeat");
        a.player.Seek(3.8, true);
        Until([&]() { return a.player.GetCurrentTime() > 4.5; }, "Clearing A/B did not release native loop");
        a.CloseContextMenu(); Pass("one timed A/B action cycles via Shift+A and submenu repositions intact");
    }
    static void MoveUndo(App& a) {
        a.currentTab = TabView::Playlists;
        Playlist p; p.id = "ui_second"; p.name = "Second"; p.itemPaths = { L"one.mp4", L"two.mp4", L"three.mp4" };
        a.library.playlists.push_back(p);
        const auto before = a.library.playlists.front().id;
        a.dragRailSnapshot.clear(); for (const auto& pl : a.library.playlists) a.dragRailSnapshot.push_back(pl.id);
        a.activeDragType = App::DragType::PlaylistRail; a.dragFromIndex = 0; a.dragToIndex = 1;
        a.HandleMessage(a.hWnd, WM_LBUTTONUP, 0, Point(200, 400));
        Require(a.library.playlists[1].id == before, "Playlist drag did not reorder");
        Key(a, 'Z', false, true); Require(a.library.playlists.front().id == before, "Ctrl+Z did not restore playlist order");
        Pass("Hub Ctrl+Z restores playlist rail order");
        a.activePlaylistId = p.id; a.dragPlaylistId = p.id; a.dragPathSnapshot = p.itemPaths;
        a.activeDragType = App::DragType::PlaylistItem; a.dragFromIndex = 0; a.dragToIndex = 2;
        a.HandleMessage(a.hWnd, WM_LBUTTONUP, 0, Point(500, 400));
        Require(a.library.FindPlaylist(p.id)->itemPaths[2] == L"one.mp4", "Playlist item drag did not reorder");
        Key(a, 'Z', false, true); Require(a.library.FindPlaylist(p.id)->itemPaths == p.itemPaths, "Ctrl+Z did not restore playlist item order");
        Pass("Hub Ctrl+Z restores playlist item order");
        a.OpenBackgroundContextMenu(100, 200); const auto position = a.contextMenuOrigin;
        const auto firstId = a.contextMenuItems.front().id;
        a.HandleMessage(a.hWnd, WM_RBUTTONDOWN, 0, Point(650, 200));
        Require(a.contextMenuOrigin.x != position.x && a.contextMenuItems.front().id == firstId, "Hub menu did not relocate intact");
        a.CloseContextMenu(); Pass("Hub right-click relocates existing context menu");
    }
    static void RenameUndo(App& a) {
        a.playbackSession.Close();
        Playlist p; p.id = "ui_playlist"; p.name = "Before"; a.library.playlists.push_back(p);
        a.currentTab = TabView::Playlists; a.StartInlinePlaylistRename(p.id);
        a.inlineRenameInput.SetText(u"After"); a.CommitInlinePlaylistRename();
        Require(a.library.FindPlaylist(p.id)->name == "After", "Rename not applied");
        a.library.mediaItems[0].lastPosSec = 17;
        Key(a, 'Z', false, true);
        Require(a.library.FindPlaylist(p.id)->name == "Before", "Ctrl+Z did not undo rename");
        Require(a.library.mediaItems[0].lastPosSec == 17, "Rename undo restored stale playback progress");
        Pass("Hub Ctrl+Z undoes rename without reverting unrelated progress");
    }
    static void MediaAndFolderUndo(App& a) {
        a.currentTab = TabView::Media;
        a.libraryGroupMode = LibraryGroupMode::Flat; a.librarySortField = LibrarySortField::Title;
        a.isLastWatchedCleared = true;
        for (const auto* title : { L"Z last", L"A first" }) {
            MediaItem item; item.title = title;
            item.filePath = GetExeDir() + L"\\" + title + L"\\fixture.mp4";
            a.library.mediaItems.push_back(item);
        }
        a.library.RebuildPathIndex(); a.InvalidateLibraryDerivedState();
        std::vector<std::wstring> before;
        for (const auto& item : a.library.mediaItems) before.push_back(item.filePath);
        a.dragPathSnapshot.clear();
        for (size_t index : a.GetVisibleMediaIndices()) a.dragPathSnapshot.push_back(a.library.mediaItems[index].filePath);
        const auto moved = a.dragPathSnapshot.front();
        a.activeDragType = App::DragType::MediaList; a.dragFromIndex = 0; a.dragToIndex = 2;
        a.HandleMessage(a.hWnd, WM_LBUTTONUP, 0, Point(500, 400));
        Require(a.library.mediaItems[2].filePath == moved, "Media drag did not reorder visible items");
        a.library.mediaItems[2].lastPosSec = 23;
        Key(a, 'Z', false, true);
        for (size_t i = 0; i < before.size(); ++i) Require(a.library.mediaItems[i].filePath == before[i], "Media undo did not restore raw order");
        Require(a.library.FindByPath(moved)->lastPosSec == 23, "Media undo reverted progress");
        Pass("Hub Ctrl+Z restores media order when view and storage orders differ");
        a.libraryGroupMode = LibraryGroupMode::Folder; a.InvalidateLibraryDerivedState();
        const auto beforeFolders = a.settings.customFolderOrder;
        Require(a.GetCurrentMediaProjection().groups.size() >= 2, "Folder fixtures missing");
        a.activeDragType = App::DragType::MediaFolder; a.dragFromIndex = 0; a.dragToIndex = 1;
        a.HandleMessage(a.hWnd, WM_LBUTTONUP, 0, Point(500, 400));
        Require(a.settings.customFolderOrder != beforeFolders, "Folder order did not change");
        Key(a, 'Z', false, true); Require(a.settings.customFolderOrder == beforeFolders, "Folder order undo failed");
        Pass("Hub Ctrl+Z restores folder order");
        const auto mediaCount = a.library.mediaItems.size();
        a.selectedFolderKeys.insert(a.GetCurrentMediaProjection().groups.front().key);
        a.RemoveSelectedFolders();
        Require(a.library.mediaItems.size() < mediaCount, "Folder removal did not remove library entries");
        Key(a, 'Z', false, true);
        Require(a.library.mediaItems.size() == mediaCount, "Folder removal undo failed");
        Pass("Hub Ctrl+Z restores removed folder entries");
    }
    static void EditHistory(App& a) {
        a.currentTab = TabView::Playlists;
        const auto path = a.library.mediaItems.front().filePath;
        a.library.mediaItems.front().status = "new"; a.library.mediaItems.front().lastPosSec = 7;
        const auto oldItems = a.library.FindPlaylist("ui_playlist")->itemPaths;
        Require(a.SetWatchedWithUndo({path}, true) == 1, "Watched edit failed");
        Require(a.AddToPlaylistWithUndo("ui_playlist", {path}) == 1, "Playlist addition failed");
        a.StartInlinePlaylistRename("ui_playlist"); a.inlineRenameInput.SetText(u"Chained"); a.CommitInlinePlaylistRename();
        Key(a, 'Z', false, true); Require(a.library.FindPlaylist("ui_playlist")->name == "Before", "Chained rename undo failed");
        Key(a, 'Z', false, true); Require(a.library.FindPlaylist("ui_playlist")->itemPaths == oldItems, "Chained addition undo failed");
        Key(a, 'Z', false, true); Require(a.library.FindByPath(path)->status == "new" && a.library.FindByPath(path)->lastPosSec == 7, "Chained watched undo failed");
        Pass("Ctrl+Z history reverses rename, playlist addition and watched state in order");
        a.ShowNewPlaylistModal(); a.modalInput.SetText(u"Undo creation"); a.modalOnConfirm();
        const auto created = a.activePlaylistId;
        Require(a.library.FindPlaylist(created) != nullptr, "Playlist creation failed");
        Key(a, 'Z', false, true); Require(!a.library.FindPlaylist(created), "Playlist creation undo failed");
        Pass("Ctrl+Z reverses playlist creation");
        const auto criteria = SmartPlaylistCriteria::FromRuleString("status = new");
        const auto smartId = a.library.CreateSmartPlaylist("Rules fixture", criteria)->id;
        a.ShowEditSmartPlaylistModal(smartId); a.modalInput.SetText(u"duration > 10m"); a.modalOnConfirm();
        Require(a.library.FindPlaylist(smartId)->smartCriteria.ToRuleString() != criteria.ToRuleString(), "Smart rules edit failed");
        Key(a, 'Z', false, true);
        Require(a.library.FindPlaylist(smartId)->smartCriteria.ToRuleString() == criteria.ToRuleString(), "Smart rules undo failed");
        Pass("Ctrl+Z restores smart playlist rules");
        a.SetWatchedWithUndo({path}, true); a.library.FindByPath(path)->lastPosSec = 19;
        Key(a, 'Z', false, true);
        Require(a.library.FindByPath(path)->lastPosSec == 19 && a.libraryUndoHistory.empty(), "Stale undo overwrote later progress");
        Pass("Stale undo refuses to overwrite newer progress");
        a.library.FindByPath(path)->status = "new"; a.library.FindByPath(path)->lastPosSec = 7;
        a.activePlaylistId = "ui_playlist"; a.library.FindPlaylist(a.activePlaylistId)->itemPaths = {path};
        a.selectedEpisodeIndices.clear(); a.selectedEpisodeIndices.insert(0);
        a.OpenPlaylistItemContextMenu(0, 100, 200);
        auto watched = std::find_if(a.contextMenuItems.begin(), a.contextMenuItems.end(), [](const auto& item) { return item.id == "mark_watched"; });
        Require(watched != a.contextMenuItems.end() && watched->onClick, "Playlist watched action missing");
        auto action = watched->onClick; a.CloseContextMenu(); action();
        Require(a.library.FindByPath(path)->status == "done", "Playlist watched action failed");
        Key(a, 'Z', false, true); Require(a.library.FindByPath(path)->status == "new", "Playlist watched action did not arm undo");
        Pass("Playlist watched menu actions participate in Ctrl+Z history");
        const auto historyCount = a.library.history.size();
        HistoryRecord record; record.filePath = path; record.title = L"History fixture";
        a.library.history.push_back(record); a.currentTab = TabView::History;
        a.OpenHistoryContextMenu(record, 100, 200);
        auto remove = std::find_if(a.contextMenuItems.begin(), a.contextMenuItems.end(), [](const auto& item) { return item.id == "remove"; });
        Require(remove != a.contextMenuItems.end() && remove->onClick, "History remove action missing");
        action = remove->onClick; a.CloseContextMenu(); action();
        Require(a.library.history.size() == historyCount, "History removal failed");
        Key(a, 'Z', false, true); Require(a.library.history.size() == historyCount + 1, "History removal undo failed");
        Pass("Ctrl+Z restores an individual history entry");
        a.libraryUndoHistory.clear();
        for (int i = 0; i < 40; ++i) a.ArmLibraryUndo(L"Bound", []() { return true; });
        Require(a.libraryUndoHistory.size() == 32, "Undo entry bound failed");
        a.ArmLibraryUndo(L"Large", []() { return true; }, 5 * 1024 * 1024);
        a.ArmLibraryUndo(L"Large", []() { return true; }, 5 * 1024 * 1024);
        Require(a.libraryUndoHistory.size() == 1, "Undo payload bound failed");
        a.libraryUndoHistory.clear();
        Pass("Undo history bounds entry count and retained payload");
    }
    static void Run(App& a, const std::wstring& media) {
        Setup(a, media); Controls(a); Subtitles(a); Bookmark(a); RepeatAndMenu(a); RenameUndo(a); MoveUndo(a); MediaAndFolderUndo(a); EditHistory(a); NoVisibleWindows();
    }
    static void Release(App& a) {
        if (a.hWnd) SetWindowLongPtrW(a.hWnd, GWLP_USERDATA, 0);
        const auto owner = a.hWnd; a.Shutdown();
        if (owner) DestroyWindow(owner);
        current = nullptr;
    }
};
int PlaybackUiTestAccess::passed = 0;
std::wstring PlaybackUiTestAccess::output;
App* PlaybackUiTestAccess::current = nullptr;
}
static LRESULT CALLBACK HarnessProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!app) return DefWindowProcW(window, message, w, l);
    return PlaybackUiTestAccess::IsOverlay(*app, window) ? app->HandleOverlayMessage(window, message, w, l) : app->HandleMessage(window, message, w, l);
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::cerr << "Usage: playback-ui-tests.exe <media> <new-output-dir>\n"; return 2; }
    OleInitialize(nullptr);
    PlaybackUiTestAccess::output = argv[2];
    auto app = std::make_unique<App>(); int result = 0;
    try { PlaybackUiTestAccess::Run(*app, argv[1]); }
    catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << std::endl; result = 1; }
    PlaybackUiTestAccess::Release(*app); app.reset(); OleUninitialize();
    std::cout << PlaybackUiTestAccess::passed << " hidden interaction checks passed\n"; return result;
}
