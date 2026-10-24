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
#include "ChannelPolicy.h"
#include "FileAssociations.h"
#include "JumpList.h"
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


TextInputModel* App::ActiveTextInputModel() {
    if (activeTextInputField == TextInputField::PlaylistSearch) return &playlistSearchInput;
    if (activeTextInputField == TextInputField::MediaSearch) return &mediaSearchInput;
    if (activeTextInputField == TextInputField::Modal) return &modalInput;
    if (activeTextInputField == TextInputField::PlaybackSpeed) return &playbackSpeedInput;
    if (activeTextInputField == TextInputField::InlineRename) return &inlineRenameInput;
    if (activeTextInputField == TextInputField::GlobalSearch) return &globalSearchModel.GetTextInputModel();
    return nullptr;
}


const TextInputModel* App::ActiveTextInputModel() const {
    if (activeTextInputField == TextInputField::PlaylistSearch) return &playlistSearchInput;
    if (activeTextInputField == TextInputField::MediaSearch) return &mediaSearchInput;
    if (activeTextInputField == TextInputField::Modal) return &modalInput;
    if (activeTextInputField == TextInputField::PlaybackSpeed) return &playbackSpeedInput;
    if (activeTextInputField == TextInputField::InlineRename) return &inlineRenameInput;
    if (activeTextInputField == TextInputField::GlobalSearch) return &globalSearchModel.GetTextInputModel();
    return nullptr;
}


void App::FocusTextInput(TextInputField field) {
    if (activeTextInputField != field) {
        if (TextInputModel* previous = ActiveTextInputModel()) previous->EndComposition();
        activeTextInputField = field;
    }
    isPlaylistSearchFocused = field == TextInputField::PlaylistSearch;
    isMediaSearchFocused = field == TextInputField::MediaSearch;
    textInputDragging = false;
    hasPendingHighSurrogate = false;
    pendingImeEcho.clear();
    if (hWnd) KillTimer(hWnd, kTextInputImeEchoTimer);
    textInputCaretVisible = true;
    if (hWnd) { if (IsWindowVisible(hWnd)) SetFocus(hWnd); SetTimer(hWnd, kTextInputCaretTimer, 530, nullptr); }
    UpdateImeCandidateWindow();
    MarkAccessibilityDirty();
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::StopTextInputCaretTimer() { if (hWnd) KillTimer(hWnd, kTextInputCaretTimer); textInputCaretVisible = false; }


void App::BlurTextInput() {
    if (activeTextInputField == TextInputField::PlaybackSpeed) CommitPlaybackSpeedInput();
    if (TextInputModel* input = ActiveTextInputModel()) input->EndComposition();
    if (isInlineRenaming) { CommitInlinePlaylistRename(); }
    activeTextInputField = TextInputField::None;
    isPlaylistSearchFocused = false;
    isMediaSearchFocused = false;
    textInputDragging = false;
    hasPendingHighSurrogate = false;
    pendingImeEcho.clear();
    if (hWnd) KillTimer(hWnd, kTextInputImeEchoTimer);
    StopTextInputCaretTimer();
    MarkAccessibilityDirty();
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ResetTextInputCaret() {
    textInputCaretVisible = true;
    if (hWnd && activeTextInputField != TextInputField::None) SetTimer(hWnd, kTextInputCaretTimer, 530, nullptr);
    UpdateImeCandidateWindow();
}


void App::MarkTextInputEdit(bool committed) {
    ResetTextInputCaret();
    if (committed) MarkAccessibilityDirty();
    if (activeTextInputField == TextInputField::PlaylistSearch) { playlistDetailScrollY = 0.0f; ClampScrollOffsets(); }
    if (activeTextInputField == TextInputField::MediaSearch) {
        mediaScrollY = 0.0f;
        m_mediaQueryEngine.InvalidateCache();
        ClampScrollOffsets();
    }
    if (activeTextInputField == TextInputField::GlobalSearch)
        globalSearchModel.SetQuery(Text16ToWideString(globalSearchModel.GetTextInputModel().GetText()), library, true);
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ClearMediaSearch() {
    if (!mediaSearchInput.GetText().empty()) {
        mediaSearchInput.SetText(u"");
        m_mediaQueryEngine.InvalidateCache();
        mediaScrollY = 0.0f;
        ClampScrollOffsets();
        MarkAccessibilityDirty();
        InvalidateRect(hWnd, NULL, FALSE);
    }
}


void App::FocusMediaSearch() {
    if (currentTab != TabView::Media) { currentTab = TabView::Media; activeFocusPane = FocusPane::MediaList; }
    FocusTextInput(TextInputField::MediaSearch);
    mediaSearchInput.SelectAll();
    InvalidateRect(hWnd, NULL, FALSE);
}


TextInputModel::ClipboardCallbacks App::MakeClipboardCallbacks() {
    TextInputModel::ClipboardCallbacks callbacks;
    callbacks.readText = [this]() {
        if (!hWnd || !OpenClipboard(hWnd)) return TextInputModel::Text{};
        TextInputModel::Text result;
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (data) {
            const wchar_t* value = static_cast<const wchar_t*>(GlobalLock(data));
            if (value) { result = WideStringToText16(std::wstring(value)); GlobalUnlock(data); }
        }
        CloseClipboard();
        return result;
    };
    callbacks.writeText = [this](const TextInputModel::Text& value) {
        if (!hWnd || !OpenClipboard(hWnd)) return;
        const std::wstring wide = Text16ToWideString(value);
        const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (handle) {
            void* target = GlobalLock(handle);
            if (target) {
                CopyMemory(target, wide.c_str(), bytes);
                GlobalUnlock(handle);
                EmptyClipboard();
                if (!SetClipboardData(CF_UNICODETEXT, handle)) GlobalFree(handle);
                handle = nullptr;
            }
        }
        if (handle) GlobalFree(handle);
        CloseClipboard();
    };
    return callbacks;
}


bool App::HandleTextInputKeyDown(WPARAM wParam, LPARAM) {
    TextInputModel* input = ActiveTextInputModel();
    if (!input) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    // WM_CHAR inserts printable spaces; do not activate a modal or playback
    // shortcut from the corresponding key-down while an editor owns focus.
    if (wParam == VK_SPACE && !ctrl && !alt) return true;
    if (ctrl && !alt) {
        if (wParam == 'A' || wParam == 'a' || wParam == 'C' || wParam == 'c' || wParam == 'X' || wParam == 'x' || wParam == 'V' ||
            wParam == 'v' || wParam == 'Z' || wParam == 'z' || wParam == 'Y' || wParam == 'y') {
            const TextInputModel::Text before = input->GetText();
            input->HandleCtrlShortcut(static_cast<char16_t>(wParam), MakeClipboardCallbacks());
            MarkTextInputEdit(input->GetText() != before);
            return true;
        }
    }
    if (wParam == VK_SHIFT || wParam == VK_CONTROL || wParam == VK_MENU) return true;
    if (wParam == VK_LEFT || wParam == VK_RIGHT) {
        const bool changed = wParam == VK_LEFT ? input->MoveLeft(shift, ctrl) : input->MoveRight(shift, ctrl);
        if (changed || shift) MarkTextInputEdit(false);
        return true;
    }
    if (wParam == VK_HOME || wParam == VK_END) {
        const bool changed = wParam == VK_HOME ? input->MoveHome(shift) : input->MoveEnd(shift);
        if (changed || shift) MarkTextInputEdit(false);
        return true;
    }
    if (wParam == VK_BACK || wParam == VK_DELETE) {
        const TextInputModel::Text before = input->GetText();
        if (wParam == VK_BACK) input->DeleteBackward(ctrl);
        else input->DeleteForward(ctrl);
        MarkTextInputEdit(input->GetText() != before);
        return true;
    }
    if (wParam == VK_ESCAPE) {
        if (activeTextInputField == TextInputField::PlaybackSpeed && !input->IsComposing()) {
            activeTextInputField = TextInputField::None;
            BlurTextInput();
            return true;
        }
        if (input->IsComposing()) { input->CancelComposition(); MarkTextInputEdit(false); return true; }
        if (activeTextInputField == TextInputField::InlineRename) { CancelInlinePlaylistRename(); return true; }
        if (activeTextInputField == TextInputField::PlaylistSearch || activeTextInputField == TextInputField::MediaSearch) {
            if (!input->GetText().empty() || input->HasSelection()) {
                input->SelectAll();
                const TextInputModel::Text before = input->GetText();
                input->ReplaceSelection({});
                MarkTextInputEdit(input->GetText() != before);
            } else { BlurTextInput(); }
            return true;
        }
        if (activeTextInputField == TextInputField::Modal) { DismissActiveModal(false); return true; }
    }
    if (wParam == VK_TAB && activeTextInputField == TextInputField::PlaybackSpeed) {
        if (!CommitPlaybackSpeedInput()) return true;
        activeTextInputField = TextInputField::None;
        BlurTextInput();
        return false;
    }
    if (wParam == VK_RETURN) {
        if (activeTextInputField == TextInputField::PlaybackSpeed) {
            input->EndComposition();
            if (CommitPlaybackSpeedInput()) { activeTextInputField = TextInputField::None; BlurTextInput(); }
            return true;
        }
        if (activeTextInputField == TextInputField::InlineRename) { CommitInlinePlaylistRename(); return true; }
        if (activeTextInputField == TextInputField::Modal) {
            const auto confirm = modalOnConfirm;
            BlurTextInput();
            if (confirm) confirm();
            return true;
        }
        if (activeTextInputField == TextInputField::PlaylistSearch || activeTextInputField == TextInputField::MediaSearch) {
            BlurTextInput();
            return true;
        }
    }
    // Do not let list navigation, player shortcuts, F2, or Delete actions
    // observe keystrokes while an editor owns focus. Up/Down are intentionally
    // consumed too; a single-line field has no vertical navigation.
    return true;
}


bool App::HandleTextInputChar(wchar_t ch) {
    TextInputModel* input = ActiveTextInputModel();
    if (!input) return false;
    // IMEs can expose the committed result both through GCS_RESULTSTR and a
    // following WM_CHAR/WM_UNICHAR sequence. Suppress only an exact matching
    // echo; a different next character starts a normal edit immediately.
    if (!pendingImeEcho.empty()) {
        if (static_cast<char16_t>(ch) == pendingImeEcho.front()) { pendingImeEcho.erase(pendingImeEcho.begin()); return true; }
        pendingImeEcho.clear();
    }
    auto insertText = [this, input](const TextInputModel::Text& text) {
        const TextInputModel::Text before = input->GetText();
        input->InsertText(text);
        MarkTextInputEdit(input->GetText() != before);
    };
    if (IsHighUtf16Unit(ch)) {
        if (hasPendingHighSurrogate) insertText(TextInputModel::Text(1, static_cast<char16_t>(pendingHighSurrogate)));
        pendingHighSurrogate = ch;
        hasPendingHighSurrogate = true;
        ResetTextInputCaret();
        return true;
    }
    if (IsLowUtf16Unit(ch) && hasPendingHighSurrogate) {
        TextInputModel::Text pair;
        pair.push_back(static_cast<char16_t>(pendingHighSurrogate));
        pair.push_back(static_cast<char16_t>(ch));
        hasPendingHighSurrogate = false;
        pendingHighSurrogate = 0;
        insertText(pair);
        return true;
    }
    if (hasPendingHighSurrogate) {
        insertText(TextInputModel::Text(1, static_cast<char16_t>(pendingHighSurrogate)));
        hasPendingHighSurrogate = false;
        pendingHighSurrogate = 0;
    }
    if (ch < 32 || ch == 127) return true;
    insertText(TextInputModel::Text(1, static_cast<char16_t>(ch)));
    return true;
}


bool App::HandleTextInputIme(UINT message, WPARAM, LPARAM lParam) {
    TextInputModel* input = ActiveTextInputModel();
    if (!input) return false;
    if (message == WM_IME_STARTCOMPOSITION) {
        hasPendingHighSurrogate = false;
        pendingImeEcho.clear();
        input->StartComposition();
        ResetTextInputCaret();
        InvalidateRect(hWnd, NULL, FALSE);
        return true;
    }
    if (message == WM_IME_COMPOSITION) {
        HIMC himc = ImmGetContext(hWnd);
        bool committed = false;
        if (himc) {
            if ((lParam & GCS_RESULTSTR) != 0) {
                const TextInputModel::Text result = WideStringToText16(ReadImeString(himc, GCS_RESULTSTR));
                input->CommitComposition(result);
                pendingImeEcho = result;
                if (hWnd && !pendingImeEcho.empty()) SetTimer(hWnd, kTextInputImeEchoTimer, 120, nullptr);
                committed = true;
            } else if ((lParam & GCS_COMPSTR) != 0) {
                LONG cursor = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
                input->UpdateComposition(WideStringToText16(ReadImeString(himc, GCS_COMPSTR)), cursor >= 0 ? static_cast<size_t>(cursor) : TextInputModel::kNoIndex);
            }
            ImmReleaseContext(hWnd, himc);
        }
        MarkTextInputEdit(committed);
        UpdateImeCandidateWindow();
        return true;
    }
    if (message == WM_IME_ENDCOMPOSITION) {
        input->EndComposition();
        hasPendingHighSurrogate = false;
        MarkTextInputEdit(false);
        return true;
    }
    return false;
}


void App::UpdateImeCandidateWindow() {
    if (!hWnd || activeTextInputField == TextInputField::None) return;
    const TextInputModel* input = ActiveTextInputModel();
    if (!input) return;
    RECT rc{};
    GetClientRect(hWnd, &rc);
    const float winW = static_cast<float>(std::max<LONG>(1, rc.right - rc.left));
    const float winH = static_cast<float>(std::max<LONG>(1, rc.bottom - rc.top));
    const float dp = std::max(0.5f, renderer.GetDpiScale());
    float left = 0.0f, right = winW, bottom = winH;
    FontType font = FontType::Regular;
    if (activeTextInputField == TextInputField::PlaybackSpeed) {
        const auto box = PlaybackSpeedEditorBounds();
        left = box.left + 10 * dp; right = box.right - 10 * dp; bottom = box.bottom;
        font = FontType::Title;
    } else if (activeTextInputField == TextInputField::Modal) {
        const float boxW = std::min(460.0f * dp, std::max(320.0f * dp, winW - 48.0f * dp)), boxH = 204.0f * dp, cx = winW * 0.5f, cy = winH * 0.5f;
        const D2D1_RECT_F box = D2D1::RectF(cx - boxW * 0.5f, cy - boxH * 0.5f, cx + boxW * 0.5f, cy + boxH * 0.5f);
        left = box.left + 36.0f * dp;
        right = box.right - 36.0f * dp;
        bottom = box.top + 106.0f * dp;
        font = FontType::Medium;
    } else if (activeTextInputField == TextInputField::MediaSearch) {
        if (currentTab != TabView::Media) return;
        left = mediaSearchBtnBounds.left + 28.0f * dp;
        right = mediaSearchBtnBounds.right - (mediaSearchInput.GetText().empty() ? 8.0f : 26.0f) * dp;
        bottom = mediaSearchBtnBounds.bottom;
        font = FontType::Small;
    } else {
        const D2D1_RECT_F view = Presentation::HubView(winW, winH, dp);
        const float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(view) * 0.42f));
        const D2D1_RECT_F detail = D2D1::RectF(view.left + railW + 16.0f * dp, view.top, view.right, view.bottom);
        const float toolbarY = detail.top + 14.0f * dp + 50.0f * dp + 16.0f * dp + 112.0f * dp + 16.0f * dp;
        left = detail.left + 30.0f * dp;
        right = detail.left + 230.0f * dp - 8.0f * dp;
        bottom = toolbarY + 32.0f * dp;
        font = FontType::Small;
    }
    const std::wstring display = Text16ToWideString(input->GetDisplayText());
    size_t caret = input->IsComposing() ? input->GetDisplayCaret() : input->GetCaret();
    caret = SafeWideBoundary(display, caret);
    const float availableW = std::max(1.0f, right - left);
    const float caretWidth = renderer.MeasureString(display.substr(0, caret), font).width;
    const float scrollX = std::max(0.0f, caretWidth - availableW);
    const float caretX = left + caretWidth - scrollX;
    HIMC himc = ImmGetContext(hWnd);
    if (!himc) return;
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos.x = static_cast<LONG>(std::lround(caretX));
    composition.ptCurrentPos.y = static_cast<LONG>(std::lround(bottom + 2.0f * dp));
    composition.rcArea = RECT{ static_cast<LONG>(left), 0, static_cast<LONG>(right), static_cast<LONG>(bottom) };
    ImmSetCompositionWindow(himc, &composition);
    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = composition.ptCurrentPos;
    ImmSetCandidateWindow(himc, &candidate);
    ImmReleaseContext(hWnd, himc);
}


bool App::HitTestTextInput(float x, float y, std::size_t& outIndex) {
    const TextInputModel* input = ActiveTextInputModel();
    if (!input) return false;
    RECT rc{};
    GetClientRect(hWnd, &rc);
    const float winW = static_cast<float>(std::max<LONG>(1, rc.right - rc.left));
    const float winH = static_cast<float>(std::max<LONG>(1, rc.bottom - rc.top));
    const float dp = std::max(0.5f, renderer.GetDpiScale());
    float left = 0.0f, right = winW, top = 0.0f, bottom = winH;
    FontType font = FontType::Regular;
    if (activeTextInputField == TextInputField::PlaybackSpeed) {
        const auto box = PlaybackSpeedEditorBounds();
        left = box.left + 10 * dp; right = box.right - 10 * dp; top = box.top; bottom = box.bottom;
        font = FontType::Title;
    } else if (activeTextInputField == TextInputField::Modal) {
        if (activeModal != ModalType::NewPlaylist && activeModal != ModalType::NewSmartPlaylist && activeModal != ModalType::EditSmartPlaylist && activeModal != ModalType::BookmarkInput && activeModal != ModalType::EditMetadataOverrides && activeModal != ModalType::SaveSubtitleProfile && activeModal != ModalType::RenameSubtitleProfile) return false;
        const float boxW = std::min(460.0f * dp, std::max(320.0f * dp, winW - 48.0f * dp)), boxH = 204.0f * dp, cx = winW * 0.5f, cy = winH * 0.5f;
        const D2D1_RECT_F box = D2D1::RectF(cx - boxW * 0.5f, cy - boxH * 0.5f, cx + boxW * 0.5f, cy + boxH * 0.5f);
        left = box.left + 36.0f * dp;
        right = box.right - 36.0f * dp;
        top = box.top + 64.0f * dp;
        bottom = box.top + 106.0f * dp;
        font = FontType::Medium;
    } else if (activeTextInputField == TextInputField::InlineRename) {
        if (currentTab != TabView::Playlists) return false;
        const D2D1_RECT_F view = Presentation::HubView(winW, winH, dp);
        const float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(view) * 0.42f));
        const D2D1_RECT_F railBounds = D2D1::RectF(view.left, view.top, view.left + railW, view.bottom);
        float itemH = 52.0f * dp, gap = 6.0f * dp, curY = railBounds.top + 48.0f * dp - 2.0f * dp - playlistRailScrollY;
        int plIdx = -1;
        for (size_t i = 0; i < library.playlists.size(); ++i) {
            if (library.playlists[i].id == inlineRenamePlaylistId) { plIdx = (int)i; break; }
        }
        if (plIdx < 0) return false;
        float rowTop = curY + plIdx * (itemH + gap);
        D2D1_RECT_F itemRect = D2D1::RectF(railBounds.left + 8.0f * dp, rowTop, railBounds.right - 8.0f * dp, rowTop + itemH);
        float ringCX = itemRect.left + 32.0f * dp, ringRadius = 8.5f * dp;
        D2D1_RECT_F titleRect = D2D1::RectF(ringCX + ringRadius + 12.0f * dp, itemRect.top + 7.0f * dp, itemRect.right - 10.0f * dp, itemRect.top + 27.0f * dp);
        D2D1_RECT_F renameBox = D2D1::RectF(titleRect.left - 4.0f * dp, itemRect.top + 4.0f * dp, itemRect.right - 8.0f * dp, itemRect.top + 30.0f * dp);
        left = renameBox.left + 6.0f * dp;
        right = renameBox.right - 6.0f * dp;
        top = renameBox.top;
        bottom = renameBox.bottom;
        font = FontType::Medium;
    } else if (activeTextInputField == TextInputField::GlobalSearch) {
        if (!globalSearchModel.IsOpen()) return false;
        const float cardW = std::min(440.0f * dp, winW - 36.0f * dp);
        const float cardLeft = 18.0f * dp, cardTop = 76.0f * dp, closeLeft = cardLeft + cardW - 42.0f * dp;
        left = cardLeft + 24.0f * dp + 20.0f * dp;
        right = closeLeft - 20.0f * dp;
        top = cardTop + 10.0f * dp;
        bottom = cardTop + 42.0f * dp;
        font = FontType::Regular;
    } else if (activeTextInputField == TextInputField::MediaSearch) {
        if (currentTab != TabView::Media) return false;
        left = mediaSearchBtnBounds.left + 28.0f * dp;
        right = mediaSearchBtnBounds.right - (mediaSearchInput.GetText().empty() ? 8.0f : 26.0f) * dp;
        top = mediaSearchBtnBounds.top;
        bottom = mediaSearchBtnBounds.bottom;
        font = FontType::Small;
    } else {
        if (currentTab != TabView::Playlists) return false;
        const D2D1_RECT_F view = Presentation::HubView(winW, winH, dp);
        const float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(view) * 0.42f));
        const D2D1_RECT_F detail = D2D1::RectF(view.left + railW + 16.0f * dp, view.top, view.right, view.bottom);
        const float toolbarY = detail.top + 14.0f * dp + 50.0f * dp + 16.0f * dp + 112.0f * dp + 16.0f * dp;
        left = detail.left + 30.0f * dp;
        right = detail.left + 230.0f * dp - 8.0f * dp;
        top = toolbarY;
        bottom = toolbarY + 32.0f * dp;
        font = FontType::Small;
    }
    if ((!textInputDragging) && (y < top || y > bottom)) return false;
    const std::wstring display = Text16ToWideString(input->GetDisplayText());
    // The drawn editor has 12dp of inner padding. Treat clicks in that padding as the nearest edge instead of falling through to a bogus
    // right-edge caret, and keep drag selection clamped in the same way.
    if (x < left - 12.0f * dp) { outIndex = 0; return true; }
    if (x > right + 12.0f * dp) { outIndex = display.size(); return true; }
    const float availableW = std::max(1.0f, right - left);
    size_t visualCaret = input->IsComposing() ? input->GetDisplayCaret() : input->GetCaret();
    visualCaret = SafeWideBoundary(display, visualCaret);
    float scrollX = 0.0f;
    const float caretWidth = renderer.MeasureString(display.substr(0, visualCaret), font).width;
    if (caretWidth > availableW) scrollX = caretWidth - availableW;
    const float target = std::clamp(x - left + scrollX, 0.0f, std::max(0.0f, renderer.MeasureString(display, font).width));
    size_t best = 0;
    float bestDistance = std::numeric_limits<float>::max();
    for (size_t index = 0; index <= display.size();) {
        const size_t boundary = SafeWideBoundary(display, index);
        const float width = renderer.MeasureString(display.substr(0, boundary), font).width;
        const float distance = std::abs(width - target);
        if (distance < bestDistance) { bestDistance = distance; best = boundary; }
        if (boundary >= display.size()) break;
        index = boundary + (IsHighUtf16Unit(display[boundary]) && boundary + 1 < display.size() && IsLowUtf16Unit(display[boundary + 1]) ? 2 : 1);
    }
    outIndex = best;
    return true;
}


void App::SetTextInputCaretFromMouse(float x, float y, bool extendSelection) {
    TextInputModel* input = ActiveTextInputModel();
    if (!input) return;
    std::size_t index = 0;
    if (!HitTestTextInput(x, y, index)) {
        // HitTestTextInput includes a vertical check; dragging may move above
        // or below the field, so repeat the horizontal calculation by clamping to the nearest edge.
        const std::wstring display = Text16ToWideString(input->GetDisplayText());
        index = x <= 0.0f ? 0 : display.size();
    }
    if (textInputDragging) input->SetSelection(textInputDragAnchor, index);
    else if (extendSelection) input->SetCaretFromMouseIndex(index, true);
    else input->SetCaretFromMouseIndex(index, false);
    MarkTextInputEdit(false);
}


bool App::HandleModalKeyDown(WPARAM wParam) {
    if (activeModal == ModalType::None) return false;
    const bool isInputModal = activeModal == ModalType::NewPlaylist || activeModal == ModalType::NewSmartPlaylist ||
        activeModal == ModalType::EditSmartPlaylist || activeModal == ModalType::SeekStepInput ||
        activeModal == ModalType::FrameSeekInput || activeModal == ModalType::CustomSettingInput ||
        activeModal == ModalType::BookmarkInput || activeModal == ModalType::EditMetadataOverrides ||
        activeModal == ModalType::SaveSubtitleProfile || activeModal == ModalType::RenameSubtitleProfile ||
        activeModal == ModalType::JumpToTime || activeModal == ModalType::CustomAspectInput || activeModal == ModalType::CustomRotationInput;
    if (modalFocusOwner != activeModal) { modalFocusOwner = activeModal; modalKeyboardFocus = 0; }
    const int count = isInputModal ? 3 : 2;
    if (wParam == VK_TAB || wParam == VK_LEFT || wParam == VK_RIGHT) {
        const bool reverse = wParam == VK_LEFT || (wParam == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        modalKeyboardFocus = (modalKeyboardFocus + (reverse ? count - 1 : 1)) % count;
        if (isInputModal && modalKeyboardFocus == 0) FocusTextInput(TextInputField::Modal);
        else BlurTextInput();
        MarkAccessibilityDirty();
        InvalidateRect(hWnd, nullptr, FALSE);
        return true;
    }
    const auto cancel = [this]() { DismissActiveModal(false); };
    if (wParam == VK_ESCAPE) { cancel(); return true; }
    if (wParam == VK_RETURN || wParam == VK_SPACE) {
        if (modalKeyboardFocus == count - 1) cancel();
        else DismissActiveModal(true);
        return true;
    }
    return true;
}


bool App::HandleGlobalKeyDown(WPARAM wParam, LPARAM lParam) {
    if (activeTextInputField != TextInputField::None && HandleTextInputKeyDown(wParam, lParam)) { return true; }
    if (activeTextInputField == TextInputField::PlaybackSpeed && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) return true;
    // The diagnostics log viewer closes on Escape before any other chord runs.
    if (showLogViewer && playbackSession.IsOpen() && wParam == VK_ESCAPE) { ToggleLogViewer(); return true; }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_SHIFT) & 0x8000) != 0 && (wParam == 'O' || wParam == 'o')) {
        OpenFolderDialog();
        return true;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (wParam == 'O' || wParam == 'o')) { OpenFileDialog(); return true; }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (wParam == 'Z' || wParam == 'z')) {
        if (!UndoLastLibraryAction()) ShowOSD(IconType::Refresh, L"Nothing to undo");
        return true;
    }
    if (globalSearchModel.IsOpen()) {
        if (wParam == VK_ESCAPE) { CloseGlobalSearch(); return true; }
        if (wParam == VK_DOWN) { globalSearchModel.MoveSelectionDown(); InvalidateRect(hWnd, NULL, FALSE); return true; }
        if (wParam == VK_UP) { globalSearchModel.MoveSelectionUp(); InvalidateRect(hWnd, NULL, FALSE); return true; }
        if (wParam == VK_PRIOR) { globalSearchModel.MoveSelectionPageUp(5); InvalidateRect(hWnd, NULL, FALSE); return true; }
        if (wParam == VK_NEXT) { globalSearchModel.MoveSelectionPageDown(5); InvalidateRect(hWnd, NULL, FALSE); return true; }
        if (wParam == VK_RETURN) {
            auto action = globalSearchModel.ActivateSelection(library);
            ExecuteGlobalSearchAction(action);
            return true;
        }
        return true;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (wParam == 'F' || wParam == 'f')) {
        if (currentTab == TabView::Media) { FocusMediaSearch(); return true; } else if (currentTab == TabView::Playlists) {
            FocusTextInput(TextInputField::PlaylistSearch);
            playlistSearchInput.SelectAll();
            InvalidateRect(hWnd, NULL, FALSE);
            return true;
        }
        ToggleGlobalSearch();
        return true;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (wParam == 'L' || wParam == 'l')) { ShowOpenNetworkStreamModal(); return true; }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_SHIFT) & 0x8000) != 0 && (wParam == 'I' || wParam == 'i')) {
        ImportStandardPlaylist();
        return true;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_SHIFT) & 0x8000) != 0 && (wParam == 'E' || wParam == 'e')) {
        ExportActivePlaylist();
        return true;
    }
    if (activeModal == ModalType::None && playbackSession.IsOpen() && wParam == 'A' &&
        (GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
        if (!(lParam & (1LL << 30))) CycleABRepeat();
        if (isContextMenuOpen) OpenPlayerAdvancedContextMenu(contextMenuOrigin.x, contextMenuOrigin.y);
        return true;
    }
    if (activeModal != ModalType::None) return HandleModalKeyDown(wParam);
    if (importJobActive) {
        if (wParam == VK_ESCAPE) {
            CancelImportJob();
            ShowOSD(IconType::VolumeMuted, L"Import canceled");
            InvalidateRect(hWnd, NULL, FALSE);
        }
        // Keep navigation and destructive shortcuts from touching the library while the worker's result is waiting to be merged.
        return true;
    }
    if (!isContextMenuOpen && (GetKeyState(VK_SHIFT) & 0x8000) != 0 && wParam == VK_OEM_2) { OpenShortcutOverlay(); return true; }
    if (isContextMenuOpen) {
        if (wParam == VK_ESCAPE) {
            CloseContextMenu();
            InvalidateRect(hWnd, NULL, FALSE);
            if (playbackSession.IsOpen()) Render();
            return true;
        }
        if (wParam == VK_UP) {
            for (int step = 0; step < (int)contextMenuItems.size(); ++step) {
                if (hoveredContextMenuItem <= 0) hoveredContextMenuItem = (int)contextMenuItems.size() - 1;
                else hoveredContextMenuItem--;
                if (!contextMenuItems[hoveredContextMenuItem].isSeparator && !contextMenuItems[hoveredContextMenuItem].isDisabled) break;
            }
            InvalidateRect(hWnd, NULL, FALSE);
            if (playbackSession.IsOpen()) Render();
            return true;
        }
        if (wParam == VK_DOWN) {
            for (int step = 0; step < (int)contextMenuItems.size(); ++step) {
                if (hoveredContextMenuItem >= (int)contextMenuItems.size() - 1) hoveredContextMenuItem = 0;
                else hoveredContextMenuItem++;
                if (!contextMenuItems[hoveredContextMenuItem].isSeparator && !contextMenuItems[hoveredContextMenuItem].isDisabled) break;
            }
            InvalidateRect(hWnd, NULL, FALSE);
            if (playbackSession.IsOpen()) Render();
            return true;
        }
        if (wParam == VK_RETURN) {
            if (hoveredContextMenuItem >= 0 && (size_t)hoveredContextMenuItem < contextMenuItems.size() &&
                !contextMenuItems[hoveredContextMenuItem].isDisabled && !contextMenuItems[hoveredContextMenuItem].isSeparator) {
                auto cb = contextMenuItems[hoveredContextMenuItem].onClick;
                bool keepOpen = contextMenuItems[hoveredContextMenuItem].keepOpen;
                if (!keepOpen) CloseContextMenu();
                if (cb) cb();
                InvalidateRect(hWnd, NULL, FALSE);
                if (playbackSession.IsOpen()) Render();
                return true;
            }
        }
    }
    if (!rebindingActionId.empty()) {
        if (wParam == VK_ESCAPE) { CancelShortcutCapture(); return true; }
        if (wParam == VK_SHIFT || wParam == VK_CONTROL || wParam == VK_MENU) {
            std::wstring chord = GetCurrentCaptureChordText();
            if (chord != lastAnnouncedModifierChord && chord != L"[ Press Key... ]") {
                lastAnnouncedModifierChord = chord;
                AnnounceStatus(chord, false);
                MarkAccessibilityDirty();
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return true;
        }
        // Mouse pseudo-keys complete the capture through the same shared path
        // (see App::HandleShortcutCaptureMouse routing in the mouse handlers).
        CompleteShortcutCapture((UINT)wParam,
            (GetKeyState(VK_CONTROL) & 0x8000) != 0, (GetKeyState(VK_SHIFT) & 0x8000) != 0, (GetKeyState(VK_MENU) & 0x8000) != 0);
        return true;
    }
    if (playbackSession.IsOpen()) {
        // Keyboard activity is also player activity: wake the controls before
        // dispatching a shortcut, including when the settings flyout is open.
        controlsIdleTimer = 0.0f;
        controlsOpacity = 1.0f;
        if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
        if (wParam == VK_SPACE && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000)) {
            commandDispatcher.Execute(AppCommand::TogglePlay);
            return true;
        }
        if (isSettingsFlyoutOpen) {
            const int optionCount = GetFlyoutOptionCount();
            if (wParam == VK_ESCAPE) { CloseSettingsFlyout(); Render(); return true; }
            if (wParam == VK_TAB) {
                isKeyboardNavActive = true;
                if (optionCount > 0) {
                    const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    if (hoveredFlyoutOption < 0) {
                        hoveredFlyoutOption = reverse ? optionCount - 1 : 0;
                    } else { hoveredFlyoutOption = (hoveredFlyoutOption + (reverse ? -1 : 1) + optionCount) % optionCount; }
                    MarkAccessibilityDirty();
                    RefreshAccessibilityTree(true);
                    const LONG flyoutId = 9100 + hoveredFlyoutOption + static_cast<LONG>(currentFlyoutMenu.size() * 100);
                    accessibilityTree.SetFocusedNode(flyoutId);
                    Render();
                }
                return true;
            }
            if (wParam == VK_LEFT && currentFlyoutMenu != "main") {
                currentFlyoutMenu = "main";
                hoveredFlyoutOption = -1;
                Render();
                return true;
            }
            if (wParam == VK_UP || wParam == VK_DOWN) {
                if (optionCount <= 0) return true;
                const int direction = wParam == VK_DOWN ? 1 : -1;
                if (currentFlyoutMenu == "queue") {
                    const size_t queueSize = playbackSession.GetQueueSize();
                    const size_t maxFirst = queueSize > 7 ? queueSize - 7 : 0;
                    if (hoveredFlyoutOption < 0) {
                        hoveredFlyoutOption = direction > 0 ? 0 : optionCount - 1;
                    } else if (direction > 0 && hoveredFlyoutOption == optionCount - 1 && queueFlyoutFirstVisible < maxFirst) {
                        ++queueFlyoutFirstVisible;
                    } else if (direction < 0 && hoveredFlyoutOption == 0 && queueFlyoutFirstVisible > 0) {
                        --queueFlyoutFirstVisible;
                    } else { hoveredFlyoutOption = std::clamp(hoveredFlyoutOption + direction, 0, optionCount - 1); }
                } else {
                    if (hoveredFlyoutOption < 0) hoveredFlyoutOption = direction > 0 ? 0 : optionCount - 1;
                    else hoveredFlyoutOption = std::clamp(hoveredFlyoutOption + direction, 0, optionCount - 1);
                }
                Render();
                return true;
            }
            if ((wParam == VK_LEFT || wParam == VK_RIGHT) && optionCount > 0 && hoveredFlyoutOption >= 0) {
                // Horizontal navigation is useful for the speed pills and
                // delay buttons; vertical menus stay on their current row.
                if (currentFlyoutMenu == "speed") {
                    int row = hoveredFlyoutOption / 3, col = hoveredFlyoutOption % 3;
                    col = std::clamp(col + (wParam == VK_RIGHT ? 1 : -1), 0, 2);
                    hoveredFlyoutOption = row * 3 + col;
                    Render();
                    return true;
                }
                if (currentFlyoutMenu == "audiodelay" || currentFlyoutMenu == "subdelay") {
                    hoveredFlyoutOption = std::clamp(hoveredFlyoutOption + (wParam == VK_RIGHT ? 1 : -1), 0, 2);
                    Render();
                    return true;
                }
            }
            if (wParam == VK_RETURN && hoveredFlyoutOption == PlaybackFlyoutLayout::Back) {
                currentFlyoutMenu = "main";
                hoveredFlyoutOption = PlaybackFlyoutLayout::None;
                Render();
                return true;
            }
            if (wParam == VK_RETURN && hoveredFlyoutOption >= 0 && hoveredFlyoutOption < optionCount) {
                const float dp = std::max(0.5f, overlayRenderer.GetDpiScale());
                const auto bounds = D2D1::RectF(0, 0, 280.0f * dp, GetFlyoutHeight());
                const auto row = PlaybackFlyoutLayout::Option(currentFlyoutMenu, hoveredFlyoutOption, bounds, dp);
                HandleFlyoutClick((row.left + row.right) * 0.5f, (row.top + row.bottom) * 0.5f);
                Render();
                return true;
            }
        }
        bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool isAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        RECT playerRc{};
        GetClientRect(hOverlayWnd ? hOverlayWnd : hWnd, &playerRc);
        const float playerW = static_cast<float>(playerRc.right - playerRc.left);
        const float playerH = static_cast<float>(playerRc.bottom - playerRc.top);
        const float playerDp = std::max(0.5f, overlayRenderer.GetDpiScale());
        const bool queuePlayback = playbackSession.IsQueuePlayback();
        const auto playerLayout = Presentation::Player(playerW, playerH, playerDp, queuePlayback);
        std::vector<LONG> playerFocusOrder;
        if (playbackSession.IsPiP()) {
            playerFocusOrder = { 9020 };
            if (playbackSession.IsQueuePlayback() && (playbackSession.GetQueueIndex() > 0 || settings.loop))
                playerFocusOrder.push_back(9023);
            if (playbackSession.IsQueuePlayback() && playbackSession.GetQueueSize() > 1) { playerFocusOrder.push_back(9024); }
            playerFocusOrder.push_back(9025);
            playerFocusOrder.push_back(9026);
            playerFocusOrder.push_back(9021);
        } else {
            playerFocusOrder.push_back(9001); // Back
            if (queuePlayback && playbackSession.GetQueueSize() > 0) {
                if (playbackSession.GetQueueIndex() > 0 || settings.loop) {
                    playerFocusOrder.push_back(9010); // Previous
                }
            }
            playerFocusOrder.push_back(9002); // Play/Pause
            playerFocusOrder.push_back(9011); // Next
            playerFocusOrder.push_back(9003); // Timeline
            playerFocusOrder.push_back(9004); // Mute
            if (playerLayout.showVolume) playerFocusOrder.push_back(9012);
            if (playerLayout.showSubtitles) playerFocusOrder.push_back(9005);
            playerFocusOrder.push_back(9006); // Settings
            if (playerLayout.showPip) playerFocusOrder.push_back(9007);
            playerFocusOrder.push_back(9008); // Fullscreen
        }
        const auto focusPlayerControl = [this](LONG id) {
            playerKeyboardFocusId = id;
            isKeyboardNavActive = true;
            hoveredPlayerButtonId = 0;
            isBackHovered = false;
            isVolumeHovered = false;
            isTimelineHovered = false;
            hoveredFlyoutOption = -1;
            MarkAccessibilityDirty();
            RefreshAccessibilityTree(true);
            accessibilityTree.SetFocusedNode(id);
            Render();
        };
        if (wParam == VK_TAB && !isCtrl && !isAlt && !playerFocusOrder.empty()) {
            auto it = std::find(playerFocusOrder.begin(), playerFocusOrder.end(), playerKeyboardFocusId);
            size_t nextIndex = 0;
            if (it == playerFocusOrder.end()) { nextIndex = isShift ? playerFocusOrder.size() - 1 : 0; } else {
                const size_t currentIndex = static_cast<size_t>(std::distance(playerFocusOrder.begin(), it));
                nextIndex = isShift ? (currentIndex + playerFocusOrder.size() - 1) % playerFocusOrder.size()
                    : (currentIndex + 1) % playerFocusOrder.size();
            }
            focusPlayerControl(playerFocusOrder[nextIndex]);
            return true;
        }
        if (playbackSession.IsPiP() && isPiPVolumeSliderOpen && !isCtrl && !isAlt) {
            if (wParam == VK_ESCAPE || wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN) {
                isPiPVolumeSliderOpen = false;
                AnimateVisual(pipVolumeAnim, 0.0f, MotionTokens::kToggle, MotionTokens::Spring());
                Render();
                return true;
            }
        }
        if (playbackSession.IsPiP() && !isCtrl && !isAlt && (wParam == VK_RETURN || wParam == VK_SPACE)) {
            if (!isKeyboardNavActive || playerKeyboardFocusId == 0 || playerKeyboardFocusId == 9020) {
                commandDispatcher.Execute(AppCommand::TogglePlay);
                return true;
            }
        }
        const bool focusedControlVisible = std::find(playerFocusOrder.begin(), playerFocusOrder.end(), playerKeyboardFocusId) != playerFocusOrder.end();
        if (focusedControlVisible && !isCtrl && !isAlt && (wParam == VK_LEFT || wParam == VK_RIGHT)) {
            if (playerKeyboardFocusId == 9003 && player.GetDuration() > 0.0 && std::isfinite(player.GetDuration())) {
                commandDispatcher.Execute(wParam == VK_RIGHT ? AppCommand::SeekForward : AppCommand::SeekBackward);
                focusPlayerControl(9003);
                return true;
            }
            if (playerKeyboardFocusId == 9012) {
                commandDispatcher.Execute(wParam == VK_RIGHT ? AppCommand::VolumeUp : AppCommand::VolumeDown);
                focusPlayerControl(9012);
                return true;
            }
        }
        if (focusedControlVisible && !isCtrl && !isAlt && (wParam == VK_RETURN || wParam == VK_SPACE)) {
            const LONG id = playerKeyboardFocusId;
            switch (id) {
            case 9001: commandDispatcher.Execute(AppCommand::ClosePlayer); return true;
            case 9002: commandDispatcher.Execute(AppCommand::TogglePlay); break;
            case 9011: commandDispatcher.Execute(AppCommand::PlayNext); break;
            case 9004: commandDispatcher.Execute(AppCommand::ToggleMute); break;
            case 9005: commandDispatcher.Execute(AppCommand::ToggleSubtitles); break;
            case 9006:
                PushFocusSnapshot(TransientSurfaceType::SettingsFlyout);
                isSettingsFlyoutOpen = true;
                currentFlyoutMenu = "main";
                hoveredFlyoutOption = 0;
                flyoutClosing = false;
                flyoutAnim.SnapTo(0.0f);
                AnimateVisual(flyoutAnim, 1.0f, MotionTokens::kToggle, MotionTokens::Spring());
                MarkAccessibilityDirty();
                RefreshAccessibilityTree(true);
                accessibilityTree.SetFocusedNode(9100 + static_cast<LONG>(currentFlyoutMenu.size() * 100));
                Render();
                return true;
            case 9007: commandDispatcher.Execute(AppCommand::TogglePiP); break;
            case 9008: commandDispatcher.Execute(AppCommand::ToggleFullscreen); break;
            case 9010: commandDispatcher.Execute(AppCommand::PlayPrevious); break;
            case 9020: commandDispatcher.Execute(AppCommand::TogglePlay); break;
            case 9023: commandDispatcher.Execute(AppCommand::PlayPrevious); break;
            case 9024: commandDispatcher.Execute(AppCommand::PlayNext); break;
            case 9025:
                isPiPVolumeSliderOpen = !isPiPVolumeSliderOpen;
                if (isPiPVolumeSliderOpen) {
                    pipVolumeAnim.SnapTo(0.0f);
                    AnimateVisual(pipVolumeAnim, 1.0f, MotionTokens::kToggle, MotionTokens::Spring());
                } else {
                    AnimateVisual(pipVolumeAnim, 0.0f, MotionTokens::kToggle, MotionTokens::Spring());
                }
                Render();
                return true;
            case 9026:
                playbackSession.TogglePiPPinned(hWnd);
                SyncOverlayPosition();
                MarkAccessibilityDirty();
                break;
            case 9021: commandDispatcher.Execute(AppCommand::TogglePiP); break;
            case 9022: SendMessage(hWnd, WM_CLOSE, 0, 0); return true;
            case 9003:
            case 9012: return true;
            default: break;
            }
            if (playbackSession.IsOpen()) focusPlayerControl(id);
            return true;
        }
        AppCommand cmd = commandDispatcher.TranslateKeyChord((UINT)wParam, isCtrl, isShift, isAlt, playbackSession.IsOpen(), settings);
        if (cmd != AppCommand::None) { commandDispatcher.Execute(cmd); return true; }
        if (wParam == VK_ESCAPE) {
            if (isSettingsFlyoutOpen) { CloseSettingsFlyout(); Render(); return true; }
            if (isContextMenuOpen) { CloseContextMenu(); Render(); return true; }
            if (playbackSession.IsFullscreen()) { commandDispatcher.Execute(AppCommand::ToggleFullscreen); return true; }
            commandDispatcher.Execute(AppCommand::ClosePlayer);
            return true;
        }
        return false;
    } else {
        bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool isAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        // Activate keyboard navigation mode and clear mouse hover states
        isKeyboardNavActive = true;
        hoveredMediaIndex = -1;
        hoveredPlaylistRailIndex = -1;
        hoveredPlaylistDetailItemIndex = -1;
        hoveredHistoryIndex = -1;
        isPlaylistResumeHovered = false;
        isNavOpenHovered = false;
        isNavSettingsHovered = false;
        hoveredTab = (TabView)-1;
        // Active Modal intercept
        if (activeModal != ModalType::None) return HandleModalKeyDown(wParam);
        // Global Dismiss / Escape
        if (wParam == VK_ESCAPE) {
            if (isContextMenuOpen) { CloseContextMenu(); InvalidateRect(hWnd, NULL, FALSE); return true; }
            if (activeFocusPane == FocusPane::PlaylistDetail) {
                activeFocusPane = FocusPane::PlaylistRail;
                int curPlIdx = 0;
                for (size_t i = 0; i < library.playlists.size(); ++i) {
                    if (library.playlists[i].id == activePlaylistId) { curPlIdx = (int)i; break; }
                }
                keyboardFocusedIndex = curPlIdx;
                selectedPlaylistIndices = { (size_t)curPlIdx };
                AutoScrollToFocusedItem();
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
            if (activeFocusPane == FocusPane::SettingsContent) {
                activeFocusPane = FocusPane::SettingsCategoryList;
                keyboardFocusedIndex = (int)currentSettingsCat;
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
            if (activeFocusPane != FocusPane::TopNavBar) {
                activeFocusPane = FocusPane::TopNavBar;
                if (currentTab == TabView::Media) topNavFocusedIndex = 0;
                else if (currentTab == TabView::Playlists) topNavFocusedIndex = 1;
                else if (currentTab == TabView::History) topNavFocusedIndex = 2;
                else if (currentTab == TabView::Settings) topNavFocusedIndex = 3;
                ClearSelectionInCurrentPane();
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
            ClearSelectionInCurrentPane();
            return true;
        }
        // CTRL + A (Select All)
        if (isCtrl && (wParam == 'A' || wParam == 'a')) { SelectAllInCurrentPane(); return true; }
        // Delete (Batch remove selected)
        if (wParam == VK_DELETE) { DeleteSelectedInCurrentPane(); return true; }
        // F2 (Rename playlist inline)
        if (wParam == VK_F2) {
            if (currentTab == TabView::Playlists) {
                std::string targetId = activePlaylistId;
                if (activeFocusPane == FocusPane::PlaylistRail && keyboardFocusedIndex >= 0 && keyboardFocusedIndex < (int)library.playlists.size()) {
                    targetId = library.playlists[keyboardFocusedIndex].id;
                } else if (targetId.empty() && !library.playlists.empty()) { targetId = library.playlists[0].id; }
                if (!targetId.empty()) { StartInlinePlaylistRename(targetId); return true; }
            }
        }
        // Tab Switching via Ctrl+Left / Ctrl+Right
        if (isCtrl && (wParam == VK_LEFT || wParam == VK_RIGHT)) {
            if (wParam == VK_LEFT) {
                if (currentTab == TabView::Settings) { currentTab = TabView::History; topNavFocusedIndex = 2; }
                else if (currentTab == TabView::History) { currentTab = TabView::Playlists; topNavFocusedIndex = 1; }
                else if (currentTab == TabView::Playlists) { currentTab = TabView::Media; topNavFocusedIndex = 0; }
            } else {
                if (currentTab == TabView::Media) { currentTab = TabView::Playlists; topNavFocusedIndex = 1; }
                else if (currentTab == TabView::Playlists) { currentTab = TabView::History; topNavFocusedIndex = 2; }
                else if (currentTab == TabView::History) { currentTab = TabView::Settings; topNavFocusedIndex = 3; }
            }
            activeFocusPane = FocusPane::TopNavBar;
            InvalidateRect(hWnd, NULL, FALSE);
            return true;
        }
        // ZONE A: Top Navigation Bar Focus
        if (activeFocusPane == FocusPane::TopNavBar) {
            if (wParam == VK_TAB && currentTab == TabView::History && !isCtrl && !isAlt) {
                activeFocusPane = FocusPane::HistoryList;
                isKeyboardNavActive = true;
                LONG targetAccessibilityId = 4081;
                if (!library.history.empty()) {
                    int targetIndex = keyboardFocusedIndex;
                    if (targetIndex < 0 || targetIndex >= static_cast<int>(library.history.size())) {
                        targetIndex = 0;
                        if (!selectedHistoryIndices.empty() && *selectedHistoryIndices.begin() < library.history.size())
                            targetIndex = static_cast<int>(*selectedHistoryIndices.begin());
                    }
                    keyboardFocusedIndex = targetIndex;
                    selectedHistoryIndices = { static_cast<size_t>(targetIndex) };
                    targetAccessibilityId = AccessibilityStableId(4000, library.history[targetIndex].filePath);
                    AutoScrollToFocusedItem();
                } else { keyboardFocusedIndex = 0; selectedHistoryIndices.clear(); }
                MarkAccessibilityDirty();
                RefreshAccessibilityTree(true);
                accessibilityTree.SetFocusedNode(targetAccessibilityId);
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
            if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                int dir = (wParam == VK_RIGHT) ? 1 : -1;
                topNavFocusedIndex = (topNavFocusedIndex + dir + 5) % 5;
                if (topNavFocusedIndex == 0) currentTab = TabView::Media;
                else if (topNavFocusedIndex == 1) currentTab = TabView::Playlists;
                else if (topNavFocusedIndex == 2) currentTab = TabView::History;
                else if (topNavFocusedIndex == 3) currentTab = TabView::Settings;
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
            if (wParam == VK_DOWN) {
                if (currentTab == TabView::Settings) {
                    activeFocusPane = FocusPane::SettingsCategoryList;
                    keyboardFocusedIndex = (int)currentSettingsCat;
                } else if (currentTab == TabView::Playlists) {
                    activeFocusPane = FocusPane::PlaylistRail;
                    keyboardFocusedIndex = 0;
                    selectedPlaylistIndices = { 0 };
                    if (!library.playlists.empty()) activePlaylistId = library.playlists[0].id;
                    AutoScrollToFocusedItem();
                } else if (currentTab == TabView::Media) {
                    activeFocusPane = FocusPane::MediaList;
                    keyboardFocusedIndex = 0;
                    selectedMediaIndices = { 0 };
                    AutoScrollToFocusedItem();
                } else if (currentTab == TabView::History) {
                    activeFocusPane = FocusPane::HistoryList;
                    keyboardFocusedIndex = 0;
                    selectedHistoryIndices = { 0 };
                    AutoScrollToFocusedItem();
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
            if (wParam == VK_RETURN || wParam == VK_SPACE) {
                if (topNavFocusedIndex == 4) { OpenFileDialog(); } else {
                    if (topNavFocusedIndex == 0) {
                        currentTab = TabView::Media;
                        activeFocusPane = FocusPane::MediaList;
                        keyboardFocusedIndex = 0;
                        selectedMediaIndices = { 0 };
                    } else if (topNavFocusedIndex == 1) {
                        currentTab = TabView::Playlists;
                        activeFocusPane = FocusPane::PlaylistRail;
                        keyboardFocusedIndex = 0;
                        selectedPlaylistIndices = { 0 };
                        if (!library.playlists.empty()) activePlaylistId = library.playlists[0].id;
                    } else if (topNavFocusedIndex == 2) {
                        currentTab = TabView::History;
                        activeFocusPane = FocusPane::HistoryList;
                        keyboardFocusedIndex = 0;
                        selectedHistoryIndices = { 0 };
                    } else if (topNavFocusedIndex == 3) {
                        currentTab = TabView::Settings;
                        activeFocusPane = FocusPane::SettingsCategoryList;
                        keyboardFocusedIndex = (int)currentSettingsCat;
                    }
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
            return false;
        }
        // ZONE B: Settings Navigation & Category List
        if (currentTab == TabView::Settings) {
            SettingsCategory cats[] = {SettingsCategory::Experience, SettingsCategory::Appearance, SettingsCategory::Playback,
                                       SettingsCategory::Subtitles, SettingsCategory::Shortcuts, SettingsCategory::System};
            int curCatIdx = (int)currentSettingsCat;
            if (activeFocusPane == FocusPane::SettingsCategoryList) {
                if (wParam == VK_UP) {
                    if (curCatIdx <= 0) {
                        activeFocusPane = FocusPane::TopNavBar;
                        topNavFocusedIndex = 3;
                    } else { currentSettingsCat = cats[curCatIdx - 1]; keyboardFocusedIndex = curCatIdx - 1; settingsScrollY = 0.0f; hoveredSettingsRow = -1; }
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                if (wParam == VK_DOWN) {
                    if (curCatIdx >= 5) {
                        activeFocusPane = FocusPane::SettingsContent;
                        keyboardFocusedIndex = 0;
                        keyboardFocusedSubIndex = 0;
                    } else { currentSettingsCat = cats[curCatIdx + 1]; keyboardFocusedIndex = curCatIdx + 1; settingsScrollY = 0.0f; hoveredSettingsRow = -1; }
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                if (wParam == VK_RIGHT || wParam == VK_RETURN || wParam == VK_SPACE || wParam == VK_TAB) {
                    activeFocusPane = FocusPane::SettingsContent;
                    keyboardFocusedIndex = 0;
                    keyboardFocusedSubIndex = 0;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                return false;
            }
            if (activeFocusPane == FocusPane::SettingsContent) {
                const auto& allThemes = settings.GetAllThemes();
                auto scList = settings.GetShortcutActionListRef();
                size_t midSc = (scList.size() + 1) / 2;
                int maxRows = 1;
                const bool simpleSettings = FeatureGate::ModeFromString(settings.experienceMode) == ExperienceMode::Simple;
                if (currentSettingsCat == SettingsCategory::Appearance) maxRows = 2;
                else if (currentSettingsCat == SettingsCategory::Playback) maxRows = SettingsLayout::PlaybackRowCount(simpleSettings);
                else if (currentSettingsCat == SettingsCategory::Subtitles) maxRows = 6;
                else if (currentSettingsCat == SettingsCategory::Experience) maxRows = static_cast<int>(FeatureGate::Catalog().size()) + 1;
                else if (currentSettingsCat == SettingsCategory::Shortcuts) maxRows = (int)midSc + 1;
                else if (currentSettingsCat == SettingsCategory::System) maxRows = SettingsLayout::SystemRowCount(simpleSettings);
                auto getMaxSubIndex = [&](int row) -> int {
                    if (currentSettingsCat == SettingsCategory::Appearance) {
                        auto languages = settings.GetAvailableLanguages();
                        if (row == 0) return std::max(0, (int)allThemes.size() - 1);
                        if (row == 1) return std::max(0, (int)languages.size() - 1);
                    } else if (currentSettingsCat == SettingsCategory::Playback) {
                        if (row >= 0 && row <= 3) return 0;
                        if (row == 4) return 1; // Seek Dropdown (0) and Custom (1)
                        if (row == 5) return 1; // Frame Seek Dropdown (0) and Custom (1)
                        if (row == 6) return 5; // Speed
                        if (row == 7) return 0; // Custom MPV Config Toggle
                        if (row == 8) return 2; // Edit (0), Reset (1), Revert (2)
                        if (row >= 9 && row <= 11) return 0; // Global Hotkeys / Tray Icon / Minimize to Tray toggles
                        if (row == 12) return 1; // Network Cache Forward Dropdown (0) and Custom (1)
                        if (row == 13) return 1; // Network Cache Back Dropdown (0) and Custom (1)
                    } else if (currentSettingsCat == SettingsCategory::Experience) {
                        if (row == 0) return 2; // Mode tiles: Simple, Advanced, Custom
                        return 0;
                    } else if (currentSettingsCat == SettingsCategory::Subtitles) {
                        if (row == 0) return 6; // Audio: 6 languages + Default
                        if (row == 1) return 6; // Subtitles: 6 languages + Off
                        if (row == 2) return 18; // Font Size slider (14px..32px)
                        if (row == 3) return 5; // Color
                        if (row == 4) return 3; // Bg
                        if (row == 5) return 2; // Position
                    } else if (currentSettingsCat == SettingsCategory::Shortcuts) {
                        if (row == 0) return 0; // Reset button
                        return 1; // 2 columns
                    } else if (currentSettingsCat == SettingsCategory::System) { return 0; }
                    return 0;
                };
                if (wParam == VK_ESCAPE) {
                    activeFocusPane = FocusPane::SettingsCategoryList;
                    keyboardFocusedIndex = (int)currentSettingsCat;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                if (wParam == VK_LEFT) {
                    if (keyboardFocusedSubIndex > 0) {
                        keyboardFocusedSubIndex--;
                    } else { activeFocusPane = FocusPane::SettingsCategoryList; keyboardFocusedIndex = (int)currentSettingsCat; }
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                if (wParam == VK_RIGHT) {
                    int maxSub = getMaxSubIndex(keyboardFocusedIndex);
                    if (keyboardFocusedSubIndex < maxSub) { keyboardFocusedSubIndex++; InvalidateRect(hWnd, NULL, FALSE); return true; }
                    return true;
                }
                if (wParam == VK_UP) {
                    if (keyboardFocusedIndex <= 0) {
                        activeFocusPane = FocusPane::SettingsCategoryList;
                        keyboardFocusedIndex = (int)currentSettingsCat;
                    } else {
                        keyboardFocusedIndex--;
                        int maxSub = getMaxSubIndex(keyboardFocusedIndex);
                        keyboardFocusedSubIndex = std::clamp(keyboardFocusedSubIndex, 0, maxSub);
                        AutoScrollToFocusedItem();
                    }
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                if (wParam == VK_DOWN) {
                    if (keyboardFocusedIndex < maxRows - 1) {
                        keyboardFocusedIndex++;
                        int maxSub = getMaxSubIndex(keyboardFocusedIndex);
                        keyboardFocusedSubIndex = std::clamp(keyboardFocusedSubIndex, 0, maxSub);
                        AutoScrollToFocusedItem();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return true;
                    }
                    return true;
                }
                if (wParam == VK_RETURN || wParam == VK_SPACE) {
                    int row = std::clamp(keyboardFocusedIndex, 0, maxRows - 1);
                    int sub = keyboardFocusedSubIndex;
                    if (currentSettingsCat == SettingsCategory::Appearance) {
                        if (row == 0 && sub < (int)allThemes.size()) {
                            settings.themePreset = allThemes[sub].id;
                            settings.UpdateThemeColors();
                            ApplyWindowTheme();
                            SaveSettings();
                        } else if (row == 1) {
                            auto languages = settings.GetAvailableLanguages();
                            if (sub >= 0 && sub < (int)languages.size()) {
                                settings.appLanguage = languages[sub].tag;
                                Localization::Instance().SetLocale(settings.appLanguage);
                                renderer.CreateFonts();
                                overlayRenderer.CreateFonts();
                                SaveSettings();
                            }
                        }
                    } else if (currentSettingsCat == SettingsCategory::Experience) {
                        if (row == 0 && sub >= 0 && sub <= 2) {
                            ApplyExperienceModeSelection(static_cast<ExperienceMode>(sub));
                        } else if (row >= 1) {
                            const auto& catalog = FeatureGate::Catalog();
                            const int fi = row - 1;
                            if (fi < static_cast<int>(catalog.size())) {
                                ToggleExperienceFeature(catalog[fi].key);
                            }
                        }
                    } else if (currentSettingsCat == SettingsCategory::Playback) {
                        if (row == 0) {
                            settings.autoplay = !settings.autoplay;
                            AnimateVisual(autoplayToggleAnim, settings.autoplay ? 1.0f : 0.0f, MotionTokens::kToggle, MotionTokens::Standard());
                        } else if (row == 1) {
                            settings.rememberPos = !settings.rememberPos;
                            AnimateVisual(rememberPosToggleAnim, settings.rememberPos ? 1.0f : 0.0f, MotionTokens::kToggle, MotionTokens::Standard());
                        } else if (row == 2) {
                            settings.hwAccel = !settings.hwAccel;
                            AnimateVisual(hwAccelToggleAnim, settings.hwAccel ? 1.0f : 0.0f, MotionTokens::kToggle, MotionTokens::Standard());
                        } else if (row == 3) {
                            settings.loop = !settings.loop;
                            AnimateVisual(loopToggleAnim, settings.loop ? 1.0f : 0.0f, MotionTokens::kToggle, MotionTokens::Standard());
                        } else if (row == 4) {
                            if (sub == 0) {
                                double steps[] = { 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0 };
                                int curIdx = -1;
                                for (int k = 0; k < 7; ++k) {
                                    if (std::abs(settings.arrowSeekStepSec - steps[k]) < 0.05) { curIdx = k; break; }
                                }
                                int nextIdx = (curIdx + 1) % 7;
                                settings.arrowSeekStepSec = steps[nextIdx];
                            } else { ShowCustomSeekStepModal(); return true; }
                        } else if (row == 5) {
                            if (sub == 0) {
                                int frameList[] = { 1, 2, 5, 10, 24, 30, 60 };
                                int curIdx = -1;
                                for (int k = 0; k < 7; ++k) {
                                    if (settings.ctrlSeekFrames == frameList[k]) { curIdx = k; break; }
                                }
                                int nextIdx = (curIdx + 1) % 7;
                                settings.ctrlSeekFrames = frameList[nextIdx];
                            } else { ShowCustomFrameSeekModal(); return true; }
                        } else if (row == 6) {
                            double speeds[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };
                            if (sub < 6) { settings.defaultSpeed = speeds[sub]; player.SetPlaybackRate(settings.defaultSpeed); }
                        } else if (row == 7) { ToggleCustomMpvConfig(); return true; } else if (row == 8) {
                            if (sub == 0) EditCustomMpvConfigFile();
                            else if (sub == 1) ResetCustomMpvConfigToDefault();
                            else if (sub == 2) RevertCustomMpvConfigToLastKnownGood();
                            return true;
                        } else if (row == 9) {
                            settings.globalHotkeysEnabled = !settings.globalHotkeysEnabled;
                            ApplyGlobalHotkeys();
                            ShowOSD(IconType::SettingsGear, settings.globalHotkeysEnabled
                                ? (L"Global Hotkeys: On (" + std::to_wstring(registeredGlobalHotkeys) + L" registered)")
                                : L"Global Hotkeys: Off");
                        } else if (row == 10) {
                            settings.trayIconEnabled = !settings.trayIconEnabled;
                            if (!settings.trayIconEnabled && isMinimizedToTray) { RestoreFromTray(); }
                            UpdateTrayIcon();
                            ShowOSD(IconType::SettingsGear, settings.trayIconEnabled ? L"Tray Icon: On" : L"Tray Icon: Off");
                        } else if (row == 11) {
                            settings.minimizeToTray = !settings.minimizeToTray;
                            ShowOSD(IconType::SettingsGear, settings.minimizeToTray ? L"Minimize to Tray: On" : L"Minimize to Tray: Off");
                        } else if (row == 12) {
                            if (sub == 0) {
                                int cacheList[] = { 8, 32, 64, 128, 256 };
                                int curIdx = -1;
                                for (int k = 0; k < 5; ++k) {
                                    if (settings.networkForwardCacheMB == cacheList[k]) { curIdx = k; break; }
                                }
                                int nextIdx = (curIdx + 1) % 5;
                                settings.networkForwardCacheMB = cacheList[nextIdx];
                            } else { ShowCustomNetworkForwardCacheModal(); return true; }
                        } else if (row == 13) {
                            if (sub == 0) {
                                int cacheList[] = { 0, 16, 64, 128 };
                                int curIdx = -1;
                                for (int k = 0; k < 4; ++k) {
                                    if (settings.networkBackCacheMB == cacheList[k]) { curIdx = k; break; }
                                }
                                int nextIdx = (curIdx + 1) % 4;
                                settings.networkBackCacheMB = cacheList[nextIdx];
                            } else { ShowCustomNetworkBackCacheModal(); return true; }
                        }
                        ApplyPlayerPreferences();
                        SaveSettings();
                    } else if (currentSettingsCat == SettingsCategory::Subtitles) {
                        const char* audioLangCodes[] = { "eng", "jpn", "fra", "deu", "spa", "ita", "auto" };
                        const char* subLangCodes[] = { "eng", "jpn", "fra", "deu", "spa", "ita", "off" };
                        if (row == 0 && sub >= 0 && sub < 7) {
                            settings.audioLang = audioLangCodes[sub];
                        } else if (row == 1 && sub >= 0 && sub < 7) {
                            if (sub == 6) {
                                settings.subtitlesEnabled = false;
                            } else { settings.subLang = subLangCodes[sub]; settings.subtitlesEnabled = true; }
                        } else if (row == 2) {
                            if (sub >= 0 && sub <= 18) settings.subFontSize = SubtitleSizeToString(14 + sub);
                        } else if (row == 3) {
                            const char* colHex[] = { "#cad3f5", "#eed49f", "#a6da95", "#91d7e3", "#f5a97f", "#ed8796" };
                            if (sub < 6) settings.subColorHex = colHex[sub];
                        } else if (row == 4) {
                            const char* bgKeys[] = { "vlc-outline", "translucent", "solid", "clean" };
                            if (sub < 4) settings.subBgStyle = bgKeys[sub];
                        } else if (row == 5) {
                            const char* posKeys[] = { "top", "elevated", "bottom" };
                            if (sub < 3) settings.subPosition = posKeys[sub];
                        }
                        ApplyPlayerPreferences();
                        SaveSettings();
                    } else if (currentSettingsCat == SettingsCategory::Shortcuts) {
                        if (row == 0) {
                            ShowConfirmationModal(L"Reset Shortcuts", L"Are you sure you want to reset all keyboard shortcuts to their default key assignments?", L"Reset", [this]() {
                                settings.ResetShortcuts();
                                SaveSettings();
                                ApplyGlobalHotkeys();
                            });
                            return true;
                        } else {
                            size_t idx = (sub == 0) ? (size_t)(row - 1) : (midSc + (size_t)(row - 1));
                            if (idx < scList.size()) { StartShortcutCapture(scList[idx].first, scList[idx].second, row, sub); }
                        }
                    } else if (currentSettingsCat == SettingsCategory::System) {
                        const int clampedRow = std::clamp(row, 0, SettingsLayout::kSystemRowCount - 1);
                        const int action = SettingsLayout::kSystemActions[clampedRow];
                        if (action == 0) {
                            std::wstring dir = GetDataDir();
                            ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
                        } else if (action == 2) {
                            CacheStats statsBefore = QueryCacheStats();
                            ClearDisposableCache();
                            renderer.ClearThumbnailCache();
                            CacheStats statsAfter = QueryCacheStats();
                            uint64_t freed = (statsBefore.totalBytes >= statsAfter.totalBytes) ? (statsBefore.totalBytes - statsAfter.totalBytes) : statsBefore.totalBytes;
                            wchar_t buf[128];
                            swprintf_s(buf, L"Disposable cache cleared (freed %.1f MB)", (double)freed / (1024.0 * 1024.0));
                            ShowOSD(IconType::Trash, buf);
                        } else if (action == 7) {
                            ShowConfirmationModal(L"Clear Watch History", L"Erase all watch history and playback progress? You can undo this afterward.", L"Clear History", [this]() {
                                ArmLibraryUndo(L"History clear");
                                library.ClearHistory();
                                MarkJumpListDirty();
                                SaveLibrary();
                                selectedHistoryIndices.clear();
                                isLastWatchedCleared = true;
                                InvalidateMediaLayoutCache();
                                InvalidateRect(hWnd, NULL, FALSE);
                            });
                            return true;
                        } else if (action == 9) {
                            settings.hoverPreviewEnabled = !settings.hoverPreviewEnabled;
                            if (!settings.hoverPreviewEnabled) previewPlayer.StopPreview();
                            SaveSettings();
                            ShowOSD(IconType::MediaFilm, settings.hoverPreviewEnabled ? L"Hover previews enabled" : L"Hover previews disabled");
                        } else if (action == 10) {
                            settings.reduceMotionEnabled = !settings.reduceMotionEnabled;
                            if (settings.reduceMotionEnabled) SnapVisualAnimations();
                            SaveSettings();
                            ShowOSD(IconType::SettingsGear, settings.reduceMotionEnabled ? L"Reduced motion enabled" : L"Following Windows motion preferences");
                        } else if (action == 11) { CheckForUpdates(true); } else if (action == 8) {
                            ShowConfirmationModal(L"Reset Settings to Defaults", L"Are you sure you want to restore factory default theme, shortcuts, and playback configurations?", L"Reset Defaults", [this]() {
                                settings.ResetToDefaults();
                                playlistGridView = settings.playlistViewMode == "grid";
                                libraryGroupMode = LibraryGroupMode::Flat;
                                renderer.UpdateFontFamily(settings.uiFontFamily);
                                overlayRenderer.UpdateFontFamily(settings.uiFontFamily);
                                ApplyWindowTheme();
                                ApplyPlayerPreferences();
                                // ResetToDefaults clears shortcuts and turns the
                                // system-integration toggles off: re-derive both.
                                ApplyGlobalHotkeys();
                                UpdateTrayIcon();
                            });
                            return true;
                        } else if (action == 13) {
                            settings.smartSeriesDetection = !settings.smartSeriesDetection;
                            if (libraryGroupMode != LibraryGroupMode::Flat)
                                SetLibraryGroupMode(settings.smartSeriesDetection ? LibraryGroupMode::Series : LibraryGroupMode::Folder);
                            SaveSettings();
                            ShowOSD(IconType::SettingsGear, settings.smartSeriesDetection ? L"Smart series detection enabled" : L"Grouping by folder name");
                        } else if (action == 14) {
                            settings.jumpListEnabled = !settings.jumpListEnabled;
                            SaveSettings();
                            ShowOSD(IconType::HistoryClock, settings.jumpListEnabled ? L"Jump list enabled" : L"Jump list disabled");
                            // On: rebuild now; off: publish an empty list to remove the category.
                            PumpJumpListUpdate(true);
                        } else if (action == 15) {
                            const FileAssociationResult registered = RegisterFileAssociations();
                            if (registered.registered == 0 && registered.failed > 0) {
                                ShowOSD(IconType::MediaFilm, L"File type registration failed");
                            } else {
                                wchar_t buf[160];
                                swprintf_s(buf, L"File types registered (%d extensions; use Open With to pick Aperture)", registered.registered);
                                ShowOSD(IconType::MediaFilm, buf);
                            }
                        } else if (action == 16) {
                            if (!GetFileAssociationState()) {
                                ShowOSD(IconType::MediaFilm, L"No file types registered");
                            } else {
                                const FileAssociationResult removed = UnregisterFileAssociations();
                                if (removed.registered == 0 && removed.failed > 0) {
                                    ShowOSD(IconType::MediaFilm, L"File type removal failed");
                                } else {
                                    ShowOSD(IconType::MediaFilm, L"File type registration removed");
                                }
                            }
                        }
                        SaveSettings();
                    }
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                return false;
            }
            return false;
        }
        // Playlists Navigation (Rail <-> Continue Watching Card <-> Episode Cards)
        if (currentTab == TabView::Playlists) {
            Playlist* pl = library.FindPlaylist(activePlaylistId);
            const auto visible = GetVisiblePlaylistIndices();
            size_t railCount = library.playlists.size();
            size_t epCount = visible.size();
            if (activeFocusPane == FocusPane::PlaylistRail) {
                int curPlIdx = 0;
                for (size_t i = 0; i < library.playlists.size(); ++i) {
                    if (library.playlists[i].id == activePlaylistId) { curPlIdx = (int)i; break; }
                }
                if (keyboardFocusedIndex < 0) keyboardFocusedIndex = curPlIdx;
                if (wParam == VK_RIGHT) {
                    activeFocusPane = FocusPane::PlaylistDetail;
                    keyboardFocusedIndex = -1; // Continue Watching card
                    selectedEpisodeIndices.clear();
                    AutoScrollToFocusedItem();
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                if (wParam == VK_DOWN) {
                    if (isCtrl) {
                        int nextIdx = std::min((int)railCount - 1, keyboardFocusedIndex + 5);
                        keyboardFocusedIndex = nextIdx;
                        selectedPlaylistIndices = { (size_t)nextIdx };
                        if (nextIdx < (int)railCount) activePlaylistId = library.playlists[nextIdx].id;
                        AutoScrollToFocusedItem();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return true;
                    }
                    if (keyboardFocusedIndex >= (int)railCount - 1) {
                        // At the last folder, pressing down takes it inside the folder
                        activeFocusPane = FocusPane::PlaylistDetail;
                        keyboardFocusedIndex = -1; // Continue Watching card
                        selectedEpisodeIndices.clear();
                        AutoScrollToFocusedItem();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return true;
                    }
                    int nextIdx = keyboardFocusedIndex + 1;
                    keyboardFocusedIndex = nextIdx;
                    selectedPlaylistIndices = { (size_t)nextIdx };
                    if (nextIdx < (int)railCount) activePlaylistId = library.playlists[nextIdx].id;
                    AutoScrollToFocusedItem();
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                if (wParam == VK_UP) {
                    if (isCtrl) {
                        int nextIdx = std::max(0, keyboardFocusedIndex - 5);
                        keyboardFocusedIndex = nextIdx;
                        selectedPlaylistIndices = { (size_t)nextIdx };
                        if (nextIdx < (int)railCount) activePlaylistId = library.playlists[nextIdx].id;
                        AutoScrollToFocusedItem();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return true;
                    }
                    if (keyboardFocusedIndex <= 0) {
                        activeFocusPane = FocusPane::TopNavBar;
                        topNavFocusedIndex = 1;
                        InvalidateRect(hWnd, NULL, FALSE);
                        return true;
                    }
                    int nextIdx = keyboardFocusedIndex - 1;
                    keyboardFocusedIndex = nextIdx;
                    selectedPlaylistIndices = { (size_t)nextIdx };
                    if (nextIdx < (int)railCount) activePlaylistId = library.playlists[nextIdx].id;
                    AutoScrollToFocusedItem();
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                if (wParam == VK_RETURN || wParam == VK_SPACE || wParam == VK_TAB) {
                    activeFocusPane = FocusPane::PlaylistDetail;
                    keyboardFocusedIndex = -1;
                    selectedEpisodeIndices.clear();
                    AutoScrollToFocusedItem();
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                return false;
            }
            if (activeFocusPane == FocusPane::PlaylistDetail) {
                int gridStep = 1;
                size_t gridColumns = 1;
                if (playlistGridView) {
                    RECT rc{}; GetClientRect(hWnd, &rc);
                    float dp = renderer.GetDpiScale();
                    float winW = (float)std::max<LONG>(1, rc.right - rc.left);
                    float padX = 18.0f * dp;
                    float avail = std::max(1.0f * dp, winW - 2.0f * padX);
                    float railW = std::min(260.0f * dp, std::max(120.0f * dp, avail * 0.42f));
                    float detailW = std::max(120.0f * dp, winW - padX - (padX + railW + 16.0f * dp) - 26.0f * dp);
                    gridColumns = (size_t)std::max(1.0f, std::floor((detailW + 10.0f * dp) / (190.0f * dp)));
                    gridStep = (int)std::max<size_t>(1, gridColumns);
                }
                if (keyboardFocusedIndex == -1) {
                    // On Continue Watching Big Card
                    if (wParam == VK_DOWN) {
                        if (!visible.empty()) {
                            keyboardFocusedIndex = (int)visible[0];
                            selectedEpisodeIndices = { visible[0] };
                        }
                        AutoScrollToFocusedItem();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return true;
                    }
                    if (wParam == VK_UP || wParam == VK_LEFT || wParam == VK_TAB) {
                        activeFocusPane = FocusPane::PlaylistRail;
                        int curPlIdx = 0;
                        for (size_t i = 0; i < library.playlists.size(); ++i) {
                            if (library.playlists[i].id == activePlaylistId) { curPlIdx = (int)i; break; }
                        }
                        keyboardFocusedIndex = curPlIdx;
                        selectedPlaylistIndices = { (size_t)curPlIdx };
                        AutoScrollToFocusedItem();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return true;
                    }
                    if (wParam == VK_RETURN || wParam == VK_SPACE) {
                        if (pl && !pl->itemPaths.empty()) {
                            size_t resIdx = library.GetPlaylistResumeIndex(pl);
                            PlayPlaylist(activePlaylistId, resIdx);
                        }
                        return true;
                    }
                    return false;
                }
                // On Episode Cards
                int currentPos = 0;
                auto found = std::find(visible.begin(), visible.end(), (size_t)std::max(0, keyboardFocusedIndex));
                if (found != visible.end()) currentPos = (int)std::distance(visible.begin(), found);
                if (wParam == VK_UP) {
                    if (isCtrl) {
                        int nextPos = std::max(0, currentPos - 5);
                        keyboardFocusedIndex = (int)visible[nextPos];
                        selectedEpisodeIndices = { visible[nextPos] };
                        AutoScrollToFocusedItem();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return true;
                    }
                    int nextPos = currentPos - gridStep;
                    if (nextPos < 0) {
                        // Return to Continue Watching Big Card
                        keyboardFocusedIndex = -1;
                        selectedEpisodeIndices.clear();
                    } else {
                        keyboardFocusedIndex = (int)visible[nextPos];
                        selectedEpisodeIndices = { visible[nextPos] };
                    }
                    AutoScrollToFocusedItem();
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                if (wParam == VK_DOWN) {
                    if (visible.empty()) return true;
                    if (isCtrl) {
                        int nextPos = std::min((int)visible.size() - 1, currentPos + 5);
                        keyboardFocusedIndex = (int)visible[nextPos];
                        selectedEpisodeIndices = { visible[nextPos] };
                        AutoScrollToFocusedItem();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return true;
                    }
                    int nextPos = std::min((int)visible.size() - 1, currentPos + gridStep);
                    keyboardFocusedIndex = (int)visible[nextPos];
                    selectedEpisodeIndices = { visible[nextPos] };
                    AutoScrollToFocusedItem();
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                if (wParam == VK_LEFT) {
                    if (playlistGridView && (currentPos % gridColumns) > 0) {
                        int nextPos = currentPos - 1;
                        keyboardFocusedIndex = (int)visible[nextPos];
                        selectedEpisodeIndices = { visible[nextPos] };
                        AutoScrollToFocusedItem();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return true;
                    }
                    activeFocusPane = FocusPane::PlaylistRail;
                    int curPlIdx = 0;
                    for (size_t i = 0; i < library.playlists.size(); ++i) {
                        if (library.playlists[i].id == activePlaylistId) { curPlIdx = (int)i; break; }
                    }
                    keyboardFocusedIndex = curPlIdx;
                    selectedPlaylistIndices = { (size_t)curPlIdx };
                    AutoScrollToFocusedItem();
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
                if (wParam == VK_RIGHT) {
                    if (playlistGridView && (currentPos % gridColumns) < (gridColumns - 1) && currentPos + 1 < (int)visible.size()) {
                        int nextPos = currentPos + 1;
                        keyboardFocusedIndex = (int)visible[nextPos];
                        selectedEpisodeIndices = { visible[nextPos] };
                        AutoScrollToFocusedItem();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return true;
                    }
                    return false;
                }
                if (wParam == VK_RETURN || wParam == VK_SPACE) {
                    if (keyboardFocusedIndex >= 0 && pl) { PlayPlaylist(activePlaylistId, (size_t)keyboardFocusedIndex); }
                    return true;
                }
                if (wParam == VK_TAB) {
                    activeFocusPane = FocusPane::PlaylistRail;
                    int curPlIdx = 0;
                    for (size_t i = 0; i < library.playlists.size(); ++i) {
                        if (library.playlists[i].id == activePlaylistId) { curPlIdx = (int)i; break; }
                    }
                    keyboardFocusedIndex = curPlIdx;
                    selectedPlaylistIndices = { (size_t)curPlIdx };
                    AutoScrollToFocusedItem();
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
            }
            return false;
        }
        // ZONE C: Main Content Area Navigation
        if (currentTab == TabView::History && activeFocusPane == FocusPane::HistoryList && wParam == VK_TAB && !isCtrl && !isAlt) {
            activeFocusPane = FocusPane::TopNavBar;
            topNavFocusedIndex = 2;
            isKeyboardNavActive = true;
            MarkAccessibilityDirty();
            RefreshAccessibilityTree(true);
            accessibilityTree.SetFocusedNode(102);
            InvalidateRect(hWnd, NULL, FALSE);
            return true;
        }
        if (currentTab == TabView::Media && GetVisibleMediaIndices().empty()) {
            int totalEmptyBtns = library.mediaItems.empty() ? 2 : 1;
            if (keyboardFocusedIndex < 0) keyboardFocusedIndex = 0;
            if (keyboardFocusedIndex >= totalEmptyBtns) keyboardFocusedIndex = totalEmptyBtns - 1;
            if (wParam == VK_LEFT) {
                if (keyboardFocusedIndex > 0) { keyboardFocusedIndex--; InvalidateRect(hWnd, NULL, FALSE); return true; }
            } else if (wParam == VK_RIGHT) {
                if (keyboardFocusedIndex < totalEmptyBtns - 1) { keyboardFocusedIndex++; InvalidateRect(hWnd, NULL, FALSE); return true; }
            } else if (wParam == VK_UP) {
                activeFocusPane = FocusPane::TopNavBar;
                topNavFocusedIndex = 0;
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            } else if (wParam == VK_RETURN || wParam == VK_SPACE) {
                if (library.mediaItems.empty()) {
                    if (keyboardFocusedIndex == 0) OpenFileDialog();
                    else if (keyboardFocusedIndex == 1) AddWatchedFolderPrompt();
                } else { if (keyboardFocusedIndex == 0) SetLibraryGroupMode(LibraryGroupMode::Flat); }
                return true;
            }
            return false;
        }
        if (currentTab == TabView::History && library.history.empty()) {
            if (keyboardFocusedIndex < 0) keyboardFocusedIndex = 0;
            if (wParam == VK_UP) {
                activeFocusPane = FocusPane::TopNavBar;
                topNavFocusedIndex = 2;
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            } else if (wParam == VK_RETURN || wParam == VK_SPACE) { if (keyboardFocusedIndex == 0) OpenFileDialog(); return true; }
            return false;
        }
        size_t totalCount = 0;
        FocusPane pane = activeFocusPane;
        if (currentTab == TabView::Media) {
            pane = FocusPane::MediaList;
            totalCount = GetVisibleMediaIndices().size();
        } else if (currentTab == TabView::Playlists) {
            if (activeFocusPane == FocusPane::PlaylistRail) {
                pane = FocusPane::PlaylistRail;
                totalCount = library.playlists.size();
            } else {
                pane = FocusPane::PlaylistDetail;
                Playlist* pl = library.FindPlaylist(activePlaylistId);
                totalCount = pl ? pl->itemPaths.size() : 0;
            }
        } else if (currentTab == TabView::History) { pane = FocusPane::HistoryList; totalCount = library.history.size(); }
        // Arrow Up: when at index 0 without Shift, smoothly navigates UP TO TOP NAV BAR!
        if (wParam == VK_UP) {
            if (keyboardFocusedIndex <= 0 && !isShift) {
                activeFocusPane = FocusPane::TopNavBar;
                if (currentTab == TabView::Media) topNavFocusedIndex = 0;
                else if (currentTab == TabView::Playlists) topNavFocusedIndex = 1;
                else if (currentTab == TabView::History) topNavFocusedIndex = 2;
                else if (currentTab == TabView::Settings) topNavFocusedIndex = 3;
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            } else if (totalCount > 0) {
                int nextIdx = std::max(0, keyboardFocusedIndex - 1);
                if (isCtrl) { keyboardFocusedIndex = nextIdx; } else {
                    HandleItemSelection(pane, (size_t)nextIdx, isShift, isCtrl, totalCount);
                    if (pane == FocusPane::PlaylistRail && nextIdx < (int)library.playlists.size())
                        activePlaylistId = library.playlists[nextIdx].id;
                }
                AutoScrollToFocusedItem();
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
        }
        // Arrow Down
        if (wParam == VK_DOWN) {
            if (totalCount > 0) {
                int nextIdx = (keyboardFocusedIndex < 0) ? 0 : std::min((int)totalCount - 1, keyboardFocusedIndex + 1);
                if (isCtrl) { keyboardFocusedIndex = nextIdx; } else {
                    HandleItemSelection(pane, (size_t)nextIdx, isShift, isCtrl, totalCount);
                    if (pane == FocusPane::PlaylistRail && nextIdx < (int)library.playlists.size())
                        activePlaylistId = library.playlists[nextIdx].id;
                }
                AutoScrollToFocusedItem();
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
        }
        if (wParam == VK_HOME) {
            if (totalCount > 0) {
                if (isCtrl) { keyboardFocusedIndex = 0; } else {
                    HandleItemSelection(pane, 0, isShift, isCtrl, totalCount);
                    if (pane == FocusPane::PlaylistRail && !library.playlists.empty()) { activePlaylistId = library.playlists[0].id; }
                }
                AutoScrollToFocusedItem();
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
        }
        if (wParam == VK_END) {
            if (totalCount > 0) {
                int lastIdx = (int)totalCount - 1;
                if (isCtrl) { keyboardFocusedIndex = lastIdx; } else {
                    HandleItemSelection(pane, (size_t)lastIdx, isShift, isCtrl, totalCount);
                    if (pane == FocusPane::PlaylistRail && lastIdx < (int)library.playlists.size())
                        activePlaylistId = library.playlists[lastIdx].id;
                }
                AutoScrollToFocusedItem();
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
        }
        if (wParam == VK_PRIOR) { // Page Up
            if (totalCount > 0) {
                int nextIdx = std::max(0, (keyboardFocusedIndex < 0 ? 0 : keyboardFocusedIndex - 5));
                if (isCtrl) { keyboardFocusedIndex = nextIdx; } else {
                    HandleItemSelection(pane, (size_t)nextIdx, isShift, isCtrl, totalCount);
                    if (pane == FocusPane::PlaylistRail && nextIdx < (int)library.playlists.size())
                        activePlaylistId = library.playlists[nextIdx].id;
                }
                AutoScrollToFocusedItem();
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
        }
        if (wParam == VK_NEXT) { // Page Down
            if (totalCount > 0) {
                int nextIdx = std::min((int)totalCount - 1, (keyboardFocusedIndex < 0 ? 0 : keyboardFocusedIndex + 5));
                if (isCtrl) { keyboardFocusedIndex = nextIdx; } else {
                    HandleItemSelection(pane, (size_t)nextIdx, isShift, isCtrl, totalCount);
                    if (pane == FocusPane::PlaylistRail && nextIdx < (int)library.playlists.size())
                        activePlaylistId = library.playlists[nextIdx].id;
                }
                AutoScrollToFocusedItem();
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
        }
        if (wParam == VK_SPACE) {
            if (isCtrl) {
                if (keyboardFocusedIndex >= 0 && (size_t)keyboardFocusedIndex < totalCount) {
                    HandleItemSelection(pane, (size_t)keyboardFocusedIndex, false, true, totalCount);
                    InvalidateRect(hWnd, NULL, FALSE);
                    return true;
                }
            } else { PlaySelectedInCurrentPane(); return true; }
        }
        if (wParam == VK_RETURN) {
            if (currentTab == TabView::Playlists && activeFocusPane == FocusPane::PlaylistRail) {
                activeFocusPane = FocusPane::PlaylistDetail;
                keyboardFocusedIndex = 0;
                AutoScrollToFocusedItem();
                InvalidateRect(hWnd, NULL, FALSE);
                return true;
            }
            PlaySelectedInCurrentPane();
            return true;
        }
    }
    return false;
}

} // namespace Aperture
