#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace Aperture {

class TooltipManager {
public:
    using ToolId = UINT_PTR;
    TooltipManager() = default;
    ~TooltipManager();
    // Creates a common-controls v6 tooltip owned by `owner`. Repeating Create
    // for the same valid owner is idempotent; changing owners recreates it.
    bool Create(HWND owner, int initialDelayMs = 500, int maxWidthPx = 360);
    void Destroy();
    bool AddOrUpdate(ToolId id, const std::wstring& text, const RECT& clientRect);
    bool Remove(ToolId id);
    void Clear();
    bool SetActive(bool active);
    // Relay mouse messages from a custom-drawn owner without subclassing it.
    void RelayMouseMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void RelayMessage(const MSG& message);
    // Shows a tool for keyboard focus without activating the tooltip window. The optional anchor is in screen coordinates. Without one, the
    // tool's client rectangle is converted to a point above its center.
    bool ShowForKeyboard(ToolId id, const POINT* screenAnchor = nullptr);
    void HideForKeyboard();
    // Applies standard-control colors and theme hints. High contrast takes
    // precedence over dark mode and follows COLOR_INFOBK/COLOR_INFOTEXT.
    bool SetTheme(bool darkMode, bool highContrast);
    HWND GetHandle() const { return tooltip_; }
    bool IsCreated() const { return tooltip_ != nullptr; }
    bool IsActive() const { return active_; }
    std::size_t ToolCount() const { return tools_.size(); }
    std::vector<ToolId> GetToolIds() const;

private:
    struct ToolRecord {
        std::wstring text;
        RECT clientRect{};
    };
    bool AddNativeTool(ToolId id, const ToolRecord& record);
    bool UpdateNativeTool(ToolId id, const ToolRecord& record, bool updateText);
    bool BuildToolInfo(ToolId id, TOOLINFOW& info) const;
    bool BuildToolInfoForRecord(ToolId id, const ToolRecord& record, UINT flags, TOOLINFOW& info) const;
    bool EnsureTrackingTool(ToolId sourceId);
    void RemoveTrackingTool();
    bool ShowTracking(ToolId id, const POINT& screenAnchor, bool keyboard);
    void HideTracking();
    void ScheduleHover(ToolId id, const POINT& screenAnchor);
    void CancelHoverTimer();
    static VOID CALLBACK HoverTimerProc(HWND, UINT, UINT_PTR timerId, DWORD);
    POINT DefaultKeyboardAnchor(ToolId id) const;
    void ApplyTiming(int initialDelayMs, int maxWidthPx);
    HWND owner_ = nullptr;
    HWND tooltip_ = nullptr;
    bool active_ = true;
    ToolId trackedId_ = 0;
    ToolId hoverId_ = 0;
    bool keyboardTracking_ = false, trackingToolAdded_ = false;
    ToolRecord trackingRecord_;
    UINT hoverDelayMs_ = 500;
    UINT_PTR hoverTimerId_ = 0;
    ToolId pendingHoverId_ = 0;
    POINT pendingHoverAnchor_{};
    std::map<ToolId, ToolRecord> tools_;
};

} // namespace Aperture
