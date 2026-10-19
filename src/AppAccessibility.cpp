#include "App.h"
#include "AppLayout.h"
#include "SettingsLayout.h"
#include "Presentation.h"
#include "Localization.h"

namespace Aperture {

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

void App::RefreshAccessibilityTree(bool force) {
    if (!hWnd || shutdownComplete) return;
    const ULONGLONG now = GetTickCount64();
    if (!force && !accessibilityDirty && (now - accessibilityLastRefreshMs < 125)) return;
    accessibilityLastRefreshMs = now;
    accessibilityDirty = false;
    accessibilityActions.clear();
    RECT client{};
    if (!GetClientRect(hWnd, &client)) return;
    POINT screenOrigin{ 0, 0 };
    ClientToScreen(hWnd, &screenOrigin);
    const float dp = std::max(0.5f, playbackSession.IsOpen() ? overlayRenderer.GetDpiScale() : renderer.GetDpiScale());
    const float winW = static_cast<float>(std::max<LONG>(1, client.right - client.left));
    const float winH = static_cast<float>(std::max<LONG>(1, client.bottom - client.top));
    const float padX = 18.0f * dp, padY = 14.0f * dp, viewTop = 84.0f * dp;
    std::vector<AccessibilityNode> nodes;
    nodes.reserve(64);
    std::unordered_set<LONG> usedIds;
    auto screenRect = [&](float left, float top, float right, float bottom) {
        RECT result{screenOrigin.x + static_cast<LONG>(std::lround(left)), screenOrigin.y + static_cast<LONG>(std::lround(top)),
                    screenOrigin.x + static_cast<LONG>(std::lround(right)), screenOrigin.y + static_cast<LONG>(std::lround(bottom))};
        return result;
    };
    auto intersects = [&](const RECT& rect) {
        RECT viewport = { screenOrigin.x, screenOrigin.y + static_cast<LONG>(std::lround(viewTop)),
            screenOrigin.x + static_cast<LONG>(std::lround(winW)), screenOrigin.y + static_cast<LONG>(std::lround(winH)) };
        RECT overlap{};
        return IntersectRect(&overlap, &rect, &viewport) != FALSE;
    };
    auto stateBits = [&](bool selected, bool focused, bool disabled, bool offscreen, bool checked) {
        LONG state = disabled ? STATE_SYSTEM_UNAVAILABLE : STATE_SYSTEM_FOCUSABLE;
        if (selected) state |= STATE_SYSTEM_SELECTED;
        if (focused) state |= STATE_SYSTEM_FOCUSED;
        if (offscreen) state |= STATE_SYSTEM_OFFSCREEN;
        if (checked) state |= STATE_SYSTEM_CHECKED;
        return state;
    };
    auto shortcut = [&](const std::string& actionId) {
        const auto it = settings.shortcuts.find(actionId);
        return it == settings.shortcuts.end() ? std::wstring() : Settings::FormatShortcutKey(it->second);
    };
    auto addNode = [&](LONG preferredId, const std::wstring &name, const std::wstring &description, const std::wstring &value, LONG role,
                       LONG state, const RECT &bounds, const std::wstring &actionName, const std::wstring &key,
                       const std::function<void()> &action, const std::function<void(AccessibilityNode &)> &config = nullptr) {
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
            accessibilityActions[id] = action;
            node.onDefaultAction = [this, id](LONG) { QueueAccessibilityAction(id); };
            if (role == ROLE_SYSTEM_PUSHBUTTON || role == ROLE_SYSTEM_MENUITEM) {
                node.hasInvoke = true;
                node.onInvoke = [this, id](LONG) { QueueAccessibilityAction(id); };
            }
        }
        if (config) { config(node); }
        nodes.push_back(std::move(node));
        return id;
    };
    auto addStatusLiveRegion = [&]() {
        addNode(kStatusLiveRegionId, currentLiveAnnouncement.empty() ? L"Ready" : currentLiveAnnouncement,
            L"Status announcements and notifications", L"", ROLE_SYSTEM_STATUSBAR, STATE_SYSTEM_READONLY,
            screenRect(0.0f, winH - 24.0f * dp, winW, winH), L"", L"", nullptr, [&](AccessibilityNode& n) {
                n.automationId = L"AppStatusLiveRegion";
                n.className = L"ApertureStatusLiveRegion";
                n.controlTypeId = UIA_StatusBarControlTypeId;
                n.itemType = L"Status Region";
                n.ariaRole = L"status";
                n.liveSetting = Polite;
            });
    };
    // --- 1. MODAL STATE: Trap accessibility tree exclusively in modal ---
    if (activeModal != ModalType::None) {
        bool isInputModal = (activeModal == ModalType::NewPlaylist || activeModal == ModalType::NewSmartPlaylist ||
            activeModal == ModalType::EditSmartPlaylist || activeModal == ModalType::SeekStepInput ||
            activeModal == ModalType::FrameSeekInput || activeModal == ModalType::CustomSettingInput ||
            activeModal == ModalType::BookmarkInput || activeModal == ModalType::EditMetadataOverrides ||
            activeModal == ModalType::SaveSubtitleProfile || activeModal == ModalType::RenameSubtitleProfile);
        const float maxBoxW = isInputModal ? 460.0f * dp : 500.0f * dp;
        const float boxW = std::min(maxBoxW, std::max(320.0f * dp, winW - 48.0f * dp));
        const float boxH = (isInputModal ? 204.0f : Presentation::ConfirmationHeight) * dp;
        const float left = (winW - boxW) * 0.5f, top = (winH - boxH) * 0.5f;
        const float btnH = 36.0f * dp, btnY = top + boxH - 20.0f * dp - btnH;
        const float confBtnW = isInputModal ? 92.0f * dp : std::max(110.0f * dp, renderer.MeasureString(modalConfirmText, FontType::Medium).width + 32.0f * dp);
        const float cancelBtnW = isInputModal ? 92.0f * dp : 100.0f * dp, confLeft = left + boxW - 24.0f * dp - confBtnW;
        const float cancelLeft = confLeft - 10.0f * dp - cancelBtnW;
        const std::wstring modalHeading = !modalTitle.empty() ? modalTitle : (!modalPrompt.empty() ? modalPrompt : L"Dialog");
        const std::wstring modalExplanation = !modalMessage.empty() ? modalMessage : (!modalPrompt.empty() ? modalPrompt : L"Modal dialog");
        addNode(8050, modalHeading, modalExplanation, L"", ROLE_SYSTEM_GROUPING, stateBits(false, false, false, false, false),
                screenRect(left, top, left + boxW, top + boxH), L"", L"", nullptr, [&](AccessibilityNode &n) {
                n.automationId = L"ModalDialog";
                n.className = L"ApertureDialog";
                n.controlTypeId = UIA_GroupControlTypeId;
                n.itemType = L"Modal Dialog";
                n.ariaRole = L"dialog";
                n.helpText = modalExplanation;
                n.controllerForIds = isInputModal ? std::vector<LONG>{ 8000, 8001, 8002 } : std::vector<LONG>{ 8001, 8002 };
                n.flowsToIds = isInputModal ? std::vector<LONG>{ 8000 } : std::vector<LONG>{ 8001 };
            });
        if (isInputModal) {
            addNode(8000, L"Modal input", modalPrompt, Text16ToWideString(modalInput.GetText()), ROLE_SYSTEM_TEXT,
                stateBits(false, modalKeyboardFocus == 0, false, false, false), screenRect(left + 24.0f * dp, top + 64.0f * dp, left + boxW - 24.0f * dp, top + 106.0f * dp),
                L"Edit", L"", [this]() { modalKeyboardFocus = 0; FocusTextInput(TextInputField::Modal); }, [&](AccessibilityNode& n) {
                    n.automationId = L"ModalInput";
                    n.className = L"ApertureTextBox";
                    n.controlTypeId = UIA_EditControlTypeId;
                    n.itemType = L"Modal Input";
                    n.labeledById = 8050;
                    n.describedByIds = { 8050 };
                    n.groupId = 8050;
                    n.flowsFromIds = { 8050 };
                    n.flowsToIds = { 8001 };
                    n.hasValue = true;
                    n.isReadOnly = false;
                    n.onSetValue = [this](LONG, const std::wstring& val) {
                        modalInput.SetText(WideStringToText16(val));
                        MarkAccessibilityDirty();
                        InvalidateRect(hWnd, NULL, FALSE);
                    };
                });
        }
        addNode(8001, (activeModal == ModalType::DeletePlaylist || activeModal == ModalType::Confirmation) ? modalConfirmText : L"OK", modalExplanation, L"", ROLE_SYSTEM_PUSHBUTTON,
            stateBits(false, isModalOkHovered || modalKeyboardFocus == (isInputModal ? 1 : 0), false, false, false), screenRect(confLeft, btnY, confLeft + confBtnW, btnY + btnH), L"Confirm", L"Enter",
            [this]() { DismissActiveModal(true); }, [&](AccessibilityNode& n) {
                n.automationId = L"ModalConfirmButton";
                n.className = L"ApertureButton";
                n.controlTypeId = UIA_ButtonControlTypeId;
                n.itemType = L"Modal Action";
                n.helpText = modalExplanation;
                n.describedByIds = { 8050 };
                n.groupId = 8050;
                n.flowsFromIds = isInputModal ? std::vector<LONG>{ 8000 } : std::vector<LONG>{ 8050 };
                n.flowsToIds = { 8002 };
            });
        addNode(8002, L"Cancel", L"Close dialog without applying changes", L"", ROLE_SYSTEM_PUSHBUTTON,
            stateBits(false, isModalCancelHovered || modalKeyboardFocus == (isInputModal ? 2 : 1), false, false, false), screenRect(cancelLeft, btnY, cancelLeft + cancelBtnW, btnY + btnH), L"Cancel", L"Escape",
            [this]() { DismissActiveModal(false); }, [&](AccessibilityNode& n) {
                n.automationId = L"ModalCancelButton";
                n.className = L"ApertureButton";
                n.controlTypeId = UIA_ButtonControlTypeId;
                n.itemType = L"Modal Action";
                n.describedByIds = { 8050 };
                n.groupId = 8050;
                n.flowsFromIds = { 8001 };
            });
        addStatusLiveRegion();
        accessibilityTree.ReplaceNodes(nodes);
        return;
    }
    // --- 2. SHORTCUT CAPTURE SURFACE: Trap accessibility tree exclusively during shortcut rebinding ---
    if (!rebindingActionId.empty()) {
        const float cardW = std::min(520.0f * dp, winW - 48.0f * dp);
        const float cardH = 180.0f * dp;
        const float cardLeft = (winW - cardW) * 0.5f;
        const float cardTop = (winH - cardH) * 0.5f;
        const float btnH = 36.0f * dp, btnW = 100.0f * dp, cancelLeft = cardLeft + cardW - 24.0f * dp - btnW;
        const float btnY = cardTop + cardH - 20.0f * dp - btnH;
        const std::wstring actionDisplayName = Utf8ToUtf16(rebindingActionName.empty() ? rebindingActionId : rebindingActionName);
        const std::wstring currentChord = GetCurrentCaptureChordText();
        addNode(8150, L"Rebind Shortcut: " + actionDisplayName,
            L"Shortcut capture dialog. Press desired key combination with optional Ctrl, Shift, or Alt to rebind " + actionDisplayName + L". Press Escape to cancel.",
            L"", ROLE_SYSTEM_GROUPING, stateBits(false, false, false, false, false),
            screenRect(cardLeft, cardTop, cardLeft + cardW, cardTop + cardH), L"", L"", nullptr, [&](AccessibilityNode& n) {
                n.automationId = L"ShortcutCaptureDialog";
                n.className = L"ApertureShortcutCaptureDialog";
                n.controlTypeId = UIA_GroupControlTypeId;
                n.itemType = L"Shortcut Capture Dialog";
                n.ariaRole = L"dialog";
                n.helpText = L"Press desired key combination with optional Ctrl, Shift, or Alt modifiers. Press Escape to cancel.";
                n.controllerForIds = { 8100, 8101 };
                n.flowsToIds = { 8100 };
            });
        addNode(8100, L"Shortcut for " + actionDisplayName, L"Listening for key combination (supports Ctrl, Shift, and Alt modifiers)",
            currentChord, ROLE_SYSTEM_TEXT, stateBits(false, true, false, false, false),
            screenRect(cardLeft + 24.0f * dp, cardTop + 54.0f * dp, cardLeft + cardW - 24.0f * dp, cardTop + 96.0f * dp),
            L"Rebind", L"", [this]() {}, [&](AccessibilityNode& n) {
                n.automationId = L"ShortcutCaptureInput";
                n.className = L"ApertureShortcutCaptureBox";
                n.controlTypeId = UIA_EditControlTypeId;
                n.itemType = L"Shortcut Capture Input";
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
            });
        addNode(8101, L"Cancel", L"Cancel shortcut capture without changes", L"", ROLE_SYSTEM_PUSHBUTTON,
            stateBits(false, false, false, false, false), screenRect(cancelLeft, btnY, cancelLeft + btnW, btnY + btnH),
            L"Cancel", L"Escape", [this]() { CancelShortcutCapture(); }, [&](AccessibilityNode& n) {
                n.automationId = L"ShortcutCaptureCancelButton";
                n.className = L"ApertureButton";
                n.controlTypeId = UIA_ButtonControlTypeId;
                n.itemType = L"Modal Action";
                n.describedByIds = { 8150 };
                n.groupId = 8150;
                n.flowsFromIds = { 8100 };
                n.hasInvoke = true;
                n.onInvoke = [this](LONG) { CancelShortcutCapture(); };
            });
        addStatusLiveRegion();
        accessibilityTree.ReplaceNodes(nodes);
        return;
    }
    // --- 2. GLOBAL SEARCH SURFACE ---
    if (globalSearchModel.IsOpen()) {
        auto proj = globalSearchModel.GetProjection();
        const float cardW = std::min(440.0f * dp, winW - 36.0f * dp);
        const float cardH = std::min(390.0f * dp, winH - 106.0f * dp);
        const float cardLeft = 18.0f * dp, cardTop = 76.0f * dp;
        const D2D1_RECT_F closeRect = D2D1::RectF(cardLeft + cardW - 42.0f * dp, cardTop + 10.0f * dp,
            cardLeft + cardW - 10.0f * dp, cardTop + 42.0f * dp);
        const D2D1_RECT_F inputRect = D2D1::RectF(cardLeft + 12.0f * dp, cardTop + 10.0f * dp,
            closeRect.left - 8.0f * dp, cardTop + 42.0f * dp);
        addNode(9000, L"Global Search", L"Search media library and playback history", proj.query, ROLE_SYSTEM_TEXT,
            stateBits(false, activeTextInputField == TextInputField::GlobalSearch, false, false, false),
            screenRect(inputRect.left, inputRect.top, inputRect.right, inputRect.bottom),
            L"Edit", L"", [this]() { FocusTextInput(TextInputField::GlobalSearch); }, [&](AccessibilityNode& n) {
                n.automationId = L"GlobalSearchInput";
                n.className = L"ApertureTextBox";
                n.controlTypeId = UIA_EditControlTypeId;
                n.itemType = L"Search Input";
                n.ariaRole = L"searchbox";
                std::vector<LONG> resultIds;
                resultIds.reserve(proj.results.size());
                for (size_t r = 0; r < proj.results.size(); ++r) {
                    resultIds.push_back(AccessibilityStableId(9200 + static_cast<LONG>(r), proj.results[r].filePath));
                }
                n.controllerForIds = resultIds;
                if (!resultIds.empty()) { n.flowsToIds = {resultIds.front()}; }
                n.hasValue = true;
                n.isReadOnly = false;
                n.onSetValue = [this](LONG, const std::wstring& val) {
                    globalSearchModel.SetQuery(val, library, true);
                    MarkAccessibilityDirty();
                    InvalidateRect(hWnd, NULL, FALSE);
                };
            });
        addNode(9001, L"Close Search", L"Close global search", L"", ROLE_SYSTEM_PUSHBUTTON,
            stateBits(isGlobalSearchCloseHovered, false, false, false, false),
            screenRect(closeRect.left, closeRect.top, closeRect.right, closeRect.bottom),
            L"Close", L"Esc", [this]() { CloseGlobalSearch(); }, [](AccessibilityNode& n) {
                n.automationId = L"GlobalSearchClose";
                n.className = L"ApertureButton";
                n.controlTypeId = UIA_ButtonControlTypeId;
                n.itemType = L"Search Action";
            });
        float itemY = cardTop + 52.0f * dp;
        for (size_t i = 0; i < proj.results.size(); ++i) {
            const auto& srcItem = proj.results[i];
            const bool isSel = (static_cast<int>(i) == proj.selectedIndex);
            const bool isFoc = (activeTextInputField != TextInputField::GlobalSearch && isSel);
            const std::wstring itemTitle = srcItem.title.empty() ? srcItem.fileName : srcItem.title;
            const std::wstring itemDesc = srcItem.GetSourceDisplayName() + L" \x2022 " + srcItem.GetFormattedDuration();
            const RECT itemBounds = screenRect(cardLeft + 8.0f * dp, itemY, cardLeft + cardW - 8.0f * dp, itemY + 44.0f * dp);
            addNode(AccessibilityStableId(9200 + static_cast<LONG>(i), srcItem.filePath), itemTitle,
                itemDesc, srcItem.filePath, ROLE_SYSTEM_LISTITEM, stateBits(isSel, isFoc, !srcItem.existsOnDisk, false, isSel), itemBounds,
                L"Play", L"Enter", [this, srcItem]() {
                    auto action = globalSearchModel.ActivateSelection(library);
                    ExecuteGlobalSearchAction(action);
                }, [&](AccessibilityNode& n) {
                    n.automationId = L"GlobalSearchItem_" + std::to_wstring(i);
                    n.className = L"ApertureListItem";
                    n.controlTypeId = UIA_ListItemControlTypeId;
                    n.itemType = L"Search Result";
                    n.itemStatus = srcItem.GetSourceDisplayName();
                    n.controlledPeerIds = { 9000 };
                    n.selectionContainerId = 9000;
                    n.positionInSet = static_cast<int>(i + 1);
                    n.sizeOfSet = static_cast<int>(proj.results.size());
                    n.flowsFromIds = (i == 0) ? std::vector<LONG>{ 9000 } : std::vector<LONG>{ AccessibilityStableId(9200 + static_cast<LONG>(i - 1), proj.results[i - 1].filePath) };
                    if (i + 1 < proj.results.size()) {
                        n.flowsToIds = { AccessibilityStableId(9200 + static_cast<LONG>(i + 1), proj.results[i + 1].filePath) };
                    }
                    n.hasSelectionItem = true;
                    n.selected = isSel;
                    n.onSelect = [this, i](LONG) {
                        globalSearchModel.SetSelectedIndex(static_cast<int>(i));
                        InvalidateRect(hWnd, NULL, FALSE);
                    };
                    n.hasScrollItem = true;
                    n.onScrollIntoView = [this, i](LONG) {
                        globalSearchModel.SetSelectedIndex(static_cast<int>(i));
                        InvalidateRect(hWnd, NULL, FALSE);
                        MarkAccessibilityDirty();
                    };
                });
            itemY += 46.0f * dp;
        }
        addStatusLiveRegion();
        accessibilityTree.ReplaceNodes(nodes);
        return;
    }
    // --- 3. CONTEXT MENU & SHORTCUT OVERLAY POPUP ---
    if (isContextMenuOpen) {
        int totalMenuItems = 0;
        for (const auto& itm : contextMenuItems) { if (!itm.isSeparator) ++totalMenuItems; }
        float y = contextMenuOrigin.y + 8.0f * dp;
        int menuPos = 0;
        for (size_t i = 0; i < contextMenuItems.size(); ++i) {
            const auto& item = contextMenuItems[i];
            if (item.isSeparator) { y += 6.0f * dp; continue; }
            const LONG itemId = 7000 + static_cast<LONG>(i);
            const RECT bounds = screenRect(contextMenuOrigin.x, y, contextMenuOrigin.x + 220.0f * dp, y + 32.0f * dp);
            const int curPos = menuPos++;
            addNode(itemId, item.label, item.shortcut.empty() ? L"Context menu action" : item.shortcut,
                item.isChecked ? L"Checked" : L"", ROLE_SYSTEM_MENUITEM,
                stateBits(item.isChecked, hoveredContextMenuItem == static_cast<int>(i), item.isDisabled, false, item.isChecked),
                bounds, item.isDisabled ? L"" : L"Activate", item.shortcut,
                (!item.isDisabled && item.onClick) ? std::function<void()>{ [this, i]() {
                    if (i < contextMenuItems.size() && !contextMenuItems[i].isDisabled && contextMenuItems[i].onClick) {
                        auto cb = contextMenuItems[i].onClick;
                        CloseContextMenu();
                        if (cb) cb();
                    }
                } } : std::function<void()>{}, [&](AccessibilityNode& n) {
                    n.automationId = item.id.empty() ? (L"ContextMenuItem_" + std::to_wstring(n.id)) : (L"ContextMenuItem_" + Utf8ToUtf16(item.id));
                    n.className = L"ApertureMenuItem";
                    n.controlTypeId = UIA_MenuItemControlTypeId;
                    n.itemType = L"Menu Item";
                    n.positionInSet = curPos + 1;
                    n.sizeOfSet = totalMenuItems;
                    if (i > 0) { n.flowsFromIds = {static_cast<LONG>(7000 + i - 1)}; }
                    if (i + 1 < contextMenuItems.size()) { n.flowsToIds = {static_cast<LONG>(7000 + i + 1)}; }
                    if (item.isChecked) {
                        n.hasToggle = true;
                        n.toggleState = ToggleState_On;
                        n.onToggle = [this, i](LONG, ToggleState) {
                            if (i < contextMenuItems.size() && !contextMenuItems[i].isDisabled && contextMenuItems[i].onClick) {
                                auto cb = contextMenuItems[i].onClick;
                                CloseContextMenu();
                                if (cb) cb();
                            }
                        };
                    }
                });
            y += 32.0f * dp;
        }
        addStatusLiveRegion();
        accessibilityTree.ReplaceNodes(nodes);
        return;
    }
    // --- 4. PLAYER OVERLAY ---
    if (playbackSession.IsOpen()) {
        const float playerPadX = Presentation::PlayerPadding * dp;
        const bool queuePlayback = playbackSession.IsQueuePlayback();
        const bool queuePrevDisabled = queuePlayback && playbackSession.GetQueueIndex() == 0 && !settings.loop;
        const auto playerLayout = Presentation::Player(winW, winH, dp, queuePlayback);
        const auto playerRect = [&](const D2D1_RECT_F& r) { return screenRect(r.left, r.top, r.right, r.bottom); };
        const float topBarH = 64.0f * dp, bottomBarH = 76.0f * dp, btn = 36.0f * dp;
        const bool hidden = controlsOpacity <= 0.02f && !isSettingsFlyoutOpen;
        const LONG playerState = stateBits(false, false, false, hidden, false);
        const auto playerControlState = [&](LONG id, bool disabled = false) {
            return stateBits(false, playerKeyboardFocusId == id, disabled, hidden, false);
        };
        if (playbackSession.IsPiP()) {
            const auto pip = Presentation::PiP(winW, winH, dp);
            const bool pipPrevDisabled = !playbackSession.IsQueuePlayback() || (playbackSession.GetQueueIndex() == 0 && !settings.loop);
            const bool pipNextDisabled = !playbackSession.IsQueuePlayback() || playbackSession.GetQueueSize() <= 1;
            addNode(9020, player.IsPlaying() ? L"Pause" : L"Play", L"Play or pause PiP media", player.IsPlaying() ? L"Playing" : L"Paused",
                ROLE_SYSTEM_PUSHBUTTON, playerControlState(9020), playerRect(pip.playPause),
                player.IsPlaying() ? L"Pause" : L"Play", shortcut("togglePlay"), [this]() { commandDispatcher.Execute(AppCommand::TogglePlay); },
                [&](AccessibilityNode& n) {
                    n.automationId = L"PiPPlayPauseButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.controllerForIds = { 9023, 9024, 9025, 9026, 9021, 9022 };
                    n.flowsToIds = { 9023 };
                    n.hasToggle = true;
                    n.toggleState = player.IsPlaying() ? ToggleState_On : ToggleState_Off;
                    n.onToggle = [this](LONG, ToggleState) { commandDispatcher.Execute(AppCommand::TogglePlay); };
                    n.itemStatus = player.IsPlaying() ? L"Playing" : L"Paused";
                });
            std::function<void()> pipPreviousAction;
            if (!pipPrevDisabled) pipPreviousAction = [this]() { commandDispatcher.Execute(AppCommand::PlayPrevious); };
            addNode(9023, L"Previous", L"Play the previous queued item", pipPrevDisabled ? L"Unavailable" : L"",
                ROLE_SYSTEM_PUSHBUTTON, playerControlState(9023, pipPrevDisabled), playerRect(pip.previous),
                L"Play", shortcut("playPrevious"), pipPreviousAction, [](AccessibilityNode& n) {
                    n.automationId = L"PiPPreviousButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.flowsFromIds = { 9020 };
                    n.flowsToIds = { 9024 };
                });
            std::function<void()> pipNextAction;
            if (!pipNextDisabled) pipNextAction = [this]() { commandDispatcher.Execute(AppCommand::PlayNext); };
            addNode(9024, L"Next", L"Play the next queued item", pipNextDisabled ? L"Unavailable" : L"",
                ROLE_SYSTEM_PUSHBUTTON, playerControlState(9024, pipNextDisabled), playerRect(pip.next),
                L"Play", shortcut("playNext"), pipNextAction, [](AccessibilityNode& n) {
                    n.automationId = L"PiPNextButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.flowsFromIds = { 9023 };
                    n.flowsToIds = { 9025 };
                });
            addNode(9025, L"Volume", L"Toggle volume control", L"", ROLE_SYSTEM_PUSHBUTTON,
                playerControlState(9025), playerRect(pip.audio), L"Toggle", L"", [this]() {
                    isPiPVolumeSliderOpen = !isPiPVolumeSliderOpen;
                    if (isPiPVolumeSliderOpen) {
                        pipVolumeAnim.SnapTo(0.0f);
                        AnimateVisual(pipVolumeAnim, 1.0f, MotionTokens::kToggle, MotionTokens::Spring());
                    } else {
                        AnimateVisual(pipVolumeAnim, 0.0f, MotionTokens::kToggle, MotionTokens::Spring());
                    }
                    MarkAccessibilityDirty();
                    Render();
                }, [](AccessibilityNode& n) {
                    n.automationId = L"PiPVolumeButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.flowsFromIds = { 9024 };
                    n.flowsToIds = { 9026 };
                });
            addNode(9026, playbackSession.IsPiPPinned() ? L"Unpin Picture-in-Picture" : L"Pin Picture-in-Picture",
                L"Toggle picture-in-picture always-on-top", playbackSession.IsPiPPinned() ? L"Pinned" : L"Not pinned",
                ROLE_SYSTEM_PUSHBUTTON, playerControlState(9026), playerRect(pip.pin), L"Toggle", L"", [this]() {
                    playbackSession.TogglePiPPinned(hWnd);
                    SyncOverlayPosition();
                    MarkAccessibilityDirty();
                    Render();
                }, [&](AccessibilityNode& n) {
                    n.automationId = L"PiPPinButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.flowsFromIds = { 9025 };
                    n.flowsToIds = { 9021 };
                    n.hasToggle = true;
                    n.toggleState = playbackSession.IsPiPPinned() ? ToggleState_On : ToggleState_Off;
                    n.onToggle = [this](LONG, ToggleState) {
                        playbackSession.TogglePiPPinned(hWnd);
                        SyncOverlayPosition();
                        MarkAccessibilityDirty();
                        Render();
                    };
                    n.itemStatus = playbackSession.IsPiPPinned() ? L"Pinned" : L"Not pinned";
                });
            addNode(9021, L"Restore Window", L"Exit picture-in-picture mode", L"", ROLE_SYSTEM_PUSHBUTTON,
                playerControlState(9021), playerRect(pip.restore),
                L"Restore", shortcut("togglePiP"), [this]() { commandDispatcher.Execute(AppCommand::TogglePiP); },
                [](AccessibilityNode& n) {
                    n.automationId = L"PiPRestoreButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.flowsFromIds = { 9026 };
                    n.flowsToIds = { 9022 };
                });
            addNode(9022, L"Close Player", L"Close player session", L"", ROLE_SYSTEM_PUSHBUTTON,
                playerControlState(9022), playerRect(pip.close),
                L"Close", shortcut("closePlayer"), [this]() { SendMessage(hWnd, WM_CLOSE, 0, 0); },
                [](AccessibilityNode& n) {
                    n.automationId = L"PiPCloseButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.flowsFromIds = { 9021 };
                });
        } else {
            addNode(9001, L"Back", L"Return to the media library", L"", ROLE_SYSTEM_PUSHBUTTON,
                playerControlState(9001), playerRect(playerLayout.back),
                L"Back", shortcut("closePlayer"), [this]() { commandDispatcher.Execute(AppCommand::ClosePlayer); },
                [&](AccessibilityNode& n) {
                    n.automationId = L"BackButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Navigation";
                    n.flowsToIds = { (queuePlayback && !queuePrevDisabled) ? 9010L : 9002L };
                });
            if (queuePlayback && playbackSession.GetQueueSize() > 0) {
                std::function<void()> previousAction;
                if (!queuePrevDisabled) previousAction = [this]() { commandDispatcher.Execute(AppCommand::PlayPrevious); };
                addNode(9010, L"Previous in queue", L"Play the previous queued item", queuePrevDisabled ? L"Unavailable at start of queue" : L"",
                    ROLE_SYSTEM_PUSHBUTTON, playerControlState(9010, queuePrevDisabled), playerRect(playerLayout.prev),
                    L"Play", shortcut("playPrevious"), previousAction, [&](AccessibilityNode& n) {
                        n.automationId = L"PreviousButton";
                        n.className = L"ApertureButton";
                        n.controlTypeId = UIA_ButtonControlTypeId;
                        n.itemType = L"Player Control";
                        n.flowsFromIds = { 9001 };
                        n.flowsToIds = { 9002 };
                    });
            }
            addNode(9002, player.IsPlaying() ? L"Pause" : L"Play", L"Play or pause the current media", player.IsPlaying() ? L"Playing" : L"Paused",
                ROLE_SYSTEM_PUSHBUTTON, playerControlState(9002), playerRect(playerLayout.play),
                player.IsPlaying() ? L"Pause" : L"Play", shortcut("togglePlay"), [this]() { commandDispatcher.Execute(AppCommand::TogglePlay); },
                [&](AccessibilityNode& n) {
                    n.automationId = L"PlayPauseButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.controllerForIds = { 9003, 9011 };
                    n.flowsFromIds = { (queuePlayback && !queuePrevDisabled) ? 9010L : 9001L };
                    n.flowsToIds = { 9011 };
                    n.hasToggle = true;
                    n.toggleState = player.IsPlaying() ? ToggleState_On : ToggleState_Off;
                    n.onToggle = [this](LONG, ToggleState) { commandDispatcher.Execute(AppCommand::TogglePlay); };
                    n.itemStatus = player.IsPlaying() ? L"Playing" : L"Paused";
                });
            addNode(9011, Presentation::NextLabel(playbackSession.IsQueuePlayback(), playbackSession.GetQueueIndex(), playbackSession.GetQueueSize()), L"Play the next library item or queued item", L"", ROLE_SYSTEM_PUSHBUTTON, playerControlState(9011),
                playerRect(playerLayout.next), L"Play", shortcut("playNext"), [this]() { commandDispatcher.Execute(AppCommand::PlayNext); },
                [](AccessibilityNode& n) {
                    n.automationId = L"NextButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.controlledPeerIds = { 9002 };
                    n.flowsFromIds = { 9002 };
                    n.flowsToIds = { 9003 };
                });
            const double curTime = std::max(0.0, player.GetCurrentTime());
            const double dur = player.GetDuration();
            const bool isFiniteDuration = (dur > 0.0 && std::isfinite(dur));
            const double safeDur = isFiniteDuration ? dur : 0.0;
            const double remTime = isFiniteDuration ? std::max(0.0, safeDur - curTime) : 0.0;
            auto chapters = player.GetChapters();
            int curChap = player.GetCurrentChapter();
            if (curChap < 0 && !chapters.empty() && isFiniteDuration) {
                for (int i = (int)chapters.size() - 1; i >= 0; --i) {
                    if (chapters[i].time <= curTime + 0.25) { curChap = i; break; }
                }
            }
            std::wstring timelineVal;
            if (isFiniteDuration) {
                timelineVal = FormatDuration(curTime) + L" of " + FormatDuration(safeDur) + L" (-" + FormatDuration(remTime) + L" remaining)";
            } else { timelineVal = FormatDuration(curTime) + L" (Live stream)"; }
            if (curChap >= 0 && curChap < (int)chapters.size()) {
                const std::wstring& chTitle = chapters[curChap].title;
                timelineVal += L", Chapter " + std::to_wstring(curChap + 1) + L" of " + std::to_wstring(chapters.size());
                if (!chTitle.empty()) { timelineVal += L": " + chTitle; }
            }
            if (abLoopA >= 0.0 && abLoopB > abLoopA) {
                timelineVal += L", A-B Repeat: " + FormatDuration(abLoopA) + L" to " + FormatDuration(abLoopB);
            } else if (abLoopA >= 0.0) { timelineVal += L", A-B Start: " + FormatDuration(abLoopA); }
            const double cacheDur = player.GetCacheDuration();
            if (cacheDur > 0.0 && isFiniteDuration) {
                const double bufferedPos = std::min(safeDur, curTime + cacheDur);
                const int bufferedPct = static_cast<int>(std::round((bufferedPos / safeDur) * 100.0));
                timelineVal += L", " + std::to_wstring(bufferedPct) + L"% buffered";
            }
            std::wstring itemStatusStr;
            if (isFiniteDuration) {
                itemStatusStr = player.IsPlaying() ? L"Playing, Seekable" : L"Paused, Seekable";
                if (curChap >= 0 && curChap < (int)chapters.size())
                    itemStatusStr += L", Chapter " + std::to_wstring(curChap + 1) + L"/" + std::to_wstring(chapters.size());
                if (abLoopA >= 0.0 && abLoopB > abLoopA) {
                    itemStatusStr += L", A-B Active (" + FormatDuration(abLoopA) + L" - " + FormatDuration(abLoopB) + L")";
                } else if (abLoopA >= 0.0) { itemStatusStr += L", A-B Start: " + FormatDuration(abLoopA); }
                if (cacheDur > 0.0)
                    itemStatusStr += L", Buffer: " + std::to_wstring(static_cast<int>(std::round(cacheDur))) + L"s ahead";
            } else { itemStatusStr = player.IsPlaying() ? L"Live stream, Playing, Non-seekable" : L"Live stream, Paused, Non-seekable"; }
            std::wstring helpTextStr;
            if (isFiniteDuration) {
                helpTextStr = L"Seekable playback timeline. Use Left/Right arrow keys to seek by " +
                              std::to_wstring(static_cast<int>(std::round(settings.arrowSeekStepSec))) +
                              L" seconds, or type a timestamp or percentage.";
            } else { helpTextStr = L"Live stream playback timeline. Seeking is disabled for live media."; }
            addNode(9003, L"Timeline", helpTextStr, timelineVal,
                ROLE_SYSTEM_SLIDER, playerControlState(9003), screenRect(playerPadX, winH - bottomBarH + 8.0f * dp, winW - playerPadX, winH - bottomBarH + 24.0f * dp),
                isFiniteDuration ? L"Seek" : L"", L"", {}, [&](AccessibilityNode& n) {
                    n.automationId = L"TimelineSlider";
                    n.className = L"ApertureSlider";
                    n.controlTypeId = UIA_SliderControlTypeId;
                    n.itemType = L"Timeline Slider";
                    n.itemStatus = itemStatusStr;
                    n.helpText = helpTextStr;
                    n.labeledById = 9002;
                    n.controllerForIds = { 9002 };
                    n.controlledPeerIds = { 9002, 9011 };
                    n.flowsFromIds = { 9011 };
                    n.flowsToIds = { 9004 };
                    if (isFiniteDuration) {
                        n.hasRangeValue = true;
                        n.rangeMin = 0.0;
                        n.rangeMax = safeDur;
                        n.rangeValue = std::clamp(std::floor(curTime), 0.0, safeDur);
                        n.rangeSmallChange = std::max(1.0, settings.arrowSeekStepSec);
                        n.rangeLargeChange = std::max(n.rangeSmallChange, settings.arrowSeekStepSec * 2.0);
                        n.rangeIsReadOnly = false;
                        n.onSetRangeValue = [this, safeDur](LONG, double val) {
                            double clamped = std::clamp(val, 0.0, safeDur);
                            commandDispatcher.Execute(AppCommand::SeekTo, CommandArgs{ clamped });
                            AnnounceStatus(L"Seek to " + FormatDuration(clamped));
                        };
                        n.hasValue = true;
                        n.isReadOnly = false;
                        n.onSetValue = [this, safeDur](LONG, const std::wstring& val) {
                            if (val.empty() || safeDur <= 0.0) return;
                            std::wstring clean = val;
                            while (!clean.empty() && iswspace(clean.front())) clean.erase(clean.begin());
                            while (!clean.empty() && iswspace(clean.back())) clean.pop_back();
                            if (clean.empty()) return;
                            double targetSec = 0.0;
                            if (clean.back() == L'%') {
                                clean.pop_back();
                                while (!clean.empty() && iswspace(clean.back())) clean.pop_back();
                                try { double pct = std::stod(clean); targetSec = (pct / 100.0) * safeDur; } catch (...) { return; }
                            } else if (clean.find(L':') != std::wstring::npos) {
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
                                if (parts.size() == 1) { targetSec = parts[0]; } else if (parts.size() == 2) {
                                    targetSec = parts[0] * 60.0 + parts[1];
                                } else if (parts.size() >= 3) { targetSec = parts[0] * 3600.0 + parts[1] * 60.0 + parts[2]; }
                            } else {
                                try { targetSec = std::stod(clean); } catch (...) { return; }
                            }
                            double clamped = std::clamp(targetSec, 0.0, safeDur);
                            commandDispatcher.Execute(AppCommand::SeekTo, CommandArgs{ clamped });
                            AnnounceStatus(L"Seek to " + FormatDuration(clamped));
                        };
                    } else {
                        n.hasRangeValue = false;
                        n.rangeMin = 0.0;
                        n.rangeMax = 0.0;
                        n.rangeValue = 0.0;
                        n.rangeIsReadOnly = true;
                        n.onSetRangeValue = nullptr;
                        n.hasValue = true;
                        n.isReadOnly = true;
                        n.onSetValue = nullptr;
                    }
                });
            const float volX = playerPadX + btn * 2.0f + 14.0f * dp;
            addNode(9004, player.IsMuted() ? L"Unmute" : L"Mute", L"Mute or unmute audio", player.IsMuted() ? L"Muted" : std::to_wstring(static_cast<int>(std::round(player.GetVolume() * 100.0))) + L"%",
                ROLE_SYSTEM_PUSHBUTTON, playerControlState(9004), playerRect(playerLayout.mute),
                player.IsMuted() ? L"Unmute" : L"Mute", shortcut("toggleMute"), [this]() { commandDispatcher.Execute(AppCommand::ToggleMute); },
                [&](AccessibilityNode& n) {
                    n.automationId = L"MuteButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Audio Control";
                    n.controllerForIds = { 9012 };
                    n.controlledPeerIds = { 9012 };
                    n.flowsFromIds = { 9003 };
                    n.flowsToIds = { 9012 };
                    n.hasToggle = true;
                    n.toggleState = player.IsMuted() ? ToggleState_On : ToggleState_Off;
                    n.onToggle = [this](LONG, ToggleState) { commandDispatcher.Execute(AppCommand::ToggleMute); };
                    n.itemStatus = player.IsMuted() ? L"Muted" : L"Unmuted";
                });
            addNode(9012, L"Volume", L"Playback volume", std::to_wstring(static_cast<int>(std::round(player.GetVolume() * 100.0))) + L"%", ROLE_SYSTEM_SLIDER, playerControlState(9012) | (playerLayout.showVolume ? 0 : STATE_SYSTEM_INVISIBLE),
                playerRect(playerLayout.volume),
                L"Increase", shortcut("volumeUp"), [this]() { commandDispatcher.Execute(AppCommand::VolumeUp); },
                [&](AccessibilityNode& n) {
                    n.automationId = L"VolumeSlider";
                    n.className = L"ApertureSlider";
                    n.controlTypeId = UIA_SliderControlTypeId;
                    n.itemType = L"Volume Slider";
                    n.labeledById = 9004;
                    n.controllerForIds = { 9004 };
                    n.controlledPeerIds = { 9004 };
                    n.flowsFromIds = { 9004 };
                    n.flowsToIds = { 9005 };
                    n.hasRangeValue = true;
                    n.rangeMin = 0.0;
                    n.rangeMax = 100.0;
                    n.rangeValue = std::clamp(player.GetVolume() * 100.0, 0.0, 100.0);
                    n.rangeSmallChange = 5.0;
                    n.rangeLargeChange = 10.0;
                    n.rangeIsReadOnly = false;
                    n.onSetRangeValue = [this](LONG, double val) { commandDispatcher.Execute(AppCommand::SetVolume, CommandArgs{ std::clamp(val / 100.0, 0.0, 1.0) }); };
                });
            const float right = winW - playerPadX - btn;
            addNode(9005, L"Subtitles / CC", L"Cycle subtitle tracks", player.GetCurrentSubtitleTrackTitle(), ROLE_SYSTEM_PUSHBUTTON, playerControlState(9005) | (playerLayout.showSubtitles ? 0 : STATE_SYSTEM_INVISIBLE),
                playerRect(playerLayout.subtitles),
                L"Toggle", L"", [this]() { commandDispatcher.Execute(AppCommand::ToggleSubtitles); },
                [&](AccessibilityNode& n) {
                    n.automationId = L"SubtitlesButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.flowsFromIds = { 9012 };
                    n.flowsToIds = { 9006 };
                    n.hasToggle = true;
                    n.toggleState = settings.subtitlesEnabled ? ToggleState_On : ToggleState_Off;
                    n.onToggle = [this](LONG, ToggleState) { commandDispatcher.Execute(AppCommand::ToggleSubtitles); };
                });
            addNode(9006, L"Settings", L"Open playback settings", L"", ROLE_SYSTEM_PUSHBUTTON, playerControlState(9006),
                playerRect(playerLayout.settings),
                L"Open", L"", [this]() {
                    PushFocusSnapshot(TransientSurfaceType::SettingsFlyout);
                    isSettingsFlyoutOpen = true;
                    currentFlyoutMenu = "main";
                    controlsOpacity = 1.0f;
                    flyoutClosing = false;
                    flyoutAnim.SnapTo(0.0f);
                    AnimateVisual(flyoutAnim, 1.0f, MotionTokens::kToggle, MotionTokens::Spring());
                },
                [&](AccessibilityNode& n) {
                    n.automationId = L"PlayerSettingsButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.flowsFromIds = { 9005 };
                    n.flowsToIds = { 9007 };
                    if (isSettingsFlyoutOpen) {
                        std::vector<LONG> flyoutIds;
                        for (int f = 0; f < GetFlyoutOptionCount(); ++f) {
                            flyoutIds.push_back(static_cast<LONG>(9100 + f + currentFlyoutMenu.size() * 100));
                        }
                        n.controllerForIds = flyoutIds;
                    }
                });
            addNode(9007, playbackSession.IsPiP() ? L"Exit Picture-in-Picture" : L"Picture-in-Picture", L"Toggle picture-in-picture mode", L"", ROLE_SYSTEM_PUSHBUTTON, playerControlState(9007) | (playerLayout.showPip ? 0 : STATE_SYSTEM_INVISIBLE),
                playerRect(playerLayout.pip),
                L"Toggle", shortcut("togglePiP"), [this]() { commandDispatcher.Execute(AppCommand::TogglePiP); },
                [&](AccessibilityNode& n) {
                    n.automationId = L"PiPButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.flowsFromIds = { 9006 };
                    n.flowsToIds = { 9008 };
                    n.hasToggle = true;
                    n.toggleState = playbackSession.IsPiP() ? ToggleState_On : ToggleState_Off;
                    n.onToggle = [this](LONG, ToggleState) { commandDispatcher.Execute(AppCommand::TogglePiP); };
                });
            addNode(9008, playbackSession.IsFullscreen() ? L"Exit Fullscreen" : L"Fullscreen", L"Toggle fullscreen mode", L"", ROLE_SYSTEM_PUSHBUTTON, playerControlState(9008),
                playerRect(playerLayout.fullscreen),
                L"Toggle", shortcut("toggleFullscreen"), [this]() { commandDispatcher.Execute(AppCommand::ToggleFullscreen); },
                [&](AccessibilityNode& n) {
                    n.automationId = L"FullscreenButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Player Control";
                    n.flowsFromIds = { 9007 };
                    n.hasToggle = true;
                    n.toggleState = playbackSession.IsFullscreen() ? ToggleState_On : ToggleState_Off;
                    n.onToggle = [this](LONG, ToggleState) { commandDispatcher.Execute(AppCommand::ToggleFullscreen); };
                });
        }
        if (isSettingsFlyoutOpen) {
            const float flyoutW = 280.0f * dp;
            const float flyoutH = GetFlyoutHeight();
            const float left = winW - flyoutW - 20.0f * dp, top = winH - bottomBarH - flyoutH - 10.0f * dp;
            const int count = GetFlyoutOptionCount();
            const auto addFlyout = [&](int index, const std::wstring& name, float x, float y, const std::function<void()>& action,
                                       const std::function<void(AccessibilityNode&)>& cfg = nullptr) {
                addNode(9100 + index + static_cast<LONG>(currentFlyoutMenu.size() * 100), name, L"Playback setting", L"", ROLE_SYSTEM_PUSHBUTTON,
                    stateBits(index == hoveredFlyoutOption, false, false, false, false), screenRect(left + x, top + y, left + x + flyoutW - 28.0f * dp, top + y + 34.0f * dp),
                    L"Select", L"", action, [&](AccessibilityNode& n) {
                        n.automationId = L"FlyoutItem_" + std::to_wstring(n.id);
                        n.className = L"ApertureMenuItem";
                        n.controlTypeId = UIA_MenuItemControlTypeId;
                        n.itemType = L"Flyout Option";
                        n.controlledPeerIds = { 9006 };
                        n.selectionContainerId = 9006;
                        n.positionInSet = index + 1;
                        n.sizeOfSet = count;
                        n.flowsFromIds = (index == 0) ? std::vector<LONG>{ 9006 } : std::vector<LONG>{ static_cast<LONG>(9100 + index - 1 + currentFlyoutMenu.size() * 100) };
                        if (index + 1 < count) {
                            n.flowsToIds = { static_cast<LONG>(9100 + index + 1 + currentFlyoutMenu.size() * 100) };
                        }
                        if (cfg) cfg(n);
                    });
            };
            if (currentFlyoutMenu == "main") {
                const wchar_t* labels[] = { L"Subtitles / CC", L"Audio", L"Speed", L"Aspect Ratio", L"Queue", L"Loop", L"Autoplay", L"Private Session" };
                for (int i = 0; i < count && i < 8; ++i) {
                    addFlyout(i, labels[i], 14.0f * dp, 8.0f * dp + i * 42.0f * dp,
                        [this, i]() { HandleFlyoutClick(140.0f * overlayRenderer.GetDpiScale(), (8.0f + i * 42.0f + 20.0f) * overlayRenderer.GetDpiScale()); },
                        [&, i](AccessibilityNode& n) {
                            if (i == 5) { // Loop
                                n.hasToggle = true;
                                n.toggleState = settings.loop ? ToggleState_On : ToggleState_Off;
                                n.onToggle = [this, i](LONG, ToggleState) { HandleFlyoutClick(140.0f * overlayRenderer.GetDpiScale(), (8.0f + i * 42.0f + 20.0f) * overlayRenderer.GetDpiScale()); };
                            } else if (i == 6) { // Autoplay
                                n.hasToggle = true;
                                n.toggleState = settings.autoplay ? ToggleState_On : ToggleState_Off;
                                n.onToggle = [this, i](LONG, ToggleState) { HandleFlyoutClick(140.0f * overlayRenderer.GetDpiScale(), (8.0f + i * 42.0f + 20.0f) * overlayRenderer.GetDpiScale()); };
                            } else if (i == 7) { // Private Session
                                n.hasToggle = true;
                                n.toggleState = playbackSession.IsPrivateMode() ? ToggleState_On : ToggleState_Off;
                                n.onToggle = [this, i](LONG, ToggleState) { HandleFlyoutClick(140.0f * overlayRenderer.GetDpiScale(), (8.0f + i * 42.0f + 20.0f) * overlayRenderer.GetDpiScale()); };
                            }
                        });
                }
            } else if (currentFlyoutMenu == "queue") {
                const auto queue = playbackSession.GetQueue();
                if (queue.empty()) {
                    addNode(9100 + static_cast<LONG>(currentFlyoutMenu.size() * 100), L"Queue is empty", L"No queued media", L"",
                        ROLE_SYSTEM_STATICTEXT, stateBits(false, false, true, false, false),
                        screenRect(left + 14.0f * dp, top + 68.0f * dp, left + flyoutW - 14.0f * dp, top + 104.0f * dp), L"", L"", nullptr,
                        [](AccessibilityNode& n) {
                            n.automationId = L"QueueEmptyState";
                            n.className = L"ApertureText";
                            n.controlTypeId = UIA_TextControlTypeId;
                            n.itemType = L"Queue Status";
                            n.controlledPeerIds = { 9006 };
                        });
                } else {
                    const size_t first = std::min(queueFlyoutFirstVisible, queue.size() - 1);
                    const size_t end = std::min(queue.size(), first + static_cast<size_t>(7));
                    for (size_t i = first; i < end; ++i) {
                        const int visibleIndex = static_cast<int>(i - first);
                        const std::wstring title = GetFileTitleFromPath(queue[i]);
                        addFlyout(visibleIndex, title, 14.0f * dp, 78.0f * dp + visibleIndex * 36.0f * dp, [this, visibleIndex]() {
                                const float d = overlayRenderer.GetDpiScale();
                                HandleFlyoutClick(140.0f * d, 78.0f * d + visibleIndex * 36.0f * d + 17.0f * d);
                            }, [&, i](AccessibilityNode& n) {
                                n.itemType = L"Queue Item";
                                n.hasSelectionItem = true;
                                n.selected = i == playbackSession.GetQueueIndex();
                                n.itemStatus = n.selected ? L"Currently playing" : L"";
                            });
                    }
                }
            } else if (currentFlyoutMenu == "speed") {
                const wchar_t* labels[] = { L"0.25x", L"0.5x", L"0.75x", L"1.0x", L"1.25x", L"1.5x", L"1.75x", L"2.0x", L"3.0x" };
                const double speeds[] = { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0 };
                for (int i = 0; i < 9; ++i) {
                    const int row = i / 3, col = i % 3;
                    const float pillW = (flyoutW - 28.0f * dp - 10.0f * dp) / 3.0f;
                    const bool isSel = std::abs(player.GetPlaybackRate() - speeds[i]) < 0.05;
                    addFlyout(i, labels[i], 14.0f * dp + col * (pillW + 5.0f * dp), 108.0f * dp + row * 35.0f * dp,
                        [this, i]() { const float d = overlayRenderer.GetDpiScale(); const float w = (280.0f * d - 28.0f * d - 10.0f * d) / 3.0f; HandleFlyoutClick(14.0f * d + (i % 3) * (w + 5.0f * d) + w * 0.5f, 108.0f * d + (i / 3) * 35.0f * d + 15.0f * d); },
                        [&, isSel](AccessibilityNode& n) { n.hasSelectionItem = true; n.selected = isSel; });
                }
            } else if (currentFlyoutMenu == "audiodelay" || currentFlyoutMenu == "subdelay") {
                const wchar_t* labels[] = { L"Decrease", L"Reset", L"Increase" };
                for (int i = 0; i < 3; ++i) addFlyout(i, labels[i], 14.0f * dp + i * 82.0f * dp, 96.0f * dp,
                    [this, i]() { const float d = overlayRenderer.GetDpiScale(); HandleFlyoutClick(14.0f * d + i * 82.0f * d + 30.0f * d, 113.0f * d); });
            } else {
                if (currentFlyoutMenu == "aspect") {
                    const wchar_t* labels[] = { L"Auto", L"16:9", L"4:3", L"21:9", L"Fill", L"Cover" };
                    for (int i = 0; i < 6; ++i) addFlyout(i, labels[i], 14.0f * dp, 50.0f * dp + i * 36.0f * dp,
                        [this, i]() { const float d = overlayRenderer.GetDpiScale(); HandleFlyoutClick(140.0f * d, 50.0f * d + i * 36.0f * d + 16.0f * d); });
                } else if (currentFlyoutMenu == "subtitles") {
                    auto tracks = player.GetSubtitleTracks();
                    addFlyout(0, L"Off", 14.0f * dp, 50.0f * dp, [this]() { HandleFlyoutClick(140.0f * overlayRenderer.GetDpiScale(), 66.0f * overlayRenderer.GetDpiScale()); });
                    for (size_t i = 0; i < tracks.size(); ++i) addFlyout(static_cast<int>(i + 1), tracks[i].title, 14.0f * dp, 50.0f * dp + static_cast<float>(i + 1) * 36.0f * dp,
                        [this, i]() { const float d = overlayRenderer.GetDpiScale(); HandleFlyoutClick(140.0f * d, 50.0f * d + static_cast<float>(i + 1) * 36.0f * d + 16.0f * d); });
                } else if (currentFlyoutMenu == "audio") {
                    auto tracks = player.GetAudioTracks();
                    for (size_t i = 0; i < tracks.size(); ++i) addFlyout(static_cast<int>(i), tracks[i].title, 14.0f * dp, 50.0f * dp + static_cast<float>(i) * 36.0f * dp,
                        [this, i]() { const float d = overlayRenderer.GetDpiScale(); HandleFlyoutClick(140.0f * d, 50.0f * d + static_cast<float>(i) * 36.0f * d + 16.0f * d); });
                }
            }
        }
    } else {
        // --- 5. HUB NAV BAR ---
        bool isRtl = Localization::Instance().IsRtl();
        const float navW = winW - padX * 2.0f, navH = 48.0f * dp, tabW = navW / 5.0f;
        std::wstring navNames[] = { Loc::Tr("nav.media"), Loc::Tr("nav.playlists"), Loc::Tr("nav.history"), Loc::Tr("nav.settings"), Loc::Tr("nav.open_file") };
        for (int i = 0; i < 5; ++i) {
            const bool selected = i < 4 && static_cast<int>(currentTab) == i;
            float nodeLeft = isRtl ? (winW - padX - (i + 1) * tabW) : (padX + i * tabW);
            float nodeRight = isRtl ? (winW - padX - i * tabW) : (padX + (i + 1) * tabW);
            addNode(100 + i, navNames[i], i == 4 ? L"Choose media files" : L"Switch library view", L"", (i == 4) ? ROLE_SYSTEM_PUSHBUTTON : ROLE_SYSTEM_PAGETAB,
                stateBits(selected, activeFocusPane == FocusPane::TopNavBar && topNavFocusedIndex == i, false, false, selected),
                screenRect(nodeLeft, padY, nodeRight, padY + navH), L"Activate", i == 4 ? L"Ctrl+O" : L"", [this, i]() {
                    if (i == 4) {
                        OpenFileDialog();
                    } else { currentTab = static_cast<TabView>(i); activeFocusPane = FocusPane::TopNavBar; topNavFocusedIndex = i; }
                }, [&](AccessibilityNode& n) {
                    n.automationId = (i == 4) ? L"Nav_OpenFileButton" : (L"NavTab_" + std::to_wstring(i));
                    n.className = (i == 4) ? L"ApertureButton" : L"ApertureTabItem";
                    n.controlTypeId = (i == 4) ? UIA_ButtonControlTypeId : UIA_TabItemControlTypeId;
                    n.itemType = (i == 4) ? L"Navigation Action" : L"Navigation Tab";
                    n.positionInSet = i + 1;
                    n.sizeOfSet = 5;
                    n.flowsFromIds = (i > 0) ? std::vector<LONG>{ static_cast<LONG>(100 + i - 1) } : std::vector<LONG>{};
                    n.flowsToIds = (i < 4) ? std::vector<LONG>{ static_cast<LONG>(100 + i + 1) } : std::vector<LONG>{};
                    if (i < 4) {
                        n.hasSelectionItem = true;
                        n.selected = (static_cast<int>(currentTab) == i);
                        n.onSelect = [this, i](LONG) {
                            currentTab = static_cast<TabView>(i);
                            activeFocusPane = FocusPane::TopNavBar;
                            topNavFocusedIndex = i;
                            InvalidateRect(hWnd, NULL, FALSE);
                        };
                    }
                    if (i == 0 && currentTab == TabView::Media) {
                        n.hasSelection = true;
                        n.canSelectMultiple = true;
                        n.isSelectionRequired = false;
                        n.onGetSelection = [this](LONG) -> std::vector<LONG> {
                            std::vector<LONG> result;
                            std::unordered_set<size_t> allSelected = selectedMediaIndices;
                            if (selectedMediaIndex >= 0 && static_cast<size_t>(selectedMediaIndex) < library.mediaItems.size())
                                allSelected.insert(static_cast<size_t>(selectedMediaIndex));
                            result.reserve(allSelected.size());
                            for (size_t origIdx : allSelected) {
                                if (origIdx < library.mediaItems.size())
                                    result.push_back(AccessibilityStableId(1000, library.mediaItems[origIdx].filePath));
                            }
                            return result;
                        };
                        float maxScroll = GetMaxScrollForPane(AutoscrollPane::Media);
                        if (maxScroll > 0.5f) {
                            float tbH = 32.0f * dp, mediaListTop = viewTop + tbH + 12.0f * dp;
                            float mediaViewH = std::max(1.0f * dp, (winH - padY) - mediaListTop);
                            float mediaContentH = m_mediaLayoutCache.contentHeightDIP * dp;
                            ConfigureAccessibleScroll(n, &App::mediaScrollY, maxScroll, mediaViewH, mediaContentH, 50.0f, dp);
                        }
                    } else if (i == 1 && currentTab == TabView::Playlists) {
                        n.hasSelection = true;
                        n.canSelectMultiple = false;
                        n.isSelectionRequired = false;
                        n.onGetSelection = [this](LONG) -> std::vector<LONG> {
                            std::vector<LONG> result;
                            if (!activePlaylistId.empty()) { result.push_back(AccessibilityStableId(3001, Utf8ToUtf16(activePlaylistId))); }
                            return result;
                        };
                        float railMax = GetMaxScrollForPane(AutoscrollPane::PlaylistRail);
                        if (railMax > 0.5f) {
                            float viewH = std::max(1.0f * dp, (winH - padY) - viewTop);
                            float railContentH = 48.0f * dp - 2.0f * dp + library.playlists.size() * (52.0f * dp + 6.0f * dp);
                            float railVisibleH = std::max(1.0f * dp, viewH - 34.0f * dp - 18.0f * dp);
                            ConfigureAccessibleScroll(n, &App::playlistRailScrollY, railMax, railVisibleH, railContentH, 58.0f, dp);
                        }
                    } else if (i == 2 && currentTab == TabView::History) {
                        float histMax = GetMaxScrollForPane(AutoscrollPane::History);
                        if (histMax > 0.5f) {
                            float viewH = std::max(1.0f * dp, (winH - padY) - viewTop);
                            const float historyContentBottom = library.history.empty() ? kHistoryContentInset * dp
                                : HistoryContentHeight(GetHistoryProjection().groups, dp);
                            ConfigureAccessibleScroll(n, &App::historyScrollY, histMax, viewH, historyContentBottom, 84.0f, dp);
                        }
                    } else if (i == 3 && currentTab == TabView::Settings) {
                        float settingsMax = GetMaxScrollForPane(AutoscrollPane::Settings);
                        if (settingsMax > 0.5f) {
                            float viewH = std::max(1.0f * dp, (winH - padY) - viewTop);
                            float settingsContentH = GetSettingsContentHeight(dp);
                            ConfigureAccessibleScroll(n, &App::settingsScrollY, settingsMax, viewH, settingsContentH, 54.0f, dp);
                        }
                    }
                });
        }
        const D2D1_RECT_F view = D2D1::RectF(padX, viewTop, winW - padX, winH - padY);
        // --- 6. MEDIA TAB ---
        if (currentTab == TabView::Media) {
            const float tbH = 32.0f * dp;
            const auto toolbar = ComputeMediaToolbarGeometry(D2D1::RectF(view.left, view.top, view.right, view.top + tbH), dp);
            addNode(1048, L"Filter Media", L"Filter media by title, filename, or series stem", Text16ToWideString(mediaSearchInput.GetText()), ROLE_SYSTEM_TEXT,
                stateBits(false, activeTextInputField == TextInputField::MediaSearch, false, false, false),
                screenRect(toolbar.search.left, toolbar.search.top, toolbar.search.right, toolbar.search.bottom),
                L"Edit", L"Ctrl+F", [this]() { FocusTextInput(TextInputField::MediaSearch); activeFocusPane = FocusPane::MediaList; },
                [this](AccessibilityNode& n) {
                    n.automationId = L"MediaToolbar_SearchInput";
                    n.className = L"ApertureEdit";
                    n.controlTypeId = UIA_EditControlTypeId;
                    n.itemType = L"Media Search Input";
                    n.ariaRole = L"searchbox";
                    n.hasValue = true;
                    n.isReadOnly = false;
                    n.onSetValue = [this](LONG, const std::wstring& val) {
                        mediaSearchInput.SetText(WideStringToText16(val));
                        MarkTextInputEdit(true);
                    };
                });
            addNode(1049, L"Sort Media", L"Current sort: " + GetLibrarySortLabel(), L"", ROLE_SYSTEM_PUSHBUTTON,
                stateBits(false, false, false, false, false),
                screenRect(toolbar.sort.left, toolbar.sort.top, toolbar.sort.right, toolbar.sort.bottom),
                L"Open", L"", [this, toolbar, dp]() { OpenMediaSortContextMenu(toolbar.sort.left, toolbar.sort.bottom + 6.0f * dp); },
                [](AccessibilityNode& n) {
                    n.automationId = L"MediaToolbar_Sort";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Media Toolbar Action";
                });
            const wchar_t* groupLabels[] = { L"Standalone", settings.smartSeriesDetection ? L"Series" : L"Folders" };
            const LibraryGroupMode groupModes[] = { LibraryGroupMode::Flat, settings.smartSeriesDetection ? LibraryGroupMode::Series : LibraryGroupMode::Folder };
            const D2D1_RECT_F groupRects[] = { toolbar.flat, toolbar.series };
            for (int g = 0; g < 2; ++g) {
                const bool active = libraryGroupMode == groupModes[g];
                addNode(1050 + g, groupLabels[g], L"Library grouping mode", active ? L"Selected" : L"", ROLE_SYSTEM_RADIOBUTTON,
                    stateBits(active, false, false, false, active),
                    screenRect(groupRects[g].left, groupRects[g].top, groupRects[g].right, groupRects[g].bottom),
                    L"Select", L"", [this, mode = groupModes[g]]() { SetLibraryGroupMode(mode); }, [&, g, active](AccessibilityNode& n) {
                        n.automationId = L"MediaGroup_" + std::wstring(groupLabels[g]);
                        n.className = L"ApertureRadioButton";
                        n.controlTypeId = UIA_RadioButtonControlTypeId;
                        n.itemType = L"Group Mode Option";
                        n.positionInSet = g + 1;
                        n.sizeOfSet = 2;
                        n.selectionContainerId = 100;
                        n.flowsFromIds = (g == 0) ? std::vector<LONG>{ 100 } : std::vector<LONG>{ static_cast<LONG>(1050 + g - 1) };
                        if (g < 1) { n.flowsToIds = {static_cast<LONG>(1050 + g + 1)}; }
                        n.hasSelectionItem = true;
                        n.selected = active;
                        n.onSelect = [this, mode = groupModes[g]](LONG) { SetLibraryGroupMode(mode); };
                    });
            }
            EnsureMediaLayoutCache();
            const auto& visibleIndices = GetVisibleMediaIndices();
            if (visibleIndices.empty()) {
                const bool libraryEmpty = library.mediaItems.empty();
                const std::wstring title = libraryEmpty ? L"Your Media Library is empty" : L"No duplicate media found";
                const std::wstring body = libraryEmpty ? L"Open media or add a folder to start your portable library."
                    : L"Switch back to the full library view.";
                const float cardW = std::min(560.0f * dp, RectWidth(view) - 48.0f * dp);
                const D2D1_RECT_F card = D2D1::RectF( view.left + (RectWidth(view) - cardW) * 0.5f, view.top + 70.0f * dp,
                    view.left + (RectWidth(view) + cardW) * 0.5f, view.top + 290.0f * dp);
                const size_t buttonCount = libraryEmpty ? 2u : 1u;
                addNode(1080, title, body, L"", ROLE_SYSTEM_GROUPING, stateBits(false, false, false, false, false),
                    screenRect(card.left, card.top, card.right, card.bottom), L"", L"", nullptr, [buttonCount](AccessibilityNode& n) {
                        n.automationId = L"MediaEmptyState_Card";
                        n.className = L"ApertureEmptyState";
                        n.controlTypeId = UIA_GroupControlTypeId;
                        n.itemType = L"Empty State";
                        n.controllerForIds = (buttonCount == 2) ? std::vector<LONG>{ 1081, 1082 } : std::vector<LONG>{ 1081 };
                        n.flowsToIds = { 1081 };
                    });
                const wchar_t* labels[] = { L"Open File", L"Add Folder" };
                for (size_t b = 0; b < buttonCount; ++b) {
                    const std::wstring label = libraryEmpty ? labels[b] : L"Clear Filters";
                    const float buttonW = libraryEmpty ? 128.0f * dp : 140.0f * dp, gap = 12.0f * dp;
                    const float totalW = buttonCount * buttonW + (buttonCount - 1) * gap;
                    const float left = view.left + (RectWidth(view) - totalW) * 0.5f + b * (buttonW + gap);
                    const RECT bounds = screenRect(left, card.bottom - 58.0f * dp, left + buttonW, card.bottom - 20.0f * dp);
                    std::function<void()> action = !libraryEmpty
                        ? std::function<void()>{ [this]() { SetLibraryGroupMode(LibraryGroupMode::Flat); } }
                        : (b == 0 ? std::function<void()>{ [this]() { OpenFileDialog(); } }
                                  : std::function<void()>{ [this]() { AddWatchedFolderPrompt(); } });
                    const bool focused = activeFocusPane == FocusPane::MediaList && keyboardFocusedIndex == static_cast<int>(b);
                    addNode(1081 + static_cast<LONG>(b), label, label, L"", ROLE_SYSTEM_PUSHBUTTON,
                        stateBits(false, focused, false, false, false), bounds,
                        L"Activate", (libraryEmpty && b == 0) ? L"Ctrl+O" : L"", action, [label, b, buttonCount](AccessibilityNode& n) {
                            n.automationId = label == L"Open File" ? L"MediaEmptyState_OpenFile" :
                                (label == L"Add Folder" ? L"MediaEmptyState_AddFolder" : L"MediaEmptyState_ClearFilters");
                            n.className = L"ApertureButton";
                            n.controlTypeId = UIA_ButtonControlTypeId;
                            n.itemType = L"Empty State Action";
                            n.describedByIds = { 1080 };
                            n.groupId = 1080;
                            n.controlledPeerIds = { 1080 };
                            n.positionInSet = static_cast<int>(b + 1);
                            n.sizeOfSet = static_cast<int>(buttonCount);
                            n.flowsFromIds = (b == 0) ? std::vector<LONG>{ 1080 } : std::vector<LONG>{ 1081 };
                        });
                }
            } else {
                float scrollYDIP = mediaScrollY / dp;
                float viewportHDIP = RectHeight(view) / dp;
                auto [firstVisible, lastVisible] = GetVisibleMediaRowRange(scrollYDIP, viewportHDIP, visibleIndices.size(), 2);
                std::unordered_set<size_t> indicesToInclude;
                for (size_t i = firstVisible; i < lastVisible; ++i) { indicesToInclude.insert(i); }
                if (activeFocusPane == FocusPane::MediaList && keyboardFocusedIndex >= 0 && keyboardFocusedIndex < (int)visibleIndices.size())
                    indicesToInclude.insert((size_t)keyboardFocusedIndex);
                for (size_t vi = 0; vi < visibleIndices.size(); ++vi) {
                    size_t oIdx = visibleIndices[vi];
                    if (selectedMediaIndices.count(oIdx) != 0 || selectedMediaIndex == static_cast<int>(oIdx))
                        indicesToInclude.insert(vi);
                }
                float listTop = view.top + tbH + 12.0f * dp;
                for (size_t i : indicesToInclude) {
                    if (i >= visibleIndices.size()) continue;
                    size_t origIdx = visibleIndices[i];
                    if (origIdx >= library.mediaItems.size()) continue;
                    float rowTopDIP = GetMediaRowTopDIP(i);
                    float y = listTop + (rowTopDIP * dp) - mediaScrollY;
                    float height = GetMediaRowHeightDIP(i) * dp;
                    const RECT bounds = screenRect(view.left + dp, y, view.right - 2.0f * dp, y + height);
                    if (intersects(bounds) || (activeFocusPane == FocusPane::MediaList && keyboardFocusedIndex == static_cast<int>(i)) || selectedMediaIndices.count(origIdx) != 0 || selectedMediaIndex == static_cast<int>(origIdx)) {
                        const auto& item = library.mediaItems[origIdx];
                        const bool selected = selectedMediaIndices.count(origIdx) != 0 || selectedMediaIndex == static_cast<int>(origIdx);
                        addNode(AccessibilityStableId(1000, item.filePath), item.title.empty() ? GetFileNameFromPath(item.filePath) : item.title,
                            item.filePath, Utf8ToUtf16(item.status), ROLE_SYSTEM_LISTITEM,
                            stateBits(selected, activeFocusPane == FocusPane::MediaList && keyboardFocusedIndex == static_cast<int>(i), false, false, selected), bounds,
                            L"Play", L"Enter", [this, path = item.filePath]() { OpenAndPlayMedia(path); }, [&](AccessibilityNode& n) {
                                n.automationId = L"MediaItem_" + std::to_wstring(n.id);
                                n.className = L"ApertureListItem";
                                n.controlTypeId = UIA_ListItemControlTypeId;
                                n.itemType = L"Media Item";
                                n.itemStatus = Utf8ToUtf16(item.status);
                                n.selectionContainerId = 100;
                                n.controlledPeerIds = { 1050, 1051, 1052, 1053 };
                                n.positionInSet = static_cast<int>(i + 1);
                                n.sizeOfSet = static_cast<int>(visibleIndices.size());
                                n.hasSelectionItem = true;
                                n.selected = selected;
                                n.onSelect = [this, origIdx, i](LONG) {
                                    if (origIdx >= library.mediaItems.size()) return;
                                    activeFocusPane = FocusPane::MediaList;
                                    keyboardFocusedIndex = static_cast<int>(i);
                                    selectionAnchorIndex = static_cast<int>(i);
                                    selectedMediaIndex = static_cast<int>(origIdx);
                                    selectedMediaIndices.clear();
                                    selectedMediaIndices.insert(origIdx);
                                    InvalidateRect(hWnd, NULL, FALSE);
                                };
                                n.onAddToSelection = [this, origIdx, i](LONG) {
                                    if (origIdx >= library.mediaItems.size()) return;
                                    activeFocusPane = FocusPane::MediaList;
                                    keyboardFocusedIndex = static_cast<int>(i);
                                    selectionAnchorIndex = static_cast<int>(i);
                                    selectedMediaIndices.insert(origIdx);
                                    selectedMediaIndex = static_cast<int>(origIdx);
                                    InvalidateRect(hWnd, NULL, FALSE);
                                };
                                n.onRemoveFromSelection = [this, origIdx, i](LONG) {
                                    if (origIdx >= library.mediaItems.size()) return;
                                    selectedMediaIndices.erase(origIdx);
                                    if (selectedMediaIndex == static_cast<int>(origIdx))
                                        selectedMediaIndex = selectedMediaIndices.empty() ? -1 : static_cast<int>(*selectedMediaIndices.begin());
                                    InvalidateRect(hWnd, NULL, FALSE);
                                };
                                n.hasScrollItem = true;
                                n.onScrollIntoView = [this, origIdx, i](LONG) {
                                    activeFocusPane = FocusPane::MediaList;
                                    keyboardFocusedIndex = static_cast<int>(i);
                                    selectionAnchorIndex = static_cast<int>(i);
                                    selectedMediaIndex = static_cast<int>(origIdx);
                                    selectedMediaIndices.clear();
                                    selectedMediaIndices.insert(origIdx);
                                    AutoScrollToFocusedItem();
                                    ClampScrollOffsets();
                                    InvalidateRect(hWnd, NULL, FALSE);
                                    MarkAccessibilityDirty();
                                };
                            });
                    }
                }
            }
        } else if (currentTab == TabView::Playlists) {
            // --- 7. PLAYLISTS TAB ---
            const float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(view) * 0.42f));
            float y = view.top + 46.0f * dp - playlistRailScrollY;
            addNode(3000, L"Add Playlist", L"Create a new playlist", L"", ROLE_SYSTEM_PUSHBUTTON, stateBits(false, false, false, false, false),
                screenRect(view.left, view.top, view.left + railW, view.top + 44.0f * dp), L"Create", L"", [this]() { ShowNewPlaylistModal(); },
                [this](AccessibilityNode& n) {
                    n.automationId = L"AddPlaylistButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"Playlist Action";
                    if (!library.playlists.empty()) { n.flowsToIds = {AccessibilityStableId(3001, Utf8ToUtf16(library.playlists[0].id))}; }
                });
            for (size_t i = 0; i < library.playlists.size(); ++i) {
                const RECT bounds = screenRect(view.left, y, view.left + railW, y + 52.0f * dp);
                if (intersects(bounds)) {
                    const auto& pl = library.playlists[i];
                    const bool selected = pl.id == activePlaylistId || selectedPlaylistIndices.count(i) != 0;
                    addNode(AccessibilityStableId(3001, Utf8ToUtf16(pl.id)), Utf8ToUtf16(pl.name), L"Playlist", std::to_wstring(pl.itemPaths.size()) + L" items", ROLE_SYSTEM_LISTITEM,
                        stateBits(selected, activeFocusPane == FocusPane::PlaylistRail && keyboardFocusedIndex == static_cast<int>(i), false, false, selected), bounds, L"Open", L"Enter",
                        [this, id = pl.id]() { activePlaylistId = id; currentTab = TabView::Playlists; activeFocusPane = FocusPane::PlaylistRail; },
                        [&](AccessibilityNode& n) {
                            n.automationId = L"PlaylistItem_" + Utf8ToUtf16(pl.id);
                            n.className = L"ApertureListItem";
                            n.controlTypeId = UIA_ListItemControlTypeId;
                            n.itemType = L"Playlist";
                            n.selectionContainerId = 101;
                            n.positionInSet = static_cast<int>(i + 1);
                            n.sizeOfSet = static_cast<int>(library.playlists.size());
                            if (selected) {
                                n.controllerForIds = { 3050, 3051, 3052, 3053, 3054 };
                                if (!pl.IsSmart() && pl.itemPaths.empty()) { n.controllerForIds.push_back(3056); }
                            }
                            n.hasSelectionItem = true;
                            n.selected = selected;
                            n.onSelect = [this, id = pl.id](LONG) {
                                activePlaylistId = id;
                                currentTab = TabView::Playlists;
                                activeFocusPane = FocusPane::PlaylistRail;
                                InvalidateRect(hWnd, NULL, FALSE);
                            };
                            n.hasScrollItem = true;
                            n.onScrollIntoView = [this, id = pl.id, i](LONG) {
                                activePlaylistId = id;
                                currentTab = TabView::Playlists;
                                activeFocusPane = FocusPane::PlaylistRail;
                                keyboardFocusedIndex = static_cast<int>(i);
                                AutoScrollToFocusedItem();
                                ClampScrollOffsets();
                                InvalidateRect(hWnd, NULL, FALSE);
                                MarkAccessibilityDirty();
                            };
                        });
                }
                y += 58.0f * dp;
            }
            Playlist* playlist = library.FindPlaylist(activePlaylistId);
            if (playlist) {
                const bool hasItems = !playlist->itemPaths.empty();
                addNode(3050, L"Play All", L"Play every item in this playlist", L"", ROLE_SYSTEM_PUSHBUTTON,
                    stateBits(false, false, !hasItems, false, false), screenRect(view.left + railW + 16.0f * dp, view.top + 116.0f * dp, view.left + railW + 116.0f * dp, view.top + 152.0f * dp),
                    L"Play", L"", hasItems ? std::function<void()>{ [this]() { PlayPlaylist(activePlaylistId, 0); } } : std::function<void()>{},
                    [this](AccessibilityNode& n) {
                        n.automationId = L"PlayAllButton";
                        n.className = L"ApertureButton";
                        n.controlTypeId = UIA_ButtonControlTypeId;
                        n.itemType = L"Playlist Action";
                        n.controlledPeerIds = { AccessibilityStableId(3001, Utf8ToUtf16(activePlaylistId)) };
                        n.flowsToIds = { 3051 };
                    });
                addNode(3051, L"Resume Playlist", L"Resume the current playlist", L"", ROLE_SYSTEM_PUSHBUTTON,
                    stateBits(false, false, !hasItems, false, false), screenRect(view.left + railW + 124.0f * dp, view.top + 116.0f * dp, view.left + railW + 244.0f * dp, view.top + 152.0f * dp),
                    L"Resume", L"", hasItems ? std::function<void()>{ [this]() {
                        Playlist* pl = library.FindPlaylist(activePlaylistId);
                        size_t resIdx = pl ? library.GetPlaylistResumeIndex(pl) : 0;
                        PlayPlaylist(activePlaylistId, resIdx);
                    } } : std::function<void()>{}, [](AccessibilityNode& n) {
                        n.automationId = L"ResumePlaylistButton";
                        n.className = L"ApertureButton";
                        n.controlTypeId = UIA_ButtonControlTypeId;
                        n.itemType = L"Playlist Action";
                        n.flowsFromIds = { 3050 };
                        n.flowsToIds = { 3052 };
                    });
                addNode(3052, L"Delete Playlist", L"Delete this playlist after confirmation", L"", ROLE_SYSTEM_PUSHBUTTON,
                    stateBits(false, false, false, false, false), screenRect(view.right - 130.0f * dp, view.top + 116.0f * dp, view.right, view.top + 152.0f * dp),
                    L"Delete", L"", [this]() { ShowDeletePlaylistModal(activePlaylistId); }, [playlist](AccessibilityNode& n) {
                        n.automationId = L"DeletePlaylistButton";
                        n.className = L"ApertureButton";
                        n.controlTypeId = UIA_ButtonControlTypeId;
                        n.itemType = L"Playlist Action";
                        n.flowsFromIds = { 3051 };
                        n.flowsToIds = (playlist && playlist->IsSmart()) ? std::vector<LONG>{ 3055 } : std::vector<LONG>{ 3053 };
                    });
                if (playlist->IsSmart()) {
                    addNode(3055, L"Edit Smart Rules", L"Modify filter rules for this smart playlist", L"", ROLE_SYSTEM_PUSHBUTTON,
                        stateBits(false, false, false, false, false), screenRect(view.right - 270.0f * dp, view.top + 116.0f * dp, view.right - 138.0f * dp, view.top + 152.0f * dp),
                        L"Edit", L"", [this]() { ShowEditSmartPlaylistModal(activePlaylistId); }, [](AccessibilityNode& n) {
                            n.automationId = L"EditSmartRulesButton";
                            n.className = L"ApertureButton";
                            n.controlTypeId = UIA_ButtonControlTypeId;
                            n.itemType = L"Playlist Action";
                            n.flowsFromIds = { 3052 };
                            n.flowsToIds = { 3053 };
                        });
                }
                addNode(3053, L"Search Playlist", L"Filter playlist items by title or filename", Text16ToWideString(playlistSearchInput.GetText()), ROLE_SYSTEM_TEXT,
                    stateBits(false, activeTextInputField == TextInputField::PlaylistSearch, false, false, false), screenRect(view.left + railW + 16.0f * dp, view.top + 178.0f * dp, view.right - 126.0f * dp, view.top + 214.0f * dp),
                    L"Edit", L"", [this]() { FocusTextInput(TextInputField::PlaylistSearch); activeFocusPane = FocusPane::PlaylistDetail; },
                    [&](AccessibilityNode& n) {
                        n.automationId = L"PlaylistSearchInput";
                        n.className = L"ApertureTextBox";
                        n.controlTypeId = UIA_EditControlTypeId;
                        n.itemType = L"Playlist Search";
                        n.ariaRole = L"searchbox";
                        n.flowsToIds = { 3054 };
                        n.hasValue = true;
                        n.isReadOnly = false;
                        n.onSetValue = [this](LONG, const std::wstring& val) {
                            playlistSearchInput.SetText(WideStringToText16(val));
                            MarkAccessibilityDirty();
                            InvalidateRect(hWnd, NULL, FALSE);
                        };
                    });
                addNode(3054, playlistGridView ? L"List View" : L"Grid View", L"Toggle playlist layout", playlistGridView ? L"Grid" : L"List", ROLE_SYSTEM_PUSHBUTTON,
                    stateBits(false, false, false, false, playlistGridView), screenRect(view.right - 116.0f * dp, view.top + 178.0f * dp, view.right, view.top + 214.0f * dp),
                    L"Toggle", L"", [this]() { playlistGridView = !playlistGridView; settings.playlistViewMode = playlistGridView ? "grid" : "list"; SaveSettings(); InvalidateRect(hWnd, NULL, FALSE); },
                    [&](AccessibilityNode& n) {
                        n.automationId = L"PlaylistLayoutToggleButton";
                        n.className = L"ApertureButton";
                        n.controlTypeId = UIA_ButtonControlTypeId;
                        n.itemType = L"Playlist Action";
                        n.flowsFromIds = { 3053 };
                        if (!hasItems && playlist && !playlist->IsSmart()) { n.flowsToIds = {3056}; }
                        n.hasToggle = true;
                        n.toggleState = playlistGridView ? ToggleState_On : ToggleState_Off;
                        n.onToggle = [this](LONG, ToggleState) {
                            playlistGridView = !playlistGridView;
                            settings.playlistViewMode = playlistGridView ? "grid" : "list";
                            SaveSettings();
                            InvalidateRect(hWnd, NULL, FALSE);
                        };
                    });
                if (!hasItems && !playlist->IsSmart()) {
                    const float detailLeft = view.left + railW + 16.0f * dp;
                    const float paneRight = std::max(detailLeft + 120.0f * dp, view.right - 26.0f * dp);
                    const float dropTop = view.top + 14.0f * dp + 50.0f * dp + 16.0f * dp;
                    const float centerX = (detailLeft + paneRight) * 0.5f;
                    const float addFilesW = 118.0f * dp, addFilesH = 34.0f * dp;
                    addNode(3056, L"Add Files", L"Add one or more media files to this playlist", L"", ROLE_SYSTEM_PUSHBUTTON,
                        stateBits(false, false, false, false, false), screenRect(centerX - addFilesW * 0.5f, dropTop + 168.0f * dp,
                                   centerX + addFilesW * 0.5f, dropTop + 168.0f * dp + addFilesH),
                        L"Add", L"", [this]() { OpenFileDialogForPlaylist(activePlaylistId); }, [this](AccessibilityNode& n) {
                            n.automationId = L"PlaylistAddFilesButton";
                            n.className = L"ApertureButton";
                            n.controlTypeId = UIA_ButtonControlTypeId;
                            n.itemType = L"Playlist Action";
                            n.controlledPeerIds = { AccessibilityStableId(3001, Utf8ToUtf16(activePlaylistId)) };
                            n.flowsFromIds = { 3054 };
                        });
                }
                const auto& visible = GetVisiblePlaylistIndices();
                const float listTop = view.top + 252.0f * dp, detailLeft = view.left + railW + 16.0f * dp, detailRight = view.right;
                const float paneW = std::max(120.0f * dp, detailRight - detailLeft - 26.0f * dp);
                const float itemGap = playlistGridView ? 10.0f * dp : 6.0f * dp;
                const size_t gridColumns = playlistGridView ? (size_t)std::max(1.0f, std::floor((paneW + itemGap) / (190.0f * dp))) : 1;
                const float gridW = playlistGridView ? std::max(120.0f * dp, (paneW - (gridColumns - 1) * itemGap) / (float)gridColumns) : paneW;
                const float rowH = playlistGridView ? 166.0f * dp : 62.0f * dp, rowPitch = rowH + itemGap;
                float detailMax = GetMaxScrollForPane(AutoscrollPane::PlaylistDetail);
                addNode(3060, playlist->name.empty() ? L"Playlist Items" : (Utf8ToUtf16(playlist->name) + L" Items"),
                    L"Playlist item list", std::to_wstring(playlist->itemPaths.size()) + L" items",
                    ROLE_SYSTEM_LIST, stateBits(false, false, false, false, false), screenRect(detailLeft, listTop, detailRight, view.bottom),
                    L"", L"", nullptr, [&](AccessibilityNode& n) {
                        n.automationId = L"PlaylistDetailList";
                        n.className = L"ApertureList";
                        n.controlTypeId = UIA_ListControlTypeId;
                        n.itemType = L"Playlist Detail Content";
                        n.groupId = 101;
                        n.selectionContainerId = 101;
                        n.hasSelection = true;
                        n.canSelectMultiple = false;
                        n.isSelectionRequired = false;
                        n.onGetSelection = [this, playlist](LONG) -> std::vector<LONG> {
                            std::vector<LONG> result;
                            for (size_t selIdx : selectedEpisodeIndices) {
                                if (selIdx < playlist->itemPaths.size())
                                    result.push_back(AccessibilityStableId(3200, playlist->itemPaths[selIdx]));
                            }
                            return result;
                        };
                        if (detailMax > 0.5f) {
                            float detailContentBottom = 14.0f * dp + 50.0f * dp + 16.0f * dp + 112.0f * dp + 16.0f * dp + 32.0f * dp + 12.0f * dp;
                            size_t visibleCount = GetVisiblePlaylistIndices().size();
                            if (playlistGridView) {
                                size_t rows = (visibleCount + gridColumns - 1) / gridColumns;
                                detailContentBottom += rows * (166.0f * dp + 10.0f * dp) - (visibleCount ? 10.0f * dp : 0.0f);
                            } else { detailContentBottom += visibleCount * (62.0f * dp + 6.0f * dp) - (visibleCount ? 6.0f * dp : 0.0f); }
                            detailContentBottom += 40.0f * dp;
                            float detailViewH = std::max(1.0f * dp, (winH - padY) - viewTop);
                            n.hasScroll = true;
                            n.verticallyScrollable = true;
                            n.verticalScrollPercent = std::clamp(static_cast<double>((playlistDetailScrollY / detailMax) * 100.0), 0.0, 100.0);
                            n.verticalViewSize = (detailContentBottom > 0.0f) ? std::clamp(static_cast<double>((detailViewH / detailContentBottom) * 100.0), 0.0, 100.0) : 100.0;
                            n.horizontallyScrollable = false;
                            n.horizontalScrollPercent = UIA_ScrollPatternNoScroll;
                            n.horizontalViewSize = 100.0;
                            n.onScroll = [this, detailMax, detailViewH, dp](LONG, ScrollAmount, ScrollAmount vAmount) {
                                if (vAmount == ScrollAmount_NoAmount) return;
                                float lineStep = (playlistGridView ? 176.0f : 68.0f) * dp;
                                float pageStep = std::max(lineStep, detailViewH - 20.0f * dp);
                                if (vAmount == ScrollAmount_LargeIncrement) {
                                    playlistDetailScrollY += pageStep;
                                } else if (vAmount == ScrollAmount_SmallIncrement) {
                                    playlistDetailScrollY += lineStep;
                                } else if (vAmount == ScrollAmount_LargeDecrement) {
                                    playlistDetailScrollY -= pageStep;
                                } else if (vAmount == ScrollAmount_SmallDecrement) { playlistDetailScrollY -= lineStep; }
                                ClampScrollOffsets();
                                InvalidateRect(hWnd, NULL, FALSE);
                                MarkAccessibilityDirty();
                            };
                            n.onSetScrollPercent = [this, detailMax](LONG, double, double vPercent) {
                                if (vPercent != UIA_ScrollPatternNoScroll) {
                                    playlistDetailScrollY = static_cast<float>((std::clamp(vPercent, 0.0, 100.0) / 100.0) * detailMax);
                                    ClampScrollOffsets();
                                    InvalidateRect(hWnd, NULL, FALSE);
                                    MarkAccessibilityDirty();
                                }
                            };
                        }
                    });
                size_t firstPos = 0;
                size_t lastPos = visible.size();
                float viewportH = std::max(1.0f * dp, view.bottom - listTop);
                if (playlistGridView) {
                    int totalRows = static_cast<int>((visible.size() + gridColumns - 1) / gridColumns);
                    int firstRow = std::max<int>(0, static_cast<int>(std::floor((playlistDetailScrollY - rowH) / rowPitch)) - 1);
                    int lastRow = std::min<int>(totalRows - 1, static_cast<int>(std::ceil((playlistDetailScrollY + viewportH) / rowPitch)) + 1);
                    firstPos = static_cast<size_t>(firstRow * (int)gridColumns);
                    lastPos = std::min<size_t>(visible.size(), static_cast<size_t>((lastRow + 1) * (int)gridColumns));
                } else {
                    int firstRow = std::max<int>(0, static_cast<int>(std::floor((playlistDetailScrollY - rowH) / rowPitch)) - 2);
                    int lastRow = std::min<int>(static_cast<int>(visible.size()), static_cast<int>(std::ceil((playlistDetailScrollY + viewportH) / rowPitch)) + 3);
                    firstPos = static_cast<size_t>(firstRow);
                    lastPos = std::min<size_t>(visible.size(), static_cast<size_t>(lastRow));
                }
                for (size_t visiblePos = firstPos; visiblePos < lastPos; ++visiblePos) {
                    const size_t index = visible[visiblePos];
                    const float rowY = listTop - playlistDetailScrollY + (playlistGridView ? (visiblePos / gridColumns) * rowPitch : visiblePos * rowPitch);
                    const float colX = playlistGridView ? detailLeft + (visiblePos % gridColumns) * (gridW + itemGap) : detailLeft;
                    const RECT bounds = screenRect(colX, rowY, playlistGridView ? colX + gridW : detailRight, rowY + rowH);
                    if (intersects(bounds) && index < playlist->itemPaths.size()) {
                        const std::wstring path = playlist->itemPaths[index];
                        const MediaItem* item = library.FindByPath(path);
                        const std::wstring name = item ? item->title : GetFileNameFromPath(path);
                        const bool selected = selectedEpisodeIndices.count(index) != 0;
                        addNode(AccessibilityStableId(3200, path), name, path, item ? Utf8ToUtf16(item->status) : L"", ROLE_SYSTEM_LISTITEM,
                            stateBits(selected, activeFocusPane == FocusPane::PlaylistDetail && keyboardFocusedIndex == static_cast<int>(index), false, false, selected), bounds,
                            L"Play", L"Enter", [this, path]() { OpenAndPlayMedia(path); }, [&](AccessibilityNode& n) {
                                n.automationId = L"PlaylistDetailItem_" + std::to_wstring(n.id);
                                n.className = L"ApertureListItem";
                                n.controlTypeId = UIA_ListItemControlTypeId;
                                n.itemType = L"Playlist Track";
                                if (item) n.itemStatus = Utf8ToUtf16(item->status);
                                n.groupId = 3060;
                                n.selectionContainerId = 3060;
                                n.controlledPeerIds = { 3053, AccessibilityStableId(3001, Utf8ToUtf16(activePlaylistId)) };
                                n.positionInSet = static_cast<int>(visiblePos + 1);
                                n.sizeOfSet = static_cast<int>(visible.size());
                                n.hasSelectionItem = true;
                                n.selected = selected;
                                n.onSelect = [this, index](LONG) {
                                    activeFocusPane = FocusPane::PlaylistDetail;
                                    keyboardFocusedIndex = static_cast<int>(index);
                                    selectedEpisodeIndices.clear();
                                    selectedEpisodeIndices.insert(index);
                                    InvalidateRect(hWnd, NULL, FALSE);
                                };
                                n.hasScrollItem = true;
                                n.onScrollIntoView = [this, index](LONG) {
                                    activeFocusPane = FocusPane::PlaylistDetail;
                                    keyboardFocusedIndex = static_cast<int>(index);
                                    selectedEpisodeIndices.clear();
                                    selectedEpisodeIndices.insert(index);
                                    AutoScrollToFocusedItem();
                                    ClampScrollOffsets();
                                    InvalidateRect(hWnd, NULL, FALSE);
                                    MarkAccessibilityDirty();
                                };
                            });
                    }
                }
            }
        } else if (currentTab == TabView::History) {
            // --- 8. HISTORY TAB ---
            if (library.history.empty()) {
                const float cardW = std::min(520.0f * dp, RectWidth(view) - 48.0f * dp);
                const D2D1_RECT_F card = D2D1::RectF( view.left + (RectWidth(view) - cardW) * 0.5f, view.top + 70.0f * dp,
                    view.left + (RectWidth(view) + cardW) * 0.5f, view.top + 290.0f * dp);
                addNode(4080, L"No watch history yet", L"Play something and it will appear here.", L"", ROLE_SYSTEM_GROUPING,
                    stateBits(false, false, false, false, false), screenRect(card.left, card.top, card.right, card.bottom),
                    L"", L"", nullptr, [](AccessibilityNode& n) {
                        n.automationId = L"HistoryEmptyState_Card";
                        n.className = L"ApertureEmptyState";
                        n.controlTypeId = UIA_GroupControlTypeId;
                        n.itemType = L"Empty State";
                        n.controllerForIds = { 4081 };
                        n.flowsToIds = { 4081 };
                    });
                const float buttonW = 128.0f * dp;
                const float left = view.left + (RectWidth(view) - buttonW) * 0.5f;
                addNode(4081, L"Open File", L"Choose media to play", L"", ROLE_SYSTEM_PUSHBUTTON,
                    stateBits(false, activeFocusPane == FocusPane::HistoryList && keyboardFocusedIndex == 0, false, false, false),
                    screenRect(left, card.bottom - 58.0f * dp, left + buttonW, card.bottom - 20.0f * dp),
                    L"Activate", L"Ctrl+O", [this]() { OpenFileDialog(); }, [](AccessibilityNode& n) {
                        n.automationId = L"HistoryEmptyState_OpenFile";
                        n.className = L"ApertureButton";
                        n.controlTypeId = UIA_ButtonControlTypeId;
                        n.itemType = L"Empty State Action";
                        n.describedByIds = { 4080 };
                        n.groupId = 4080;
                        n.controlledPeerIds = { 4080 };
                        n.flowsFromIds = { 4080 };
                    });
            } else {
                const HistoryTimelineProjection& proj = GetHistoryProjection();
                float headerY = view.top + kHistoryContentInset * dp - historyScrollY;
                for (size_t g = 0; g < proj.groups.size(); ++g) {
                    const auto& grp = proj.groups[g];
                    const HistoryGroupLayout layout(headerY, grp.count, dp);
                    headerY = layout.NextHeaderTop();
                    const RECT grpBounds = screenRect(view.left + 12.0f * dp, layout.headerTop,
                        view.right - 24.0f * dp, layout.headerTop + layout.headerHeight);
                    if (intersects(grpBounds)) {
                        addNode(4080 + static_cast<int>(grp.group) * 2, grp.title, grp.title + L" (" + grp.countText + L")", L"", ROLE_SYSTEM_GROUPING,
                            stateBits(false, false, false, false, false), grpBounds, L"", L"", nullptr, [&, g](AccessibilityNode& n) {
                                n.automationId = L"HistoryGroup_" + std::wstring(grp.title);
                                n.className = L"ApertureGroupHeader";
                                n.controlTypeId = UIA_GroupControlTypeId;
                                n.itemType = L"History Group Header";
                                n.groupId = 102;
                                std::vector<LONG> grpItemIds;
                                grpItemIds.reserve(grp.count);
                                for (size_t itmIdx = grp.startIndex; itmIdx < grp.startIndex + grp.count; ++itmIdx) {
                                    if (itmIdx < proj.itemIndices.size() && proj.itemIndices[itmIdx] < library.history.size())
                                        grpItemIds.push_back(AccessibilityStableId(4000, library.history[proj.itemIndices[itmIdx]].filePath));
                                }
                                n.controllerForIds = grpItemIds;
                                if (!grpItemIds.empty()) { n.flowsToIds = {grpItemIds.front()}; }
                            });
                    }
                    // Keep a pixel of margin for screenRect's integer rounding;
                    // intersects() remains the final accessibility clip check.
                    const auto [firstRow, lastRow] = layout.VisibleRows(viewTop - 1.0f, winH + 1.0f);
                    for (size_t row = firstRow; row < lastRow; ++row) {
                        const size_t idx = grp.startIndex + row;
                        size_t origIdx = proj.itemIndices[idx];
                        if (origIdx >= library.history.size()) continue;
                        const auto& record = library.history[origIdx];
                        const float y = layout.RowTop(row);
                        const RECT bounds = screenRect(view.left + 12.0f * dp, y, view.right - 24.0f * dp, y + layout.rowHeight);
                        if (intersects(bounds)) {
                            const bool selected = selectedHistoryIndices.count(origIdx) != 0;
                            std::wstring itemTitle = record.title.empty() ? GetFileNameFromPath(record.filePath) : record.title;
                            std::wstring itemDesc = grp.title + L" \x2022 " + FormatHistoryProgress(record);
                            addNode(AccessibilityStableId(4000, record.filePath), itemTitle,
                                record.filePath, itemDesc, ROLE_SYSTEM_LISTITEM,
                                stateBits(selected, activeFocusPane == FocusPane::HistoryList && keyboardFocusedIndex == static_cast<int>(origIdx), false, false, selected), bounds,
                                L"Resume", L"Enter", [this, path = record.filePath, pos = record.lastPosSec]() { OpenAndPlayMedia(path, pos); },
                                [&](AccessibilityNode& n) {
                                    n.automationId = L"HistoryItem_" + std::to_wstring(n.id);
                                    n.className = L"ApertureListItem";
                                    n.controlTypeId = UIA_ListItemControlTypeId;
                                    n.itemType = L"History Item";
                                    n.groupId = 4080 + static_cast<int>(grp.group) * 2;
                                    n.describedByIds = { 4080 + static_cast<int>(grp.group) * 2 };
                                    n.controlledPeerIds = { 4080 + static_cast<int>(grp.group) * 2 };
                                    n.selectionContainerId = 102;
                                    n.positionInSet = static_cast<int>(idx - grp.startIndex + 1);
                                    n.sizeOfSet = static_cast<int>(grp.count);
                                    n.hasSelectionItem = true;
                                    n.selected = selected;
                                    n.onSelect = [this, origIdx](LONG) {
                                        selectedHistoryIndices.clear();
                                        selectedHistoryIndices.insert(origIdx);
                                        InvalidateRect(hWnd, NULL, FALSE);
                                    };
                                    n.hasScrollItem = true;
                                    n.onScrollIntoView = [this, origIdx](LONG) {
                                        activeFocusPane = FocusPane::HistoryList;
                                        keyboardFocusedIndex = static_cast<int>(origIdx);
                                        selectedHistoryIndices.clear();
                                        selectedHistoryIndices.insert(origIdx);
                                        AutoScrollToFocusedItem();
                                        ClampScrollOffsets();
                                        InvalidateRect(hWnd, NULL, FALSE);
                                        MarkAccessibilityDirty();
                                    };
                                });
                        }
                    }
                }
            }
            D2D1_SIZE_F clearSize = renderer.MeasureString(L"Clear History", FontType::Medium);
            float clearW = clearSize.width + 24.0f * dp, clearH = 32.0f * dp;
            float clearTop = view.top + (46.0f * dp - clearH) * 0.5f;
            addNode(4090, L"Clear Watch History", L"Erase watch history", L"", ROLE_SYSTEM_PUSHBUTTON, stateBits(false, isClearHistoryHovered, false, false, false),
                screenRect(view.right - clearW - 6.0f * dp, clearTop, view.right - 6.0f * dp, clearTop + clearH), L"Clear", L"", [this]() {
                    ShowConfirmationModal(L"Clear Watch History", L"Erase all watch history and playback progress? You can undo this afterward.", L"Clear History", [this]() {
                        ArmLibraryUndo(L"History clear");
                        library.ClearHistory();
                        SaveLibrary();
                        selectedHistoryIndices.clear();
                        isLastWatchedCleared = true;
                        InvalidateMediaLayoutCache();
                        InvalidateRect(hWnd, NULL, FALSE);
                    });
                }, [](AccessibilityNode& n) {
                    n.automationId = L"ClearHistoryButton";
                    n.className = L"ApertureButton";
                    n.controlTypeId = UIA_ButtonControlTypeId;
                    n.itemType = L"History Action";
                });
        } else {
            // --- 9. SETTINGS TAB ---
            const float stripTop = view.top + SettingsLayout::kSidebarTopInset * dp, stripH = SettingsLayout::kSidebarItemHeight * dp;
            const float sideW = SettingsLayout::kSidebarWidth * dp;
            const wchar_t* cats[] = { L"Appearance", L"Playback", L"Audio & Subtitles", L"Shortcuts", L"System" };
            for (int i = 0; i < 5; ++i) {
                const float catTop = stripTop + i * (stripH + SettingsLayout::kSidebarItemGap * dp);
                addNode(500 + i, cats[i], L"Settings category", L"", ROLE_SYSTEM_PAGETAB,
                    stateBits(static_cast<int>(currentSettingsCat) == i, activeFocusPane == FocusPane::SettingsCategoryList && keyboardFocusedIndex == i, false, false, static_cast<int>(currentSettingsCat) == i),
                    screenRect(view.left + 8.0f * dp, catTop, view.left + sideW - 8.0f * dp, catTop + stripH), L"Open", L"",
                    [this, i]() { currentSettingsCat = static_cast<SettingsCategory>(i); currentTab = TabView::Settings; settingsScrollY = 0.0f; },
                    [&](AccessibilityNode& n) {
                        n.automationId = L"SettingsCategory_" + std::wstring(cats[i]);
                        n.className = L"ApertureTabItem";
                        n.controlTypeId = UIA_TabItemControlTypeId;
                        n.itemType = L"Settings Category";
                        n.positionInSet = i + 1;
                        n.sizeOfSet = 5;
                        n.selectionContainerId = 103;
                        n.flowsFromIds = (i > 0) ? std::vector<LONG>{ static_cast<LONG>(500 + i - 1) } : std::vector<LONG>{};
                        n.flowsToIds = (i < 4) ? std::vector<LONG>{ static_cast<LONG>(500 + i + 1) } : std::vector<LONG>{};
                        n.hasSelectionItem = true;
                        n.selected = (static_cast<int>(currentSettingsCat) == i);
                        n.onSelect = [this, i](LONG) {
                            currentSettingsCat = static_cast<SettingsCategory>(i);
                            currentTab = TabView::Settings;
                            settingsScrollY = 0.0f;
                            InvalidateRect(hWnd, NULL, FALSE);
                        };
                        if (static_cast<int>(currentSettingsCat) == i) {
                            float settingsMax = GetMaxScrollForPane(AutoscrollPane::Settings);
                            if (settingsMax > 0.5f) {
                                float viewH = std::max(1.0f * dp, (winH - padY) - viewTop);
                                float settingsContentH = GetSettingsContentHeight(dp);
                                ConfigureAccessibleScroll(n, &App::settingsScrollY, settingsMax, viewH, settingsContentH, 54.0f, dp);
                            }
                        }
                    });
            }
            const float rowTop = view.top + (SettingsLayout::kContentStartOffset +
                (currentSettingsCat == SettingsCategory::Subtitles ? SettingsLayout::kSubtitlePageHeaderReserve : SettingsLayout::kPageHeaderReserve)) * dp - settingsScrollY;
            std::vector<std::pair<std::wstring, std::wstring>> rows;
            if (currentSettingsCat == SettingsCategory::Appearance) {
                rows = {{Loc::Tr("settings.appearance.theme_palette"), Utf8ToUtf16(settings.themePreset)},
                        {Loc::Tr("settings.appearance.display_language"), Utf8ToUtf16(settings.appLanguage)}};
            } else if (currentSettingsCat == SettingsCategory::Playback) {
                rows = {{Loc::Tr("settings.playback.autoplay"), settings.autoplay ? L"On" : L"Off"},
                    { L"Remember Last Frame & Position", settings.rememberPos ? L"On" : L"Off" },
                    { L"Hardware Accelerated Video Decoding", settings.hwAccel ? L"On" : L"Off" },
                    { L"Loop Playback", settings.loop ? L"On" : L"Off" },
                    { L"Arrow Keys Seek Step", std::to_wstring(static_cast<int>(settings.arrowSeekStepSec)) + L" sec" },
                    { L"Frame Step Size", std::to_wstring(settings.ctrlSeekFrames) + (settings.ctrlSeekFrames == 1 ? L" frame" : L" frames") },
                    { L"Default Playback Speed", std::to_wstring(settings.defaultSpeed) + L"x" },
                    { L"Enable Custom MPV Configuration", settings.customMpvConfigEnabled ? L"On" : L"Off" },
                    {L"Custom MPV Configuration Actions", L"Open / Reset / Revert"}};
            } else if (currentSettingsCat == SettingsCategory::Subtitles) {
                rows = {{L"Preferred Audio Track", Utf8ToUtf16(settings.audioLang)},
                    { L"Preferred Subtitle Track", Utf8ToUtf16(settings.subLang) },
                    { L"Subtitle Display Font Size", Utf8ToUtf16(settings.subFontSize) },
                    { L"Subtitle Text Color", Utf8ToUtf16(settings.subColorHex) },
                    { L"Subtitle Background & Box Style", Utf8ToUtf16(settings.subBgStyle) },
                    {L"Subtitle Screen Position", Utf8ToUtf16(settings.subPosition)}};
            } else if (currentSettingsCat == SettingsCategory::Shortcuts) {
                rows.push_back({ L"Reset Shortcuts", L"Restore defaults" });
                for (const auto& pair : settings.GetShortcutActionListRef()) {
                    rows.push_back({ Utf8ToUtf16(pair.second), shortcut(pair.first) });
                }
            } else {
                rows = {
                    { Loc::Tr("settings.system.check_updates"), L"Update" },
                    { Loc::Tr("settings.system.hover_previews"), settings.hoverPreviewEnabled ? L"On" : L"Off" },
                    { Loc::Tr("settings.system.reduce_motion"), settings.reduceMotionEnabled ? L"On" : L"Windows setting" },
                    { Loc::Tr("settings.system.smart_series"), settings.smartSeriesDetection ? L"On" : L"Off" },
                    { Loc::Tr("settings.system.open_data"), L"Open" },
                    { Loc::Tr("settings.system.clear_disk_cache"), L"Clear" },
                    { Loc::Tr("settings.system.clear_history"), L"Clear" },
                    { Loc::Tr("settings.system.reset_defaults"), L"Reset" }
                };
            }
            for (size_t i = 0; i < rows.size(); ++i) {
                RECT bounds{};
                if (currentSettingsCat == SettingsCategory::System) {
                    float sysRowTop = rowTop + SettingsLayout::kSystemActionsTop * dp;
                    bounds = screenRect(view.left + (SettingsLayout::kSidebarWidth + 16.0f + SettingsLayout::kContentPadX) * dp, sysRowTop + i * (58.0f + 10.0f) * dp, view.right - SettingsLayout::kContentPadX * dp, sysRowTop + i * (58.0f + 10.0f) * dp + 58.0f * dp);
                } else if (currentSettingsCat == SettingsCategory::Playback) {
                    const float cardLeft = view.left + SettingsLayout::kSidebarWidth * dp + 16.0f * dp + SettingsLayout::kContentPadX * dp - 10.0f * dp;
                    const float cardRight = view.right - SettingsLayout::kContentPadX * dp + 10.0f * dp;
                    const float contentLeft = cardLeft + 10.0f * dp, contentRight = cardRight - 10.0f * dp;
                    const float cardGap = SettingsLayout::kSectionCardGap * dp, titleH = SettingsLayout::kPlaybackCardTitleHeight * dp;
                    const float behaviorTop = rowTop;
                    if (i < 4) {
                        const float rowTopPx = behaviorTop + titleH + i * SettingsLayout::kPlaybackBehaviorRowHeight * dp;
                        bounds = screenRect(contentLeft, rowTopPx, contentRight,
                                            rowTopPx + SettingsLayout::kPlaybackBehaviorRowHeight * dp);
                    } else if (i == 4 || i == 5) {
                        const float cardTop = behaviorTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp +
                            (i == 5 ? SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap : 0.0f) + cardGap;
                        bounds = screenRect(cardLeft, cardTop, cardRight, cardTop + SettingsLayout::kPlaybackSimpleCardHeight * dp);
                    } else if (i == 6) {
                        const float cardTop = behaviorTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp +
                            2.0f * (SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap) + cardGap;
                        bounds = screenRect(cardLeft, cardTop, cardRight, cardTop + SettingsLayout::kPlaybackSpeedCardHeight * dp);
                    } else if (i == 7) {
                        const float cardTop = behaviorTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp +
                            2.0f * (SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap) +
                            SettingsLayout::kPlaybackSpeedCardHeight * dp + 2.0f * cardGap;
                        bounds = screenRect(cardLeft, cardTop + titleH, cardRight,
                                            cardTop + titleH + SettingsLayout::kPlaybackMpvSwitchHeight * dp);
                    } else {
                        const float cardTop = behaviorTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp +
                            2.0f * (SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap) +
                            SettingsLayout::kPlaybackSpeedCardHeight * dp + 2.0f * cardGap;
                        const float actionTop = cardTop + SettingsLayout::kPlaybackMpvActionTop * dp;
                        const float actionAreaH = 3.0f * SettingsLayout::kPlaybackMpvActionHeight * dp +
                            2.0f * SettingsLayout::kPlaybackMpvActionGap * dp;
                        bounds = screenRect(cardLeft, actionTop, cardRight, actionTop + actionAreaH);
                    }
                } else if (currentSettingsCat == SettingsCategory::Subtitles) {
                    const float cardGap = SettingsLayout::kSubtitleCardGap * dp;
                    const float cardHeights[] = {
                        SettingsLayout::kSubtitleTrackCardHeight * dp, SettingsLayout::kSubtitleTrackCardHeight * dp,
                        SettingsLayout::kSubtitleFontCardHeight * dp, SettingsLayout::kSubtitleColorCardHeight * dp,
                        SettingsLayout::kSubtitleStyleCardHeight * dp, SettingsLayout::kSubtitlePositionCardHeight * dp};
                    const int row = std::clamp(static_cast<int>(i), 0, 5);
                    float cardTop = rowTop + SettingsLayout::kSubtitlePreviewCardHeight * dp + cardGap;
                    for (int card = 0; card < row; ++card) cardTop += cardHeights[card] + cardGap;
                    bounds = screenRect(view.left + SettingsLayout::kSidebarWidth * dp + 16.0f * dp + SettingsLayout::kContentPadX * dp - 10.0f * dp,
                                        cardTop, view.right - SettingsLayout::kContentPadX * dp + 10.0f * dp, cardTop + cardHeights[row]);
                } else if (currentSettingsCat == SettingsCategory::Shortcuts && i == 0) {
                    bounds = screenRect(view.left + 20.0f * dp, rowTop - 2.0f * dp, view.right - 20.0f * dp, rowTop + 24.0f * dp);
                } else {
                    bounds = screenRect(view.left + 20.0f * dp, rowTop + 28.0f * dp + i * 54.0f * dp, view.right - 20.0f * dp, rowTop + 28.0f * dp + i * 54.0f * dp + 50.0f * dp);
                }
                if (!intersects(bounds)) continue;
                bool isFocused = false;
                if (currentSettingsCat == SettingsCategory::Shortcuts) {
                    if (i == 0) { isFocused = (activeFocusPane == FocusPane::SettingsContent && keyboardFocusedIndex == 0); } else {
                        auto scList = settings.GetShortcutActionListRef();
                        size_t midSc = (scList.size() + 1) / 2;
                        size_t scIdx = i - 1;
                        int expectedRow = (scIdx < midSc) ? static_cast<int>(scIdx + 1) : static_cast<int>(scIdx - midSc + 1);
                        int expectedSub = (scIdx < midSc) ? 0 : 1;
                        isFocused = (activeFocusPane == FocusPane::SettingsContent && keyboardFocusedIndex == expectedRow && keyboardFocusedSubIndex == expectedSub);
                    }
                } else { isFocused = (activeFocusPane == FocusPane::SettingsContent && keyboardFocusedIndex == static_cast<int>(i)); }
                std::function<void()> rowAction = [this, i]() {
                    activeFocusPane = FocusPane::SettingsContent;
                    if (currentSettingsCat == SettingsCategory::Shortcuts && i > 0) {
                        auto scList = settings.GetShortcutActionListRef();
                        size_t midSc = (scList.size() + 1) / 2;
                        size_t scIdx = i - 1;
                        if (scIdx < scList.size()) {
                            int expectedRow = (scIdx < midSc) ? static_cast<int>(scIdx + 1) : static_cast<int>(scIdx - midSc + 1);
                            int expectedSub = (scIdx < midSc) ? 0 : 1;
                            StartShortcutCapture(scList[scIdx].first, scList[scIdx].second, expectedRow, expectedSub);
                        }
                    } else { keyboardFocusedIndex = static_cast<int>(i); HandleGlobalKeyDown(VK_RETURN, 0); }
                };
                addNode(600 + static_cast<LONG>(static_cast<int>(currentSettingsCat) * 100 + i), rows[i].first,
                    (currentSettingsCat == SettingsCategory::Shortcuts && i > 0) ? (L"Shortcut for " + rows[i].first) : L"Settings option",
                    rows[i].second, ROLE_SYSTEM_LISTITEM, stateBits(false, isFocused, false, false, false), bounds,
                    (currentSettingsCat == SettingsCategory::Shortcuts && i > 0) ? L"Rebind" : L"Activate",
                    (currentSettingsCat == SettingsCategory::Shortcuts && i > 0) ? rows[i].second : L"Enter", rowAction,
                    [&](AccessibilityNode& n) {
                        n.automationId = L"SettingsRow_" + std::to_wstring(n.id);
                        n.className = L"ApertureSettingsRow";
                        n.controlTypeId = UIA_ListItemControlTypeId;
                        n.itemType = (currentSettingsCat == SettingsCategory::Shortcuts && i > 0) ? L"Shortcut Setting" : L"Setting Option";
                        if (currentSettingsCat == SettingsCategory::Shortcuts && i > 0) {
                            n.accessKey = rows[i].second;
                            n.keyboardShortcut = rows[i].second;
                            n.helpText = L"Press Enter to rebind " + rows[i].first + (rows[i].second.empty() ? L" (currently Unassigned)" : (L" (currently " + rows[i].second + L")"));
                        } else if (currentSettingsCat == SettingsCategory::System && i == 3) {
                            n.helpText = Loc::Tr("settings.system.smart_series_on");
                        }
                        n.selectionContainerId = 500 + static_cast<LONG>(currentSettingsCat);
                        n.controlledPeerIds = { 500 + static_cast<LONG>(currentSettingsCat) };
                        n.positionInSet = static_cast<int>(i + 1);
                        n.sizeOfSet = static_cast<int>(rows.size());
                        n.flowsFromIds = (i == 0) ? std::vector<LONG>{ static_cast<LONG>(500 + static_cast<int>(currentSettingsCat)) } : std::vector<LONG>{ static_cast<LONG>(600 + static_cast<int>(currentSettingsCat) * 100 + i - 1) };
                        if (i + 1 < rows.size()) {
                            n.flowsToIds = { static_cast<LONG>(600 + static_cast<int>(currentSettingsCat) * 100 + i + 1) };
                        }
                        if (currentSettingsCat == SettingsCategory::Playback && (i == 0 || i == 1 || i == 2 || i == 3 || i == 7)) {
                            n.hasToggle = true;
                            bool checked = (i == 0 ? settings.autoplay : (i == 1 ? settings.rememberPos : (i == 2 ? settings.hwAccel : (i == 3 ? settings.loop : settings.customMpvConfigEnabled))));
                            n.toggleState = checked ? ToggleState_On : ToggleState_Off;
                            n.onToggle = [this, i](LONG, ToggleState) {
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = static_cast<int>(i);
                                HandleGlobalKeyDown(VK_RETURN, 0);
                            };
                        }
                        if (currentSettingsCat == SettingsCategory::System && (i == 1 || i == 2 || i == 3)) {
                            n.hasToggle = true;
                            const bool checked = (i == 1 ? settings.hoverPreviewEnabled : (i == 2 ? settings.reduceMotionEnabled : settings.smartSeriesDetection));
                            n.toggleState = checked ? ToggleState_On : ToggleState_Off;
                            n.onToggle = [this, i](LONG, ToggleState) {
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = static_cast<int>(i);
                                HandleGlobalKeyDown(VK_RETURN, 0);
                            };
                        }
                        n.hasScrollItem = true;
                        n.onScrollIntoView = [this, i](LONG) {
                            activeFocusPane = FocusPane::SettingsContent;
                            if (currentSettingsCat == SettingsCategory::Shortcuts && i > 0) {
                                auto scList = settings.GetShortcutActionListRef();
                                size_t midSc = (scList.size() + 1) / 2;
                                size_t scIdx = i - 1;
                                keyboardFocusedIndex = (scIdx < midSc) ? static_cast<int>(scIdx + 1) : static_cast<int>(scIdx - midSc + 1);
                                keyboardFocusedSubIndex = (scIdx < midSc) ? 0 : 1;
                            } else { keyboardFocusedIndex = static_cast<int>(i); }
                            AutoScrollToFocusedItem();
                            ClampScrollOffsets();
                            InvalidateRect(hWnd, NULL, FALSE);
                            MarkAccessibilityDirty();
                        };
                    });
            }
        }
    }
    addStatusLiveRegion();
    accessibilityTree.ReplaceNodes(nodes);
}

} // namespace Aperture
