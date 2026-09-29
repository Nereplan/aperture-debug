#include "App.h"
#include "AppInternals.h"
#include "Presentation.h"
#include "AnimationCurves.h"

namespace Aperture {

namespace {

short ClampShort(float value) {
    return static_cast<short>(std::clamp(std::lroundf(value), -32768L, 32767L));
}

} // namespace

// ============================================================================
// Hub (main window) touch gestures
//
// A touch press is delivered to the existing mouse path immediately so all
// press feedback and dispatch logic stays authoritative (zero added latency).
// The gesture layer watches the pointer in parallel and, when a pan is
// recognized before the tap settles, it cancels the armed press (card drag,
// press visuals) and drives the scroll offset directly, with fling inertia
// and rubber-band overscroll. Double-tap synthesizes WM_LBUTTONDBLCLK so the
// double-click affordances (open media / playlist / history row) work for
// touch exactly like a mouse. Long-press synthesizes WM_RBUTTONDOWN so the
// hover-gated context menus (card actions, row delete, ...) are reachable.
// ============================================================================

// True when the touch press landed on a control that owns its own drag or
// executes an action that a pan must not cancel. These keep the press flowing
// through the synthetic-mouse path with no pan takeover.
bool App::TouchPressArmedHubControl() const {
    return textInputDragging ||
        settingsPressState.target != SettingsPressState::Target::None ||
        pressedNavIndex >= 0 ||
        pressedMediaEmptyBtn >= 0 || pressedHistoryEmptyBtn >= 0 ||
        isClearHistoryPressed || isModalOkPressed || isModalCancelPressed ||
        isJumpAheadPressed || isInlineRenaming || subtitleSizeDragging ||
        hoveredMediaGroupHeader >= 0 || hoveredFolderDeleteIndex >= 0 ||
        hoveredPlaylistDetailDeleteIndex >= 0 || hoveredHistoryDeleteIndex >= 0;
}

bool App::HandleHubTouchPointer(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    TouchPointerEvent ev;
    if (!GetTouchPointerEvent(hwnd, uMsg, wParam, ev)) return false;
    const float dp = std::max(0.5f, renderer.GetDpiScale());
    const double now = NowSeconds();
    const float xDip = ev.xPx / dp, yDip = ev.yPx / dp;

    switch (uMsg) {
        case WM_POINTERDOWN: {
            StopAutoscroll(true);
            touchHubTapSettleAt = -1.0;
            if (touchHub.active()) return true; // second finger: ignored
            // Pen barrel: the legacy translation already turns the whole
            // gesture into a right-click, so hand it over untouched.
            if (ev.barrelButton) return false;
            // Pen hover never arrives as DOWN; a contact begins a gesture.
            hubTouchBlocked = activeModal != ModalType::None || globalSearchModel.IsOpen() ||
                isContextMenuOpen || importJobActive || activeDragType != DragType::None;
            hubTouchMode = HubTouchMode::Passthrough;
            touchPanPane = AutoscrollPane::None;
            touchHub.Begin(ev.pointerId, xDip, yDip, now);
            touchPressSuppressesHoverExpand = true;
            if (!hubTouchBlocked) {
                float maxScroll = 0.0f;
                const AutoscrollPane pane = HitTestAutoscrollPane(ev.xPx, ev.yPx, maxScroll);
                if (pane != AutoscrollPane::None) {
                    touchPanPane = pane;
                    hubTouchPanMaxScroll = maxScroll;
                    // Resolve what the press landed on before delivering it, so a
                    // rail item (which opens on press) can be deferred instead.
                    RefreshHubHitTargets(ev.xPx, ev.yPx);
                    hubTouchMode = hoveredPlaylistRailIndex >= 0 ? HubTouchMode::Deferred
                                                                 : HubTouchMode::PanCandidate;
                }
            }
            if (hubTouchMode == HubTouchMode::Deferred) return true;
            DeliverTouchMouse(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, ev.xPx, ev.yPx);
            return true;
        }
        case WM_POINTERUPDATE: {
            if (!touchHub.active()) return false; // pen hover: legacy mouse translation
            if (touchHub.pointerId() != ev.pointerId) return true;
            if (touchHub.phase() == Touch::Phase::LongPress || touchHub.phase() == Touch::Phase::Consumed) return true;
            const bool panBegan = touchHub.Move(xDip, yDip, now);
            if (hubTouchMode == HubTouchMode::PanCandidate && panBegan && TouchPressArmedHubControl()) {
                hubTouchMode = HubTouchMode::Passthrough; // slider/text/nav owns the drag
            }
            if (hubTouchMode == HubTouchMode::PanCandidate && touchHub.phase() == Touch::Phase::Panning) {
                if (panBegan) {
                    touchPanLastY = yDip;
                    CancelCardDrag();          // drag was armed at the press; pan wins
                    ClearHubPressVisualState();
                }
                if (touchPanPane != AutoscrollPane::None) {
                    float* offset = GetScrollOffsetPtr(touchPanPane);
                    if (offset) {
                        const float step = Touch::ResistedScrollStep(*offset, 0.0f, hubTouchPanMaxScroll, -(yDip - touchPanLastY));
                        *offset += step;
                        InvalidateRect(hWnd, nullptr, FALSE);
                    }
                }
                touchPanLastY = yDip;
                return true;
            }
            if (hubTouchMode == HubTouchMode::Deferred) {
                if (panBegan) { hubTouchMode = HubTouchMode::PanCandidate; touchPanLastY = yDip; }
                return true; // deciding or panning; nothing was armed for deferred presses
            }
            DeliverTouchMouse(hwnd, WM_MOUSEMOVE, MK_LBUTTON, ev.xPx, ev.yPx);
            return true;
        }
        case WM_POINTERUP: {
            if (!touchHub.active()) return false;
            if (touchHub.pointerId() != ev.pointerId) return true;
            const bool longPress = touchHub.phase() == Touch::Phase::LongPress;
            const bool consumed = touchHub.phase() == Touch::Phase::Consumed;
            const auto outcome = touchHub.Finish(now);
            touchPressSuppressesHoverExpand = false;
            const float xPx = ev.xPx, yPx = ev.yPx;
            touchHub.Reset();
            if (longPress || consumed) { hubTouchMode = HubTouchMode::Passthrough; touchPanPane = AutoscrollPane::None; return true; }
            if (outcome == Touch::Engine::Outcome::Pan && hubTouchMode == HubTouchMode::PanCandidate) {
                BeginTouchInertia();
                hubTouchMode = HubTouchMode::Passthrough;
                return true;
            }
            touchPanPane = AutoscrollPane::None;
            // A tap. Deferred rail items were never delivered, so send the full
            // press+release now; everything else only needs the release.
            if (hubTouchMode == HubTouchMode::Deferred) {
                DeliverSyntheticTap(hwnd, xPx, yPx);
            } else {
                DeliverTouchMouse(hwnd, WM_LBUTTONUP, 0, xPx, yPx);
            }
            if (outcome == Touch::Engine::Outcome::Tap && !hubTouchBlocked &&
                touchHub.IsDoubleTapCandidate(xPx / dp, yPx / dp, now)) {
                // Mirror the full mouse double-click message sequence (DOWN UP
                // DBLCLK UP): opens media, playlist rows/episodes, and history
                // rows for touch, and the trailing UP releases the armed drag.
                touchHub.ClearTapMemory();
                RefreshHubHitTargets(xPx, yPx);
                DeliverTouchMouse(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, xPx, yPx);
                DeliverTouchMouse(hwnd, WM_LBUTTONUP, 0, xPx, yPx);
            } else {
                if (outcome == Touch::Engine::Outcome::Tap) touchHub.RegisterTap(xPx / dp, yPx / dp, now);
                // Clear the hover the press left behind before the 350 ms hover
                // expansion/preview would start: touch has no persistent hover.
                touchHubTapSettleAt = now + Touch::kDoubleTapWindowSec;
            }
            hubTouchMode = HubTouchMode::Passthrough;
            return true;
        }
    }
    return false;
}

// ============================================================================
// Player (overlay + video child windows) touch gestures
//
// Center video taps pass straight through (press toggles play, exactly like a
// mouse click). Edge-zone taps wait out the double-tap window: a second tap
// seeks ±10 s with the YouTube-style ripple OSD; a lone tap is delivered late
// as a normal press. Horizontal pan in the video area scrubs the timeline;
// a vertical pan on the right side drags volume. The queue flyout pans its
// rows. Long-press in the video area opens the player context menu.
// ============================================================================

bool App::HandlePlayerTouchPointer(HWND hTarget, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    TouchPointerEvent ev;
    if (!GetTouchPointerEvent(hTarget, uMsg, wParam, ev)) return false;
    const float dp = std::max(0.5f, overlayRenderer.GetDpiScale());
    RECT rc{};
    GetClientRect(hWnd, &rc);
    const float winW = static_cast<float>(std::max<LONG>(1, rc.right - rc.left));
    const float winH = static_cast<float>(std::max<LONG>(1, rc.bottom - rc.top));
    const float topBarH = 64.0f * dp, bottomBarH = 76.0f * dp;
    const double now = NowSeconds();
    const float xDip = ev.xPx / dp, yDip = ev.yPx / dp;

    switch (uMsg) {
        case WM_POINTERDOWN: {
            if (touchPlayer.active()) return true; // second finger: ignored
            // Pen barrel: the legacy translation already turns the whole
            // gesture into a right-click, so hand it over untouched.
            if (ev.barrelButton) return false;
            playerHoverClearAt = -1.0;
            playerTouch.mode = PlayerTouchMode::Passthrough;
            playerTouch.seekScrub = playerTouch.volumeDrag = false;
            playerTouch.lastYPx = playerTouch.flyoutAccumPx = 0.0f;
            touchPlayer.Begin(ev.pointerId, xDip, yDip, now);
            playerTouch.downXPx = ev.xPx;
            playerTouch.downYPx = ev.yPx;

            if (isContextMenuOpen) {
                DeliverTouchMouse(hTarget, WM_LBUTTONDOWN, MK_LBUTTON, ev.xPx, ev.yPx);
                return true;
            }
            if (isSettingsFlyoutOpen) {
                const float flyoutW = 280.0f * dp;
                const float flyoutH = GetFlyoutHeight();
                const D2D1_RECT_F flyoutBounds = D2D1::RectF(winW - flyoutW - 20.0f * dp, winH - bottomBarH - flyoutH - 10.0f * dp,
                    winW - 20.0f * dp, winH - bottomBarH - 10.0f * dp);
                if (currentFlyoutMenu == "queue" && PtInRectF(flyoutBounds, ev.xPx, ev.yPx)) {
                    playerTouch.mode = PlayerTouchMode::QueueFlyout; // rows execute on press: defer
                    return true;
                }
                DeliverTouchMouse(hTarget, WM_LBUTTONDOWN, MK_LBUTTON, ev.xPx, ev.yPx);
                return true;
            }
            if (ev.yPx >= winH - bottomBarH || ev.yPx <= topBarH) {
                // Controls region: inflate the timeline/volume hit bands for fingers.
                float nudgedX = ev.xPx, nudgedY = ev.yPx;
                NudgeTouchControlTarget(ev.xPx, ev.yPx, winW, winH, dp, nudgedX, nudgedY);
                DeliverTouchMouse(hTarget, WM_LBUTTONDOWN, MK_LBUTTON, nudgedX, nudgedY);
                return true;
            }
            if (playbackSession.IsPiP()) {
                // PiP keeps plain mouse semantics (tap toggles play on release,
                // drag moves the window); marking onVideo lets a second quick
                // tap synthesize the DBLCLK that leaves PiP.
                playerTouch.onVideo = true;
                DeliverTouchMouse(hTarget, WM_LBUTTONDOWN, MK_LBUTTON, ev.xPx, ev.yPx);
                return true;
            }
            if (isJumpAheadVisible && PtInRectF(jumpAheadBounds, ev.xPx, ev.yPx)) {
                DeliverTouchMouse(hTarget, WM_LBUTTONDOWN, MK_LBUTTON, ev.xPx, ev.yPx);
                return true;
            }
            // Video area. Edge zones defer the tap so a quick second tap can
            // become a ±10 s seek instead of a play/pause toggle.
            const float edgeW = winW * 0.30f;
            if (ev.xPx < edgeW || ev.xPx > winW - edgeW) {
                playerTouch.mode = PlayerTouchMode::VideoEdge;
                playerTouch.onVideo = true;
                return true;
            }
            DeliverTouchMouse(hTarget, WM_LBUTTONDOWN, MK_LBUTTON, ev.xPx, ev.yPx);
            playerTouch.onVideo = true;
            return true;
        }
        case WM_POINTERUPDATE: {
            if (!touchPlayer.active()) return false; // pen hover: legacy mouse translation
            if (touchPlayer.pointerId() != ev.pointerId) return true;
            if (touchPlayer.phase() == Touch::Phase::LongPress || touchPlayer.phase() == Touch::Phase::Consumed) return true;
            const bool panBegan = touchPlayer.Move(xDip, yDip, now);
            if (panBegan && playerTouch.mode == PlayerTouchMode::VideoEdge) {
                if (touchPlayer.axis() == Touch::Axis::Horizontal) {
                    StartTouchSeekScrub(ev.xPx, winW, dp);
                } else if (touchPlayer.axis() == Touch::Axis::Vertical && playerTouch.downXPx > winW * 0.6f) {
                    playerTouch.volumeDrag = true;
                    playerTouch.volumeStart = player.GetVolume();
                } else {
                    touchPlayer.Consume(); // vertical pan on the left half: reserved no-op
                    return true;
                }
            }
            if (touchPlayer.phase() == Touch::Phase::Panning) {
                if (playerTouch.seekScrub) {
                    ScrubTimelineToPx(ev.xPx, winW, dp);
                    controlsIdleTimer = 0.0f;
                    controlsOpacity = 1.0f;
                    InvalidateRect(hOverlayWnd ? hOverlayWnd : hWnd, nullptr, FALSE);
                } else if (playerTouch.volumeDrag) {
                    const float volume = std::clamp(playerTouch.volumeStart - touchPlayer.totalDy() / 220.0f, 0.0f, 1.0f);
                    ApplyTouchVolume(volume);
                } else if (playerTouch.mode == PlayerTouchMode::QueueFlyout) {
                    const float rowStep = 36.0f * dp;
                    playerTouch.flyoutAccumPx += playerTouch.lastYPx > 0.0f ? playerTouch.lastYPx - ev.yPx : 0.0f;
                    playerTouch.lastYPx = ev.yPx;
                    const size_t queueSize = playbackSession.GetQueueSize();
                    const size_t maxFirst = queueSize > 7 ? queueSize - 7 : 0;
                    while (playerTouch.flyoutAccumPx >= rowStep && queueFlyoutFirstVisible < maxFirst) {
                        ++queueFlyoutFirstVisible;
                        playerTouch.flyoutAccumPx -= rowStep;
                    }
                    while (playerTouch.flyoutAccumPx <= -rowStep && queueFlyoutFirstVisible > 0) {
                        --queueFlyoutFirstVisible;
                        playerTouch.flyoutAccumPx += rowStep;
                    }
                    if (playerTouch.flyoutAccumPx > rowStep) playerTouch.flyoutAccumPx = rowStep;
                    if (playerTouch.flyoutAccumPx < -rowStep) playerTouch.flyoutAccumPx = -rowStep;
                    InvalidateRect(hOverlayWnd ? hOverlayWnd : hWnd, nullptr, FALSE);
                }
                return true;
            }
            if (playerTouch.mode == PlayerTouchMode::Passthrough) {
                DeliverTouchMouse(hTarget, WM_MOUSEMOVE, MK_LBUTTON, ev.xPx, ev.yPx);
            }
            return true;
        }
        case WM_POINTERUP: {
            if (!touchPlayer.active()) return false;
            if (touchPlayer.pointerId() != ev.pointerId) return true;
            const bool longPress = touchPlayer.phase() == Touch::Phase::LongPress;
            const bool consumed = touchPlayer.phase() == Touch::Phase::Consumed;
            const auto outcome = touchPlayer.Finish(now);
            const float xPx = ev.xPx, yPx = ev.yPx;
            const bool wasVideo = playerTouch.onVideo;
            const bool wasEdge = playerTouch.mode == PlayerTouchMode::VideoEdge;
            const bool wasFlyout = playerTouch.mode == PlayerTouchMode::QueueFlyout;
            touchPlayer.Reset();

            if (longPress || consumed) { playerTouch = {}; return true; }
            if (outcome == Touch::Engine::Outcome::Pan) {
                if (playerTouch.seekScrub) FinishTouchSeekScrub(true);
                playerTouch.mode = PlayerTouchMode::Passthrough;
                playerTouch.seekScrub = playerTouch.volumeDrag = false;
                playerTouch.onVideo = false;
                return true;
            }
            if (wasFlyout) {
                DeliverSyntheticTap(hTarget, xPx, yPx);
                playerTouch.mode = PlayerTouchMode::Passthrough;
                return true;
            }
            if (wasEdge && wasVideo) {
                const bool forward = playerTouch.downXPx > winW * 0.5f;
                if (outcome == Touch::Engine::Outcome::Tap && touchPlayer.IsDoubleTapCandidate(xPx / dp, yPx / dp, now)) {
                    playerTouch.deliverAt = -1.0;
                    touchPlayer.ClearTapMemory();
                    commandDispatcher.Execute(forward ? AppCommand::SeekForwardLarge : AppCommand::SeekBackwardLarge);
                } else {
                    if (outcome == Touch::Engine::Outcome::Tap) touchPlayer.RegisterTap(xPx / dp, yPx / dp, now);
                    playerTouch.deliverAt = now + Touch::kDoubleTapWindowSec;
                    playerTouch.deliverXPx = playerTouch.downXPx;
                    playerTouch.deliverYPx = playerTouch.downYPx;
                }
                playerTouch.mode = PlayerTouchMode::Passthrough;
                playerTouch.onVideo = false;
                return true;
            }
            // Passthrough tap (center video, controls, PiP).
            DeliverTouchMouse(hTarget, WM_LBUTTONUP, 0, xPx, yPx);
            const bool centerVideo = wasVideo && !wasEdge;
            if (centerVideo && outcome == Touch::Engine::Outcome::Tap &&
                touchPlayer.IsDoubleTapCandidate(xPx / dp, yPx / dp, now)) {
                touchPlayer.ClearTapMemory();
                RefreshOverlayHitTargets(xPx, yPx);
                // Mouse parity: double-click on the video toggles fullscreen
                // (the paired play toggles cancel out across the two taps).
                // The trailing UP completes the mouse message sequence.
                DeliverTouchMouse(hTarget, WM_LBUTTONDBLCLK, MK_LBUTTON, xPx, yPx);
                DeliverTouchMouse(hTarget, WM_LBUTTONUP, 0, xPx, yPx);
                playerHoverClearAt = -1.0;
            } else {
                if (outcome == Touch::Engine::Outcome::Tap) touchPlayer.RegisterTap(xPx / dp, yPx / dp, now);
                playerHoverClearAt = now + Touch::kDoubleTapWindowSec;
            }
            playerTouch.mode = PlayerTouchMode::Passthrough;
            playerTouch.onVideo = false;
            return true;
        }
    }
    return false;
}

// ============================================================================
// Per-frame touch work: long-press timers, deferred taps, fling inertia,
// rubber-band settle, and post-tap hover clears.
// ============================================================================

bool App::UpdateTouchGestures(float dt, double nowSec) {
    bool needsRedraw = false;
    dt = std::clamp(dt, 0.0f, 0.05f);

    // Hub long-press: open the hover-derived context menu. Presses that armed
    // a control (slider, nav, rename, ...) keep their mouse semantics instead.
    if (touchHub.phase() == Touch::Phase::Pressing && !hubTouchBlocked && !TouchPressArmedHubControl() &&
        touchHub.LongPressDue(nowSec) && hubTouchMode != HubTouchMode::Passthrough) {
        const float dp = std::max(0.5f, renderer.GetDpiScale());
        const float xPx = touchHub.x() * dp, yPx = touchHub.y() * dp;
        CancelCardDrag();
        ClearHubPressVisualState();
        RefreshHubHitTargets(xPx, yPx);
        DeliverTouchMouse(hWnd, WM_RBUTTONDOWN, MK_RBUTTON, xPx, yPx);
        touchHub.MarkLongPress();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    // Player long-press (deferred edge zone): open the player context menu.
    if (touchPlayer.phase() == Touch::Phase::Pressing && playerTouch.mode == PlayerTouchMode::VideoEdge &&
        touchPlayer.LongPressDue(nowSec)) {
        RECT rc{};
        GetClientRect(hWnd, &rc);
        const float dp = std::max(0.5f, overlayRenderer.GetDpiScale());
        const float winW = static_cast<float>(std::max<LONG>(1, rc.right - rc.left));
        const float winH = static_cast<float>(std::max<LONG>(1, rc.bottom - rc.top));
        if (playerTouch.downYPx > 64.0f * dp && playerTouch.downYPx < winH - 76.0f * dp && !playbackSession.IsPiP()) {
            DeliverTouchMouse(hOverlayWnd ? hOverlayWnd : hWnd, WM_RBUTTONDOWN, MK_RBUTTON, playerTouch.downXPx, playerTouch.downYPx);
            touchPlayer.MarkLongPress();
            InvalidateRect(hOverlayWnd ? hOverlayWnd : hWnd, nullptr, FALSE);
        }
    }
    // Deferred edge-zone single tap: deliver late so double-tap seek can win.
    if (playerTouch.deliverAt > 0.0 && nowSec >= playerTouch.deliverAt) {
        playerTouch.deliverAt = -1.0;
        if (touchPlayer.phase() == Touch::Phase::Idle && playbackSession.IsOpen()) {
            DeliverSyntheticTap(hOverlayWnd ? hOverlayWnd : hWnd, playerTouch.deliverXPx, playerTouch.deliverYPx);
            playerHoverClearAt = nowSec + Touch::kDoubleTapWindowSec;
        }
    }
    // Post-tap hover clears.
    if (touchHubTapSettleAt > 0.0 && nowSec >= touchHubTapSettleAt) {
        touchHubTapSettleAt = -1.0;
        if (touchHub.phase() == Touch::Phase::Idle && !playbackSession.IsOpen() &&
            !IsAutoscrolling() && activeDragType == DragType::None) {
            OnMouseLeave();
        }
    }
    if (playerHoverClearAt > 0.0 && nowSec >= playerHoverClearAt) {
        playerHoverClearAt = -1.0;
        if (touchPlayer.phase() == Touch::Phase::Idle && playbackSession.IsOpen() &&
            !playerTouch.seekScrub && !playerTouch.volumeDrag) {
            ClearPlayerHoverState();
        }
    }

    // Fling inertia and rubber-band settle for the hub pane being panned.
    if (touchPanVelocity != 0.0f || touchSpringBack) {
        float* offset = GetScrollOffsetPtr(touchPanPane);
        if (!offset) {
            touchPanVelocity = 0.0f;
            touchSpringBack = false;
            touchPanPane = AutoscrollPane::None;
        } else {
            const float maxScroll = GetMaxScrollForPane(touchPanPane);
            if (touchPanVelocity != 0.0f) {
                *offset += touchPanVelocity * dt;
                touchPanVelocity *= std::exp(-dt / Touch::kInertiaTauSec);
                if (*offset <= 0.0f || *offset >= maxScroll) {
                    touchPanVelocity = 0.0f;
                    touchSpringBack = true;
                } else if (std::abs(touchPanVelocity) < Touch::kFlingStopDIPPerSec) {
                    touchPanVelocity = 0.0f;
                    if (*offset < -0.5f || *offset > maxScroll + 0.5f) touchSpringBack = true;
                    else touchPanPane = AutoscrollPane::None;
                }
            } else if (touchSpringBack) {
                const float target = std::clamp(*offset, 0.0f, std::max(0.0f, maxScroll));
                *offset = Touch::SpringToward(*offset, target, dt, Touch::kOverscrollSpringRate);
                if (std::abs(target - *offset) < 0.4f) {
                    *offset = target;
                    touchSpringBack = false;
                    touchPanPane = AutoscrollPane::None;
                }
            }
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        needsRedraw = true;
    }
    return needsRedraw;
}

void App::CancelTouchGestures() {
    touchHub.Cancel();
    touchPlayer.Cancel();
    hubTouchMode = HubTouchMode::Passthrough;
    hubTouchBlocked = false;
    touchPressSuppressesHoverExpand = false;
    touchHubTapSettleAt = -1.0;
    playerHoverClearAt = -1.0;
    playerTouch = {};
    touchPanVelocity = 0.0f;
    touchSpringBack = false;
    touchPanPane = AutoscrollPane::None;
}

void App::BeginTouchInertia() {
    float* offset = GetScrollOffsetPtr(touchPanPane);
    if (!offset) { touchPanPane = AutoscrollPane::None; return; }
    const float maxScroll = GetMaxScrollForPane(touchPanPane);
    if (IsReducedMotionEnabled()) {
        *offset = std::clamp(*offset, 0.0f, std::max(0.0f, maxScroll));
        touchPanPane = AutoscrollPane::None;
        return;
    }
    if (*offset < -0.5f || *offset > maxScroll + 0.5f) {
        touchPanVelocity = 0.0f;
        touchSpringBack = true;
        return;
    }
    const float velocity = std::clamp(-touchHub.velocityY(), -Touch::kMaxFlingDIPPerSec, Touch::kMaxFlingDIPPerSec);
    if (std::abs(velocity) < Touch::kFlingStopDIPPerSec) {
        touchPanPane = AutoscrollPane::None;
        return;
    }
    touchPanVelocity = velocity;
}

void App::ClearHubPressVisualState() {
    settingsPressState = {};
    pressedNavIndex = -1;
    pressedMediaEmptyBtn = -1;
    pressedHistoryEmptyBtn = -1;
    isClearHistoryPressed = false;
    isModalOkPressed = false;
    isModalCancelPressed = false;
    isJumpAheadPressed = false;
    textInputDragging = false;
    // A pan replaces hover: drop it so no row keeps a stuck highlight or a
    // hover preview behind the finger.
    OnMouseLeave();
}

void App::ClearPlayerHoverState() {
    isOverlayMouseTracking = false;
    isVideoMouseTracking = false;
    OnMouseLeave();
    InvalidateRect(hOverlayWnd ? hOverlayWnd : hWnd, nullptr, FALSE);
}

void App::DeliverTouchMouse(HWND hTarget, UINT mouseMessage, WPARAM wParam, float xPx, float yPx) {
    const LPARAM lp = MAKELPARAM(ClampShort(xPx), ClampShort(yPx));
    if (hTarget == hWnd) HandleMessage(hWnd, mouseMessage, wParam, lp);
    else HandleOverlayMessage(hTarget, mouseMessage, wParam, lp);
}

void App::DeliverSyntheticTap(HWND hTarget, float xPx, float yPx) {
    DeliverTouchMouse(hTarget, WM_LBUTTONDOWN, MK_LBUTTON, xPx, yPx);
    DeliverTouchMouse(hTarget, WM_LBUTTONUP, 0, xPx, yPx);
}

// Timeline/volume hit bands are sized for a mouse cursor; fingers need more
// room. When a touch press lands near either control (and not on top of one
// of the buttons), nudge the synthesized press into the band the existing
// mouse hit-testing already understands.
void App::NudgeTouchControlTarget(float xPx, float yPx, float winW, float winH, float dp, float& outX, float& outY) {
    outX = xPx;
    outY = yPx;
    const float barLeft = 20.0f * dp, barRight = winW - 20.0f * dp, barW = barRight - barLeft;
    const float trackCenterY = winH - 76.0f * dp + 16.0f * dp;
    const float controlsY = winH - 76.0f * dp + Presentation::PlayerControlsY * dp;
    const bool overBottomButton = yPx >= trackCenterY - 22.0f * dp && yPx <= controlsY + 28.0f * dp;
    const bool inTimelineBand = xPx >= barLeft && xPx <= barRight &&
        std::abs(yPx - trackCenterY) <= 20.0f * dp && std::abs(yPx - trackCenterY) > 12.0f * dp;
    const auto layout = Presentation::Player(winW, winH, dp, playbackSession.IsQueuePlayback());
    const bool overButton = overBottomButton && (
        PtInRectF(layout.play, xPx, yPx) || PtInRectF(layout.prev, xPx, yPx) || PtInRectF(layout.next, xPx, yPx) ||
        PtInRectF(layout.mute, xPx, yPx) || PtInRectF(layout.subtitles, xPx, yPx) || PtInRectF(layout.settings, xPx, yPx) ||
        PtInRectF(layout.pip, xPx, yPx) || PtInRectF(layout.fullscreen, xPx, yPx) ||
        PtInRectF(D2D1::RectF(24.0f * dp, 16.0f * dp, 60.0f * dp, 52.0f * dp), xPx, yPx));
    if (!overButton && inTimelineBand) { outY = trackCenterY; return; }
    const float volTrackX = layout.volume.left, volW = 60.0f * dp;
    const bool inVolumeBand = xPx >= volTrackX - 16.0f * dp && xPx <= volTrackX + volW + 16.0f * dp &&
        yPx >= controlsY - 18.0f * dp && yPx <= controlsY + 54.0f * dp &&
        !(yPx >= controlsY - 8.0f * dp && yPx <= controlsY + 44.0f * dp &&
          xPx >= volTrackX - 10.0f * dp && xPx <= volTrackX + volW + 10.0f * dp);
    if (!overButton && inVolumeBand) {
        // Pull the press into the volume band the mouse hit test understands.
        outY = controlsY + 14.0f * dp;
        outX = std::clamp(xPx, volTrackX + 1.0f, volTrackX + volW - 1.0f);
    }
}

void App::ScrubTimelineToPx(float xPx, float winW, float dp) {
    ScrubTimelineBetween(xPx, 20.0f * dp, winW - 40.0f * dp);
}

// Shared by the mouse scrub path and the touch drag-to-seek gesture: maps the
// pointer X onto the timeline and issues throttled imprecise seeks.
void App::ScrubTimelineBetween(float xPx, float barLeft, float barW) {
    if (player.GetDuration() <= 0.0) return;
    const double ratio = std::clamp((double)(xPx - barLeft) / (double)std::max(1.0f, barW), 0.0, 1.0);
    const double scrubTime = ratio * player.GetDuration();
    scrubTargetTime = scrubTime;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMpvSeekTimestamp).count();
    if (elapsedMs >= 30) { lastMpvSeekTimestamp = now; lastMpvSeekTime = scrubTime; player.Seek(scrubTime, false); }
}

void App::StartTouchSeekScrub(float xPx, float winW, float dp) {
    playerTouch.seekScrub = true;
    isScrubbing = true;
    scrubTargetTime = -1.0;
    controlsIdleTimer = 0.0f;
    controlsOpacity = 1.0f;
    if (hOverlayWnd) SetCapture(hOverlayWnd);
    ScrubTimelineToPx(xPx, winW, dp);
}

void App::FinishTouchSeekScrub(bool seekFinal) {
    if (seekFinal && isScrubbing && player.GetDuration() > 0.0) {
        if (scrubTargetTime >= 0.0) player.Seek(scrubTargetTime, true);
        AnnounceStatus(L"Seek to " + FormatDuration(player.GetCurrentTime()), true);
    }
    isScrubbing = false;
    scrubTargetTime = -1.0;
    isTimelineHovered = false;
    if (GetCapture() == hOverlayWnd) ReleaseCapture();
}

void App::ApplyTouchVolume(float volume) {
    player.SetVolume(volume);
    settings.volume = volume;
    settings.muted = false;
    if (volume > 0.01) playbackSession.SetPriorUnmutedVolume(volume);
    const int pct = static_cast<int>(std::round(volume * 100.0));
    ShowOSD(volume <= 0.01 ? IconType::VolumeMuted : (volume > 0.5 ? IconType::VolumeHigh : IconType::VolumeLow),
            std::to_wstring(pct) + L"%");
    InvalidateRect(hOverlayWnd ? hOverlayWnd : hWnd, nullptr, FALSE);
}

} // namespace Aperture
