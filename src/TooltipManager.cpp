#include "TooltipManager.h"

#include <algorithm>
#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <utility>
#include <vector>

#if defined(_WIN32)
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#endif

namespace Aperture {
namespace {

constexpr UINT kTooltipStyle = TTS_ALWAYSTIP | TTS_NOPREFIX;
constexpr int kDefaultReshowDelayMs = 100;
constexpr int kDefaultAutopopDelayMs = 10000;
constexpr TooltipManager::ToolId kTrackingToolId = static_cast<TooltipManager::ToolId>(~TooltipManager::ToolId(0));
} // namespace

TooltipManager::~TooltipManager() { Destroy(); }

bool TooltipManager::Create(HWND owner, int initialDelayMs, int maxWidthPx) {
    if (!owner || !IsWindow(owner)) return false;
    if (tooltip_ && owner_ == owner && IsWindow(tooltip_)) { ApplyTiming(initialDelayMs, maxWidthPx); return true; }
    if (tooltip_ || owner_) Destroy();
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_WIN95_CLASSES };
    if (!InitCommonControlsEx(&controls)) return false;
    owner_ = owner;
    tooltip_ = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | kTooltipStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, owner_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!tooltip_) { owner_ = nullptr; return false; }
    active_ = true;
    trackedId_ = 0;
    ApplyTiming(initialDelayMs, maxWidthPx);
    SendMessageW(tooltip_, TTM_ACTIVATE, TRUE, 0);
    return true;
}

void TooltipManager::Destroy() {
    CancelDeferredRefresh();
    if (tooltip_) { HideForKeyboard(); Clear(); DestroyWindow(tooltip_); }
    tooltip_ = nullptr;
    owner_ = nullptr;
    trackedId_ = 0;
    hoverId_ = 0;
    keyboardTracking_ = false;
    trackingToolAdded_ = false;
    trackingRecord_ = ToolRecord{};
    hoverDelayMs_ = 500;
    hoverTimerId_ = 0;
    pendingHoverId_ = 0;
    pendingHoverAnchor_ = POINT{};
    active_ = true;
    tools_.clear();
}

void TooltipManager::ApplyTiming(int initialDelayMs, int maxWidthPx) {
    if (!tooltip_) return;
    initialDelayMs = (initialDelayMs < 0) ? 0 : initialDelayMs;
    maxWidthPx = (maxWidthPx < 64) ? 64 : maxWidthPx;
    hoverDelayMs_ = static_cast<UINT>(initialDelayMs);
    SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, initialDelayMs);
    SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_RESHOW, kDefaultReshowDelayMs);
    SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, kDefaultAutopopDelayMs);
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, maxWidthPx);
}

bool TooltipManager::BuildToolInfo(ToolId id, TOOLINFOW& info) const {
    auto it = tools_.find(id);
    if (it == tools_.end() || !owner_ || !tooltip_) return false;
    return BuildToolInfoForRecord(id, it->second, 0, info);
}

bool TooltipManager::BuildToolInfoForRecord( ToolId id, const ToolRecord& record, UINT flags, TOOLINFOW& info) const {
    if (!owner_ || !tooltip_) return false;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.hwnd = owner_;
    info.uId = id;
    info.hinst = GetModuleHandleW(nullptr);
    info.rect = record.clientRect;
    info.lpszText = const_cast<LPWSTR>(record.text.c_str());
    // Public tools deliberately omit TTF_SUBCLASS and TTF_TRACK. The App can
    // relay messages from custom-drawn regions; private keyboard/hover
    // tracking uses a separate TOOLINFO with TTF_TRACK.
    info.uFlags = flags;
    return true;
}

bool TooltipManager::AddNativeTool(ToolId id, const ToolRecord& record) {
    TOOLINFOW info{};
    if (!BuildToolInfo(id, info)) return false;
    info.rect = record.clientRect;
    info.lpszText = const_cast<LPWSTR>(record.text.c_str());
    return SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info)) != FALSE;
}

bool TooltipManager::UpdateNativeTool(ToolId id, const ToolRecord& record, bool updateText) {
    TOOLINFOW info{};
    if (!BuildToolInfo(id, info)) return false;
    info.rect = record.clientRect;
    info.lpszText = const_cast<LPWSTR>(record.text.c_str());
    bool changed = SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&info)) != FALSE;
    if (updateText) { SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info)); changed = true; }
    return changed;
}

bool TooltipManager::EnsureTrackingTool(ToolId sourceId) {
    auto it = tools_.find(sourceId);
    if (it == tools_.end() || !tooltip_ || !owner_) return false;
    trackingRecord_ = it->second;
    TOOLINFOW info{};
    if (!BuildToolInfoForRecord(kTrackingToolId, trackingRecord_, TTF_TRACK, info)) return false;
    if (!trackingToolAdded_) {
        if (SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info)) == FALSE) {
            trackingRecord_ = ToolRecord{};
            return false;
        }
        trackingToolAdded_ = true;
    } else { SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info)); }
    return true;
}

void TooltipManager::RemoveTrackingTool() {
    if (tooltip_ && trackingToolAdded_) {
        TOOLINFOW info{};
        info.cbSize = sizeof(info);
        info.hwnd = owner_;
        info.uId = kTrackingToolId;
        SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
    trackingToolAdded_ = false;
    trackingRecord_ = ToolRecord{};
}

bool TooltipManager::ShowTracking(ToolId id, const POINT& screenAnchor, bool keyboard) {
    if (!tooltip_ || !active_ || tools_.find(id) == tools_.end()) return false;
    CancelHoverTimer();
    if (trackedId_ != 0 && trackedId_ != id) HideTracking();
    if (!EnsureTrackingTool(id)) return false;
    TOOLINFOW info{};
    if (!BuildToolInfoForRecord(kTrackingToolId, trackingRecord_, TTF_TRACK, info)) return false;
    POINT anchor = screenAnchor;
    if (!keyboard) { anchor.x += 10; anchor.y += 18; }
    SendMessageW(tooltip_, TTM_TRACKPOSITION, 0, MAKELPARAM(anchor.x, anchor.y));
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
    ShowWindow(tooltip_, SW_SHOWNOACTIVATE);
    trackedId_ = id;
    hoverId_ = keyboard ? 0 : id;
    keyboardTracking_ = keyboard;
    return true;
}

void TooltipManager::HideTracking() {
    CancelHoverTimer();
    if (tooltip_ && trackingToolAdded_) {
        TOOLINFOW info{};
        if (BuildToolInfoForRecord(kTrackingToolId, trackingRecord_, TTF_TRACK, info))
            SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&info));
        SendMessageW(tooltip_, TTM_POP, 0, 0);
        ShowWindow(tooltip_, SW_HIDE);
    }
    trackedId_ = 0;
    hoverId_ = 0;
    keyboardTracking_ = false;
    RemoveTrackingTool();
}

void TooltipManager::CancelHoverTimer() {
    if (tooltip_ && hoverTimerId_ != 0) KillTimer(tooltip_, hoverTimerId_);
    hoverTimerId_ = 0;
    pendingHoverId_ = 0;
    pendingHoverAnchor_ = POINT{};
}

void TooltipManager::ScheduleHover(ToolId id, const POINT& screenAnchor) {
    CancelHoverTimer();
    if (hoverDelayMs_ == 0) { ShowTracking(id, screenAnchor, false); return; }
    pendingHoverId_ = id;
    pendingHoverAnchor_ = screenAnchor;
    hoverTimerId_ = SetTimer( tooltip_, reinterpret_cast<UINT_PTR>(this), hoverDelayMs_, &TooltipManager::HoverTimerProc);
    if (hoverTimerId_ == 0) { pendingHoverId_ = 0; ShowTracking(id, screenAnchor, false); }
}

VOID CALLBACK TooltipManager::HoverTimerProc(HWND, UINT, UINT_PTR timerId, DWORD) {
    auto* self = reinterpret_cast<TooltipManager*>(timerId);
    if (!self || self->hoverTimerId_ != timerId) return;
    KillTimer(self->tooltip_, timerId);
    self->hoverTimerId_ = 0;
    const ToolId id = self->pendingHoverId_;
    const POINT anchor = self->pendingHoverAnchor_;
    self->pendingHoverId_ = 0;
    self->pendingHoverAnchor_ = POINT{};
    if (id != 0 && !self->keyboardTracking_) self->ShowTracking(id, anchor, false);
}

void TooltipManager::CancelDeferredRefresh() {
    if (tooltip_ && deferredRefreshTimerId_ != 0) KillTimer(tooltip_, deferredRefreshTimerId_);
    deferredRefreshTimerId_ = 0;
    deferredRefreshCallback_ = nullptr;
}

void TooltipManager::ScheduleDeferredRefresh(UINT delayMs, std::function<void()> callback) {
    if (!tooltip_ || !IsWindow(tooltip_)) return;
    CancelDeferredRefresh();
    deferredRefreshCallback_ = std::move(callback);
    deferredRefreshTimerId_ = SetTimer(
        tooltip_, reinterpret_cast<UINT_PTR>(this) + 1, std::max<UINT>(1, delayMs), &TooltipManager::DeferredRefreshTimerProc);
    if (deferredRefreshTimerId_ == 0) {
        auto cb = std::move(deferredRefreshCallback_);
        deferredRefreshCallback_ = nullptr;
        if (cb) cb();
    }
}

VOID CALLBACK TooltipManager::DeferredRefreshTimerProc(HWND, UINT, UINT_PTR timerId, DWORD) {
    auto* self = reinterpret_cast<TooltipManager*>(timerId - 1);
    if (!self || self->deferredRefreshTimerId_ != timerId) return;
    KillTimer(self->tooltip_, timerId);
    self->deferredRefreshTimerId_ = 0;
    auto cb = std::move(self->deferredRefreshCallback_);
    self->deferredRefreshCallback_ = nullptr;
    if (cb) cb();
}

bool TooltipManager::AddOrUpdate(ToolId id, const std::wstring& text, const RECT& clientRect) {
    if (!tooltip_ || !owner_ || id == 0 || id == kTrackingToolId || !IsWindow(tooltip_)) return false;
    auto it = tools_.find(id);
    if (it == tools_.end()) {
        ToolRecord record;
        record.text = text;
        record.clientRect = clientRect;
        tools_.emplace(id, record);
        if (!AddNativeTool(id, tools_.find(id)->second)) { tools_.erase(id); return false; }
        return true;
    }
    const bool textChanged = it->second.text != text;
    const bool rectChanged = !EqualRect(&it->second.clientRect, &clientRect);
    if (!textChanged && !rectChanged) return true;
    // Update the native record while the old map entry is still present, then commit the manager state only after the native operation
    // succeeds. This keeps GetTool() and the tooltip control in sync on an API failure.
    ToolRecord next;
    next.text = text;
    next.clientRect = clientRect;
    if (!UpdateNativeTool(id, next, textChanged)) return false;
    it->second = std::move(next);
    if (textChanged && trackedId_ == id && trackingToolAdded_) { EnsureTrackingTool(id); }
    return true;
}

bool TooltipManager::Remove(ToolId id) {
    auto it = tools_.find(id);
    if (it == tools_.end()) return false;
    if (trackedId_ == id || hoverId_ == id || pendingHoverId_ == id) HideTracking();
    if (tooltip_) {
        TOOLINFOW info{};
        info.cbSize = sizeof(info);
        info.hwnd = owner_;
        info.uId = id;
        SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
    tools_.erase(it);
    return true;
}

void TooltipManager::Clear() {
    if (!tools_.empty()) {
        std::vector<ToolId> ids;
        ids.reserve(tools_.size());
        for (const auto& pair : tools_) ids.push_back(pair.first);
        for (ToolId id : ids) Remove(id);
    }
    if (trackedId_ != 0 || pendingHoverId_ != 0) HideTracking();
}

bool TooltipManager::SetActive(bool active) {
    if (!tooltip_ || !IsWindow(tooltip_)) return false;
    active_ = active;
    SendMessageW(tooltip_, TTM_ACTIVATE, active ? TRUE : FALSE, 0);
    if (!active) HideForKeyboard();
    return true;
}

void TooltipManager::RelayMouseMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (!tooltip_ || !owner_) return;
    MSG relay{};
    relay.hwnd = owner_;
    relay.message = message;
    relay.wParam = wParam;
    relay.lParam = lParam;
    relay.time = GetMessageTime();
    GetCursorPos(&relay.pt);
    RelayMessage(relay);
}

void TooltipManager::RelayMessage(const MSG& message) {
    if (!tooltip_ || !owner_) return;
    MSG relay = message;
    if (!relay.hwnd) relay.hwnd = owner_;
    if (relay.message == WM_MOUSELEAVE) { HideTracking(); return; }
    const bool isMouseMove = relay.message == WM_MOUSEMOVE || relay.message == WM_NCMOUSEMOVE;
    // Mouse movement is handled by the manager's hit-test and delayed private tracking record. Relaying it as well would start a second
    // native timer for the public tool and could produce duplicate/competing popups.
    if (!isMouseMove) { SendMessageW(tooltip_, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&relay)); }
    if (!active_ || keyboardTracking_ || !isMouseMove) { return; }
    POINT clientPoint{};
    if (relay.message == WM_NCMOUSEMOVE) {
        clientPoint.x = GET_X_LPARAM(relay.lParam);
        clientPoint.y = GET_Y_LPARAM(relay.lParam);
        ScreenToClient(owner_, &clientPoint);
    } else { clientPoint.x = GET_X_LPARAM(relay.lParam); clientPoint.y = GET_Y_LPARAM(relay.lParam); }
    TTHITTESTINFO hit{};
    hit.hwnd = owner_;
    hit.pt = clientPoint;
    if (SendMessageW(tooltip_, TTM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit)) != FALSE && tools_.find(hit.ti.uId) != tools_.end()) {
        POINT screenPoint = relay.pt;
        if (screenPoint.x == 0 && screenPoint.y == 0) { screenPoint = clientPoint; ClientToScreen(owner_, &screenPoint); }
        if (hoverId_ == hit.ti.uId) {
            if (trackedId_ != hit.ti.uId) { ShowTracking(hit.ti.uId, screenPoint, false); }
        } else { ScheduleHover(hit.ti.uId, screenPoint); }
    } else if (hoverId_ != 0 || pendingHoverId_ != 0) { HideTracking(); }
}

POINT TooltipManager::DefaultKeyboardAnchor(ToolId id) const {
    POINT result{ 0, 0 };
    auto it = tools_.find(id);
    if (it == tools_.end() || !owner_) return result;
    RECT rect = it->second.clientRect;
    POINT topLeft{ rect.left, rect.top };
    POINT bottomRight{ rect.right, rect.bottom };
    ClientToScreen(owner_, &topLeft);
    ClientToScreen(owner_, &bottomRight);
    result.x = (topLeft.x + bottomRight.x) / 2;
    result.y = topLeft.y;
    return result;
}

bool TooltipManager::ShowForKeyboard(ToolId id, const POINT* screenAnchor) {
    if (!tooltip_ || !active_ || tools_.find(id) == tools_.end()) return false;
    POINT anchor = screenAnchor ? *screenAnchor : DefaultKeyboardAnchor(id);
    return ShowTracking(id, anchor, true);
}

void TooltipManager::HideForKeyboard() { if (keyboardTracking_ || trackedId_ != 0 || pendingHoverId_ != 0) HideTracking(); }

bool TooltipManager::SetTheme(bool darkMode, bool highContrast) {
    if (!tooltip_ || !IsWindow(tooltip_)) return false;
    COLORREF background = GetSysColor(COLOR_INFOBK);
    COLORREF text = GetSysColor(COLOR_INFOTEXT);
    if (!highContrast && darkMode) { background = RGB(38, 41, 52); text = RGB(242, 244, 248); }
    SendMessageW(tooltip_, TTM_SETTIPBKCOLOR, background, 0);
    SendMessageW(tooltip_, TTM_SETTIPTEXTCOLOR, text, 0);
    // Empty theme names allow the system/high-contrast theme to take over;
    // dark mode is requested through the standard immersive attribute where available, without changing activation/focus behavior.
    SetWindowTheme(tooltip_, highContrast ? L"" : nullptr, highContrast ? L"" : nullptr);
    if (!highContrast) {
        BOOL enabled = darkMode ? TRUE : FALSE;
        DwmSetWindowAttribute(tooltip_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &enabled, sizeof(enabled));
    }
    return true;
}

std::vector<TooltipManager::ToolId> TooltipManager::GetToolIds() const {
    std::vector<ToolId> ids;
    ids.reserve(tools_.size());
    for (const auto& pair : tools_) ids.push_back(pair.first);
    return ids;
}

} // namespace Aperture
