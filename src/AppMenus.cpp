#include "App.h"
#include "Localization.h"
#include "OnlineSubtitles.h"
#include "SplitIrisMenuLayout.h"

namespace Aperture {

void App::CloseContextMenu() {
    if (!isContextMenuOpen) return;
    isContextMenuOpen = false;
    hoveredContextMenuItem = -1;
    hoveredIrisShoulder = 0;
    activeIrisDialIndex = -1;
    irisApertureTarget = 0.06f;
    contextMenuClosing = true;
    AnimateVisual(contextMenuAnim, 0.0f, MotionTokens::kFast, MotionTokens::Standard());
    PopAndRestoreFocus(TransientSurfaceType::ContextMenu);
}

void App::CloseSettingsFlyout() {
    if (!isSettingsFlyoutOpen) return;
    HandlePlaybackSpeedDragMessage(WM_CAPTURECHANGED, 0);
    if (activeTextInputField == TextInputField::PlaybackSpeed) BlurTextInput();
    isSettingsFlyoutOpen = false;
    hoveredFlyoutOption = -1;
    flyoutClosing = true;
    AnimateVisual(flyoutAnim, 0.0f, MotionTokens::kToggle, MotionTokens::Spring());
    PopAndRestoreFocus(TransientSurfaceType::SettingsFlyout);
}

void App::PrepareContextMenu(float& x, float& y, float width, float dp, bool reposition) {
    if (isContextMenuOpen && !isSplitIrisMenu && !reposition) { x = contextMenuOrigin.x; y = contextMenuOrigin.y; }
    contextMenuPreferredWidth = width;
    RECT rc;
    GetClientRect(hWnd, &rc);
    float winW = (float)(rc.right - rc.left);
    float winH = (float)(rc.bottom - rc.top);
    float menuW = width * dp, totalH = 8.0f * dp;
    for (const auto& it : contextMenuItems) totalH += it.isSeparator ? 6.0f * dp : 32.0f * dp;
    x = std::clamp(x, 8.0f * dp, winW - menuW - 8.0f * dp);
    if (y + totalH > winH - 8.0f * dp) y = std::max(8.0f * dp, y - totalH);
    contextMenuOrigin = D2D1::Point2F(x, y);
    hoveredContextMenuItem = -1;
    hoveredIrisShoulder = 0;
    activeIrisDialIndex = -1;
    isSplitIrisMenu = false;
    isContextMenuOpen = true;
    contextMenuClosing = false;
}

void App::RepositionContextMenu(float x, float y) {
    if (!isContextMenuOpen) return;
    const float dp = playbackSession.IsOpen() ? overlayRenderer.GetDpiScale() : renderer.GetDpiScale();
    if (isSplitIrisMenu) {
        irisAnchorCursor = D2D1::Point2F(x, y);
        PrepareSplitIrisContextMenu(x, y, dp, false);
    } else PrepareContextMenu(x, y, contextMenuPreferredWidth, dp, true);
    UpdateContextMenuHitTarget(x, y, dp);
    isKeyboardNavActive = false;
    MarkAccessibilityDirty();
    if (playbackSession.IsOpen()) Render();
    else InvalidateRect(hWnd, nullptr, FALSE);
}

void App::PrepareSplitIrisContextMenu(float x, float y, float dp, bool resetIrisAnimation) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    float winW = (float)(rc.right - rc.left);
    float winH = (float)(rc.bottom - rc.top);
    const float halfW = SplitIrisMenuLayout::Center * dp;
    const float archH = SplitIrisMenuLayout::Center * dp;
    const float pad = 12.0f * dp;
    const float bodyH = (SplitIrisMenuLayout::Bottom(contextMenuItems) - SplitIrisMenuLayout::Center) * dp;
    if (resetIrisAnimation) {
        irisAnchorCursor = D2D1::Point2F(x, y);
    }
    float minCX = halfW + pad;
    float maxCX = std::max(minCX, winW - halfW - pad);
    float minCY = archH + pad;
    float maxCY = std::max(minCY, winH - bodyH - pad);
    float clampedX = std::clamp(irisAnchorCursor.x, minCX, maxCX);
    float clampedY = std::clamp(irisAnchorCursor.y, minCY, maxCY);
    contextMenuOrigin = D2D1::Point2F(clampedX - halfW, clampedY - archH);
    hoveredContextMenuItem = -1;
    hoveredIrisShoulder = 0;
    if (resetIrisAnimation) {
        activeIrisDialIndex = -1;
        irisApertureCurrent = 0.06f;
        irisApertureTarget = 0.22f;
    }
    isSplitIrisMenu = true;
    isContextMenuOpen = true;
    contextMenuClosing = false;
}

void App::SetIrisShoulders(IrisShoulderButton left, IrisShoulderButton right) {
    irisLeftShoulder = std::move(left);
    irisRightShoulder = std::move(right);
}

void App::SetActiveIrisDial(int index) {
    if (activeIrisDialIndex == index) return;
    const bool wasInactive = (activeIrisDialIndex == -1);
    activeIrisDialIndex = index;
    if (index < 0 || index >= 5) {
        irisApertureTarget = 0.22f;
        return;
    }
    static constexpr float kCenterDeg[5] = { -72.0f, -36.0f, 0.0f, 36.0f, 72.0f };
    irisApertureTarget = irisDialApertures[index];
    irisTargetWedgeAngle = kCenterDeg[index];
    if (wasInactive) {
        irisCurrentWedgeAngle = irisTargetWedgeAngle;
    }
}

void App::StepIrisDial(int index, int dir) {
    if (index < 0 || index >= 5 || dir == 0) return;
    switch (index) {
    case 0: {
        double cur = player.GetPlaybackRate();
        double next = std::clamp(std::round((cur + dir * 0.05) * 100.0) / 100.0, 0.25, 4.0);
        irisDialApertures[0] = std::clamp(irisDialApertures[0] + dir * 0.08f, 0.12f, 0.90f);
        irisApertureTarget = irisDialApertures[0];
        commandDispatcher.Execute(AppCommand::SetSpeed, CommandArgs{ next });
        break;
    }
    case 1: {
        settings.subDelayMs += dir * 50;
        player.SetSubDelay(static_cast<double>(settings.subDelayMs) / 1000.0);
        SaveSettings();
        irisDialApertures[1] = std::clamp(irisDialApertures[1] + dir * 0.08f, 0.12f, 0.90f);
        irisApertureTarget = irisDialApertures[1];
        wchar_t buf[64];
        swprintf_s(buf, L"Subtitle Delay: %+d ms", settings.subDelayMs);
        ShowOSD(IconType::Subtitles, buf);
        break;
    }
    case 2: {
        settings.audioDelayMs += dir * 25;
        player.SetAudioDelay(static_cast<double>(settings.audioDelayMs) / 1000.0);
        SaveSettings();
        irisDialApertures[2] = std::clamp(irisDialApertures[2] + dir * 0.08f, 0.12f, 0.90f);
        irisApertureTarget = irisDialApertures[2];
        wchar_t buf[64];
        swprintf_s(buf, L"Audio Delay: %+d ms", settings.audioDelayMs);
        ShowOSD(IconType::VolumeHigh, buf);
        break;
    }
    case 3: {
        static constexpr const char* kAspects[5] = { "auto", "16:9", "21:9", "4:3", "1:1" };
        static constexpr float kOpenings[5] = { 0.56f, 0.72f, 0.88f, 0.38f, 0.22f };
        int curIdx = 0;
        for (int i = 0; i < 5; ++i) {
            if (settings.lastAspectRatio == kAspects[i]) { curIdx = i; break; }
        }
        int nextIdx = (curIdx + dir + 5) % 5;
        irisDialApertures[3] = kOpenings[nextIdx];
        irisApertureTarget = irisDialApertures[3];
        CommandArgs args;
        args.stringVal = kAspects[nextIdx];
        commandDispatcher.Execute(AppCommand::SetAspectRatio, args);
        break;
    }
    case 4: {
        int curRot = player.GetVideoRotate();
        int nextRot = (curRot + dir * 90 + 360) % 360;
        player.SetVideoRotate(nextRot);
        float opening = 0.56f;
        if (nextRot == 90) opening = 0.72f;
        else if (nextRot == 180) opening = 0.88f;
        else if (nextRot == 270) opening = 0.32f;
        irisDialApertures[4] = opening;
        irisApertureTarget = opening;
        ShowOSD(IconType::AspectFrame, L"Rotation: " + std::to_wstring(nextRot) + L"\u00b0");
        break;
    }
    default: break;
    }
}

void App::ResetIrisDial(int index) {
    if (index < 0 || index >= 5) return;
    irisDialApertures[index] = 0.56f;
    irisApertureTarget = 0.56f;
    switch (index) {
    case 0:
        commandDispatcher.Execute(AppCommand::SpeedReset);
        break;
    case 1:
        commandDispatcher.Execute(AppCommand::SubDelayReset);
        break;
    case 2:
        commandDispatcher.Execute(AppCommand::AudioDelayReset);
        break;
    case 3: {
        CommandArgs args;
        args.stringVal = "auto";
        commandDispatcher.Execute(AppCommand::SetAspectRatio, args);
        break;
    }
    case 4:
        player.SetVideoRotate(0);
        ShowOSD(IconType::AspectFrame, L"Rotation: 0\u00b0");
        break;
    default: break;
    }
}

std::array<IrisDialSlotViewModel, 5> App::BuildIrisDialViewModels() const {
    std::array<IrisDialSlotViewModel, 5> dials{};
    const double spd = player.GetPlaybackRate();
    wchar_t spdBuf[32];
    if (((int)std::lround(spd * 100.0)) % 100 == 0)
        swprintf_s(spdBuf, L"%.1fx", spd);
    else
        swprintf_s(spdBuf, L"%.2fx", spd);
    dials[0] = { L"Speed", spdBuf, -72.0f, std::abs(spd - 1.0) > 0.005, activeIrisDialIndex == 0 };

    wchar_t subBuf[32];
    swprintf_s(subBuf, L"%+dms", settings.subDelayMs);
    dials[1] = { L"Sub Delay", subBuf, -36.0f, settings.subDelayMs != 0, activeIrisDialIndex == 1 };

    wchar_t audBuf[32];
    swprintf_s(audBuf, L"%+dms", settings.audioDelayMs);
    dials[2] = { L"Audio Delay", audBuf, 0.0f, settings.audioDelayMs != 0, activeIrisDialIndex == 2 };

    std::string asp = settings.lastAspectRatio.empty() ? "auto" : settings.lastAspectRatio;
    dials[3] = { L"Aspect", Utf8ToUtf16(asp), 36.0f, asp != "auto", activeIrisDialIndex == 3 };

    int rot = player.GetVideoRotate();
    dials[4] = { L"Rotate", std::to_wstring(rot) + L"\u00b0", 72.0f, rot != 0, activeIrisDialIndex == 4 };
    return dials;
}

void App::PopulateContextMenuViewModel(ContextMenuViewModel& out) const {
    out.isOpen = true;
    out.isSplitIris = isSplitIrisMenu;
    out.origin = contextMenuOrigin;
    out.items = contextMenuItems;
    out.hoveredIndex = hoveredContextMenuItem;
    out.animProgress = contextMenuAnim.current;
    if (isSplitIrisMenu) {
        out.dials = BuildIrisDialViewModels();
        out.activeDialIndex = activeIrisDialIndex;
        out.currentWedgeAngle = irisCurrentWedgeAngle;
        out.apertureOpenFraction = irisApertureCurrent;
        out.leftShoulder = irisLeftShoulder;
        out.rightShoulder = irisRightShoulder;
        out.hoveredShoulder = hoveredIrisShoulder;
    }
}

void App::UpdateContextMenuHitTarget(float mouseX, float mouseY, float dp) {
    hoveredContextMenuItem = -1;
    hoveredIrisShoulder = 0;
    if (!isContextMenuOpen) return;
    if (!isSplitIrisMenu) {
        float menuW = 220.0f * dp, itemH = 32.0f * dp, curY = contextMenuOrigin.y + 4.0f * dp;
        for (size_t i = 0; i < contextMenuItems.size(); ++i) {
            if (contextMenuItems[i].isSeparator) { curY += 6.0f * dp; continue; }
            if (mouseX >= contextMenuOrigin.x && mouseX <= contextMenuOrigin.x + menuW && mouseY >= curY && mouseY <= curY + itemH) {
                hoveredContextMenuItem = static_cast<int>(i);
                break;
            }
            curY += itemH;
        }
        return;
    }

    const auto pointer = SplitIrisMenuLayout::Unscale(mouseX, mouseY, contextMenuOrigin, dp, contextMenuAnim.current);
    mouseX = pointer.x;
    mouseY = pointer.y;
    const float cx = contextMenuOrigin.x + 119.0f * dp;
    const float cy = contextMenuOrigin.y + 119.0f * dp;
    const float dx = mouseX - cx;
    const float dy = mouseY - cy;
    const float dist = std::hypot(dx, dy);

    // 1. Top Semi-Circular Iris Arch (5 mouse-wheel wedges)
    if (dy <= 0.0f) {
        if (dist >= 38.0f * dp && dist <= 126.0f * dp) {
            const float deg = (std::atan2(dy, dx) * 180.0f / 3.1415926535f) + 90.0f;
            if (deg >= -92.0f && deg <= 92.0f) {
                int nextIdx = activeIrisDialIndex;
                static constexpr float kCenterDeg[5] = { -72.0f, -36.0f, 0.0f, 36.0f, 72.0f };
                if (activeIrisDialIndex < 0 || activeIrisDialIndex >= 5 ||
                    std::abs(deg - kCenterDeg[activeIrisDialIndex]) > 20.5f) {
                    const float clamped = std::clamp(deg, -89.9f, 89.9f);
                    nextIdx = std::clamp(static_cast<int>(std::floor((clamped + 90.0f) / 36.0f)), 0, 4);
                }
                SetActiveIrisDial(nextIdx);
                return;
            }
        }
        if (dist < 38.0f * dp || dist > 126.0f * dp) {
            SetActiveIrisDial(-1);
        }
        return;
    }

    SetActiveIrisDial(-1);

    // 2. Sculpted Shoulder Dock (Left & Right Contoured Buttons hugging r = 38dp iris chamber with r = 43dp clearance)
    const float xLocal = (mouseX - (contextMenuOrigin.x + 1.0f * dp)) / dp;
    const float yLocal = (mouseY - cy) / dp - 1.0f;
    const float distDp = std::hypot(xLocal - 118.0f, yLocal);
    if (yLocal >= 5.0f && yLocal <= 36.0f && distDp >= 43.0f) {
        if (xLocal >= 6.0f && xLocal <= 94.6f && !irisLeftShoulder.isDisabled) {
            hoveredIrisShoulder = 1;
            return;
        }
        if (xLocal >= 141.4f && xLocal <= 230.0f && !irisRightShoulder.isDisabled) {
            hoveredIrisShoulder = 2;
            return;
        }
    }

    // 3. Full-Width Menu Rows below the Shoulder Dock
    const float rowLeft = contextMenuOrigin.x + 5.0f * dp;
    const float rowRight = contextMenuOrigin.x + 233.0f * dp;
    if (mouseX < rowLeft || mouseX > rowRight) return;
    float curY = contextMenuOrigin.y + SplitIrisMenuLayout::RowsTop * dp;
    const float itemH = SplitIrisMenuLayout::RowHeight * dp;
    for (size_t i = 0; i < contextMenuItems.size(); ++i) {
        if (contextMenuItems[i].isSeparator) {
            curY += SplitIrisMenuLayout::SeparatorHeight * dp;
            continue;
        }
        if (mouseY >= curY && mouseY <= curY + itemH) {
            hoveredContextMenuItem = static_cast<int>(i);
            return;
        }
        curY += SplitIrisMenuLayout::RowPitch * dp;
    }
}

bool App::HandleContextMenuClick(float mouseX, float mouseY, float dp) {
    if (!isContextMenuOpen) return false;
    UpdateContextMenuHitTarget(mouseX, mouseY, dp);

    if (isSplitIrisMenu) {
        // 1. Clicking an active wedge in the top Iris Arch resets that dial & smoothly resets its aperture
        if (activeIrisDialIndex >= 0 && activeIrisDialIndex < 5) {
            ResetIrisDial(activeIrisDialIndex);
            return true;
        }
        // 2. Clicking Left or Right Sculpted Shoulder Button
        if (hoveredIrisShoulder == 1 || hoveredIrisShoulder == 2) {
            const IrisShoulderButton btn = (hoveredIrisShoulder == 1) ? irisLeftShoulder : irisRightShoulder;
            if (!btn.isDisabled) {
                auto cb = btn.onClick;
                const bool keep = btn.keepOpen;
                if (!keep) CloseContextMenu();
                if (cb) cb();
            }
            return true;
        }
        // 3. Clicking a Menu Row
        if (hoveredContextMenuItem >= 0 && static_cast<size_t>(hoveredContextMenuItem) < contextMenuItems.size()) {
            const ContextMenuItem item = contextMenuItems[hoveredContextMenuItem];
            if (!item.isDisabled && !item.isSeparator) {
                auto cb = item.onClick;
                const bool keep = item.keepOpen;
                if (!keep) CloseContextMenu();
                if (cb) cb();
            }
            return true;
        }
        // 4. Check if click was inside the inert body or center iris circle (keep open) vs outside (close)
        const auto pointer = SplitIrisMenuLayout::Unscale(mouseX, mouseY, contextMenuOrigin, dp, contextMenuAnim.current);
        mouseX = pointer.x;
        mouseY = pointer.y;
        const float cx = contextMenuOrigin.x + 119.0f * dp;
        const float cy = contextMenuOrigin.y + 119.0f * dp;
        const float dist = std::hypot(mouseX - cx, mouseY - cy);
        const float bodyBottom = contextMenuOrigin.y + SplitIrisMenuLayout::Bottom(contextMenuItems) * dp;
        const bool inArchOrIris = (dist <= 119.0f * dp && mouseY <= cy + 38.0f * dp);
        const bool inBody = (mouseX >= contextMenuOrigin.x && mouseX <= contextMenuOrigin.x + 238.0f * dp &&
                             mouseY >= cy && mouseY <= bodyBottom);
        if (!inArchOrIris && !inBody) {
            CloseContextMenu();
        }
        return true;
    }

    if (hoveredContextMenuItem >= 0 && static_cast<size_t>(hoveredContextMenuItem) < contextMenuItems.size() &&
        !contextMenuItems[hoveredContextMenuItem].isDisabled && !contextMenuItems[hoveredContextMenuItem].isSeparator) {
        auto cb = contextMenuItems[hoveredContextMenuItem].onClick;
        const bool keep = contextMenuItems[hoveredContextMenuItem].keepOpen;
        if (!keep) CloseContextMenu();
        if (cb) cb();
    } else {
        CloseContextMenu();
    }
    return true;
}

bool App::HandleContextMenuWheel(short delta) {
    if (!isContextMenuOpen || !isSplitIrisMenu || activeIrisDialIndex < 0 || activeIrisDialIndex >= 5) return false;
    const int dir = (delta > 0) ? 1 : -1;
    StepIrisDial(activeIrisDialIndex, dir);
    return true;
}

bool App::TickSplitIrisAnimation(float dt) {
    if ((!isContextMenuOpen && !contextMenuClosing) || !isSplitIrisMenu) return false;
    bool changed = false;
    const float clampedDt = std::clamp(dt, 0.001f, 0.05f);
    if (activeIrisDialIndex != -1 && std::abs(irisTargetWedgeAngle - irisCurrentWedgeAngle) > 0.05f) {
        const float angleAlpha = 1.0f - std::pow(1.0f - 0.36f, clampedDt * 60.0f);
        irisCurrentWedgeAngle += (irisTargetWedgeAngle - irisCurrentWedgeAngle) * angleAlpha;
        changed = true;
    }
    if (std::abs(irisApertureTarget - irisApertureCurrent) > 0.0008f) {
        const float apertureAlpha = 1.0f - std::pow(1.0f - 0.24f, clampedDt * 60.0f);
        irisApertureCurrent += (irisApertureTarget - irisApertureCurrent) * apertureAlpha;
        changed = true;
    }
    return changed;
}

void App::PresentHubContextMenu(float x, float y, float width) {
    PrepareContextMenu(x, y, width, renderer.GetDpiScale());
    contextMenuAnim.SnapTo(0.0f);
    AnimateVisual(contextMenuAnim, 1.0f, MotionTokens::kFast, MotionTokens::Standard());
    InvalidateRect(hWnd, NULL, FALSE);
}

void App::PresentPlayerContextMenu(float x, float y, float width, bool animate, PlayerMenuReveal reveal) {
    const float dp = overlayRenderer.GetDpiScale();
    const bool wasAlreadyOpenIris = isContextMenuOpen && isSplitIrisMenu;
    if (playbackSession.IsPiP()) {
        PrepareContextMenu(x, y, width, dp);
    } else {
        PrepareSplitIrisContextMenu(x, y, dp, !wasAlreadyOpenIris);
    }
    if (reveal != PlayerMenuReveal::None) { controlsOpacity = 1.0f; controlsIdleTimer = 0.0f; }
    if (reveal == PlayerMenuReveal::Cursor && isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
    if (!wasAlreadyOpenIris) {
        contextMenuAnim.SnapTo(animate ? 0.0f : 1.0f);
        if (animate) AnimateVisual(contextMenuAnim, 1.0f, MotionTokens::kFast, MotionTokens::Standard());
    }
    Render();
}

void App::OpenMediaSortContextMenu(float x, float y) {
    PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    const auto addField = [this](const char* id, const wchar_t* label, LibrarySortField field) {
        const bool effectiveChecked = librarySortField == field ||
            (field == LibrarySortField::Title && librarySortField == LibrarySortField::Manual && libraryGroupMode != LibraryGroupMode::Flat);
        contextMenuItems.push_back({ id, label, L"", IconType::Checkmark, false, effectiveChecked, false,
            [this, field]() { SetLibrarySort(field, librarySortDirection); } });
        contextMenuItems.back().showCheckedIndicator = true;
    };
    if (libraryGroupMode == LibraryGroupMode::Flat && settings.manualMediaOrder)
        addField("sort_manual", L"Manual", LibrarySortField::Manual);
    addField("sort_title", L"Title", LibrarySortField::Title);
    addField("sort_added", L"Date Added", LibrarySortField::DateAdded);
    addField("sort_last_played", L"Last Played", LibrarySortField::LastPlayed);
    addField("sort_duration", L"Duration", LibrarySortField::Duration);
    LibrarySortField effective = librarySortField;
    if (effective == LibrarySortField::Manual && libraryGroupMode != LibraryGroupMode::Flat) effective = LibrarySortField::Title;
    if (effective != LibrarySortField::Manual) {
        contextMenuItems.push_back({ "sort_sep", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({ "sort_ascending", L"Ascending", L"", IconType::Checkmark, false,
            librarySortDirection == SortDirection::Ascending, false,
            [this]() { SetLibrarySort(librarySortField, SortDirection::Ascending); } });
        contextMenuItems.back().showCheckedIndicator = true;
        contextMenuItems.push_back({ "sort_descending", L"Descending", L"", IconType::Checkmark, false,
            librarySortDirection == SortDirection::Descending, false,
            [this]() { SetLibrarySort(librarySortField, SortDirection::Descending); } });
        contextMenuItems.back().showCheckedIndicator = true;
    }
    contextMenuOrigin = D2D1::Point2F(x, y);
    hoveredContextMenuItem = -1;
    isContextMenuOpen = true;
    contextMenuClosing = false;
    contextMenuAnim.SnapTo(0.0f);
    AnimateVisual(contextMenuAnim, 1.0f, MotionTokens::kFast, MotionTokens::Standard());
    MarkAccessibilityDirty();
    InvalidateRect(hWnd, NULL, FALSE);
}

void App::UpdateFlyoutHitTarget(float mouseX, float mouseY, float winW, float winH, float dp, float bottomBarH) {
    hoveredFlyoutOption = PlaybackFlyoutLayout::None;
    if (!isSettingsFlyoutOpen) return;
    const auto bounds = D2D1::RectF(winW - 300.0f * dp, winH - bottomBarH - GetFlyoutHeight() - 10.0f * dp,
        winW - 20.0f * dp, winH - bottomBarH - 10.0f * dp);
    hoveredFlyoutOption = PlaybackFlyoutLayout::HitTest(currentFlyoutMenu, GetFlyoutOptionCount(), bounds, mouseX, mouseY, dp);
}

void App::OpenFolderContextMenu(int groupIndex, float x, float y) {
    const auto& proj = GetCurrentMediaProjection(true);
    if (groupIndex < 0 || groupIndex >= (int)proj.groups.size()) return;
    const auto grp = proj.groups[groupIndex];
    const std::wstring firstPath = grp.startIndex < proj.indices.size() ? library.mediaItems[proj.indices[grp.startIndex]].filePath : L"";
    if (selectedFolderKeys.count(grp.key) == 0) { selectedFolderKeys.clear(); selectedFolderKeys.insert(grp.key); }
    PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    const size_t selCount = selectedFolderKeys.size();
    const bool isMulti = selCount > 1;
    const bool isSeriesMode = (libraryGroupMode == LibraryGroupMode::Series);
    if (isMulti) {
        std::wstring titleStr = isSeriesMode ? (L"Remove " + std::to_wstring(selCount) + L" Series from Library...")
            : (L"Remove " + std::to_wstring(selCount) + L" Folders from Library...");
        contextMenuItems.push_back(
            {"remove_folders", titleStr, L"Del", IconType::Trash, false, false, true, [this]() { RemoveSelectedFolders(); }});
        if (!isSeriesMode) {
            contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
            contextMenuItems.push_back({"rediscover", L"Rediscover / Reconnect Missing Files", L"", IconType::Refresh, false, false, false,
                                        [this]() { RunFolderRediscovery(true); }});
        }
    } else {
        std::wstring playLabel = isSeriesMode ? (L"Play Series (" + std::to_wstring(grp.count) + L" Episodes)")
            : (L"Play All in Folder (" + std::to_wstring(grp.count) + L" Videos)");
        contextMenuItems.push_back({ "play_all", playLabel, L"Enter", IconType::Play, false, false, false, [this, key = grp.key]() {
            const auto& p = GetCurrentMediaProjection(true);
            for (const auto& g : p.groups) {
                if (g.key == key) {
                    for (size_t i = 0; i < g.count; ++i) {
                        size_t pIdx = g.startIndex + i;
                        if (pIdx < p.indices.size()) { OpenAndPlayMedia(library.mediaItems[p.indices[pIdx]].filePath); break; }
                    }
                    break;
                }
            }
        }});
        std::wstring revealPath = grp.key;
        if (isSeriesMode) {
            if (!firstPath.empty()) revealPath = ExtractDirectory(firstPath);
        }
        std::wstring revealLabel = isSeriesMode ? L"Open Series Folder in Explorer" : L"Open Folder in Explorer";
        contextMenuItems.push_back({"reveal", revealLabel, L"", IconType::FolderPlaylist, false, false, false,
                                    [this, revealPath]() { OpenInExplorer(revealPath); }});
        if (!isSeriesMode) {
            contextMenuItems.push_back({"rediscover", L"Rediscover / Reconnect Missing Files", L"", IconType::Refresh, false, false, false,
                                        [this]() { RunFolderRediscovery(true); }});
        }
        contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
        std::wstring removeLabel = isSeriesMode ? L"Remove Series from Library..." : L"Remove Folder from Library...";
        contextMenuItems.push_back(
            {"remove_folder", removeLabel, L"Del", IconType::Trash, false, false, true, [this]() { RemoveSelectedFolders(); }});
    }
    PresentHubContextMenu(x, y, 260.0f);
}

void App::OpenMediaContextMenu(const MediaItem& item, float x, float y) {
    PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    size_t selCount = selectedMediaIndices.size();
    bool isMulti = selCount > 1;
    if (isMulti) {
        contextMenuItems.push_back({"play_queue", Loc::Format("ux.queue_selected", std::to_wstring(selCount)), L"Enter", IconType::Play,
                                    false, false, false, [this]() { PlaySelectedInCurrentPane(); }});
        contextMenuItems.push_back({"mark_watched", L"Mark as watched (" + std::to_wstring(selCount) + L" Videos)", L"",
                                    IconType::Checkmark, false, false, false, [this]() { BulkSetWatched(true); }});
        contextMenuItems.push_back({"mark_unwatched", L"Mark as unwatched (" + std::to_wstring(selCount) + L" Videos)", L"",
                                    IconType::Refresh, false, false, false, [this]() { BulkSetWatched(false); }});
        contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
        for (const auto& pl : library.playlists) {
            if (pl.IsSmart()) continue;
            std::string plId = pl.id;
            std::wstring plName = Utf8ToUtf16(pl.name);
            contextMenuItems.push_back({"add_to_pl_" + plId, L"Add " + std::to_wstring(selCount) + L" items to " + plName, L"",
                                        IconType::FolderPlaylist, false, false, false, [this, plId]() { BulkAddToPlaylist(plId); }});
        }
        contextMenuItems.push_back({ "sep2", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({"remove_multi", L"Remove Selected (" + std::to_wstring(selCount) + L" Files)...", L"Del",
                                    IconType::Trash, false, false, true, [this]() { ConfirmDeleteSelectedMedia(); }});
    } else {
        contextMenuItems.push_back(
            {"play", L"Play", L"Enter", IconType::Play, false, false, false, [this, item]() { OpenAndPlayMedia(item.filePath); }});
        if (!FileExists(item.filePath)) {
            contextMenuItems.push_back({ "locate_missing", L"Locate Missing File\u2026", L"", IconType::FolderPlaylist,
                false, false, false, [this, item]() {
                    const std::wstring replacement = RelocateMissingMedia(item.filePath);
                    if (!replacement.empty()) {
                        ShowOSD(IconType::Checkmark, L"Missing media repaired");
                        InvalidateMediaLayoutCache();
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                }});
        }
        if (item.lastPosSec > 5.0 && item.durationSec > 0.0) {
            contextMenuItems.push_back({"resume", L"Resume (" + FormatDuration(item.lastPosSec) + L")", L"", IconType::Play, false, false,
                                        false, [this, item]() { OpenAndPlayMedia(item.filePath, item.lastPosSec); }});
        }
        contextMenuItems.push_back({ "play_queue", Loc::Tr("ux.queue_single"), L"", IconType::Next, false, false, false, [this, item]() {
            playbackSession.OpenQueue({ item.filePath }, 0);
            OpenAndPlayMedia(item.filePath, 0.0, true);
        }});
        const bool isWatched = (item.status == "done" || item.status == "watched");
        contextMenuItems.push_back({ "toggle_watched", isWatched ? L"Mark as unwatched" : L"Mark as watched", L"", IconType::Checkmark, false, false, false, [this, item, isWatched]() {
            SetWatchedWithUndo({ item.filePath }, !isWatched);
            SaveLibrary();
            ShowOSD(IconType::Checkmark, isWatched ? L"Marked as unwatched" : L"Marked as watched");
            InvalidateRect(hWnd, NULL, FALSE);
        }});
        contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
        for (const auto& pl : library.playlists) {
            if (pl.IsSmart()) continue;
            std::string plId = pl.id;
            std::wstring plName = Utf8ToUtf16(pl.name);
            contextMenuItems.push_back({ "add_to_pl_" + plId, L"Add to " + plName, L"", IconType::FolderPlaylist, false, false, false, [this, plId, plName, item]() {
                size_t skipped = 0;
                size_t added = AddToPlaylistWithUndo(plId, { item.filePath }, &skipped);
                SaveLibrary();
                if (added > 0) {
                    ShowOSD(IconType::FolderPlaylist, L"Added to " + plName);
                } else { ShowOSD(IconType::FolderPlaylist, L"Item already in " + plName); }
            }});
        }
        contextMenuItems.push_back({ "sep2", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({"explorer", L"Show in Explorer", L"", IconType::FolderPlaylist, false, false, false,
                                    [this, item]() { OpenInExplorer(item.filePath); }});
        contextMenuItems.push_back({ "sep4", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({ "remove", L"Remove from Library...", L"Del", IconType::Trash, false, false, true, [this, item]() {
            // The context menu closes and restores focus before invoking its callback. Rebind selection to the item that opened this menu
            // so removal cannot act on a stale visible/query index.
            selectedMediaIndices.clear();
            for (size_t index = 0; index < library.mediaItems.size(); ++index) {
                if (library.mediaItems[index].id == item.id) {
                    selectedMediaIndices.insert(index);
                    selectedMediaIndex = static_cast<int>(index);
                    break;
                }
            }
            ConfirmDeleteSelectedMedia();
        }});
    }
    PresentHubContextMenu(x, y, 220.0f);
}

void App::OpenBackgroundContextMenu(float x, float y) {
    PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    if (currentTab == TabView::Media) {
        bool hasSelection = !selectedMediaIndices.empty();
        size_t selCount = selectedMediaIndices.size();
        bool hasMedia = !library.mediaItems.empty();
        contextMenuItems.push_back({"select_all", L"Select All", L"Ctrl+A", IconType::Checkmark, false, false, false,
                                    [this]() { SelectAllInCurrentPane(); }, !hasMedia});
        if (hasSelection) {
            contextMenuItems.push_back({"play_queue", L"Play Selected in Queue (" + std::to_wstring(selCount) + L" Videos)", L"Enter",
                                        IconType::Play, false, false, false, [this]() { PlaySelectedInCurrentPane(); }});
            contextMenuItems.push_back({"mark_watched", L"Mark Selected as Watched (" + std::to_wstring(selCount) + L" Videos)", L"",
                                        IconType::Checkmark, false, false, false, [this]() { BulkSetWatched(true); }});
            contextMenuItems.push_back({"mark_unwatched", L"Mark Selected as Unwatched (" + std::to_wstring(selCount) + L" Videos)", L"",
                                        IconType::Refresh, false, false, false, [this]() { BulkSetWatched(false); }});
            for (const auto& pl : library.playlists) {
                if (pl.IsSmart()) continue;
                std::string plId = pl.id;
                std::wstring plName = Utf8ToUtf16(pl.name);
                contextMenuItems.push_back({"add_to_pl_" + plId, L"Add " + std::to_wstring(selCount) + L" items to " + plName, L"",
                                            IconType::FolderPlaylist, false, false, false, [this, plId]() { BulkAddToPlaylist(plId); }});
            }
            contextMenuItems.push_back({"remove_selected", L"Remove Selected (" + std::to_wstring(selCount) + L" Files)...", L"Del",
                                        IconType::Trash, false, false, true, [this]() { ConfirmDeleteSelectedMedia(); }});
        }
        contextMenuItems.push_back({ "sep_bg_1", L"", L"", IconType::Play, true });
    } else if (currentTab == TabView::Playlists || currentTab == TabView::History) {
        contextMenuItems.push_back(
            {"select_all", L"Select All", L"Ctrl+A", IconType::Checkmark, false, false, false, [this]() { SelectAllInCurrentPane(); }});
        contextMenuItems.push_back({ "sep_bg_1", L"", L"", IconType::Play, true });
    }
    contextMenuItems.push_back(
        {"open_file", L"Open File...", L"Ctrl+O", IconType::Plus, false, false, false, [this]() { OpenFileDialog(); }});
    contextMenuItems.push_back({"open_folder", L"Open Folder...", L"Ctrl+Shift+O", IconType::FolderPlaylist, false, false, false,
                                [this]() { OpenFolderDialog(); }});
    contextMenuItems.push_back({"add_watched_folder", L"Add Watched Folder...", L"", IconType::FolderPlaylist, false, false, false,
                                [this]() { AddWatchedFolderPrompt(); }});
    const auto& watched = library.GetWatchedFolders();
    if (!watched.empty()) {
        contextMenuItems.push_back({"rescan_watched", L"Rescan Watched Folders", L"", IconType::Refresh, false, false, false,
                                    [this]() { RescanAllWatchedFolders(); }});
        for (size_t i = 0; i < watched.size(); ++i) {
            std::wstring folder = watched[i];
            std::wstring fName = GetFileNameFromPath(folder);
            if (fName.empty()) fName = folder;
            contextMenuItems.push_back({ "remove_watch_" + std::to_string(i), L"Stop Watching: " + fName, L"", IconType::Trash, false, false, false, [this, folder]() {
                library.RemoveWatchedFolder(folder);
                folderWatcher.SetWatchedFoldersAsync(library.GetWatchedFolders());
                ShowOSD(IconType::Trash, L"Removed Watched Folder");
                InvalidateRect(hWnd, NULL, FALSE);
            }});
        }
    }
    contextMenuItems.push_back({ "sep_bg_2", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({ "refresh", L"Refresh", L"F5", IconType::Refresh, false, false, false, [this]() {
        if (!libraryLoadInProgress) { StartLibraryLoad(libraryGroupMode, true); ShowOSD(IconType::Refresh, L"Refreshing library\x2026"); }
    }});
    PresentHubContextMenu(x, y, 220.0f);
}

void App::OpenPlaylistContextMenu(const Playlist& playlist, float x, float y) {
    PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    std::string plId = playlist.id;
    contextMenuItems.push_back({ "open", L"Open Playlist", L"Enter", IconType::FolderPlaylist, false, false, false, [this, plId]() {
        activePlaylistId = plId;
        currentTab = TabView::Playlists;
        activeFocusPane = FocusPane::PlaylistDetail;
        keyboardFocusedIndex = 0;
        InvalidateRect(hWnd, NULL, FALSE);
    }});
    contextMenuItems.push_back(
        {"play_all", L"Play All", L"", IconType::Play, false, false, false, [this, plId]() { PlayPlaylist(plId, 0); }});
    contextMenuItems.push_back(
        {"new_playlist", L"New Playlist...", L"", IconType::Plus, false, false, false, [this]() { ShowNewPlaylistModal(); }});
    contextMenuItems.push_back({ "sep0", L"", L"", IconType::Play, true });
    if (playlist.IsSmart()) {
        contextMenuItems.push_back({"edit_rules", L"Edit Rules...", L"", IconType::SettingsGear, false, false, false,
                                    [this, plId]() { ShowEditSmartPlaylistModal(plId); }});
    }
    contextMenuItems.push_back(
        {"rename", L"Rename", L"F2", IconType::SettingsGear, false, false, false, [this, plId]() { ShowRenamePlaylistModal(plId); }});
    contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
    contextMenuItems.push_back(
        {"delete", L"Delete Playlist", L"Del", IconType::Trash, false, false, true, [this, plId]() { ShowDeletePlaylistModal(plId); }});
    PresentHubContextMenu(x, y, 220.0f);
}

void App::OpenRailBackgroundContextMenu(float x, float y) {
    PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    contextMenuItems.push_back(
        {"new_playlist", L"New Playlist...", L"", IconType::Plus, false, false, false, [this]() { ShowNewPlaylistModal(); }});
    PresentHubContextMenu(x, y, 220.0f);
}

void App::OpenPlaylistItemContextMenu(size_t itemIndex, float x, float y) {
    PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    Playlist* pl = library.FindPlaylist(activePlaylistId);
    if (!pl || itemIndex >= pl->itemPaths.size()) return;
    std::wstring path = pl->itemPaths[itemIndex];
    std::string plId = activePlaylistId;
    contextMenuItems.clear();
    size_t selCount = selectedEpisodeIndices.size();
    bool isMulti = selCount > 1;
    if (isMulti) {
        contextMenuItems.push_back({ "play_queue", Loc::Format("ux.queue_selected", std::to_wstring(selCount)), L"Enter", IconType::Play, false, false, false, [this]() {
            PlaySelectedInCurrentPane();
        }});
        contextMenuItems.push_back({ "mark_watched", L"Mark as watched (" + std::to_wstring(selCount) + L" Videos)", L"", IconType::Checkmark, false, false, false, [this, plId]() {
            Playlist* p = library.FindPlaylist(plId);
            if (!p) return;
            std::vector<std::wstring> paths;
            paths.reserve(selectedEpisodeIndices.size());
            for (size_t idx : selectedEpisodeIndices) { if (idx < p->itemPaths.size()) paths.push_back(p->itemPaths[idx]); }
            SetWatchedWithUndo(paths, true);
            ShowOSD(IconType::Checkmark, L"Marked as watched");
            InvalidateRect(hWnd, NULL, FALSE);
        }});
        contextMenuItems.push_back({ "mark_unwatched", L"Mark as unwatched (" + std::to_wstring(selCount) + L" Videos)", L"", IconType::Refresh, false, false, false, [this, plId]() {
            Playlist* p = library.FindPlaylist(plId);
            if (!p) return;
            std::vector<std::wstring> paths;
            paths.reserve(selectedEpisodeIndices.size());
            for (size_t idx : selectedEpisodeIndices) { if (idx < p->itemPaths.size()) paths.push_back(p->itemPaths[idx]); }
            SetWatchedWithUndo(paths, false);
            ShowOSD(IconType::Refresh, L"Marked as unwatched");
            InvalidateRect(hWnd, NULL, FALSE);
        }});
        contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({"remove_from_pl", L"Remove Selected (" + std::to_wstring(selCount) + L" Videos)", L"Del",
                                    IconType::Trash, false, false, true, [this]() { DeleteSelectedInCurrentPane(); }});
    } else {
        contextMenuItems.push_back({"play_item", L"Play Video", L"Enter", IconType::Play, false, false, false,
                                    [this, plId, itemIndex]() { PlayPlaylist(plId, itemIndex); }});
        contextMenuItems.push_back({ "mark_watched", L"Mark as watched", L"", IconType::Checkmark, false, false, false, [this, path]() {
            SetWatchedWithUndo({ path }, true);
            ShowOSD(IconType::Checkmark, L"Marked as watched");
            InvalidateRect(hWnd, NULL, FALSE);
        }});
        contextMenuItems.push_back({ "mark_unwatched", L"Mark as unwatched", L"", IconType::Refresh, false, false, false, [this, path]() {
            SetWatchedWithUndo({ path }, false);
            ShowOSD(IconType::Refresh, L"Marked as unwatched");
            InvalidateRect(hWnd, NULL, FALSE);
        }});
        contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({"explorer", L"Show in Explorer", L"", IconType::FolderPlaylist, false, false, false,
                                    [this, path]() { OpenInExplorer(path); }});
        contextMenuItems.push_back({ "sep2", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({ "remove_from_pl", L"Remove from Playlist", L"Del", IconType::Trash, false, false, true, [this, plId, itemIndex]() {
            ArmLibraryUndo(L"Playlist item removal");
            library.RemoveFromPlaylist(plId, itemIndex);
            SaveLibrary();
            ShowOSD(IconType::Trash, L"Removed from playlist \u2022 Ctrl+Z to undo");
            InvalidateRect(hWnd, NULL, FALSE);
        }});
    }
    PresentHubContextMenu(x, y, 220.0f);
}

void App::OpenHistoryContextMenu(const HistoryRecord& item, float x, float y) {
    PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    std::wstring path = item.filePath;
    double pos = item.lastPosSec;
    size_t selCount = selectedHistoryIndices.size();
    bool isMulti = selCount > 1;
    if (isMulti) {
        contextMenuItems.push_back({"remove_multi", L"Remove Selected (" + std::to_wstring(selCount) + L" Records)", L"Del",
                                    IconType::Trash, false, false, true, [this]() { DeleteSelectedInCurrentPane(); }});
    } else {
        contextMenuItems.push_back(
            {"play", L"Resume", L"Enter", IconType::Play, false, false, false, [this, path, pos]() { OpenAndPlayMedia(path, pos); }});
        contextMenuItems.push_back(
            {"restart", L"Restart", L"R", IconType::Refresh, false, false, false, [this, path]() { OpenAndPlayMedia(path, 0.0); }});
        contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({"explorer", L"Show in Explorer", L"", IconType::FolderPlaylist, false, false, false,
                                    [this, path]() { OpenInExplorer(path); }});
        contextMenuItems.push_back({ "sep2", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({ "remove", L"Remove", L"Del", IconType::Trash, false, false, true, [this, path]() {
            for (size_t i = 0; i < library.history.size(); ++i) {
                if (library.history[i].filePath == path) { ArmLibraryUndo(L"History removal"); library.RemoveFromHistory(i); break; }
            }
            ShowOSD(IconType::Trash, L"Removed from History");
            InvalidateRect(hWnd, NULL, FALSE);
        }});
    }
    PresentHubContextMenu(x, y, 220.0f);
}

void App::OpenSleepTimerContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenPlayerAdvancedContextMenu(x, y); } },
        { L"Cancel", IconType::CloseX, false, false, sleepTimerRemainingSec <= 0.0f && !stopAfterCurrent, false, [this]() {
            sleepTimerRemainingSec = 0.0f;
            stopAfterCurrent = false;
            ShowOSD(IconType::HistoryClock, L"Sleep timer canceled");
        } });
    const auto addMinutes = [this](const char* id, const wchar_t* label, int minutes) {
        contextMenuItems.push_back({ id, label, L"", IconType::HistoryClock, false, false, false, [this, minutes]() {
            sleepTimerRemainingSec = static_cast<float>(minutes * 60);
            ShowOSD(IconType::HistoryClock, L"Sleep timer set for " + std::to_wstring(minutes) + L" minutes");
        }});
    };
    addMinutes("sleep_15", L"Stop in 15 minutes", 15);
    addMinutes("sleep_30", L"Stop in 30 minutes", 30);
    addMinutes("sleep_60", L"Stop in 60 minutes", 60);
    addMinutes("sleep_90", L"Stop in 90 minutes", 90);
    contextMenuItems.push_back({ "stop_after_current", L"Stop After Current", L"", IconType::HistoryClock, false,
        stopAfterCurrent, false, [this]() {
            stopAfterCurrent = !stopAfterCurrent;
            ShowOSD(IconType::HistoryClock, stopAfterCurrent ? L"Will stop after current media" : L"Stop-after-current canceled");
        }});
    contextMenuItems.push_back({ "sleep_cancel", L"Cancel Sleep Timer", L"", IconType::CloseX, false, false,
        true, [this]() {
            sleepTimerRemainingSec = 0.0f;
            stopAfterCurrent = false;
            ShowOSD(IconType::HistoryClock, L"Sleep timer canceled");
        }, sleepTimerRemainingSec <= 0.0f && !stopAfterCurrent });
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::None);
}

void App::CycleABRepeat() {
    if (!playbackSession.IsOpen()) return;
    if (abLoopA >= 0.0 && abLoopB > abLoopA) {
        abLoopA = abLoopB = -1.0;
        player.ClearABLoop();
        ShowOSD(IconType::Refresh, L"A-B repeat cleared");
        return;
    }
    const double point = player.GetCurrentTime();
    if (!std::isfinite(point) || point < 0.0) return;
    if (abLoopA < 0.0) {
        abLoopA = point;
        abLoopB = -1.0;
        player.ClearABLoop();
        player.SetABLoopA(point);
        ShowOSD(IconType::Refresh, L"A-B start: " + FormatDuration(point));
    } else if (point > abLoopA) {
        abLoopB = point;
        player.SetABLoopB(point);
        ShowOSD(IconType::Refresh, L"A-B repeat: " + FormatDuration(abLoopA) + L" - " + FormatDuration(point));
    } else ShowOSD(IconType::VolumeMuted, L"Choose an end after " + FormatDuration(abLoopA));
}

void App::OpenPlayerContextMenu(float x, float y) {
    if (isSettingsFlyoutOpen) CloseSettingsFlyout();
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();

    if (playbackSession.IsPiP()) {
        contextMenuItems.push_back({ "play_pause", player.IsPlaying() ? L"Pause" : L"Play", L"Space", player.IsPlaying() ? IconType::Pause : IconType::Play, false, false, false, [this]() {
            commandDispatcher.Execute(AppCommand::TogglePlay);
        }});
        if (playbackSession.IsQueuePlayback()) {
            const bool previousDisabled = playbackSession.GetQueueIndex() == 0 && !settings.loop;
            contextMenuItems.push_back({"previous", L"Previous", L"", IconType::Prev, false, false, previousDisabled,
                                        [this]() { commandDispatcher.Execute(AppCommand::PlayPrevious); }});
            contextMenuItems.push_back({"next", L"Next", L"N", IconType::Next, false, false, false,
                                        [this]() { commandDispatcher.Execute(AppCommand::PlayNext); }});
        }
        contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({"audio_cycle", L"Cycle Audio Track", L"A", IconType::VolumeHigh, false, false, false,
                                    [this]() { commandDispatcher.Execute(AppCommand::CycleAudio); }});
        contextMenuItems.push_back({"subtitles_menu", L"Subtitles...", L"S", IconType::Subtitles, false, false, false,
                                    [this, x, y]() { OpenSubtitlesContextMenu(x, y); }});
        contextMenuItems.push_back({ "sep2", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({"pip_pin", playbackSession.IsPiPPinned() ? L"Unpin" : L"Always on Top", L"", IconType::Pin, false, playbackSession.IsPiPPinned(), false,
                                    [this]() { playbackSession.TogglePiPPinned(hWnd); SyncOverlayPosition(); MarkAccessibilityDirty(); RefreshAccessibilityTree(true); Render(); }});
        contextMenuItems.push_back({"pip", L"Exit PiP", L"P", IconType::PictureInPicture, false, false, false,
                                    [this]() { commandDispatcher.Execute(AppCommand::TogglePiP); }});
        contextMenuItems.push_back({"exit", L"Exit Player", L"Esc", IconType::CloseX, false, false, false,
                                    [this]() { SendMessage(hWnd, WM_CLOSE, 0, 0); }});
        PresentPlayerContextMenu(x, y, 160.0f, true, PlayerMenuReveal::Cursor);
        return;
    }

    SetIrisShoulders(
        { player.IsPlaying() ? L"Pause" : L"Play", player.IsPlaying() ? IconType::Pause : IconType::Play, true, false, false, true,
          [this]() {
              commandDispatcher.Execute(AppCommand::TogglePlay);
              irisLeftShoulder.label = player.IsPlaying() ? L"Pause" : L"Play";
              irisLeftShoulder.icon = player.IsPlaying() ? IconType::Pause : IconType::Play;
          } },
        { L"Next", IconType::Next, true, true, false, false,
          [this]() { commandDispatcher.Execute(AppCommand::PlayNext); } });

    contextMenuItems.push_back({ "loop", settings.loop ? L"Loop: On" : L"Loop: Off", L"", IconType::Refresh, false, settings.loop, false,
        [this]() {
            commandDispatcher.Execute(AppCommand::ToggleLoop);
            for (auto& it : contextMenuItems) {
                if (it.id == "loop") {
                    it.label = settings.loop ? L"Loop: On" : L"Loop: Off";
                    it.isChecked = settings.loop;
                    break;
                }
            }
        }, false, false, true });

    auto chapters = player.GetChapters();
    if (!chapters.empty()) {
        wchar_t chBuf[64];
        swprintf_s(chBuf, L"Chapters (%zu)...", chapters.size());
        contextMenuItems.push_back({"chapters_menu", chBuf, L"\u25B8", IconType::FolderPlaylist, false, false, false,
                                    [this, x, y]() { OpenChaptersContextMenu(x, y); }, false, false, true});
    }
    const MediaItem* curBookmarkItem = library.FindByPath(player.GetCurrentFilePath());
    size_t bookmarkCount = curBookmarkItem ? curBookmarkItem->bookmarks.size() : 0;
    if (bookmarkCount > 0) {
        wchar_t bmBuf[64];
        swprintf_s(bmBuf, L"Bookmarks (%zu)...", bookmarkCount);
        contextMenuItems.push_back(
            {"bookmarks_menu", bmBuf, L"\u25B8", IconType::MediaFilm, false, false, false, [this, x, y]() { OpenBookmarksContextMenu(x, y); }, false, false, true});
    }
    contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({"subtitles_menu", L"Subtitles & Dual Subtitles...", L"\u25B8", IconType::Subtitles, false, false, false,
                                [this, x, y]() { OpenSubtitlesContextMenu(x, y); }, false, false, true});
    contextMenuItems.push_back({"sub_open", L"Open Subtitle File...", L"S", IconType::Subtitles, false, false, false,
                                [this]() { commandDispatcher.Execute(AppCommand::OpenSubtitleFile); }});
    contextMenuItems.push_back({"audio_cycle", L"Cycle Audio Track", L"A", IconType::VolumeHigh, false, false, false,
                                [this]() { commandDispatcher.Execute(AppCommand::CycleAudio); }});
    contextMenuItems.push_back({ "sep2", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({"pip", playbackSession.IsPiP() ? L"Exit PiP" : L"PiP Mode", L"P", IconType::PictureInPicture, false, false,
                                false, [this]() { commandDispatcher.Execute(AppCommand::TogglePiP); }});
    contextMenuItems.push_back({"full", playbackSession.IsFullscreen() ? L"Exit Fullscreen" : L"Fullscreen", L"F", IconType::Fullscreen,
                                false, false, false, [this]() { commandDispatcher.Execute(AppCommand::ToggleFullscreen); }});
    contextMenuItems.push_back({"screenshot", L"Take Screenshot", L"Ctrl+S", IconType::MediaFilm, false, false, false,
                                [this]() { commandDispatcher.Execute(AppCommand::TakeScreenshot); }});
    contextMenuItems.push_back({"explorer", L"Show in Explorer", L"", IconType::FolderPlaylist, false, false, false,
                                [this]() { commandDispatcher.Execute(AppCommand::ShowInExplorer); }});
    contextMenuItems.push_back({ "sep3", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({"advanced_menu", L"More Options...", L"\u25B8", IconType::SettingsGear, false, false, false,
                                [this, x, y]() { OpenPlayerAdvancedContextMenu(x, y); }, false, false, true});
    contextMenuItems.push_back({"exit", L"Exit Player", L"Esc", IconType::CloseX, false, false, true,
                                [this]() { commandDispatcher.Execute(AppCommand::ClosePlayer); }});
    PresentPlayerContextMenu(x, y, 238.0f, true, PlayerMenuReveal::Cursor);
}

void App::OpenPlayerAdvancedContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenPlayerContextMenu(x, y); } },
        { L"Restart", IconType::Refresh, false, false, false, false, [this]() { commandDispatcher.Execute(AppCommand::RestartVideo); } });

    contextMenuItems.push_back(
        {"add_bookmark", L"Add Bookmark...", L"", IconType::Plus, false, false, false, [this]() { ShowAddBookmarkModal(); }});
    const bool abStarted = abLoopA >= 0.0;
    const bool abActive = abStarted && abLoopB > abLoopA;
    const std::wstring abTimes = !abStarted ? L"Shift+A" :
        (abActive ? FormatDuration(abLoopA) + L"\u2013" + FormatDuration(abLoopB) : L"A " + FormatDuration(abLoopA));
    contextMenuItems.push_back({ "ab_cycle", abActive ? L"Clear A-B Repeat" : abStarted ? L"Set A-B End" : L"Set A-B Start",
        abTimes, IconType::Refresh, false, abStarted, abActive, [this, x, y]() {
            CycleABRepeat();
            OpenPlayerAdvancedContextMenu(x, y);
        }, false, abStarted, true });
    contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({ "sleep_timer", L"Sleep Timer...", L"", IconType::HistoryClock, false,
        sleepTimerRemainingSec > 0.0f || stopAfterCurrent, false,
        [this, x, y]() { OpenSleepTimerContextMenu(x, y); }, false, false, true});
    std::wstring balSummary = L"Center";
    if (settings.audioBalance < -0.01) {
        wchar_t buf[32];
        swprintf_s(buf, L"Left %.0f%%", -settings.audioBalance * 100.0);
        balSummary = buf;
    } else if (settings.audioBalance > 0.01) {
        wchar_t buf[32];
        swprintf_s(buf, L"Right %.0f%%", settings.audioBalance * 100.0);
        balSummary = buf;
    }
    contextMenuItems.push_back({"audio_balance_menu", L"Audio Balance (" + balSummary + L")...", L"", IconType::VolumeHigh, false, false,
                                false, [this, x, y]() { OpenAudioBalanceContextMenu(x, y); }, false, false, true});
    std::wstring chSummary = L"Auto";
    if (settings.audioChannelMap == "stereo") chSummary = L"Stereo";
    else if (settings.audioChannelMap == "mono-left") chSummary = L"Mono (L)";
    else if (settings.audioChannelMap == "mono-right") chSummary = L"Mono (R)";
    else if (settings.audioChannelMap == "swap") chSummary = L"Swap (L\u2194R)";
    contextMenuItems.push_back({"audio_channel_menu", L"Channel Mapping (" + chSummary + L")...", L"", IconType::VolumeHigh, false, false,
                                false, [this, x, y]() { OpenAudioChannelMapContextMenu(x, y); }, false, false, true});
    contextMenuItems.push_back({"video_adjustments_menu", L"Video Adjustments...", L"", IconType::SettingsGear, false, false, false,
                                [this, x, y]() { OpenVideoAdjustmentsContextMenu(x, y); }, false, false, true});
    contextMenuItems.push_back({ "sep2", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({ "diagnostics", showPlaybackDiagnostics ? L"Hide Diagnostics" : L"Playback Diagnostics", L"I", IconType::SettingsGear, false, showPlaybackDiagnostics, false, [this]() {
        commandDispatcher.Execute(AppCommand::TogglePlaybackDiagnostics);
    }});
    if (!lastScreenshotPath.empty() && FileExists(lastScreenshotPath)) {
        contextMenuItems.push_back({"open_screenshot", L"Show Last Screenshot", L"", IconType::FolderPlaylist, false, false, false,
                                    [this]() { OpenInExplorer(lastScreenshotPath); }});
    }
    PresentPlayerContextMenu(x, y, 238.0f, true, PlayerMenuReveal::Controls);
}

void App::OpenAudioDelayContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenPlayerAdvancedContextMenu(x, y); } },
        { L"Reset", IconType::Refresh, false, false, false, true, [this, x, y]() {
            ResetIrisDial(2);
            OpenAudioDelayContextMenu(x, y);
        } });
    wchar_t headerBuf[64];
    swprintf_s(headerBuf, L"Audio Delay: %+d ms", settings.audioDelayMs);
    contextMenuItems.push_back({ "adelay_title", headerBuf, L"", IconType::VolumeHigh, false, false, false, nullptr, true });
    contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({ "adelay_inc", L"+50 ms (Increase)", L"Ctrl+Right", IconType::Next, false, false, false, [this, x, y]() {
        commandDispatcher.Execute(AppCommand::AudioDelayPlus);
        OpenAudioDelayContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "adelay_dec", L"−50 ms (Decrease)", L"Ctrl+Left", IconType::Prev, false, false, false, [this, x, y]() {
        commandDispatcher.Execute(AppCommand::AudioDelayMinus);
        OpenAudioDelayContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "adelay_rst", L"Reset (0 ms)", L"Ctrl+Bksp", IconType::Refresh, false, false, false, [this, x, y]() {
        commandDispatcher.Execute(AppCommand::AudioDelayReset);
        OpenAudioDelayContextMenu(x, y);
    }, false, false, true});
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::None);
}

void App::OpenAudioBalanceContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenPlayerAdvancedContextMenu(x, y); } },
        { L"Center", IconType::Refresh, false, false, false, true, [this, x, y]() {
            commandDispatcher.Execute(AppCommand::AudioBalanceReset);
            OpenAudioBalanceContextMenu(x, y);
        } });
    std::wstring balText = L"Audio Balance: Center (0%)";
    if (settings.audioBalance < -0.01) {
        wchar_t buf[64];
        swprintf_s(buf, L"Audio Balance: Left %.0f%%", -settings.audioBalance * 100.0);
        balText = buf;
    } else if (settings.audioBalance > 0.01) {
        wchar_t buf[64];
        swprintf_s(buf, L"Audio Balance: Right %.0f%%", settings.audioBalance * 100.0);
        balText = buf;
    }
    contextMenuItems.push_back({ "abal_title", balText, L"", IconType::VolumeHigh, false, false, false, nullptr, true });
    contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({ "abal_left_10", L"← Shift Left 10%", L"", IconType::Prev, false, false, false, [this, x, y]() {
        commandDispatcher.Execute(AppCommand::AudioBalanceLeft);
        OpenAudioBalanceContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "abal_right_10", L"Shift Right 10% →", L"", IconType::Next, false, false, false, [this, x, y]() {
        commandDispatcher.Execute(AppCommand::AudioBalanceRight);
        OpenAudioBalanceContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "sep2", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({ "abal_full_left", L"Full Left (100% L)", L"", IconType::Prev, false, settings.audioBalance <= -0.99, false, [this, x, y]() {
        CommandArgs args;
        args.doubleVal = -1.0;
        commandDispatcher.Execute(AppCommand::SetAudioBalance, args);
        OpenAudioBalanceContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "abal_center", L"Center (Reset 0%)", L"", IconType::Refresh, false, std::abs(settings.audioBalance) < 0.01, false, [this, x, y]() {
        commandDispatcher.Execute(AppCommand::AudioBalanceReset);
        OpenAudioBalanceContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "abal_full_right", L"Full Right (100% R)", L"", IconType::Next, false, settings.audioBalance >= 0.99, false, [this, x, y]() {
        CommandArgs args;
        args.doubleVal = 1.0;
        commandDispatcher.Execute(AppCommand::SetAudioBalance, args);
        OpenAudioBalanceContextMenu(x, y);
    }, false, false, true});
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::None);
}

void App::OpenAudioChannelMapContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenPlayerAdvancedContextMenu(x, y); } },
        { L"Auto", IconType::Refresh, false, false, false, true, [this, x, y]() {
            commandDispatcher.Execute(AppCommand::AudioChannelMapReset);
            OpenAudioChannelMapContextMenu(x, y);
        } });
    std::string curMap = Settings::NormalizeAudioChannelMap(settings.audioChannelMap);
    contextMenuItems.push_back({ "ach_auto", L"Auto (Default)", L"", IconType::VolumeHigh, false, curMap == "auto", false, [this, x, y]() {
        CommandArgs args;
        args.stringVal = "auto";
        commandDispatcher.Execute(AppCommand::SetAudioChannelMap, args);
        OpenAudioChannelMapContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "ach_stereo", L"Stereo (Direct)", L"", IconType::VolumeHigh, false, curMap == "stereo", false, [this, x, y]() {
        CommandArgs args;
        args.stringVal = "stereo";
        commandDispatcher.Execute(AppCommand::SetAudioChannelMap, args);
        OpenAudioChannelMapContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "ach_mono_left", L"Mono (Left Channel Only)", L"", IconType::Prev, false, curMap == "mono-left", false, [this, x, y]() {
        CommandArgs args;
        args.stringVal = "mono-left";
        commandDispatcher.Execute(AppCommand::SetAudioChannelMap, args);
        OpenAudioChannelMapContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "ach_mono_right", L"Mono (Right Channel Only)", L"", IconType::Next, false, curMap == "mono-right", false, [this, x, y]() {
        CommandArgs args;
        args.stringVal = "mono-right";
        commandDispatcher.Execute(AppCommand::SetAudioChannelMap, args);
        OpenAudioChannelMapContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "ach_swap", L"Swap (Left ↔ Right)", L"", IconType::Refresh, false, curMap == "swap", false, [this, x, y]() {
        CommandArgs args;
        args.stringVal = "swap";
        commandDispatcher.Execute(AppCommand::SetAudioChannelMap, args);
        OpenAudioChannelMapContextMenu(x, y);
    }, false, false, true});
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::None);
}

void App::OpenVideoAdjustmentsContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenPlayerAdvancedContextMenu(x, y); } },
        { L"Reset", IconType::Refresh, false, false, false, true, [this, x, y]() {
            commandDispatcher.Execute(AppCommand::ResetVideoAdjustments);
            OpenVideoAdjustmentsContextMenu(x, y);
        } });
    wchar_t rotBuf[64];
    swprintf_s(rotBuf, L"Rotate 90° (Current: %d°)", player.GetVideoRotate());
    contextMenuItems.push_back({ "vadjust_rot", rotBuf, L"", IconType::AspectFrame, false, false, false, [this, x, y]() {
        commandDispatcher.Execute(AppCommand::RotateVideo);
        OpenVideoAdjustmentsContextMenu(x, y);
    }, false, false, true});
    wchar_t brightBuf[64];
    swprintf_s(brightBuf, L"Brightness: %+d...", player.GetBrightness());
    contextMenuItems.push_back({"vadjust_bright", brightBuf, L"\u25B8", IconType::SettingsGear, false, false, false,
                                [this, x, y]() { OpenAdjustmentContextMenu("brightness", x, y); }, false, false, true});
    wchar_t contrastBuf[64];
    swprintf_s(contrastBuf, L"Contrast: %+d...", player.GetContrast());
    contextMenuItems.push_back({"vadjust_contrast", contrastBuf, L"\u25B8", IconType::SettingsGear, false, false, false,
                                [this, x, y]() { OpenAdjustmentContextMenu("contrast", x, y); }, false, false, true});
    wchar_t satBuf[64];
    swprintf_s(satBuf, L"Saturation: %+d...", player.GetSaturation());
    contextMenuItems.push_back({"vadjust_sat", satBuf, L"\u25B8", IconType::SettingsGear, false, false, false,
                                [this, x, y]() { OpenAdjustmentContextMenu("saturation", x, y); }, false, false, true});
    wchar_t gammaBuf[64];
    swprintf_s(gammaBuf, L"Gamma: %+d...", player.GetGamma());
    contextMenuItems.push_back({"vadjust_gamma", gammaBuf, L"\u25B8", IconType::SettingsGear, false, false, false,
                                [this, x, y]() { OpenAdjustmentContextMenu("gamma", x, y); }, false, false, true});
    wchar_t hueBuf[64];
    swprintf_s(hueBuf, L"Hue: %+d...", player.GetHue());
    contextMenuItems.push_back({"vadjust_hue", hueBuf, L"\u25B8", IconType::SettingsGear, false, false, false,
                                [this, x, y]() { OpenAdjustmentContextMenu("hue", x, y); }, false, false, true});
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::None);
}

void App::OpenAdjustmentContextMenu(const std::string& type, float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    std::wstring typeName = L"Adjustment";
    AppCommand plusCmd = AppCommand::None;
    AppCommand minusCmd = AppCommand::None;
    AppCommand resetCmd = AppCommand::None;
    int curVal = 0;
    if (type == "brightness") {
        typeName = L"Brightness";
        plusCmd = AppCommand::BrightnessPlus;
        minusCmd = AppCommand::BrightnessMinus;
        resetCmd = AppCommand::BrightnessReset;
        curVal = player.GetBrightness();
    } else if (type == "contrast") {
        typeName = L"Contrast";
        plusCmd = AppCommand::ContrastPlus;
        minusCmd = AppCommand::ContrastMinus;
        resetCmd = AppCommand::ContrastReset;
        curVal = player.GetContrast();
    } else if (type == "saturation") {
        typeName = L"Saturation";
        plusCmd = AppCommand::SaturationPlus;
        minusCmd = AppCommand::SaturationMinus;
        resetCmd = AppCommand::SaturationReset;
        curVal = player.GetSaturation();
    } else if (type == "gamma") {
        typeName = L"Gamma";
        plusCmd = AppCommand::GammaPlus;
        minusCmd = AppCommand::GammaMinus;
        resetCmd = AppCommand::GammaReset;
        curVal = player.GetGamma();
    } else if (type == "hue") {
        typeName = L"Hue";
        plusCmd = AppCommand::HuePlus;
        minusCmd = AppCommand::HueMinus;
        resetCmd = AppCommand::HueReset;
        curVal = player.GetHue();
    }
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenVideoAdjustmentsContextMenu(x, y); } },
        { L"Reset", IconType::Refresh, false, false, false, true, [this, type, resetCmd, x, y]() {
            commandDispatcher.Execute(resetCmd);
            OpenAdjustmentContextMenu(type, x, y);
        } });
    wchar_t headerBuf[64];
    swprintf_s(headerBuf, L"%s: %+d", typeName.c_str(), curVal);
    contextMenuItems.push_back({ "adj_title", headerBuf, L"", IconType::SettingsGear, false, false, false, nullptr, true });
    contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({ "adj_inc", L"+5 (Increase)", L"", IconType::Next, false, false, false, [this, type, plusCmd, x, y]() {
        commandDispatcher.Execute(plusCmd);
        OpenAdjustmentContextMenu(type, x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "adj_dec", L"−5 (Decrease)", L"", IconType::Prev, false, false, false, [this, type, minusCmd, x, y]() {
        commandDispatcher.Execute(minusCmd);
        OpenAdjustmentContextMenu(type, x, y);
    }, false, false, true});
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::None);
}

void App::OpenChaptersContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenPlayerContextMenu(x, y); } },
        { L"Skip OP", IconType::Next, true, true, false, false, [this]() {
            double target = std::min(player.GetDuration(), player.GetCurrentTime() + 85.0);
            commandDispatcher.Execute(AppCommand::SeekTo, CommandArgs{ target });
            ShowOSD(IconType::Next, L"Skipped OP (+85s)");
        } });
    auto chapters = player.GetChapters();
    int currentChap = player.GetCurrentChapter();
    if (currentChap < 0 && !chapters.empty()) {
        double curTime = player.GetCurrentTime();
        for (int i = (int)chapters.size() - 1; i >= 0; --i) {
            if (chapters[i].time <= curTime + 0.25) { currentChap = i; break; }
        }
    }
    for (size_t i = 0; i < chapters.size() && i < 50; ++i) {
        const auto& ch = chapters[i];
        std::wstring label = ch.title.empty() ? (std::to_wstring(i + 1) + L". Chapter " + std::to_wstring(i + 1)) : ch.title;
        if (label.length() > 32) { label = label.substr(0, 29) + L"..."; }
        std::wstring timeStr = FormatDuration(ch.time);
        bool isCurrent = (static_cast<int>(i) == currentChap);
        int idx = static_cast<int>(i);
        contextMenuItems.push_back({ "chapter_" + std::to_string(i), label, timeStr, IconType::Play, false, isCurrent, false, [this, idx]() {
            player.SetChapter(idx);
            auto chaps = player.GetChapters();
            if (idx >= 0 && idx < (int)chaps.size()) { ShowOSD(IconType::Next, chaps[idx].title); }
        }});
    }
    PresentPlayerContextMenu(x, y, 238.0f, true, PlayerMenuReveal::Cursor);
}

void App::OpenBookmarksContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenPlayerContextMenu(x, y); } },
        { L"Add...", IconType::Plus, true, false, false, false, [this]() { ShowAddBookmarkModal(); } });
    const MediaItem* curItem = library.FindByPath(player.GetCurrentFilePath());
    const auto& bookmarks = curItem ? curItem->bookmarks : std::vector<MediaBookmark>{};
    if (!bookmarks.empty()) {
        double curTime = player.GetCurrentTime();
        for (size_t i = 0; i < bookmarks.size() && i < 50; ++i) {
            const auto& bm = bookmarks[i];
            std::wstring label = bm.label.empty() ? (L"Bookmark at " + FormatDuration(bm.timeSec)) : bm.label;
            if (label.length() > 32) { label = label.substr(0, 29) + L"..."; }
            std::wstring timeStr = FormatDuration(bm.timeSec);
            bool isCurrent = (std::abs(curTime - bm.timeSec) < 1.0);
            double seekTime = bm.timeSec;
            std::wstring bmLabel = bm.label;
            contextMenuItems.push_back({ "bm_seek_" + std::to_string(i), label, timeStr, IconType::Play, false, isCurrent, false, [this, seekTime, bmLabel]() {
                commandDispatcher.Execute(AppCommand::SeekTo, CommandArgs{ seekTime });
                ShowOSD(IconType::Play, bmLabel.empty() ? (L"Bookmark: " + FormatDuration(seekTime)) : (L"Bookmark: " + bmLabel));
            }});
        }
        contextMenuItems.push_back({ "sep_bm2", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({"delete_bookmark_menu", L"Delete Bookmark...", L"\u25B8", IconType::Trash, false, false, false,
                                    [this, x, y]() { OpenDeleteBookmarksContextMenu(x, y); }, false, false, true});
        contextMenuItems.push_back({"clear_all_bookmarks", L"Clear All Bookmarks", L"", IconType::Trash, false, false, true,
                                    [this]() { PromptClearAllBookmarks(); }});
    } else {
        contextMenuItems.push_back(
            {"add_bookmark_sub", L"Add Bookmark...", L"", IconType::Plus, false, false, false, [this]() { ShowAddBookmarkModal(); }});
    }
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::None);
}

void App::OpenDeleteBookmarksContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenBookmarksContextMenu(x, y); } },
        { L"Clear All", IconType::Trash, false, false, false, false, [this]() { PromptClearAllBookmarks(); } });
    const MediaItem* curItem = library.FindByPath(player.GetCurrentFilePath());
    const auto& bookmarks = curItem ? curItem->bookmarks : std::vector<MediaBookmark>{};
    std::wstring path = player.GetCurrentFilePath();
    for (size_t i = 0; i < bookmarks.size() && i < 50; ++i) {
        const auto& bm = bookmarks[i];
        std::wstring label = bm.label.empty() ? (L"Bookmark at " + FormatDuration(bm.timeSec)) : bm.label;
        if (label.length() > 32) { label = label.substr(0, 29) + L"..."; }
        std::wstring timeStr = FormatDuration(bm.timeSec);
        std::string bmId = bm.id;
        std::wstring bmLabel = bm.label;
        contextMenuItems.push_back({ "del_bm_" + std::to_string(i), label, timeStr, IconType::Trash, false, false, true, [this, path, bmId, bmLabel, x, y]() {
            if (library.RemoveBookmark(path, bmId)) { SaveLibrary(); ShowOSD(IconType::Trash, L"Deleted: " + bmLabel); }
            const MediaItem* updated = library.FindByPath(path);
            if (updated && !updated->bookmarks.empty()) { OpenDeleteBookmarksContextMenu(x, y); } else { OpenPlayerContextMenu(x, y); }
        }, false, false, true});
    }
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::None);
}

void App::OpenSubtitleDelayContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenSubtitlesContextMenu(x, y); } },
        { L"Reset", IconType::Refresh, false, false, false, true, [this, x, y]() {
            ResetIrisDial(1);
            OpenSubtitleDelayContextMenu(x, y);
        } });
    wchar_t headerBuf[64];
    swprintf_s(headerBuf, L"Subtitle Delay: %+d ms", settings.subDelayMs);
    contextMenuItems.push_back({ "sdelay_title", headerBuf, L"", IconType::Subtitles, false, false, false, nullptr, true });
    contextMenuItems.push_back({ "sep1", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({ "sdelay_inc", L"+100 ms (Increase)", L"Ctrl+Shift+Right", IconType::Next, false, false, false, [this, x, y]() {
        commandDispatcher.Execute(AppCommand::SubDelayPlus);
        OpenSubtitleDelayContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "sdelay_dec", L"−100 ms (Decrease)", L"Ctrl+Shift+Left", IconType::Prev, false, false, false, [this, x, y]() {
        commandDispatcher.Execute(AppCommand::SubDelayMinus);
        OpenSubtitleDelayContextMenu(x, y);
    }, false, false, true});
    contextMenuItems.push_back({ "sdelay_rst", L"Reset (0 ms)", L"Ctrl+Shift+Bksp", IconType::Refresh, false, false, false, [this, x, y]() {
        commandDispatcher.Execute(AppCommand::SubDelayReset);
        OpenSubtitleDelayContextMenu(x, y);
    }, false, false, true});
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::None);
}

void App::OpenSubtitlesContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenPlayerContextMenu(x, y); } },
        { L"Open...", IconType::Plus, true, false, false, false, [this]() { commandDispatcher.Execute(AppCommand::OpenSubtitleFile); } });
    auto subTracks = player.GetSubtitleTracks();
    // 1. Primary Subtitle track selection
    contextMenuItems.push_back({ "prim_hdr", L"Primary Subtitle (Bottom):", L"", IconType::Subtitles, false, false, false, nullptr, true });
    bool isPrimaryOff = (!player.GetSubtitleVisibility() || player.GetCurrentSubtitleTrackId() == 0);
    contextMenuItems.push_back({ "prim_off", L"  Off", isPrimaryOff ? L"\u2713" : L"", IconType::Subtitles, false, isPrimaryOff, false, [this, x, y]() {
        commandDispatcher.Execute(AppCommand::SelectSubtitleTrack, CommandArgs{ 0.0, 0, "", L"", L"" });
        OpenSubtitlesContextMenu(x, y);
    }, false, false, true});
    for (const auto& t : subTracks) {
        std::wstring name = t.title.empty() ? (L"Track " + std::to_wstring(t.id)) : t.title;
        if (!t.lang.empty() && t.title.find(t.lang) == std::wstring::npos) { name += L" [" + t.lang + L"]"; }
        bool isSelected = t.isSelected && player.GetSubtitleVisibility();
        int targetId = t.id;
        std::wstring targetTitle = t.title;
        std::wstring targetLang = t.lang;
        contextMenuItems.push_back({ "prim_track_" + std::to_string(t.id), L"  " + name, isSelected ? L"\u2713" : L"V", IconType::Subtitles, false, isSelected, false, [this, x, y, targetId, targetTitle, targetLang]() {
            commandDispatcher.Execute(AppCommand::SelectSubtitleTrack, CommandArgs{ 0.0, targetId, "", targetTitle, targetLang });
            OpenSubtitlesContextMenu(x, y);
        }, false, false, true});
    }
    contextMenuItems.push_back({ "sep2", L"", L"", IconType::Play, true });
    // 2. Secondary Subtitle track selection (Dual Subtitles)
    contextMenuItems.push_back({ "sec_hdr", L"Dual Subtitles (Top):", L"", IconType::Subtitles, false, false, false, nullptr, true });
    bool isSecondaryOff = (!player.GetSecondarySubtitleVisibility() || player.GetCurrentSecondarySubtitleTrackId() == 0);
    contextMenuItems.push_back({ "sec_off", L"  Off", isSecondaryOff ? L"\u2713" : L"", IconType::Subtitles, false, isSecondaryOff, false, [this, x, y]() {
        player.SelectSecondarySubtitleTrack(0);
        player.SetSecondarySubtitleVisibility(false);
        OpenSubtitlesContextMenu(x, y);
    }, false, false, true});
    for (const auto& t : subTracks) {
        std::wstring name = t.title.empty() ? (L"Track " + std::to_wstring(t.id)) : t.title;
        if (!t.lang.empty() && t.title.find(t.lang) == std::wstring::npos) { name += L" [" + t.lang + L"]"; }
        bool isSecSelected = (t.isSecondarySelected || player.GetCurrentSecondarySubtitleTrackId() == t.id) && player.GetSecondarySubtitleVisibility();
        int targetId = t.id;
        contextMenuItems.push_back({ "sec_track_" + std::to_string(t.id), L"  " + name, isSecSelected ? L"\u2713" : L"", IconType::Subtitles, false, isSecSelected, false, [this, x, y, targetId]() {
            player.SelectSecondarySubtitleTrack(targetId);
            player.SetSecondarySubtitleVisibility(true);
            OpenSubtitlesContextMenu(x, y);
        }, false, false, true});
    }
    // 3. Discovered external subtitles
    std::wstring currentMedia = player.GetCurrentFilePath().empty() ? playbackSession.GetCurrentFilePath() : player.GetCurrentFilePath();
    if (!currentMedia.empty()) {
        auto discovered = SubtitleDiscovery::DiscoverSubtitles(currentMedia);
        if (!discovered.empty()) {
            contextMenuItems.push_back({ "sep3", L"", L"", IconType::Play, true });
            contextMenuItems.push_back({ "disc_hdr", L"Discovered Subtitles:", L"", IconType::FolderPlaylist, false, false, false, nullptr, true });
            for (size_t i = 0; i < discovered.size() && i < 6; ++i) {
                const auto& disc = discovered[i];
                std::wstring discLabel = disc.title.empty() ? GetFileNameFromPath(disc.filePath) : disc.title;
                if (!disc.language.empty() && discLabel.find(disc.language) == std::wstring::npos)
                    discLabel += L" [" + disc.language + L"]";
                std::wstring discPath = disc.filePath;
                contextMenuItems.push_back({ "disc_sub_" + std::to_string(i), L"  " + discLabel, L"", IconType::Subtitles, false, false, false, [this, x, y, discPath]() {
                    bool loaded = player.LoadExternalSubtitle(discPath, false);
                    if (loaded) { subtitles.Clear(); ShowOSD(IconType::Subtitles, L"Loaded: " + GetFileNameFromPath(discPath)); }
                    OpenSubtitlesContextMenu(x, y);
                }, false, false, true});
            }
        }
    }
    contextMenuItems.push_back({ "sep4", L"", L"", IconType::Play, true });
    contextMenuItems.push_back({"sub_online", L"Search Online Subtitles...", L"\u25B8", IconType::Subtitles, false, false, false,
                                [this, x, y]() { OpenOnlineSubtitlesContextMenu(x, y); }, false, false, true});
    std::wstring styleProfLabel = L"Style Profiles...";
    if (!settings.activeSubtitleProfile.empty())
        styleProfLabel = L"Style Profiles (" + Utf8ToUtf16(settings.activeSubtitleProfile) + L")...";
    contextMenuItems.push_back({"sub_style_profiles", styleProfLabel, L"\u25B8", IconType::Subtitles, false, false, false,
                                [this, x, y]() { OpenSubtitleProfilesContextMenu(x, y); }, false, false, true});
    wchar_t safeAreaBuf[64];
    if (settings.subSafeAreaX > 0 || settings.subSafeAreaY > 0) {
        swprintf_s(safeAreaBuf, L"Subtitle Safe Area (%d%% H, %d%% V)...", settings.subSafeAreaX, settings.subSafeAreaY);
    } else { swprintf_s(safeAreaBuf, L"Subtitle Safe Area (Off)..."); }
    contextMenuItems.push_back({ "sub_safe_area_menu", safeAreaBuf, L"\u25B8", IconType::SettingsGear, false, false, false, [this, x, y]() {
        OpenSubtitleSafeAreaContextMenu(x, y);
    }, false, false, true});
    PresentPlayerContextMenu(x, y, 238.0f, true, PlayerMenuReveal::Cursor);
}

void App::OpenSubtitleSafeAreaContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenSubtitlesContextMenu(x, y); } },
        { L"Reset", IconType::Refresh, false, false, false, true, [this, x, y]() {
            settings.subSafeAreaX = 0;
            settings.subSafeAreaY = 0;
            ApplyPlayerPreferences();
            SaveSettings();
            ShowOSD(IconType::Subtitles, L"Subtitle Safe Area Reset");
            OpenSubtitleSafeAreaContextMenu(x, y);
        } });
    struct SafeAreaPreset { const char* id; const wchar_t* label; int margin; const wchar_t* message; };
    static constexpr SafeAreaPreset presets[] = {
        {"preset_off", L"Off / None (0%)", 0, L"Subtitle Safe Area: Off (0%)"},
        {"preset_action", L"Action Safe (5%)", 5, L"Subtitle Safe Area: Action Safe (5%)"},
        {"preset_title", L"Title Safe (10%)", 10, L"Subtitle Safe Area: Title Safe (10%)"},
        {"preset_wide", L"Wide / Large (15%)", 15, L"Subtitle Safe Area: Wide (15%)"},
        {"preset_cinema", L"Cinema (20%)", 20, L"Subtitle Safe Area: Cinema (20%)"},
    };
    for (const auto& preset : presets) {
        const bool selected = settings.subSafeAreaX == preset.margin && settings.subSafeAreaY == preset.margin;
        contextMenuItems.push_back({preset.id, preset.label, selected ? L"\u2713" : L"", IconType::SettingsGear, false, selected, false, [this, x, y, preset]() {
            settings.subSafeAreaX = preset.margin;
            settings.subSafeAreaY = preset.margin;
            ApplyPlayerPreferences();
            SaveSettings();
            ShowOSD(IconType::Subtitles, preset.message);
            OpenSubtitleSafeAreaContextMenu(x, y);
        }, false, false, true});
    }
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::Cursor);
}

bool App::PumpOnlineSubtitleJobs() {
    const std::wstring currentMedia = player.GetCurrentFilePath().empty()
        ? playbackSession.GetCurrentFilePath() : player.GetCurrentFilePath();
    bool changed = onlineSubtitleJobs.SyncContext(currentMedia, settings, mediaGenerationToken, playbackSession.IsPrivateMode());
    changed = onlineSubtitleJobs.PumpSearch() || changed;
    if (auto download = onlineSubtitleJobs.TakeDownload()) {
        changed = true;
        if (download->success && !download->savedFilePath.empty()) {
            const bool loaded = player.LoadExternalSubtitle(download->savedFilePath, false);
            if (loaded) {
                subtitles.Clear();
                ShowOSD(IconType::Subtitles, L"Loaded Online Subtitle: " + GetFileNameFromPath(download->savedFilePath));
            } else ShowOSD(IconType::Subtitles, L"Downloaded but could not load");
        } else if (download->status != OnlineSubtitleStatus::Cancelled) {
            ShowOSD(IconType::Subtitles, L"Download Failed: " +
                (download->errorMessage.empty() ? std::wstring(L"Download failed") : download->errorMessage));
        }
    }
    if (changed && isContextMenuOpen && !contextMenuItems.empty() && contextMenuItems.front().id == "onl_title")
        OpenOnlineSubtitlesContextMenu(irisAnchorCursor.x, irisAnchorCursor.y);
    return changed;
}

void App::OpenOnlineSubtitlesContextMenu(float x, float y) {
    const bool refreshing = isContextMenuOpen && !contextMenuItems.empty() && contextMenuItems.front().id == "onl_title";
    const int oldHovered = hoveredContextMenuItem;
    const std::string hoveredId = oldHovered >= 0 && static_cast<size_t>(oldHovered) < contextMenuItems.size()
        ? contextMenuItems[oldHovered].id : std::string();
    if (!refreshing && !isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenSubtitlesContextMenu(x, y); } },
        { L"Retry", IconType::Refresh, false, false, !settings.onlineSubtitlesEnabled, true, [this, x, y]() {
            onlineSubtitleJobs.RetrySearch(settings);
            OpenOnlineSubtitlesContextMenu(x, y);
        } });
    contextMenuItems.push_back({ "onl_title", L"Online Subtitles", L"", IconType::Subtitles, false, false, false, nullptr, true });
    contextMenuItems.push_back({ "sep_onl1", L"", L"", IconType::Play, true });
    const std::wstring currentMedia = player.GetCurrentFilePath().empty()
        ? playbackSession.GetCurrentFilePath() : player.GetCurrentFilePath();
    onlineSubtitleJobs.SyncContext(currentMedia, settings, mediaGenerationToken, playbackSession.IsPrivateMode());
    if (!settings.onlineSubtitlesEnabled) {
        contextMenuItems.push_back({ "onl_dis_info", L"Online Retrieval is Disabled", L"", IconType::Subtitles, false, false, false, nullptr, true });
        contextMenuItems.push_back({ "onl_enable", L"Enable Online Subtitles (Opt-In)", L"", IconType::Plus, false, false, false, [this, x, y]() {
            settings.onlineSubtitlesEnabled = true;
            SaveSettings();
            ShowOSD(IconType::Subtitles, L"Online Subtitles Enabled");
            OpenOnlineSubtitlesContextMenu(x, y);
        }, false, false, true});
    } else if (currentMedia.empty()) {
        contextMenuItems.push_back({ "onl_no_media", L"No media file currently active", L"", IconType::Subtitles, false, false, false, nullptr, true });
    } else {
        const std::wstring query = OnlineSubtitleService::DeriveSearchQuery(currentMedia);
        const std::wstring qDisplay = query.length() > 22 ? query.substr(0, 22) + L"..." : query;
        contextMenuItems.push_back({ "onl_query", L"Query: " + qDisplay, L"", IconType::Subtitles, false, false, false, nullptr, true });
        contextMenuItems.push_back({ "sep_onl2", L"", L"", IconType::Play, true });
        onlineSubtitleJobs.EnsureSearch(settings);
        if (onlineSubtitleJobs.GetSearchState() == OnlineSubtitleJobs::SearchState::Loading) {
            contextMenuItems.push_back({ "onl_loading", L"Searching online subtitles...", L"", IconType::Subtitles, false, false, false, nullptr, true });
        } else if (onlineSubtitleJobs.IsDownloading()) {
            contextMenuItems.push_back({ "onl_downloading", L"Downloading subtitle...", L"", IconType::Subtitles, false, false, false, nullptr, true });
        } else {
            const auto& searchRes = onlineSubtitleJobs.GetSearchResult();
            if (!searchRes.success) {
                std::wstring errLabel = searchRes.errorMessage.empty() ? L"Search failed" : searchRes.errorMessage;
                if (errLabel.length() > 30) errLabel = errLabel.substr(0, 30) + L"...";
                contextMenuItems.push_back({ "onl_err", errLabel, L"", IconType::Subtitles, false, false, false, nullptr, true });
                contextMenuItems.push_back({ "onl_retry", L"Retry Search", L"", IconType::Refresh, false, false, false, [this, x, y]() {
                    onlineSubtitleJobs.RetrySearch(settings); OpenOnlineSubtitlesContextMenu(x, y);
                }, false, false, true});
            } else if (searchRes.searchResults.empty()) {
                contextMenuItems.push_back({ "onl_none", L"No subtitles found", L"", IconType::Subtitles, false, false, false, nullptr, true });
            } else {
                for (size_t i = 0; i < searchRes.searchResults.size() && i < 8; ++i) {
                    const auto& item = searchRes.searchResults[i];
                    std::wstring label = item.title;
                    if (label.length() > 22) label = label.substr(0, 22) + L"...";
                    if (!item.language.empty()) label += L" [" + item.language + L"]";
                    contextMenuItems.push_back({ "onl_item_" + std::to_string(i), label, L"", IconType::Subtitles, false, false, false,
                        [this, x, y, item, currentMedia]() {
                            const std::wstring activeMedia = player.GetCurrentFilePath().empty()
                                ? playbackSession.GetCurrentFilePath() : player.GetCurrentFilePath();
                            onlineSubtitleJobs.SyncContext(activeMedia, settings, mediaGenerationToken, playbackSession.IsPrivateMode());
                            if (onlineSubtitleJobs.StartDownload(item, currentMedia, settings))
                                ShowOSD(IconType::Subtitles, L"Downloading online subtitle...");
                            OpenOnlineSubtitlesContextMenu(x, y);
                        }, false, false, true});
                }
            }
        }
    }
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::Cursor);
    if (refreshing && !hoveredId.empty()) {
        for (size_t i = 0; i < contextMenuItems.size(); ++i)
            if (contextMenuItems[i].id == hoveredId) { hoveredContextMenuItem = static_cast<int>(i); break; }
    }
    MarkAccessibilityDirty();
}

void App::OpenSubtitleProfilesContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenSubtitlesContextMenu(x, y); } },
        { L"Save...", IconType::Plus, true, false, false, false, [this]() { ShowSaveSubtitleProfileModal(); } });
    contextMenuItems.push_back({"save_sub_prof", L"Save Current Style as Profile...", L"", IconType::Plus, false, false, false,
                                [this]() { ShowSaveSubtitleProfileModal(); }});
    contextMenuItems.push_back({ "restore_sub_def", L"Restore Default Style", L"", IconType::Refresh, false, false, false, [this, x, y]() {
        RestoreSubtitleStyleDefault();
        OpenSubtitleProfilesContextMenu(x, y);
    }, false, false, true});
    if (!settings.subtitleProfiles.empty()) {
        contextMenuItems.push_back({ "sep_prof2", L"", L"", IconType::Play, true });
        for (size_t i = 0; i < settings.subtitleProfiles.size(); ++i) {
            const auto& prof = settings.subtitleProfiles[i];
            bool isActive = (_stricmp(settings.activeSubtitleProfile.c_str(), prof.name.c_str()) == 0);
            std::wstring label = Utf8ToUtf16(prof.name);
            std::string pName = prof.name;
            contextMenuItems.push_back({ "apply_prof_" + std::to_string(i), label, isActive ? L"\u2713" : L"", IconType::Subtitles, false, isActive, false, [this, pName, x, y]() {
                ApplySubtitleStyleProfile(pName);
                OpenSubtitleProfilesContextMenu(x, y);
            }, false, false, true});
        }
        contextMenuItems.push_back({ "sep_prof3", L"", L"", IconType::Play, true });
        contextMenuItems.push_back({"rename_prof_menu", L"Rename Profile...", L"\u25B8", IconType::FolderPlaylist, false, false, false,
                                    [this, x, y]() { OpenRenameSubtitleProfileContextMenu(x, y); }, false, false, true});
        contextMenuItems.push_back({"del_prof_menu", L"Delete Profile...", L"\u25B8", IconType::Trash, false, false, false,
                                    [this, x, y]() { OpenDeleteSubtitleProfileContextMenu(x, y); }, false, false, true});
    }
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::Cursor);
}

void App::OpenRenameSubtitleProfileContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenSubtitleProfilesContextMenu(x, y); } },
        { L"Save...", IconType::Plus, true, false, false, false, [this]() { ShowSaveSubtitleProfileModal(); } });
    for (size_t i = 0; i < settings.subtitleProfiles.size(); ++i) {
        const auto& prof = settings.subtitleProfiles[i];
        std::string pName = prof.name;
        contextMenuItems.push_back({"ren_prof_" + std::to_string(i), Utf8ToUtf16(prof.name), L"", IconType::FolderPlaylist, false, false,
                                    false, [this, pName]() { ShowRenameSubtitleProfileModal(pName); }});
    }
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::Cursor);
}

void App::OpenDeleteSubtitleProfileContextMenu(float x, float y) {
    if (!isContextMenuOpen) PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    SetIrisShoulders(
        { L"Back", IconType::ChevronLeft, true, false, false, true, [this, x, y]() { OpenSubtitleProfilesContextMenu(x, y); } },
        { L"Save...", IconType::Plus, true, false, false, false, [this]() { ShowSaveSubtitleProfileModal(); } });
    for (size_t i = 0; i < settings.subtitleProfiles.size(); ++i) {
        const auto& prof = settings.subtitleProfiles[i];
        std::string pName = prof.name;
        contextMenuItems.push_back({"del_prof_" + std::to_string(i), Utf8ToUtf16(prof.name), L"", IconType::Trash, false, false, true,
                                    [this, pName]() { ShowDeleteSubtitleProfileModal(pName); }});
    }
    PresentPlayerContextMenu(x, y, 238.0f, false, PlayerMenuReveal::Cursor);
}

void App::OpenSeekStepContextMenu(float x, float y) {
    PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    double presets[] = { 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0 };
    const wchar_t *presetLabels[] = {L"1.0 second",   L"2.0 seconds",  L"5.0 seconds (Default)", L"10.0 seconds",
                                     L"15.0 seconds", L"30.0 seconds", L"60.0 seconds (1 min)"};
    for (int i = 0; i < 7; ++i) {
        double val = presets[i];
        bool isSel = (std::abs(settings.arrowSeekStepSec - val) < 0.05);
        contextMenuItems.push_back({
            "seek_" + std::to_string((int)val), presetLabels[i], isSel ? L"✓" : L"", IconType::SpeedGauge, false, false, false,
            [this, val]() { settings.arrowSeekStepSec = val; SaveSettings(); ApplyPlayerPreferences(); InvalidateRect(hWnd, NULL, FALSE); }
        });
    }
    // Separator
    contextMenuItems.push_back({ "", L"", L"", IconType::Play, true, false, false, nullptr });
    // Custom option
    bool isCustom = true;
    for (double p : presets) {
        if (std::abs(settings.arrowSeekStepSec - p) < 0.05) { isCustom = false; break; }
    }
    std::wstring customLabel = L"Custom Duration... (Type Value)";
    if (isCustom) { wchar_t cbuf[32]; swprintf_s(cbuf, L"Custom (%.1fs)...", settings.arrowSeekStepSec); customLabel = cbuf; }
    contextMenuItems.push_back({
        "seek_custom", customLabel, isCustom ? L"✓" : L"", IconType::SettingsGear, false, false, false,
        [this]() { ShowCustomSeekStepModal(); }
    });
    PresentHubContextMenu(x, y, 220.0f);
}

void App::OpenFrameSeekContextMenu(float x, float y) {
    PushFocusSnapshot(TransientSurfaceType::ContextMenu);
    contextMenuItems.clear();
    int presets[] = { 1, 2, 5, 10, 24, 30, 60 };
    const wchar_t* presetLabels[] = {
        L"1 frame (Default)", L"2 frames", L"5 frames", L"10 frames", L"24 frames (1s @ 24fps)", L"30 frames (1s @ 30fps)",
        L"60 frames (1s @ 60fps)"};
    for (int i = 0; i < 7; ++i) {
        int val = presets[i];
        bool isSel = (settings.ctrlSeekFrames == val);
        contextMenuItems.push_back({
            "frame_" + std::to_string(val), presetLabels[i], isSel ? L"✓" : L"", IconType::AspectFrame, false, false, false,
            [this, val]() { settings.ctrlSeekFrames = val; SaveSettings(); ApplyPlayerPreferences(); InvalidateRect(hWnd, NULL, FALSE); }
        });
    }
    // Separator
    contextMenuItems.push_back({ "", L"", L"", IconType::Play, true, false, false, nullptr });
    // Custom option
    bool isCustom = true;
    for (int p : presets) {
        if (settings.ctrlSeekFrames == p) { isCustom = false; break; }
    }
    std::wstring customLabel = L"Custom Frames... (Type Value)";
    if (isCustom)
        customLabel = L"Custom (" + std::to_wstring(settings.ctrlSeekFrames) + (settings.ctrlSeekFrames == 1 ? L" frame" : L" frames") + L")...";
    contextMenuItems.push_back({"frame_custom", customLabel, isCustom ? L"✓" : L"", IconType::SettingsGear, false, false, false,
                                [this]() { ShowCustomFrameSeekModal(); }});
    PresentHubContextMenu(x, y, 220.0f);
}

float App::GetFlyoutHeight() const {
    float dp = std::max(0.5f, overlayRenderer.GetDpiScale());
    if (currentFlyoutMenu == "main") return 360.0f * dp;
    if (currentFlyoutMenu == "audiodelay" || currentFlyoutMenu == "subdelay") return 180.0f * dp;
    return 340.0f * dp;
}

int App::GetFlyoutOptionCount() const {
    if (currentFlyoutMenu == "main") return 8;
    if (currentFlyoutMenu == "speed") return 9;
    if (currentFlyoutMenu == "aspect") return 6;
    if (currentFlyoutMenu == "subtitles") return (int)player.GetSubtitleTracks().size() + 1;
    if (currentFlyoutMenu == "audio") return (int)player.GetAudioTracks().size();
    if (currentFlyoutMenu == "queue") {
        const size_t queueSize = playbackSession.GetQueueSize();
        if (queueSize == 0) return 0;
        const size_t first = std::min(queueFlyoutFirstVisible, queueSize - 1);
        return static_cast<int>(std::min<size_t>(7, queueSize - first));
    }
    return 0;
}

D2D1_RECT_F App::PlaybackSpeedEditorBounds() const {
    RECT client{};
    GetClientRect(hWnd, &client);
    const float dp = std::max(0.5f, overlayRenderer.GetDpiScale());
    const auto bounds = D2D1::RectF(client.right - 300 * dp, client.bottom - 86 * dp - GetFlyoutHeight(),
        client.right - 20 * dp, client.bottom - 86 * dp);
    return PlaybackFlyoutLayout::SpeedInput(bounds, dp);
}

bool App::CommitPlaybackSpeedInput() {
    double speed = 0;
    if (!PlaybackFlyoutLayout::ParseSpeed(Text16ToWideString(playbackSpeedInput.GetText()), speed)) {
        ShowOSD(IconType::SpeedGauge, L"Enter a speed from 0.25 to 4.00");
        return false;
    }
    commandDispatcher.Execute(AppCommand::SetSpeed, CommandArgs{ speed });
    return true;
}

void App::UpdatePlaybackSpeedDrag(float clientX) {
    RECT client{};
    GetClientRect(hWnd, &client);
    const float dp = std::max(0.5f, overlayRenderer.GetDpiScale());
    const auto track = PlaybackFlyoutLayout::SpeedTrack(D2D1::RectF(client.right - 300 * dp, 0, client.right - 20 * dp, 340 * dp), dp);
    const double ratio = std::clamp((double)(clientX - track.left) / std::max(1.0f, RectWidth(track)), 0.0, 1.0);
    const double speed = std::round((0.25 + ratio * 2.75) * 100) / 100;
    if (std::abs(speed - playbackSpeedDragValue) < 0.0001) return;
    playbackSpeedDragValue = speed;
    player.SetPlaybackRate(speed);
    Render();
}

bool App::HandlePlaybackSpeedDragMessage(UINT message, LPARAM position) {
    if (!isPlaybackSpeedDragging) return false;
    if (message == WM_MOUSEMOVE) { UpdatePlaybackSpeedDrag(static_cast<float>(GET_X_LPARAM(position))); return true; }
    if (message != WM_LBUTTONUP && message != WM_CAPTURECHANGED) return false;
    if (message == WM_LBUTTONUP) UpdatePlaybackSpeedDrag(static_cast<float>(GET_X_LPARAM(position)));
    isPlaybackSpeedDragging = false;
    if (GetCapture() == hWnd || GetCapture() == hOverlayWnd) ReleaseCapture();
    if (playbackSession.IsOpen()) commandDispatcher.Execute(AppCommand::SetSpeed, CommandArgs{ playbackSpeedDragValue });
    Render();
    return true;
}

void App::HandleFlyoutClick(float localX, float localY) {
    const float dp = std::max(0.5f, overlayRenderer.GetDpiScale());
    const auto bounds = D2D1::RectF(0, 0, 280.0f * dp, GetFlyoutHeight());
    const int idx = PlaybackFlyoutLayout::HitTest(currentFlyoutMenu, GetFlyoutOptionCount(), bounds, localX, localY, dp);
    if (activeTextInputField == TextInputField::PlaybackSpeed &&
        (currentFlyoutMenu != "speed" || !PtInRectF(PlaybackFlyoutLayout::SpeedInput(bounds, dp), localX, localY))) BlurTextInput();
    if (idx == PlaybackFlyoutLayout::Back) {
        currentFlyoutMenu = "main";
        hoveredFlyoutOption = PlaybackFlyoutLayout::None;
        return;
    }
    if (currentFlyoutMenu == "speed") {
        if (PtInRectF(PlaybackFlyoutLayout::SpeedInput(bounds, dp), localX, localY)) {
            if (activeTextInputField == TextInputField::PlaybackSpeed) {
                const auto editor = PlaybackSpeedEditorBounds();
                const bool extend = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                SetTextInputCaretFromMouse(editor.left + localX - (bounds.right * 0.5f - 56 * dp), editor.top + localY - 50 * dp, extend);
                textInputDragAnchor = playbackSpeedInput.GetAnchor();
                textInputDragging = true;
                SetCapture(hWnd);
                return;
            }
            wchar_t text[32];
            swprintf_s(text, L"%.2f", player.GetPlaybackRate());
            playbackSpeedInput.SetText(WideStringToText16(text));
            playbackSpeedInput.SelectAll();
            FocusTextInput(TextInputField::PlaybackSpeed);
            return;
        }
        const auto track = PlaybackFlyoutLayout::SpeedTrack(bounds, dp);
        if (PtInRectF(track, localX, localY)) {
            isPlaybackSpeedDragging = true;
            playbackSpeedDragValue = player.GetPlaybackRate();
            SetCapture(hOverlayWnd ? hOverlayWnd : hWnd);
            RECT client{};
            GetClientRect(hWnd, &client);
            UpdatePlaybackSpeedDrag(client.right - 300 * dp + localX);
        } else if (idx >= 0 && idx < 9) {
            commandDispatcher.Execute(AppCommand::SetSpeed, CommandArgs{ PlaybackFlyoutLayout::Speeds[idx] });
        }
        return;
    }
    if (idx < 0) return;
    if (currentFlyoutMenu == "main") {
        switch (idx) {
        case 0: currentFlyoutMenu = "subtitles"; break;
        case 1: currentFlyoutMenu = "audio"; break;
        case 2: currentFlyoutMenu = "speed"; break;
        case 3: currentFlyoutMenu = "aspect"; break;
        case 4: {
            currentFlyoutMenu = "queue";
            const size_t count = playbackSession.GetQueueSize();
            const size_t current = count ? std::min(playbackSession.GetQueueIndex(), count - 1) : 0;
            queueFlyoutFirstVisible = std::min(count > 7 ? count - 7 : 0, current > 3 ? current - 3 : 0);
            break;
        }
        case 5: commandDispatcher.Execute(AppCommand::ToggleLoop); break;
        case 6: commandDispatcher.Execute(AppCommand::ToggleAutoplay); break;
        case 7: commandDispatcher.Execute(AppCommand::TogglePrivateMode); break;
        }
        hoveredFlyoutOption = PlaybackFlyoutLayout::None;
    } else if (currentFlyoutMenu == "queue") {
        const auto queue = playbackSession.GetQueue();
        if (queue.empty()) return;
        const size_t target = std::min(queueFlyoutFirstVisible, queue.size() - 1) + static_cast<size_t>(idx);
        if (target >= queue.size()) return;
        PersistCurrentPlaybackProgress();
        playbackSession.SetQueue(queue, target);
        OpenAndPlayMedia(queue[target], 0.0, true);
    } else if (currentFlyoutMenu == "aspect") {
        const char* aspects[] = { "auto", "16:9", "4:3", "21:9", "fill", "cover" };
        commandDispatcher.Execute(AppCommand::SetAspectRatio, CommandArgs{ 0.0, 0, aspects[idx] });
        currentFlyoutMenu = "main";
    } else if (currentFlyoutMenu == "subtitles") {
        const auto tracks = player.GetSubtitleTracks();
        if (idx == 0) commandDispatcher.Execute(AppCommand::SelectSubtitleTrack, CommandArgs{});
        else if (static_cast<size_t>(idx - 1) < tracks.size()) {
            const auto& track = tracks[idx - 1];
            commandDispatcher.Execute(AppCommand::SelectSubtitleTrack, CommandArgs{ 0.0, track.id, "", track.title, track.lang });
        }
        currentFlyoutMenu = "main";
    } else if (currentFlyoutMenu == "audio") {
        const auto tracks = player.GetAudioTracks();
        if (static_cast<size_t>(idx) >= tracks.size()) return;
        const auto& track = tracks[idx];
        commandDispatcher.Execute(AppCommand::SelectAudioTrack, CommandArgs{ 0.0, track.id, "", track.title, track.lang });
        currentFlyoutMenu = "main";
    }
}

} // namespace Aperture
