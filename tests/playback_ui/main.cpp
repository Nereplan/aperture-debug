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
    static double FrameSeconds() {
        LARGE_INTEGER now{}, frequency{}; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
        return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
    }
    static LRESULT CALLBACK ObserveTooltip(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        if (message == TTM_HITTESTA || message == TTM_HITTESTW) ++*reinterpret_cast<unsigned*>(data);
        return DefSubclassProc(window, message, w, l);
    }
    static std::wstring PlayTooltip(const App& a) {
        wchar_t text[256]{};
        TOOLINFOW info{}; info.cbSize = sizeof(info); info.hwnd = a.hOverlayWnd; info.uId = 2002; info.lpszText = text;
        SendMessageW(a.playerTooltips.GetHandle(), TTM_GETTEXTW, 256, reinterpret_cast<LPARAM>(&info));
        return text;
    }
    static void PacedInputBookkeeping(App& a) {
        auto& r = a.overlayRenderer;
        const auto savedShortcut = a.settings.shortcuts.find("togglePlay");
        const bool hadShortcut = savedShortcut != a.settings.shortcuts.end();
        const ShortcutBinding original = hadShortcut ? savedShortcut->second : ShortcutBinding{};
        a.settings.shortcuts["togglePlay"].vkCode = VK_F8;
        a.controlsOpacity = 1.0f; a.controlsIdleTimer = 0.0f; a.playerKeyboardFocusId = 9002;
        const auto layout = Presentation::Player(1024, 640, r.GetDpiScale(), false);
        const auto center = [](const D2D1_RECT_F& rect) { return Point(static_cast<int>((rect.left + rect.right) * .5f), static_cast<int>((rect.top + rect.bottom) * .5f)); };
        const LPARAM play = center(layout.play), settings = center(layout.settings);
        a.RefreshOverlayHitTargets(static_cast<float>(GET_X_LPARAM(play)), static_cast<float>(GET_Y_LPARAM(play)));
        r.framePacer.Reset(); a.MarkAccessibilityDirty(); a.Render();
        Require(a.playerTooltips.IsCreated() && a.playerTooltips.IsActive(), "Player tooltip setup failed");
        Require(PlayTooltip(a).find(L"F8") != std::wstring::npos, "Initial playback shortcut missing from tooltip");
        Require(r.pBitmapBits && r.width && r.height, "No initial paced overlay pixels");
        const size_t hoverPixel = static_cast<size_t>((layout.settings.top + layout.settings.bottom) * .5f) * r.width +
            static_cast<size_t>(layout.settings.left + 2 * r.GetDpiScale());
        const auto initialPixel = static_cast<const uint32_t*>(r.pBitmapBits)[hoverPixel];
        const auto frames = r.GetFrameStatistics().framesDrawn;
        const auto signature = a.playerTooltipSignature;
        const auto initialTooltip = PlayTooltip(a);
        const double cadenceCheck = r.nextCadenceCheckSeconds;
        // Control the existing private clock state, rather than racing a real
        // 16ms interval or slowing the flood enough to accept an accidental frame.
        const double now = FrameSeconds();
        r.nextCadenceCheckSeconds = now + 3600;
        r.framePacer.FrameStarted(now + 3600);
        Require(a.hubTooltips.Create(a.hWnd) && a.hubTooltips.SetActive(true), "Hidden hub tooltip transition setup failed");
        a.accessibilityLastRefreshMs = 0; a.MarkAccessibilityDirty();
        a.playerKeyboardFocusId = 9006;
        struct TooltipProbe {
            HWND window; unsigned hitTests = 0;
            explicit TooltipProbe(HWND value) : window(value) {
                Require(SetWindowSubclass(window, ObserveTooltip, 1, reinterpret_cast<DWORD_PTR>(&hitTests)) != FALSE, "Tooltip relay observer setup failed");
            }
            ~TooltipProbe() { RemoveWindowSubclass(window, ObserveTooltip, 1); }
        } probe(a.playerTooltips.GetHandle());
        for (int i = 0; i < 256; ++i) {
            a.settings.shortcuts["togglePlay"].vkCode = (i & 1) ? VK_F9 : VK_F10;
            const LPARAM point = (i & 1) ? settings : play;
            a.HandleOverlayMessage(a.hOverlayWnd, WM_MOUSEMOVE, 0, point);
            Require(a.hoveredPlayerButtonId == ((i & 1) ? 5 : 1), "Pacing deferred immediate mouse hit testing");
            Require(!a.hubTooltips.IsActive(), "Rejected playback frame kept hub tooltips active");
        }
        Require(probe.hitTests >= 256, "Pacing suppressed tooltip mouse relay");
        Require(a.accessibilityLastRefreshMs == 0 && a.accessibilityDirty, "Rejected frames refreshed accessibility");
        Require(a.playerTooltipSignature == signature && PlayTooltip(a) == initialTooltip, "Rejected frames refreshed tooltips");
        Require(r.GetFrameStatistics().framesDrawn == frames && r.HasPendingFrame(), "Input flood drew early or lost its final frame");
        Require(r.frameTimer && WaitForSingleObject(r.frameTimer, 0) == WAIT_TIMEOUT, "Rejected frame did not retain its deadline timer");
        // Commands remain synchronous even while their visual frame is pending.
        const bool wasPlaying = a.player.IsPlaying();
        a.HandleOverlayMessage(a.hOverlayWnd, WM_LBUTTONDOWN, MK_LBUTTON, play);
        Require(a.player.IsPlaying() != wasPlaying, "Pacing deferred a playback command");
        a.HandleOverlayMessage(a.hOverlayWnd, WM_LBUTTONUP, 0, play);
        a.HandleOverlayMessage(a.hOverlayWnd, WM_LBUTTONDOWN, MK_LBUTTON, play);
        Require(a.player.IsPlaying() == wasPlaying, "Second playback command was lost");
        a.HandleOverlayMessage(a.hOverlayWnd, WM_LBUTTONUP, 0, play);
        a.HandleOverlayMessage(a.hOverlayWnd, WM_MOUSEMOVE, 0, settings);
        a.playerKeyboardFocusId = 9006; a.MarkAccessibilityDirty();
        a.HandleMessage(a.hWnd, WM_GETOBJECT, 0, static_cast<LPARAM>(UiaRootObjectId));
        AccessibilityNode node;
        Require(!a.accessibilityDirty && a.accessibilityLastRefreshMs != 0 && a.accessibilityTree.GetNodeCopy(9006, node) && node.focused,
            "WM_GETOBJECT did not force current accessibility during a pending frame");
        Require(r.GetFrameStatistics().framesDrawn == frames && r.HasPendingFrame(), "Accessibility query consumed the pending visual frame");
        a.settings.shortcuts["togglePlay"].vkCode = VK_F11;
        a.accessibilityLastRefreshMs = 0; a.MarkAccessibilityDirty();
        // Make the retained request due and use its timer wake, with no more input.
        r.framePacer.FrameStarted(FrameSeconds() - 1); r.framePacer.Request(); r.ArmFrameTimer(FrameSeconds());
        Require(WaitForSingleObject(r.frameTimer, 1000) == WAIT_OBJECT_0 && r.IsFrameDue(), "Pending overlay timer did not wake");
        a.Render();
        Require(r.GetFrameStatistics().framesDrawn == frames + 1 && !r.HasPendingFrame(), "Deadline did not consume exactly one pending frame");
        Require(a.BuildPlayerRenderViewModel().bottomBar.hoveredButtonId == 5 &&
            static_cast<const uint32_t*>(r.pBitmapBits)[hoverPixel] != initialPixel, "Deadline frame did not draw the newest hover");
        Require(!a.accessibilityDirty && a.accessibilityLastRefreshMs != 0 && a.accessibilityTree.GetNodeCopy(9002, node) &&
            node.keyboardShortcut.find(L"F11") != std::wstring::npos, "Deadline frame missed the newest accessibility shortcut");
        Require(a.playerTooltipSignature != signature && PlayTooltip(a).find(L"F11") != std::wstring::npos, "Deadline frame missed the newest tooltip shortcut");
        if (hadShortcut) a.settings.shortcuts["togglePlay"] = original;
        else a.settings.shortcuts.erase("togglePlay");
        r.nextCadenceCheckSeconds = cadenceCheck;
        a.MarkAccessibilityDirty();
        Pass("input flood keeps hit testing, tooltip relay and commands immediate; deferred bookkeeping draws latest state on timer wake");
    }
    static void PreviewIdleTeardown(App& a, const std::wstring& media) {
        const bool hoverEnabled = a.settings.hoverPreviewEnabled;
        a.settings.hoverPreviewEnabled = true;
        a.HideTooltipKeyboardFocus();
        for (HWND window : {a.hWnd, a.hOverlayWnd, a.hVideoWnd}) {
            TRACKMOUSEEVENT cancel{ sizeof(TRACKMOUSEEVENT), TME_CANCEL | TME_LEAVE, window, 0 }; TrackMouseEvent(&cancel);
        }
        Pump(25);
        a.player.Pause(); a.playbackSession.Close();
        a.currentTab = TabView::Media; a.libraryGroupMode = LibraryGroupMode::Flat;
        a.isLastWatchedCleared = true; a.EnsureMediaLayoutCache();
        a.Update(0);
        Require(!a.previewInitialized && !a.previewInitAttempted && !a.previewPlayer.GetPreviewBitmap(), "Idle hub eagerly initialized previews");
        const float dp = a.renderer.GetDpiScale();
        const auto view = Presentation::HubView(1024, 640, dp);
        const LPARAM hover = Point(static_cast<int>((view.left + view.right) * .5f),
            static_cast<int>(view.top + 44 * dp + (a.GetMediaRowTopDIP(0) + a.GetMediaRowHeightDIP(0) * .5f) * dp));
        const auto warmHover = [&]() {
            // Hidden synthetic input must not ask Windows to compare the real
            // desktop cursor with this invisible owner and synthesize a leave.
            a.isMouseTracking = true;
            a.HandleMessage(a.hWnd, WM_MOUSEMOVE, 0, hover);
            a.hubTooltips.SetActive(false); a.HideTooltipKeyboardFocus();
            Require(a.hoveredMediaIndex == 0 && a.rowAnimStates[0].isHovering, "Hidden media hover did not reach the row");
            Until([&]() {
                a.Update(0);
                return a.previewInitialized && a.previewPlayer.IsActive() && a.previewPlayer.GetPreviewBitmap();
            }, "Hub hover did not initialize and decode a real preview");
            NoVisibleWindows();
        };
        warmHover();
        const auto dimensions = a.previewPlayer.GetPreviewBitmap()->GetPixelSize();
        Require(dimensions.width >= 640 && dimensions.height >= 180, "Hub hover produced no preview-sized bitmap");
        a.previewLastActivity = NowSeconds() - 13;
        a.Update(0);
        Require(a.previewInitialized && a.previewPlayer.IsActive() && a.previewPlayer.GetPreviewBitmap(), "Idle cleanup interrupted an active hub preview");
        // Keep the native player/window hidden and exercise the same stop/session
        // states used when opening main playback, without a visible transition.
        Require(a.playbackSession.OpenDirect(media) && a.playbackSession.OnPlayerOpened(true), "Hidden playback transition failed");
        a.previewPlayer.StopPreview(); a.player.Play();
        Require(a.player.IsPlaying() && !a.previewPlayer.IsActive() && a.previewPlayer.GetPreviewBitmap(), "Stopped preview was not warm during native playback");
        a.previewLastActivity = NowSeconds() - 11;
        a.Update(0);
        Require(a.previewInitialized && a.previewInitAttempted && a.previewPlayer.GetPreviewBitmap(), "Idle cleanup discarded a recent stopped preview");
        a.previewLastActivity = NowSeconds() - 13;
        a.Update(0);
        Require(!a.previewInitialized && !a.previewInitAttempted && !a.previewPlayer.IsActive() && !a.previewPlayer.GetPreviewBitmap(),
            "Main playback retained the expired preview or prevented lazy retry");
        const double position = a.player.GetCurrentTime();
        Until([&]() { return a.player.IsPlaying() && a.player.GetCurrentTime() > position + .1; }, "Preview teardown interrupted native main playback");
        a.player.Pause(); a.playbackSession.Close();
        a.HandleMessage(a.hWnd, WM_MOUSEMOVE, 0, Point(-100, -100));
        warmHover();
        Require(a.previewInitAttempted && a.previewPlayer.GetCurrentPreviewPath() == media, "Fresh hub hover did not lazily restart after cleanup");
        a.HandleMessage(a.hWnd, WM_MOUSEMOVE, 0, Point(-100, -100));
        a.Update(0);
        Require(!a.previewPlayer.IsActive(), "Leaving the hub row did not stop the restarted preview");
        Require(a.playbackSession.OpenDirect(media) && a.playbackSession.OnPlayerOpened(true), "Restoring hidden playback failed");
        a.player.Play(); a.previewLastActivity = NowSeconds() - 13; a.Update(0);
        a.settings.hoverPreviewEnabled = hoverEnabled;
        a.controlsOpacity = 1.0f; a.controlsIdleTimer = 0.0f;
        Pass("expired hub preview resources release during main playback; active/recent previews survive and fresh hover restarts lazily");
    }
    static void HiddenLibraryRetirement(App& a, const std::wstring& media) {
        Require(a.playbackSession.OpenDirect(media) && a.playbackSession.OnPlayerOpened(true), "Hidden playback setup for retirement");
        a.player.Play();
        Require(a.renderer.EnsureGraphicsContext(), "Hub graphics setup for retirement");
        a.previewPlayer.StopPreview();
        a.previewPlayer.BindDeviceContext(a.renderer.GetD2DContext());
        a.hiddenLibrarySinceMs = GetTickCount64() - 16000;
        a.hiddenLibraryCachesTrimmed = false;
        a.Update(0);
        Require(a.hiddenLibraryCachesTrimmed && !a.renderer.GetD2DContext(), "Hidden playback retained hub graphics");
        for (int i = 0; i < 3; ++i) a.Update(0);
        Require(!a.renderer.GetD2DContext(), "Hidden playback reacquired hub graphics");
        Pass("playback retires hub graphics once and keeps them retired");
        a.ClosePlayer();
        Require(!a.playbackSession.IsOpen() && !a.playerInitialized, "Hidden player did not close");
        Require(!a.renderer.GetD2DContext(), "Hidden close eagerly recreated hub graphics");
        a.player.SetVideoBounds(0, 0, 1024, 640);
        Require(!(GetWindowLongPtrW(a.hVideoWnd, GWL_STYLE) & WS_VISIBLE), "Resizing revealed the stopped video host");
        Require(!a.hiddenLibrarySinceMs && !a.hiddenLibraryCachesTrimmed, "Close retained a completed trim state");
        NoVisibleWindows();
        a.Render();
        Require(a.renderer.GetD2DContext(), "Hub draw did not lazily recreate graphics");
        const auto size = a.renderer.GetD2DContext()->GetSize();
        Require(size.width == 1024 && size.height == 640, "Restored hub target dimensions changed");
        Pass("hidden close stays lazy; later hub drawing restores the target");
    }
    static void DiagnosticsClose(App& a) {
        Key(a, 'I'); Require(a.showPlaybackDiagnostics, "Diagnostics keyboard toggle did not open panel");
        const float dp = a.overlayRenderer.GetDpiScale();
        const auto layout = Presentation::PlaybackDiagnostics(D2D1::RectF(0, 0, 1024, 640), dp);
        Require(layout.visible && std::abs(RectWidth(layout.close) - 36 * dp) < 0.01f &&
            std::abs(RectHeight(layout.close) - 36 * dp) < 0.01f, "Diagnostics close target is not touch sized");
        const int x = static_cast<int>(layout.close.left + 2 * dp), y = static_cast<int>(layout.close.top + 2 * dp);
        Require(a.IsPlaybackDiagnosticsCloseHit(static_cast<float>(x), static_cast<float>(y)), "Close padding is not tappable");
        a.activeModal = ModalType::Confirmation;
        Require(!a.IsPlaybackDiagnosticsCloseHit(static_cast<float>(x), static_cast<float>(y)), "Diagnostics close stole modal input");
        a.activeModal = ModalType::None;
        a.isContextMenuOpen = true;
        Require(!a.IsPlaybackDiagnosticsCloseHit(static_cast<float>(x), static_cast<float>(y)), "Diagnostics close stole context menu input");
        a.isContextMenuOpen = false;
        Capture(a, L"diagnostics-close");
        const bool playing = a.player.IsPlaying();
        a.HandleOverlayMessage(a.hOverlayWnd, WM_LBUTTONDOWN, MK_LBUTTON, Point(x, y));
        a.HandleOverlayMessage(a.hOverlayWnd, WM_LBUTTONUP, 0, Point(x, y));
        Pump(60);
        Require(!a.showPlaybackDiagnostics && a.player.IsPlaying() == playing, "Diagnostics close fell through to playback");
        Key(a, 'I'); a.HandleMessage(a.hWnd, WM_LBUTTONDOWN, MK_LBUTTON, Point(x, y));
        a.HandleMessage(a.hWnd, WM_LBUTTONUP, 0, Point(x, y));
        Pump(60);
        Require(!a.showPlaybackDiagnostics && a.player.IsPlaying() == playing, "Host close fell through to playback");
        for (float scale : {0.75f, 1.0f, 1.5f, 2.0f}) {
            const auto scaled = Presentation::PlaybackDiagnostics(D2D1::RectF(0, 0, 1024 * scale, 640 * scale), scale);
            Require(scaled.visible && std::abs(RectWidth(scaled.close) - 36 * scale) < 0.01f, "Diagnostics close DPI layout mismatch");
        }
        Require(!Presentation::PlaybackDiagnostics(D2D1::RectF(0, 0, 180, 120), 1).visible, "Invisible panel retained a close target");
        Pass("diagnostics close accepts padded taps without changing playback and respects higher surfaces/DPI");
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
    struct ReturnObservation {
        App* app;
        bool watching = false, prepared = true;
        bool hasContext = false, hasFrame = false, occluded = false, menusClosed = false;
        unsigned presentation = 0;
        int hideEvents = 0;
    };
    static LRESULT CALLBACK ObserveVideoHide(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        auto& observation = *reinterpret_cast<ReturnObservation*>(data);
        if (observation.watching && message == WM_SHOWWINDOW && !w) {
            ++observation.hideEvents;
            const auto& a = *observation.app;
            observation.hasContext = a.renderer.GetD2DContext() != nullptr;
            observation.hasFrame = a.renderer.HasPresentedFrame();
            observation.occluded = a.renderer.IsOccluded();
            observation.menusClosed = !a.isSettingsFlyoutOpen && !a.isContextMenuOpen;
            observation.presentation = static_cast<unsigned>(a.playbackSession.GetPresentationMode());
            observation.prepared &= observation.hasContext && observation.hasFrame && !observation.occluded &&
                observation.menusClosed && a.playbackSession.GetPresentationMode() == PresentationMode::Windowed;
        }
        return DefSubclassProc(window, message, w, l);
    }
    static void VisibleReturns(const std::wstring& media, const std::string& mode) {
            auto a = std::make_unique<App>();
            ReturnObservation observation{a.get()};
            try {
                Setup(*a, media);
                a->player.Pause(); a->playbackSession.Close();
                SetWindowLongPtrW(a->hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
                SetWindowPos(a->hWnd, nullptr, 80, 80, 1040, 680, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                ShowWindow(a->hWnd, mode == "maximized" ? SW_MAXIMIZE : SW_SHOW);
                a->Render(); Pump(50);
                // A completed minimized-hub trim must not suppress playback retirement.
                a->hiddenLibrarySinceMs = GetTickCount64() - 16000; a->hiddenLibraryCachesTrimmed = true;
                a->OpenAndPlayMedia(media);
                Require(a->playbackSession.IsOpen() && !a->hiddenLibraryCachesTrimmed && !a->hiddenLibrarySinceMs, "Playback inherited an old trim state");
                Until([&]() { return a->player.IsPlaying() && a->player.GetCurrentTime() > .1; }, "Visible playback did not start");
                if (mode == "fullscreen") a->ToggleFullscreen();
                if (mode == "pip") a->TogglePiP();
                Pump(100);
                a->hiddenLibrarySinceMs = GetTickCount64() - 16000; a->hiddenLibraryCachesTrimmed = false;
                a->Update(0);
                Require(!a->renderer.GetD2DContext() && !a->renderer.HasPresentedFrame(), "Visible playback did not retire graphics");
                if (mode == "minimized") ShowWindow(a->hWnd, SW_MINIMIZE);
                Require(SetWindowSubclass(a->hVideoWnd, ObserveVideoHide, 1, reinterpret_cast<DWORD_PTR>(&observation)), "Video hide observer installation");
                a->isSettingsFlyoutOpen = a->isContextMenuOpen = true;
                observation.watching = mode != "minimized";
                const bool visibleBeforeClose = IsWindowVisible(a->hWnd) != FALSE;
                const bool minimizedBeforeClose = IsIconic(a->hWnd) != FALSE;
                a->ClosePlayer(); observation.watching = false;
                std::ofstream diagnostic(output + L"/return-state-" + Utf8ToUtf16(mode) + L".json");
                diagnostic << "{\"hideEvents\":" << observation.hideEvents << ",\"prepared\":" << observation.prepared
                    << ",\"hasContextAtHide\":" << observation.hasContext << ",\"hasFrameAtHide\":" << observation.hasFrame
                    << ",\"occludedAtHide\":" << observation.occluded << ",\"menusClosedAtHide\":" << observation.menusClosed
                    << ",\"presentationAtHide\":" << observation.presentation << ",\"visibleBeforeClose\":" << visibleBeforeClose
                    << ",\"minimizedBeforeClose\":" << minimizedBeforeClose << "}\n";
                diagnostic.close();
                Require(!a->playbackSession.IsOpen() && !a->hiddenLibrarySinceMs && !a->hiddenLibraryCachesTrimmed, "Close retained playback/trim state");
                if (mode == "minimized") {
                    Require(IsIconic(a->hWnd) && !a->renderer.GetD2DContext(), "Minimized stop allocated graphics or restored the window");
                    ShowWindow(a->hWnd, SW_RESTORE); a->Render();
                } else {
                    Require(observation.hideEvents > 0 && observation.prepared, "Video hidden before a restored hub frame was prepared");
                    Require(a->renderer.HasPresentedFrame() && !a->renderer.IsOccluded(), "Restored hub frame unavailable");
                }
                Require(a->renderer.GetD2DContext(), "Restored hub has no graphics context");
                RECT client{}; GetClientRect(a->hWnd, &client);
                const auto size = a->renderer.GetD2DContext()->GetSize();
                Require(size.width == client.right && size.height == client.bottom, "Hub target differs from restored client dimensions");
                Require((mode == "maximized") == (IsZoomed(a->hWnd) != FALSE), "Restored maximized state changed");
                SetWindowPos(a->hWnd, nullptr, 80, 80, 900, 600, SWP_NOZORDER | SWP_NOACTIVATE);
                a->Render();
                Require(!(GetWindowLongPtrW(a->hVideoWnd, GWL_STYLE) & WS_VISIBLE), "Hub resize exposed a stopped video host");
                if (mode == "windowed") {
                    // No intervening hub Update: a new session still needs a new trim timer.
                    a->OpenAndPlayMedia(media);
                    Require(a->playbackSession.IsOpen() && !a->hiddenLibraryCachesTrimmed && !a->hiddenLibrarySinceMs, "Rapid reopen inherited trim state");
                    a->hiddenLibrarySinceMs = GetTickCount64() - 16000; a->Update(0);
                    Require(!a->renderer.GetD2DContext(), "Rapid reopen retained recreated hub graphics");
                    observation.watching = true; a->ClosePlayer(); observation.watching = false;
                    Require(observation.hideEvents >= 2 && observation.prepared, "Rapid reopen lost covered frame preparation");
                }
                RemoveWindowSubclass(a->hVideoWnd, ObserveVideoHide, 1);
                std::ofstream record(output + L"/return-" + Utf8ToUtf16(mode) + L".json");
                record << "{\"mode\":\"" << mode << "\",\"hideEvents\":" << observation.hideEvents
                    << ",\"preparedBeforeVideoHide\":" << (observation.prepared ? "true" : "false")
                    << ",\"restoredClientWidth\":" << client.right << ",\"restoredClientHeight\":" << client.bottom
                    << ",\"scope\":\"Opt-in visible functional state/order checks; forced15s trim clock, no cadence or physical-panel/latency measurement\"}\n";
            } catch (...) { Release(*a); throw; }
            Release(*a); Pass(("visible library return: " + mode).c_str());
    }
    static void Run(App& a, const std::wstring& media) {
        Require(!a.RenderHubFrame(), "Uninitialized hub renderer accepted a frame");
        Setup(a, media); PacedInputBookkeeping(a); PreviewIdleTeardown(a, media); DiagnosticsClose(a); Controls(a); Subtitles(a); Bookmark(a); RepeatAndMenu(a); RenameUndo(a); MoveUndo(a); MediaAndFolderUndo(a); EditHistory(a); HiddenLibraryRetirement(a, media); NoVisibleWindows();
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
    const bool visibleReturns = argc == 5 && std::wstring(argv[3]) == L"--visible-return";
    const std::string mode = visibleReturns ? Utf16ToUtf8(argv[4]) : "";
    if ((argc != 3 && !visibleReturns) || (visibleReturns && mode != "windowed" && mode != "maximized" && mode != "fullscreen" && mode != "pip" && mode != "minimized")) {
        std::cerr << "Usage: playback-ui-tests.exe <media> <new-output-dir> [--visible-return <windowed|maximized|fullscreen|pip|minimized>]\n"; return 2;
    }
    OleInitialize(nullptr);
    PlaybackUiTestAccess::output = argv[2];
    int result = 0;
    if (visibleReturns) {
        try { PlaybackUiTestAccess::VisibleReturns(argv[1], mode); }
        catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << std::endl; result = 1; }
    } else {
        auto app = std::make_unique<App>();
        try { PlaybackUiTestAccess::Run(*app, argv[1]); }
        catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << std::endl; result = 1; }
        PlaybackUiTestAccess::Release(*app);
    }
    OleUninitialize();
    std::cout << PlaybackUiTestAccess::passed << (visibleReturns ? " native interaction checks passed\n" : " hidden interaction checks passed\n"); return result;
}
