#include "App.h"
#include "AppLayout.h"
#include "SettingsLayout.h"
#include "Presentation.h"
#include "Localization.h"

namespace Aperture {

namespace {

bool IsInputModalType(ModalType modal) {
    return modal == ModalType::NewPlaylist || modal == ModalType::NewSmartPlaylist ||
           modal == ModalType::EditSmartPlaylist || modal == ModalType::SeekStepInput ||
           modal == ModalType::FrameSeekInput || modal == ModalType::CustomSettingInput ||
           modal == ModalType::BookmarkInput || modal == ModalType::EditMetadataOverrides ||
           modal == ModalType::SaveSubtitleProfile || modal == ModalType::RenameSubtitleProfile ||
           modal == ModalType::JumpToTime || modal == ModalType::CustomAspectInput ||
           modal == ModalType::CustomRotationInput;
}

double ParseTimelineSeekTarget(const std::wstring& val, double safeDur) {
    std::wstring clean = val;
    while (!clean.empty() && iswspace(clean.front())) clean.erase(clean.begin());
    while (!clean.empty() && iswspace(clean.back())) clean.pop_back();
    if (clean.empty() || safeDur <= 0.0) return -1.0;

    if (clean.back() == L'%') {
        clean.pop_back();
        while (!clean.empty() && iswspace(clean.back())) clean.pop_back();
        try {
            return std::clamp((std::stod(clean) / 100.0) * safeDur, 0.0, safeDur);
        } catch (...) {
            return -1.0;
        }
    }
    if (clean.find(L':') != std::wstring::npos) {
        std::vector<double> parts;
        size_t start = 0;
        while (start < clean.size()) {
            size_t colon = clean.find(L':', start);
            if (colon == std::wstring::npos) colon = clean.size();
            std::wstring part = clean.substr(start, colon - start);
            while (!part.empty() && iswspace(part.front())) part.erase(part.begin());
            while (!part.empty() && iswspace(part.back())) part.pop_back();
            try { parts.push_back(std::stod(part)); } catch (...) { parts.push_back(0.0); }
            start = colon + 1;
        }
        double targetSec = 0.0;
        if (parts.size() == 1) targetSec = parts[0];
        else if (parts.size() == 2) targetSec = parts[0] * 60.0 + parts[1];
        else if (parts.size() >= 3) targetSec = parts[0] * 3600.0 + parts[1] * 60.0 + parts[2];
        return std::clamp(targetSec, 0.0, safeDur);
    }
    try {
        return std::clamp(std::stod(clean), 0.0, safeDur);
    } catch (...) {
        return -1.0;
    }
}

} // namespace

void App::ConfigureAccessibleScroll(AccessibilityNode& n, float App::*offset, float maximum, float viewHeight, float contentHeight,
                                    float lineHeight, float dp) {
    n.hasScroll = true;
    n.verticallyScrollable = true;
    n.verticalScrollPercent = std::clamp(static_cast<double>(((this->*offset) / maximum) * 100.0), 0.0, 100.0);
    n.verticalViewSize = contentHeight > 0.0f ? std::clamp(static_cast<double>((viewHeight / contentHeight) * 100.0), 0.0, 100.0) : 100.0;
    n.horizontallyScrollable = false;
    n.horizontalScrollPercent = UIA_ScrollPatternNoScroll;
    n.horizontalViewSize = 100.0;
    n.onScroll = [this, offset, maximum, viewHeight, lineHeight, dp](LONG, ScrollAmount, ScrollAmount amount) {
        if (amount == ScrollAmount_NoAmount) return;
        const float lineStep = lineHeight * dp;
        const float pageStep = std::max(lineStep, viewHeight - 20.0f * dp);
        if (amount == ScrollAmount_LargeIncrement) this->*offset += pageStep;
        else if (amount == ScrollAmount_SmallIncrement) this->*offset += lineStep;
        else if (amount == ScrollAmount_LargeDecrement) this->*offset -= pageStep;
        else if (amount == ScrollAmount_SmallDecrement) this->*offset -= lineStep;
        ClampScrollOffsets();
        InvalidateRect(hWnd, NULL, FALSE);
        MarkAccessibilityDirty();
    };
    n.onSetScrollPercent = [this, offset, maximum](LONG, double, double percent) {
        if (percent == UIA_ScrollPatternNoScroll) return;
        this->*offset = static_cast<float>((std::clamp(percent, 0.0, 100.0) / 100.0) * maximum);
        ClampScrollOffsets();
        InvalidateRect(hWnd, NULL, FALSE);
        MarkAccessibilityDirty();
    };
}

struct AccessibilityTreeBuilder {
    using TextInputField = App::TextInputField;
    using FocusPane = App::FocusPane;
    using AutoscrollPane = App::AutoscrollPane;
    using TransientSurfaceType = App::TransientSurfaceType;

    App& app;
    float dp = 1.0f;
    float winW = 1.0f;
    float winH = 1.0f;
    float padX = 0.0f;
    float padY = 0.0f;
    float viewTop = 0.0f;
    POINT screenOrigin{ 0, 0 };
    std::vector<AccessibilityNode> nodes;
    std::unordered_set<LONG> usedIds;

    AccessibilityTreeBuilder(App& a, const RECT& client, POINT origin)
        : app(a), screenOrigin(origin) {
        dp = std::max(0.5f, app.playbackSession.IsOpen() ? app.overlayRenderer.GetDpiScale() : app.renderer.GetDpiScale());
        winW = static_cast<float>(std::max<LONG>(1, client.right - client.left));
        winH = static_cast<float>(std::max<LONG>(1, client.bottom - client.top));
        padX = 18.0f * dp;
        padY = 14.0f * dp;
        viewTop = 84.0f * dp;
        nodes.reserve(64);
    }

    RECT ScreenRect(float left, float top, float right, float bottom) const {
        return RECT{
            screenOrigin.x + static_cast<LONG>(std::lround(left)),
            screenOrigin.y + static_cast<LONG>(std::lround(top)),
            screenOrigin.x + static_cast<LONG>(std::lround(right)),
            screenOrigin.y + static_cast<LONG>(std::lround(bottom))
        };
    }

    RECT PlayerRect(const D2D1_RECT_F& r) const { return ScreenRect(r.left, r.top, r.right, r.bottom); }

    bool Intersects(const RECT& rect) const {
        RECT viewport = {
            screenOrigin.x, screenOrigin.y + static_cast<LONG>(std::lround(viewTop)),
            screenOrigin.x + static_cast<LONG>(std::lround(winW)), screenOrigin.y + static_cast<LONG>(std::lround(winH))
        };
        RECT overlap{};
        return IntersectRect(&overlap, &rect, &viewport) != FALSE;
    }

    static LONG StateBits(bool selected, bool focused, bool disabled, bool offscreen, bool checked) {
        LONG state = disabled ? STATE_SYSTEM_UNAVAILABLE : STATE_SYSTEM_FOCUSABLE;
        if (selected) state |= STATE_SYSTEM_SELECTED;
        if (focused) state |= STATE_SYSTEM_FOCUSED;
        if (offscreen) state |= STATE_SYSTEM_OFFSCREEN;
        if (checked) state |= STATE_SYSTEM_CHECKED;
        return state;
    }

    std::wstring Shortcut(const std::string& actionId) const {
        const auto it = app.settings.shortcuts.find(actionId);
        return it == app.settings.shortcuts.end() ? std::wstring() : Settings::FormatShortcutKey(it->second);
    }

    AccessibilityNode& AddNode(LONG preferredId, const std::wstring& name, const std::wstring& description,
                               const std::wstring& value, LONG role, LONG state, const RECT& bounds,
                               const std::wstring& actionName, const std::wstring& key,
                               std::function<void()> action) {
        LONG id = std::max<LONG>(1, preferredId);
        while (usedIds.count(id)) ++id;
        usedIds.insert(id);
        AccessibilityNode node;
        node.id = id;
        node.name = name;
        node.description = description;
        node.value = value;
        node.defaultAction = actionName;
        node.keyboardShortcut = key;
        node.role = role;
        node.state = state;
        node.bounds = bounds;
        node.selected = (state & STATE_SYSTEM_SELECTED) != 0;
        node.focused = (state & STATE_SYSTEM_FOCUSED) != 0;
        if (action) {
            app.accessibilityActions[id] = std::move(action);
            App* pApp = &app;
            node.onDefaultAction = [pApp](LONG nodeId) { pApp->QueueAccessibilityAction(nodeId); };
            if (role == ROLE_SYSTEM_PUSHBUTTON || role == ROLE_SYSTEM_MENUITEM) {
                node.hasInvoke = true;
                node.onInvoke = node.onDefaultAction;
            }
        }
        nodes.push_back(std::move(node));
        return nodes.back();
    }

    AccessibilityNode& AddMetaNode(LONG preferredId, const std::wstring& name, const std::wstring& description,
                                   const std::wstring& value, LONG role, LONG state, const RECT& bounds,
                                   const std::wstring& actionName, const std::wstring& key,
                                   std::function<void()> action, const std::wstring& autoId,
                                   const wchar_t* className, LONG controlTypeId, const wchar_t* itemType) {
        AccessibilityNode& n = AddNode(preferredId, name, description, value, role, state, bounds, actionName, key, std::move(action));
        n.automationId = autoId;
        n.className = className;
        n.controlTypeId = controlTypeId;
        n.itemType = itemType;
        return n;
    }

    AccessibilityNode& AddButton(LONG preferredId, const std::wstring& name, const std::wstring& description,
                                 const std::wstring& value, LONG state, const RECT& bounds,
                                 const std::wstring& actionName, const std::wstring& key,
                                 std::function<void()> action, const std::wstring& autoId,
                                 const wchar_t* itemType) {
        return AddMetaNode(preferredId, name, description, value, ROLE_SYSTEM_PUSHBUTTON, state, bounds,
                           actionName, key, std::move(action), autoId, L"ApertureButton", UIA_ButtonControlTypeId, itemType);
    }

    AccessibilityNode& AddCmdButton(LONG preferredId, const std::wstring& name, const std::wstring& description,
                                    const std::wstring& value, LONG state, const RECT& bounds,
                                    const std::wstring& actionName, const char* shortcutAction, AppCommand cmd,
                                    const std::wstring& autoId, const wchar_t* itemType, bool enabled = true) {
        App* pApp = &app;
        std::function<void()> act;
        if (enabled) act = [pApp, cmd]() { pApp->commandDispatcher.Execute(cmd); };
        const std::wstring key = (shortcutAction && *shortcutAction) ? Shortcut(shortcutAction) : std::wstring();
        return AddButton(preferredId, name, description, value, state, bounds, actionName, key,
                         std::move(act), autoId, itemType);
    }

    void EnableToggle(AccessibilityNode& n, bool on) {
        n.hasToggle = true;
        n.toggleState = on ? ToggleState_On : ToggleState_Off;
        App* pApp = &app;
        n.onToggle = [pApp](LONG nodeId, ToggleState) {
            auto it = pApp->accessibilityActions.find(nodeId);
            if (it != pApp->accessibilityActions.end() && it->second) it->second();
        };
    }

    void Commit() {
        auto& statusNode = AddMetaNode(
            App::kStatusLiveRegionId,
            app.currentLiveAnnouncement.empty() ? L"Ready" : app.currentLiveAnnouncement,
            L"Status announcements and notifications", L"", ROLE_SYSTEM_STATUSBAR, STATE_SYSTEM_READONLY,
            ScreenRect(0.0f, winH - 24.0f * dp, winW, winH), L"", L"", nullptr,
            L"AppStatusLiveRegion", L"ApertureStatusLiveRegion", UIA_StatusBarControlTypeId, L"Status Region");
        statusNode.ariaRole = L"status";
        statusNode.liveSetting = Polite;
        app.accessibilityTree.ReplaceNodes(std::move(nodes));
    }

    void AppendModal() {
        const bool isInputModal = IsInputModalType(app.activeModal);
        const float maxBoxW = isInputModal ? 460.0f * dp : 500.0f * dp;
        const float boxW = std::min(maxBoxW, std::max(320.0f * dp, winW - 48.0f * dp));
        const float boxH = (isInputModal ? 204.0f : Presentation::ConfirmationHeight) * dp;
        const float left = (winW - boxW) * 0.5f, top = (winH - boxH) * 0.5f;
        const float btnH = 36.0f * dp, btnY = top + boxH - 20.0f * dp - btnH;
        const float confBtnW = isInputModal ? 92.0f * dp : std::max(110.0f * dp, app.renderer.MeasureString(app.modalConfirmText, FontType::Medium).width + 32.0f * dp);
        const float cancelBtnW = isInputModal ? 92.0f * dp : 100.0f * dp;
        const float confLeft = left + boxW - 24.0f * dp - confBtnW;
        const float cancelLeft = confLeft - 10.0f * dp - cancelBtnW;
        const std::wstring modalHeading = !app.modalTitle.empty() ? app.modalTitle : (!app.modalPrompt.empty() ? app.modalPrompt : L"Dialog");
        const std::wstring modalExplanation = !app.modalMessage.empty() ? app.modalMessage : (!app.modalPrompt.empty() ? app.modalPrompt : L"Modal dialog");

        App* pApp = &app;
        {
            auto& n = AddMetaNode(8050, modalHeading, modalExplanation, L"", ROLE_SYSTEM_GROUPING,
                                  StateBits(false, false, false, false, false),
                                  ScreenRect(left, top, left + boxW, top + boxH), L"", L"", nullptr,
                                  L"ModalDialog", L"ApertureDialog", UIA_GroupControlTypeId, L"Modal Dialog");
            n.ariaRole = L"dialog";
            n.helpText = modalExplanation;
            n.controllerForIds = isInputModal ? std::vector<LONG>{ 8000, 8001, 8002 } : std::vector<LONG>{ 8001, 8002 };
            n.flowsToIds = isInputModal ? std::vector<LONG>{ 8000 } : std::vector<LONG>{ 8001 };
        }
        if (isInputModal) {
            auto& n = AddMetaNode(8000, L"Modal input", app.modalPrompt, Text16ToWideString(app.modalInput.GetText()), ROLE_SYSTEM_TEXT,
                                  StateBits(false, app.modalKeyboardFocus == 0, false, false, false),
                                  ScreenRect(left + 24.0f * dp, top + 64.0f * dp, left + boxW - 24.0f * dp, top + 106.0f * dp),
                                  L"Edit", L"", [pApp]() { pApp->modalKeyboardFocus = 0; pApp->FocusTextInput(TextInputField::Modal); },
                                  L"ModalInput", L"ApertureTextBox", UIA_EditControlTypeId, L"Modal Input");
            n.labeledById = 8050;
            n.describedByIds = { 8050 };
            n.groupId = 8050;
            n.flowsFromIds = { 8050 };
            n.flowsToIds = { 8001 };
            n.hasValue = true;
            n.isReadOnly = false;
            n.onSetValue = [pApp](LONG, const std::wstring& val) {
                pApp->modalInput.SetText(WideStringToText16(val));
                pApp->MarkAccessibilityDirty();
                InvalidateRect(pApp->hWnd, NULL, FALSE);
            };
        }
        {
            const std::wstring confirmLabel = (app.activeModal == ModalType::DeletePlaylist || app.activeModal == ModalType::Confirmation)
                ? app.modalConfirmText : L"OK";
            auto& n = AddButton(8001, confirmLabel, modalExplanation, L"",
                                StateBits(false, app.isModalOkHovered || app.modalKeyboardFocus == (isInputModal ? 1 : 0), false, false, false),
                                ScreenRect(confLeft, btnY, confLeft + confBtnW, btnY + btnH), L"Confirm", L"Enter",
                                [pApp]() { pApp->DismissActiveModal(true); }, L"ModalConfirmButton", L"Modal Action");
            n.helpText = modalExplanation;
            n.describedByIds = { 8050 };
            n.groupId = 8050;
            n.flowsFromIds = isInputModal ? std::vector<LONG>{ 8000 } : std::vector<LONG>{ 8050 };
            n.flowsToIds = { 8002 };
        }
        {
            auto& n = AddButton(8002, L"Cancel", L"Close dialog without applying changes", L"",
                                StateBits(false, app.isModalCancelHovered || app.modalKeyboardFocus == (isInputModal ? 2 : 1), false, false, false),
                                ScreenRect(cancelLeft, btnY, cancelLeft + cancelBtnW, btnY + btnH), L"Cancel", L"Escape",
                                [pApp]() { pApp->DismissActiveModal(false); }, L"ModalCancelButton", L"Modal Action");
            n.describedByIds = { 8050 };
            n.groupId = 8050;
            n.flowsFromIds = { 8001 };
        }
        Commit();
    }

    void AppendShortcutCapture() {
        const float cardW = std::min(520.0f * dp, winW - 48.0f * dp);
        const float cardH = 180.0f * dp;
        const float cardLeft = (winW - cardW) * 0.5f, cardTop = (winH - cardH) * 0.5f;
        const float btnH = 36.0f * dp, btnW = 100.0f * dp;
        const float cancelLeft = cardLeft + cardW - 24.0f * dp - btnW;
        const float btnY = cardTop + cardH - 20.0f * dp - btnH;
        const std::wstring actionDisplayName = Utf8ToUtf16(app.rebindingActionName.empty() ? app.rebindingActionId : app.rebindingActionName);
        const std::wstring currentChord = app.GetCurrentCaptureChordText();
        App* pApp = &app;

        {
            auto& n = AddMetaNode(8150, L"Rebind Shortcut: " + actionDisplayName,
                                  L"Shortcut capture dialog. Press desired key combination with optional Ctrl, Shift, or Alt to rebind " + actionDisplayName + L". Press Escape to cancel.",
                                  L"", ROLE_SYSTEM_GROUPING, StateBits(false, false, false, false, false),
                                  ScreenRect(cardLeft, cardTop, cardLeft + cardW, cardTop + cardH), L"", L"", nullptr,
                                  L"ShortcutCaptureDialog", L"ApertureShortcutCaptureDialog", UIA_GroupControlTypeId, L"Shortcut Capture Dialog");
            n.ariaRole = L"dialog";
            n.helpText = L"Press desired key combination with optional Ctrl, Shift, or Alt modifiers. Press Escape to cancel.";
            n.controllerForIds = { 8100, 8101 };
            n.flowsToIds = { 8100 };
        }
        {
            auto& n = AddMetaNode(8100, L"Shortcut for " + actionDisplayName,
                                  L"Listening for key combination (supports Ctrl, Shift, and Alt modifiers)",
                                  currentChord, ROLE_SYSTEM_TEXT, StateBits(false, true, false, false, false),
                                  ScreenRect(cardLeft + 24.0f * dp, cardTop + 54.0f * dp, cardLeft + cardW - 24.0f * dp, cardTop + 96.0f * dp),
                                  L"Rebind", L"", []() {},
                                  L"ShortcutCaptureInput", L"ApertureShortcutCaptureBox", UIA_EditControlTypeId, L"Shortcut Capture Input");
            n.itemStatus = (currentChord == L"[ Press Key... ]") ? L"Listening" : currentChord;
            n.helpText = L"Press any key combination or Escape to cancel.";
            n.ariaRole = L"textbox";
            n.labeledById = 8150;
            n.describedByIds = { 8150 };
            n.groupId = 8150;
            n.flowsFromIds = { 8150 };
            n.flowsToIds = { 8101 };
            n.hasValue = true;
            n.isReadOnly = true;
            n.focused = true;
        }
        {
            auto& n = AddButton(8101, L"Cancel", L"Cancel shortcut capture without changes", L"",
                                StateBits(false, false, false, false, false),
                                ScreenRect(cancelLeft, btnY, cancelLeft + btnW, btnY + btnH), L"Cancel", L"Escape",
                                [pApp]() { pApp->CancelShortcutCapture(); },
                                L"ShortcutCaptureCancelButton", L"Modal Action");
            n.describedByIds = { 8150 };
            n.groupId = 8150;
            n.flowsFromIds = { 8100 };
            n.hasInvoke = true;
            n.onInvoke = [pApp](LONG) { pApp->CancelShortcutCapture(); };
        }
        Commit();
    }

    void AppendGlobalSearch() {
        auto proj = app.globalSearchModel.GetProjection();
        const float cardW = std::min(440.0f * dp, winW - 36.0f * dp);
        const float cardLeft = 18.0f * dp, cardTop = 76.0f * dp;
        const D2D1_RECT_F closeRect = D2D1::RectF(cardLeft + cardW - 42.0f * dp, cardTop + 10.0f * dp,
            cardLeft + cardW - 10.0f * dp, cardTop + 42.0f * dp);
        const D2D1_RECT_F inputRect = D2D1::RectF(cardLeft + 12.0f * dp, cardTop + 10.0f * dp,
            closeRect.left - 8.0f * dp, cardTop + 42.0f * dp);
        App* pApp = &app;

        std::vector<LONG> resultIds;
        resultIds.reserve(proj.results.size());
        for (size_t r = 0; r < proj.results.size(); ++r) {
            resultIds.push_back(AccessibilityStableId(9200 + static_cast<LONG>(r), proj.results[r].filePath));
        }

        {
            auto& n = AddMetaNode(9000, L"Global Search", L"Search media library and playback history", proj.query, ROLE_SYSTEM_TEXT,
                                  StateBits(false, app.activeTextInputField == TextInputField::GlobalSearch, false, false, false),
                                  ScreenRect(inputRect.left, inputRect.top, inputRect.right, inputRect.bottom),
                                  L"Edit", L"", [pApp]() { pApp->FocusTextInput(TextInputField::GlobalSearch); },
                                  L"GlobalSearchInput", L"ApertureTextBox", UIA_EditControlTypeId, L"Search Input");
            n.ariaRole = L"searchbox";
            if (!resultIds.empty()) n.flowsToIds = { resultIds.front() };
            n.controllerForIds = std::move(resultIds);
            n.hasValue = true;
            n.isReadOnly = false;
            n.onSetValue = [pApp](LONG, const std::wstring& val) {
                pApp->globalSearchModel.SetQuery(val, pApp->library, true);
                pApp->MarkAccessibilityDirty();
                InvalidateRect(pApp->hWnd, NULL, FALSE);
            };
        }
        AddButton(9001, L"Close Search", L"Close global search", L"",
                  StateBits(app.isGlobalSearchCloseHovered, false, false, false, false),
                  ScreenRect(closeRect.left, closeRect.top, closeRect.right, closeRect.bottom),
                  L"Close", L"Esc", [pApp]() { pApp->CloseGlobalSearch(); },
                  L"GlobalSearchClose", L"Search Action");

        float itemY = cardTop + 52.0f * dp;
        for (size_t i = 0; i < proj.results.size(); ++i) {
            const auto& srcItem = proj.results[i];
            const bool isSel = (static_cast<int>(i) == proj.selectedIndex);
            const bool isFoc = (app.activeTextInputField != TextInputField::GlobalSearch && isSel);
            const std::wstring itemTitle = srcItem.title.empty() ? srcItem.fileName : srcItem.title;
            const std::wstring itemDesc = srcItem.GetSourceDisplayName() + L" \x2022 " + srcItem.GetFormattedDuration();
            const RECT itemBounds = ScreenRect(cardLeft + 8.0f * dp, itemY, cardLeft + cardW - 8.0f * dp, itemY + 44.0f * dp);
            auto& n = AddMetaNode(
                AccessibilityStableId(9200 + static_cast<LONG>(i), srcItem.filePath), itemTitle,
                itemDesc, srcItem.filePath, ROLE_SYSTEM_LISTITEM,
                StateBits(isSel, isFoc, !srcItem.existsOnDisk, false, isSel), itemBounds,
                L"Play", L"Enter", [pApp]() {
                    auto action = pApp->globalSearchModel.ActivateSelection(pApp->library);
                    pApp->ExecuteGlobalSearchAction(action);
                },
                L"GlobalSearchItem_" + std::to_wstring(i), L"ApertureListItem", UIA_ListItemControlTypeId, L"Search Result");
            n.itemStatus = srcItem.GetSourceDisplayName();
            n.controlledPeerIds = { 9000 };
            n.selectionContainerId = 9000;
            n.positionInSet = static_cast<int>(i + 1);
            n.sizeOfSet = static_cast<int>(proj.results.size());
            n.flowsFromIds = (i == 0)
                ? std::vector<LONG>{ 9000 }
                : std::vector<LONG>{ AccessibilityStableId(9200 + static_cast<LONG>(i - 1), proj.results[i - 1].filePath) };
            if (i + 1 < proj.results.size()) {
                n.flowsToIds = { AccessibilityStableId(9200 + static_cast<LONG>(i + 1), proj.results[i + 1].filePath) };
            }
            n.hasSelectionItem = true;
            n.selected = isSel;
            n.onSelect = [pApp, i](LONG) {
                pApp->globalSearchModel.SetSelectedIndex(static_cast<int>(i));
                InvalidateRect(pApp->hWnd, NULL, FALSE);
            };
            n.hasScrollItem = true;
            n.onScrollIntoView = [pApp, i](LONG) {
                pApp->globalSearchModel.SetSelectedIndex(static_cast<int>(i));
                InvalidateRect(pApp->hWnd, NULL, FALSE);
                pApp->MarkAccessibilityDirty();
            };
            itemY += 46.0f * dp;
        }
        Commit();
    }

    void AppendContextMenu() {
        int totalMenuItems = 0;
        for (const auto& itm : app.contextMenuItems) {
            if (!itm.isSeparator) ++totalMenuItems;
        }
        float y = app.contextMenuOrigin.y + 8.0f * dp;
        int menuPos = 0;
        App* pApp = &app;
        for (size_t i = 0; i < app.contextMenuItems.size(); ++i) {
            const auto& item = app.contextMenuItems[i];
            if (item.isSeparator) { y += 6.0f * dp; continue; }
            const LONG itemId = 7000 + static_cast<LONG>(i);
            const RECT bounds = ScreenRect(app.contextMenuOrigin.x, y, app.contextMenuOrigin.x + 220.0f * dp, y + 32.0f * dp);
            const int curPos = menuPos++;
            std::function<void()> act;
            if (!item.isDisabled && item.onClick) {
                act = [pApp, i]() {
                    if (i < pApp->contextMenuItems.size() && !pApp->contextMenuItems[i].isDisabled && pApp->contextMenuItems[i].onClick) {
                        auto cb = pApp->contextMenuItems[i].onClick;
                        pApp->CloseContextMenu();
                        if (cb) cb();
                    }
                };
            }
            auto& n = AddMetaNode(
                itemId, item.label, item.shortcut.empty() ? L"Context menu action" : item.shortcut,
                item.isChecked ? L"Checked" : L"", ROLE_SYSTEM_MENUITEM,
                StateBits(item.isChecked, app.hoveredContextMenuItem == static_cast<int>(i), item.isDisabled, false, item.isChecked),
                bounds, item.isDisabled ? L"" : L"Activate", item.shortcut, std::move(act),
                L"", L"ApertureMenuItem", UIA_MenuItemControlTypeId, L"Menu Item");
            n.automationId = item.id.empty() ? (L"ContextMenuItem_" + std::to_wstring(n.id)) : (L"ContextMenuItem_" + Utf8ToUtf16(item.id));
            n.positionInSet = curPos + 1;
            n.sizeOfSet = totalMenuItems;
            if (i > 0) n.flowsFromIds = { static_cast<LONG>(7000 + i - 1) };
            if (i + 1 < app.contextMenuItems.size()) n.flowsToIds = { static_cast<LONG>(7000 + i + 1) };
            if (item.isChecked) EnableToggle(n, true);
            y += 32.0f * dp;
        }
        Commit();
    }

    void AppendSettingsFlyout(float bottomBarH) {
        const float flyoutW = 280.0f * dp;
        const float flyoutH = app.GetFlyoutHeight();
        const float left = winW - flyoutW - 20.0f * dp;
        const float top = winH - bottomBarH - flyoutH - 10.0f * dp;
        const int count = app.GetFlyoutOptionCount();
        const LONG menuOffset = static_cast<LONG>(app.currentFlyoutMenu.size() * 100);
        App* pApp = &app;

        auto addFlyout = [&](int index, const std::wstring& name, float x, float y,
                             std::function<void()> action) -> AccessibilityNode& {
            auto& n = AddMetaNode(
                9100 + index + menuOffset, name, L"Playback setting", L"", ROLE_SYSTEM_PUSHBUTTON,
                StateBits(index == app.hoveredFlyoutOption, false, false, false, false),
                ScreenRect(left + x, top + y, left + x + flyoutW - 28.0f * dp, top + y + 34.0f * dp),
                L"Select", L"", std::move(action),
                L"", L"ApertureMenuItem", UIA_MenuItemControlTypeId, L"Flyout Option");
            n.automationId = L"FlyoutItem_" + std::to_wstring(n.id);
            n.controlledPeerIds = { 9006 };
            n.selectionContainerId = 9006;
            n.positionInSet = index + 1;
            n.sizeOfSet = count;
            n.flowsFromIds = (index == 0) ? std::vector<LONG>{ 9006 } : std::vector<LONG>{ static_cast<LONG>(9100 + index - 1 + menuOffset) };
            if (index + 1 < count) n.flowsToIds = { static_cast<LONG>(9100 + index + 1 + menuOffset) };
            return n;
        };

        auto flyoutClickRow = [pApp](float relX, float relY) {
            return [pApp, relX, relY]() {
                const float d = pApp->overlayRenderer.GetDpiScale();
                pApp->HandleFlyoutClick(relX * d, relY * d);
            };
        };

        if (app.currentFlyoutMenu == "main") {
            const wchar_t* labels[] = { L"Subtitles / CC", L"Audio", L"Speed", L"Aspect Ratio", L"Queue", L"Loop", L"Autoplay", L"Private Session" };
            for (int i = 0; i < count && i < 8; ++i) {
                auto& n = addFlyout(i, labels[i], 14.0f * dp, 8.0f * dp + i * 42.0f * dp,
                                    flyoutClickRow(140.0f, 8.0f + i * 42.0f + 20.0f));
                if (i == 5) EnableToggle(n, app.settings.loop);
                else if (i == 6) EnableToggle(n, app.settings.autoplay);
                else if (i == 7) EnableToggle(n, app.playbackSession.IsPrivateMode());
            }
        } else if (app.currentFlyoutMenu == "queue") {
            const auto queue = app.playbackSession.GetQueue();
            if (queue.empty()) {
                auto& n = AddMetaNode(
                    9100 + menuOffset, L"Queue is empty", L"No queued media", L"",
                    ROLE_SYSTEM_STATICTEXT, StateBits(false, false, true, false, false),
                    ScreenRect(left + 14.0f * dp, top + 68.0f * dp, left + flyoutW - 14.0f * dp, top + 104.0f * dp),
                    L"", L"", nullptr,
                    L"QueueEmptyState", L"ApertureText", UIA_TextControlTypeId, L"Queue Status");
                n.controlledPeerIds = { 9006 };
            } else {
                const size_t first = std::min(app.queueFlyoutFirstVisible, queue.size() - 1);
                const size_t end = std::min(queue.size(), first + static_cast<size_t>(7));
                for (size_t i = first; i < end; ++i) {
                    const int visibleIndex = static_cast<int>(i - first);
                    const std::wstring title = GetFileTitleFromPath(queue[i]);
                    auto& n = addFlyout(visibleIndex, title, 14.0f * dp, 78.0f * dp + visibleIndex * 36.0f * dp,
                                        flyoutClickRow(140.0f, 78.0f + visibleIndex * 36.0f + 17.0f));
                    n.itemType = L"Queue Item";
                    n.hasSelectionItem = true;
                    n.selected = (i == app.playbackSession.GetQueueIndex());
                    n.itemStatus = n.selected ? L"Currently playing" : L"";
                }
            }
        } else if (app.currentFlyoutMenu == "speed") {
            const wchar_t* labels[] = { L"0.25x", L"0.5x", L"0.75x", L"1.0x", L"1.25x", L"1.5x", L"1.75x", L"2.0x", L"3.0x" };
            const double speeds[] = { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0 };
            for (int i = 0; i < 9; ++i) {
                const int row = i / 3, col = i % 3;
                const float pillW = (flyoutW - 28.0f * dp - 10.0f * dp) / 3.0f;
                const bool isSel = std::abs(app.player.GetPlaybackRate() - speeds[i]) < 0.05;
                const float rawW = (280.0f - 28.0f - 10.0f) / 3.0f;
                auto& n = addFlyout(i, labels[i], 14.0f * dp + col * (pillW + 5.0f * dp), 108.0f * dp + row * 35.0f * dp,
                                    flyoutClickRow(14.0f + col * (rawW + 5.0f) + rawW * 0.5f, 108.0f + row * 35.0f + 15.0f));
                n.hasSelectionItem = true;
                n.selected = isSel;
            }
        } else if (app.currentFlyoutMenu == "audiodelay" || app.currentFlyoutMenu == "subdelay") {
            const wchar_t* labels[] = { L"Decrease", L"Reset", L"Increase" };
            for (int i = 0; i < 3; ++i) {
                addFlyout(i, labels[i], 14.0f * dp + i * 82.0f * dp, 96.0f * dp,
                          flyoutClickRow(14.0f + i * 82.0f + 30.0f, 113.0f));
            }
        } else if (app.currentFlyoutMenu == "aspect") {
            const wchar_t* labels[] = { L"Auto", L"16:9", L"4:3", L"21:9", L"Fill", L"Cover" };
            for (int i = 0; i < 6; ++i) {
                addFlyout(i, labels[i], 14.0f * dp, 50.0f * dp + i * 36.0f * dp,
                          flyoutClickRow(140.0f, 50.0f + i * 36.0f + 16.0f));
            }
        } else if (app.currentFlyoutMenu == "subtitles") {
            auto tracks = app.player.GetSubtitleTracks();
            addFlyout(0, L"Off", 14.0f * dp, 50.0f * dp, flyoutClickRow(140.0f, 66.0f));
            for (size_t i = 0; i < tracks.size(); ++i) {
                addFlyout(static_cast<int>(i + 1), tracks[i].title, 14.0f * dp, 50.0f * dp + static_cast<float>(i + 1) * 36.0f * dp,
                          flyoutClickRow(140.0f, 50.0f + static_cast<float>(i + 1) * 36.0f + 16.0f));
            }
        } else if (app.currentFlyoutMenu == "audio") {
            auto tracks = app.player.GetAudioTracks();
            for (size_t i = 0; i < tracks.size(); ++i) {
                addFlyout(static_cast<int>(i), tracks[i].title, 14.0f * dp, 50.0f * dp + static_cast<float>(i) * 36.0f * dp,
                          flyoutClickRow(140.0f, 50.0f + static_cast<float>(i) * 36.0f + 16.0f));
            }
        }
    }

    void AppendPlayerOverlay() {
        const float playerPadX = Presentation::PlayerPadding * dp;
        const bool queuePlayback = app.playbackSession.IsQueuePlayback();
        const bool queuePrevDisabled = queuePlayback && app.playbackSession.GetQueueIndex() == 0 && !app.settings.loop;
        const auto playerLayout = Presentation::Player(winW, winH, dp, queuePlayback);
        const float bottomBarH = 76.0f * dp;
        const bool hidden = app.controlsOpacity <= 0.02f && !app.isSettingsFlyoutOpen;
        auto playerControlState = [&](LONG id, bool disabled = false) {
            return StateBits(false, app.playerKeyboardFocusId == id, disabled, hidden, false);
        };
        App* pApp = &app;

        if (app.playbackSession.IsPiP()) {
            const auto pip = Presentation::PiP(winW, winH, dp);
            const bool pipPrevDisabled = !app.playbackSession.IsQueuePlayback() || (app.playbackSession.GetQueueIndex() == 0 && !app.settings.loop);
            const bool pipNextDisabled = !app.playbackSession.IsQueuePlayback() || app.playbackSession.GetQueueSize() <= 1;
            const bool playing = app.player.IsPlaying();

            {
                auto& n = AddCmdButton(9020, playing ? L"Pause" : L"Play", L"Play or pause PiP media",
                                       playing ? L"Playing" : L"Paused", playerControlState(9020), PlayerRect(pip.playPause),
                                       playing ? L"Pause" : L"Play", "togglePlay", AppCommand::TogglePlay,
                                       L"PiPPlayPauseButton", L"Player Control");
                n.controllerForIds = { 9023, 9024, 9025, 9026, 9021, 9022 };
                n.flowsToIds = { 9023 };
                n.itemStatus = playing ? L"Playing" : L"Paused";
                EnableToggle(n, playing);
            }
            {
                auto& n = AddCmdButton(9023, L"Previous", L"Play the previous queued item",
                                       pipPrevDisabled ? L"Unavailable" : L"",
                                       playerControlState(9023, pipPrevDisabled), PlayerRect(pip.previous),
                                       L"Play", "playPrevious", AppCommand::PlayPrevious,
                                       L"PiPPreviousButton", L"Player Control", !pipPrevDisabled);
                n.flowsFromIds = { 9020 };
                n.flowsToIds = { 9024 };
            }
            {
                auto& n = AddCmdButton(9024, L"Next", L"Play the next queued item",
                                       pipNextDisabled ? L"Unavailable" : L"",
                                       playerControlState(9024, pipNextDisabled), PlayerRect(pip.next),
                                       L"Play", "playNext", AppCommand::PlayNext,
                                       L"PiPNextButton", L"Player Control", !pipNextDisabled);
                n.flowsFromIds = { 9023 };
                n.flowsToIds = { 9025 };
            }
            {
                auto& n = AddButton(9025, L"Volume", L"Toggle volume control", L"",
                                    playerControlState(9025), PlayerRect(pip.audio), L"Toggle", L"",
                                    [pApp]() {
                                        pApp->isPiPVolumeSliderOpen = !pApp->isPiPVolumeSliderOpen;
                                        if (pApp->isPiPVolumeSliderOpen) {
                                            pApp->pipVolumeAnim.SnapTo(0.0f);
                                            pApp->AnimateVisual(pApp->pipVolumeAnim, 1.0f, MotionTokens::kToggle, MotionTokens::Spring());
                                        } else {
                                            pApp->AnimateVisual(pApp->pipVolumeAnim, 0.0f, MotionTokens::kToggle, MotionTokens::Spring());
                                        }
                                        pApp->MarkAccessibilityDirty();
                                        pApp->Render();
                                    }, L"PiPVolumeButton", L"Player Control");
                n.flowsFromIds = { 9024 };
                n.flowsToIds = { 9026 };
            }
            {
                const bool pinned = app.playbackSession.IsPiPPinned();
                auto& n = AddButton(9026, pinned ? L"Unpin Picture-in-Picture" : L"Pin Picture-in-Picture",
                                    L"Toggle picture-in-picture always-on-top", pinned ? L"Pinned" : L"Not pinned",
                                    playerControlState(9026), PlayerRect(pip.pin), L"Toggle", L"",
                                    [pApp]() {
                                        pApp->playbackSession.TogglePiPPinned(pApp->hWnd);
                                        pApp->SyncOverlayPosition();
                                        pApp->MarkAccessibilityDirty();
                                        pApp->Render();
                                    }, L"PiPPinButton", L"Player Control");
                n.flowsFromIds = { 9025 };
                n.flowsToIds = { 9021 };
                n.itemStatus = pinned ? L"Pinned" : L"Not pinned";
                EnableToggle(n, pinned);
            }
            {
                auto& n = AddCmdButton(9021, L"Restore Window", L"Exit picture-in-picture mode", L"",
                                       playerControlState(9021), PlayerRect(pip.restore),
                                       L"Restore", "togglePiP", AppCommand::TogglePiP,
                                       L"PiPRestoreButton", L"Player Control");
                n.flowsFromIds = { 9026 };
                n.flowsToIds = { 9022 };
            }
            {
                auto& n = AddButton(9022, L"Close Player", L"Close player session", L"",
                                    playerControlState(9022), PlayerRect(pip.close), L"Close", Shortcut("closePlayer"),
                                    [pApp]() { SendMessage(pApp->hWnd, WM_CLOSE, 0, 0); },
                                    L"PiPCloseButton", L"Player Control");
                n.flowsFromIds = { 9021 };
            }
        } else {
            {
                auto& n = AddCmdButton(9001, L"Back", L"Return to the media library", L"",
                                       playerControlState(9001), PlayerRect(playerLayout.back),
                                       L"Back", "closePlayer", AppCommand::ClosePlayer,
                                       L"BackButton", L"Player Navigation");
                n.flowsToIds = { (queuePlayback && !queuePrevDisabled) ? 9010L : 9002L };
            }
            if (queuePlayback && app.playbackSession.GetQueueSize() > 0) {
                auto& n = AddCmdButton(9010, L"Previous in queue", L"Play the previous queued item",
                                       queuePrevDisabled ? L"Unavailable at start of queue" : L"",
                                       playerControlState(9010, queuePrevDisabled), PlayerRect(playerLayout.prev),
                                       L"Play", "playPrevious", AppCommand::PlayPrevious,
                                       L"PreviousButton", L"Player Control", !queuePrevDisabled);
                n.flowsFromIds = { 9001 };
                n.flowsToIds = { 9002 };
            }
            {
                const bool playing = app.player.IsPlaying();
                auto& n = AddCmdButton(9002, playing ? L"Pause" : L"Play", L"Play or pause the current media",
                                       playing ? L"Playing" : L"Paused", playerControlState(9002), PlayerRect(playerLayout.play),
                                       playing ? L"Pause" : L"Play", "togglePlay", AppCommand::TogglePlay,
                                       L"PlayPauseButton", L"Player Control");
                n.controllerForIds = { 9003, 9011 };
                n.flowsFromIds = { (queuePlayback && !queuePrevDisabled) ? 9010L : 9001L };
                n.flowsToIds = { 9011 };
                n.itemStatus = playing ? L"Playing" : L"Paused";
                EnableToggle(n, playing);
            }
            {
                auto& n = AddCmdButton(9011, Presentation::NextLabel(queuePlayback, app.playbackSession.GetQueueIndex(), app.playbackSession.GetQueueSize()),
                                       L"Play the next library item or queued item", L"",
                                       playerControlState(9011), PlayerRect(playerLayout.next),
                                       L"Play", "playNext", AppCommand::PlayNext,
                                       L"NextButton", L"Player Control");
                n.controlledPeerIds = { 9002 };
                n.flowsFromIds = { 9002 };
                n.flowsToIds = { 9003 };
            }
            AppendPlayerTimeline(playerPadX, bottomBarH, playerControlState(9003));
            {
                const bool muted = app.player.IsMuted();
                const std::wstring volStr = muted ? L"Muted" : (std::to_wstring(static_cast<int>(std::round(app.player.GetVolume() * 100.0))) + L"%");
                auto& n = AddCmdButton(9004, muted ? L"Unmute" : L"Mute", L"Mute or unmute audio", volStr,
                                       playerControlState(9004), PlayerRect(playerLayout.mute),
                                       muted ? L"Unmute" : L"Mute", "toggleMute", AppCommand::ToggleMute,
                                       L"MuteButton", L"Audio Control");
                n.controllerForIds = { 9012 };
                n.controlledPeerIds = { 9012 };
                n.flowsFromIds = { 9003 };
                n.flowsToIds = { 9012 };
                n.itemStatus = muted ? L"Muted" : L"Unmuted";
                EnableToggle(n, muted);
            }
            {
                const std::wstring volVal = std::to_wstring(static_cast<int>(std::round(app.player.GetVolume() * 100.0))) + L"%";
                auto& n = AddCmdButton(9012, L"Volume", L"Playback volume", volVal,
                                       playerControlState(9012) | (playerLayout.showVolume ? 0 : STATE_SYSTEM_INVISIBLE),
                                       PlayerRect(playerLayout.volume), L"Increase", "volumeUp", AppCommand::VolumeUp,
                                       L"VolumeSlider", L"Volume Slider");
                n.className = L"ApertureSlider";
                n.controlTypeId = UIA_SliderControlTypeId;
                n.labeledById = 9004;
                n.controllerForIds = { 9004 };
                n.controlledPeerIds = { 9004 };
                n.flowsFromIds = { 9004 };
                n.flowsToIds = { 9005 };
                n.hasRangeValue = true;
                n.rangeMin = 0.0;
                n.rangeMax = 100.0;
                n.rangeValue = std::clamp(app.player.GetVolume() * 100.0, 0.0, 100.0);
                n.rangeSmallChange = 5.0;
                n.rangeLargeChange = 10.0;
                n.rangeIsReadOnly = false;
                n.onSetRangeValue = [pApp](LONG, double val) {
                    pApp->commandDispatcher.Execute(AppCommand::SetVolume, CommandArgs{ std::clamp(val / 100.0, 0.0, 1.0) });
                };
            }
            {
                auto& n = AddCmdButton(9005, L"Subtitles / CC", L"Cycle subtitle tracks", app.player.GetCurrentSubtitleTrackTitle(),
                                       playerControlState(9005) | (playerLayout.showSubtitles ? 0 : STATE_SYSTEM_INVISIBLE),
                                       PlayerRect(playerLayout.subtitles), L"Toggle", "", AppCommand::ToggleSubtitles,
                                       L"SubtitlesButton", L"Player Control");
                n.flowsFromIds = { 9012 };
                n.flowsToIds = { 9006 };
                EnableToggle(n, app.settings.subtitlesEnabled);
            }
            {
                auto& n = AddButton(9006, L"Settings", L"Open playback settings", L"",
                                    playerControlState(9006), PlayerRect(playerLayout.settings), L"Open", L"",
                                    [pApp]() {
                                        pApp->PushFocusSnapshot(TransientSurfaceType::SettingsFlyout);
                                        pApp->isSettingsFlyoutOpen = true;
                                        pApp->currentFlyoutMenu = "main";
                                        pApp->controlsOpacity = 1.0f;
                                        pApp->flyoutClosing = false;
                                        pApp->flyoutAnim.SnapTo(0.0f);
                                        pApp->AnimateVisual(pApp->flyoutAnim, 1.0f, MotionTokens::kToggle, MotionTokens::Spring());
                                    }, L"PlayerSettingsButton", L"Player Control");
                n.flowsFromIds = { 9005 };
                n.flowsToIds = { 9007 };
                if (app.isSettingsFlyoutOpen) {
                    const int flyCount = app.GetFlyoutOptionCount();
                    n.controllerForIds.reserve(flyCount);
                    for (int f = 0; f < flyCount; ++f) {
                        n.controllerForIds.push_back(static_cast<LONG>(9100 + f + app.currentFlyoutMenu.size() * 100));
                    }
                }
            }
            {
                const bool isPiP = app.playbackSession.IsPiP();
                auto& n = AddCmdButton(9007, isPiP ? L"Exit Picture-in-Picture" : L"Picture-in-Picture",
                                       L"Toggle picture-in-picture mode", L"",
                                       playerControlState(9007) | (playerLayout.showPip ? 0 : STATE_SYSTEM_INVISIBLE),
                                       PlayerRect(playerLayout.pip), L"Toggle", "togglePiP", AppCommand::TogglePiP,
                                       L"PiPButton", L"Player Control");
                n.flowsFromIds = { 9006 };
                n.flowsToIds = { 9008 };
                EnableToggle(n, isPiP);
            }
            {
                const bool isFs = app.playbackSession.IsFullscreen();
                auto& n = AddCmdButton(9008, isFs ? L"Exit Fullscreen" : L"Fullscreen",
                                       L"Toggle fullscreen mode", L"", playerControlState(9008),
                                       PlayerRect(playerLayout.fullscreen), L"Toggle", "toggleFullscreen", AppCommand::ToggleFullscreen,
                                       L"FullscreenButton", L"Player Control");
                n.flowsFromIds = { 9007 };
                EnableToggle(n, isFs);
            }
        }
        if (app.isSettingsFlyoutOpen) AppendSettingsFlyout(bottomBarH);
        Commit();
    }

    void AppendPlayerTimeline(float playerPadX, float bottomBarH, LONG state) {
        const double curTime = std::max(0.0, app.player.GetCurrentTime());
        const double dur = app.player.GetDuration();
        const bool isFiniteDuration = (dur > 0.0 && std::isfinite(dur));
        const double safeDur = isFiniteDuration ? dur : 0.0;
        const double remTime = isFiniteDuration ? std::max(0.0, safeDur - curTime) : 0.0;
        const auto& chapters = app.GetCachedChapters();
        int curChap = app.player.GetCurrentChapter();
        if (curChap < 0 && !chapters.empty() && isFiniteDuration) {
            for (int i = static_cast<int>(chapters.size()) - 1; i >= 0; --i) {
                if (chapters[i].time <= curTime + 0.25) { curChap = i; break; }
            }
        }

        std::wstring timelineVal = isFiniteDuration
            ? (FormatDuration(curTime) + L" of " + FormatDuration(safeDur) + L" (-" + FormatDuration(remTime) + L" remaining)")
            : (FormatDuration(curTime) + L" (Live stream)");
        if (curChap >= 0 && curChap < static_cast<int>(chapters.size())) {
            timelineVal += L", Chapter " + std::to_wstring(curChap + 1) + L" of " + std::to_wstring(chapters.size());
            if (!chapters[curChap].title.empty()) timelineVal += L": " + chapters[curChap].title;
        }
        if (app.abLoopA >= 0.0 && app.abLoopB > app.abLoopA) {
            timelineVal += L", A-B Repeat: " + FormatDuration(app.abLoopA) + L" to " + FormatDuration(app.abLoopB);
        } else if (app.abLoopA >= 0.0) {
            timelineVal += L", A-B Start: " + FormatDuration(app.abLoopA);
        }
        const double cacheDur = app.player.GetCacheDuration();
        if (cacheDur > 0.0 && isFiniteDuration) {
            const double bufferedPos = std::min(safeDur, curTime + cacheDur);
            const int bufferedPct = static_cast<int>(std::round((bufferedPos / safeDur) * 100.0));
            timelineVal += L", " + std::to_wstring(bufferedPct) + L"% buffered";
        }

        std::wstring itemStatusStr;
        if (isFiniteDuration) {
            itemStatusStr = app.player.IsPlaying() ? L"Playing, Seekable" : L"Paused, Seekable";
            if (curChap >= 0 && curChap < static_cast<int>(chapters.size())) {
                itemStatusStr += L", Chapter " + std::to_wstring(curChap + 1) + L"/" + std::to_wstring(chapters.size());
            }
            if (app.abLoopA >= 0.0 && app.abLoopB > app.abLoopA) {
                itemStatusStr += L", A-B Active (" + FormatDuration(app.abLoopA) + L" - " + FormatDuration(app.abLoopB) + L")";
            } else if (app.abLoopA >= 0.0) {
                itemStatusStr += L", A-B Start: " + FormatDuration(app.abLoopA);
            }
            if (cacheDur > 0.0) {
                itemStatusStr += L", Buffer: " + std::to_wstring(static_cast<int>(std::round(cacheDur))) + L"s ahead";
            }
        } else {
            itemStatusStr = app.player.IsPlaying() ? L"Live stream, Playing, Non-seekable" : L"Live stream, Paused, Non-seekable";
        }

        const std::wstring helpTextStr = isFiniteDuration
            ? (L"Seekable playback timeline. Use Left/Right arrow keys to seek by " +
               std::to_wstring(static_cast<int>(std::round(app.settings.arrowSeekStepSec))) +
               L" seconds, or type a timestamp or percentage.")
            : L"Live stream playback timeline. Seeking is disabled for live media.";

        auto& n = AddMetaNode(
            9003, L"Timeline", helpTextStr, timelineVal, ROLE_SYSTEM_SLIDER, state,
            ScreenRect(playerPadX, winH - bottomBarH + 8.0f * dp, winW - playerPadX, winH - bottomBarH + 24.0f * dp),
            isFiniteDuration ? L"Seek" : L"", L"", {},
            L"TimelineSlider", L"ApertureSlider", UIA_SliderControlTypeId, L"Timeline Slider");
        n.itemStatus = std::move(itemStatusStr);
        n.helpText = helpTextStr;
        n.labeledById = 9002;
        n.controllerForIds = { 9002 };
        n.controlledPeerIds = { 9002, 9011 };
        n.flowsFromIds = { 9011 };
        n.flowsToIds = { 9004 };
        App* pApp = &app;
        if (isFiniteDuration) {
            n.hasRangeValue = true;
            n.rangeMin = 0.0;
            n.rangeMax = safeDur;
            n.rangeValue = std::clamp(std::floor(curTime), 0.0, safeDur);
            n.rangeSmallChange = std::max(1.0, app.settings.arrowSeekStepSec);
            n.rangeLargeChange = std::max(n.rangeSmallChange, app.settings.arrowSeekStepSec * 2.0);
            n.rangeIsReadOnly = false;
            n.onSetRangeValue = [pApp, safeDur](LONG, double val) {
                double clamped = std::clamp(val, 0.0, safeDur);
                pApp->commandDispatcher.Execute(AppCommand::SeekTo, CommandArgs{ clamped });
                pApp->AnnounceStatus(L"Seek to " + FormatDuration(clamped));
            };
            n.hasValue = true;
            n.isReadOnly = false;
            n.onSetValue = [pApp, safeDur](LONG, const std::wstring& val) {
                double target = ParseTimelineSeekTarget(val, safeDur);
                if (target < 0.0) return;
                pApp->commandDispatcher.Execute(AppCommand::SeekTo, CommandArgs{ target });
                pApp->AnnounceStatus(L"Seek to " + FormatDuration(target));
            };
        } else {
            n.hasRangeValue = false;
            n.rangeIsReadOnly = true;
            n.hasValue = true;
            n.isReadOnly = true;
        }
    }

    void AppendHubNav() {
        const bool isRtl = Localization::Instance().IsRtl();
        const float navW = winW - padX * 2.0f, navH = 48.0f * dp, tabW = navW / 5.0f;
        const std::wstring navNames[] = {
            Loc::Tr("nav.media"), Loc::Tr("nav.playlists"), Loc::Tr("nav.history"),
            Loc::Tr("nav.settings"), Loc::Tr("nav.open_file")
        };
        App* pApp = &app;
        for (int i = 0; i < 5; ++i) {
            const bool selected = i < 4 && static_cast<int>(app.currentTab) == i;
            const float nodeLeft = isRtl ? (winW - padX - (i + 1) * tabW) : (padX + i * tabW);
            const float nodeRight = isRtl ? (winW - padX - i * tabW) : (padX + (i + 1) * tabW);
            auto& n = AddMetaNode(
                100 + i, navNames[i], i == 4 ? L"Choose media files" : L"Switch library view", L"",
                (i == 4) ? ROLE_SYSTEM_PUSHBUTTON : ROLE_SYSTEM_PAGETAB,
                StateBits(selected, app.activeFocusPane == FocusPane::TopNavBar && app.topNavFocusedIndex == i, false, false, selected),
                ScreenRect(nodeLeft, padY, nodeRight, padY + navH), L"Activate", i == 4 ? L"Ctrl+O" : L"",
                [pApp, i]() {
                    if (i == 4) { pApp->OpenFileDialog(); }
                    else { pApp->currentTab = static_cast<TabView>(i); pApp->activeFocusPane = FocusPane::TopNavBar; pApp->topNavFocusedIndex = i; }
                },
                (i == 4) ? L"Nav_OpenFileButton" : (L"NavTab_" + std::to_wstring(i)),
                (i == 4) ? L"ApertureButton" : L"ApertureTabItem",
                (i == 4) ? UIA_ButtonControlTypeId : UIA_TabItemControlTypeId,
                (i == 4) ? L"Navigation Action" : L"Navigation Tab");
            n.positionInSet = i + 1;
            n.sizeOfSet = 5;
            if (i > 0) n.flowsFromIds = { static_cast<LONG>(100 + i - 1) };
            if (i < 4) {
                n.flowsToIds = { static_cast<LONG>(100 + i + 1) };
                n.hasSelectionItem = true;
                n.selected = selected;
                n.onSelect = [pApp, i](LONG) {
                    pApp->currentTab = static_cast<TabView>(i);
                    pApp->activeFocusPane = FocusPane::TopNavBar;
                    pApp->topNavFocusedIndex = i;
                    InvalidateRect(pApp->hWnd, NULL, FALSE);
                };
            }
            if (i == 0 && app.currentTab == TabView::Media) {
                n.hasSelection = true;
                n.canSelectMultiple = true;
                n.isSelectionRequired = false;
                n.onGetSelection = [pApp](LONG) -> std::vector<LONG> {
                    std::vector<LONG> result;
                    std::unordered_set<size_t> allSelected = pApp->selectedMediaIndices;
                    if (pApp->selectedMediaIndex >= 0 && static_cast<size_t>(pApp->selectedMediaIndex) < pApp->library.mediaItems.size()) {
                        allSelected.insert(static_cast<size_t>(pApp->selectedMediaIndex));
                    }
                    result.reserve(allSelected.size());
                    for (size_t origIdx : allSelected) {
                        if (origIdx < pApp->library.mediaItems.size()) {
                            result.push_back(AccessibilityStableId(1000, pApp->library.mediaItems[origIdx].filePath));
                        }
                    }
                    return result;
                };
                const float maxScroll = app.GetMaxScrollForPane(AutoscrollPane::Media);
                if (maxScroll > 0.5f) {
                    const float tbH = 32.0f * dp, mediaListTop = viewTop + tbH + 12.0f * dp;
                    const float mediaViewH = std::max(1.0f * dp, (winH - padY) - mediaListTop);
                    app.ConfigureAccessibleScroll(n, &App::mediaScrollY, maxScroll, mediaViewH, app.m_mediaLayoutCache.contentHeightDIP * dp, 50.0f, dp);
                }
            } else if (i == 1 && app.currentTab == TabView::Playlists) {
                n.hasSelection = true;
                n.canSelectMultiple = false;
                n.isSelectionRequired = false;
                n.onGetSelection = [pApp](LONG) -> std::vector<LONG> {
                    std::vector<LONG> result;
                    if (!pApp->activePlaylistId.empty()) {
                        result.push_back(AccessibilityStableId(3001, Utf8ToUtf16(pApp->activePlaylistId)));
                    }
                    return result;
                };
                const float railMax = app.GetMaxScrollForPane(AutoscrollPane::PlaylistRail);
                if (railMax > 0.5f) {
                    const float viewH = std::max(1.0f * dp, (winH - padY) - viewTop);
                    const float railContentH = 48.0f * dp - 2.0f * dp + app.library.playlists.size() * (52.0f * dp + 6.0f * dp);
                    const float railVisibleH = std::max(1.0f * dp, viewH - 34.0f * dp - 18.0f * dp);
                    app.ConfigureAccessibleScroll(n, &App::playlistRailScrollY, railMax, railVisibleH, railContentH, 58.0f, dp);
                }
            } else if (i == 2 && app.currentTab == TabView::History) {
                const float histMax = app.GetMaxScrollForPane(AutoscrollPane::History);
                if (histMax > 0.5f) {
                    const float viewH = std::max(1.0f * dp, (winH - padY) - viewTop);
                    const float historyContentBottom = app.library.history.empty()
                        ? kHistoryContentInset * dp
                        : HistoryContentHeight(app.GetHistoryProjection().groups, dp);
                    app.ConfigureAccessibleScroll(n, &App::historyScrollY, histMax, viewH, historyContentBottom, 84.0f, dp);
                }
            } else if (i == 3 && app.currentTab == TabView::Settings) {
                const float settingsMax = app.GetMaxScrollForPane(AutoscrollPane::Settings);
                if (settingsMax > 0.5f) {
                    const float viewH = std::max(1.0f * dp, (winH - padY) - viewTop);
                    app.ConfigureAccessibleScroll(n, &App::settingsScrollY, settingsMax, viewH, app.GetSettingsContentHeight(dp), 54.0f, dp);
                }
            }
        }
    }

    void AppendMediaTab(const D2D1_RECT_F& view) {
        App* pApp = &app;
        const float tbH = 32.0f * dp;
        const auto toolbar = ComputeMediaToolbarGeometry(D2D1::RectF(view.left, view.top, view.right, view.top + tbH), dp);
        {
            auto& n = AddMetaNode(
                1048, L"Filter Media", L"Filter media by title, filename, or series stem",
                Text16ToWideString(app.mediaSearchInput.GetText()), ROLE_SYSTEM_TEXT,
                StateBits(false, app.activeTextInputField == TextInputField::MediaSearch, false, false, false),
                ScreenRect(toolbar.search.left, toolbar.search.top, toolbar.search.right, toolbar.search.bottom),
                L"Edit", L"Ctrl+F", [pApp]() { pApp->FocusTextInput(TextInputField::MediaSearch); pApp->activeFocusPane = FocusPane::MediaList; },
                L"MediaToolbar_SearchInput", L"ApertureEdit", UIA_EditControlTypeId, L"Media Search Input");
            n.ariaRole = L"searchbox";
            n.hasValue = true;
            n.isReadOnly = false;
            n.onSetValue = [pApp](LONG, const std::wstring& val) {
                pApp->mediaSearchInput.SetText(WideStringToText16(val));
                pApp->MarkTextInputEdit(true);
            };
        }
        AddButton(1049, L"Sort Media", L"Current sort: " + app.GetLibrarySortLabel(), L"",
                  StateBits(false, false, false, false, false),
                  ScreenRect(toolbar.sort.left, toolbar.sort.top, toolbar.sort.right, toolbar.sort.bottom),
                  L"Open", L"", [pApp, sortRect = toolbar.sort, d = dp]() {
                      pApp->OpenMediaSortContextMenu(sortRect.left, sortRect.bottom + 6.0f * d);
                  }, L"MediaToolbar_Sort", L"Media Toolbar Action");

        const wchar_t* groupLabels[] = { L"Standalone", app.settings.smartSeriesDetection ? L"Series" : L"Folders", L"Listen" };
        const LibraryGroupMode groupModes[] = {
            LibraryGroupMode::Flat,
            app.settings.smartSeriesDetection ? LibraryGroupMode::Series : LibraryGroupMode::Folder,
            LibraryGroupMode::Listen
        };
        const D2D1_RECT_F groupRects[] = { toolbar.flat, toolbar.series, toolbar.listen };
        for (int g = 0; g < 3; ++g) {
            const bool active = app.libraryGroupMode == groupModes[g];
            const auto mode = groupModes[g];
            auto& n = AddMetaNode(
                1050 + g, groupLabels[g], L"Library grouping mode", active ? L"Selected" : L"", ROLE_SYSTEM_RADIOBUTTON,
                StateBits(active, false, false, false, active), PlayerRect(groupRects[g]),
                L"Select", L"", [pApp, mode]() { pApp->SetLibraryGroupMode(mode); },
                L"MediaGroup_" + std::wstring(groupLabels[g]), L"ApertureRadioButton", UIA_RadioButtonControlTypeId, L"Group Mode Option");
            n.positionInSet = g + 1;
            n.sizeOfSet = 3;
            n.selectionContainerId = 100;
            n.flowsFromIds = (g == 0) ? std::vector<LONG>{ 100 } : std::vector<LONG>{ static_cast<LONG>(1050 + g - 1) };
            if (g < 2) n.flowsToIds = { static_cast<LONG>(1050 + g + 1) };
            n.hasSelectionItem = true;
            n.selected = active;
            n.onSelect = [pApp, mode](LONG) { pApp->SetLibraryGroupMode(mode); };
        }

        app.EnsureMediaLayoutCache();
        const auto& visibleIndices = app.GetVisibleMediaIndices();
        if (visibleIndices.empty()) {
            const bool libraryEmpty = app.library.mediaItems.empty();
            const std::wstring title = libraryEmpty ? L"Your Media Library is empty" : L"No duplicate media found";
            const std::wstring body = libraryEmpty ? L"Open media or add a folder to start your portable library."
                                                   : L"Switch back to the full library view.";
            const float cardW = std::min(560.0f * dp, RectWidth(view) - 48.0f * dp);
            const D2D1_RECT_F card = D2D1::RectF(
                view.left + (RectWidth(view) - cardW) * 0.5f, view.top + 70.0f * dp,
                view.left + (RectWidth(view) + cardW) * 0.5f, view.top + 290.0f * dp);
            const size_t buttonCount = libraryEmpty ? 2u : 1u;
            {
                auto& n = AddMetaNode(1080, title, body, L"", ROLE_SYSTEM_GROUPING, StateBits(false, false, false, false, false),
                                      PlayerRect(card), L"", L"", nullptr,
                                      L"MediaEmptyState_Card", L"ApertureEmptyState", UIA_GroupControlTypeId, L"Empty State");
                n.controllerForIds = (buttonCount == 2) ? std::vector<LONG>{ 1081, 1082 } : std::vector<LONG>{ 1081 };
                n.flowsToIds = { 1081 };
            }
            const wchar_t* labels[] = { L"Open File", L"Add Folder" };
            for (size_t b = 0; b < buttonCount; ++b) {
                const std::wstring label = libraryEmpty ? labels[b] : L"Clear Filters";
                const float buttonW = libraryEmpty ? 128.0f * dp : 140.0f * dp, gap = 12.0f * dp;
                const float totalW = buttonCount * buttonW + (buttonCount - 1) * gap;
                const float left = view.left + (RectWidth(view) - totalW) * 0.5f + b * (buttonW + gap);
                const RECT bounds = ScreenRect(left, card.bottom - 58.0f * dp, left + buttonW, card.bottom - 20.0f * dp);
                std::function<void()> action;
                if (!libraryEmpty) action = [pApp]() { pApp->SetLibraryGroupMode(LibraryGroupMode::Flat); };
                else if (b == 0) action = [pApp]() { pApp->OpenFileDialog(); };
                else action = [pApp]() { pApp->AddWatchedFolderPrompt(); };
                const bool focused = app.activeFocusPane == FocusPane::MediaList && app.keyboardFocusedIndex == static_cast<int>(b);
                const wchar_t* autoId = (label == L"Open File") ? L"MediaEmptyState_OpenFile"
                    : (label == L"Add Folder" ? L"MediaEmptyState_AddFolder" : L"MediaEmptyState_ClearFilters");
                auto& n = AddButton(1081 + static_cast<LONG>(b), label, label, L"", StateBits(false, focused, false, false, false),
                                    bounds, L"Activate", (libraryEmpty && b == 0) ? L"Ctrl+O" : L"", std::move(action),
                                    autoId, L"Empty State Action");
                n.describedByIds = { 1080 };
                n.groupId = 1080;
                n.controlledPeerIds = { 1080 };
                n.positionInSet = static_cast<int>(b + 1);
                n.sizeOfSet = static_cast<int>(buttonCount);
                n.flowsFromIds = (b == 0) ? std::vector<LONG>{ 1080 } : std::vector<LONG>{ 1081 };
            }
            return;
        }

        const float scrollYDIP = app.mediaScrollY / dp;
        const float viewportHDIP = RectHeight(view) / dp;
        auto [firstVisible, lastVisible] = app.GetVisibleMediaRowRange(scrollYDIP, viewportHDIP, visibleIndices.size(), 2);
        std::unordered_set<size_t> indicesToInclude;
        for (size_t i = firstVisible; i < lastVisible; ++i) indicesToInclude.insert(i);
        if (app.activeFocusPane == FocusPane::MediaList && app.keyboardFocusedIndex >= 0 && app.keyboardFocusedIndex < static_cast<int>(visibleIndices.size())) {
            indicesToInclude.insert(static_cast<size_t>(app.keyboardFocusedIndex));
        }
        for (size_t vi = 0; vi < visibleIndices.size(); ++vi) {
            const size_t oIdx = visibleIndices[vi];
            if (app.selectedMediaIndices.count(oIdx) != 0 || app.selectedMediaIndex == static_cast<int>(oIdx)) {
                indicesToInclude.insert(vi);
            }
        }
        const float listTop = view.top + tbH + 12.0f * dp;
        for (size_t i : indicesToInclude) {
            if (i >= visibleIndices.size()) continue;
            const size_t origIdx = visibleIndices[i];
            if (origIdx >= app.library.mediaItems.size()) continue;
            const float y = listTop + (app.GetMediaRowTopDIP(i) * dp) - app.mediaScrollY;
            const float height = app.GetMediaRowHeightDIP(i) * dp;
            const RECT bounds = ScreenRect(view.left + dp, y, view.right - 2.0f * dp, y + height);
            const bool selected = app.selectedMediaIndices.count(origIdx) != 0 || app.selectedMediaIndex == static_cast<int>(origIdx);
            const bool focused = app.activeFocusPane == FocusPane::MediaList && app.keyboardFocusedIndex == static_cast<int>(i);
            if (!Intersects(bounds) && !focused && !selected) continue;

            const auto& item = app.library.mediaItems[origIdx];
            const std::wstring statusStr = Utf8ToUtf16(item.status);
            auto& n = AddMetaNode(
                AccessibilityStableId(1000, item.filePath),
                item.title.empty() ? GetFileNameFromPath(item.filePath) : item.title,
                item.filePath, statusStr, ROLE_SYSTEM_LISTITEM, StateBits(selected, focused, false, false, selected),
                bounds, L"Play", L"Enter", [pApp, path = item.filePath]() { pApp->OpenAndPlayMedia(path); },
                L"", L"ApertureListItem", UIA_ListItemControlTypeId, L"Media Item");
            n.automationId = L"MediaItem_" + std::to_wstring(n.id);
            n.itemStatus = statusStr;
            n.selectionContainerId = 100;
            n.controlledPeerIds = { 1050, 1051, 1052, 1053 };
            n.positionInSet = static_cast<int>(i + 1);
            n.sizeOfSet = static_cast<int>(visibleIndices.size());
            n.hasSelectionItem = true;
            n.selected = selected;
            n.onSelect = [pApp, origIdx, i](LONG) {
                if (origIdx >= pApp->library.mediaItems.size()) return;
                pApp->activeFocusPane = FocusPane::MediaList;
                pApp->keyboardFocusedIndex = static_cast<int>(i);
                pApp->selectionAnchorIndex = static_cast<int>(i);
                pApp->selectedMediaIndex = static_cast<int>(origIdx);
                pApp->selectedMediaIndices = { origIdx };
                InvalidateRect(pApp->hWnd, NULL, FALSE);
            };
            n.onAddToSelection = [pApp, origIdx, i](LONG) {
                if (origIdx >= pApp->library.mediaItems.size()) return;
                pApp->activeFocusPane = FocusPane::MediaList;
                pApp->keyboardFocusedIndex = static_cast<int>(i);
                pApp->selectionAnchorIndex = static_cast<int>(i);
                pApp->selectedMediaIndices.insert(origIdx);
                pApp->selectedMediaIndex = static_cast<int>(origIdx);
                InvalidateRect(pApp->hWnd, NULL, FALSE);
            };
            n.onRemoveFromSelection = [pApp, origIdx](LONG) {
                if (origIdx >= pApp->library.mediaItems.size()) return;
                pApp->selectedMediaIndices.erase(origIdx);
                if (pApp->selectedMediaIndex == static_cast<int>(origIdx)) {
                    pApp->selectedMediaIndex = pApp->selectedMediaIndices.empty() ? -1 : static_cast<int>(*pApp->selectedMediaIndices.begin());
                }
                InvalidateRect(pApp->hWnd, NULL, FALSE);
            };
            n.hasScrollItem = true;
            n.onScrollIntoView = [pApp, origIdx, i](LONG) {
                pApp->activeFocusPane = FocusPane::MediaList;
                pApp->keyboardFocusedIndex = static_cast<int>(i);
                pApp->selectionAnchorIndex = static_cast<int>(i);
                pApp->selectedMediaIndex = static_cast<int>(origIdx);
                pApp->selectedMediaIndices = { origIdx };
                pApp->AutoScrollToFocusedItem();
                pApp->ClampScrollOffsets();
                InvalidateRect(pApp->hWnd, NULL, FALSE);
                pApp->MarkAccessibilityDirty();
            };
        }
    }

    void AppendPlaylistsTab(const D2D1_RECT_F& view) {
        App* pApp = &app;
        const float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(view) * 0.42f));
        float y = view.top + 46.0f * dp - app.playlistRailScrollY;
        {
            auto& n = AddButton(3000, L"Add Playlist", L"Create a new playlist", L"",
                                StateBits(false, false, false, false, false),
                                ScreenRect(view.left, view.top, view.left + railW, view.top + 44.0f * dp),
                                L"Create", L"", [pApp]() { pApp->ShowNewPlaylistModal(); },
                                L"AddPlaylistButton", L"Playlist Action");
            if (!app.library.playlists.empty()) {
                n.flowsToIds = { AccessibilityStableId(3001, Utf8ToUtf16(app.library.playlists[0].id)) };
            }
        }
        for (size_t i = 0; i < app.library.playlists.size(); ++i) {
            const RECT bounds = ScreenRect(view.left, y, view.left + railW, y + 52.0f * dp);
            if (Intersects(bounds)) {
                const auto& pl = app.library.playlists[i];
                const bool selected = pl.id == app.activePlaylistId || app.selectedPlaylistIndices.count(i) != 0;
                const std::wstring wId = Utf8ToUtf16(pl.id);
                auto& n = AddMetaNode(
                    AccessibilityStableId(3001, wId), Utf8ToUtf16(pl.name), L"Playlist",
                    std::to_wstring(pl.itemPaths.size()) + L" items", ROLE_SYSTEM_LISTITEM,
                    StateBits(selected, app.activeFocusPane == FocusPane::PlaylistRail && app.keyboardFocusedIndex == static_cast<int>(i), false, false, selected),
                    bounds, L"Open", L"Enter",
                    [pApp, id = pl.id]() { pApp->activePlaylistId = id; pApp->currentTab = TabView::Playlists; pApp->activeFocusPane = FocusPane::PlaylistRail; },
                    L"PlaylistItem_" + wId, L"ApertureListItem", UIA_ListItemControlTypeId, L"Playlist");
                n.selectionContainerId = 101;
                n.positionInSet = static_cast<int>(i + 1);
                n.sizeOfSet = static_cast<int>(app.library.playlists.size());
                if (selected) {
                    n.controllerForIds = { 3050, 3051, 3052, 3053, 3054 };
                    if (!pl.IsSmart() && pl.itemPaths.empty()) n.controllerForIds.push_back(3056);
                }
                n.hasSelectionItem = true;
                n.selected = selected;
                n.onSelect = [pApp, id = pl.id](LONG) {
                    pApp->activePlaylistId = id;
                    pApp->currentTab = TabView::Playlists;
                    pApp->activeFocusPane = FocusPane::PlaylistRail;
                    InvalidateRect(pApp->hWnd, NULL, FALSE);
                };
                n.hasScrollItem = true;
                n.onScrollIntoView = [pApp, id = pl.id, i](LONG) {
                    pApp->activePlaylistId = id;
                    pApp->currentTab = TabView::Playlists;
                    pApp->activeFocusPane = FocusPane::PlaylistRail;
                    pApp->keyboardFocusedIndex = static_cast<int>(i);
                    pApp->AutoScrollToFocusedItem();
                    pApp->ClampScrollOffsets();
                    InvalidateRect(pApp->hWnd, NULL, FALSE);
                    pApp->MarkAccessibilityDirty();
                };
            }
            y += 58.0f * dp;
        }

        Playlist* playlist = app.library.FindPlaylist(app.activePlaylistId);
        if (!playlist) return;
        const bool hasItems = !playlist->itemPaths.empty();
        {
            std::function<void()> act;
            if (hasItems) act = [pApp]() { pApp->PlayPlaylist(pApp->activePlaylistId, 0); };
            auto& n = AddButton(3050, L"Play All", L"Play every item in this playlist", L"",
                                StateBits(false, false, !hasItems, false, false),
                                ScreenRect(view.left + railW + 16.0f * dp, view.top + 116.0f * dp, view.left + railW + 116.0f * dp, view.top + 152.0f * dp),
                                L"Play", L"", std::move(act), L"PlayAllButton", L"Playlist Action");
            n.controlledPeerIds = { AccessibilityStableId(3001, Utf8ToUtf16(app.activePlaylistId)) };
            n.flowsToIds = { 3051 };
        }
        {
            std::function<void()> act;
            if (hasItems) {
                act = [pApp]() {
                    Playlist* pl = pApp->library.FindPlaylist(pApp->activePlaylistId);
                    size_t resIdx = pl ? pApp->library.GetPlaylistResumeIndex(pl) : 0;
                    pApp->PlayPlaylist(pApp->activePlaylistId, resIdx);
                };
            }
            auto& n = AddButton(3051, L"Resume Playlist", L"Resume the current playlist", L"",
                                StateBits(false, false, !hasItems, false, false),
                                ScreenRect(view.left + railW + 124.0f * dp, view.top + 116.0f * dp, view.left + railW + 244.0f * dp, view.top + 152.0f * dp),
                                L"Resume", L"", std::move(act), L"ResumePlaylistButton", L"Playlist Action");
            n.flowsFromIds = { 3050 };
            n.flowsToIds = { 3052 };
        }
        {
            auto& n = AddButton(3052, L"Delete Playlist", L"Delete this playlist after confirmation", L"",
                                StateBits(false, false, false, false, false),
                                ScreenRect(view.right - 130.0f * dp, view.top + 116.0f * dp, view.right, view.top + 152.0f * dp),
                                L"Delete", L"", [pApp]() { pApp->ShowDeletePlaylistModal(pApp->activePlaylistId); },
                                L"DeletePlaylistButton", L"Playlist Action");
            n.flowsFromIds = { 3051 };
            n.flowsToIds = playlist->IsSmart() ? std::vector<LONG>{ 3055 } : std::vector<LONG>{ 3053 };
        }
        if (playlist->IsSmart()) {
            auto& n = AddButton(3055, L"Edit Smart Rules", L"Modify filter rules for this smart playlist", L"",
                                StateBits(false, false, false, false, false),
                                ScreenRect(view.right - 270.0f * dp, view.top + 116.0f * dp, view.right - 138.0f * dp, view.top + 152.0f * dp),
                                L"Edit", L"", [pApp]() { pApp->ShowEditSmartPlaylistModal(pApp->activePlaylistId); },
                                L"EditSmartRulesButton", L"Playlist Action");
            n.flowsFromIds = { 3052 };
            n.flowsToIds = { 3053 };
        }
        {
            auto& n = AddMetaNode(
                3053, L"Search Playlist", L"Filter playlist items by title or filename",
                Text16ToWideString(app.playlistSearchInput.GetText()), ROLE_SYSTEM_TEXT,
                StateBits(false, app.activeTextInputField == TextInputField::PlaylistSearch, false, false, false),
                ScreenRect(view.left + railW + 16.0f * dp, view.top + 178.0f * dp, view.right - 126.0f * dp, view.top + 214.0f * dp),
                L"Edit", L"", [pApp]() { pApp->FocusTextInput(TextInputField::PlaylistSearch); pApp->activeFocusPane = FocusPane::PlaylistDetail; },
                L"PlaylistSearchInput", L"ApertureTextBox", UIA_EditControlTypeId, L"Playlist Search");
            n.ariaRole = L"searchbox";
            n.flowsToIds = { 3054 };
            n.hasValue = true;
            n.isReadOnly = false;
            n.onSetValue = [pApp](LONG, const std::wstring& val) {
                pApp->playlistSearchInput.SetText(WideStringToText16(val));
                pApp->MarkAccessibilityDirty();
                InvalidateRect(pApp->hWnd, NULL, FALSE);
            };
        }
        {
            auto& n = AddButton(3054, app.playlistGridView ? L"List View" : L"Grid View", L"Toggle playlist layout",
                                app.playlistGridView ? L"Grid" : L"List",
                                StateBits(false, false, false, false, app.playlistGridView),
                                ScreenRect(view.right - 116.0f * dp, view.top + 178.0f * dp, view.right, view.top + 214.0f * dp),
                                L"Toggle", L"", [pApp]() {
                                    pApp->playlistGridView = !pApp->playlistGridView;
                                    pApp->settings.playlistViewMode = pApp->playlistGridView ? "grid" : "list";
                                    pApp->SaveSettings();
                                    InvalidateRect(pApp->hWnd, NULL, FALSE);
                                }, L"PlaylistLayoutToggleButton", L"Playlist Action");
            n.flowsFromIds = { 3053 };
            if (!hasItems && !playlist->IsSmart()) n.flowsToIds = { 3056 };
            EnableToggle(n, app.playlistGridView);
        }
        if (!hasItems && !playlist->IsSmart()) {
            const float detailLeft = view.left + railW + 16.0f * dp;
            const float paneRight = std::max(detailLeft + 120.0f * dp, view.right - 26.0f * dp);
            const float dropTop = view.top + 14.0f * dp + 50.0f * dp + 16.0f * dp;
            const float centerX = (detailLeft + paneRight) * 0.5f;
            const float addFilesW = 118.0f * dp, addFilesH = 34.0f * dp;
            auto& n = AddButton(3056, L"Add Files", L"Add one or more media files to this playlist", L"",
                                StateBits(false, false, false, false, false),
                                ScreenRect(centerX - addFilesW * 0.5f, dropTop + 168.0f * dp, centerX + addFilesW * 0.5f, dropTop + 168.0f * dp + addFilesH),
                                L"Add", L"", [pApp]() { pApp->OpenFileDialogForPlaylist(pApp->activePlaylistId); },
                                L"PlaylistAddFilesButton", L"Playlist Action");
            n.controlledPeerIds = { AccessibilityStableId(3001, Utf8ToUtf16(app.activePlaylistId)) };
            n.flowsFromIds = { 3054 };
        }

        const auto& visible = app.GetVisiblePlaylistIndices();
        const float listTop = view.top + 252.0f * dp, detailLeft = view.left + railW + 16.0f * dp, detailRight = view.right;
        const float paneW = std::max(120.0f * dp, detailRight - detailLeft - 26.0f * dp);
        const float itemGap = app.playlistGridView ? 10.0f * dp : 6.0f * dp;
        const size_t gridColumns = app.playlistGridView ? static_cast<size_t>(std::max(1.0f, std::floor((paneW + itemGap) / (190.0f * dp)))) : 1;
        const float gridW = app.playlistGridView ? std::max(120.0f * dp, (paneW - (gridColumns - 1) * itemGap) / static_cast<float>(gridColumns)) : paneW;
        const float rowH = app.playlistGridView ? 166.0f * dp : 62.0f * dp, rowPitch = rowH + itemGap;
        const float detailMax = app.GetMaxScrollForPane(AutoscrollPane::PlaylistDetail);
        {
            auto& n = AddMetaNode(
                3060, playlist->name.empty() ? L"Playlist Items" : (Utf8ToUtf16(playlist->name) + L" Items"),
                L"Playlist item list", std::to_wstring(playlist->itemPaths.size()) + L" items",
                ROLE_SYSTEM_LIST, StateBits(false, false, false, false, false),
                ScreenRect(detailLeft, listTop, detailRight, view.bottom), L"", L"", nullptr,
                L"PlaylistDetailList", L"ApertureList", UIA_ListControlTypeId, L"Playlist Detail Content");
            n.groupId = 101;
            n.selectionContainerId = 101;
            n.hasSelection = true;
            n.canSelectMultiple = false;
            n.isSelectionRequired = false;
            n.onGetSelection = [pApp, playlist](LONG) -> std::vector<LONG> {
                std::vector<LONG> result;
                for (size_t selIdx : pApp->selectedEpisodeIndices) {
                    if (selIdx < playlist->itemPaths.size()) {
                        result.push_back(AccessibilityStableId(3200, playlist->itemPaths[selIdx]));
                    }
                }
                return result;
            };
            if (detailMax > 0.5f) {
                float detailContentBottom = 14.0f * dp + 50.0f * dp + 16.0f * dp + 112.0f * dp + 16.0f * dp + 32.0f * dp + 12.0f * dp;
                const size_t visibleCount = visible.size();
                if (app.playlistGridView) {
                    const size_t rowsCount = (visibleCount + gridColumns - 1) / gridColumns;
                    detailContentBottom += rowsCount * (166.0f * dp + 10.0f * dp) - (visibleCount ? 10.0f * dp : 0.0f);
                } else {
                    detailContentBottom += visibleCount * (62.0f * dp + 6.0f * dp) - (visibleCount ? 6.0f * dp : 0.0f);
                }
                detailContentBottom += 40.0f * dp;
                const float detailViewH = std::max(1.0f * dp, (winH - padY) - viewTop);
                app.ConfigureAccessibleScroll(n, &App::playlistDetailScrollY, detailMax, detailViewH,
                                              detailContentBottom, app.playlistGridView ? 176.0f : 68.0f, dp);
            }
        }

        const float viewportH = std::max(1.0f * dp, view.bottom - listTop);
        size_t firstPos = 0, lastPos = visible.size();
        if (app.playlistGridView) {
            const int totalRows = static_cast<int>((visible.size() + gridColumns - 1) / gridColumns);
            const int firstRow = std::max<int>(0, static_cast<int>(std::floor((app.playlistDetailScrollY - rowH) / rowPitch)) - 1);
            const int lastRow = std::min<int>(totalRows - 1, static_cast<int>(std::ceil((app.playlistDetailScrollY + viewportH) / rowPitch)) + 1);
            firstPos = static_cast<size_t>(firstRow * static_cast<int>(gridColumns));
            lastPos = std::min<size_t>(visible.size(), static_cast<size_t>((lastRow + 1) * static_cast<int>(gridColumns)));
        } else {
            const int firstRow = std::max<int>(0, static_cast<int>(std::floor((app.playlistDetailScrollY - rowH) / rowPitch)) - 2);
            const int lastRow = std::min<int>(static_cast<int>(visible.size()), static_cast<int>(std::ceil((app.playlistDetailScrollY + viewportH) / rowPitch)) + 3);
            firstPos = static_cast<size_t>(firstRow);
            lastPos = std::min<size_t>(visible.size(), static_cast<size_t>(lastRow));
        }

        const LONG activePlStableId = AccessibilityStableId(3001, Utf8ToUtf16(app.activePlaylistId));
        for (size_t visiblePos = firstPos; visiblePos < lastPos; ++visiblePos) {
            const size_t index = visible[visiblePos];
            const float rowY = listTop - app.playlistDetailScrollY + (app.playlistGridView ? (visiblePos / gridColumns) * rowPitch : visiblePos * rowPitch);
            const float colX = app.playlistGridView ? detailLeft + (visiblePos % gridColumns) * (gridW + itemGap) : detailLeft;
            const RECT bounds = ScreenRect(colX, rowY, app.playlistGridView ? colX + gridW : detailRight, rowY + rowH);
            if (!Intersects(bounds) || index >= playlist->itemPaths.size()) continue;
            const std::wstring path = playlist->itemPaths[index];
            const MediaItem* item = app.library.FindByPath(path);
            const std::wstring name = item ? item->title : GetFileNameFromPath(path);
            const std::wstring itemStatus = item ? Utf8ToUtf16(item->status) : L"";
            const bool selected = app.selectedEpisodeIndices.count(index) != 0;
            auto& n = AddMetaNode(
                AccessibilityStableId(3200, path), name, path, itemStatus, ROLE_SYSTEM_LISTITEM,
                StateBits(selected, app.activeFocusPane == FocusPane::PlaylistDetail && app.keyboardFocusedIndex == static_cast<int>(index), false, false, selected),
                bounds, L"Play", L"Enter", [pApp, path]() { pApp->OpenAndPlayMedia(path); },
                L"", L"ApertureListItem", UIA_ListItemControlTypeId, L"Playlist Track");
            n.automationId = L"PlaylistDetailItem_" + std::to_wstring(n.id);
            n.itemStatus = itemStatus;
            n.groupId = 3060;
            n.selectionContainerId = 3060;
            n.controlledPeerIds = { 3053, activePlStableId };
            n.positionInSet = static_cast<int>(visiblePos + 1);
            n.sizeOfSet = static_cast<int>(visible.size());
            n.hasSelectionItem = true;
            n.selected = selected;
            n.onSelect = [pApp, index](LONG) {
                pApp->activeFocusPane = FocusPane::PlaylistDetail;
                pApp->keyboardFocusedIndex = static_cast<int>(index);
                pApp->selectedEpisodeIndices = { index };
                InvalidateRect(pApp->hWnd, NULL, FALSE);
            };
            n.hasScrollItem = true;
            n.onScrollIntoView = [pApp, index](LONG) {
                pApp->activeFocusPane = FocusPane::PlaylistDetail;
                pApp->keyboardFocusedIndex = static_cast<int>(index);
                pApp->selectedEpisodeIndices = { index };
                pApp->AutoScrollToFocusedItem();
                pApp->ClampScrollOffsets();
                InvalidateRect(pApp->hWnd, NULL, FALSE);
                pApp->MarkAccessibilityDirty();
            };
        }
    }

    void AppendHistoryTab(const D2D1_RECT_F& view) {
        App* pApp = &app;
        if (app.library.history.empty()) {
            const float cardW = std::min(520.0f * dp, RectWidth(view) - 48.0f * dp);
            const D2D1_RECT_F card = D2D1::RectF(
                view.left + (RectWidth(view) - cardW) * 0.5f, view.top + 70.0f * dp,
                view.left + (RectWidth(view) + cardW) * 0.5f, view.top + 290.0f * dp);
            {
                auto& n = AddMetaNode(4080, L"No watch history yet", L"Play something and it will appear here.", L"",
                                      ROLE_SYSTEM_GROUPING, StateBits(false, false, false, false, false), PlayerRect(card),
                                      L"", L"", nullptr, L"HistoryEmptyState_Card", L"ApertureEmptyState", UIA_GroupControlTypeId, L"Empty State");
                n.controllerForIds = { 4081 };
                n.flowsToIds = { 4081 };
            }
            const float buttonW = 128.0f * dp;
            const float left = view.left + (RectWidth(view) - buttonW) * 0.5f;
            auto& n = AddButton(4081, L"Open File", L"Choose media to play", L"",
                                StateBits(false, app.activeFocusPane == FocusPane::HistoryList && app.keyboardFocusedIndex == 0, false, false, false),
                                ScreenRect(left, card.bottom - 58.0f * dp, left + buttonW, card.bottom - 20.0f * dp),
                                L"Activate", L"Ctrl+O", [pApp]() { pApp->OpenFileDialog(); },
                                L"HistoryEmptyState_OpenFile", L"Empty State Action");
            n.describedByIds = { 4080 };
            n.groupId = 4080;
            n.controlledPeerIds = { 4080 };
            n.flowsFromIds = { 4080 };
        } else {
            const HistoryTimelineProjection& proj = app.GetHistoryProjection();
            float headerY = view.top + kHistoryContentInset * dp - app.historyScrollY;
            for (const auto& grp : proj.groups) {
                const HistoryGroupLayout layout(headerY, grp.count, dp);
                headerY = layout.NextHeaderTop();
                const LONG grpNodeId = 4080 + static_cast<int>(grp.group) * 2;
                const RECT grpBounds = ScreenRect(view.left + 12.0f * dp, layout.headerTop,
                                                  view.right - 24.0f * dp, layout.headerTop + layout.headerHeight);
                if (Intersects(grpBounds)) {
                    auto& n = AddMetaNode(
                        grpNodeId, grp.title, grp.title + L" (" + grp.countText + L")", L"",
                        ROLE_SYSTEM_GROUPING, StateBits(false, false, false, false, false), grpBounds, L"", L"", nullptr,
                        L"HistoryGroup_" + std::wstring(grp.title), L"ApertureGroupHeader", UIA_GroupControlTypeId, L"History Group Header");
                    n.groupId = 102;
                    n.controllerForIds.reserve(grp.count);
                    for (size_t itmIdx = grp.startIndex; itmIdx < grp.startIndex + grp.count; ++itmIdx) {
                        if (itmIdx < proj.itemIndices.size() && proj.itemIndices[itmIdx] < app.library.history.size()) {
                            n.controllerForIds.push_back(AccessibilityStableId(4000, app.library.history[proj.itemIndices[itmIdx]].filePath));
                        }
                    }
                    if (!n.controllerForIds.empty()) n.flowsToIds = { n.controllerForIds.front() };
                }
                const auto [firstRow, lastRow] = layout.VisibleRows(viewTop - 1.0f, winH + 1.0f);
                for (size_t row = firstRow; row < lastRow; ++row) {
                    const size_t idx = grp.startIndex + row;
                    const size_t origIdx = proj.itemIndices[idx];
                    if (origIdx >= app.library.history.size()) continue;
                    const auto& record = app.library.history[origIdx];
                    const float y = layout.RowTop(row);
                    const RECT bounds = ScreenRect(view.left + 12.0f * dp, y, view.right - 24.0f * dp, y + layout.rowHeight);
                    if (!Intersects(bounds)) continue;

                    const bool selected = app.selectedHistoryIndices.count(origIdx) != 0;
                    const std::wstring itemTitle = record.title.empty() ? GetFileNameFromPath(record.filePath) : record.title;
                    const std::wstring itemDesc = grp.title + L" \x2022 " + FormatHistoryProgress(record);
                    auto& n = AddMetaNode(
                        AccessibilityStableId(4000, record.filePath), itemTitle, record.filePath, itemDesc, ROLE_SYSTEM_LISTITEM,
                        StateBits(selected, app.activeFocusPane == FocusPane::HistoryList && app.keyboardFocusedIndex == static_cast<int>(origIdx), false, false, selected),
                        bounds, L"Resume", L"Enter",
                        [pApp, path = record.filePath, pos = record.lastPosSec]() { pApp->OpenAndPlayMedia(path, pos); },
                        L"", L"ApertureListItem", UIA_ListItemControlTypeId, L"History Item");
                    n.automationId = L"HistoryItem_" + std::to_wstring(n.id);
                    n.groupId = grpNodeId;
                    n.describedByIds = { grpNodeId };
                    n.controlledPeerIds = { grpNodeId };
                    n.selectionContainerId = 102;
                    n.positionInSet = static_cast<int>(idx - grp.startIndex + 1);
                    n.sizeOfSet = static_cast<int>(grp.count);
                    n.hasSelectionItem = true;
                    n.selected = selected;
                    n.onSelect = [pApp, origIdx](LONG) {
                        pApp->selectedHistoryIndices = { origIdx };
                        InvalidateRect(pApp->hWnd, NULL, FALSE);
                    };
                    n.hasScrollItem = true;
                    n.onScrollIntoView = [pApp, origIdx](LONG) {
                        pApp->activeFocusPane = FocusPane::HistoryList;
                        pApp->keyboardFocusedIndex = static_cast<int>(origIdx);
                        pApp->selectedHistoryIndices = { origIdx };
                        pApp->AutoScrollToFocusedItem();
                        pApp->ClampScrollOffsets();
                        InvalidateRect(pApp->hWnd, NULL, FALSE);
                        pApp->MarkAccessibilityDirty();
                    };
                }
            }
        }
        const D2D1_SIZE_F clearSize = app.renderer.MeasureString(L"Clear History", FontType::Medium);
        const float clearW = clearSize.width + 24.0f * dp, clearH = 32.0f * dp;
        const float clearTop = view.top + (46.0f * dp - clearH) * 0.5f;
        AddButton(4090, L"Clear Watch History", L"Erase watch history", L"",
                  StateBits(false, app.isClearHistoryHovered, false, false, false),
                  ScreenRect(view.right - clearW - 6.0f * dp, clearTop, view.right - 6.0f * dp, clearTop + clearH),
                  L"Clear", L"", [pApp]() {
                      pApp->ShowConfirmationModal(
                          L"Clear Watch History",
                          L"Erase all watch history and playback progress? You can undo this afterward.",
                          L"Clear History",
                          [pApp]() {
                              pApp->ArmLibraryUndo(L"History clear");
                              pApp->library.ClearHistory();
                              pApp->SaveLibrary();
                              pApp->selectedHistoryIndices.clear();
                              pApp->isLastWatchedCleared = true;
                              pApp->InvalidateMediaLayoutCache();
                              InvalidateRect(pApp->hWnd, NULL, FALSE);
                          });
                  }, L"ClearHistoryButton", L"History Action");
    }

    void AppendSettingsTab(const D2D1_RECT_F& view) {
        App* pApp = &app;
        const float stripTop = view.top + SettingsLayout::kSidebarTopInset * dp;
        const float stripH = SettingsLayout::kSidebarItemHeight * dp;
        const float sideW = SettingsLayout::kSidebarWidth * dp;
        const wchar_t* cats[] = { L"Experience", L"Appearance", L"Playback", L"Audio & Subtitles", L"Shortcuts", L"System" };
        for (int i = 0; i < 6; ++i) {
            const float catTop = stripTop + i * (stripH + SettingsLayout::kSidebarItemGap * dp);
            const bool isSel = static_cast<int>(app.currentSettingsCat) == i;
            auto& n = AddMetaNode(
                500 + i, cats[i], L"Settings category", L"", ROLE_SYSTEM_PAGETAB,
                StateBits(isSel, app.activeFocusPane == FocusPane::SettingsCategoryList && app.keyboardFocusedIndex == i, false, false, isSel),
                ScreenRect(view.left + 8.0f * dp, catTop, view.left + sideW - 8.0f * dp, catTop + stripH),
                L"Open", L"", [pApp, i]() { pApp->currentSettingsCat = static_cast<SettingsCategory>(i); pApp->currentTab = TabView::Settings; pApp->settingsScrollY = 0.0f; },
                L"SettingsCategory_" + std::wstring(cats[i]), L"ApertureTabItem", UIA_TabItemControlTypeId, L"Settings Category");
            n.positionInSet = i + 1;
            n.sizeOfSet = 6;
            n.selectionContainerId = 103;
            if (i > 0) n.flowsFromIds = { static_cast<LONG>(500 + i - 1) };
            if (i < 5) n.flowsToIds = { static_cast<LONG>(500 + i + 1) };
            n.hasSelectionItem = true;
            n.selected = isSel;
            n.onSelect = [pApp, i](LONG) {
                pApp->currentSettingsCat = static_cast<SettingsCategory>(i);
                pApp->currentTab = TabView::Settings;
                pApp->settingsScrollY = 0.0f;
                InvalidateRect(pApp->hWnd, NULL, FALSE);
            };
            if (isSel) {
                const float settingsMax = app.GetMaxScrollForPane(AutoscrollPane::Settings);
                if (settingsMax > 0.5f) {
                    const float viewH = std::max(1.0f * dp, (winH - padY) - viewTop);
                    app.ConfigureAccessibleScroll(n, &App::settingsScrollY, settingsMax, viewH, app.GetSettingsContentHeight(dp), 54.0f, dp);
                }
            }
        }

        const float rowTop = view.top + (SettingsLayout::kContentStartOffset +
            (app.currentSettingsCat == SettingsCategory::Subtitles ? SettingsLayout::kSubtitlePageHeaderReserve : SettingsLayout::kPageHeaderReserve)) * dp - app.settingsScrollY;
        const bool scSingleColumn = SettingsLayout::ShortcutsSingleColumn(app.SettingsContentWidth(dp), dp);
        const bool simpleSettings = FeatureGate::ModeFromString(app.settings.experienceMode) == ExperienceMode::Simple;
        std::vector<std::pair<std::wstring, std::wstring>> rows;

        if (app.currentSettingsCat == SettingsCategory::Appearance) {
            rows = {
                { Loc::Tr("settings.appearance.theme_palette"), Utf8ToUtf16(app.settings.themePreset) },
                { Loc::Tr("settings.appearance.display_language"), Utf8ToUtf16(app.settings.appLanguage) }
            };
        } else if (app.currentSettingsCat == SettingsCategory::Playback) {
            rows = {
                { Loc::Tr("settings.playback.autoplay"), app.settings.autoplay ? L"On" : L"Off" },
                { L"Remember Last Frame & Position", app.settings.rememberPos ? L"On" : L"Off" },
                { L"Hardware Accelerated Video Decoding", app.settings.hwAccel ? L"On" : L"Off" },
                { L"Loop Playback", app.settings.loop ? L"On" : L"Off" },
                { L"Arrow Keys Seek Step", std::to_wstring(static_cast<int>(app.settings.arrowSeekStepSec)) + L" sec" },
                { L"Frame Step Size", std::to_wstring(app.settings.ctrlSeekFrames) + (app.settings.ctrlSeekFrames == 1 ? L" frame" : L" frames") },
                { L"Default Playback Speed", std::to_wstring(app.settings.defaultSpeed) + L"x" }
            };
            if (!simpleSettings) {
                rows.insert(rows.end(), {
                    { L"Enable Custom MPV Configuration", app.settings.customMpvConfigEnabled ? L"On" : L"Off" },
                    { L"Custom MPV Configuration Actions", L"Open / Reset / Revert" },
                    { L"Global Media Hotkeys", app.settings.globalHotkeysEnabled ? L"On" : L"Off" },
                    { L"Show Tray Icon", app.settings.trayIconEnabled ? L"On" : L"Off" },
                    { L"Minimize to Tray", app.settings.minimizeToTray ? L"On" : L"Off" },
                    { L"Network Cache (Forward)", std::to_wstring(app.settings.networkForwardCacheMB) + L" MB" },
                    { L"Network Cache (Back)", app.settings.networkBackCacheMB == 0 ? L"Off" : std::to_wstring(app.settings.networkBackCacheMB) + L" MB" }
                });
            }
        } else if (app.currentSettingsCat == SettingsCategory::Subtitles) {
            rows = {
                { L"Preferred Audio Track", Utf8ToUtf16(app.settings.audioLang) },
                { L"Preferred Subtitle Track", Utf8ToUtf16(app.settings.subLang) },
                { L"Subtitle Display Font Size", Utf8ToUtf16(app.settings.subFontSize) },
                { L"Subtitle Text Color", Utf8ToUtf16(app.settings.subColorHex) },
                { L"Subtitle Background & Box Style", Utf8ToUtf16(app.settings.subBgStyle) },
                { L"Subtitle Screen Position", Utf8ToUtf16(app.settings.subPosition) }
            };
        } else if (app.currentSettingsCat == SettingsCategory::Shortcuts) {
            rows.push_back({ L"Reset Shortcuts", L"Restore defaults" });
            for (const auto& pair : app.settings.GetShortcutActionListRef()) {
                rows.push_back({ Utf8ToUtf16(pair.second), Shortcut(pair.first) });
            }
        } else if (app.currentSettingsCat == SettingsCategory::Experience) {
            const ExperienceMode expMode = FeatureGate::ModeFromString(app.settings.experienceMode);
            rows.push_back({ Loc::Tr("settings.experience.title"),
                             Loc::Tr(std::string("settings.experience.mode.") + FeatureGate::ModeToString(expMode)) });
            if (expMode == ExperienceMode::Custom) {
                for (size_t c = 0; c < FeatureGate::Catalog().size(); ++c) {
                    const bool enabled = FeatureGate::FeatureEnabledAt(expMode, app.settings.customEnabledFeatures, c);
                    rows.push_back({ Loc::Tr(FeatureGate::Catalog()[c].displayNameKey), enabled ? L"On" : L"Off" });
                }
            }
        } else {
            std::wstring updateTitle = Loc::Tr("settings.system.check_updates");
            if (!app.updateStatusMessage.empty()) updateTitle += L" - " + app.updateStatusMessage;
            std::wstring updateBtn = Loc::Tr("settings.system.btn.check_now");
            if (app.updatePhase == App::UpdatePhase::Checking ||
                app.updatePhase == App::UpdatePhase::Downloading ||
                app.updatePhase == App::UpdatePhase::Verifying) {
                updateBtn = Loc::Tr("settings.system.btn.cancel");
            } else if (app.updatePhase == App::UpdatePhase::Installing) {
                updateBtn = Loc::Tr("settings.system.btn.installing");
            } else if (app.updatePhase == App::UpdatePhase::Available) {
                updateBtn = Loc::Tr("settings.system.btn.install");
            } else if (app.updatePhase == App::UpdatePhase::Failed) {
                updateBtn = Loc::Tr("settings.system.btn.retry");
            }
            rows = {
                { updateTitle, updateBtn },
                { Loc::Tr("settings.system.hover_previews"), app.settings.hoverPreviewEnabled ? L"On" : L"Off" },
                { Loc::Tr("settings.system.reduce_motion"), app.settings.reduceMotionEnabled ? L"On" : L"Windows setting" },
                { Loc::Tr("settings.system.smart_series"), app.settings.smartSeriesDetection ? L"On" : L"Off" },
                { Loc::Tr("settings.system.jump_list"), app.settings.jumpListEnabled ? L"On" : L"Off" },
                { Loc::Tr("settings.system.open_data"), L"Open" },
                { Loc::Tr("settings.system.clear_disk_cache"), L"Clear" },
                { Loc::Tr("settings.system.clear_history"), L"Clear" },
                { Loc::Tr("settings.system.reset_defaults"), L"Reset" },
                { Loc::Tr("settings.system.register_types"), L"Register" },
                { Loc::Tr("settings.system.remove_types"), L"Remove" }
            };
            if (simpleSettings) rows.resize(SettingsLayout::kSystemSimpleRowCount);
        }

        const auto scList = (app.currentSettingsCat == SettingsCategory::Shortcuts)
            ? app.settings.GetShortcutActionListRef()
            : std::vector<std::pair<std::string, std::string>>{};
        const size_t midSc = scSingleColumn ? scList.size() : (scList.size() + 1) / 2;

        for (size_t i = 0; i < rows.size(); ++i) {
            RECT bounds{};
            if (app.currentSettingsCat == SettingsCategory::Experience) {
                const float contentLeft = view.left + SettingsLayout::kSidebarWidth * dp + 16.0f * dp + SettingsLayout::kContentPadX * dp;
                const float contentRight = view.right - SettingsLayout::kContentPadX * dp;
                const auto layout = SettingsLayout::MakeExperienceLayout(
                    contentLeft, contentRight, rowTop, dp,
                    FeatureGate::ModeFromString(app.settings.experienceMode) == ExperienceMode::Custom);
                const D2D1_RECT_F target = (i == 0)
                    ? D2D1::RectF(layout.modeTiles[0].left, layout.modeTiles[0].top, layout.modeTiles[2].right, layout.modeTiles[2].bottom)
                    : layout.rows[i - 1];
                bounds = PlayerRect(target);
            } else if (app.currentSettingsCat == SettingsCategory::System) {
                const float sysRowTop = rowTop + SettingsLayout::kSystemActionsTop * dp;
                bounds = ScreenRect(view.left + (SettingsLayout::kSidebarWidth + 16.0f + SettingsLayout::kContentPadX) * dp,
                                    sysRowTop + i * (58.0f + 10.0f) * dp,
                                    view.right - SettingsLayout::kContentPadX * dp,
                                    sysRowTop + i * (58.0f + 10.0f) * dp + 58.0f * dp);
            } else if (app.currentSettingsCat == SettingsCategory::Playback) {
                const float cardLeft = view.left + SettingsLayout::kSidebarWidth * dp + 16.0f * dp + SettingsLayout::kContentPadX * dp - 10.0f * dp;
                const float cardRight = view.right - SettingsLayout::kContentPadX * dp + 10.0f * dp;
                const float contentLeft = cardLeft + 10.0f * dp, contentRight = cardRight - 10.0f * dp;
                const float cardGap = SettingsLayout::kSectionCardGap * dp, titleH = SettingsLayout::kPlaybackCardTitleHeight * dp;
                const float mpvCardTop = rowTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp +
                    2.0f * (SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap) +
                    SettingsLayout::kPlaybackSpeedCardHeight * dp + 2.0f * cardGap;
                if (i < 4) {
                    const float rowTopPx = rowTop + titleH + i * SettingsLayout::kPlaybackBehaviorRowHeight * dp;
                    bounds = ScreenRect(contentLeft, rowTopPx, contentRight, rowTopPx + SettingsLayout::kPlaybackBehaviorRowHeight * dp);
                } else if (i == 4 || i == 5) {
                    const float cardTop = rowTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp +
                        (i == 5 ? SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap : 0.0f) + cardGap;
                    bounds = ScreenRect(cardLeft, cardTop, cardRight, cardTop + SettingsLayout::kPlaybackSimpleCardHeight * dp);
                } else if (i == 6) {
                    const float cardTop = rowTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp +
                        2.0f * (SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap) + cardGap;
                    bounds = ScreenRect(cardLeft, cardTop, cardRight, cardTop + SettingsLayout::kPlaybackSpeedCardHeight * dp);
                } else if (i == 7) {
                    bounds = ScreenRect(cardLeft, mpvCardTop + titleH, cardRight, mpvCardTop + titleH + SettingsLayout::kPlaybackMpvSwitchHeight * dp);
                } else if (i == 8) {
                    const float actionTop = mpvCardTop + SettingsLayout::kPlaybackMpvActionTop * dp;
                    const float actionAreaH = 3.0f * SettingsLayout::kPlaybackMpvActionHeight * dp + 2.0f * SettingsLayout::kPlaybackMpvActionGap * dp;
                    bounds = ScreenRect(cardLeft, actionTop, cardRight, actionTop + actionAreaH);
                } else {
                    const float cardTop = mpvCardTop + SettingsLayout::kPlaybackMpvCardHeight * dp + cardGap +
                        static_cast<float>(i - 9) * (SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap);
                    bounds = ScreenRect(cardLeft, cardTop, cardRight, cardTop + SettingsLayout::kPlaybackSimpleCardHeight * dp);
                }
            } else if (app.currentSettingsCat == SettingsCategory::Subtitles) {
                const float cardGap = SettingsLayout::kSubtitleCardGap * dp;
                const float cardHeights[] = {
                    SettingsLayout::kSubtitleTrackCardHeight * dp, SettingsLayout::kSubtitleTrackCardHeight * dp,
                    SettingsLayout::kSubtitleFontCardHeight * dp, SettingsLayout::kSubtitleColorCardHeight * dp,
                    SettingsLayout::kSubtitleStyleCardHeight * dp, SettingsLayout::kSubtitlePositionCardHeight * dp
                };
                const int row = std::clamp(static_cast<int>(i), 0, 5);
                float cardTop = rowTop + SettingsLayout::kSubtitlePreviewCardHeight * dp + cardGap;
                for (int card = 0; card < row; ++card) cardTop += cardHeights[card] + cardGap;
                bounds = ScreenRect(view.left + SettingsLayout::kSidebarWidth * dp + 16.0f * dp + SettingsLayout::kContentPadX * dp - 10.0f * dp,
                                    cardTop, view.right - SettingsLayout::kContentPadX * dp + 10.0f * dp, cardTop + cardHeights[row]);
            } else if (app.currentSettingsCat == SettingsCategory::Shortcuts && i == 0) {
                bounds = ScreenRect(view.left + 20.0f * dp, rowTop - 2.0f * dp, view.right - 20.0f * dp, rowTop + 24.0f * dp);
            } else {
                const float y0 = rowTop + 28.0f * dp + i * 43.0f * dp;
                bounds = ScreenRect(view.left + 20.0f * dp, y0, view.right - 20.0f * dp, y0 + 38.0f * dp);
            }
            if (!Intersects(bounds)) continue;

            const bool isShortcutItem = app.currentSettingsCat == SettingsCategory::Shortcuts && i > 0;
            bool isFocused = false;
            if (isShortcutItem) {
                const size_t scIdx = i - 1;
                const int expectedRow = (scIdx < midSc) ? static_cast<int>(scIdx + 1) : static_cast<int>(scIdx - midSc + 1);
                const int expectedSub = (scIdx < midSc) ? 0 : 1;
                isFocused = app.activeFocusPane == FocusPane::SettingsContent &&
                            app.keyboardFocusedIndex == expectedRow && app.keyboardFocusedSubIndex == expectedSub;
            } else {
                isFocused = app.activeFocusPane == FocusPane::SettingsContent && app.keyboardFocusedIndex == static_cast<int>(i);
            }

            std::function<void()> rowAction;
            if (isShortcutItem) {
                rowAction = [pApp, i, scSingleColumn]() {
                    pApp->activeFocusPane = FocusPane::SettingsContent;
                    auto list = pApp->settings.GetShortcutActionListRef();
                    const size_t mid = scSingleColumn ? list.size() : (list.size() + 1) / 2;
                    const size_t scIdx = i - 1;
                    if (scIdx < list.size()) {
                        const int expectedRow = (scIdx < mid) ? static_cast<int>(scIdx + 1) : static_cast<int>(scIdx - mid + 1);
                        const int expectedSub = (scIdx < mid) ? 0 : 1;
                        pApp->StartShortcutCapture(list[scIdx].first, list[scIdx].second, expectedRow, expectedSub);
                    }
                };
            } else {
                rowAction = [pApp, i]() {
                    pApp->activeFocusPane = FocusPane::SettingsContent;
                    pApp->keyboardFocusedIndex = static_cast<int>(i);
                    pApp->HandleGlobalKeyDown(VK_RETURN, 0);
                };
            }

            const LONG catIdx = static_cast<LONG>(app.currentSettingsCat);
            auto& n = AddMetaNode(
                600 + catIdx * 100 + static_cast<LONG>(i), rows[i].first,
                isShortcutItem ? (L"Shortcut for " + rows[i].first) : L"Settings option",
                rows[i].second, ROLE_SYSTEM_LISTITEM, StateBits(false, isFocused, false, false, false),
                bounds, isShortcutItem ? L"Rebind" : L"Activate", isShortcutItem ? rows[i].second : L"Enter",
                std::move(rowAction), L"", L"ApertureSettingsRow", UIA_ListItemControlTypeId,
                isShortcutItem ? L"Shortcut Setting" : L"Setting Option");
            n.automationId = L"SettingsRow_" + std::to_wstring(n.id);
            if (isShortcutItem) {
                n.accessKey = rows[i].second;
                n.keyboardShortcut = rows[i].second;
                n.helpText = L"Press Enter to rebind " + rows[i].first +
                    (rows[i].second.empty() ? L" (currently Unassigned)" : (L" (currently " + rows[i].second + L")"));
            } else if (app.currentSettingsCat == SettingsCategory::System && i == 3) {
                n.helpText = Loc::Tr("settings.system.smart_series_on");
            }
            n.selectionContainerId = 500 + catIdx;
            n.controlledPeerIds = { 500 + catIdx };
            n.positionInSet = static_cast<int>(i + 1);
            n.sizeOfSet = static_cast<int>(rows.size());
            n.flowsFromIds = (i == 0) ? std::vector<LONG>{ 500 + catIdx } : std::vector<LONG>{ 600 + catIdx * 100 + static_cast<LONG>(i - 1) };
            if (i + 1 < rows.size()) n.flowsToIds = { 600 + catIdx * 100 + static_cast<LONG>(i + 1) };

            if (app.currentSettingsCat == SettingsCategory::Playback &&
                (i == 0 || i == 1 || i == 2 || i == 3 || i == 7 || i == 9 || i == 10 || i == 11)) {
                const bool checked = (i == 0 ? app.settings.autoplay
                    : (i == 1 ? app.settings.rememberPos
                    : (i == 2 ? app.settings.hwAccel
                    : (i == 3 ? app.settings.loop
                    : (i == 7 ? app.settings.customMpvConfigEnabled
                    : (i == 9 ? app.settings.globalHotkeysEnabled
                    : (i == 10 ? app.settings.trayIconEnabled : app.settings.minimizeToTray)))))));
                EnableToggle(n, checked);
            } else if (app.currentSettingsCat == SettingsCategory::System && (i == 1 || i == 2 || i == 3 || i == 4)) {
                const bool checked = (i == 1 ? app.settings.hoverPreviewEnabled
                    : (i == 2 ? app.settings.reduceMotionEnabled
                    : (i == 3 ? app.settings.smartSeriesDetection : app.settings.jumpListEnabled)));
                EnableToggle(n, checked);
            }
        }
    }
};

void App::RefreshAccessibilityTree(bool force) {
    if (!hWnd || shutdownComplete) return;
    if (!force && !accessibilityClientActive) return;
    const ULONGLONG now = GetTickCount64();
    if (playbackSession.IsOpen()) {
        const double curTime = player.GetCurrentTime();
        const long long curSec = static_cast<long long>(std::floor(std::max(0.0, curTime)));
        const double durSec = std::floor(std::max(0.0, player.GetDuration()));
        const int volPct = static_cast<int>(std::lround(std::clamp(settings.volume, 0.0, 1.0) * 100.0));
        const std::wstring curSub = subtitles.GetActiveTrack()
            ? subtitles.GetCurrentSubtitleText(curTime, settings.subDelayMs)
            : std::wstring{};
        const bool playbackChanged =
            (curSec != accessibilityLastPlaybackSec) ||
            (durSec != accessibilityLastDurationSec) ||
            (settings.muted != accessibilityLastMuted) ||
            (volPct != accessibilityLastVolumePct) ||
            (curSub != accessibilityLastSubtitleText);
        if (!force && !accessibilityDirty && ((now - accessibilityLastRefreshMs < 250) || !playbackChanged)) {
            return;
        }
        accessibilityLastPlaybackSec = curSec;
        accessibilityLastDurationSec = durSec;
        accessibilityLastMuted = settings.muted;
        accessibilityLastVolumePct = volPct;
        accessibilityLastSubtitleText = curSub;
    } else {
        if (!force && !accessibilityDirty) return;
        accessibilityLastPlaybackSec = -1;
        accessibilityLastDurationSec = -1.0;
        accessibilityLastVolumePct = -1;
        accessibilityLastSubtitleText.clear();
    }
    accessibilityLastRefreshMs = now;
    accessibilityDirty = false;
    accessibilityActions.clear();
    RECT client{};
    if (!GetClientRect(hWnd, &client)) return;
    POINT screenOrigin{ 0, 0 };
    ClientToScreen(hWnd, &screenOrigin);

    AccessibilityTreeBuilder builder(*this, client, screenOrigin);
    if (activeModal != ModalType::None) { builder.AppendModal(); return; }
    if (!rebindingActionId.empty()) { builder.AppendShortcutCapture(); return; }
    if (globalSearchModel.IsOpen()) { builder.AppendGlobalSearch(); return; }
    if (isContextMenuOpen) { builder.AppendContextMenu(); return; }
    if (playbackSession.IsOpen()) { builder.AppendPlayerOverlay(); return; }

    builder.AppendHubNav();
    const D2D1_RECT_F view = D2D1::RectF(builder.padX, builder.viewTop, builder.winW - builder.padX, builder.winH - builder.padY);
    if (currentTab == TabView::Media) builder.AppendMediaTab(view);
    else if (currentTab == TabView::Playlists) builder.AppendPlaylistsTab(view);
    else if (currentTab == TabView::History) builder.AppendHistoryTab(view);
    else builder.AppendSettingsTab(view);
    builder.Commit();
}

} // namespace Aperture
