#include "Presentation.h"
#include "SplitIrisMenuLayout.h"
#include "UIComponents.h"
#include "PlaybackFlyoutLayout.h"
#include "AppLayout.h"
#include "ChannelPolicy.h"
#include "FeatureGate.h"
#include "Version.h"
#include "Localization.h"
#include "SettingsLayout.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace Aperture {

// DirectWrite formats are deliberately configured as single-line text.  Keep
// long user supplied names inside their layout columns instead of allowing the
// native renderer to paint over neighboring controls at narrow widths.
//
// Fit results are memoized: without this, every truncated title paid O(log n) full text-layout creations per frame (each binary-search
// candidate is a distinct string that pollutes the shared layout LRU), churning the layout cache on every playlist scroll. Memo hits verify
// with a single cached measure so font/locale changes self-correct instead of serving stale fits.
namespace FitMemo {
struct Key {
    std::wstring value;
    FontType font = FontType::Regular;
    int widthBucket = 0;
    bool rtl = false;
};
// Copy-free probe: hits must not copy the title into a full Key every frame.
struct KeyProbe {
    std::wstring_view value;
    FontType font = FontType::Regular;
    int widthBucket = 0;
    bool rtl = false;
};
struct KeyLess {
    using is_transparent = void;
    template <typename A, typename B>
    bool operator()(const A& a, const B& b) const {
        if (a.font != b.font) return a.font < b.font;
        if (a.widthBucket != b.widthBucket) return a.widthBucket < b.widthBucket;
        if (a.rtl != b.rtl) return a.rtl < b.rtl;
        return a.value < b.value;
    }
};
struct Entry { std::wstring fitted; std::list<Key>::iterator lruIt; };
constexpr size_t kFitCacheLimit = 512;
inline int Bucket(float maxWidth) { return (int)std::lround(maxWidth * 0.5f); }
} // namespace FitMemo

template <typename R>
static std::wstring FitSingleLineMemo(R& r, std::map<FitMemo::Key, FitMemo::Entry, FitMemo::KeyLess>& cache,
                                      std::list<FitMemo::Key>& lru, const std::wstring& value, FontType font, float maxWidth) {
    if (maxWidth <= 1.0f || value.empty() || r.MeasureString(value, font).width <= maxWidth) return value;
    const bool rtl = Localization::Instance().IsRtl();
    const FitMemo::KeyProbe probe{ std::wstring_view(value), font, FitMemo::Bucket(maxWidth), rtl };
    auto it = cache.find(probe);
    if (it != cache.end()) {
        // Single cached measure verifies the hit; a font change that widens
        // text falls through to a fresh binary search below.
        if (r.MeasureString(it->second.fitted, font).width <= maxWidth) {
            lru.splice(lru.begin(), lru, it->second.lruIt);
            it->second.lruIt = lru.begin();
            return it->second.fitted;
        }
        lru.erase(it->second.lruIt);
        cache.erase(it);
    }
    const std::wstring ellipsis = L"\x2026";
    if (r.MeasureString(ellipsis, font).width > maxWidth) return L"";
    size_t lo = 0;
    size_t hi = value.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        std::wstring candidate = value.substr(0, mid) + ellipsis;
        if (r.MeasureString(candidate, font).width <= maxWidth) lo = mid;
        else hi = mid - 1;
    }
    std::wstring fitted = value.substr(0, lo) + ellipsis;
    while (cache.size() >= FitMemo::kFitCacheLimit && !lru.empty()) { cache.erase(lru.back()); lru.pop_back(); }
    lru.push_front(FitMemo::Key{ std::wstring(value), font, probe.widthBucket, rtl });
    const FitMemo::Key& key = lru.front();
    FitMemo::Entry entry;
    entry.fitted = fitted;
    entry.lruIt = lru.begin();
    cache.emplace(key, std::move(entry));
    return fitted;
}

static std::wstring FitSingleLine(Renderer& r, const std::wstring& value, FontType font, float maxWidth) {
    static std::map<FitMemo::Key, FitMemo::Entry, FitMemo::KeyLess> cache;
    static std::list<FitMemo::Key> lru;
    return FitSingleLineMemo(r, cache, lru, value, font, maxWidth);
}

static std::wstring FitSingleLine(OverlayRenderer& r, const std::wstring& value, FontType font, float maxWidth) {
    static std::map<FitMemo::Key, FitMemo::Entry, FitMemo::KeyLess> cache;
    static std::list<FitMemo::Key> lru;
    return FitSingleLineMemo(r, cache, lru, value, font, maxWidth);
}

static float SafeWidth(float left, float right, float minimum = 0.0f) { return std::max(minimum, right - left); }

static void DrawMetadataPill(Renderer& r, const Settings& s, const D2D1_RECT_F& bounds, const std::wstring& text, bool accent = false,
                             FontType font = FontType::MonoSmall) {
    if (text.empty() || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    const float dp = r.GetDpiScale();
    const float radius = std::min(6.0f * dp, RectHeight(bounds) * 0.5f);
    const auto& th = s.currentTheme;
    r.FillRoundedRect(bounds, radius, accent ? ColorWithAlpha(th.accent, 0.14f) : ColorWithAlpha(th.surfaceElevated, 0.86f));
    r.DrawRoundedRect(bounds, radius, accent ? ColorWithAlpha(th.accent, 0.38f) : th.borderSubtle, 1.0f * dp);
    r.DrawString(text, bounds, accent ? th.accent : th.textMuted, font, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

static bool IsHighUtf16(wchar_t c) { const unsigned int u = static_cast<unsigned int>(c); return u >= 0xD800u && u <= 0xDBFFu; }

static bool IsLowUtf16(wchar_t c) { const unsigned int u = static_cast<unsigned int>(c); return u >= 0xDC00u && u <= 0xDFFFu; }

// Keep all renderer-side substring/measurement operations on the same UTF-16 boundaries that TextInputModel exposes. A mouse hit or
// composition range that accidentally points between a surrogate pair is snapped left.
static size_t SafeUtf16Boundary(const std::wstring& text, size_t index) {
    index = std::min(index, text.size());
    if (index > 0 && index < text.size() && IsHighUtf16(text[index - 1]) && IsLowUtf16(text[index])) { return index - 1; }
    return index;
}

template<class DrawContext> static float MeasureUtf16Prefix(DrawContext& r, const std::wstring& text, FontType font, size_t index) {
    index = SafeUtf16Boundary(text, index);
    return text.empty() || index == 0 ? 0.0f : r.MeasureString(text.substr(0, index), font).width;
}

template<class DrawContext> static std::wstring FitVisualText(DrawContext& r, const std::wstring& value, FontType font, float maxWidth) {
    if (maxWidth <= 1.0f || value.empty() || r.MeasureString(value, font).width <= maxWidth) return value;
    const std::wstring ellipsis = L"\x2026";
    if (r.MeasureString(ellipsis, font).width > maxWidth) return L"";
    std::vector<size_t> boundaries;
    boundaries.reserve(value.size() + 1);
    boundaries.push_back(0);
    for (size_t i = 0; i < value.size();) {
        size_t next = i + 1;
        if (IsHighUtf16(value[i]) && next < value.size() && IsLowUtf16(value[next])) next += 1;
        boundaries.push_back(next);
        i = next;
    }
    size_t lo = 0;
    size_t hi = boundaries.size() - 1;
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        const size_t boundary = boundaries[mid];
        const std::wstring candidate = value.substr(0, boundary) + ellipsis;
        if (r.MeasureString(candidate, font).width <= maxWidth) lo = mid;
        else hi = mid - 1;
    }
    return value.substr(0, boundaries[lo]) + ellipsis;
}

template<class DrawContext> void DrawTextInputImpl(DrawContext& r, const Settings& s, const D2D1_RECT_F& bounds, const TextInputVisualState& state,
    const std::wstring& placeholder, FontType font, float paddingX ) {
    const float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    const float left = bounds.left + std::max(0.0f, paddingX);
    const float right = bounds.right - std::max(0.0f, paddingX);
    const float availableW = std::max(1.0f, right - left);
    if (right <= left || bounds.bottom <= bounds.top) return;
    const bool hasValue = !state.displayText.empty();
    const bool canEllipsize = !state.focused && !state.hasCompositionRange && hasValue;
    const std::wstring drawText = canEllipsize ? FitVisualText(r, state.displayText, font, availableW) :
        (hasValue ? state.displayText : placeholder);
    const D2D1_COLOR_F textColor = hasValue ? th.text : th.textMuted;
    const bool isFullValue = hasValue && drawText == state.displayText;
    size_t compositionStart = 0, compositionEnd = 0;
    size_t visualCaret = SafeUtf16Boundary(drawText, state.caretIndex);
    if (state.hasCompositionRange && isFullValue) {
        compositionStart = SafeUtf16Boundary(drawText, state.compositionStart);
        compositionEnd = SafeUtf16Boundary(drawText, state.compositionEnd);
        if (compositionEnd < compositionStart) std::swap(compositionStart, compositionEnd);
        visualCaret = state.compositionCaretIndex == TextInputVisualState::kNoIndex ? compositionEnd :
            SafeUtf16Boundary(drawText, state.compositionCaretIndex);
    }
    visualCaret = std::min(visualCaret, drawText.size());
    float scrollX = 0.0f;
    if (state.focused && hasValue && isFullValue) {
        const float caretW = MeasureUtf16Prefix(r, drawText, font, visualCaret);
        if (caretW > availableW) scrollX = caretW - availableW;
    }
    const D2D1_RECT_F clipBounds = D2D1::RectF(left, bounds.top, right, bounds.bottom);
    r.PushClip(clipBounds);
    // Selection highlight is intentionally suppressed during composition: the IME replaces the captured committed selection visually, and
    // its composition underline communicates the temporary range instead.
    if (state.focused && !state.hasCompositionRange && hasValue && isFullValue) {
        size_t selectionStart = SafeUtf16Boundary(drawText, state.selectionStart);
        size_t selectionEnd = SafeUtf16Boundary(drawText, state.selectionEnd);
        if (selectionEnd < selectionStart) std::swap(selectionStart, selectionEnd);
        if (selectionEnd > selectionStart) {
            const float x1 = left + MeasureUtf16Prefix(r, drawText, font, selectionStart) - scrollX;
            const float x2 = left + MeasureUtf16Prefix(r, drawText, font, selectionEnd) - scrollX;
            const float insetY = std::min(3.0f * dp, std::max(0.0f, RectHeight(bounds) * 0.18f));
            r.FillRoundedRect(D2D1::RectF(x1, bounds.top + insetY, x2, bounds.bottom - insetY), 2.0f * dp, ColorWithAlpha(th.accent, 0.28f));
        }
    }
    const float textX = left - scrollX;
    const D2D1_RECT_F textBounds = D2D1::RectF(textX, bounds.top, textX + availableW, bounds.bottom);
    r.DrawString(drawText, textBounds, textColor, font, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (state.hasCompositionRange && isFullValue && compositionEnd > compositionStart) {
        const float x1 = left + MeasureUtf16Prefix(r, drawText, font, compositionStart) - scrollX;
        const float x2 = left + MeasureUtf16Prefix(r, drawText, font, compositionEnd) - scrollX;
        r.DrawLine(x1, bounds.bottom - 4.0f * dp, x2, bounds.bottom - 4.0f * dp, th.accent, 1.5f * dp);
    }
    if (state.focused && state.caretVisible) {
        const float caretX = left + MeasureUtf16Prefix(r, drawText, font, visualCaret) - scrollX;
        r.DrawLine(caretX, bounds.top + 5.0f * dp, caretX, bounds.bottom - 5.0f * dp, th.accent, 1.5f * dp);
    }
    r.PopClip();
}

void UIComponents::DrawTextInput(Renderer& r, const Settings& s, const D2D1_RECT_F& b, const TextInputVisualState& v, const std::wstring& p, FontType f, float x) { DrawTextInputImpl(r, s, b, v, p, f, x); }
void UIComponents::DrawTextInput(OverlayRenderer& r, const Settings& s, const D2D1_RECT_F& b, const TextInputVisualState& v, const std::wstring& p, FontType f, float x) { DrawTextInputImpl(r, s, b, v, p, f, x); }

void UIComponents::DrawNavBar(Renderer& r, const Settings& s, const NavBarViewModel& vm) {
    (void)vm.hoveredTab; (void)vm.isSettingsHovered; (void)vm.isOpenHovered;
    const float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    const bool isRtl = Localization::Instance().IsRtl();
    // Bounds are inset symmetrically, so the window width is recoverable here.
    const float winW = vm.bounds.left + vm.bounds.right;
    const auto nav = Presentation::Nav(winW, dp, isRtl, [&](const std::wstring& t) { return r.MeasureString(t, FontType::Medium).width; });
    const float iconSz = nav.iconSize, gap = nav.iconGap, tabH = nav.tabH;
    // Icon + label centered inside a rect (mirrored for RTL).
    const auto drawContent = [&](const D2D1_RECT_F& rect, IconType icon, const std::wstring& label, D2D1_COLOR_F iconCol,
                                 D2D1_COLOR_F textCol, float shiftY, float rotation) {
        const float strW = r.MeasureString(label, FontType::Medium).width;
        const float contentW = iconSz + gap + strW;
        const float top = rect.top + (tabH - iconSz) * 0.5f + shiftY;
        if (!isRtl) {
            const float x0 = rect.left + (RectWidth(rect) - contentW) * 0.5f;
            r.DrawIconRotated(icon, D2D1::RectF(x0, top, x0 + iconSz, top + iconSz), iconCol, rotation, 1.4f * dp);
            r.DrawString(label, D2D1::RectF(x0 + iconSz + gap, rect.top + shiftY, rect.right, rect.bottom + shiftY), textCol, FontType::Medium,
                         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else {
            const float x0 = rect.right - (RectWidth(rect) - contentW) * 0.5f;
            r.DrawIconRotated(icon, D2D1::RectF(x0 - iconSz, top, x0, top + iconSz), iconCol, rotation, 1.4f * dp);
            r.DrawString(label, D2D1::RectF(rect.left, rect.top + shiftY, x0 - iconSz - gap, rect.bottom + shiftY), textCol, FontType::Medium,
                         DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    };
    // 1. Segmented track with a sliding selection pill.
    const float trackR = RectHeight(nav.track) * 0.5f;
    r.FillRoundedRect(nav.track, trackR, LerpColor(th.bg, th.text, 0.045f));
    r.DrawRoundedRect(nav.track, trackR, th.borderSubtle, 1.0f * dp);
    const bool tabActive = vm.activeTab != TabView::Settings;
    const float pos = std::clamp(vm.indicatorPos, 0.0f, 2.0f);
    if (tabActive) {
        const int i0 = std::min(2, static_cast<int>(std::floor(pos)));
        const int i1 = std::min(2, i0 + 1);
        const float f = pos - static_cast<float>(i0);
        const auto lerp = [f](float a, float b) { return a + (b - a) * f; };
        const D2D1_RECT_F pill = D2D1::RectF(lerp(nav.tabs[i0].left, nav.tabs[i1].left), nav.tabs[0].top,
                                             lerp(nav.tabs[i0].right, nav.tabs[i1].right), nav.tabs[0].bottom);
        SurfaceStyle ps;
        ps.radius = tabH * 0.5f;
        ps.fill = LerpColor(th.cardBg, th.text, 0.10f);
        ps.border = ColorWithAlpha(th.accent, 0.38f);
        ps.borderWidth = 1.0f;
        ps.hasInnerHighlight = false;
        ps.shadowColor = D2D1::ColorF(0, 0, 0, 0.35f);
        ps.shadowBlur = 8.0f * dp;
        ps.shadowOffsetY = 2.0f * dp;
        r.DrawSurface(pill, ps);
    }
    const struct { IconType icon; std::wstring name; float mix; } tabs[3] = {
        { IconType::MediaFilm, Loc::Tr("nav.media"), std::clamp(vm.mediaHoverMix, 0.0f, 1.0f) },
        { IconType::FolderPlaylist, Loc::Tr("nav.playlists"), std::clamp(vm.playlistHoverMix, 0.0f, 1.0f) },
        { IconType::HistoryClock, Loc::Tr("nav.history"), std::clamp(vm.historyHoverMix, 0.0f, 1.0f) } };
    for (int i = 0; i < 3; ++i) {
        const bool isFocused = (vm.focusedNavIndex == i), isPressed = (vm.pressedNavIndex == i);
        const bool isNavDrop = (i == 1 && vm.isPlaylistsNavDropHovered);
        const float mix = isPressed ? 1.0f : tabs[i].mix;
        const float weight = tabActive ? std::clamp(1.0f - std::abs(pos - static_cast<float>(i)), 0.0f, 1.0f) : 0.0f;
        const D2D1_RECT_F rect = nav.tabs[i];
        if (isNavDrop) {
            r.FillRoundedRect(rect, tabH * 0.5f, ColorWithAlpha(th.accent, 0.20f));
            r.DrawRoundedRect(rect, tabH * 0.5f, th.accent, 1.5f * dp);
        } else if (weight < 0.5f && mix > 0.01f) {
            r.FillRoundedRect(rect, tabH * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f * mix));
        }
        if (isFocused) r.DrawRoundedRect(InsetRectF(rect, -2.0f * dp, -2.0f * dp), tabH * 0.5f + 2.0f * dp, th.focusRing, 1.5f * dp);
        const float emphasis = std::max(weight, mix * 0.85f);
        const D2D1_COLOR_F textCol = isNavDrop ? th.text : LerpColor(th.textMuted, th.text, isFocused ? 1.0f : emphasis);
        const D2D1_COLOR_F iconCol = isNavDrop ? th.accent : LerpColor(LerpColor(th.textMuted, th.text, emphasis), th.accent, weight);
        drawContent(rect, tabs[i].icon, isNavDrop ? Loc::Tr("nav.drop_to_playlist") : tabs[i].name, iconCol, textCol,
                    isPressed ? 1.0f * dp : 0.0f, 0.0f);
    }
    // 2. Settings: quiet ghost pill; accent-tinted while its page is open.
    {
        const bool active = (vm.activeTab == TabView::Settings), focused = (vm.focusedNavIndex == 3), pressed = (vm.pressedNavIndex == 3);
        const float mix = pressed ? 1.0f : std::clamp(vm.settingsHoverMix, 0.0f, 1.0f);
        const float rad = tabH * 0.5f;
        if (active) {
            r.FillRoundedRect(nav.settings, rad, th.surfaceSelected);
            r.DrawRoundedRect(nav.settings, rad, ColorWithAlpha(th.accent, 0.38f), 1.0f * dp);
        } else if (mix > 0.01f) {
            r.FillRoundedRect(nav.settings, rad, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f * mix));
        }
        if (focused) r.DrawRoundedRect(InsetRectF(nav.settings, -2.0f * dp, -2.0f * dp), rad + 2.0f * dp, th.focusRing, 1.5f * dp);
        const D2D1_COLOR_F col = LerpColor(th.textMuted, th.text, (active || focused) ? 1.0f : mix * 0.85f);
        drawContent(nav.settings, IconType::SettingsGear, Loc::Tr("nav.settings"), active ? th.accent : col, col,
                    pressed ? 1.0f * dp : 0.0f, vm.settingsRotationDegrees);
    }
    // 3. Open File: the one filled, accent-colored call to action.
    {
        const bool focused = (vm.focusedNavIndex == 4), pressed = (vm.pressedNavIndex == 4);
        const float mix = pressed ? 1.0f : std::clamp(vm.openHoverMix, 0.0f, 1.0f);
        const auto colors = Presentation::Button(th, true, false, true, pressed);
        const float shift = pressed ? 1.0f * dp : 0.0f, rad = tabH * 0.5f;
        const D2D1_RECT_F rect = D2D1::RectF(nav.open.left, nav.open.top + shift, nav.open.right, nav.open.bottom + shift);
        if (focused) r.DrawRoundedRect(InsetRectF(rect, -3.0f * dp, -3.0f * dp), rad + 3.0f * dp, th.focusRing, 1.5f * dp);
        r.FillRoundedRect(rect, rad, LerpColor(th.accent, colors.fill, mix));
        r.DrawString(Loc::Tr("nav.open_file"), rect, colors.text, FontType::Medium, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}
void UIComponents::DrawMediaItemCard( Renderer& r, const Settings& s, const D2D1_RECT_F& bounds, const MediaItem& item, bool isHovered,
    bool isSelected, bool isPinnedTop, float previewOpacity, ID2D1Bitmap* pLivePreviewBitmap, bool isCloseHovered,
    const std::string* statusOverride ) {
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    // The library item is read-only for drawing; a frame-local override (for
    // example "missing" detected at view-model build time) replaces its status.
    const std::string& effectiveStatus = statusOverride ? *statusOverride : item.status;
    bool isDone = (item.durationSec > 0.0 && (item.lastPosSec / item.durationSec) >= 0.90) || (effectiveStatus == "done");
    double progressRatio = item.GetProgressRatio();
    const bool isMissing = effectiveStatus == "missing";
    // 1. Surface Style Calculation. App animates hovered rows into the
    // WebView's large preview state (currently 160–230 logical px depending on the wave). Infer that state from the supplied bounds so the
    // drawing remains compatible with the existing public signature.
    const float cardH = RectHeight(bounds);
    const bool isExpanded = !isPinnedTop && cardH >= 135.0f * dp;
    SurfaceStyle style = r.CreateSurfaceStyle(
        isExpanded ? Elevation::ExpandedCard : (isPinnedTop ? Elevation::RaisedCard : (isHovered ? Elevation::RaisedCard : Elevation::Card)),
        s, isHovered, isSelected, isPinnedTop );
    if (isMissing) {
        style.border = ColorWithAlpha(th.red, 0.72f);
        style.borderWidth = 1.5f;
        style.fill = ColorWithAlpha(th.surfaceElevated, 0.82f);
    }
    style.backdropFile = item.filePath;
    style.backdropOpacity = isExpanded ? 0.42f : (isPinnedTop ? (isHovered ? 0.35f : 0.22f) : (isHovered ? 0.30f : 0.18f));
    style.pLivePreviewBitmap = pLivePreviewBitmap;
    style.livePreviewOpacity = isExpanded ? std::clamp(previewOpacity, 0.0f, 1.0f) : previewOpacity;
    style.hasInnerHighlight = true;
    style.innerHighlight = D2D1::ColorF(1.0f, 1.0f, 1.0f, isExpanded ? 0.18f : (isHovered ? 0.15f : 0.07f));
    if (isPinnedTop) {
        // [Disabled per user request] Gold hue glow effect around pinned top card
        // style.glowColor = ColorWithAlpha(th.accent, isHovered ? 0.25f : 0.14f);
        // style.glowBlur = (isHovered ? 20.0f : 14.0f) * dp;
        style.glowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
        style.glowBlur = 0.0f;
        style.leftAccentBarAlpha = 1.0f;
        style.leftAccentBarColor = th.accent;
        style.leftAccentBarWidth = 6.0f;
    } else { style.leftAccentBarAlpha = 0.0f; style.leftAccentBarColor = th.accent; style.leftAccentBarWidth = 0.0f; }
    // 2. Render Card Surface Base
    r.DrawSurface(bounds, style);
    if (isPinnedTop) {
        // ==========================================
        // CONTINUE WATCHING (PINNED TOP CARD)
        // ==========================================
        // WebView pinned rows use 22px content padding after the 6px accent border (x=58 at the reference 1280px capture).
        float padL = bounds.left + 28.0f * dp;
        // Dismiss (X) Close button: vertically centered on far right
        float closeSize = 28.0f * dp, closeX = bounds.right - 14.0f * dp - closeSize;
        float closeY = bounds.top + (cardH - closeSize) * 0.5f;
        D2D1_RECT_F closeRect = D2D1::RectF(closeX, closeY, closeX + closeSize, closeY + closeSize);
        float closeCX = closeX + closeSize * 0.5f, closeCY = closeY + closeSize * 0.5f;
        if (isCloseHovered) {
            r.FillCircle(closeCX, closeCY, closeSize * 0.5f, D2D1::ColorF(0.95f, 0.25f, 0.35f, 0.25f));
            r.DrawCircle(closeCX, closeCY, closeSize * 0.5f, D2D1::ColorF(0.95f, 0.25f, 0.35f, 0.60f), 1.0f);
            r.DrawIcon(IconType::CloseX, InsetRectF(closeRect, 7.5f * dp, 7.5f * dp), D2D1::ColorF(0.95f, 0.25f, 0.35f));
        } else {
            r.FillCircle(closeCX, closeCY, closeSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f));
            r.DrawCircle(closeCX, closeCY, closeSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f), 1.0f);
            r.DrawIcon(IconType::CloseX, InsetRectF(closeRect, 8.0f * dp, 8.0f * dp), th.textMuted);
        }
        // Content right bound is to the left of the close button
        float padR = closeX - 12.0f * dp;
        // Duration string on top right
        std::wstring timeStr = isMissing ? Loc::Tr("media.missing") : Loc::Tr("media.duration_unavailable");
        if (!isMissing && item.durationSec > 0.0)
            timeStr = isDone ? FormatDuration(item.durationSec) : (FormatDuration(item.lastPosSec) + L" / " + FormatDuration(item.durationSec));
        D2D1_SIZE_F timeSize = r.MeasureString(timeStr, FontType::Medium);
        float timeX = padR - timeSize.width;
        D2D1_RECT_F timeRect = D2D1::RectF(timeX - 4.0f * dp, bounds.top + 14.0f * dp, padR + 4.0f * dp, bounds.top + 34.0f * dp);
        r.DrawString(timeStr, timeRect, isMissing ? th.red : (isDone ? th.accent : th.textMuted), FontType::Medium, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Title Line on top-left
        float titleY = bounds.top + 14.0f * dp;
        std::wstring titleText = item.GetDisplayTitle();
        std::wstring sizeText = item.fileSizeBytes > 0 ? (L" (" + FormatFileSize(item.fileSizeBytes) + L")") : L"";
        D2D1_SIZE_F titleSize = r.MeasureString(titleText, FontType::Title);
        D2D1_SIZE_F sizeSize = r.MeasureString(sizeText, FontType::Regular);
        float maxTitleW = std::max(0.0f, timeX - padL - sizeSize.width - 24.0f * dp);
        titleText = FitSingleLine(r, titleText, FontType::Title, maxTitleW);
        titleSize = r.MeasureString(titleText, FontType::Title);
        float drawTitleW = std::min(titleSize.width, maxTitleW);
        D2D1_RECT_F titleRect = D2D1::RectF(padL, titleY - 2.0f * dp, padL + drawTitleW + 8.0f * dp, titleY + 24.0f * dp);
        r.DrawStringWithShadow(titleText, titleRect, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.90f), FontType::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (!sizeText.empty()) {
            D2D1_RECT_F sizeRect = D2D1::RectF(titleRect.right + 4.0f * dp, titleY - 2.0f * dp, titleRect.right + 4.0f * dp + sizeSize.width + 8.0f * dp, titleY + 24.0f * dp);
            r.DrawString(sizeText, sizeRect, th.textMuted, FontType::Regular, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // Sleek Progress Bar pulled up below title line
        float trackY = bounds.top + 52.0f * dp, trackH = 7.0f * dp;
        D2D1_RECT_F trackRect = D2D1::RectF(padL, trackY, padR, trackY + trackH);
        // DrawProgressBar scales its radius internally; pass logical dp here
        // so high-DPI rendering does not inflate the groove twice.
        r.DrawProgressBar(trackRect, progressRatio, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f), th.accent, 3.5f);
    } else if (isExpanded) {
        // ==========================================
        // HOVER-EXPANDED MEDIA PREVIEW
        // ==========================================
        // WebView uses 20px padding around a full-bleed backdrop/video frame.
        // Renderer::DrawSurface already clips the bitmap and applies a dark
        // horizontal scrim; keep all text in the same stable columns while
        // the row's height animates underneath it.
        float padL = bounds.left + 20.0f * dp, padR = bounds.right - 20.0f * dp, dragW = 16.0f * dp, dragH = 16.0f * dp;
        D2D1_RECT_F dragRect = D2D1::RectF(padL, bounds.top + 20.0f * dp, padL + dragW, bounds.top + 20.0f * dp + dragH);
        r.DrawIcon(IconType::DragHandle, dragRect, ColorWithAlpha(th.accent, isHovered ? 0.95f : 0.70f), 1.45f * dp);
        float contentLeft = dragRect.right + 18.0f * dp, titleY = bounds.top + 18.0f * dp;
        std::wstring titleText = item.GetDisplayTitle();
        std::wstring sizeText = item.fileSizeBytes > 0 ? (L" (" + FormatFileSize(item.fileSizeBytes) + L")") : L"";
        std::wstring timeStr = isMissing ? Loc::Tr("media.missing") : Loc::Tr("media.duration_unavailable");
        if (!isMissing && item.durationSec > 0.0)
            timeStr = isDone ? FormatDuration(item.durationSec) : (FormatDuration(item.lastPosSec) + L" / " + FormatDuration(item.durationSec));
        float timeW = r.MeasureString(timeStr, FontType::MonoSmall).width;
        float timeRight = padR;
        float timeLeft = std::max(contentLeft + 30.0f * dp, timeRight - timeW - 2.0f * dp);
        r.DrawString(timeStr, D2D1::RectF(timeLeft, titleY, timeRight, titleY + 22.0f * dp), isMissing ? th.red : (isDone ? th.accent : th.textMuted), FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        float maxTitleW = std::max(24.0f * dp, timeLeft - contentLeft - r.MeasureString(sizeText, FontType::Regular).width - 18.0f * dp);
        titleText = FitSingleLine(r, titleText, FontType::Title, maxTitleW);
        D2D1_SIZE_F titleSize = r.MeasureString(titleText, FontType::Title);
        D2D1_RECT_F titleRect = D2D1::RectF(contentLeft, titleY - 1.0f * dp, contentLeft + std::min(titleSize.width, maxTitleW) + 8.0f * dp, titleY + 24.0f * dp);
        r.DrawStringWithShadow(titleText, titleRect, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.92f), FontType::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (!sizeText.empty()) {
            D2D1_RECT_F sizeRect = D2D1::RectF(titleRect.right + 4.0f * dp, titleY - 1.0f * dp, titleRect.right + 4.0f * dp + r.MeasureString(sizeText, FontType::Regular).width + 8.0f * dp, titleY + 24.0f * dp);
            r.DrawString(sizeText, sizeRect, th.textMuted, FontType::Regular, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // 10px Web-style track below the title; a completed item gets the
        // textual status treatment used by the reference UI.
        float trackY = bounds.top + 56.0f * dp;
        if (isDone) {
            r.DrawString(Loc::Tr("media.done"), D2D1::RectF(contentLeft, trackY - 3.0f * dp, padR, trackY + 22.0f * dp), th.accent, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else {
            D2D1_RECT_F trackRect = D2D1::RectF(contentLeft, trackY, padR, trackY + 10.0f * dp);
            r.DrawProgressBar(trackRect, progressRatio, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.68f), th.accent, 5.0f);
        }
    } else {
        // ==========================================
        // STANDARD MEDIA ITEM CARD
        // ==========================================
        // CSS row padding is 16px, followed by a 16px drag handle and an
        // 18px flex gap.  This keeps title columns aligned with WebView.
        float padL = bounds.left + 16.0f * dp, padR = bounds.right - 16.0f * dp;
        // 1. Drag Handle Dots (::) on the left - enlarged ~10-15% for optimal visibility
        float dragW = 16.0f * dp, dragH = 16.0f * dp, dragX = padL;
        float dragY = bounds.top + (cardH - dragH) * 0.5f;
        D2D1_RECT_F dragRect = D2D1::RectF(dragX, dragY, dragX + dragW, dragY + dragH);
        D2D1_COLOR_F dragCol = isHovered ? ColorWithAlpha(th.accent, 0.95f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f);
        r.DrawIcon(IconType::DragHandle, dragRect, dragCol);
        float contentLeft = dragRect.right + 18.0f * dp;
        // Duration string on the right
        std::wstring timeStr = isMissing ? Loc::Tr("media.missing") : Loc::Tr("media.duration_unavailable");
        if (!isMissing && item.durationSec > 0.0)
            timeStr = isDone ? FormatDuration(item.durationSec) : (FormatDuration(item.lastPosSec) + L" / " + FormatDuration(item.durationSec));
        D2D1_SIZE_F timeSize = r.MeasureString(timeStr, FontType::Medium);
        float timeX = padR - timeSize.width;
        D2D1_RECT_F timeRect = D2D1::RectF(timeX - 4.0f * dp, bounds.top + 12.0f * dp, padR + 4.0f * dp, bounds.top + 32.0f * dp);
        r.DrawString(timeStr, timeRect, isMissing ? th.red : (isDone ? th.accent : th.textMuted), FontType::Medium, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Title Line on top-left
        float titleY = bounds.top + 12.0f * dp;
        std::wstring titleText = item.GetDisplayTitle();
        std::wstring sizeText = item.fileSizeBytes > 0 ? (L" (" + FormatFileSize(item.fileSizeBytes) + L")") : L"";
        D2D1_SIZE_F titleSize = r.MeasureString(titleText, FontType::Subheading);
        D2D1_SIZE_F sizeSize = r.MeasureString(sizeText, FontType::Regular);
        float maxTitleW = std::max(0.0f, timeX - contentLeft - sizeSize.width - 24.0f * dp);
        titleText = FitSingleLine(r, titleText, FontType::Subheading, maxTitleW);
        titleSize = r.MeasureString(titleText, FontType::Subheading);
        float drawTitleW = std::min(titleSize.width, maxTitleW);
        D2D1_RECT_F titleRect = D2D1::RectF(contentLeft, titleY - 2.0f * dp, contentLeft + drawTitleW + 8.0f * dp, titleY + 22.0f * dp);
        r.DrawStringWithShadow(titleText, titleRect, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.85f), FontType::Subheading, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (!sizeText.empty()) {
            D2D1_RECT_F sizeRect = D2D1::RectF(titleRect.right + 4.0f * dp, titleY - 2.0f * dp, titleRect.right + 4.0f * dp + sizeSize.width + 8.0f * dp, titleY + 22.0f * dp);
            r.DrawString(sizeText, sizeRect, th.textMuted, FontType::Regular, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // Sleek Progress Bar pulled up
        float trackY = bounds.top + 44.0f * dp, trackH = 6.0f * dp;
        D2D1_RECT_F trackRect = D2D1::RectF(contentLeft, trackY, padR, trackY + trackH);
        r.DrawProgressBar(trackRect, progressRatio, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f), th.accent, 3.0f);
    }
}

void UIComponents::DrawHistoryGroupHeader(Renderer& r, const Settings& s, const HistoryGroupSummary& vm) {
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    // Icon on left
    float iconSz = 16.0f * dp;
    float iconY = vm.headerRect.top + (RectHeight(vm.headerRect) - iconSz) * 0.5f;
    D2D1_RECT_F iconRect = D2D1::RectF(vm.headerRect.left + 4.0f * dp, iconY, vm.headerRect.left + 4.0f * dp + iconSz, iconY + iconSz);
    r.DrawIcon(IconType::HistoryClock, iconRect, th.accent);
    // Group Title
    D2D1_SIZE_F titleSize = r.MeasureString(vm.title, FontType::Subheading);
    float maxTitleW = std::min(titleSize.width + 4.0f * dp, RectWidth(vm.headerRect) * 0.55f);
    D2D1_RECT_F titleRect = D2D1::RectF(iconRect.right + 8.0f * dp, vm.headerRect.top, iconRect.right + 8.0f * dp + maxTitleW, vm.headerRect.bottom);
    r.DrawString(vm.title, titleRect, th.text, FontType::Subheading, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Count badge
    D2D1_SIZE_F badgeSize = r.MeasureString(vm.countText, FontType::MonoSmall);
    float badgeW = badgeSize.width + 16.0f * dp, badgeH = 20.0f * dp;
    float badgeY = vm.headerRect.top + (RectHeight(vm.headerRect) - badgeH) * 0.5f;
    D2D1_RECT_F badgeRect = D2D1::RectF(titleRect.right + 8.0f * dp, badgeY, titleRect.right + 8.0f * dp + badgeW, badgeY + badgeH);
    DrawMetadataPill(r, s, badgeRect, vm.countText);
    // Divider line extending to right edge
    float lineLeft = badgeRect.right + 12.0f * dp, lineRight = vm.headerRect.right - 4.0f * dp;
    if (lineRight > lineLeft + 10.0f * dp) {
        float lineY = vm.headerRect.top + RectHeight(vm.headerRect) * 0.5f;
        r.DrawLine(lineLeft, lineY, lineRight, lineY, ColorWithAlpha(th.border, 0.35f), 1.0f * dp);
    }
}

void UIComponents::DrawSettingsView(Renderer& r, const Settings& s, const SettingsTabViewModel& vm) {
    float dp = r.GetDpiScale();
    // Hover feedback is resolved here from the live pointer position: the row rectangles exist only inside this function, and settings rows
    // must respond to hover before a click happens.
    auto pointerIn = [&](const D2D1_RECT_F& rc) {
        return vm.mouseX >= rc.left && vm.mouseX <= rc.right && vm.mouseY >= rc.top && vm.mouseY <= rc.bottom;
    };
    const auto& th = s.currentTheme;
    // Settings follows the reference layout's flatter visual hierarchy:
    // canvas -> pane/card -> compact control.  Keep the theme's semantic
    // colors, but do not let the generic acrylic surface defaults introduce a drop shadow on every row.
    const D2D1_COLOR_F canvasBg            = th.bg;
    const D2D1_COLOR_F surfaceBg           = th.cardBg;
    const D2D1_COLOR_F surfaceCard         = th.iconBg;
    const D2D1_COLOR_F surfaceBorder       = th.border;
    const D2D1_COLOR_F surfaceSelected     = th.surfaceSelected;
    const D2D1_COLOR_F surfaceHover        = th.surfaceHover;
    const D2D1_COLOR_F textPri             = th.text;
    const D2D1_COLOR_F textNorm            = th.text;
    const D2D1_COLOR_F textMute            = th.textMuted;
    const D2D1_COLOR_F textOnAccent        = canvasBg;
    const D2D1_COLOR_F accentCol           = th.accent;
    const D2D1_COLOR_F toggleOff           = th.trackBg;
    const D2D1_COLOR_F knobOff             = th.textMuted;
    const D2D1_COLOR_F dangerCol           = th.red;
    const D2D1_COLOR_F dangerBorder        = ColorWithAlpha(th.red, 0.35f);
    const D2D1_COLOR_F clearColor           = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    const D2D1_COLOR_F panelFill            = ColorWithAlpha(surfaceBg, 0.96f);
    const D2D1_COLOR_F panelBorder          = ColorWithAlpha(surfaceBorder, 0.24f);
    auto makeFlatSurface = [&](float radius = 8.0f) {
        SurfaceStyle style;
        style.radius = radius * dp;
        style.shadowColor = clearColor;
        style.shadowBlur = 0.0f;
        style.shadowOffsetY = 0.0f;
        style.glowColor = clearColor;
        style.glowBlur = 0.0f;
        style.hasInnerHighlight = false;
        return style;
    };
    auto drawFlatPanel = [&](const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& fill, const D2D1_COLOR_F& border, float borderWidth = 1.0f) {
        r.FillRoundedRect(rect, radius * dp, fill);
        if (border.a > 0.001f && borderWidth > 0.0f) { r.DrawRoundedRect(rect, radius * dp, border, borderWidth * dp); }
    };
    bool showSidebarKbFocus = vm.isKeyboardNavActive && vm.isSidebarFocused;
    bool showContentKbFocus = vm.isKeyboardNavActive && vm.isContentFocused;
    // Sidebar Layout Geometry
    bool isRtl = Localization::Instance().IsRtl();
    float sideW = SettingsLayout::kSidebarWidth * dp;
    D2D1_RECT_F sideBounds = isRtl ? D2D1::RectF(vm.bounds.right - sideW, vm.bounds.top, vm.bounds.right, vm.bounds.bottom)
                                   : D2D1::RectF(vm.bounds.left, vm.bounds.top, vm.bounds.left + sideW, vm.bounds.bottom);
    D2D1_RECT_F contentBounds = isRtl ? D2D1::RectF(vm.bounds.left, vm.bounds.top, vm.bounds.right - sideW - 16.0f * dp, vm.bounds.bottom)
                                      : D2D1::RectF(vm.bounds.left + sideW + 16.0f * dp, vm.bounds.top, vm.bounds.right, vm.bounds.bottom);
    // The reference uses a continuous two-pane canvas instead of nested
    // rounded containers.  A single divider gives the sidebar its boundary.
    r.FillRect(sideBounds, surfaceBg);
    r.FillRect(contentBounds, canvasBg);
    if (isRtl) {
        r.DrawLine(contentBounds.right, contentBounds.top, contentBounds.right, contentBounds.bottom, ColorWithAlpha(th.borderSubtle, 0.85f), 1.0f * dp);
    } else {
        r.DrawLine(contentBounds.left, contentBounds.top, contentBounds.left, contentBounds.bottom, ColorWithAlpha(th.borderSubtle, 0.85f), 1.0f * dp);
    }
    struct CatDef { SettingsCategory cat; std::wstring name; IconType icon; };
    CatDef cats[] = {
        { SettingsCategory::Experience, Loc::Tr("settings.category.experience"), IconType::SettingsGear },
        { SettingsCategory::Appearance, Loc::Tr("settings.category.appearance"), IconType::AspectFrame },
        { SettingsCategory::Playback, Loc::Tr("settings.category.playback"), IconType::Play },
        { SettingsCategory::Subtitles, Loc::Tr("settings.category.subtitles"), IconType::Subtitles },
        { SettingsCategory::Shortcuts, Loc::Tr("settings.category.shortcuts"), IconType::SpeedGauge },
        {SettingsCategory::System, Loc::Tr("settings.category.system"), IconType::SettingsGear}};
    float catH = SettingsLayout::kSidebarItemHeight * dp, curCatY = sideBounds.top + SettingsLayout::kSidebarTopInset * dp;
    const float catPos = std::clamp(vm.categoryIndicatorPos, 0.0f, 5.0f);
    const float itemStep = catH + SettingsLayout::kSidebarItemGap * dp;
    const float pillTop = (sideBounds.top + SettingsLayout::kSidebarTopInset * dp) + catPos * itemStep;
    const D2D1_RECT_F slidingPill = D2D1::RectF(sideBounds.left + 8.0f * dp, pillTop, sideBounds.right - 8.0f * dp, pillTop + catH);
    SurfaceStyle pillStyle;
    pillStyle.radius = 8.0f * dp;
    pillStyle.fill = surfaceSelected;
    pillStyle.border = ColorWithAlpha(accentCol, 0.35f);
    pillStyle.borderWidth = 1.0f * dp;
    pillStyle.shadowColor = D2D1::ColorF(0, 0, 0, 0.25f);
    pillStyle.shadowBlur = 6.0f * dp;
    pillStyle.shadowOffsetY = 1.0f * dp;
    r.DrawSurface(slidingPill, pillStyle);
    const float barW = 3.0f * dp, barInset = 8.0f * dp;
    if (isRtl) {
        r.FillRoundedRect(D2D1::RectF(slidingPill.right - barW, slidingPill.top + barInset, slidingPill.right, slidingPill.bottom - barInset), barW * 0.5f, accentCol);
    } else {
        r.FillRoundedRect(D2D1::RectF(slidingPill.left, slidingPill.top + barInset, slidingPill.left + barW, slidingPill.bottom - barInset), barW * 0.5f, accentCol);
    }
    for (int i = 0; i < 6; ++i) {
        const auto& c = cats[i];
        bool isActive = (vm.activeCat == c.cat);
        bool isHover = (vm.hoveredCat == c.cat);
        bool isFocused = showSidebarKbFocus && (vm.activeCat == c.cat);
        bool isPressed = vm.pressState.IsCategoryPressed(c.cat);
        D2D1_RECT_F catRect = D2D1::RectF(sideBounds.left + 8.0f * dp, curCatY, sideBounds.right - 8.0f * dp, curCatY + catH);
        SurfaceStyle cStyle = makeFlatSurface();
        if (isPressed) {
            cStyle.fill = ColorWithAlpha(accentCol, 0.28f);
            cStyle.border = clearColor;
            r.DrawSurface(catRect, cStyle);
        } else if (isFocused) {
            r.DrawRoundedRect(InsetRectF(catRect, -1.0f * dp, -1.0f * dp), 8.0f * dp + 1.0f * dp, th.focusRing, 1.5f * dp);
        } else if (isHover && std::abs(catPos - static_cast<float>(i)) > 0.5f) {
            cStyle.fill = surfaceHover;
            r.DrawSurface(catRect, cStyle);
        }

        const float catWeight = std::clamp(1.0f - std::abs(catPos - static_cast<float>(i)), 0.0f, 1.0f);
        const D2D1_COLOR_F catIconCol = LerpColor(isHover ? textNorm : textMute, accentCol, catWeight);
        const D2D1_COLOR_F catTextCol = LerpColor(isHover ? textPri : textNorm, textPri, catWeight);

        float iconSz = 16.0f * dp;
        D2D1_RECT_F iconRect = D2D1::RectF(catRect.left + 12.0f * dp, catRect.top + (catH - iconSz) * 0.5f, catRect.left + 12.0f * dp + iconSz, catRect.top + (catH + iconSz) * 0.5f);
        r.DrawIcon(c.icon, iconRect, catIconCol, 1.4f * dp);
        D2D1_RECT_F textRect = D2D1::RectF(iconRect.right + 10.0f * dp, catRect.top, catRect.right - 10.0f * dp, catRect.bottom);
        r.DrawString(c.name, textRect, catTextCol, (isFocused || isActive) ? FontType::Medium : FontType::Regular, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        curCatY += catH + SettingsLayout::kSidebarItemGap * dp;
    }
    r.PushClip(contentBounds);
    float padX = SettingsLayout::kContentPadX * dp, contentY = contentBounds.top + SettingsLayout::kContentStartOffset * dp - vm.scrollY;
    float contentW = RectWidth(contentBounds) - padX * 2.0f;
    auto drawPageHeader = [&](const std::wstring& title, const std::wstring& description) {
        const float headerTop = contentY;
        D2D1_RECT_F titleRect = D2D1::RectF(contentBounds.left + padX, headerTop, contentBounds.right - padX, headerTop + 26.0f * dp);
        r.DrawString(title, titleRect, textPri, FontType::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F descriptionRect = D2D1::RectF(contentBounds.left + padX, headerTop + 29.0f * dp,
                                                   contentBounds.right - padX, headerTop + 48.0f * dp);
        r.DrawString(FitSingleLine(r, description, FontType::Small, contentW), descriptionRect,
                     textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        contentY += SettingsLayout::kPageHeaderReserve * dp;
    };
    auto drawHeader = [&](const wchar_t* title) {
        D2D1_RECT_F hRect = D2D1::RectF(contentBounds.left + padX, contentY, contentBounds.right - padX, contentY + 22.0f * dp);
        r.DrawString(title, hRect, textMute, FontType::Caption, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        contentY += 28.0f * dp;
    };
    if (vm.activeCat == SettingsCategory::Experience) {
        drawPageHeader(Loc::Tr("settings.category.experience"), Loc::Tr("settings.experience.description"));
        // The Experience page follows the playback card rhythm: a mode picker
        // card, plus a Custom-features card that only exists in Custom mode.
        const ExperienceMode expMode = FeatureGate::ModeFromString(s.experienceMode);
        const float cardGap = SettingsLayout::kSectionCardGap * dp, cardLeft = contentBounds.left + padX - 10.0f * dp;
        const float cardRight = contentBounds.right - padX + 10.0f * dp, contentLeft = contentBounds.left + padX;
        const float contentRight = contentBounds.right - padX, cardTitleH = SettingsLayout::kExperienceCardTitleHeight * dp;
        auto drawExperienceCard = [&](float top, float height) {
            drawFlatPanel(D2D1::RectF(cardLeft, top, cardRight, top + height), SettingsLayout::kSectionCardRadius, panelFill, panelBorder);
        };
        auto drawExperienceTitle = [&](float top, const std::wstring& title) {
            const D2D1_RECT_F titleRect = D2D1::RectF(contentLeft + 22.0f * dp, top + 10.0f * dp,
                                                       contentRight - 22.0f * dp, top + 34.0f * dp);
            r.DrawString(title, titleRect, textPri, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        };
        auto drawModeChip = [&](const D2D1_RECT_F& rect, const std::wstring& label, bool selected, bool focused, bool pressed) {
            SurfaceStyle style = makeFlatSurface(SettingsLayout::kControlRadius);
            if (pressed) {
                style.fill = ColorWithAlpha(accentCol, 0.34f);
                style.border = accentCol;
                style.borderWidth = 2.0f;
            } else if (selected) {
                style.fill = accentCol;
                style.border = accentCol;
                style.borderWidth = focused ? 2.0f : 1.0f;
            } else if (focused) {
                style.fill = ColorWithAlpha(accentCol, 0.12f);
                style.border = accentCol;
                style.borderWidth = 2.0f;
            } else { style.fill = surfaceCard; style.border = clearColor; style.borderWidth = 0.0f; }
            r.DrawSurface(rect, style);
            r.DrawString(label, rect, (selected && !pressed) ? textOnAccent : textPri,
                         (selected || focused || pressed) ? FontType::Medium : FontType::Regular,
                         DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        };
        // Card 1: Experience Mode (Row 0; chips 0..2)
        const float modeCardH = SettingsLayout::kExperienceModeCardHeight * dp, modeTop = contentY;
        drawExperienceCard(modeTop, modeCardH);
        drawExperienceTitle(modeTop, Loc::Tr("settings.experience.title"));
        const char* modeKeys[] = { "settings.experience.mode.simple", "settings.experience.mode.advanced",
                                   "settings.experience.mode.custom" };
        const char* modeDescKeys[] = { "settings.experience.mode.simple_desc", "settings.experience.mode.advanced_desc",
                                       "settings.experience.mode.custom_desc" };
        const float chipGap = 8.0f * dp;
        const float chipW = (contentW - 2.0f * chipGap) / 3.0f, chipH = SettingsLayout::kControlHeight * dp;
        const float chipTop = modeTop + cardTitleH;
        for (int i = 0; i < 3; ++i) {
            const D2D1_RECT_F chipRect = D2D1::RectF(contentLeft + i * (chipW + chipGap), chipTop,
                                                     contentLeft + i * (chipW + chipGap) + chipW, chipTop + chipH);
            drawModeChip(chipRect, Loc::Tr(modeKeys[i]), expMode == static_cast<ExperienceMode>(i),
                         showContentKbFocus && vm.keyboardFocusedRow == 0 && vm.keyboardFocusedSubIndex == i,
                         vm.pressState.IsOptionPressed(0, i));
        }
        const float descTop = chipTop + chipH + 8.0f * dp;
        r.DrawString(Loc::Tr(modeDescKeys[static_cast<int>(expMode)]),
                     D2D1::RectF(contentLeft, descTop, contentRight, descTop + 18.0f * dp),
                     textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        contentY = modeTop + modeCardH + cardGap;
        // Card 2: Custom Features (Rows 1..12, one per catalog feature)
        if (expMode == ExperienceMode::Custom) {
            const float featuresTop = contentY;
            drawExperienceCard(featuresTop, SettingsLayout::kExperienceFeaturesCardHeight * dp);
            const D2D1_RECT_F titleRect = D2D1::RectF(contentLeft + 22.0f * dp, featuresTop + 10.0f * dp,
                                                      contentRight - 22.0f * dp, featuresTop + 34.0f * dp);
            r.DrawString(Loc::Tr("settings.experience.features_title"), titleRect, textPri, FontType::Medium,
                         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const D2D1_RECT_F hintRect = D2D1::RectF(contentLeft + 22.0f * dp, featuresTop + 10.0f * dp,
                                                     contentRight - 22.0f * dp, featuresTop + 34.0f * dp);
            r.DrawString(Loc::Tr("settings.experience.features_hint"), hintRect, textMute, FontType::Small,
                         DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const float rowH = SettingsLayout::kExperienceFeatureRowHeight * dp;
            const auto& catalog = FeatureGate::Catalog();
            float rowY = featuresTop + cardTitleH;
            const char* lastGroup = nullptr;
            for (int i = 0; i < static_cast<int>(catalog.size()); ++i) {
                const auto& info = catalog[i];
                if (!lastGroup || std::string(lastGroup) != info.group) {
                    lastGroup = info.group;
                    const D2D1_RECT_F groupRect = D2D1::RectF(contentLeft + 22.0f * dp, rowY,
                                                              contentRight - 22.0f * dp, rowY + SettingsLayout::kExperienceGroupHeaderHeight * dp);
                    r.DrawString(Loc::Tr(std::string("settings.experience.group.") + info.group), groupRect, textMute,
                                 FontType::Caption, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    rowY += SettingsLayout::kExperienceGroupHeaderHeight * dp;
                }
                D2D1_RECT_F rRect = D2D1::RectF(contentLeft, rowY, contentRight, rowY + rowH);
                const bool value = FeatureGate::FeatureEnabled(ExperienceMode::Custom, s.customEnabledFeatures, info.id);
                bool isKbFocused = showContentKbFocus && (vm.keyboardFocusedRow == i + 1);
                bool isPressed = vm.pressState.IsOptionPressed(i + 1);
                bool isHover = !isPressed && !isKbFocused && pointerIn(rRect);
                if (isPressed || isKbFocused || isHover) {
                    SurfaceStyle rowFocusStyle = makeFlatSurface();
                    rowFocusStyle.fill = isPressed ? ColorWithAlpha(accentCol, 0.20f) : isKbFocused ? ColorWithAlpha(accentCol, 0.10f)
                                                     : surfaceHover;
                    rowFocusStyle.border = (isPressed || isKbFocused) ? accentCol : clearColor;
                    rowFocusStyle.borderWidth = (isPressed || isKbFocused) ? 1.5f : 0.0f;
                    r.DrawSurface(rRect, rowFocusStyle);
                }
                const D2D1_RECT_F rowTextRect = InsetRectF(rRect, 14.0f * dp, 0.0f);
                r.DrawString(Loc::Tr(info.displayNameKey), rowTextRect, isKbFocused ? textPri : textNorm, FontType::Medium,
                             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                // Compact switch (34px x 18px); eased progress mirrors the playback behavior rows.
                float swW = 34.0f * dp, swH = 18.0f * dp;
                D2D1_RECT_F swRect = D2D1::RectF(rRect.right - swW - 12.0f * dp, rRect.top + (rowH - swH) * 0.5f, rRect.right - 12.0f * dp, rRect.top + (rowH + swH) * 0.5f);
                float t = std::clamp(vm.experienceToggleAnim[i], 0.0f, 1.0f);
                if (t <= 0.0f && value) t = 1.0f; // Secondary render paths receive no eased anim; fall back to the static state.
                D2D1_COLOR_F trackCol = LerpColor(toggleOff, accentCol, t);
                r.FillRoundedRect(swRect, swH * 0.5f, trackCol);
                r.DrawRoundedRect(swRect, swH * 0.5f, LerpColor(surfaceBorder, accentCol, t), 1.0f);
                float knobRadius = 7.0f * dp;
                float knobX = (swRect.left + 2.0f * dp + knobRadius) + t * (swW - 4.0f * dp - 2.0f * knobRadius);
                float knobY = swRect.top + swH * 0.5f;
                D2D1_COLOR_F knobCol = LerpColor(knobOff, surfaceBg, t);
                r.FillCircle(knobX, knobY, knobRadius, knobCol);
                if (i + 1 < static_cast<int>(catalog.size())) {
                    r.DrawLine(rRect.left + 8.0f * dp, rRect.bottom, rRect.right - 8.0f * dp, rRect.bottom, surfaceBorder, 1.0f);
                }
                rowY += rowH;
            }
        }
    } else if (vm.activeCat == SettingsCategory::Appearance) {
        drawPageHeader(Loc::Tr("settings.category.appearance"), Loc::Tr("settings.page.appearance"));
        // Section 1: Themes (Row 0)
        drawHeader(Loc::Tr("settings.appearance.theme_palette").c_str());
        const auto& allThemes = s.GetAllThemes();
        float cardW = (contentW - 12.0f * dp) * 0.5f;
        float cardH = 46.0f * dp;
        const size_t themeRows = (allThemes.size() + 1) / 2;
        const float themeGridH = themeRows > 0 ? themeRows * (cardH + 8.0f * dp) - 8.0f * dp : 0.0f;
        drawFlatPanel(D2D1::RectF(contentBounds.left + padX - 10.0f * dp, contentY - 10.0f * dp,
                                  contentBounds.right - padX + 10.0f * dp, contentY + themeGridH + 10.0f * dp),
                      12.0f, panelFill, panelBorder);
        for (size_t i = 0; i < allThemes.size(); ++i) {
            float col = (float)(i % 2);
            float row = (float)(i / 2);
            float tx = contentBounds.left + padX + col * (cardW + 12.0f * dp);
            float ty = contentY + row * (cardH + 8.0f * dp);
            D2D1_RECT_F tRect = D2D1::RectF(tx, ty, tx + cardW, ty + cardH);
            bool isSelected = (s.themePreset == allThemes[i].id);
            bool isKbFocused = showContentKbFocus && (vm.keyboardFocusedRow == 0) && (vm.keyboardFocusedSubIndex == (int)i);
            bool isPressed = vm.pressState.IsOptionPressed(0, (int)i);
            bool isHover = !isSelected && !isKbFocused && !isPressed && pointerIn(tRect);
            SurfaceStyle tStyle = makeFlatSurface();
            if (isPressed) {
                tStyle.fill = ColorWithAlpha(accentCol, 0.28f);
                tStyle.border = accentCol;
                tStyle.borderWidth = 2.0f;
            } else if (isKbFocused) {
                tStyle.fill = ColorWithAlpha(accentCol, 0.12f);
                tStyle.border = accentCol;
                tStyle.borderWidth = 2.0f;
            } else if (isSelected) {
                tStyle.fill = ColorWithAlpha(accentCol, 0.08f);
                tStyle.border = accentCol;
                tStyle.borderWidth = 2.0f;
            } else if (isHover) {
                tStyle.fill = surfaceHover;
                tStyle.border = surfaceBorder;
                tStyle.borderWidth = 1.0f;
            } else { tStyle.fill = canvasBg; tStyle.border = clearColor; tStyle.borderWidth = 0.0f; }
            r.DrawSurface(tRect, tStyle);
            r.FillCircle(tRect.left + 18.0f * dp, tRect.top + cardH * 0.5f, 6.0f * dp, allThemes[i].accent);
            r.FillCircle(tRect.left + 34.0f * dp, tRect.top + cardH * 0.5f, 6.0f * dp, allThemes[i].cardBg);
            r.DrawCircle(tRect.left + 34.0f * dp, tRect.top + cardH * 0.5f, 6.0f * dp, surfaceBorder, 1.0f);
            D2D1_RECT_F nameRect = D2D1::RectF(tRect.left + 48.0f * dp, tRect.top, tRect.right - 10.0f * dp, tRect.bottom);
            r.DrawString(Utf8ToUtf16(allThemes[i].displayName), nameRect, (isSelected || isKbFocused) ? textPri : textNorm, (isSelected || isKbFocused) ? FontType::Medium : FontType::Regular, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        contentY += (themeRows * (cardH + 8.0f * dp)) + 16.0f * dp;
        // Section 2: Display Language (Row 1)
        drawHeader(Loc::Tr("settings.appearance.display_language").c_str());
        auto languages = s.GetAvailableLanguages();
        float langW = (contentW - 16.0f * dp) / 3.0f;
        float langH = 34.0f * dp;
        const size_t languageRows = (languages.size() + 2) / 3;
        const float languageGridH = languageRows > 0 ? languageRows * (langH + 8.0f * dp) - 8.0f * dp : 0.0f;
        drawFlatPanel(D2D1::RectF(contentBounds.left + padX - 10.0f * dp, contentY - 8.0f * dp,
                                  contentBounds.right - padX + 10.0f * dp, contentY + languageGridH + 8.0f * dp),
                      12.0f, panelFill, panelBorder);
        for (size_t i = 0; i < languages.size(); ++i) {
            float col = (float)(i % 3);
            float row = (float)(i / 3);
            float lx = contentBounds.left + padX + col * (langW + 8.0f * dp);
            float ly = contentY + row * (langH + 8.0f * dp);
            D2D1_RECT_F lRect = D2D1::RectF(lx, ly, lx + langW, ly + langH);
            bool isSelected = (s.appLanguage == languages[i].tag);
            bool isKbFocused = showContentKbFocus && (vm.keyboardFocusedRow == 1) && (vm.keyboardFocusedSubIndex == (int)i);
            bool isPressed = vm.pressState.IsOptionPressed(1, (int)i);
            bool isHover = !isSelected && !isKbFocused && !isPressed && pointerIn(lRect);
            SurfaceStyle lStyle = makeFlatSurface();
            if (isPressed) {
                lStyle.fill = ColorWithAlpha(accentCol, 0.34f);
                lStyle.border = accentCol;
                lStyle.borderWidth = 2.0f;
            } else if (isKbFocused) {
                lStyle.fill = ColorWithAlpha(accentCol, 0.12f);
                lStyle.border = accentCol;
                lStyle.borderWidth = 2.0f;
            } else if (isSelected) { lStyle.fill = accentCol; lStyle.border = accentCol; lStyle.borderWidth = 1.0f; } else if (isHover) {
                lStyle.fill = surfaceHover;
                lStyle.border = surfaceBorder;
                lStyle.borderWidth = 1.0f;
            } else { lStyle.fill = surfaceCard; lStyle.border = clearColor; lStyle.borderWidth = 0.0f; }
            r.DrawSurface(lRect, lStyle);
            r.DrawString(Utf8ToUtf16(languages[i].displayName), lRect, isSelected ? textOnAccent : textNorm, (isSelected || isKbFocused) ? FontType::Medium : FontType::Regular, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        contentY += (languageRows * (langH + 8.0f * dp)) + 16.0f * dp;
    } else if (vm.activeCat == SettingsCategory::Playback) {
        drawPageHeader(Loc::Tr("settings.category.playback"), Loc::Tr("settings.page.playback"));
        // Playback follows the same card-first rhythm as Audio & Subtitles: section labels live inside their cards, while controls share
        // the card surface and only selected/focused controls receive emphasis.
        const float cardGap = SettingsLayout::kSectionCardGap * dp, cardLeft = contentBounds.left + padX - 10.0f * dp;
        const float cardRight = contentBounds.right - padX + 10.0f * dp, contentLeft = contentBounds.left + padX;
        const float contentRight = contentBounds.right - padX, cardTitleH = SettingsLayout::kPlaybackCardTitleHeight * dp;
        auto drawPlaybackCard = [&](float top, float height) {
            drawFlatPanel(D2D1::RectF(cardLeft, top, cardRight, top + height), SettingsLayout::kSectionCardRadius, panelFill, panelBorder);
        };
        auto drawPlaybackTitle = [&](float top, const std::wstring& title, bool eyebrow) {
            const D2D1_RECT_F titleRect = D2D1::RectF(contentLeft + 22.0f * dp, top + 10.0f * dp,
                                                       contentRight - 22.0f * dp, top + 34.0f * dp);
            r.DrawString(title, titleRect, eyebrow ? textMute : textPri, eyebrow ? FontType::Caption : FontType::Medium,
                         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        };
        // Section 1: Behavior Switches (Rows 0..3)
        const float rowH = SettingsLayout::kPlaybackBehaviorRowHeight * dp, behaviorTop = contentY;
        const float behaviorH = SettingsLayout::kPlaybackBehaviorCardHeight * dp;
        drawPlaybackCard(behaviorTop, behaviorH);
        drawPlaybackTitle(behaviorTop, Loc::Tr("settings.playback.behavior"), true);
        contentY = behaviorTop + cardTitleH;
        struct SwitchRow { std::wstring label; std::wstring desc; bool value; float anim; };
        SwitchRow rows[] = {
            { Loc::Tr("settings.playback.autoplay"), Loc::Tr("ux.autoplay_description"), s.autoplay, vm.autoplayProg },
            { Loc::Tr("settings.playback.remember_pos"), L"", s.rememberPos, vm.rememberPosProg },
            { Loc::Tr("settings.playback.hw_accel"), Loc::Tr("settings.playback.hw_accel_desc"), s.hwAccel, vm.hwAccelProg },
            {Loc::Tr("settings.playback.loop"), Loc::Tr("settings.playback.loop_desc"), s.loop, vm.loopProg}};
        for (int i = 0; i < 4; ++i) {
            D2D1_RECT_F rRect = D2D1::RectF(contentLeft, contentY, contentRight, contentY + rowH);
            bool isKbFocused = showContentKbFocus && (vm.keyboardFocusedRow == i);
            bool isPressed = vm.pressState.IsOptionPressed(i);
            bool isHover = !isPressed && !isKbFocused && pointerIn(rRect);
            if (isPressed || isKbFocused || isHover) {
                SurfaceStyle rowFocusStyle = makeFlatSurface();
                rowFocusStyle.fill = isPressed ? ColorWithAlpha(accentCol, 0.20f) : isKbFocused ? ColorWithAlpha(accentCol, 0.10f)
                                                 : surfaceHover;
                rowFocusStyle.border = (isPressed || isKbFocused) ? accentCol : clearColor;
                rowFocusStyle.borderWidth = (isPressed || isKbFocused) ? 1.5f : 0.0f;
                r.DrawSurface(rRect, rowFocusStyle);
            }
            const D2D1_RECT_F rowTextRect = InsetRectF(rRect, 14.0f * dp, 0.0f);
            if (rows[i].desc.empty()) {
                r.DrawString(rows[i].label, rowTextRect, isKbFocused ? textPri : textNorm, FontType::Medium,
                             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            } else {
                const D2D1_RECT_F rowTitleRect = D2D1::RectF(rowTextRect.left, rRect.top + 4.0f * dp,
                                                             rowTextRect.right, rRect.top + 25.0f * dp);
                const D2D1_RECT_F rowDescRect = D2D1::RectF(rowTextRect.left, rRect.top + 24.0f * dp,
                                                            rowTextRect.right, rRect.bottom - 4.0f * dp);
                r.DrawString(rows[i].label, rowTitleRect, isKbFocused ? textPri : textNorm, FontType::Medium,
                             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                r.DrawString(rows[i].desc, rowDescRect, textMute, FontType::Small,
                             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            // Compact Catppuccin Macchiato switch (34px x 18px)
            float swW = 34.0f * dp, swH = 18.0f * dp;
            D2D1_RECT_F swRect = D2D1::RectF(rRect.right - swW - 12.0f * dp, rRect.top + (rowH - swH) * 0.5f, rRect.right - 12.0f * dp, rRect.top + (rowH + swH) * 0.5f);
            float t = std::clamp(rows[i].anim, 0.0f, 1.0f);
            D2D1_COLOR_F trackCol = LerpColor(toggleOff, accentCol, t);
            r.FillRoundedRect(swRect, swH * 0.5f, trackCol);
            r.DrawRoundedRect(swRect, swH * 0.5f, LerpColor(surfaceBorder, accentCol, t), 1.0f);
            float knobRadius = 7.0f * dp;
            float knobX = (swRect.left + 2.0f * dp + knobRadius) + t * (swW - 4.0f * dp - 2.0f * knobRadius);
            float knobY = swRect.top + swH * 0.5f;
            D2D1_COLOR_F knobCol = LerpColor(knobOff, surfaceBg, t);
            r.FillCircle(knobX, knobY, knobRadius, knobCol);
            if (i < 3) { r.DrawLine(rRect.left + 8.0f * dp, rRect.bottom, rRect.right - 8.0f * dp, rRect.bottom, surfaceBorder, 1.0f); }
            contentY += rowH;
        }
        contentY = behaviorTop + behaviorH + cardGap;
        // Section 2: Arrow Key Seek Step (Row 4)
        const float simpleCardH = SettingsLayout::kPlaybackSimpleCardHeight * dp, seekCardTop = contentY;
        drawPlaybackCard(seekCardTop, simpleCardH);
        D2D1_RECT_F seekRowRect = D2D1::RectF(contentLeft, seekCardTop + 8.0f * dp, contentRight, seekCardTop + simpleCardH - 8.0f * dp);
        bool isSeekRowKbFocused = showContentKbFocus && (vm.keyboardFocusedRow == 4);
        D2D1_RECT_F seekLabelRect = D2D1::RectF(seekRowRect.left + 14.0f * dp, seekRowRect.top + 6.0f * dp, seekRowRect.right - 230.0f * dp, seekRowRect.top + 26.0f * dp);
        r.DrawString(Loc::Tr("settings.playback.seek_step_title"), seekLabelRect, textPri, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F seekSubRect = D2D1::RectF(seekRowRect.left + 14.0f * dp, seekRowRect.top + 26.0f * dp, seekRowRect.right - 230.0f * dp, seekRowRect.bottom - 6.0f * dp);
        r.DrawString(Loc::Tr("settings.playback.seek_step_desc"), seekSubRect, textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Dropdown picker button
        D2D1_RECT_F dropRect = D2D1::RectF(seekRowRect.right - 220.0f * dp, seekRowRect.top + 8.0f * dp, seekRowRect.right - 100.0f * dp, seekRowRect.bottom - 8.0f * dp);
        bool isDropKb = isSeekRowKbFocused && (vm.keyboardFocusedSubIndex == 0);
        bool isDropPressed = vm.pressState.IsOptionPressed(4, 0);
        bool isDropHover = !isDropPressed && !isDropKb && pointerIn(dropRect);
        SurfaceStyle dropStyle = makeFlatSurface();
        dropStyle.fill = isDropPressed ? ColorWithAlpha(accentCol, 0.32f) : (isDropKb ? surfaceSelected : (isDropHover ? surfaceHover : surfaceBg));
        dropStyle.border = (isDropPressed || isDropKb) ? accentCol : (isDropHover ? ColorWithAlpha(surfaceBorder, 1.0f) : surfaceBorder);
        dropStyle.borderWidth = (isDropPressed || isDropKb) ? 1.5f : 1.0f;
        r.DrawSurface(dropRect, dropStyle);
        wchar_t seekValStr[32];
        if (s.arrowSeekStepSec == (int)s.arrowSeekStepSec) swprintf_s(seekValStr, L"%d sec  v", (int)s.arrowSeekStepSec);
        else swprintf_s(seekValStr, L"%.1f sec  v", s.arrowSeekStepSec);
        r.DrawString(seekValStr, dropRect, isDropKb ? textPri : textNorm, FontType::Medium, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Custom manual input button
        D2D1_RECT_F custBtnRect = D2D1::RectF(seekRowRect.right - 92.0f * dp, seekRowRect.top + 8.0f * dp, seekRowRect.right - 10.0f * dp, seekRowRect.bottom - 8.0f * dp);
        bool isCustKb = isSeekRowKbFocused && (vm.keyboardFocusedSubIndex == 1);
        bool isCustPressed = vm.pressState.IsOptionPressed(4, 1);
        bool isCustHover = !isCustPressed && !isCustKb && pointerIn(custBtnRect);
        SurfaceStyle custStyle = makeFlatSurface();
        custStyle.fill = isCustPressed ? ColorWithAlpha(accentCol, 0.32f) : (isCustKb ? surfaceSelected : (isCustHover ? surfaceHover : surfaceBg));
        custStyle.border = (isCustPressed || isCustKb) ? accentCol : surfaceBorder;
        custStyle.borderWidth = (isCustPressed || isCustKb) ? 1.5f : 1.0f;
        r.DrawSurface(custBtnRect, custStyle);
        r.DrawString(Loc::Tr("settings.playback.custom"), custBtnRect, isCustKb ? accentCol : textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        contentY = seekCardTop + simpleCardH + cardGap;
        // Section 3: Frame Seek Step (Row 5)
        const float frameCardTop = contentY;
        drawPlaybackCard(frameCardTop, simpleCardH);
        D2D1_RECT_F frameRowRect = D2D1::RectF(contentLeft, frameCardTop + 8.0f * dp, contentRight, frameCardTop + simpleCardH - 8.0f * dp);
        bool isFrameRowKbFocused = showContentKbFocus && (vm.keyboardFocusedRow == 5);
        D2D1_RECT_F frameLabelRect = D2D1::RectF(frameRowRect.left + 14.0f * dp, frameRowRect.top + 6.0f * dp, frameRowRect.right - 230.0f * dp, frameRowRect.top + 26.0f * dp);
        r.DrawString(Loc::Tr("settings.playback.frame_step_title"), frameLabelRect, textPri, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F frameSubRect = D2D1::RectF(frameRowRect.left + 14.0f * dp, frameRowRect.top + 26.0f * dp, frameRowRect.right - 230.0f * dp, frameRowRect.bottom - 6.0f * dp);
        r.DrawString(Loc::Tr("settings.playback.frame_step_desc"), frameSubRect, textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Frame Dropdown picker button
        D2D1_RECT_F fDropRect = D2D1::RectF(frameRowRect.right - 220.0f * dp, frameRowRect.top + 8.0f * dp, frameRowRect.right - 100.0f * dp, frameRowRect.bottom - 8.0f * dp);
        bool isFDropKb = isFrameRowKbFocused && (vm.keyboardFocusedSubIndex == 0);
        bool isFDropPressed = vm.pressState.IsOptionPressed(5, 0);
        bool isFDropHover = !isFDropPressed && !isFDropKb && pointerIn(fDropRect);
        SurfaceStyle fDropStyle = makeFlatSurface();
        fDropStyle.fill = isFDropPressed ? ColorWithAlpha(accentCol, 0.32f) : (isFDropKb ? surfaceSelected : (isFDropHover ? surfaceHover : surfaceBg));
        fDropStyle.border = (isFDropPressed || isFDropKb) ? accentCol : surfaceBorder;
        fDropStyle.borderWidth = (isFDropPressed || isFDropKb) ? 1.5f : 1.0f;
        r.DrawSurface(fDropRect, fDropStyle);
        wchar_t frameValStr[32];
        if (s.ctrlSeekFrames == 1) wcscpy_s(frameValStr, L"1 Frame  v");
        else swprintf_s(frameValStr, L"%d Frames  v", s.ctrlSeekFrames);
        r.DrawString(frameValStr, fDropRect, isFDropKb ? textPri : textNorm, FontType::Medium, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Frame Custom manual input button
        D2D1_RECT_F fCustBtnRect = D2D1::RectF(frameRowRect.right - 92.0f * dp, frameRowRect.top + 8.0f * dp, frameRowRect.right - 10.0f * dp, frameRowRect.bottom - 8.0f * dp);
        bool isFCustKb = isFrameRowKbFocused && (vm.keyboardFocusedSubIndex == 1);
        bool isFCustPressed = vm.pressState.IsOptionPressed(5, 1);
        bool isFCustHover = !isFCustPressed && !isFCustKb && pointerIn(fCustBtnRect);
        SurfaceStyle fCustStyle = makeFlatSurface();
        fCustStyle.fill = isFCustPressed ? ColorWithAlpha(accentCol, 0.32f) : (isFCustKb ? surfaceSelected : (isFCustHover ? surfaceHover : surfaceBg));
        fCustStyle.border = (isFCustPressed || isFCustKb) ? accentCol : surfaceBorder;
        fCustStyle.borderWidth = (isFCustPressed || isFCustKb) ? 1.5f : 1.0f;
        r.DrawSurface(fCustBtnRect, fCustStyle);
        r.DrawString(Loc::Tr("settings.playback.custom"), fCustBtnRect, isFCustKb ? accentCol : textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        contentY = frameCardTop + simpleCardH + cardGap;
        // Section 4: Default Playback Speed (Row 6)
        double speeds[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };
        const wchar_t* speedLabels[] = { L"0.5x", L"0.75x", L"1.0x", L"1.25x", L"1.5x", L"2.0x" };
        float spdW = (contentW - 25.0f * dp) / 6.0f;
        float pH = 34.0f * dp;
        const float speedCardTop = contentY, speedCardH = SettingsLayout::kPlaybackSpeedCardHeight * dp;
        drawPlaybackCard(speedCardTop, speedCardH);
        drawPlaybackTitle(speedCardTop, Loc::Tr("settings.playback.default_speed"), false);
        contentY = speedCardTop + 42.0f * dp;
        for (int i = 0; i < 6; ++i) {
            D2D1_RECT_F pRect = D2D1::RectF(contentLeft + i * (spdW + 5.0f * dp), contentY,
                                            contentLeft + i * (spdW + 5.0f * dp) + spdW, contentY + pH);
            bool isSel = (std::abs(s.defaultSpeed - speeds[i]) < 0.05);
            bool isKbFocused = showContentKbFocus && (vm.keyboardFocusedRow == 6) && (vm.keyboardFocusedSubIndex == i);
            bool isPressed = vm.pressState.IsOptionPressed(6, i);
            bool isHover = !isSel && !isKbFocused && !isPressed && pointerIn(pRect);
            SurfaceStyle pStyle = makeFlatSurface();
            if (isPressed) {
                pStyle.fill = ColorWithAlpha(accentCol, 0.32f);
                pStyle.border = accentCol;
                pStyle.borderWidth = 2.0f;
            } else if (isKbFocused) {
                pStyle.fill = ColorWithAlpha(accentCol, 0.12f);
                pStyle.border = accentCol;
                pStyle.borderWidth = 2.0f;
            } else if (isSel) { pStyle.fill = accentCol; pStyle.border = accentCol; pStyle.borderWidth = 1.0f; } else if (isHover) {
                pStyle.fill = surfaceHover;
                pStyle.border = surfaceBorder;
                pStyle.borderWidth = 1.0f;
            } else { pStyle.fill = surfaceCard; pStyle.border = clearColor; pStyle.borderWidth = 0.0f; }
            r.DrawSurface(pRect, pStyle);
            r.DrawString(speedLabels[i], pRect, isSel ? textOnAccent : textNorm, (isSel || isKbFocused) ? FontType::Medium : FontType::Regular, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // Section 5: Custom MPV Configuration (Rows 7 & 8)
        const float mpvCardTop = speedCardTop + speedCardH + cardGap, mpvCardH = SettingsLayout::kPlaybackMpvCardHeight * dp;
        drawPlaybackCard(mpvCardTop, mpvCardH);
        drawPlaybackTitle(mpvCardTop, Loc::Tr("settings.playback.mpv_conf_header"), true);
        contentY = mpvCardTop + cardTitleH;
        // Row 7: Enable Custom MPV Config Switch
        D2D1_RECT_F mpvSwitchRect = D2D1::RectF(contentLeft, contentY, contentRight, contentY + SettingsLayout::kPlaybackMpvSwitchHeight * dp);
        bool isMpvKb = showContentKbFocus && (vm.keyboardFocusedRow == 7);
        bool isMpvPressed = vm.pressState.IsOptionPressed(7);
        bool isMpvHover = !isMpvPressed && !isMpvKb && pointerIn(mpvSwitchRect);
        SurfaceStyle mpvSwitchStyle = makeFlatSurface();
        mpvSwitchStyle.fill = isMpvPressed ? ColorWithAlpha(accentCol, 0.20f) : isMpvKb ? surfaceSelected
                            : isMpvHover ? surfaceHover : clearColor;
        mpvSwitchStyle.border = (isMpvPressed || isMpvKb) ? accentCol : clearColor;
        mpvSwitchStyle.borderWidth = (isMpvPressed || isMpvKb) ? 1.5f : 0.0f;
        r.DrawSurface(mpvSwitchRect, mpvSwitchStyle);
        D2D1_RECT_F mpvLabelRect = D2D1::RectF(mpvSwitchRect.left + 14.0f * dp, mpvSwitchRect.top + 6.0f * dp, mpvSwitchRect.right - 80.0f * dp, mpvSwitchRect.top + 26.0f * dp);
        r.DrawString(Loc::Tr("settings.playback.mpv_conf_enable"), mpvLabelRect, isMpvKb ? textPri : textNorm, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F mpvSubRect = D2D1::RectF(mpvSwitchRect.left + 14.0f * dp, mpvSwitchRect.top + 26.0f * dp, mpvSwitchRect.right - 80.0f * dp, mpvSwitchRect.bottom - 6.0f * dp);
        r.DrawString(Loc::Tr("settings.playback.mpv_conf_desc"), mpvSubRect, textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Switch on right
        float mpvSwW = 34.0f * dp, mpvSwH = 18.0f * dp;
        D2D1_RECT_F mpvSwRect = D2D1::RectF(mpvSwitchRect.right - mpvSwW - 14.0f * dp, mpvSwitchRect.top + (50.0f * dp - mpvSwH) * 0.5f, mpvSwitchRect.right - 14.0f * dp, mpvSwitchRect.top + (50.0f * dp + mpvSwH) * 0.5f);
        float mpvT = std::clamp(vm.customMpvConfigProg, 0.0f, 1.0f);
        D2D1_COLOR_F mpvTrackCol = LerpColor(toggleOff, accentCol, mpvT);
        r.FillRoundedRect(mpvSwRect, mpvSwH * 0.5f, mpvTrackCol);
        r.DrawRoundedRect(mpvSwRect, mpvSwH * 0.5f, LerpColor(surfaceBorder, accentCol, mpvT), 1.0f);
        float mpvKnobRadius = 7.0f * dp;
        float mpvKnobX = (mpvSwRect.left + 2.0f * dp + mpvKnobRadius) + mpvT * (mpvSwW - 4.0f * dp - 2.0f * mpvKnobRadius);
        float mpvKnobY = mpvSwRect.top + mpvSwH * 0.5f;
        D2D1_COLOR_F mpvKnobCol = LerpColor(knobOff, surfaceBg, mpvT);
        r.FillCircle(mpvKnobX, mpvKnobY, mpvKnobRadius, mpvKnobCol);
        r.DrawLine(mpvSwitchRect.left + 8.0f * dp, mpvSwitchRect.bottom + 5.0f * dp,
                   mpvSwitchRect.right - 8.0f * dp, mpvSwitchRect.bottom + 5.0f * dp, panelBorder, 1.0f * dp);
        contentY = mpvCardTop + SettingsLayout::kPlaybackMpvActionTop * dp;
        // Row 8: Actions (Edit, Reset, Revert)
        struct MpvActionBtn { std::wstring title; std::wstring desc; std::wstring tag; bool isDanger; };
        MpvActionBtn mpvBtns[] = {
            { Loc::Tr("settings.playback.mpv_edit_title"), Loc::Tr("settings.playback.mpv_edit_desc"), Loc::Tr("settings.playback.mpv_edit_btn"), false },
            { Loc::Tr("settings.playback.mpv_reset_title"), Loc::Tr("settings.playback.mpv_reset_desc"), Loc::Tr("settings.playback.mpv_reset_btn"), true },
            { Loc::Tr("settings.playback.mpv_revert_title"), Loc::Tr("settings.playback.mpv_revert_desc"), Loc::Tr("settings.playback.mpv_revert_btn"), false }
        };
        const float mpvActCardH = SettingsLayout::kPlaybackMpvActionHeight * dp, mpvActGap = SettingsLayout::kPlaybackMpvActionGap * dp;
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F actRowRect = D2D1::RectF(contentLeft, contentY + i * (mpvActCardH + mpvActGap),
                                                contentRight, contentY + i * (mpvActCardH + mpvActGap) + mpvActCardH);
            bool isActKb = showContentKbFocus && (vm.keyboardFocusedRow == 8) && (vm.keyboardFocusedSubIndex == i);
            bool isActPressed = vm.pressState.IsOptionPressed(8, i);
            bool isActHover = !isActPressed && !isActKb && pointerIn(actRowRect);
            SurfaceStyle actStyle = makeFlatSurface();
            if (isActPressed) {
                actStyle.fill = mpvBtns[i].isDanger ? ColorWithAlpha(dangerCol, 0.28f) : ColorWithAlpha(accentCol, 0.22f);
                actStyle.border = mpvBtns[i].isDanger ? dangerCol : accentCol;
                actStyle.borderWidth = 1.5f;
            } else if (isActKb) {
                actStyle.fill = mpvBtns[i].isDanger ? ColorWithAlpha(dangerCol, 0.16f) : surfaceSelected;
                actStyle.border = mpvBtns[i].isDanger ? dangerCol : accentCol;
                actStyle.borderWidth = 2.0f;
            } else if (isActHover) {
                actStyle.fill = surfaceHover;
                actStyle.border = clearColor;
                actStyle.borderWidth = 0.0f;
            } else { actStyle.fill = clearColor; actStyle.border = clearColor; actStyle.borderWidth = 0.0f; }
            r.DrawSurface(actRowRect, actStyle);
            if (i < 2) {
                r.DrawLine(actRowRect.left + 8.0f * dp, actRowRect.bottom + 4.0f * dp,
                           actRowRect.right - 8.0f * dp, actRowRect.bottom + 4.0f * dp, panelBorder, 1.0f * dp);
            }
            D2D1_RECT_F actTitleRect = D2D1::RectF(actRowRect.left + 14.0f * dp, actRowRect.top + 6.0f * dp, actRowRect.right - 120.0f * dp, actRowRect.top + 24.0f * dp);
            D2D1_RECT_F actDescRect = D2D1::RectF(actRowRect.left + 14.0f * dp, actRowRect.top + 24.0f * dp, actRowRect.right - 120.0f * dp, actRowRect.bottom - 4.0f * dp);
            r.DrawString(mpvBtns[i].title, actTitleRect, mpvBtns[i].isDanger ? dangerCol : (isActKb ? textPri : textNorm), FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            r.DrawString(mpvBtns[i].desc, actDescRect, textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            // Badge button
            float badgeW = 96.0f * dp, badgeH = 26.0f * dp;
            D2D1_RECT_F badgeRect = D2D1::RectF(actRowRect.right - badgeW - 12.0f * dp, actRowRect.top + (mpvActCardH - badgeH) * 0.5f, actRowRect.right - 12.0f * dp, actRowRect.top + (mpvActCardH + badgeH) * 0.5f);
            D2D1_COLOR_F bBg = isActPressed ? (mpvBtns[i].isDanger ? dangerCol : accentCol) : (isActKb ? (mpvBtns[i].isDanger ? ColorWithAlpha(dangerCol, 0.25f) : accentCol) : surfaceBg);
            D2D1_COLOR_F bBorder = (isActPressed || isActKb) ? (mpvBtns[i].isDanger ? dangerCol : accentCol) : surfaceBorder;
            D2D1_COLOR_F bText = (isActPressed) ? (mpvBtns[i].isDanger ? D2D1::ColorF(1.0f, 1.0f, 1.0f) : surfaceBg) : (isActKb ? (mpvBtns[i].isDanger ? dangerCol : surfaceBg) : (mpvBtns[i].isDanger ? dangerCol : textNorm));
            r.FillRoundedRect(badgeRect, 6.0f * dp, bBg);
            r.DrawRoundedRect(badgeRect, 6.0f * dp, bBorder, 1.0f);
            r.DrawString(mpvBtns[i].tag, badgeRect, bText, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // Section 6: System Integration (Rows 9, 10 & 11): Global Media Hotkeys, Show Tray Icon, Minimize to Tray.
        contentY = mpvCardTop + SettingsLayout::kPlaybackMpvCardHeight * dp + cardGap;
        auto drawSimpleToggleCard = [&](int row, float cardTop, const std::wstring& label, const std::wstring& desc, float t) {
            drawPlaybackCard(cardTop, simpleCardH);
            const D2D1_RECT_F rowRect = D2D1::RectF(contentLeft, cardTop + 8.0f * dp, contentRight, cardTop + simpleCardH - 8.0f * dp);
            const bool isKb = showContentKbFocus && (vm.keyboardFocusedRow == row);
            const bool isPressed = vm.pressState.IsOptionPressed(row);
            const bool isHover = !isPressed && !isKb && pointerIn(rowRect);
            if (isPressed || isKb || isHover) {
                SurfaceStyle rowFocusStyle = makeFlatSurface();
                rowFocusStyle.fill = isPressed ? ColorWithAlpha(accentCol, 0.20f) : isKb ? ColorWithAlpha(accentCol, 0.10f)
                                 : surfaceHover;
                rowFocusStyle.border = (isPressed || isKb) ? accentCol : clearColor;
                rowFocusStyle.borderWidth = (isPressed || isKb) ? 1.5f : 0.0f;
                r.DrawSurface(rowRect, rowFocusStyle);
            }
            const D2D1_RECT_F labelRect = D2D1::RectF(rowRect.left + 14.0f * dp, rowRect.top + 5.0f * dp, rowRect.right - 80.0f * dp, rowRect.top + 25.0f * dp);
            const D2D1_RECT_F descRect = D2D1::RectF(rowRect.left + 14.0f * dp, rowRect.top + 25.0f * dp, rowRect.right - 80.0f * dp, rowRect.bottom - 5.0f * dp);
            r.DrawString(label, labelRect, isKb ? textPri : textNorm, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            r.DrawString(desc, descRect, textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            // Same compact switch as the behavior rows (34px x 18px).
            float swW = 34.0f * dp, swH = 18.0f * dp;
            D2D1_RECT_F swRect = D2D1::RectF(rowRect.right - swW - 14.0f * dp, rowRect.top + (rowRect.bottom - rowRect.top - swH) * 0.5f,
                                             rowRect.right - 14.0f * dp, rowRect.top + (rowRect.bottom - rowRect.top + swH) * 0.5f);
            const float prog = std::clamp(t, 0.0f, 1.0f);
            r.FillRoundedRect(swRect, swH * 0.5f, LerpColor(toggleOff, accentCol, prog));
            r.DrawRoundedRect(swRect, swH * 0.5f, LerpColor(surfaceBorder, accentCol, prog), 1.0f);
            float knobRadius = 7.0f * dp;
            float knobX = (swRect.left + 2.0f * dp + knobRadius) + prog * (swW - 4.0f * dp - 2.0f * knobRadius);
            r.FillCircle(knobX, swRect.top + swH * 0.5f, knobRadius, LerpColor(knobOff, surfaceBg, prog));
        };
        drawSimpleToggleCard(9, contentY, L"Global Media Hotkeys", Loc::Tr("settings.playback.global_hotkeys_desc"), vm.globalHotkeysProg);
        contentY += simpleCardH + cardGap;
        drawSimpleToggleCard(10, contentY, L"Show Tray Icon", L"Show the Aperture icon in the system notification area", vm.trayIconProg);
        contentY += simpleCardH + cardGap;
        drawSimpleToggleCard(11, contentY, L"Minimize to Tray",
            s.trayIconEnabled ? L"Hide to the notification area instead of the taskbar when minimized" : L"Requires Show Tray Icon",
            vm.minimizeToTrayProg);
    } else if (vm.activeCat == SettingsCategory::Subtitles) {
        // The subtitle page uses its vertical budget for an always-visible
        // preview, so keep the title but omit the secondary page description.
        const float subtitleHeaderTop = contentY;
        r.DrawString(Loc::Tr("settings.category.subtitles"), D2D1::RectF(contentBounds.left + padX, subtitleHeaderTop,
                                 contentBounds.right - padX, subtitleHeaderTop + 26.0f * dp),
                     textPri, FontType::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        contentY += SettingsLayout::kSubtitlePageHeaderReserve * dp;
        const float usableW = contentW, subLeft = contentBounds.left + padX, cardLeft = subLeft - 10.0f * dp;
        const float cardRight = subLeft + usableW + 10.0f * dp, cardGap = SettingsLayout::kSubtitleCardGap * dp, controlGap = 8.0f * dp;
        const float controlH = SettingsLayout::kControlHeight * dp, cardPadX = 22.0f * dp;
        auto drawSubtitleCard = [&](float top, float height, const std::wstring& title, const std::wstring& hint = L"") {
            const D2D1_RECT_F card = D2D1::RectF(cardLeft, top, cardRight, top + height);
            drawFlatPanel(card, SettingsLayout::kSectionCardRadius, panelFill, panelBorder);
            const D2D1_RECT_F titleRect = D2D1::RectF(card.left + cardPadX, top + 10.0f * dp,
                                                       card.right - cardPadX - (hint.empty() ? 0.0f : 70.0f * dp), top + 34.0f * dp);
            r.DrawString(titleRect.right > titleRect.left ? title : L"", titleRect, textPri, FontType::Medium,
                         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (!hint.empty()) {
                const D2D1_RECT_F hintRect = D2D1::RectF(card.right - cardPadX - 64.0f * dp, top + 10.0f * dp,
                                                          card.right - cardPadX, top + 34.0f * dp);
                r.DrawString(hint, hintRect, textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            return card;
        };
        auto drawChoice = [&](const D2D1_RECT_F& rect, const std::wstring& label, bool selected, bool focused, bool pressed) {
            SurfaceStyle style = makeFlatSurface(SettingsLayout::kControlRadius);
            if (pressed) {
                style.fill = ColorWithAlpha(accentCol, 0.34f);
                style.border = accentCol;
                style.borderWidth = 2.0f;
            } else if (selected) {
                style.fill = accentCol;
                style.border = accentCol;
                style.borderWidth = focused ? 2.0f : 1.0f;
            } else if (focused) {
                style.fill = ColorWithAlpha(accentCol, 0.12f);
                style.border = accentCol;
                style.borderWidth = 2.0f;
            } else { style.fill = surfaceCard; style.border = clearColor; style.borderWidth = 0.0f; }
            r.DrawSurface(rect, style);
            r.DrawString(label, rect, (selected && !pressed) ? textOnAccent : textPri,
                         (selected || focused || pressed) ? FontType::Medium : FontType::Regular,
                         DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        };
        const char* audioLangCodes[] = { "eng", "jpn", "fra", "deu", "spa", "ita", "auto" };
        const wchar_t* audioLangLabels[] = { L"English", L"Japanese", L"French", L"German", L"Spanish", L"Italian", L"Default" };
        const char* subLangCodes[] = { "eng", "jpn", "fra", "deu", "spa", "ita", "off" };
        const wchar_t* subLangLabels[] = { L"English", L"Japanese", L"French", L"German", L"Spanish", L"Italian", L"Off" };
        const float trackW = (usableW - 6.0f * controlGap) / 7.0f;
        // Live subtitle preview — a plausible dim video frame instead of a hatched placeholder, with text rendered at the actual configured
        // pixel size so the slider reflects continuously.
        float cardTop = contentY;
        drawSubtitleCard(cardTop, SettingsLayout::kSubtitlePreviewCardHeight * dp, Loc::Tr("settings.subtitles.preview_title"));
        const D2D1_RECT_F previewStage = D2D1::RectF(subLeft + cardPadX, cardTop + 42.0f * dp,
                                                     cardRight - cardPadX, cardTop + SettingsLayout::kSubtitlePreviewCardHeight * dp - 10.0f * dp);
        // Video-like backdrop: soft vertical light falloff, like a dark scene.
        r.FillRoundedRect(previewStage, 8.0f * dp, D2D1::ColorF(0.16f, 0.165f, 0.175f, 1.0f));
        r.FillGradientRect(previewStage, D2D1::ColorF(0.30f, 0.30f, 0.32f, 0.55f), D2D1::ColorF(0.04f, 0.04f, 0.05f, 0.85f), true);
        r.DrawRoundedRect(previewStage, 8.0f * dp, ColorWithAlpha(surfaceBorder, 0.8f), 1.0f * dp);
        const float previewFontPx = std::clamp(static_cast<int>(std::lround(s.GetSubFontSizePoints())), 14, 32);
        const D2D1_COLOR_F previewTextColor = HexToD2DColor(s.subColorHex);
        const bool previewTop = s.subPosition == "top", previewElevated = s.subPosition == "elevated";
        const float stageH = RectHeight(previewStage);
        const float lineH = (float)previewFontPx * dp * 1.25f;
        const float previewTextY = previewTop ? previewStage.top + 6.0f * dp
            : (previewElevated ? previewStage.top + stageH * 0.5f - lineH * 0.5f : previewStage.bottom - lineH - 6.0f * dp);
        const D2D1_RECT_F previewTextRect = D2D1::RectF(previewStage.left + 10.0f * dp, previewTextY,
                                                        previewStage.right - 10.0f * dp, previewTextY + lineH);
        const std::wstring previewText = Loc::Tr("settings.subtitles.preview_text");
        if (s.subBgStyle == "translucent" || s.subBgStyle == "solid") {
            const float boxAlpha = s.subBgStyle == "solid" ? 0.92f : 0.48f;
            D2D1_SIZE_F tsz = r.MeasureStringSized(previewText, (float)previewFontPx, DWRITE_FONT_WEIGHT_MEDIUM);
            const float boxW = std::min(RectWidth(previewTextRect) - 8.0f * dp, tsz.width + 18.0f * dp);
            const float boxH = lineH;
            const D2D1_RECT_F subtitleBox =
                D2D1::RectF((previewTextRect.left + previewTextRect.right) * 0.5f - boxW * 0.5f, previewTextRect.top,
                            (previewTextRect.left + previewTextRect.right) * 0.5f + boxW * 0.5f, previewTextRect.bottom);
            r.FillRoundedRect(subtitleBox, 4.0f * dp, ColorWithAlpha(D2D1::ColorF(0.0f, 0.0f, 0.0f), boxAlpha));
        }
        // Shadow pass then fill pass, at the real configured size.
        r.DrawStringSized(previewText, previewTextRect, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.85f),
                          (float)previewFontPx, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F shadowShift = previewTextRect;
        shadowShift.top += 1.0f * dp;
        shadowShift.bottom += 1.0f * dp;
        r.DrawStringSized(previewText, shadowShift, previewTextColor, (float)previewFontPx, DWRITE_FONT_WEIGHT_MEDIUM,
                          DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        cardTop += SettingsLayout::kSubtitlePreviewCardHeight * dp + cardGap;
        // Preferred audio track
        drawSubtitleCard(cardTop, SettingsLayout::kSubtitleTrackCardHeight * dp, Loc::Tr("settings.audio.preferred_track"));
        float controlsTop = cardTop + 44.0f * dp;
        for (int i = 0; i < 7; ++i) {
            const D2D1_RECT_F rect = D2D1::RectF(subLeft + i * (trackW + controlGap), controlsTop,
                                                 subLeft + i * (trackW + controlGap) + trackW, controlsTop + controlH);
            drawChoice(rect, audioLangLabels[i], s.audioLang == audioLangCodes[i],
                       showContentKbFocus && vm.keyboardFocusedRow == 0 && vm.keyboardFocusedSubIndex == i,
                       vm.pressState.IsOptionPressed(0, i));
        }
        cardTop += SettingsLayout::kSubtitleTrackCardHeight * dp + cardGap;
        // Preferred subtitle track
        drawSubtitleCard(cardTop, SettingsLayout::kSubtitleTrackCardHeight * dp, Loc::Tr("settings.subtitles.preferred_track"));
        controlsTop = cardTop + 44.0f * dp;
        for (int i = 0; i < 7; ++i) {
            const D2D1_RECT_F rect = D2D1::RectF(subLeft + i * (trackW + controlGap), controlsTop,
                                                 subLeft + i * (trackW + controlGap) + trackW, controlsTop + controlH);
            const bool selected = i == 6 ? !s.subtitlesEnabled : (s.subtitlesEnabled && s.subLang == subLangCodes[i]);
            drawChoice(rect, subLangLabels[i], selected,
                       showContentKbFocus && vm.keyboardFocusedRow == 1 && vm.keyboardFocusedSubIndex == i,
                       vm.pressState.IsOptionPressed(1, i));
        }
        cardTop += SettingsLayout::kSubtitleTrackCardHeight * dp + cardGap;
        // Subtitle size slider
        const int fontPx = std::clamp(static_cast<int>(std::lround(s.GetSubFontSizePoints())), 14, 32);
        const float fontRatio = static_cast<float>(fontPx - 14) / 18.0f;
        drawSubtitleCard(cardTop, SettingsLayout::kSubtitleFontCardHeight * dp, Loc::Tr("settings.subtitles.font_size"),
                         std::to_wstring(fontPx) + L"px");
        const D2D1_RECT_F previewRect = D2D1::RectF(subLeft + cardPadX, cardTop + 45.0f * dp,
                                                     subLeft + cardPadX + 64.0f * dp, cardTop + 89.0f * dp);
        r.FillRoundedRect(previewRect, 8.0f * dp, canvasBg);
        r.DrawRoundedRect(previewRect, 8.0f * dp, (showContentKbFocus && vm.keyboardFocusedRow == 2) ? accentCol : panelBorder,
                          (showContentKbFocus && vm.keyboardFocusedRow == 2) ? 1.5f * dp : 1.0f * dp);
        r.DrawStringSized(L"Aa", previewRect, textPri, (float)fontPx, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                          DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const float sliderLeft = previewRect.right + 18.0f * dp, sliderRight = cardRight - cardPadX, sliderY = cardTop + 66.0f * dp;
        const D2D1_RECT_F sliderRect = D2D1::RectF(sliderLeft, sliderY, sliderRight, sliderY + 4.0f * dp);
        r.FillRoundedRect(sliderRect, 2.0f * dp, surfaceBorder);
        const float thumbX = sliderLeft + std::clamp(fontRatio, 0.0f, 1.0f) * (sliderRight - sliderLeft);
        r.FillRoundedRect(D2D1::RectF(sliderLeft, sliderY, thumbX, sliderY + 4.0f * dp), 2.0f * dp, accentCol);
        r.FillCircle(thumbX, sliderY + 2.0f * dp, 8.0f * dp, textPri);
        r.DrawCircle(thumbX, sliderY + 2.0f * dp, 8.0f * dp, accentCol, 3.0f * dp);
        if (showContentKbFocus && vm.keyboardFocusedRow == 2) { r.DrawCircle(thumbX, sliderY + 2.0f * dp, 11.0f * dp, accentCol, 1.0f * dp); }
        const wchar_t* sizeTicks[] = { L"Small", L"Medium", L"Large", L"Extra large" };
        for (int i = 0; i < 4; ++i) {
            const float tickX = sliderLeft + (sliderRight - sliderLeft) * (static_cast<float>(i) / 3.0f);
            const float tickW = 72.0f * dp;
            D2D1_RECT_F tickRect;
            DWRITE_TEXT_ALIGNMENT tickAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
            if (i == 0) {
                tickRect = D2D1::RectF(sliderLeft, cardTop + 78.0f * dp, sliderLeft + tickW, cardTop + 100.0f * dp);
                tickAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
            } else if (i == 3) {
                tickRect = D2D1::RectF(sliderRight - tickW, cardTop + 78.0f * dp, sliderRight, cardTop + 100.0f * dp);
                tickAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
            } else { tickRect = D2D1::RectF(tickX - tickW * 0.5f, cardTop + 78.0f * dp, tickX + tickW * 0.5f, cardTop + 100.0f * dp); }
            r.DrawString(sizeTicks[i], tickRect, textMute, FontType::Caption, tickAlignment, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        cardTop += SettingsLayout::kSubtitleFontCardHeight * dp + cardGap;
        // Subtitle color swatches
        struct SubColorDef { const char* hex; };
        const SubColorDef subColors[] = {
            { "#cad3f5" }, { "#eed49f" }, { "#a6da95" }, { "#91d7e3" }, { "#f5a97f" }, { "#ed8796" }
        };
        drawSubtitleCard(cardTop, SettingsLayout::kSubtitleColorCardHeight * dp, Loc::Tr("settings.subtitles.text_color"));
        const float swatchRadius = 15.0f * dp, swatchGap = 16.0f * dp, swatchCenterY = cardTop + 54.0f * dp;
        for (int i = 0; i < 6; ++i) {
            const float cx = subLeft + cardPadX + swatchRadius + i * (2.0f * swatchRadius + swatchGap);
            const D2D1_COLOR_F color = HexToD2DColor(subColors[i].hex);
            const bool selected = _stricmp(s.subColorHex.c_str(), subColors[i].hex) == 0;
            const bool focused = showContentKbFocus && vm.keyboardFocusedRow == 3 && vm.keyboardFocusedSubIndex == i;
            const bool pressed = vm.pressState.IsOptionPressed(3, i);
            r.FillCircle(cx, swatchCenterY, swatchRadius, color);
            if (selected) r.DrawCircle(cx, swatchCenterY, 19.0f * dp, textPri, 2.0f * dp);
            if (focused || pressed) r.DrawCircle(cx, swatchCenterY, 22.0f * dp, focused ? accentCol : color, 2.0f * dp);
        }
        cardTop += SettingsLayout::kSubtitleColorCardHeight * dp + cardGap;
        // Background style previews
        const char* bgKeys[] = { "vlc-outline", "translucent", "solid", "clean" };
        const std::wstring bgLabels[] = {Loc::Tr("settings.subtitles.bg_vlc"), Loc::Tr("settings.subtitles.bg_translucent"),
                                         Loc::Tr("settings.subtitles.bg_solid"), Loc::Tr("settings.subtitles.bg_clean")};
        drawSubtitleCard(cardTop, SettingsLayout::kSubtitleStyleCardHeight * dp, Loc::Tr("settings.subtitles.bg_style"));
        const float styleW = (usableW - 3.0f * controlGap) / 4.0f;
        const float styleTop = cardTop + 42.0f * dp, styleH = 92.0f * dp;
        const D2D1_COLOR_F previewBg = LerpColor(canvasBg, surfaceCard, 0.72f);
        for (int i = 0; i < 4; ++i) {
            const D2D1_RECT_F styleRect = D2D1::RectF(subLeft + i * (styleW + controlGap), styleTop,
                                                      subLeft + i * (styleW + controlGap) + styleW, styleTop + styleH);
            const bool selected = s.subBgStyle == bgKeys[i];
            const bool focused = showContentKbFocus && vm.keyboardFocusedRow == 4 && vm.keyboardFocusedSubIndex == i;
            const bool pressed = vm.pressState.IsOptionPressed(4, i);
            r.FillRoundedRect(styleRect, 10.0f * dp, selected ? ColorWithAlpha(accentCol, 0.08f) : canvasBg);
            r.DrawRoundedRect(styleRect, 10.0f * dp, (selected || focused || pressed) ? accentCol : clearColor,
                              (selected || focused || pressed) ? 2.0f * dp : 0.0f);
            const D2D1_RECT_F thumbRect = D2D1::RectF(styleRect.left + 2.0f * dp, styleRect.top + 2.0f * dp,
                                                       styleRect.right - 2.0f * dp, styleRect.top + 62.0f * dp);
            r.FillRoundedRect(thumbRect, 7.0f * dp, previewBg);
            r.FillRect(D2D1::RectF(thumbRect.left, thumbRect.top + 32.0f * dp, thumbRect.right, thumbRect.bottom),
                       ColorWithAlpha(canvasBg, 0.45f));
            const D2D1_RECT_F subRect = D2D1::RectF(thumbRect.left + 4.0f * dp, thumbRect.bottom - 23.0f * dp,
                                                     thumbRect.right - 4.0f * dp, thumbRect.bottom - 3.0f * dp);
            if (i == 1) {
                r.FillRoundedRect(D2D1::RectF(subRect.left + 5.0f * dp, subRect.top + 1.0f * dp,
                                              subRect.right - 5.0f * dp, subRect.bottom - 1.0f * dp),
                                  4.0f * dp, ColorWithAlpha(D2D1::ColorF(0.0f, 0.0f, 0.0f), 0.46f));
            } else if (i == 2) {
                r.FillRoundedRect(D2D1::RectF(subRect.left + 5.0f * dp, subRect.top + 1.0f * dp,
                                              subRect.right - 5.0f * dp, subRect.bottom - 1.0f * dp),
                                  4.0f * dp, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.92f));
            }
            if (i == 3) {
                r.DrawStringWithShadow(L"Subtitle", subRect, textPri, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.9f),
                                       FontType::Medium, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 1.0f * dp);
            } else {
                r.DrawStringWithShadow(L"Subtitle", subRect, textPri, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.9f),
                                       FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 1.0f * dp);
            }
            const D2D1_RECT_F captionRect = D2D1::RectF(styleRect.left + 4.0f * dp, styleRect.top + 65.0f * dp,
                                                        styleRect.right - 4.0f * dp, styleRect.bottom - 2.0f * dp);
            r.DrawString(FitSingleLine(r, bgLabels[i], FontType::Small, styleW - 8.0f * dp), captionRect,
                         selected || focused ? textPri : textMute, selected || focused ? FontType::Medium : FontType::Small,
                         DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        cardTop += SettingsLayout::kSubtitleStyleCardHeight * dp + cardGap;
        // Vertical position previews. The reference orders these Top -> Elevated -> Bottom.
        const char* posKeys[] = { "top", "elevated", "bottom" };
        const std::wstring posLabels[] = {Loc::Tr("settings.subtitles.pos_top"), Loc::Tr("settings.subtitles.pos_elevated"),
                                          Loc::Tr("settings.subtitles.pos_bottom")};
        drawSubtitleCard(cardTop, SettingsLayout::kSubtitlePositionCardHeight * dp, Loc::Tr("settings.subtitles.position"));
        const float posW = (usableW - 2.0f * controlGap) / 3.0f;
        const float posTop = cardTop + 42.0f * dp, posH = 92.0f * dp;
        for (int i = 0; i < 3; ++i) {
            const D2D1_RECT_F posRect = D2D1::RectF(subLeft + i * (posW + controlGap), posTop,
                                                    subLeft + i * (posW + controlGap) + posW, posTop + posH);
            const bool selected = s.subPosition == posKeys[i];
            const bool focused = showContentKbFocus && vm.keyboardFocusedRow == 5 && vm.keyboardFocusedSubIndex == i;
            const bool pressed = vm.pressState.IsOptionPressed(5, i);
            r.FillRoundedRect(posRect, 10.0f * dp, selected ? ColorWithAlpha(accentCol, 0.08f) : canvasBg);
            r.DrawRoundedRect(posRect, 10.0f * dp, (selected || focused || pressed) ? accentCol : clearColor,
                              (selected || focused || pressed) ? 2.0f * dp : 0.0f);
            const D2D1_RECT_F frameRect = D2D1::RectF(posRect.left + 10.0f * dp, posRect.top + 8.0f * dp,
                                                      posRect.right - 10.0f * dp, posRect.top + 70.0f * dp);
            r.FillRoundedRect(frameRect, 6.0f * dp, LerpColor(canvasBg, surfaceCard, 0.36f));
            const float frameW = RectWidth(frameRect);
            const float frameH = RectHeight(frameRect);
            for (float offset = -frameH + 8.0f * dp; offset < frameW; offset += 12.0f * dp) {
                const float startX = frameRect.left + std::max(0.0f, offset);
                const float startY = frameRect.top + std::max(0.0f, -offset);
                const float endX = frameRect.left + std::min(frameW, offset + frameH);
                const float endY = frameRect.top + std::min(frameH, frameW - offset);
                if (endX > startX && endY > startY) { r.DrawLine(startX, startY, endX, endY, ColorWithAlpha(canvasBg, 0.55f), 1.0f * dp); }
            }
            const float barY = (i == 0) ? frameRect.top + 10.0f * dp : (i == 1 ? frameRect.top + 31.0f * dp : frameRect.bottom - 19.0f * dp);
            r.FillRoundedRect(D2D1::RectF(frameRect.left + frameW * 0.14f, barY, frameRect.right - frameW * 0.14f, barY + 9.0f * dp),
                              3.0f * dp, accentCol);
            const D2D1_RECT_F captionRect = D2D1::RectF(posRect.left + 4.0f * dp, posRect.top + 72.0f * dp,
                                                         posRect.right - 4.0f * dp, posRect.bottom - 2.0f * dp);
            r.DrawString(FitSingleLine(r, posLabels[i], FontType::Small, posW - 8.0f * dp), captionRect,
                         selected || focused ? textPri : textMute, selected || focused ? FontType::Medium : FontType::Small,
                         DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    } else if (vm.activeCat == SettingsCategory::Shortcuts) {
        const float pageHeaderTop = contentY;
        D2D1_RECT_F titleRect = D2D1::RectF(contentBounds.left + padX, pageHeaderTop,
                                            contentBounds.right - padX - 160.0f * dp, pageHeaderTop + 26.0f * dp);
        r.DrawString(Loc::Tr("settings.category.shortcuts"), titleRect, textPri, FontType::Title,
                     DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        float resetW = 150.0f * dp, resetH = 26.0f * dp;
        D2D1_RECT_F resetRect = D2D1::RectF(contentBounds.right - padX - resetW, pageHeaderTop - 2.0f * dp,
                                            contentBounds.right - padX, pageHeaderTop - 2.0f * dp + resetH);
        bool isResetKbFocused = showContentKbFocus && (vm.keyboardFocusedRow == 0);
        bool isResetPressed = vm.pressState.IsResetShortcutsPressed();
        SurfaceStyle resetStyle = makeFlatSurface();
        if (isResetPressed) {
            resetStyle.fill = ColorWithAlpha(accentCol, 0.32f);
            resetStyle.border = accentCol;
            resetStyle.borderWidth = 1.5f;
        } else if (isResetKbFocused) {
            resetStyle.fill = surfaceSelected;
            resetStyle.border = accentCol;
            resetStyle.borderWidth = 1.5f;
        } else {
            resetStyle.fill = vm.isResetShortcutsHovered ? surfaceHover : surfaceCard;
            resetStyle.border = vm.isResetShortcutsHovered ? accentCol : surfaceBorder;
            resetStyle.borderWidth = 1.0f;
        }
        r.DrawSurface(resetRect, resetStyle);
        r.DrawString(Loc::Tr("settings.shortcuts.reset"), resetRect, (vm.isResetShortcutsHovered || isResetKbFocused || isResetPressed) ? accentCol : textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F descriptionRect = D2D1::RectF(contentBounds.left + padX, pageHeaderTop + 29.0f * dp,
                                                   contentBounds.right - padX, pageHeaderTop + 48.0f * dp);
        r.DrawString(FitSingleLine(r, Loc::Tr("settings.page.shortcuts"), FontType::Small, contentW), descriptionRect,
                     textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        contentY += SettingsLayout::kPageHeaderReserve * dp;
        contentY += 32.0f * dp;
        auto scList = s.GetShortcutActionListRef();
        size_t totalCount = scList.size();
        size_t mid = (totalCount + 1) / 2;
        float colGap = 16.0f * dp;
        float colW = (contentW - colGap) * 0.5f;
        float rowH = 38.0f * dp, rowGap = 5.0f * dp;
        const float shortcutListH = mid > 0 ? mid * (rowH + rowGap) - rowGap : 0.0f;
        drawFlatPanel(D2D1::RectF(contentBounds.left + padX - 10.0f * dp, contentY - 8.0f * dp,
                                  contentBounds.right - padX + 10.0f * dp, contentY + shortcutListH + 8.0f * dp),
                      12.0f, panelFill, panelBorder);
        for (size_t rowIdx = 0; rowIdx < mid; ++rowIdx) {
            float rTop = contentY + rowIdx * (rowH + rowGap);
            auto drawShortcutCell = [&](size_t idx, float left, float right, int colIndex) {
                const auto& [actionId, actionName] = scList[idx];
                bool isRebinding = (vm.rebindingActionId == actionId);
                bool isHover = (vm.hoveredShortcutIndex == (int)idx);
                bool isKbFocused = showContentKbFocus && (vm.keyboardFocusedRow == (int)rowIdx + 1) && (vm.keyboardFocusedSubIndex == colIndex);
                D2D1_RECT_F rRect = D2D1::RectF(left, rTop, right, rTop + rowH);
                SurfaceStyle rowStyle = makeFlatSurface();
                if (isKbFocused) {
                    rowStyle.fill = surfaceSelected;
                    rowStyle.border = accentCol;
                    rowStyle.borderWidth = 2.0f;
                } else if (isRebinding) {
                    rowStyle.fill = surfaceSelected;
                    rowStyle.border = accentCol;
                    rowStyle.borderWidth = 1.5f;
                } else if (isHover) {
                    rowStyle.fill = surfaceHover;
                    rowStyle.border = surfaceBorder;
                    rowStyle.borderWidth = 1.0f;
                } else { rowStyle.fill = clearColor; rowStyle.border = clearColor; rowStyle.borderWidth = 0.0f; }
                r.DrawSurface(rRect, rowStyle);
                D2D1_RECT_F nameRect = D2D1::RectF(rRect.left + 12.0f * dp, rRect.top, rRect.right - 95.0f * dp, rRect.bottom);
                D2D1_COLOR_F rowTextCol = (isRebinding || isKbFocused || isHover) ? textPri : textNorm;
                r.DrawString(FitSingleLine(r, Utf8ToUtf16(actionName), FontType::Regular, SafeWidth(nameRect.left, nameRect.right)), nameRect, rowTextCol, (isRebinding || isKbFocused) ? FontType::Medium : FontType::Regular, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                std::wstring keyText;
                if (isRebinding) { keyText = Loc::Tr("settings.shortcuts.press_key"); } else {
                    auto it = s.shortcuts.find(actionId);
                    keyText = (it != s.shortcuts.end()) ? Settings::FormatShortcutKey(it->second) : Loc::Tr("settings.shortcuts.unassigned");
                }
                float keyW = std::max(68.0f * dp, r.MeasureString(keyText, FontType::MonoSmall).width + 16.0f * dp);
                D2D1_RECT_F badge = D2D1::RectF(rRect.right - keyW - 8.0f * dp, rRect.top + 6.0f * dp, rRect.right - 8.0f * dp, rRect.bottom - 6.0f * dp);
                r.FillRoundedRect(badge, 6.0f * dp, isRebinding ? accentCol : surfaceBg);
                r.DrawRoundedRect(badge, 6.0f * dp, isRebinding ? accentCol : surfaceBorder, 1.0f);
                r.DrawString(keyText, badge, isRebinding ? surfaceBg : textMute, FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            };
            // Left column item (col 0)
            if (rowIdx < totalCount) { drawShortcutCell(rowIdx, contentBounds.left + padX, contentBounds.left + padX + colW, 0); }
            // Right column item (col 1)
            if (mid + rowIdx < totalCount)
                drawShortcutCell(mid + rowIdx, contentBounds.left + padX + colW + colGap, contentBounds.right - padX, 1);
            if (rowIdx + 1 < mid) {
                r.DrawLine(contentBounds.left + padX + 8.0f * dp, rTop + rowH + rowGap * 0.5f,
                           contentBounds.right - padX - 8.0f * dp, rTop + rowH + rowGap * 0.5f, panelBorder, 1.0f * dp);
            }
        }
    } else if (vm.activeCat == SettingsCategory::System) {
        drawPageHeader(Loc::Tr("settings.category.system"), Loc::Tr("settings.page.system"));
        drawHeader(Loc::Tr("settings.system.about_header").c_str());
        float aboutCardH = SettingsLayout::kSystemAboutHeight * dp;
        const auto aboutRect = D2D1::RectF(contentBounds.left + padX, contentY, contentBounds.right - padX, contentY + aboutCardH);
        SurfaceStyle aStyle = makeFlatSurface();
        aStyle.fill = surfaceCard;
        aStyle.border = surfaceBorder;
        aStyle.borderWidth = 1.0f;
        r.DrawSurface(aboutRect, aStyle);

        // Polished Aperture app emblem in the About banner
        const float logoSz = 40.0f * dp;
        const float logoLeft = isRtl ? (aboutRect.right - 14.0f * dp - logoSz) : (aboutRect.left + 14.0f * dp);
        const float logoTop = aboutRect.top + (aboutCardH - logoSz) * 0.5f;
        const D2D1_RECT_F logoBox = D2D1::RectF(logoLeft, logoTop, logoLeft + logoSz, logoTop + logoSz);
        r.FillRoundedRect(logoBox, 9.0f * dp, ColorWithAlpha(accentCol, 0.12f));
        r.DrawRoundedRect(logoBox, 9.0f * dp, ColorWithAlpha(accentCol, 0.35f), 1.0f);
        const float iconSz = 20.0f * dp;
        const D2D1_RECT_F logoIconRect = D2D1::RectF(logoBox.left + (logoSz - iconSz) * 0.5f, logoBox.top + (logoSz - iconSz) * 0.5f,
                                                     logoBox.left + (logoSz + iconSz) * 0.5f, logoBox.top + (logoSz + iconSz) * 0.5f);
        r.DrawIcon(IconType::ApertureLogo, logoIconRect, accentCol, 1.6f * dp);

        const float infoLeft = isRtl ? (aboutRect.left + 16.0f * dp) : (logoBox.right + 12.0f * dp);
        const float infoRight = isRtl ? (logoBox.left - 12.0f * dp) : (aboutRect.right - 16.0f * dp);
        r.DrawString(L"Aperture " + std::wstring(Version::DisplayVersionWide),
            D2D1::RectF(infoLeft, aboutRect.top + 12 * dp, infoRight, aboutRect.top + 34 * dp),
            textPri, FontType::Bold, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        r.DrawString(Loc::Tr("settings.system.portable_desc"),
            D2D1::RectF(infoLeft, aboutRect.top + 38 * dp, infoRight, aboutRect.bottom - 10 * dp),
            textMute, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        contentY += aboutCardH + 18.0f * dp;
        drawHeader(Loc::Tr("settings.system.actions_header").c_str());
        struct SysBtn { std::wstring label; std::wstring desc; bool isDanger; IconType icon; };
        SysBtn btns[] = {
            { Loc::Tr("settings.system.check_updates"), Loc::Tr("settings.system.check_updates_desc"), false, IconType::Refresh },
            { Loc::Tr("settings.system.hover_previews"), s.hoverPreviewEnabled ? Loc::Tr("settings.system.hover_previews_on") : Loc::Tr("settings.system.hover_previews_off"), false, IconType::MediaFilm },
            { Loc::Tr("settings.system.reduce_motion"), s.reduceMotionEnabled ? Loc::Tr("settings.system.reduce_motion_on") : Loc::Tr("settings.system.reduce_motion_off"), false, IconType::SpeedGauge },
            { Loc::Tr("settings.system.smart_series"), s.smartSeriesDetection ? Loc::Tr("settings.system.smart_series_on") : Loc::Tr("settings.system.smart_series_off"), false, IconType::FolderPlaylist },
            { Loc::Tr("settings.system.open_data"), Loc::Tr("settings.system.open_data_desc"), false, IconType::AspectFrame },
            { Loc::Tr("settings.system.clear_disk_cache"), Loc::Tr("settings.system.clear_disk_cache_desc"), false, IconType::Trash },
            { Loc::Tr("settings.system.clear_history"), Loc::Tr("settings.system.clear_history_desc"), true, IconType::HistoryClock },
            { Loc::Tr("settings.system.reset_defaults"), Loc::Tr("settings.system.reset_defaults_desc"), true, IconType::Refresh }};
        float bCardH = 58.0f * dp;
        const float systemListH = SettingsLayout::kSystemRowCount * bCardH + (SettingsLayout::kSystemRowCount - 1) * 10.0f * dp;
        drawFlatPanel(D2D1::RectF(contentBounds.left + padX - 10.0f * dp, contentY - 8.0f * dp,
                                  contentBounds.right - padX + 10.0f * dp, contentY + systemListH + 8.0f * dp),
                      12.0f, panelFill, panelBorder);
        for (int i = 0; i < SettingsLayout::kSystemRowCount; ++i) {
            D2D1_RECT_F bRect = D2D1::RectF(contentBounds.left + padX, contentY + i * (bCardH + 10.0f * dp), contentBounds.right - padX, contentY + i * (bCardH + 10.0f * dp) + bCardH);
            bool isKbFocused = showContentKbFocus && (vm.keyboardFocusedRow == i);
            bool isPressed = vm.pressState.IsSystemActionPressed(i);
            const float hoverProg = std::clamp(vm.systemRowHoverProg[i], 0.0f, 1.0f);
            // Press reads instantly on mouse-down; the eased progress only softens the release.
            const float pressProg = isPressed ? 1.0f : std::clamp(vm.systemRowPressProg[i], 0.0f, 1.0f);
            const bool danger = btns[i].isDanger;
            // Content settles 1 dp down while pressed, matching the nav-bar press model.
            const float pressDy = (1.0f * dp) * pressProg;

            // Flat state fills on the same rhythm as the sidebar categories and
            // option cards: hover tint eases, press overrides instantly.
            SurfaceStyle bStyle = makeFlatSurface();
            if (isKbFocused) {
                bStyle.fill = danger ? ColorWithAlpha(dangerCol, 0.18f) : surfaceSelected;
                bStyle.border = danger ? dangerCol : accentCol;
                bStyle.borderWidth = 2.0f;
            } else {
                const D2D1_COLOR_F hoverFill = danger ? ColorWithAlpha(dangerCol, 0.12f) : ColorWithAlpha(accentCol, 0.08f);
                const D2D1_COLOR_F hoverBorder = danger ? ColorWithAlpha(dangerCol, 0.35f) : ColorWithAlpha(accentCol, 0.30f);
                bStyle.fill = LerpColor(clearColor, hoverFill, hoverProg);
                bStyle.border = LerpColor(clearColor, hoverBorder, hoverProg);
                bStyle.borderWidth = (hoverProg > 0.001f) ? 1.0f : 0.0f;
                if (pressProg > 0.001f) {
                    const D2D1_COLOR_F pressFill = danger ? ColorWithAlpha(dangerCol, 0.28f) : ColorWithAlpha(accentCol, 0.28f);
                    bStyle.fill = LerpColor(bStyle.fill, pressFill, pressProg);
                    bStyle.border = LerpColor(bStyle.border, danger ? dangerCol : accentCol, pressProg);
                }
            }
            r.DrawSurface(bRect, bStyle);

            if (i < SettingsLayout::kSystemRowCount - 1) {
                r.DrawLine(bRect.left + 8.0f * dp, bRect.bottom + 5.0f * dp, bRect.right - 8.0f * dp, bRect.bottom + 5.0f * dp,
                           panelBorder, 1.0f * dp);
            }

            // Leading icon container, tinted on hover like the option cards.
            const float iconBoxSz = 34.0f * dp;
            const float iconBoxX = isRtl ? (bRect.right - 14.0f * dp - iconBoxSz) : (bRect.left + 14.0f * dp);
            const float iconBoxY = bRect.top + (bCardH - iconBoxSz) * 0.5f + pressDy;
            D2D1_RECT_F iconBox = D2D1::RectF(iconBoxX, iconBoxY, iconBoxX + iconBoxSz, iconBoxY + iconBoxSz);
            D2D1_COLOR_F iconBoxFill = LerpColor(ColorWithAlpha(surfaceCard, 0.6f), danger ? ColorWithAlpha(dangerCol, 0.16f) : ColorWithAlpha(accentCol, 0.15f), hoverProg);
            D2D1_COLOR_F iconBoxBorder = LerpColor(surfaceBorder, danger ? ColorWithAlpha(dangerCol, 0.45f) : ColorWithAlpha(accentCol, 0.45f), hoverProg);
            if (pressProg > 0.001f) {
                iconBoxFill = LerpColor(iconBoxFill, danger ? ColorWithAlpha(dangerCol, 0.32f) : ColorWithAlpha(accentCol, 0.28f), pressProg);
                iconBoxBorder = LerpColor(iconBoxBorder, danger ? dangerCol : accentCol, pressProg);
            }
            r.FillRoundedRect(iconBox, 7.0f * dp, iconBoxFill);
            r.DrawRoundedRect(iconBox, 7.0f * dp, iconBoxBorder, 1.0f);
            const float rowIconSz = 16.0f * dp;
            D2D1_RECT_F iconRect = D2D1::RectF(iconBox.left + (iconBoxSz - rowIconSz) * 0.5f, iconBox.top + (iconBoxSz - rowIconSz) * 0.5f,
                                               iconBox.left + (iconBoxSz + rowIconSz) * 0.5f, iconBox.top + (iconBoxSz + rowIconSz) * 0.5f);
            const D2D1_COLOR_F iconCol = danger ? dangerCol : LerpColor(textMute, accentCol, hoverProg);
            r.DrawIcon(btns[i].icon, iconRect, iconCol, 1.4f * dp);

            // Title & description; color carries hover, position stays fixed.
            const float textLeft = isRtl ? (bRect.left + 130.0f * dp) : (iconBox.right + 12.0f * dp);
            const float textRight = isRtl ? (iconBox.left - 12.0f * dp) : (bRect.right - 130.0f * dp);
            D2D1_RECT_F titleRect = D2D1::RectF(textLeft, bRect.top + 10.0f * dp + pressDy, textRight, bRect.top + 30.0f * dp + pressDy);
            D2D1_RECT_F descRect = D2D1::RectF(textLeft, bRect.top + 31.0f * dp + pressDy, textRight, bRect.bottom - 8.0f * dp + pressDy);
            const D2D1_COLOR_F titleCol = danger ? dangerCol : (isKbFocused ? textPri : LerpColor(textNorm, textPri, hoverProg));
            r.DrawString(btns[i].label, titleRect, titleCol, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            r.DrawString(btns[i].desc, descRect, LerpColor(textMute, textNorm, hoverProg * 0.35f), FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            // Right-aligned action badge; static geometry, state lives in color.
            const float actW = 110.0f * dp, actH = 28.0f * dp;
            const float actCX = isRtl ? (bRect.left + actW * 0.5f + 14.0f * dp) : (bRect.right - actW * 0.5f - 14.0f * dp);
            const float actCY = bRect.top + bCardH * 0.5f + pressDy;
            D2D1_RECT_F actRect = D2D1::RectF(actCX - actW * 0.5f, actCY - actH * 0.5f, actCX + actW * 0.5f, actCY + actH * 0.5f);
            D2D1_COLOR_F actBg;
            D2D1_COLOR_F actBorder;
            D2D1_COLOR_F actText;
            if (danger) {
                actBg = LerpColor(ColorWithAlpha(dangerCol, 0.14f), ColorWithAlpha(dangerCol, 0.25f), hoverProg);
                actBorder = LerpColor(dangerBorder, dangerCol, hoverProg);
                actText = dangerCol;
                if (pressProg > 0.001f) {
                    actBg = LerpColor(actBg, ColorWithAlpha(dangerCol, 0.55f), pressProg);
                    actBorder = dangerCol;
                    actText = LerpColor(actText, D2D1::ColorF(1.0f, 1.0f, 1.0f), pressProg);
                }
            } else if (i == 1 || i == 2 || i == 3) {
                const bool isEnabled = (i == 1 && s.hoverPreviewEnabled) ||
                                       (i == 2 && s.reduceMotionEnabled) ||
                                       (i == 3 && s.smartSeriesDetection);
                if (isEnabled) {
                    actBg = LerpColor(ColorWithAlpha(accentCol, 0.20f), ColorWithAlpha(accentCol, 0.32f), hoverProg);
                    actBorder = LerpColor(ColorWithAlpha(accentCol, 0.60f), accentCol, hoverProg);
                    actText = accentCol;
                } else {
                    actBg = LerpColor(surfaceBg, ColorWithAlpha(accentCol, 0.12f), hoverProg);
                    actBorder = LerpColor(surfaceBorder, ColorWithAlpha(accentCol, 0.50f), hoverProg);
                    actText = LerpColor(textMute, textNorm, hoverProg);
                }
                if (pressProg > 0.001f) {
                    actBg = LerpColor(actBg, accentCol, pressProg);
                    actBorder = accentCol;
                    actText = LerpColor(actText, surfaceBg, pressProg);
                }
            } else {
                actBg = LerpColor(surfaceBg, ColorWithAlpha(accentCol, 0.18f), hoverProg);
                actBorder = LerpColor(surfaceBorder, ColorWithAlpha(accentCol, 0.60f), hoverProg);
                actText = LerpColor(textNorm, textPri, hoverProg);
                if (pressProg > 0.001f) {
                    actBg = LerpColor(actBg, accentCol, pressProg);
                    actBorder = accentCol;
                    actText = LerpColor(actText, surfaceBg, pressProg);
                }
            }
            if (isKbFocused) { actBg = accentCol; actBorder = accentCol; actText = surfaceBg; }
            r.FillRoundedRect(actRect, 6.0f * dp, actBg);
            r.DrawRoundedRect(actRect, 6.0f * dp, actBorder, 1.2f);
            std::wstring btnTags[] = {
                Loc::Tr("settings.system.btn.check_now"),
                s.hoverPreviewEnabled ? Loc::Tr("settings.system.btn.enabled") : Loc::Tr("settings.system.btn.disabled"),
                s.reduceMotionEnabled ? Loc::Tr("settings.system.btn.enabled") : Loc::Tr("settings.system.btn.disabled"),
                s.smartSeriesDetection ? Loc::Tr("settings.system.btn.enabled") : Loc::Tr("settings.system.btn.disabled"),
                Loc::Tr("settings.system.btn.open_data"), Loc::Tr("settings.system.btn.clear_disk"),
                Loc::Tr("settings.system.btn.clear_history"), Loc::Tr("settings.system.btn.reset_defaults")
            };
            r.DrawString(btnTags[i], actRect, actText, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    r.PopClip();
}
void UIComponents::DrawPlayerTopBar(OverlayRenderer& r, const Settings& s, const PlayerTopBarViewModel& vm) {
    if (!vm.visible || vm.opacity <= 0.001f) return;
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    (void)th;
    float padX = 24.0f * dp, padY = 16.0f * dp;
    // Back Button — always a quiet circular affordance, with a larger luminous state on hover. This matches the WebView's 36px round button
    // and keeps the hit target visible even when the overlay is semi-idle.
    float btnSize = 36.0f * dp;
    D2D1_RECT_F backBtn = D2D1::RectF(vm.bounds.left + padX, vm.bounds.top + padY, vm.bounds.left + padX + btnSize, vm.bounds.top + padY + btnSize);
    float backCX = backBtn.left + btnSize * 0.5f, backCY = backBtn.top + btnSize * 0.5f;
    r.FillCircle(backCX, backCY, btnSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, (vm.isBackHovered ? 0.30f : 0.15f) * vm.opacity));
    if (vm.isBackHovered) { r.DrawCircle(backCX, backCY, btnSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f * vm.opacity), 1.0f * dp); }
    r.DrawIcon(IconType::ChevronLeft, InsetRectF(backBtn, 8.0f * dp, 8.0f * dp), D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity), 1.75f * dp);
    // Video title with drop shadow for contrast against bright scenes.
    D2D1_RECT_F titleRect = D2D1::RectF(backBtn.right + 14.0f * dp, vm.bounds.top + padY, vm.bounds.right - padX, vm.bounds.top + padY + btnSize);
    D2D1_RECT_F titleShadow = D2D1::RectF(titleRect.left + dp, titleRect.top + dp, titleRect.right + dp, titleRect.bottom + dp);
    r.DrawString(vm.title, titleShadow, D2D1::ColorF(0, 0, 0, 0.75f * vm.opacity), FontType::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    r.DrawString(vm.title, titleRect, D2D1::ColorF(1, 1, 1, vm.opacity), FontType::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    if (!vm.showKeyboardFocus || vm.focusedControlId == 0) return;
    const auto focus = ColorWithAlpha(s.currentTheme.focusRing, vm.opacity);
    if (vm.focusedControlId == 9001) {
        const D2D1_RECT_F back = D2D1::RectF(vm.bounds.left + 24.0f * dp, vm.bounds.top + 16.0f * dp,
            vm.bounds.left + 60.0f * dp, vm.bounds.top + 52.0f * dp);
        r.DrawCircle((back.left + back.right) * 0.5f, (back.top + back.bottom) * 0.5f, 18.0f * dp, focus, 1.5f * dp);
    }
}

void UIComponents::DrawPlayerBottomBar(OverlayRenderer& r, const Settings& s, const PlayerBottomBarViewModel& vm) {
    float timelineHoverProgress = vm.timelineHoverProgress;
    if (!vm.visible || vm.opacity <= 0.001f) return;
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    float padX = Presentation::PlayerPadding * dp;
    const auto layout = Presentation::Player(RectWidth(vm.bounds), vm.bounds.bottom, dp, vm.isQueuePlayback);
    if (timelineHoverProgress < 0.0f) { timelineHoverProgress = (vm.isTimelineHovered || vm.isScrubbing) ? 1.0f : 0.0f; }
    const bool timelineActive = (vm.isScrubbing || vm.isTimelineHovered || timelineHoverProgress > 0.01f);
    // YouTube-style geometry: 3.5px at rest, smoothly animated up to 5.5px on hover, 6.0px while scrubbing.
    const float expandedTrackH = vm.isScrubbing ? 6.0f : 5.5f;
    float trackH = (3.5f + (expandedTrackH - 3.5f) * timelineHoverProgress) * dp;
    float trackCenterY = vm.bounds.top + 16.0f * dp, trackY = trackCenterY - trackH * 0.5f;
    float barW = RectWidth(vm.bounds) - padX * 2.0f;
    D2D1_RECT_F trackRect = D2D1::RectF(vm.bounds.left + padX, trackY, vm.bounds.right - padX, trackY + trackH);
    // Track Background Groove
    r.FillRoundedRect(trackRect, trackH * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f * vm.opacity));
    // Cache / Buffered Bar
    if (vm.duration > 0.0 && vm.cacheDuration > 0.0) {
        double bufferRatio = std::clamp((vm.currentTime + vm.cacheDuration) / vm.duration, 0.0, 1.0);
        float bufferW = barW * (float)bufferRatio;
        if (bufferW > 0.0f) {
            D2D1_RECT_F bufferRect = D2D1::RectF(trackRect.left, trackRect.top, trackRect.left + bufferW, trackRect.bottom);
            r.FillRoundedRect(bufferRect, trackH * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.38f * vm.opacity));
        }
    }
    double ratio = (vm.duration > 0.0) ? std::clamp(vm.currentTime / vm.duration, 0.0, 1.0) : 0.0;
    float playedW = barW * (float)ratio;
    float knobX = trackRect.left + playedW;
    // Hover preview of the target point (YouTube behavior): While hovering without scrubbing, show a soft whitish/gray bar up to the
    // cursor. Drawn BEFORE the played bar so the played accent color stays vibrant on top.
    float bubbleAnchorX = knobX;
    double bubbleTime = std::clamp(vm.currentTime, 0.0, vm.duration);
    if (vm.duration > 0.0 && vm.isTimelineHovered && !vm.isScrubbing && vm.timelineHoverX >= trackRect.left && vm.timelineHoverX <= trackRect.right) {
        const float hoverRatio = std::clamp((vm.timelineHoverX - trackRect.left) / barW, 0.0f, 1.0f);
        float hoverW = barW * hoverRatio;
        if (hoverW > 0.0f) {
            D2D1_RECT_F hoverRect = D2D1::RectF(trackRect.left, trackRect.top, trackRect.left + hoverW, trackRect.bottom);
            r.FillRoundedRect(hoverRect, trackH * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f * vm.opacity));
        }
        bubbleAnchorX = vm.timelineHoverX;
        bubbleTime = hoverRatio * vm.duration;
    }
    // Played Progress Bar (drawn on top of the hover bar)
    if (playedW > 0.0f) {
        D2D1_RECT_F playedRect = D2D1::RectF(trackRect.left, trackRect.top, trackRect.left + playedW, trackRect.bottom);
        r.FillRoundedRect(playedRect, trackH * 0.5f, ColorWithAlpha(th.accent, vm.opacity));
    }
    // Chapter Markers on Seekbar (Theme-aware, non-interactive, does not obscure progress or hit testing)
    if (vm.chapterMarkers && !vm.chapterMarkers->empty() && barW > 10.0f) {
        float markerW = std::max(2.0f * dp, 2.0f);
        for (const auto& marker : *vm.chapterMarkers) {
            if (marker.normalizedPosition <= 0.001f || marker.normalizedPosition >= 0.999f) continue;
            float markerX = trackRect.left + marker.normalizedPosition * barW;
            D2D1_RECT_F markerRect = D2D1::RectF(markerX - markerW * 0.5f, trackRect.top, markerX + markerW * 0.5f, trackRect.bottom);
            // Dark notch divider separating chapters
            r.FillRect(markerRect, D2D1::ColorF(0.04f, 0.05f, 0.07f, 0.95f * vm.opacity));
            // Accessible theme/contrast indicator
            if (marker.isPassed) {
                r.FillRect(D2D1::RectF(markerRect.left, markerRect.top, markerRect.left + 1.0f * dp, markerRect.bottom), ColorWithAlpha(th.accent, 0.45f * vm.opacity));
            } else {
                r.FillRect(D2D1::RectF(markerRect.left, markerRect.top, markerRect.left + 1.0f * dp, markerRect.bottom), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f * vm.opacity));
            }
        }
    }
    // Scrubber Thumb Knob (Round Ball at current progress) In YouTube: present on hover (6.0dp) and while scrubbing (7.0dp), hidden at rest
    // (0.0dp). Smoothly animated with timelineHoverProgress so it blooms and moves without jitter or jagged popping.
    const float targetRadius = vm.isScrubbing ? 7.0f : 6.0f;
    float knobRadius = targetRadius * timelineHoverProgress * dp;
    if (knobRadius > 0.5f) { r.FillCircle(knobX, trackCenterY, knobRadius, ColorWithAlpha(th.accent, vm.opacity)); }
    // Exact-time bubble above the anchor point (capsule pill shape matching YouTube)
    if (timelineHoverProgress > 0.05f && vm.duration > 0.0) {
        if (vm.isScrubbing) { bubbleAnchorX = knobX; bubbleTime = vm.currentTime; }
        std::wstring bubbleText = FormatDuration(bubbleTime);
        D2D1_SIZE_F tsz = r.MeasureStringSized(bubbleText, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float bubbleW = std::max(48.0f * dp, tsz.width + 20.0f * dp);
        float bubbleH = 24.0f * dp;
        float bubbleX = std::clamp(bubbleAnchorX, vm.bounds.left + padX + bubbleW * 0.5f, vm.bounds.right - padX - bubbleW * 0.5f);
        float bubbleY = vm.bounds.top - bubbleH - 8.0f * dp;
        D2D1_RECT_F bubbleRect = D2D1::RectF(bubbleX - bubbleW * 0.5f, bubbleY, bubbleX + bubbleW * 0.5f, bubbleY + bubbleH);
        float bubbleOpacity = vm.opacity * std::clamp(timelineHoverProgress, 0.0f, 1.0f);
        // Pill capsule background (stadium shape with radius = bubbleH * 0.5f)
        r.FillRoundedRect(bubbleRect, bubbleH * 0.5f, D2D1::ColorF(0.08f, 0.08f, 0.09f, 0.88f * bubbleOpacity));
        // Clean modern sans-serif semi-bold centered timestamp text
        r.DrawStringSized(bubbleText, bubbleRect, D2D1::ColorF(1.0f, 1.0f, 1.0f, bubbleOpacity), 12.0f,
                          DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // Control Buttons Row (Statically positioned; timeline hover never shifts it.)
    float controlsY = vm.bounds.top + Presentation::PlayerControlsY * dp, btnSize = 36.0f * dp, curX = vm.bounds.left + padX;
    const float availableW = RectWidth(vm.bounds);
    const bool narrowLayout = availableW < 720.0f * dp, veryNarrowLayout = availableW < 520.0f * dp;
    // Queue playback exposes the missing reverse transport without changing the ordinary library transport layout.
    if (vm.isQueuePlayback) {
        D2D1_RECT_F prevBtn = layout.prev;
        const float prevAlpha = vm.isFirstQueueButtonDisabled ? 0.34f : 1.0f;
        if (vm.hoveredButtonId == 8 && !vm.isFirstQueueButtonDisabled) {
            r.FillCircle(prevBtn.left + btnSize * 0.5f, prevBtn.top + btnSize * 0.5f, btnSize * 0.5f,
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f * vm.opacity));
        }
        r.DrawIcon(IconType::Prev, InsetRectF(prevBtn, 8.0f * dp, 8.0f * dp), D2D1::ColorF(1.0f, 1.0f, 1.0f, prevAlpha * vm.opacity));
    }
    // 1. Play / Pause
    D2D1_RECT_F playBtn = layout.play;
    if (vm.hoveredButtonId == 1) r.FillCircle(playBtn.left + btnSize * 0.5f, playBtn.top + btnSize * 0.5f, btnSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f * vm.opacity));
    r.DrawIcon(vm.isPlaying ? IconType::Pause : IconType::Play, InsetRectF(playBtn, 8.0f * dp, 8.0f * dp), D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity));
    curX += btnSize + 6.0f * dp;
    // 2. Next
    D2D1_RECT_F nextBtn = layout.next;
    if (vm.hoveredButtonId == 2) r.FillCircle(nextBtn.left + btnSize * 0.5f, nextBtn.top + btnSize * 0.5f, btnSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f * vm.opacity));
    r.DrawIcon(IconType::Next, InsetRectF(nextBtn, 8.0f * dp, 8.0f * dp), D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity));
    curX += btnSize + 8.0f * dp;
    // 3. Volume Group (YouTube style with moving ball)
    D2D1_RECT_F volBtn = layout.mute;
    curX = volBtn.right + 4.0f * dp;
    float volSliderW = narrowLayout ? 0.0f : 60.0f * dp;
    if (!narrowLayout) {
        float volSliderH = 3.5f * dp, volTrackX = curX;
        float volTrackY = controlsY + (btnSize - volSliderH) * 0.5f;
        D2D1_RECT_F volTrack = D2D1::RectF(volTrackX, volTrackY, volTrackX + volSliderW, volTrackY + volSliderH);
        if (vm.isVolumeSliderHovered) {
            D2D1_RECT_F volCapsule = D2D1::RectF(volTrackX - 10.0f * dp, controlsY - 8.0f * dp, volTrackX + volSliderW + 10.0f * dp, controlsY + btnSize + 8.0f * dp);
            r.FillRoundedRect(volCapsule, btnSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f * vm.opacity));
        }
        r.FillRoundedRect(volTrack, volSliderH * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f * vm.opacity));
        // Full slider travel equals kMaxVolumeRatio (130%); the fill fraction scales accordingly.
        float volFillW = volSliderW * std::clamp((float)((vm.isMuted ? 0.0 : vm.volume) / kMaxVolumeRatio), 0.0f, 1.0f);
        if (volFillW > 0.0f) {
            D2D1_RECT_F volFill = D2D1::RectF(volTrack.left, volTrack.top, volTrack.left + volFillW, volTrack.bottom);
            r.FillRoundedRect(volFill, volSliderH * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f * vm.opacity));
        }
        float ballX = std::round(volTrack.left + volFillW);
        float ballY = volTrack.top + volSliderH * 0.5f, ballRadius = 6.0f * dp;
        r.FillCircle(ballX, ballY, ballRadius, D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity));
        r.DrawCircle(ballX, ballY, ballRadius, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f * vm.opacity), 0.75f * dp);
    }
    if (vm.hoveredButtonId == 3)
        r.FillCircle(volBtn.left + btnSize * 0.5f, volBtn.top + btnSize * 0.5f, btnSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f * vm.opacity));
    IconType volIcon = vm.isMuted || vm.volume <= 0.01 ? IconType::VolumeMuted : (vm.volume > 0.5 ? IconType::VolumeHigh : IconType::VolumeLow);
    r.DrawIcon(volIcon, InsetRectF(volBtn, 8.0f * dp, 8.0f * dp), D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity));
    curX += volSliderW + 18.0f * dp;
    // Time text yields first on narrow layouts; transport and mute remain.
    if (!narrowLayout) {
        std::wstring timeStr = FormatDuration(vm.currentTime) + L" / " + FormatDuration(vm.duration);
        D2D1_RECT_F timeRect = D2D1::RectF(curX, controlsY, curX + 160.0f * dp, controlsY + btnSize);
        D2D1_RECT_F timeShadow = D2D1::RectF(timeRect.left + 1.0f * dp, timeRect.top + 1.0f * dp, timeRect.right + 1.0f * dp, timeRect.bottom + 1.0f * dp);
        r.DrawString(timeStr, timeShadow, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f * vm.opacity), FontType::Mono, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        r.DrawString(timeStr, timeRect, D2D1::ColorF(0.95f, 0.95f, 0.95f, vm.opacity), FontType::Mono, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // Right Controls
    float rightX = vm.bounds.right - padX - btnSize;
    // Fullscreen
    D2D1_RECT_F fullBtn = layout.fullscreen;
    if (vm.hoveredButtonId == 7) r.FillCircle(fullBtn.left + btnSize * 0.5f, fullBtn.top + btnSize * 0.5f, btnSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f * vm.opacity));
    r.DrawIcon(vm.isFullscreen ? IconType::ExitFullscreen : IconType::Fullscreen, InsetRectF(fullBtn, 8.0f * dp, 8.0f * dp), D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity));
    rightX -= btnSize + 6.0f * dp;
    // PiP is secondary on very narrow layouts.
    if (!veryNarrowLayout) {
        D2D1_RECT_F pipBtn = layout.pip;
        if (vm.hoveredButtonId == 6) r.FillCircle(pipBtn.left + btnSize * 0.5f, pipBtn.top + btnSize * 0.5f, btnSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f * vm.opacity));
        r.DrawIcon(IconType::PictureInPicture, InsetRectF(pipBtn, 8.0f * dp, 8.0f * dp), vm.isPiP ? ColorWithAlpha(th.accent, vm.opacity) : D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity));
        rightX -= btnSize + 6.0f * dp;
    }
    // Settings
    D2D1_RECT_F setBtn = layout.settings;
    if (vm.hoveredButtonId == 5) r.FillCircle(setBtn.left + btnSize * 0.5f, setBtn.top + btnSize * 0.5f, btnSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f * vm.opacity));
    r.DrawIcon(IconType::SettingsGear, InsetRectF(setBtn, 8.0f * dp, 8.0f * dp), D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity));
    rightX -= btnSize + 6.0f * dp;
    // Subtitle state remains in Settings on narrow layouts.
    if (!narrowLayout) {
        D2D1_RECT_F ccBtn = layout.subtitles;
        if (vm.hoveredButtonId == 4) r.FillCircle(ccBtn.left + btnSize * 0.5f, ccBtn.top + btnSize * 0.5f, btnSize * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f * vm.opacity));
        r.DrawIcon(IconType::Subtitles, InsetRectF(ccBtn, 8.0f * dp, 8.0f * dp), vm.isSubtitlesActive ? ColorWithAlpha(th.accent, vm.opacity) : D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity));
        if (vm.isSubtitlesActive) {
            D2D1_RECT_F ccPill = D2D1::RectF(ccBtn.left + 8.0f * dp, ccBtn.bottom - 4.0f * dp, ccBtn.right - 8.0f * dp, ccBtn.bottom - 2.0f * dp);
            r.FillRoundedRect(ccPill, 1.0f * dp, ColorWithAlpha(th.accent, vm.opacity));
        }
    }

    if (!vm.showKeyboardFocus || vm.focusedControlId == 0) return;
    const auto focus = ColorWithAlpha(s.currentTheme.focusRing, vm.opacity);
    const auto drawButtonFocus = [&](const D2D1_RECT_F& rect) {
        r.DrawCircle((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f, 18.0f * dp, focus, 1.5f * dp);
    };
    switch (vm.focusedControlId) {
    case 9010:
        if (vm.isQueuePlayback && !vm.isFirstQueueButtonDisabled) drawButtonFocus(layout.prev);
        break;
    case 9002: drawButtonFocus(layout.play); break;
    case 9011: drawButtonFocus(layout.next); break;
    case 9003: {
        const D2D1_RECT_F timeline = D2D1::RectF(vm.bounds.left + padX, vm.bounds.top + 8.0f * dp,
            vm.bounds.right - padX, vm.bounds.top + 24.0f * dp);
        r.DrawRoundedRect(timeline, 6.0f * dp, focus, 1.5f * dp);
        break;
    }
    case 9004: drawButtonFocus(layout.mute); break;
    case 9012:
        if (layout.showVolume) r.DrawRoundedRect(layout.volume, 7.0f * dp, focus, 1.5f * dp);
        break;
    case 9005:
        if (layout.showSubtitles) drawButtonFocus(layout.subtitles);
        break;
    case 9006: drawButtonFocus(layout.settings); break;
    case 9007:
        if (layout.showPip) drawButtonFocus(layout.pip);
        break;
    case 9008: drawButtonFocus(layout.fullscreen); break;
    default: break;
    }
}

void UIComponents::DrawPiPOverlay(OverlayRenderer& r, const Settings& s, const PiPOverlayViewModel& vm) {
    if (!vm.visible || vm.opacity <= 0.001f) return;
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    const auto layout = Presentation::PiP(RectWidth(vm.bounds), RectHeight(vm.bounds), dp);
    const auto offset = [&](D2D1_RECT_F rect) {
        rect.left += vm.bounds.left; rect.right += vm.bounds.left;
        rect.top += vm.bounds.top; rect.bottom += vm.bounds.top;
        return rect;
    };
    const D2D1_RECT_F topActions = offset(layout.topActions);
    const D2D1_RECT_F prevBtn = offset(layout.previous);
    const D2D1_RECT_F nextBtn = offset(layout.next);
    const D2D1_RECT_F audioBtn = offset(layout.audio);
    const D2D1_RECT_F pinBtn = offset(layout.pin);
    const D2D1_RECT_F closeBtn = offset(layout.close);
    const D2D1_RECT_F restoreBtn = offset(layout.restore);
    const D2D1_RECT_F playBtn = offset(layout.playPause);
    const D2D1_RECT_F trackRect = offset(layout.timeline);

    // 1. Top-left Restore Button (sleek, proportioned)
    r.FillRoundedRect(restoreBtn, 8.0f * dp, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.56f * vm.opacity));
    r.DrawRoundedRect(restoreBtn, 8.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f * vm.opacity), 1.0f);
    if (vm.hoveredButtonId == 2) {
        r.FillRoundedRect(restoreBtn, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f * vm.opacity));
    }
    r.DrawIcon(IconType::PictureInPicture, InsetRectF(restoreBtn, 7.5f * dp, 7.5f * dp), D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity));

    // 2. Top-right Action Buttons: audio, pin, close
    r.FillRoundedRect(topActions, 8.0f * dp, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.56f * vm.opacity));
    r.DrawRoundedRect(topActions, 8.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f * vm.opacity), 1.0f);

    const auto drawTopButton = [&](const D2D1_RECT_F& rect, int id, IconType icon, bool enabled, bool active = false) {
        if (vm.hoveredButtonId == id && enabled) { r.FillRoundedRect(rect, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f * vm.opacity)); }
        const float iconAlpha = enabled ? vm.opacity : 0.34f * vm.opacity;
        D2D1_COLOR_F iconColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, iconAlpha);
        if (active) {
            // Pin icon turns red when pinned
            iconColor = (id == 7) ? ColorWithAlpha(th.red, iconAlpha) : ColorWithAlpha(th.accent, iconAlpha);
        }
        float iconPad = (id == 3) ? 7.0f * dp : 5.5f * dp;
        r.DrawIcon(icon, InsetRectF(rect, iconPad, iconPad), iconColor);
    };

    IconType volIcon = IconType::VolumeHigh;
    if (vm.isMuted || vm.volume <= 0.01f) volIcon = IconType::VolumeMuted;
    else if (vm.volume < 0.5f) volIcon = IconType::VolumeLow;
    drawTopButton(audioBtn, 6, volIcon, true, vm.isVolumeSliderOpen);
    drawTopButton(pinBtn, 7, IconType::Pin, true, vm.isPinned);
    drawTopButton(closeBtn, 3, IconType::CloseX, true, false);

    // 3. YouTube-style Vertical Volume Slider with smooth opening animation
    // Drawn while the reveal animates in either direction; isVolumeSliderOpen only drives the button highlight so a closing slider can fade out.
    if (vm.volumeSliderAnim > 0.01f) {
        float anim = std::clamp(vm.volumeSliderAnim, 0.0f, 1.0f);
        D2D1_RECT_F sliderRect = offset(layout.volumeSlider);
        const float fullH = RectHeight(sliderRect);
        sliderRect.bottom = sliderRect.top + fullH * anim;

        D2D1_RECT_F trackRectV = offset(layout.volumeTrack);
        const float trackFullH = layout.volumeTrack.bottom - layout.volumeTrack.top;
        trackRectV.bottom = trackRectV.top + trackFullH * anim;

        const float sliderW = RectWidth(sliderRect);
        const float sliderCx = (sliderRect.left + sliderRect.right) * 0.5f;
        const float trackTop = trackRectV.top;
        const float trackBottom = trackRectV.bottom;
        const float trackH = trackBottom - trackTop;
        const float trackLineW = 3.0f * dp;
        const float sliderAlpha = vm.opacity * anim;

        r.FillRoundedRect(sliderRect, sliderW * 0.5f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f * sliderAlpha));
        r.DrawRoundedRect(sliderRect, sliderW * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f * sliderAlpha), 1.0f);

        if (anim > 0.25f) {
            float contentAlpha = (anim - 0.25f) / 0.75f * sliderAlpha;
            // Background vertical track (semi-transparent white)
            r.DrawLine(sliderCx, trackTop, sliderCx, trackBottom, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f * contentAlpha), trackLineW);

            // Active track (from bottom up to current volume)
            float curVol = vm.isMuted ? 0.0f : std::clamp((float)(vm.volume / kMaxVolumeRatio), 0.0f, 1.0f);
            float knobY = trackBottom - curVol * trackH;
            if (curVol > 0.001f) {
                r.DrawLine(sliderCx, knobY, sliderCx, trackBottom, ColorWithAlpha(th.accent, contentAlpha), trackLineW);
            }

            // Circular knob
            r.FillCircle(sliderCx, knobY, 5.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, contentAlpha));
        }
    }

    // 4. Bottom Controls: [Prev] [Play/Pause] [Next] on the left, then timeline, then compact timer
    const auto drawTransportBtn = [&](const D2D1_RECT_F& rect, int id, IconType icon, bool enabled) {
        if (vm.hoveredButtonId == id && enabled) { r.FillRoundedRect(rect, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f * vm.opacity)); }
        const float iconAlpha = enabled ? vm.opacity : 0.34f * vm.opacity;
        r.DrawIcon(icon, InsetRectF(rect, 5.5f * dp, 5.5f * dp), D2D1::ColorF(1.0f, 1.0f, 1.0f, iconAlpha));
    };

    drawTransportBtn(prevBtn, 4, IconType::Prev, vm.canPlayPrevious);

    if (vm.hoveredButtonId == 1) { r.FillRoundedRect(playBtn, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f * vm.opacity)); }
    r.DrawIcon(vm.isPlaying ? IconType::Pause : IconType::Play, InsetRectF(playBtn, 6.0f * dp, 6.0f * dp),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity));

    drawTransportBtn(nextBtn, 5, IconType::Next, vm.canPlayNext);

    // Transparent progress bar track (no solid black background)
    const float barH = RectHeight(trackRect);
    const float barW = RectWidth(trackRect);
    r.FillRoundedRect(trackRect, barH * 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f * vm.opacity));

    // Played fill
    double ratio = (vm.duration > 0.0) ? std::clamp(vm.currentTime / vm.duration, 0.0, 1.0) : 0.0;
    float playedW = barW * (float)ratio;
    if (playedW > 0.0f) {
        D2D1_RECT_F playedRect = D2D1::RectF(trackRect.left, trackRect.top, trackRect.left + playedW, trackRect.bottom);
        r.FillRoundedRect(playedRect, barH * 0.5f, ColorWithAlpha(th.accent, vm.opacity));
    }
    if (vm.isScrubbing) {
        const float knobX = trackRect.left + playedW;
        r.FillCircle(knobX, (trackRect.top + trackRect.bottom) * 0.5f, 3.5f * dp, ColorWithAlpha(th.accent, vm.opacity));
    }
    if (layout.showTime) {
        const std::wstring timeStr = FormatDuration(vm.currentTime);
        r.DrawString(timeStr, offset(layout.time), D2D1::ColorF(0.95f, 0.95f, 0.95f, vm.opacity),
            FontType::Caption, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (!vm.showKeyboardFocus || vm.focusedControlId == 0) return;
    const auto focus = ColorWithAlpha(s.currentTheme.focusRing, vm.opacity);
    const D2D1_RECT_F* target = nullptr;
    if (vm.focusedControlId == 9020) target = &playBtn;
    else if (vm.focusedControlId == 9021) target = &restoreBtn;
    else if (vm.focusedControlId == 9022) target = &closeBtn;
    else if (vm.focusedControlId == 9023) target = &prevBtn;
    else if (vm.focusedControlId == 9024) target = &nextBtn;
    else if (vm.focusedControlId == 9025) target = &audioBtn;
    else if (vm.focusedControlId == 9026) target = &pinBtn;
    if (target) r.DrawRoundedRect(*target, 7.0f * dp, focus, 1.5f * dp);
}

void UIComponents::DrawPlaybackDiagnostics(OverlayRenderer& r, const Settings& s, const DiagnosticsOverlayViewModel& vm) {
    if (!vm.visible) return;
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    const auto layout = Presentation::PlaybackDiagnostics(vm.bounds, dp);
    if (!layout.visible) return;
    const auto& cardRect = layout.card;
    SurfaceStyle style;
    style.radius = 8.0f * dp;
    style.fill = ColorWithAlpha(th.surfaceFloating, 0.92f);
    style.border = ColorWithAlpha(th.border, 0.70f);
    style.borderWidth = 1.0f;
    style.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f);
    style.shadowBlur = 18.0f * dp;
    style.shadowOffsetY = 6.0f * dp;
    r.DrawSurface(cardRect, style);
    // Header bar
    float headerH = 28.0f * dp;
    D2D1_RECT_F headerRect = D2D1::RectF(cardRect.left + 12.0f * dp, cardRect.top + 6.0f * dp, cardRect.right - 12.0f * dp, cardRect.top + 6.0f * dp + headerH);
    float iconSz = 14.0f * dp;
    D2D1_RECT_F iconRect = D2D1::RectF(headerRect.left, headerRect.top + (headerH - iconSz) * 0.5f, headerRect.left + iconSz, headerRect.top + (headerH + iconSz) * 0.5f);
    r.DrawIcon(IconType::SettingsGear, iconRect, th.accent, 1.4f * dp);
    const bool showShortcut = RectWidth(cardRect) >= 340.0f * dp;
    const float titleRight = layout.close.left - (showShortcut ? 75.0f : 6.0f) * dp;
    D2D1_RECT_F titleRect = D2D1::RectF(iconRect.right + 6.0f * dp, headerRect.top, titleRight, headerRect.bottom);
    r.DrawString(L"PLAYBACK DIAGNOSTICS", titleRect, th.accent, FontType::Caption, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (showShortcut) {
        D2D1_RECT_F badgeRect = D2D1::RectF(layout.close.left - 69.0f * dp, headerRect.top, layout.close.left - 4.0f * dp, headerRect.bottom);
        r.DrawString(L"[I] Toggle", badgeRect, th.textMuted, FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (vm.closeHovered) r.FillRoundedRect(layout.close, 5.0f * dp, ColorWithAlpha(th.text, 0.08f));
    r.DrawIcon(IconType::CloseX, InsetRectF(layout.close, 11.0f * dp, 11.0f * dp), vm.closeHovered ? th.text : th.textMuted, 1.4f * dp);
    r.DrawLine(cardRect.left + 10.0f * dp, cardRect.top + 34.0f * dp, cardRect.right - 10.0f * dp, cardRect.top + 34.0f * dp, ColorWithAlpha(th.borderSubtle, 0.7f), 1.0f);
    float curY = cardRect.top + 38.0f * dp;
    const float rowH = 17.5f * dp, labelW = 100.0f * dp;
    auto drawRow = [&](const wchar_t* label, const std::wstring& value, const D2D1_COLOR_F& valCol) {
        if (curY + rowH > cardRect.bottom - 4.0f * dp) return;
        D2D1_RECT_F lblRect = D2D1::RectF(cardRect.left + 12.0f * dp, curY, cardRect.left + 12.0f * dp + labelW, curY + rowH);
        D2D1_RECT_F valRect = D2D1::RectF(cardRect.left + 12.0f * dp + labelW, curY, cardRect.right - 12.0f * dp, curY + rowH);
        r.DrawString(label, lblRect, th.textMuted, FontType::Caption, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        r.DrawString(FitSingleLine(r, value, FontType::MonoSmall, SafeWidth(valRect.left, valRect.right)), valRect, valCol, FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        curY += rowH;
    };
    auto drawSection = [&](const wchar_t* title) {
        if (curY + 16.0f * dp > cardRect.bottom - 4.0f * dp) return;
        D2D1_RECT_F secRect = D2D1::RectF(cardRect.left + 12.0f * dp, curY + 2.0f * dp, cardRect.right - 12.0f * dp, curY + 16.0f * dp);
        r.DrawString(title, secRect, ColorWithAlpha(th.accent, 0.85f), FontType::Caption, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        curY += 16.0f * dp;
    };
    // 1. Media & Path
    drawSection(L"MEDIA & PLAYBACK");
    std::wstring nameVal = vm.diagnostics.fileName.empty() ? L"(No media loaded)" : vm.diagnostics.fileName;
    drawRow(L"File", nameVal, th.text);
    if (!vm.diagnostics.filePath.empty()) { drawRow(L"Path", vm.diagnostics.filePath, th.textMuted); }
    // Time & Progress
    wchar_t timeBuf[128];
    double dur = vm.diagnostics.duration, cur = vm.diagnostics.currentTime;
    if (dur > 0.0) {
        double pct = std::clamp((cur / dur) * 100.0, 0.0, 100.0);
        double rem = std::max(0.0, dur - cur);
        swprintf_s(timeBuf, L"%ls / %ls (%.1f%%, -%ls)", FormatDuration(cur).c_str(), FormatDuration(dur).c_str(), pct, FormatDuration(rem).c_str());
    } else { swprintf_s(timeBuf, L"%ls / %ls", FormatDuration(cur).c_str(), dur < 0.0 ? L"Live" : L"00:00"); }
    drawRow(L"Position", timeBuf, th.text);
    // State & Rate
    wchar_t stateBuf[128];
    const wchar_t* stateStr = vm.diagnostics.isEnded ? L"Ended" : (vm.diagnostics.isPaused ? L"Paused" : (vm.diagnostics.isPlaying ? L"Playing" : L"Stopped"));
    swprintf_s(stateBuf, L"%ls | Speed: %.2fx | Loop: %ls", stateStr, vm.diagnostics.speed, vm.diagnostics.isLoop ? L"On" : L"Off");
    drawRow(L"State", stateBuf, vm.diagnostics.isPlaying ? th.accent : th.textMuted);
    // Volume & Delays
    wchar_t volBuf[128];
    int volPct = (int)std::round(vm.diagnostics.volume * 100.0);
    swprintf_s(volBuf, L"%d%% (%ls) | Audio: %+d ms | Sub: %+d ms", volPct, vm.diagnostics.isMuted ? L"Muted" : L"Unmuted", vm.audioDelayMs, vm.subDelayMs);
    drawRow(L"Audio/Sync", volBuf, vm.diagnostics.isMuted ? th.red : th.text);
    // 2. Video & Decoder
    drawSection(L"VIDEO & DECODER");
    wchar_t resBuf[128];
    if (vm.diagnostics.videoWidth > 0 && vm.diagnostics.videoHeight > 0) {
        std::wstring resTag = vm.diagnostics.resolutionStr.empty() ? L"" : (L" (" + vm.diagnostics.resolutionStr + L")");
        swprintf_s(resBuf, L"%ux%u%ls", vm.diagnostics.videoWidth, vm.diagnostics.videoHeight, resTag.c_str());
    } else { swprintf_s(resBuf, L"Audio Only / No Video"); }
    drawRow(L"Resolution", resBuf, th.text);
    wchar_t vCodecBuf[128];
    std::wstring vCodec = vm.diagnostics.videoCodec.empty() ? L"Unknown" : vm.diagnostics.videoCodec;
    std::wstring pixFmt = vm.diagnostics.pixelFormat.empty() ? L"" : (L" [" + vm.diagnostics.pixelFormat + L"]");
    swprintf_s(vCodecBuf, L"%ls%ls", vCodec.c_str(), pixFmt.c_str());
    drawRow(L"Video Codec", vCodecBuf, th.text);
    wchar_t decBuf[128];
    const auto& hw = vm.diagnostics.hwDec;
    swprintf_s(decBuf, L"HW: %ls", hw.empty() ? L"N/A" : hw.c_str());
    drawRow(L"Decoder", decBuf, !hw.empty() && hw != L"no" ? th.accent : th.textMuted);
    wchar_t fpsBuf[128];
    if (vm.diagnostics.videoFps > 0.0) {
        if (vm.diagnostics.displayFps > 0.0) {
            swprintf_s(fpsBuf, L"%.3f fps (Display: %.1f Hz)", vm.diagnostics.videoFps, vm.diagnostics.displayFps);
        } else { swprintf_s(fpsBuf, L"%.3f fps", vm.diagnostics.videoFps); }
    } else { swprintf_s(fpsBuf, L"N/A"); }
    drawRow(L"Frame Rate", fpsBuf, th.text);
    // 3. Performance & Audio Tracks
    drawSection(L"PERFORMANCE & AUDIO");
    wchar_t perfBuf[128];
    const auto frameCount = [](int64_t value) { return value < 0 ? std::wstring(L"N/A") : std::to_wstring(value); };
    swprintf_s(perfBuf, L"Drops: %ls decoder, %ls vo, %ls mistimed", frameCount(vm.diagnostics.droppedFrames).c_str(), frameCount(vm.diagnostics.voDroppedFrames).c_str(), frameCount(vm.diagnostics.mistimedFrames).c_str());
    drawRow(L"Render Drops", perfBuf, (vm.diagnostics.droppedFrames > 0 || vm.diagnostics.voDroppedFrames > 0) ? th.red : th.textMuted);
    wchar_t bitrBuf[128];
    swprintf_s(bitrBuf, L"Video: %.0f kbps | Audio: %.0f kbps", vm.diagnostics.videoBitrate, vm.diagnostics.audioBitrate);
    drawRow(L"Bitrates", bitrBuf, th.text);
    wchar_t cacheBuf[128];
    double mb = (double)vm.diagnostics.cacheUsedBytes / (1024.0 * 1024.0);
    if (mb > 0.01) {
        swprintf_s(cacheBuf, L"%.1f s (%.1f MB) | AV Sync: %+.3f s", vm.diagnostics.cacheDuration, mb, vm.diagnostics.avSync);
    } else { swprintf_s(cacheBuf, L"%.1f s | AV Sync: %+.3f s", vm.diagnostics.cacheDuration, vm.diagnostics.avSync); }
    drawRow(L"Demux Cache", cacheBuf, th.text);
    if (!vm.diagnostics.audioCodec.empty() || vm.diagnostics.audioSampleRate > 0) {
        wchar_t audBuf[128];
        std::wstring aCodec = vm.diagnostics.audioCodec.empty() ? L"Audio" : vm.diagnostics.audioCodec;
        if (vm.diagnostics.audioSampleRate > 0 && vm.diagnostics.audioChannels > 0) {
            swprintf_s(audBuf, L"%ls (%lld Hz, %lld ch) %ls", aCodec.c_str(), (long long)vm.diagnostics.audioSampleRate, (long long)vm.diagnostics.audioChannels, vm.diagnostics.audioTrackTitle.c_str());
        } else { swprintf_s(audBuf, L"%ls %ls", aCodec.c_str(), vm.diagnostics.audioTrackTitle.c_str()); }
        drawRow(L"Audio Track", audBuf, th.text);
    }
    if (!vm.diagnostics.subTrackTitle.empty()) {
        wchar_t subBuf[128];
        swprintf_s(subBuf, L"%ls (Safe Area: %d%% H, %d%% V)", vm.diagnostics.subTrackTitle.c_str(), vm.diagnostics.activeSubSafeAreaX, vm.diagnostics.activeSubSafeAreaY);
        drawRow(L"Subtitle Track", subBuf, vm.diagnostics.subTrackTitle != L"Off" ? th.text : th.textMuted);
    } else if (vm.diagnostics.activeSubSafeAreaX > 0 || vm.diagnostics.activeSubSafeAreaY > 0) {
        wchar_t subSafeBuf[64];
        swprintf_s(subSafeBuf, L"%d%% H, %d%% V", vm.diagnostics.activeSubSafeAreaX, vm.diagnostics.activeSubSafeAreaY);
        drawRow(L"Sub Safe Area", subSafeBuf, th.accent);
    }
    if (!vm.diagnostics.activeAudioDevice.empty()) {
        std::wstring devStr = vm.diagnostics.activeAudioDeviceDescription.empty() ? vm.diagnostics.activeAudioDevice : vm.diagnostics.activeAudioDeviceDescription;
        if (vm.diagnostics.audioDeviceFallback) devStr += L" (Fallback: Auto)";
        drawRow(L"Audio Device", devStr, vm.diagnostics.audioDeviceFallback ? th.textMuted : th.text);
    }
    if (!vm.diagnostics.activeAudioLayout.empty() || !vm.diagnostics.activeAudioChannelMix.empty()) {
        std::wstring layoutStr = vm.diagnostics.activeAudioLayout.empty() ? L"auto-safe" : vm.diagnostics.activeAudioLayout;
        if (!vm.diagnostics.activeAudioChannelMix.empty() && vm.diagnostics.activeAudioChannelMix != L"auto")
            layoutStr += L" (" + vm.diagnostics.activeAudioChannelMix + L")";
        drawRow(L"Audio Layout", layoutStr, th.text);
    }
    if (!vm.diagnostics.activeNightMode.empty() && vm.diagnostics.activeNightMode != L"off")
        drawRow(L"Night Mode (DRC)", vm.diagnostics.activeNightMode, th.accent);
    if (std::abs(vm.diagnostics.activeAudioBalance) > 0.001) {
        wchar_t balBuf[64];
        if (vm.diagnostics.activeAudioBalance < -0.001) {
            swprintf_s(balBuf, L"Left %.0f%%", -vm.diagnostics.activeAudioBalance * 100.0);
        } else { swprintf_s(balBuf, L"Right %.0f%%", vm.diagnostics.activeAudioBalance * 100.0); }
        drawRow(L"Audio Balance", balBuf, th.accent);
    }
    if (!vm.diagnostics.activeAudioChannelMap.empty() && vm.diagnostics.activeAudioChannelMap != L"auto")
        drawRow(L"Channel Mapping", vm.diagnostics.activeAudioChannelMap, th.accent);
    // 4. Color & HDR Management
    drawSection(L"COLOR & HDR");
    std::wstring prim = vm.diagnostics.colorPrimaries.empty() ? L"BT.709" : vm.diagnostics.colorPrimaries;
    std::wstring tr = vm.diagnostics.colorTransfer.empty() ? L"sRGB / SDR" : vm.diagnostics.colorTransfer;
    std::wstring colorStr = prim + L" / " + tr + (vm.diagnostics.isHdrSource ? (L" (" + vm.diagnostics.hdrType + L")") : L" (SDR)");
    drawRow(L"Source Color", colorStr, vm.diagnostics.isHdrSource ? th.accent : th.text);
    wchar_t peakBuf[128];
    if (vm.diagnostics.maxCll > 0.0 || vm.diagnostics.maxFall > 0.0) {
        swprintf_s(peakBuf, L"MaxCLL: %.0f nits | MaxFALL: %.0f nits", vm.diagnostics.maxCll, vm.diagnostics.maxFall);
    } else if (vm.diagnostics.sigPeak > 1.05) {
        swprintf_s(peakBuf, L"Signal Peak: %.1fx (%.0f nits)", vm.diagnostics.sigPeak, vm.diagnostics.sigPeak * 100.0);
    } else { swprintf_s(peakBuf, L"Standard SDR (100 nits)"); }
    drawRow(L"Mastering Peak", peakBuf, th.text);
    drawRow(L"Output Mode", vm.diagnostics.activeOutputMode, vm.diagnostics.isHdrSource ? th.accent : th.textMuted);
    if (!vm.diagnostics.activeToneMapping.empty()) { drawRow(L"Tone Mapping", vm.diagnostics.activeToneMapping, th.text); }
    if (!vm.diagnostics.activeIccProfile.empty()) { drawRow(L"ICC Profile", vm.diagnostics.activeIccProfile, th.text); }
}

// In-app diagnostics log viewer: a style clone of the playback-diagnostics
// panel (same floating surface, header, and close affordance) with a scrolless
// list of the last log lines in the monospace font.
void UIComponents::DrawLogOverlay(OverlayRenderer& r, const Settings& s, const LogOverlayViewModel& vm) {
    if (!vm.visible) return;
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    const auto layout = Presentation::LogViewer(vm.bounds, dp);
    if (!layout.visible) return;
    const auto& cardRect = layout.card;
    SurfaceStyle style;
    style.radius = 8.0f * dp;
    style.fill = ColorWithAlpha(th.surfaceFloating, 0.92f);
    style.border = ColorWithAlpha(th.border, 0.70f);
    style.borderWidth = 1.0f;
    style.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f);
    style.shadowBlur = 18.0f * dp;
    style.shadowOffsetY = 6.0f * dp;
    r.DrawSurface(cardRect, style);
    // Header bar
    float headerH = 28.0f * dp;
    D2D1_RECT_F headerRect = D2D1::RectF(cardRect.left + 12.0f * dp, cardRect.top + 6.0f * dp, cardRect.right - 12.0f * dp, cardRect.top + 6.0f * dp + headerH);
    float iconSz = 14.0f * dp;
    D2D1_RECT_F iconRect = D2D1::RectF(headerRect.left, headerRect.top + (headerH - iconSz) * 0.5f, headerRect.left + iconSz, headerRect.top + (headerH + iconSz) * 0.5f);
    r.DrawIcon(IconType::SettingsGear, iconRect, th.accent, 1.4f * dp);
    D2D1_RECT_F titleRect = D2D1::RectF(iconRect.right + 6.0f * dp, headerRect.top, layout.close.left - 6.0f * dp, headerRect.bottom);
    r.DrawString(L"DIAGNOSTICS LOG", titleRect, th.accent, FontType::Caption, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (vm.closeHovered) r.FillRoundedRect(layout.close, 5.0f * dp, ColorWithAlpha(th.text, 0.08f));
    r.DrawIcon(IconType::CloseX, InsetRectF(layout.close, 11.0f * dp, 11.0f * dp), vm.closeHovered ? th.text : th.textMuted, 1.4f * dp);
    r.DrawLine(cardRect.left + 10.0f * dp, cardRect.top + 34.0f * dp, cardRect.right - 10.0f * dp, cardRect.top + 34.0f * dp, ColorWithAlpha(th.borderSubtle, 0.7f), 1.0f);
    float curY = cardRect.top + 40.0f * dp;
    const float rowH = 16.0f * dp;
    for (const auto& line : vm.lines) {
        if (curY + rowH > cardRect.bottom - 6.0f * dp) break;
        D2D1_RECT_F lineRect = D2D1::RectF(cardRect.left + 12.0f * dp, curY, cardRect.right - 12.0f * dp, curY + rowH);
        r.DrawString(FitSingleLine(r, line, FontType::MonoSmall, SafeWidth(lineRect.left, lineRect.right)),
                     lineRect, th.text, FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        curY += rowH;
    }
}
void UIComponents::DrawSettingsFlyout(OverlayRenderer& r, const Settings& s, const SettingsFlyoutViewModel& vm) {
    if (!vm.isOpen) return;
    const float alpha = std::clamp(vm.animProgress, 0.0f, 1.0f);
    if (alpha <= 0.001f) return;
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    const D2D1_RECT_F flyoutRect = vm.bounds;
    SurfaceStyle mStyle;
    mStyle.radius = 10.0f * dp;
    mStyle.fill = ColorWithAlpha(th.surfaceFloating, 0.98f * alpha);
    mStyle.border = ColorWithAlpha(th.border, 0.75f * alpha);
    mStyle.borderWidth = 1.0f;
    mStyle.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    mStyle.shadowBlur = 0.0f;
    mStyle.hasInnerHighlight = false;
    r.DrawSurface(flyoutRect, mStyle);
    const float inset = 4.0f * dp, rowH = 40.0f * dp, rowGap = 2.0f * dp, headerH = 36.0f * dp;
    float curY = flyoutRect.top + 8.0f * dp;
    auto drawBackHeader = [&](const wchar_t* title) {
        D2D1_RECT_F header = PlaybackFlyoutLayout::Header(flyoutRect, dp);
        bool backHover = (vm.hoveredOptionIndex == PlaybackFlyoutLayout::Back);
        if (backHover) { r.FillRoundedRect(header, 6.0f * dp, ColorWithAlpha(th.accent, 0.12f * alpha)); }
        float iconSz = 14.0f * dp;
        D2D1_RECT_F backRect = D2D1::RectF(header.left + 8.0f * dp, header.top + (headerH - iconSz) * 0.5f, header.left + 8.0f * dp + iconSz, header.top + (headerH + iconSz) * 0.5f);
        r.DrawIcon(IconType::ChevronLeft, backRect, backHover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.textMuted, alpha), 1.5f * dp);
        D2D1_COLOR_F titleCol = backHover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.text, alpha);
        r.DrawString(FitSingleLine(r, title, FontType::Medium, SafeWidth(header.left + 30.0f * dp, header.right - 8.0f * dp)),
                     D2D1::RectF(header.left + 30.0f * dp, header.top, header.right - 8.0f * dp, header.bottom),
                     titleCol, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        curY += headerH + 6.0f * dp;
    };
    auto drawHoverRow = [&](const D2D1_RECT_F& rect, bool hover, bool selected) {
        if (hover) {
            r.FillRoundedRect(rect, 6.0f * dp, ColorWithAlpha(th.accent, 0.12f * alpha));
        } else if (selected) { r.FillRoundedRect(rect, 6.0f * dp, ColorWithAlpha(th.accent, 0.08f * alpha)); }
    };
    auto drawCheck = [&](const D2D1_RECT_F& rect, bool selected) {
        if (selected) { r.DrawIcon(IconType::Checkmark, rect, ColorWithAlpha(th.accent, alpha), 1.5f * dp); }
    };
    if (vm.currentMenu == "main") {
        // Keep this order stable: App's flyout hit map intentionally uses these indices. The order mirrors the WebView settings menu.
        struct MenuItem { std::wstring label; std::wstring value; IconType icon; };
        std::wstring subVal = (vm.activeSubIndex >= 0 && vm.activeSubIndex < (int)vm.subTracks.size()) ? vm.subTracks[vm.activeSubIndex].title : L"Off";
        std::wstring audVal = (vm.activeAudioIndex >= 0 && vm.activeAudioIndex < (int)vm.audioTracks.size()) ? vm.audioTracks[vm.activeAudioIndex].title : L"Default";
        wchar_t speedBuf[32];
        swprintf_s(speedBuf, L"%.2fx", vm.currentSpeed);
        const MenuItem items[] = {
            { L"Subtitles / CC", subVal, IconType::Subtitles }, { L"Audio Track", audVal, IconType::VolumeHigh },
            { L"Playback Speed", vm.currentSpeed == 1.0 ? L"Normal" : speedBuf, IconType::SpeedGauge },
            { L"Aspect Ratio", Utf8ToUtf16(vm.currentAspect), IconType::AspectFrame },
            { L"Queue", vm.QueueItems().empty() ? L"Empty" : (std::to_wstring(std::min(vm.queueIndex + 1, vm.QueueItems().size())) + L" / " + std::to_wstring(vm.QueueItems().size())), IconType::FolderPlaylist },
            { L"Loop Video", vm.isLoop ? L"On" : L"Off", IconType::LoopRepeat },
            { L"Autoplay", vm.isAutoplay ? L"On" : L"Off", IconType::AutoplayNext },
            {L"Private Session", vm.isPrivateMode ? L"On" : L"Off", IconType::SettingsGear}};
        for (int i = 0; i < 8; ++i) {
            D2D1_RECT_F row = D2D1::RectF(flyoutRect.left + inset, curY, flyoutRect.right - inset, curY + rowH);
            const bool hover = (vm.hoveredOptionIndex == i);
            drawHoverRow(row, hover, false);
            float iconSz = 14.0f * dp;
            D2D1_RECT_F icon = D2D1::RectF(row.left + 8.0f * dp, row.top + (rowH - iconSz) * 0.5f, row.left + 8.0f * dp + iconSz, row.top + (rowH + iconSz) * 0.5f);
            r.DrawIcon(items[i].icon, icon, hover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.textMuted, alpha), 1.5f * dp);
            const bool directToggle = (i >= 5);
            float valueW = directToggle ? 48.0f : 78.0f;
            D2D1_RECT_F label = D2D1::RectF(icon.right + 8.0f * dp, row.top, row.right - valueW - (directToggle ? 48.0f : 24.0f) * dp, row.bottom);
            D2D1_RECT_F value = D2D1::RectF(row.right - valueW - (directToggle ? 44.0f : 16.0f) * dp, row.top, row.right - (directToggle ? 48.0f : 16.0f) * dp, row.bottom);
            D2D1_COLOR_F labelCol = hover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.text, alpha);
            D2D1_COLOR_F valCol = hover ? ColorWithAlpha(th.accent, 0.85f * alpha) : ColorWithAlpha(th.textMuted, alpha);
            r.DrawString(FitSingleLine(r, items[i].label, FontType::Small, SafeWidth(label.left, label.right)), label, labelCol, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            r.DrawString(FitSingleLine(r, items[i].value, FontType::Caption, SafeWidth(value.left, value.right)), value, valCol, FontType::Caption, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (directToggle) {
                const bool enabled = (i == 5 ? vm.isLoop : (i == 6 ? vm.isAutoplay : vm.isPrivateMode));
                const float switchW = 30.0f * dp, switchH = 16.0f * dp, switchLeft = row.right - 38.0f * dp;
                const float switchTop = row.top + (rowH - switchH) * 0.5f;
                D2D1_RECT_F switchRect = D2D1::RectF(switchLeft, switchTop, switchLeft + switchW, switchTop + switchH);
                r.FillRoundedRect(switchRect, switchH * 0.5f, enabled ? ColorWithAlpha(th.accent, 0.86f * alpha) : ColorWithAlpha(th.trackBg, alpha));
                r.DrawRoundedRect(switchRect, switchH * 0.5f, enabled ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.border, alpha), 1.0f);
                const float knobRadius = 5.0f * dp, knobX = enabled ? switchRect.right - 8.0f * dp : switchRect.left + 8.0f * dp;
                r.FillCircle(knobX, switchTop + switchH * 0.5f, knobRadius, enabled ? D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha) : ColorWithAlpha(th.text, 0.82f * alpha));
            } else {
                float chevSz = 12.0f * dp;
                D2D1_RECT_F chevRect = D2D1::RectF(row.right - 16.0f * dp, row.top + (rowH - chevSz) * 0.5f, row.right - 16.0f * dp + chevSz, row.top + (rowH + chevSz) * 0.5f);
                r.DrawIcon(IconType::ChevronRight, chevRect, hover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.textMuted, alpha), 1.3f * dp);
            }
            // Hairline separator between submenus and toggles matching context menu separator style
            if (i == 4)
                r.DrawLine(flyoutRect.left + 10.0f * dp, row.bottom + rowGap * 0.5f, flyoutRect.right - 10.0f * dp, row.bottom + rowGap * 0.5f, ColorWithAlpha(th.borderSubtle, alpha), 1.0f);
            curY += rowH + rowGap;
        }
    } else if (vm.currentMenu == "queue") {
        drawBackHeader(L"Queue");
        if (vm.QueueItems().empty()) {
            r.DrawString(L"Queue is empty", D2D1::RectF(flyoutRect.left + 14.0f * dp, curY + 18.0f * dp,
                flyoutRect.right - 14.0f * dp, curY + 54.0f * dp), ColorWithAlpha(th.textMuted, alpha),
                FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else {
            D2D1_RECT_F status = D2D1::RectF(flyoutRect.left + 14.0f * dp, curY, flyoutRect.right - 14.0f * dp, curY + 24.0f * dp);
            r.DrawString(L"Playing " + std::to_wstring(std::min(vm.queueIndex + 1, vm.QueueItems().size())) + L" of " + std::to_wstring(vm.QueueItems().size()),
                status, ColorWithAlpha(th.textMuted, alpha), FontType::Caption,
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            curY += 28.0f * dp;
            constexpr size_t kVisibleQueueRows = 7;
            const size_t first = std::min(vm.queueFirstVisible, vm.QueueItems().size() - 1);
            const size_t end = std::min(vm.QueueItems().size(), first + kVisibleQueueRows);
            for (size_t i = first; i < end; ++i) {
                const int visibleIndex = static_cast<int>(i - first);
                D2D1_RECT_F row = D2D1::RectF(flyoutRect.left + inset, curY, flyoutRect.right - inset, curY + 34.0f * dp);
                const bool selected = i == vm.queueIndex, hover = vm.hoveredOptionIndex == visibleIndex;
                drawHoverRow(row, hover, selected);
                const D2D1_COLOR_F textColor = selected ? ColorWithAlpha(th.accent, alpha)
                    : (hover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.text, alpha));
                const std::wstring title = GetFileTitleFromPath(vm.QueueItems()[i]);
                if (selected)
                    r.FillCircle(row.left + 10.0f * dp, (row.top + row.bottom) * 0.5f, 2.5f * dp, ColorWithAlpha(th.accent, alpha));
                r.DrawString(FitSingleLine(r, title, FontType::Small, SafeWidth(row.left + 18.0f * dp, row.right - 12.0f * dp)),
                    D2D1::RectF(row.left + 18.0f * dp, row.top, row.right - 12.0f * dp, row.bottom),
                    textColor, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                curY += 36.0f * dp;
            }
        }
    } else if (vm.currentMenu == "speed") {
        drawBackHeader(L"Playback Speed");
        const auto& speeds = PlaybackFlyoutLayout::Speeds;
        const wchar_t* speedLabels[] = { L"0.25x", L"0.5x", L"0.75x", L"1x", L"1.25x", L"1.5x", L"1.75x", L"2x", L"3x" };
        wchar_t speedDisplay[32];
        swprintf_s(speedDisplay, L"%.2fx", vm.currentSpeed);
        D2D1_RECT_F display = D2D1::RectF(flyoutRect.left + 14.0f * dp, curY, flyoutRect.right - 14.0f * dp, curY + 32.0f * dp);
        if (vm.speedInput.focused) {
            const auto editor = PlaybackFlyoutLayout::SpeedInput(flyoutRect, dp);
            r.FillRoundedRect(editor, 5 * dp, ColorWithAlpha(th.surfaceElevated, alpha));
            r.DrawRoundedRect(editor, 5 * dp, ColorWithAlpha(th.accent, alpha), dp);
            DrawTextInput(r, s, editor, vm.speedInput, L"", FontType::Title, 10 * dp);
        } else r.DrawString(speedDisplay, display, ColorWithAlpha(th.text, alpha), FontType::Hero, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        curY += 37.0f * dp;
        float sliderLeft = flyoutRect.left + 24.0f * dp, sliderRight = flyoutRect.right - 24.0f * dp, sliderY = curY + 8.0f * dp;
        r.DrawLine(sliderLeft, sliderY, sliderRight, sliderY, ColorWithAlpha(th.trackBg, alpha), 4.0f * dp);
        float sliderT = (float)std::clamp((vm.currentSpeed - 0.25) / (3.0 - 0.25), 0.0, 1.0);
        float sliderX = sliderLeft + (sliderRight - sliderLeft) * sliderT;
        r.DrawLine(sliderLeft, sliderY, sliderX, sliderY, ColorWithAlpha(th.accent, alpha), 4.0f * dp);
        r.FillCircle(sliderX, sliderY, 6.0f * dp, ColorWithAlpha(th.accent, alpha));
        r.DrawCircle(sliderX, sliderY, 7.5f * dp, ColorWithAlpha(th.border, 0.6f * alpha), 1.0f * dp);
        curY += 21.0f * dp;
        for (int i = 0; i < 9; ++i) {
            D2D1_RECT_F pill = PlaybackFlyoutLayout::Option("speed", i, flyoutRect, dp);
            bool selected = std::abs(vm.currentSpeed - speeds[i]) < 0.01;
            bool hover = (vm.hoveredOptionIndex == i);
            drawHoverRow(pill, hover, selected);
            D2D1_COLOR_F pillTextCol = selected ? ColorWithAlpha(th.accent, alpha) : (hover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.text, alpha));
            r.DrawString(speedLabels[i], pill, pillTextCol, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    } else if (vm.currentMenu == "aspect") {
        drawBackHeader(L"Aspect Ratio");
        const char* aspects[] = { "auto", "16:9", "4:3", "21:9", "fill", "cover" };
        const wchar_t* labels[] = { L"Auto (Original)", L"16:9 Widescreen", L"4:3 Standard", L"21:9 Ultrawide", L"Stretch to Fill", L"Zoom / Crop" };
        for (int i = 0; i < 6; ++i) {
            D2D1_RECT_F row = D2D1::RectF(flyoutRect.left + inset, curY, flyoutRect.right - inset, curY + 34.0f * dp);
            bool selected = (vm.currentAspect == aspects[i]);
            bool hover = (vm.hoveredOptionIndex == i);
            drawHoverRow(row, hover, selected);
            D2D1_COLOR_F rowTextCol = selected ? ColorWithAlpha(th.accent, alpha) : (hover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.text, alpha));
            r.DrawString(FitSingleLine(r, labels[i], FontType::Small, SafeWidth(row.left + 14.0f * dp, row.right - 30.0f * dp)),
                         D2D1::RectF(row.left + 14.0f * dp, row.top, row.right - 30.0f * dp, row.bottom),
                         rowTextCol, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            float checkSz = 14.0f * dp;
            D2D1_RECT_F checkRect = D2D1::RectF(row.right - 24.0f * dp, row.top + (34.0f * dp - checkSz) * 0.5f, row.right - 24.0f * dp + checkSz, row.top + (34.0f * dp + checkSz) * 0.5f);
            drawCheck(checkRect, selected);
            curY += 36.0f * dp;
        }
    } else if (vm.currentMenu == "subtitles") {
        drawBackHeader(L"Subtitles / CC");
        D2D1_RECT_F off = D2D1::RectF(flyoutRect.left + inset, curY, flyoutRect.right - inset, curY + 34.0f * dp);
        bool offSelected = (vm.activeSubIndex < 0);
        bool offHover = (vm.hoveredOptionIndex == 0);
        drawHoverRow(off, offHover, offSelected);
        D2D1_COLOR_F offTextCol = offSelected ? ColorWithAlpha(th.accent, alpha) : (offHover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.text, alpha));
        r.DrawString(L"Off", D2D1::RectF(off.left + 14.0f * dp, off.top, off.right - 30.0f * dp, off.bottom),
                     offTextCol, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        float checkSz = 14.0f * dp;
        D2D1_RECT_F checkRect = D2D1::RectF(off.right - 24.0f * dp, off.top + (34.0f * dp - checkSz) * 0.5f, off.right - 24.0f * dp + checkSz, off.top + (34.0f * dp + checkSz) * 0.5f);
        drawCheck(checkRect, offSelected);
        curY += 36.0f * dp;
        for (size_t i = 0; i < vm.subTracks.size(); ++i) {
            D2D1_RECT_F row = D2D1::RectF(flyoutRect.left + inset, curY, flyoutRect.right - inset, curY + 34.0f * dp);
            bool selected = ((int)i == vm.activeSubIndex);
            bool hover = (vm.hoveredOptionIndex == (int)i + 1);
            drawHoverRow(row, hover, selected);
            D2D1_COLOR_F rowTextCol = selected ? ColorWithAlpha(th.accent, alpha) : (hover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.text, alpha));
            r.DrawString(FitSingleLine(r, vm.subTracks[i].title, FontType::Small, SafeWidth(row.left + 14.0f * dp, row.right - 30.0f * dp)),
                         D2D1::RectF(row.left + 14.0f * dp, row.top, row.right - 30.0f * dp, row.bottom),
                         rowTextCol, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            D2D1_RECT_F rowCheckRect = D2D1::RectF(row.right - 24.0f * dp, row.top + (34.0f * dp - checkSz) * 0.5f, row.right - 24.0f * dp + checkSz, row.top + (34.0f * dp + checkSz) * 0.5f);
            drawCheck(rowCheckRect, selected);
            curY += 36.0f * dp;
        }
    } else if (vm.currentMenu == "audio") {
        drawBackHeader(L"Audio Track");
        if (vm.audioTracks.empty()) {
            r.DrawString(L"No alternate audio tracks", D2D1::RectF(flyoutRect.left + 14.0f * dp, curY + 8.0f * dp, flyoutRect.right - 14.0f * dp, curY + 38.0f * dp),
                         ColorWithAlpha(th.textMuted, alpha), FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else {
            float checkSz = 14.0f * dp;
            for (size_t i = 0; i < vm.audioTracks.size(); ++i) {
                D2D1_RECT_F row = D2D1::RectF(flyoutRect.left + inset, curY, flyoutRect.right - inset, curY + 34.0f * dp);
                bool selected = ((int)i == vm.activeAudioIndex);
                bool hover = (vm.hoveredOptionIndex == (int)i);
                drawHoverRow(row, hover, selected);
                D2D1_COLOR_F rowTextCol = selected ? ColorWithAlpha(th.accent, alpha) : (hover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.text, alpha));
                r.DrawString(FitSingleLine(r, vm.audioTracks[i].title, FontType::Small, SafeWidth(row.left + 14.0f * dp, row.right - 30.0f * dp)),
                             D2D1::RectF(row.left + 14.0f * dp, row.top, row.right - 30.0f * dp, row.bottom),
                             rowTextCol, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F rowCheckRect = D2D1::RectF(row.right - 24.0f * dp, row.top + (34.0f * dp - checkSz) * 0.5f, row.right - 24.0f * dp + checkSz, row.top + (34.0f * dp + checkSz) * 0.5f);
                drawCheck(rowCheckRect, selected);
                curY += 36.0f * dp;
            }
        }
    }
}
template<bool DrawShadow, class R>
static void DrawOSDBadgeImpl( R& r, const Settings& s, const D2D1_RECT_F& bounds, IconType icon, const std::wstring& text, float opacity, bool compact ) {
    if (opacity <= 0.0f) return;
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    FontType font = compact ? FontType::Small : FontType::Bold;
    D2D1_SIZE_F textSize = r.MeasureString(text, font);
    float textW = std::ceil(textSize.width) + (compact ? 4.0f : 6.0f) * dp;
    float iconW = (compact ? 12.0f : 18.0f) * dp;
    float gap = (compact ? 6.0f : 10.0f) * dp;
    float padH = (compact ? 10.0f : 18.0f) * dp;
    float totalW = iconW + gap + textW + padH * 2.0f;
    float pillH = (compact ? 26.0f : 40.0f) * dp;
    float cx = (bounds.left + bounds.right) * 0.5f;
    float pillTop = bounds.top + (compact ? 10.0f : 24.0f) * dp;
    D2D1_RECT_F pillRect = D2D1::RectF(cx - totalW * 0.5f, pillTop, cx + totalW * 0.5f, pillTop + pillH);
    if constexpr (DrawShadow) r.DrawCardShadow(pillRect, pillH * 0.5f, compact ? 3.0f : 6.0f);
    r.FillRoundedRect(pillRect, pillH * 0.5f, ColorWithAlpha(th.surfaceFloating, (compact ? 0.88f : 0.94f) * opacity));
    r.DrawRoundedRect(pillRect, pillH * 0.5f, ColorWithAlpha(th.border, 0.65f * opacity), 1.0f);
    float iconX = pillRect.left + padH;
    D2D1_RECT_F iconRect = D2D1::RectF(iconX, pillTop + (pillH - iconW) * 0.5f, iconX + iconW, pillTop + (pillH + iconW) * 0.5f);
    r.DrawIcon(icon, iconRect, ColorWithAlpha(th.accent, opacity));
    D2D1_RECT_F textRect = D2D1::RectF(iconX + iconW + gap, pillTop, pillRect.right - padH, pillTop + pillH);
    r.DrawString(text, textRect, D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity), font, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void UIComponents::DrawOSDBadge(Renderer& r, const Settings& s,
    const D2D1_RECT_F& bounds, IconType icon, const std::wstring& text, float opacity, bool compact) {
    DrawOSDBadgeImpl<true>(r, s, bounds, icon, text, opacity, compact);
}

void UIComponents::DrawOSDBadge(OverlayRenderer& r, const Settings& s,
    const D2D1_RECT_F& bounds, IconType icon, const std::wstring& text, float opacity, bool compact) {
    DrawOSDBadgeImpl<false>(r, s, bounds, icon, text, opacity, compact);
}

namespace {
// One badge (sign + accumulated seconds + chevron) drawn 1:1 from youtube_double_seek.html: 26/28px bold text, 16x24 chevron with a 4.5px
// round-cap stroke, entrance scale/fade, exit drift, and per-press pops.
float SeekPopScale(float progress, float peak, const CubicBezier& curve) {
    progress = std::clamp(progress, 0.0f, 1.0f);
    // Delay onset by ~2 frames (~35ms at 60Hz) so holding the button doesn't trigger rapid pulsating
    constexpr float kDelay = 0.14f;
    if (progress <= kDelay || progress >= 1.0f) return 1.0f;
    float normalized = (progress - kDelay) / (1.0f - kDelay);
    float wave = normalized <= 0.45f ? curve.Ease(normalized / 0.45f) : 1.0f - curve.Ease((normalized - 0.45f) / 0.55f);
    wave = std::clamp(wave, 0.0f, 1.0f);
    return 1.0f + (peak - 1.0f) * wave;
}

D2D1_RECT_F ScaleRectFromCenter(const D2D1_RECT_F& rect, float scale) {
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float halfW = RectWidth(rect) * 0.5f * scale;
    const float halfH = RectHeight(rect) * 0.5f * scale;
    return D2D1::RectF(cx - halfW, cy - halfH, cx + halfW, cy + halfH);
}

void DrawSeekBadge( OverlayRenderer& r, const D2D1_RECT_F& bounds, bool isForward, double seconds, float alpha,
    float appear,     // 0..1 entrance
    float fadeOut,    // 0..1 exit progress
    const std::vector<float>& chevronSlides, float signPop, float numberPop, bool isPiP = false ) {
    const float dp = r.GetDpiScale();
    // Entrance: scale 0.92 -> 1 over 0.16s. Exit: scale -> 0.94 and drift
    // 8px toward the seek direction over 0.22s.
    const float enterEase = kEaseEnter.Ease(std::clamp(appear, 0.0f, 1.0f));
    const float exitEase = kEaseExit.Ease(std::clamp(fadeOut, 0.0f, 1.0f));
    const float groupScale = (0.92f + 0.08f * enterEase) * (1.0f - 0.06f * exitEase);
    const float driftX = (isForward ? 8.0f : -8.0f) * dp * exitEase;
    const float groupAlpha = alpha * enterEase * (1.0f - exitEase);
    if (groupAlpha <= 0.001f) return;
    // Press pops: slightly weaker (subtle ~3-3.5% scale) with delayed onset so held repeats stay rock-solid and stable
    const float signScale = SeekPopScale(signPop, 1.035f, kEasePop);
    const float numScale = SeekPopScale(numberPop, 1.030f, kEasePop);
    // Vertically center on the video player bounds.
    float cy = bounds.top + RectHeight(bounds) * 0.5f;
    const float edgeInset = isPiP ? 16.0f * dp : std::min(RectWidth(bounds) * 0.18f, 104.0f * dp);
    const float signW = (isPiP ? 12.0f : 20.0f) * dp, numMinW = (isPiP ? 18.0f : 32.0f) * dp;
    const float signNumGap = (isPiP ? 3.0f : 5.0f) * dp, chevGap = (isPiP ? 7.0f : 12.0f) * dp;
    const float chevW = (isPiP ? 9.0f : 16.0f) * dp, chevH = (isPiP ? 14.0f : 24.0f) * dp;
    wchar_t signChar = isForward ? L'+' : L'-';
    std::wstring numText = std::to_wstring((int)std::lround(seconds));
    std::wstring signText(1, signChar);
    // Measure at the base size.
    const float baseFontSize = isPiP ? 15.0f : 28.0f;
    D2D1_SIZE_F numSz = r.MeasureStringSized(numText, baseFontSize, DWRITE_FONT_WEIGHT_BOLD);
    float numW = std::max(numMinW, numSz.width + (isPiP ? 4.0f : 8.0f) * dp);
    float groupW = signW + signNumGap + numW + chevGap + chevW;
    float leftEdge = isForward ? (bounds.right - edgeInset - groupW) : (bounds.left + edgeInset);
    const float groupCx = leftEdge + groupW * 0.5f;
    auto tx = [&](float x) { return groupCx + (x - groupCx) * groupScale + driftX; };
    auto ty = [&](float y) { return cy + (y - cy) * groupScale; };
    auto transformRect = [&](const D2D1_RECT_F& rect) { return D2D1::RectF(tx(rect.left), ty(rect.top), tx(rect.right), ty(rect.bottom)); };
    auto textRect = [&](float l, float r2, float h) { return D2D1::RectF(l, cy - h * 0.5f, r2, cy + h * 0.5f); };
    const D2D1_COLOR_F white = D2D1::ColorF(1.0f, 1.0f, 1.0f, groupAlpha);
    const D2D1_COLOR_F shadow = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f * groupAlpha);
    // Forward layout: [sign][seconds] ... [chevron]. Backward: [chevron] ... [sign][seconds].
    float signL = isForward ? leftEdge : (leftEdge + chevGap + chevW);
    float numL = signL + signW + signNumGap;
    float chevL = isForward ? (numL + numW + chevGap) : leftEdge;
    // Keep bounding boxes unscaled at their exact base layout positions so center never shifts or wobbles even by 1px
    D2D1_RECT_F signRect = transformRect(textRect(signL, signL + signW, (isPiP ? 20.0f : 36.0f) * dp));
    D2D1_RECT_F numRect = transformRect(textRect(numL, numL + numW, (isPiP ? 20.0f : 36.0f) * dp));
    // Drop shadow then the white glyphs.
    D2D1_RECT_F signShadow = signRect; signShadow.top += 1.5f * dp; signShadow.bottom += 1.5f * dp;
    D2D1_RECT_F numShadow = numRect; numShadow.top += 1.5f * dp; numShadow.bottom += 1.5f * dp;
    r.DrawStringSized(signText, signShadow, shadow, baseFontSize * signScale * groupScale, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    r.DrawStringSized(signText, signRect, white, baseFontSize * signScale * groupScale, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    r.DrawStringSized(numText, numShadow, shadow, baseFontSize * numScale * groupScale, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    r.DrawStringSized(numText, numRect, white, baseFontSize * numScale * groupScale, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Draw stroked chevron path.
    auto drawChevron = [&](float offsetX, float opacity, bool drawShadow) {
        if (opacity <= 0.001f) return;
        const float outerX = (isForward ? (isPiP ? 2.0f : 3.0f) : (isPiP ? 7.0f : 13.0f)) * dp;
        const float tipX = (isForward ? (isPiP ? 7.0f : 13.0f) : (isPiP ? 2.0f : 3.0f)) * dp;
        const float x0 = tx(chevL + outerX + offsetX);
        const float x1 = tx(chevL + tipX + offsetX);
        const float y0 = ty(cy - chevH * 0.5f + (isPiP ? 1.0f : 2.0f) * dp);
        const float y1 = ty(cy);
        const float y2 = ty(cy + chevH * 0.5f - (isPiP ? 1.0f : 2.0f) * dp);
        const float stroke = (isPiP ? 2.6f : 4.5f) * dp * groupScale, radius = stroke * 0.5f;
        auto drawRoundStroke = [&](float yOffset, const D2D1_COLOR_F& color) {
            r.DrawLine(x0, y0 + yOffset, x1, y1 + yOffset, color, stroke);
            r.DrawLine(x1, y1 + yOffset, x0, y2 + yOffset, color, stroke);
            r.FillCircle(x0, y0 + yOffset, radius, color);
            r.FillCircle(x1, y1 + yOffset, radius, color);
            r.FillCircle(x0, y2 + yOffset, radius, color);
        };
        if (drawShadow) { drawRoundStroke(1.5f * dp, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f * opacity)); }
        drawRoundStroke(0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity));
    };
    // Standing chevron at rest.
    drawChevron(0.0f, groupAlpha, true);
    // Every press contributes its own incoming chevron.
    for (float rawSlide : chevronSlides) {
        rawSlide = std::clamp(rawSlide, 0.0f, 1.0f);
        if (rawSlide >= 1.0f) continue;
        const float spring = kEaseSpring.Ease(rawSlide);
        const float travel = spring * 0.72f + rawSlide * 0.28f, travelDirection = isForward ? -1.0f : 1.0f;
        const float spawnOffset = travelDirection * (isPiP ? 14.0f : 24.0f) * dp * (1.0f - travel);
        float inAlpha = rawSlide < 0.25f ? (rawSlide / 0.25f) * 0.9f : 0.9f + (rawSlide - 0.25f) / 0.75f * 0.1f;
        inAlpha *= groupAlpha;
        const float trailStep = (isPiP ? 3.0f : 5.5f) * dp * (1.0f - rawSlide);
        drawChevron(spawnOffset + travelDirection * trailStep * 2.0f, inAlpha * 0.12f, false);
        drawChevron(spawnOffset + travelDirection * trailStep, inAlpha * 0.25f, false);
        drawChevron(spawnOffset, inAlpha, true);
    }
}
} // namespace

void UIComponents::DrawDoubleTapFeedback(OverlayRenderer& r, const Settings& s, const DoubleTapFeedbackViewModel& vm) {
    (void)s;
    if (!vm.visible) return;
    if (vm.fadeSideVisible) {
        DrawSeekBadge(r, vm.bounds, vm.fadeSideForward, vm.fadeSideSeconds, 1.0f, vm.fadeSideAppear, vm.fadeSideFadeOut,
                      vm.fadeSideChevronSlides, vm.fadeSideSignPop, vm.fadeSideNumberPop, vm.isPiP);
    }
    DrawSeekBadge(r, vm.bounds, vm.isForward, vm.totalSeconds, 1.0f, vm.appear, vm.fadeOut, vm.chevronSlides, vm.signPop, vm.numberPop, vm.isPiP);
}

void UIComponents::DrawCenterPlayPauseBadge(OverlayRenderer& r, const Settings& s, const PlayPauseBadgeViewModel& vm) {
    if (!vm.visible) return;
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    float alpha = std::sin(vm.animProgress * 3.14159f);
    float scale = 0.88f + vm.animProgress * 0.18f, radius = 28.0f * dp * scale;
    float cx = (vm.bounds.left + vm.bounds.right) * 0.5f;
    float cy = (vm.bounds.top + vm.bounds.bottom) * 0.5f;
    // Compact YouTube-style frosted circular badge with subtle border
    r.FillCircle(cx, cy, radius, ColorWithAlpha(th.scrim, 0.65f * alpha));
    r.DrawCircle(cx, cy, radius, ColorWithAlpha(th.border, 0.60f * alpha), 1.0f);
    float iconW = 18.0f * dp * scale;
    D2D1_RECT_F iconRect = D2D1::RectF(cx - iconW * 0.5f, cy - iconW * 0.5f, cx + iconW * 0.5f, cy + iconW * 0.5f);
    if (vm.isPlay) { iconRect.left += 1.5f * dp; iconRect.right += 1.5f * dp; }
    r.DrawIcon(vm.isPlay ? IconType::Play : IconType::Pause, iconRect, D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha));
}

void UIComponents::DrawJumpAheadButton(OverlayRenderer& r, const Settings&, const JumpAheadViewModel& vm) {
    if (!vm.visible || vm.opacity <= 0.001f) return;
    float dp = r.GetDpiScale();
    D2D1_RECT_F bounds = vm.bounds;
    float pillH = RectHeight(bounds);
    float pillW = RectWidth(bounds);
    float radius = pillH * 0.5f;

    float cx = (bounds.left + bounds.right) * 0.5f;
    float cy = (bounds.top + bounds.bottom) * 0.5f;
    float scale = vm.scale;
    float hw = pillW * 0.5f * scale;
    float hh = pillH * 0.5f * scale;
    D2D1_RECT_F scaledRect = D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);

    // Soft, uniform ambient shadow matching modern floating pills (no harsh underside banding)
    r.DrawDropShadow(scaledRect, radius * scale, 3.5f * dp, 0.5f * dp, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.16f * vm.opacity));

    // Background color: Normal #ffffff, Hover #f2f2f2, Active #e5e5e5
    D2D1_COLOR_F bgColor;
    if (vm.isPressed) {
        bgColor = D2D1::ColorF(0.90f, 0.90f, 0.90f, vm.opacity);
    } else if (vm.isHovered) {
        bgColor = D2D1::ColorF(0.95f, 0.95f, 0.95f, vm.opacity);
    } else {
        bgColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.opacity);
    }
    r.FillRoundedRect(scaledRect, radius * scale, bgColor);

    // Subtle crisp hairline border for clean edge definition across all backgrounds
    r.DrawRoundedRect(scaledRect, radius * scale, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f * vm.opacity), 1.0f);

    // Foreground dark color: #0f0f0f
    D2D1_COLOR_F fgColor = D2D1::ColorF(0.06f, 0.06f, 0.06f, vm.opacity);

    // Layout: Left pad 16dp, icon 22dp x 14dp, gap 8dp, text, right pad 18dp
    float padLeft = 16.0f * dp * scale;
    float iconW = 22.0f * dp * scale;
    float iconH = 14.0f * dp * scale;
    float gap = 8.0f * dp * scale;
    float iconLeft = scaledRect.left + padLeft;
    float iconTop = cy - iconH * 0.5f;

    // Pixel-accurate double chevron path (SVG viewBox: 0 0 22 14),
    // stroke-width 2, stroke-linecap round, stroke-linejoin round
    // Triangle 1: (2.5, 1.5) -> (10.5, 7.0) -> (2.5, 12.5) -> closed
    // Triangle 2: (11.5, 1.5) -> (19.5, 7.0) -> (11.5, 12.5) -> closed
    float strokeW = 2.0f * dp * scale;
    float sFactor = iconH / 14.0f;
    auto mapPt = [&](float x, float y) {
        return D2D1::Point2F(iconLeft + x * sFactor, iconTop + y * sFactor);
    };

    auto drawStrokedTriangle = [&](D2D1_POINT_2F p1, D2D1_POINT_2F p2, D2D1_POINT_2F p3) {
        r.DrawLine(p1.x, p1.y, p2.x, p2.y, fgColor, strokeW);
        r.DrawLine(p2.x, p2.y, p3.x, p3.y, fgColor, strokeW);
        r.DrawLine(p3.x, p3.y, p1.x, p1.y, fgColor, strokeW);
        float rDot = strokeW * 0.5f;
        r.FillCircle(p1.x, p1.y, rDot, fgColor);
        r.FillCircle(p2.x, p2.y, rDot, fgColor);
        r.FillCircle(p3.x, p3.y, rDot, fgColor);
    };

    drawStrokedTriangle(mapPt(2.5f, 1.5f), mapPt(10.5f, 7.0f), mapPt(2.5f, 12.5f));
    drawStrokedTriangle(mapPt(11.5f, 1.5f), mapPt(19.5f, 7.0f), mapPt(11.5f, 12.5f));

    // Text label: Roboto 500 / 14px Medium
    float textLeft = iconLeft + iconW + gap;
    float textRight = scaledRect.right - 18.0f * dp * scale;
    D2D1_RECT_F textRect = D2D1::RectF(textLeft, scaledRect.top, textRight, scaledRect.bottom);
    r.DrawStringSized(vm.text, textRect, fgColor, 14.0f * scale, DWRITE_FONT_WEIGHT_MEDIUM,
                      DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

template<bool CheckedIndicators, class R>
static void DrawContextMenuImpl( R& r, const Settings& s, const D2D1_POINT_2F& origin, const std::vector<ContextMenuItem>& items,
    int hoveredIndex, float animProgress ) {
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    float alpha = std::clamp(animProgress, 0.0f, 1.0f);
    float scale = 0.94f + 0.06f * alpha, menuW = 220.0f * dp * scale, itemH = 32.0f * dp, padV = 4.0f * dp;
    float totalH = padV * 2.0f;
    for (const auto& it : items) { totalH += it.isSeparator ? 6.0f * dp : itemH; }
    totalH *= scale;
    D2D1_RECT_F menuRect = D2D1::RectF(origin.x, origin.y, origin.x + menuW, origin.y + totalH);
    SurfaceStyle mStyle;
    mStyle.radius = 10.0f * dp;
    mStyle.fill = ColorWithAlpha(th.surfaceFloating, 0.98f * alpha);
    mStyle.border = ColorWithAlpha(th.border, 0.75f * alpha);
    mStyle.borderWidth = 1.0f;
    mStyle.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    mStyle.shadowBlur = 0.0f;
    mStyle.hasInnerHighlight = false;
    r.DrawSurface(menuRect, mStyle);
    float curY = menuRect.top + padV;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        if (it.isSeparator) {
            r.DrawLine(menuRect.left + 10.0f * dp, curY + 2.5f * dp, menuRect.right - 10.0f * dp, curY + 2.5f * dp, ColorWithAlpha(th.borderSubtle, alpha), 1.0f);
            curY += 6.0f * dp;
            continue;
        }
        D2D1_RECT_F rowRect = D2D1::RectF(menuRect.left + 4.0f * dp, curY, menuRect.right - 4.0f * dp, curY + itemH);
        bool isHover = (hoveredIndex == (int)i) && !it.isDisabled;
        if (isHover)
            r.FillRoundedRect(rowRect, 6.0f * dp, it.isDanger ? ColorWithAlpha(th.red, 0.18f * alpha) : ColorWithAlpha(th.accent, 0.12f * alpha));
        float iconSz = 14.0f * dp;
        D2D1_RECT_F iRect = D2D1::RectF(rowRect.left + 8.0f * dp, rowRect.top + (itemH - iconSz) * 0.5f, rowRect.left + 8.0f * dp + iconSz, rowRect.top + (itemH + iconSz) * 0.5f);
        D2D1_COLOR_F iconCol = it.isDisabled ? ColorWithAlpha(th.textMuted, 0.40f * alpha) : (it.isDanger ? ColorWithAlpha(th.red, alpha) : (isHover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.textMuted, alpha)));
        if constexpr (CheckedIndicators) {
            if (!it.showCheckedIndicator || it.isChecked) {
                r.DrawIcon(it.showCheckedIndicator ? IconType::Checkmark : it.icon, iRect,
                    it.showCheckedIndicator ? ColorWithAlpha(th.accent, alpha) : iconCol);
            }
        } else { r.DrawIcon(it.icon, iRect, iconCol); }
        float shortcutW = 38.0f * dp;
        float labelRight = rowRect.right - (it.shortcut.empty() ? 8.0f * dp : shortcutW + 6.0f * dp);
        D2D1_RECT_F tRect = D2D1::RectF(iRect.right + 8.0f * dp, rowRect.top, labelRight, rowRect.bottom);
        D2D1_COLOR_F textCol = it.isDisabled ? ColorWithAlpha(th.textMuted, 0.45f * alpha) : (it.isDanger ? ColorWithAlpha(th.red, alpha) : (isHover ? ColorWithAlpha(th.accent, alpha) : ColorWithAlpha(th.text, alpha)));
        // Context labels come from file names and can be arbitrarily long;
        // keep them inside the menu instead of relying on a clipped layout.
        r.DrawString(FitSingleLine(r, it.label, FontType::Small, SafeWidth(tRect.left, tRect.right)), tRect, textCol, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (!it.shortcut.empty()) {
            D2D1_RECT_F scRect = D2D1::RectF(rowRect.right - shortcutW - 8.0f * dp, rowRect.top, rowRect.right - 8.0f * dp, rowRect.bottom);
            r.DrawString(FitSingleLine(r, it.shortcut, FontType::Caption, SafeWidth(scRect.left, scRect.right)), scRect, it.isDisabled ? ColorWithAlpha(th.textMuted, 0.35f * alpha) : (isHover ? ColorWithAlpha(th.accent, 0.85f * alpha) : ColorWithAlpha(th.textMuted, alpha)), FontType::Caption, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        curY += itemH;
    }
}

void UIComponents::DrawContextMenu(Renderer& r, const Settings& s, const D2D1_POINT_2F& origin, const std::vector<ContextMenuItem>& items,
    int hoveredIndex, float animProgress) {
    DrawContextMenuImpl<true>(r, s, origin, items, hoveredIndex, animProgress);
}

void UIComponents::DrawOverlayContextMenu(OverlayRenderer& r, const Settings& s,
    const D2D1_POINT_2F& origin, const std::vector<ContextMenuItem>& items, int hoveredIndex, float animProgress) {
    DrawContextMenuImpl<false>(r, s, origin, items, hoveredIndex, animProgress);
}

template<class DrawContext> void DrawInputModalImpl(DrawContext& r, const Settings& s, const D2D1_RECT_F& bounds, const ModalViewModel& vm) {
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    r.FillRect(bounds, th.scrim);
    float boxW = std::min(460.0f * dp, std::max(320.0f * dp, RectWidth(bounds) - 48.0f * dp));
    float boxH = 204.0f * dp;
    float cx = (bounds.left + bounds.right) * 0.5f;
    float cy = (bounds.top + bounds.bottom) * 0.5f;
    D2D1_RECT_F box = D2D1::RectF(cx - boxW * 0.5f, cy - boxH * 0.5f, cx + boxW * 0.5f, cy + boxH * 0.5f);
    SurfaceStyle modalStyle;
    modalStyle.radius = Presentation::DialogRadius * dp;
    modalStyle.fill = ColorWithAlpha(th.surfaceModal, 0.98f);
    modalStyle.border = th.border;
    modalStyle.borderWidth = 1.0f;
    modalStyle.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.60f);
    modalStyle.shadowBlur = 32.0f * dp;
    modalStyle.shadowOffsetY = 10.0f * dp;
    r.DrawSurface(box, modalStyle);
    D2D1_RECT_F titleRect = D2D1::RectF(box.left + 24.0f * dp, box.top + 20.0f * dp, box.right - 24.0f * dp, box.top + 48.0f * dp);
    r.DrawString(vm.title, titleRect, th.text, FontType::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Input box
    D2D1_RECT_F inputRect = D2D1::RectF(box.left + 24.0f * dp, box.top + 64.0f * dp, box.right - 24.0f * dp, box.top + 106.0f * dp);
    r.FillRoundedRect(inputRect, 8.0f * dp, ColorWithAlpha(th.surfaceElevated, 0.95f));
    r.DrawRoundedRect(inputRect, 8.0f * dp, th.accent, 1.5f);
    UIComponents::DrawTextInput(r, s, inputRect, vm.visualState, vm.prompt, FontType::Medium, 12.0f * dp);
    // Buttons
    float btnW = 92.0f * dp, btnH = 36.0f * dp, btnY = box.bottom - 20.0f * dp - btnH;
    D2D1_RECT_F okBtn = D2D1::RectF(box.right - 24.0f * dp - btnW, btnY, box.right - 24.0f * dp, btnY + btnH);
    const auto okColors = Presentation::Button(th, true, false, vm.isOkHovered, vm.isOkPressed);
    r.FillRoundedRect(okBtn, Presentation::ButtonRadius * dp, okColors.fill);
    r.DrawString(Loc::Tr("modal.ok"), okBtn, okColors.text, FontType::Medium, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_RECT_F cancelBtn = D2D1::RectF(okBtn.left - 10.0f * dp - btnW, btnY, okBtn.left - 10.0f * dp, btnY + btnH);
    const auto cancelColors = Presentation::Button(th, false, false, vm.isCancelHovered, vm.isCancelPressed);
    r.FillRoundedRect(cancelBtn, Presentation::ButtonRadius * dp, cancelColors.fill);
    r.DrawRoundedRect(cancelBtn, Presentation::ButtonRadius * dp, cancelColors.border, dp);
    r.DrawString(Loc::Tr("modal.cancel"), cancelBtn, cancelColors.text, FontType::Medium, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void UIComponents::DrawInputModal(Renderer& r, const Settings& s, const D2D1_RECT_F& b, const ModalViewModel& v) { DrawInputModalImpl(r, s, b, v); }
void UIComponents::DrawInputModal(OverlayRenderer& r, const Settings& s, const D2D1_RECT_F& b, const ModalViewModel& v) { DrawInputModalImpl(r, s, b, v); }

template<class DrawContext> void DrawConfirmModalImpl(DrawContext& r, const Settings& s, const D2D1_RECT_F& bounds, const ModalViewModel& vm) {
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    r.FillRect(bounds, th.scrim);
    float maxBoxW = 500.0f * dp;
    float boxW = std::min(maxBoxW, std::max(320.0f * dp, RectWidth(bounds) - 48.0f * dp));
    float boxH = Presentation::ConfirmationHeight * dp;
    float cx = (bounds.left + bounds.right) * 0.5f;
    float cy = (bounds.top + bounds.bottom) * 0.5f;
    D2D1_RECT_F box = D2D1::RectF(cx - boxW * 0.5f, cy - boxH * 0.5f, cx + boxW * 0.5f, cy + boxH * 0.5f);
    SurfaceStyle modalStyle;
    modalStyle.radius = Presentation::DialogRadius * dp;
    modalStyle.fill = ColorWithAlpha(th.surfaceModal, 0.98f);
    modalStyle.border = th.border;
    modalStyle.borderWidth = 1.0f;
    modalStyle.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.60f);
    modalStyle.shadowBlur = 32.0f * dp;
    modalStyle.shadowOffsetY = 10.0f * dp;
    r.DrawSurface(box, modalStyle);
    D2D1_RECT_F titleRect = D2D1::RectF(box.left + 24.0f * dp, box.top + 22.0f * dp, box.right - 24.0f * dp, box.top + 50.0f * dp);
    r.DrawString(vm.title, titleRect, th.text, FontType::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_RECT_F msgRect = D2D1::RectF(box.left + 24.0f * dp, box.top + 56.0f * dp, box.right - 24.0f * dp, box.bottom - 68.0f * dp);
    r.DrawStringWrapped(vm.message, msgRect, th.textMuted, FontType::Regular, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    float btnH = 36.0f * dp, btnY = box.bottom - 20.0f * dp - btnH;
    float confBtnW = std::max(110.0f * dp, r.MeasureString(vm.confirmText, FontType::Medium).width + 32.0f * dp);
    float cancelBtnW = 100.0f * dp;
    D2D1_RECT_F confBtn = D2D1::RectF(box.right - 24.0f * dp - confBtnW, btnY, box.right - 24.0f * dp, btnY + btnH);
    const auto confirmColors = Presentation::Button(th, true, vm.isDestructive, vm.isOkHovered, vm.isOkPressed);
    r.FillRoundedRect(confBtn, Presentation::ButtonRadius * dp, confirmColors.fill);
    r.DrawString(vm.confirmText, confBtn, confirmColors.text, FontType::Medium, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_RECT_F cancelBtn = D2D1::RectF(confBtn.left - 10.0f * dp - cancelBtnW, btnY, confBtn.left - 10.0f * dp, btnY + btnH);
    const auto cancelColors = Presentation::Button(th, false, false, vm.isCancelHovered, vm.isCancelPressed);
    r.FillRoundedRect(cancelBtn, Presentation::ButtonRadius * dp, cancelColors.fill);
    r.DrawRoundedRect(cancelBtn, Presentation::ButtonRadius * dp, cancelColors.border, dp);
    r.DrawString(Loc::Tr("modal.cancel"), cancelBtn, cancelColors.text, FontType::Medium, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void UIComponents::DrawConfirmModal(Renderer& r, const Settings& s, const D2D1_RECT_F& b, const ModalViewModel& v) { DrawConfirmModalImpl(r, s, b, v); }
void UIComponents::DrawConfirmModal(OverlayRenderer& r, const Settings& s, const D2D1_RECT_F& b, const ModalViewModel& v) { DrawConfirmModalImpl(r, s, b, v); }

void UIComponents::DrawAutoscrollAnchor(Renderer& r, const Settings& s, const AutoscrollViewModel& vm) {
    if (!vm.isActive) return;
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    // 30-DIP diameter circular anchor
    float radius = 15.0f * dp, cx = vm.anchorPoint.x, cy = vm.anchorPoint.y;
    // Card shadow around the anchor circle
    D2D1_RECT_F bounds = D2D1::RectF(cx - radius, cy - radius, cx + radius, cy + radius);
    r.DrawCardShadow(bounds, radius, 4.0f * dp);
    // Translucent dark background disc
    D2D1_COLOR_F bgCol = ColorWithAlpha(th.surfaceFloating, 0.90f);
    r.FillCircle(cx, cy, radius, bgCol);
    // Outer border
    D2D1_COLOR_F borderCol = ColorWithAlpha(th.border, 0.70f);
    r.DrawCircle(cx, cy, radius, borderCol, 1.2f * dp);
    // Center dot
    r.FillCircle(cx, cy, 2.2f * dp, th.accent);
    // Vertical chevrons (up & down)
    // 15-DIP dead zone threshold
    float deadZonePx = 15.0f * dp;
    bool isScrollingUp = (vm.dy < -deadZonePx);
    bool isScrollingDown = (vm.dy > deadZonePx);
    D2D1_COLOR_F upCol = isScrollingUp ? th.accent : ColorWithAlpha(th.text, 0.80f);
    D2D1_COLOR_F downCol = isScrollingDown ? th.accent : ColorWithAlpha(th.text, 0.80f);
    float chevronWingX = 4.5f * dp, chevronH = 3.5f * dp, strokeW = 1.6f * dp;
    // Top chevron (pointing up)
    float upTipY = cy - 8.5f * dp, upBaseY = upTipY + chevronH;
    r.DrawLine(cx - chevronWingX, upBaseY, cx, upTipY, upCol, strokeW);
    r.DrawLine(cx, upTipY, cx + chevronWingX, upBaseY, upCol, strokeW);
    // Bottom chevron (pointing down)
    float downTipY = cy + 8.5f * dp, downBaseY = downTipY - chevronH;
    r.DrawLine(cx - chevronWingX, downBaseY, cx, downTipY, downCol, strokeW);
    r.DrawLine(cx, downTipY, cx + chevronWingX, downBaseY, downCol, strokeW);
}
void UIComponents::DrawEmptyState(Renderer& r, const Settings& s, const EmptyStateViewModel& vm) {
    if (!vm.visible) return;
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    // The empty state belongs to the page, not to a floating card: plain centered text on the canvas with quiet actions beneath it.
    D2D1_RECT_F area = vm.bounds;
    float cx = (area.left + area.right) * 0.5f;
    float cy = area.top + (RectHeight(area) - 150.0f * dp) * 0.5f + 46.0f * dp;
    D2D1_RECT_F titleRect = D2D1::RectF(area.left + 24.0f * dp, cy - 44.0f * dp, area.right - 24.0f * dp, cy - 18.0f * dp);
    r.DrawString(vm.title, titleRect, th.text, FontType::Title, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_RECT_F bodyRect = D2D1::RectF(cx - 260.0f * dp, cy - 12.0f * dp, cx + 260.0f * dp, cy + 30.0f * dp);
    r.DrawStringWrapped(vm.body, bodyRect, th.textMuted, FontType::Regular, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    for (const auto& btn : vm.buttons) {
        if (RectWidth(btn.bounds) <= 0.0f || RectHeight(btn.bounds) <= 0.0f) continue;
        const auto colors = Presentation::Button(th, btn.isPrimary, false, btn.isHovered, btn.isPressed);
        const auto bgCol = colors.fill, borderCol = colors.border, fgCol = colors.text;
        const float borderWidth = dp;
        if (btn.isFocused)
            r.DrawRoundedRect(InsetRectF(btn.bounds, -3.0f * dp, -3.0f * dp), (Presentation::ButtonRadius + 3.0f) * dp, th.focusRing, 1.5f * dp);
        r.FillRoundedRect(btn.bounds, Presentation::ButtonRadius * dp, bgCol);
        r.DrawRoundedRect(btn.bounds, Presentation::ButtonRadius * dp, borderCol, borderWidth);
        float btnH = RectHeight(btn.bounds);
        float btnIconSz = 14.0f * dp;
        D2D1_SIZE_F textSz = r.MeasureString(btn.text, FontType::Medium);
        float totalContentW = btnIconSz + 6.0f * dp + textSz.width;
        float btnCX = (btn.bounds.left + btn.bounds.right) * 0.5f;
        float contentLeft = btnCX - totalContentW * 0.5f;
        D2D1_RECT_F iconRect = D2D1::RectF(contentLeft, btn.bounds.top + (btnH - btnIconSz) * 0.5f, contentLeft + btnIconSz, btn.bounds.top + (btnH + btnIconSz) * 0.5f);
        r.DrawIcon(btn.icon, iconRect, fgCol);
        D2D1_RECT_F textRect = D2D1::RectF(contentLeft + btnIconSz + 6.0f * dp, btn.bounds.top, btn.bounds.right, btn.bounds.bottom);
        r.DrawString(btn.text, textRect, fgCol, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}
void UIComponents::DrawMediaToolbar(Renderer& r, const Settings& s, const MediaTabViewModel& vm) {
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    // 1. Search Box (Inline interactive search field)
    const auto& searchBox = vm.searchBtnBounds;
    if (searchBox.right > searchBox.left) {
        if (vm.searchFocused) {
            r.FillRoundedRect(searchBox, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f));
            r.DrawRoundedRect(searchBox, 7.0f * dp, ColorWithAlpha(th.accent, 0.75f), 1.5f * dp);
        } else if (vm.searchHovered) {
            r.FillRoundedRect(searchBox, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.035f));
            r.DrawRoundedRect(searchBox, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f), 1.0f * dp);
        } else {
            r.FillRoundedRect(searchBox, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.022f));
            r.DrawRoundedRect(searchBox, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.075f), 1.0f * dp);
        }
        const float actionIcon = 14.0f * dp, iconLeft = searchBox.left + 9.0f * dp;
        const float iconTop = searchBox.top + (RectHeight(searchBox) - actionIcon) * 0.5f;
        const D2D1_RECT_F searchIconRect = D2D1::RectF(iconLeft, iconTop, iconLeft + actionIcon, iconTop + actionIcon);
        const D2D1_COLOR_F iconColor = vm.searchFocused ? th.accent : (vm.searchHovered ? th.text : th.textMuted);
        r.DrawIcon(IconType::Search, searchIconRect, iconColor);
        const float boxW = RectWidth(searchBox);
        const bool hasQuery = vm.hasSearchQuery;
        float textRight = searchBox.right - 8.0f * dp;
        if (hasQuery && RectWidth(vm.searchClearBounds) > 0.0f) {
            textRight = vm.searchClearBounds.left - 4.0f * dp;
            const float clearW = RectWidth(vm.searchClearBounds);
            const float clearH = RectHeight(vm.searchClearBounds);
            const float clearCx = vm.searchClearBounds.left + clearW * 0.5f, clearCy = vm.searchClearBounds.top + clearH * 0.5f;
            if (vm.searchClearHovered) {
                r.FillCircle(clearCx, clearCy, 9.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
            } else { r.FillCircle(clearCx, clearCy, 9.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.035f)); }
            const float closeIconSz = 10.0f * dp;
            r.DrawIcon(IconType::CloseX, D2D1::RectF(clearCx - closeIconSz * 0.5f, clearCy - closeIconSz * 0.5f,
                clearCx + closeIconSz * 0.5f, clearCy + closeIconSz * 0.5f), vm.searchClearHovered ? th.text : th.textMuted);
        }
        const D2D1_RECT_F searchTxtRect = D2D1::RectF(iconLeft + actionIcon + 7.0f * dp, searchBox.top, textRight, searchBox.bottom);
        DrawTextInput(r, s, searchTxtRect, vm.searchVisual, L"Filter media\x2026", FontType::Small, 0.0f);
    }
    // 2. Summary count text next to search box
    if (vm.showSummary && !vm.summaryText.empty())
        r.DrawString(vm.summaryText, vm.summaryTextBounds, th.textMuted, FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // 3. Sort action
    const auto sortColors = Presentation::Button(th, false, false, vm.sortHovered, false);
    r.FillRoundedRect(vm.sortBtnBounds, Presentation::ButtonRadius * dp, sortColors.fill);
    r.DrawRoundedRect(vm.sortBtnBounds, Presentation::ButtonRadius * dp, sortColors.border, dp);
    r.DrawString(vm.sortLabel, D2D1::RectF(vm.sortBtnBounds.left + 10.0f * dp, vm.sortBtnBounds.top, vm.sortBtnBounds.right - 24.0f * dp, vm.sortBtnBounds.bottom),
        sortColors.text, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const float chevronSize = 12.0f * dp, chevronCx = vm.sortBtnBounds.right - 12.0f * dp;
    const float chevronCy = (vm.sortBtnBounds.top + vm.sortBtnBounds.bottom) * 0.5f;
    r.DrawIcon(IconType::ChevronDown, D2D1::RectF(chevronCx - chevronSize * 0.5f, chevronCy - chevronSize * 0.5f,
        chevronCx + chevronSize * 0.5f, chevronCy + chevronSize * 0.5f), sortColors.text);
    // 4. Compact grouping control: the library is either a flat list or series.
    r.FillRoundedRect(vm.groupControlBounds, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.03f));
    r.DrawRoundedRect(vm.groupControlBounds, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), 1.0f);
    // Sliding selection pill
    const float gPos = std::clamp(vm.groupModeIndicatorPos, 0.0f, 1.0f);
    const float gLeft = vm.flatBtnBounds.left + gPos * (vm.seriesBtnBounds.left - vm.flatBtnBounds.left);
    const float gRight = vm.flatBtnBounds.right + gPos * (vm.seriesBtnBounds.right - vm.flatBtnBounds.right);
    const D2D1_RECT_F gPill = D2D1::RectF(gLeft, vm.flatBtnBounds.top, gRight, vm.flatBtnBounds.bottom);
    r.FillRoundedRect(gPill, 5.0f * dp, LerpColor(th.cardBg, th.text, 0.10f));
    r.DrawRoundedRect(gPill, 5.0f * dp, ColorWithAlpha(th.accent, 0.38f), 1.0f);

    bool isFlatHover = (vm.hoveredGroupBtn == 0);
    D2D1_COLOR_F flatCol = LerpColor(th.textMuted, th.text, std::max(1.0f - gPos, isFlatHover ? 0.7f : 0.0f));
    r.DrawString(L"Standalone", vm.flatBtnBounds, flatCol, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    bool isGroupedHover = (vm.hoveredGroupBtn == 1);
    D2D1_COLOR_F seriesCol = LerpColor(th.textMuted, th.text, std::max(gPos, isGroupedHover ? 0.7f : 0.0f));
    r.DrawString(s.smartSeriesDetection ? L"Series" : L"Folders", vm.seriesBtnBounds, seriesCol, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void UIComponents::DrawMediaGroupHeader(Renderer& r, const Settings& s, const VisibleMediaGroupHeaderViewModel& vm) {
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    // Selection or hover background
    if (vm.isSelected) {
        SurfaceStyle selStyle;
        selStyle.radius = 6.0f * dp;
        selStyle.fill = ColorWithAlpha(th.accent, 0.12f);
        selStyle.border = ColorWithAlpha(th.accent, 0.45f);
        selStyle.borderWidth = 1.0f;
        r.DrawSurface(vm.headerRect, selStyle);
    } else if (vm.isHovered) {
        SurfaceStyle hovStyle;
        hovStyle.radius = 6.0f * dp;
        hovStyle.fill = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.02f);
        hovStyle.border = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.04f);
        hovStyle.borderWidth = 1.0f;
        r.DrawSurface(vm.headerRect, hovStyle);
    }
    // Drag handle on far left (6 dots)
    float dragHandleW = 12.0f * dp, dragHandleH = 14.0f * dp;
    float dragHandleY = vm.headerRect.top + (RectHeight(vm.headerRect) - dragHandleH) * 0.5f;
    D2D1_RECT_F dragRect = D2D1::RectF(vm.headerRect.left + 4.0f * dp, dragHandleY, vm.headerRect.left + 4.0f * dp + dragHandleW, dragHandleY + dragHandleH);
    r.DrawIcon(IconType::DragHandle, dragRect, (vm.isHovered || vm.isSelected) ? th.textMuted : ColorWithAlpha(th.textMuted, 0.35f));
    // Chevron indicating collapse/expand state
    float chevSz = 12.0f * dp;
    float chevY = vm.headerRect.top + (RectHeight(vm.headerRect) - chevSz) * 0.5f;
    D2D1_RECT_F chevRect = D2D1::RectF(dragRect.right + 4.0f * dp, chevY, dragRect.right + 4.0f * dp + chevSz, chevY + chevSz);
    IconType chevIcon = vm.isCollapsed ? IconType::ChevronRight : IconType::ChevronDown;
    D2D1_COLOR_F chevCol = vm.isHovered ? th.accent : th.textMuted;
    r.DrawIcon(chevIcon, chevRect, chevCol, 1.5f * dp);
    // Icon (Folder or Series)
    float iconSz = 16.0f * dp;
    float iconY = vm.headerRect.top + (RectHeight(vm.headerRect) - iconSz) * 0.5f;
    D2D1_RECT_F iconRect = D2D1::RectF(chevRect.right + 6.0f * dp, iconY, chevRect.right + 6.0f * dp + iconSz, iconY + iconSz);
    IconType icon = vm.isSeries ? (vm.isSpecial ? IconType::FolderPlaylist : IconType::MediaFilm) : (vm.isSpecial ? IconType::MediaFilm : IconType::FolderPlaylist);
    D2D1_COLOR_F iconCol = vm.isHovered ? th.text : th.textMuted;
    r.DrawIcon(icon, iconRect, iconCol);
    // Title
    D2D1_SIZE_F titleSize = r.MeasureString(vm.title, FontType::Subheading);
    float maxTitleW = std::min(titleSize.width + 4.0f * dp, RectWidth(vm.headerRect) * 0.50f);
    D2D1_RECT_F titleRect = D2D1::RectF(iconRect.right + 8.0f * dp, vm.headerRect.top, iconRect.right + 8.0f * dp + maxTitleW, vm.headerRect.bottom);
    r.DrawString(vm.title, titleRect, (vm.isHovered || vm.isSelected) ? th.accent : th.text, FontType::Subheading, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Count badge
    D2D1_SIZE_F badgeSize = r.MeasureString(vm.countText, FontType::MonoSmall);
    float badgeW = badgeSize.width + 16.0f * dp, badgeH = 20.0f * dp;
    float badgeY = vm.headerRect.top + (RectHeight(vm.headerRect) - badgeH) * 0.5f;
    D2D1_RECT_F badgeRect = D2D1::RectF(titleRect.right + 8.0f * dp, badgeY, titleRect.right + 8.0f * dp + badgeW, badgeY + badgeH);
    DrawMetadataPill(r, s, badgeRect, vm.countText);
    // Trash / Delete button on far right
    float delSz = 15.0f * dp;
    float delY = vm.headerRect.top + (RectHeight(vm.headerRect) - delSz) * 0.5f;
    D2D1_RECT_F delRect = D2D1::RectF(vm.headerRect.right - 24.0f * dp, delY, vm.headerRect.right - 8.0f * dp, delY + delSz);
    if (vm.isHovered || vm.isSelected)
        r.DrawIcon(IconType::Trash, delRect, vm.isDeleteHovered ? th.red : ColorWithAlpha(th.textMuted, 0.65f));
    // Divider line between badge and trash button
    float lineLeft = badgeRect.right + 12.0f * dp;
    float lineRight = (vm.isHovered || vm.isSelected) ? (delRect.left - 10.0f * dp) : (vm.headerRect.right - 4.0f * dp);
    if (lineRight > lineLeft + 10.0f * dp) {
        float lineY = vm.headerRect.top + RectHeight(vm.headerRect) * 0.5f;
        r.DrawLine(lineLeft, lineY, lineRight, lineY, ColorWithAlpha(th.border, vm.isHovered ? 0.55f : 0.35f), 1.0f * dp);
    }
}
void UIComponents::DrawMediaItemCard(Renderer& r, const Settings& s, const VisibleMediaCardViewModel& vm) {
    float dp = r.GetDpiScale();
    if (vm.isPlaceholder) {
        const auto& th = s.currentTheme;
        r.FillRoundedRect(vm.itemRect, 10.0f * dp, ColorWithAlpha(th.accent, 0.08f));
        r.DrawDashedRoundedRect(vm.itemRect, 10.0f * dp, ColorWithAlpha(th.accent, 0.72f), 1.5f);
        return;
    }
    if (vm.liftY > 0.001f) { r.GetD2DContext()->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -vm.liftY)); }
    DrawMediaItemCard(r, s, vm.itemRect, *vm.pItem, vm.isHovered, vm.isSelected, vm.isPinned, vm.previewOpacity, vm.pLivePreviewBitmap, vm.isCloseHovered, &vm.effectiveStatus);
    if (vm.liftY > 0.001f) { r.GetD2DContext()->SetTransform(D2D1::Matrix3x2F::Identity()); }
}

void UIComponents::DrawPlaylistsRail(Renderer& r, const Settings& s, const PlaylistsRailViewModel& vm) {
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    SurfaceStyle railStyle = r.CreateSurfaceStyle(Elevation::Surface, s);
    railStyle.radius = 12.0f * dp;
    railStyle.hasInnerHighlight = false;
    railStyle.hasGradient = false;
    railStyle.glowBlur = 0.0f;
    railStyle.leftAccentBarAlpha = 0.0f;
    railStyle.leftAccentBarWidth = 0.0f;
    railStyle.fill = D2D1::ColorF(th.cardBg.r * 0.40f, th.cardBg.g * 0.40f, th.cardBg.b * 0.40f, 0.45f);
    railStyle.border = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f);
    railStyle.borderWidth = 1.0f;
    r.DrawSurface(vm.bounds, railStyle);
    float padX = 14.0f * dp, headerH = 48.0f * dp, addBtnSize = 26.0f * dp;
    D2D1_RECT_F addBtnRect = D2D1::RectF(vm.bounds.right - padX - addBtnSize, vm.bounds.top + (headerH - addBtnSize) * 0.5f + 4.0f * dp, vm.bounds.right - padX, vm.bounds.top + (headerH + addBtnSize) * 0.5f + 4.0f * dp);
    if (vm.isAddHovered) {
        r.FillRoundedRect(addBtnRect, 6.0f * dp, th.accentSubtle);
        r.DrawRoundedRect(addBtnRect, 6.0f * dp, th.accent, 1.0f);
    } else { r.FillRoundedRect(addBtnRect, 6.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.04f)); }
    r.DrawIcon(IconType::Plus, InsetRectF(addBtnRect, 5.0f * dp, 5.0f * dp), vm.isAddHovered ? th.accent : th.textMuted);
    r.PushClip(D2D1::RectF(vm.bounds.left + 2.0f * dp, vm.bounds.top + headerH - 4.0f * dp, vm.bounds.right - 2.0f * dp, vm.bounds.bottom - 8.0f * dp));
    if (!vm.items.empty()) {
        const float itemH = 52.0f * dp;
        const float itemGap = 6.0f * dp;
        const float curRailY = vm.bounds.top + headerH - 2.0f * dp - vm.scrollY;
        const float railPos = std::clamp(vm.railIndicatorPos, 0.0f, static_cast<float>(std::max(0, (int)vm.items.size() - 1)));
        const float pillY = curRailY + railPos * (itemH + itemGap);
        const D2D1_RECT_F slidingPill = D2D1::RectF(vm.bounds.left + 8.0f * dp, pillY, vm.bounds.right - 8.0f * dp, pillY + itemH);

        SurfaceStyle pillStyle;
        pillStyle.radius = 8.0f * dp;
        pillStyle.fill = ColorWithAlpha(th.accent, 0.14f);
        pillStyle.border = ColorWithAlpha(th.accent, 0.45f);
        pillStyle.borderWidth = 1.0f * dp;
        pillStyle.leftAccentBarAlpha = 1.0f;
        pillStyle.leftAccentBarColor = th.accent;
        pillStyle.leftAccentBarWidth = 3.0f * dp;
        pillStyle.glowColor = ColorWithAlpha(th.accent, 0.20f);
        pillStyle.glowBlur = 8.0f * dp;
        r.DrawSurface(slidingPill, pillStyle);
        r.FillRoundedGradientRect(slidingPill, 8.0f * dp, ColorWithAlpha(th.accent, 0.20f), ColorWithAlpha(th.accent, 0.0f), false);
    }
    for (const auto& item : vm.items) {
        SurfaceStyle pStyle;
        pStyle.radius = 8.0f * dp;
        pStyle.hasInnerHighlight = false;
        pStyle.hasGradient = false;
        pStyle.glowBlur = 0.0f;
        pStyle.leftAccentBarAlpha = 0.0f;
        pStyle.leftAccentBarWidth = 0.0f;
        if (item.isFocused) {
            pStyle.fill = ColorWithAlpha(th.surfaceElevated, 0.40f);
            pStyle.border = ColorWithAlpha(th.accent, 0.50f);
            pStyle.borderWidth = 1.0f * dp;
            r.DrawSurface(item.itemRect, pStyle);
        } else if (item.isHovered && !item.isActive) {
            pStyle.fill = ColorWithAlpha(th.surfaceElevated, 0.50f);
            pStyle.border = ColorWithAlpha(th.borderSubtle, 0.60f);
            pStyle.borderWidth = 1.0f * dp;
            r.DrawSurface(item.itemRect, pStyle);
        }
        float dotRadius = 1.4f * dp, dotCX1 = item.itemRect.left + 10.0f * dp, dotCX2 = item.itemRect.left + 15.0f * dp;
        float dotCY1 = item.itemRect.top + RectHeight(item.itemRect) * 0.5f - 6.0f * dp;
        float dotCY2 = item.itemRect.top + RectHeight(item.itemRect) * 0.5f;
        float dotCY3 = item.itemRect.top + RectHeight(item.itemRect) * 0.5f + 6.0f * dp;
        D2D1_COLOR_F dotCol = D2D1::ColorF(1.0f, 1.0f, 1.0f, (item.isFocused || item.isHovered) ? 0.28f : 0.16f);
        r.FillCircle(dotCX1, dotCY1, dotRadius, dotCol);
        r.FillCircle(dotCX2, dotCY1, dotRadius, dotCol);
        r.FillCircle(dotCX1, dotCY2, dotRadius, dotCol);
        r.FillCircle(dotCX2, dotCY2, dotRadius, dotCol);
        r.FillCircle(dotCX1, dotCY3, dotRadius, dotCol);
        r.FillCircle(dotCX2, dotCY3, dotRadius, dotCol);
        r.DrawRingProgress(item.ringCX, item.ringCY, item.ringRadius, 1.8f * dp, item.progressRatio, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), th.accent);
        if (item.isInlineRenaming) {
            D2D1_RECT_F renameBox = D2D1::RectF(item.titleRect.left - 4.0f * dp, item.itemRect.top + 4.0f * dp, item.itemRect.right - 8.0f * dp, item.itemRect.top + 30.0f * dp);
            r.FillRoundedRect(renameBox, 5.0f * dp, ColorWithAlpha(th.surfaceFloating, 0.98f));
            r.DrawRoundedRect(renameBox, 5.0f * dp, th.accent, 1.5f);
            DrawTextInput(r, s, InsetRectF(renameBox, 6.0f * dp, 0.0f), vm.inlineRenameVisual, L"Playlist name", FontType::Medium);
        } else {
            D2D1_COLOR_F titleColor = item.isActive ? th.accent : th.text;
            r.DrawString(FitSingleLine(r, Utf8ToUtf16(item.name), FontType::Medium, SafeWidth(item.titleRect.left, item.titleRect.right)), item.titleRect, titleColor, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        std::wstring subText = L"Empty";
        if (item.totalVids > 0)
            subText = (item.watchedCount > 0) ? (std::to_wstring(item.totalVids) + L" videos \x00B7 " + std::to_wstring(item.watchedCount) + L" watched") : (std::to_wstring(item.totalVids) + L" videos");
        r.DrawString(subText, item.subRect, th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (vm.hasDragClone) {
        SurfaceStyle dragStyle;
        dragStyle.radius = 10.0f * dp;
        dragStyle.fill = ColorWithAlpha(th.surfaceFloating, 0.95f);
        dragStyle.border = th.accent;
        dragStyle.borderWidth = 1.5f;
        r.DrawSurface(vm.dragCloneRect, dragStyle);
        float ringCX = vm.dragCloneRect.left + 28.0f * dp;
        float ringCY = vm.dragCloneRect.top + RectHeight(vm.dragCloneRect) * 0.5f;
        float ringRadius = 8.5f * dp;
        r.DrawRingProgress(ringCX, ringCY, ringRadius, 2.0f * dp, 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f), th.accent);
        D2D1_RECT_F titleRect = D2D1::RectF(ringCX + ringRadius + 9.0f * dp, vm.dragCloneRect.top + 7.0f * dp, vm.dragCloneRect.right - 10.0f * dp, vm.dragCloneRect.top + 27.0f * dp);
        r.DrawString(Utf8ToUtf16(vm.dragCloneName), titleRect, th.accent, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        std::wstring countStr = std::to_wstring(vm.dragCloneItemCount) + L" videos";
        D2D1_RECT_F subRect = D2D1::RectF(ringCX + ringRadius + 9.0f * dp, vm.dragCloneRect.top + 27.0f * dp, vm.dragCloneRect.right - 10.0f * dp, vm.dragCloneRect.top + 45.0f * dp);
        r.DrawString(countStr, subRect, th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    r.PopClip();
    // Folder payload hovering the rail: advertise "drop to create playlist".
    if (vm.isFolderDropActive) {
        r.FillRoundedRect(vm.bounds, 12.0f * dp, ColorWithAlpha(th.accent, 0.06f));
        r.DrawDashedRoundedRect(vm.bounds, 12.0f * dp, th.accent, 2.0f);
        D2D1_RECT_F bannerRect = D2D1::RectF(vm.bounds.left + 8.0f * dp, vm.bounds.bottom - 44.0f * dp, vm.bounds.right - 8.0f * dp, vm.bounds.bottom - 12.0f * dp);
        r.FillRoundedRect(bannerRect, 7.0f * dp, ColorWithAlpha(th.surfaceFloating, 0.95f));
        r.DrawRoundedRect(bannerRect, 7.0f * dp, th.accent, 1.5f);
        r.DrawString(L"+ Drop folder to create a playlist", bannerRect, th.accent, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

static void DrawPlaylistCardHoverPopover(Renderer& r, const Settings& s, const VisiblePlaylistEpisodeViewModel& ep, float expandP) {
    if (ep.title.empty() || expandP <= 0.001f) return;
    const float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    const D2D1_SIZE_F rtSize = r.GetRenderTargetSize();

    // The floating card contains the complete, untruncated title wrapped naturally,
    // using existing dark-theme card styling and a subtle drop shadow.
    const float popoverW = std::clamp(RectWidth(ep.itemRect) + 36.0f * dp, 240.0f * dp, 340.0f * dp);
    const float padX = 14.0f * dp;
    const float padY = 12.0f * dp;
    const float contentW = popoverW - padX * 2.0f;
    const float titleH = r.MeasureStringWrappedHeight(ep.title, FontType::Medium, contentW);
    const float popoverH = padY * 2.0f + std::max(20.0f * dp, titleH) + 4.0f * dp;

    // Horizontal centering over the hovered card with window bounds clamping:
    const float cardCX = (ep.itemRect.left + ep.itemRect.right) * 0.5f;
    float popoverLeft = cardCX - popoverW * 0.5f;
    const float minX = 12.0f * dp;
    const float maxX = std::max(minX, rtSize.width - popoverW - 12.0f * dp);
    popoverLeft = std::clamp(popoverLeft, minX, maxX);
    const float popoverRight = popoverLeft + popoverW;

    // Vertical placement: slightly above or below the card, staying within application window bounds:
    const float gap = 6.0f * dp;
    const float belowTop = ep.itemRect.bottom + gap;
    const float belowBottom = belowTop + popoverH;
    const float aboveBottom = ep.itemRect.top - gap;
    const float aboveTop = aboveBottom - popoverH;

    float popoverTop = belowTop;
    float popoverBottom = belowBottom;
    bool isAbove = false;

    if (belowBottom <= rtSize.height - 12.0f * dp) {
        popoverTop = belowTop;
        popoverBottom = belowBottom;
        isAbove = false;
    } else if (aboveTop >= 12.0f * dp) {
        popoverTop = aboveTop;
        popoverBottom = aboveBottom;
        isAbove = true;
    } else {
        popoverTop = std::clamp(belowTop, 12.0f * dp, std::max(12.0f * dp, rtSize.height - popoverH - 12.0f * dp));
        popoverBottom = popoverTop + popoverH;
        isAbove = false;
    }

    // Subtle fade and scale/slide transition
    const float alpha = std::clamp(expandP, 0.0f, 1.0f);
    const float slideOffset = (1.0f - alpha) * 4.0f * dp;
    if (isAbove) {
        popoverTop += slideOffset;
        popoverBottom += slideOffset;
    } else {
        popoverTop -= slideOffset;
        popoverBottom -= slideOffset;
    }

    const D2D1_RECT_F popoverRect = D2D1::RectF(popoverLeft, popoverTop, popoverRight, popoverBottom);

    // Subtle drop shadow
    r.DrawDropShadow(popoverRect, 8.0f * dp, 14.0f * dp, 3.0f * dp, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f * alpha));

    // Dark-theme card styling
    SurfaceStyle pStyle;
    pStyle.radius = 8.0f * dp;
    pStyle.fill = ColorWithAlpha(th.surfaceFloating, 0.98f * alpha);
    pStyle.border = ColorWithAlpha(th.border, 0.60f * alpha);
    pStyle.borderWidth = 1.0f;
    r.DrawSurface(popoverRect, pStyle);

    // Render complete untruncated title wrapped naturally
    const D2D1_RECT_F textRect = D2D1::RectF(popoverLeft + padX, popoverTop + padY,
                                            popoverRight - padX, popoverTop + padY + titleH + 4.0f * dp);
    r.DrawStringWrapped(ep.title, textRect, ColorWithAlpha(th.text, th.text.a * alpha),
                        FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void UIComponents::DrawPlaylistDetailPane(Renderer& r, const Settings& s, const PlaylistDetailViewModel& vm) {
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    if (!vm.hasPlaylist) {
        float cx = (vm.listClipBounds.left + vm.listClipBounds.right) * 0.5f;
        float cy = (vm.listClipBounds.top + vm.listClipBounds.bottom) * 0.5f - 18.0f * dp;
        r.DrawCircle(cx, cy, 24.0f * dp, D2D1::ColorF(0, 0, 0, 0), 1.0f);
        r.DrawDashedRoundedRect(D2D1::RectF(cx - 24.0f * dp, cy - 24.0f * dp, cx + 24.0f * dp, cy + 24.0f * dp), 24.0f * dp, th.border, 1.0f);
        r.DrawIcon(IconType::Plus, D2D1::RectF(cx - 10.0f * dp, cy - 10.0f * dp, cx + 10.0f * dp, cy + 10.0f * dp), th.textMuted);
        D2D1_RECT_F emptyRect = D2D1::RectF(vm.listClipBounds.left + 20.0f * dp, cy + 38.0f * dp, vm.listClipBounds.right - 20.0f * dp, cy + 76.0f * dp);
        r.DrawString(L"Select or create a playlist from the left rail to view its contents.", emptyRect, th.textMuted, FontType::Regular, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    float paneRight = std::max(vm.listClipBounds.left + 120.0f * dp, vm.listClipBounds.right - 26.0f * dp);
    float curY = vm.listClipBounds.top + 14.0f * dp, headerH = 50.0f * dp;
    D2D1_RECT_F headRect = D2D1::RectF(vm.listClipBounds.left, curY, paneRight, curY + headerH);
    D2D1_RECT_F titleRect = D2D1::RectF(headRect.left, headRect.top, headRect.right - 180.0f * dp, headRect.top + 28.0f * dp);
    r.DrawString(FitSingleLine(r, Utf8ToUtf16(vm.playlistName), FontType::Title, SafeWidth(titleRect.left, titleRect.right)), titleRect, th.text, FontType::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_RECT_F metaRect = D2D1::RectF(headRect.left, headRect.top + 28.0f * dp, headRect.right - 180.0f * dp, headRect.top + 48.0f * dp);
    r.DrawString(FitSingleLine(r, vm.metaStr, FontType::MonoSmall, SafeWidth(metaRect.left, metaRect.right)), metaRect, th.textMuted, FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    float playAllW = 106.0f * dp, playAllH = 32.0f * dp;
    D2D1_RECT_F playAllRect = D2D1::RectF(headRect.right - playAllW - 40.0f * dp, headRect.top + 6.0f * dp, headRect.right - 40.0f * dp, headRect.top + 6.0f * dp + playAllH);
    const D2D1_COLOR_F playAllFill = vm.isEmpty ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.04f)
        : (vm.isPlayAllHovered ? ColorWithAlpha(th.accent, 0.88f) : th.accent);
    const D2D1_COLOR_F playAllForeground = vm.isEmpty ? th.textMuted : D2D1::ColorF(0.06f, 0.05f, 0.04f, 1.0f);
    r.FillRoundedRect(playAllRect, 7.0f * dp, playAllFill);
    D2D1_RECT_F playIconRect = D2D1::RectF(playAllRect.left + 12.0f * dp, playAllRect.top + (playAllH - 12.0f * dp) * 0.5f, playAllRect.left + 24.0f * dp, playAllRect.top + (playAllH + 12.0f * dp) * 0.5f);
    r.DrawIcon(IconType::Play, playIconRect, playAllForeground);
    D2D1_RECT_F playTextRect = D2D1::RectF(playAllRect.left + 28.0f * dp, playAllRect.top, playAllRect.right - 10.0f * dp, playAllRect.bottom);
    r.DrawString(L"Play All", playTextRect, playAllForeground, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    float delBtnH = 32.0f * dp;
    D2D1_RECT_F delPlRect = D2D1::RectF(headRect.right - 34.0f * dp, headRect.top + 6.0f * dp, headRect.right, headRect.top + 6.0f * dp + delBtnH);
    r.FillRoundedRect(delPlRect, 7.0f * dp, vm.isDeletePlHovered ? D2D1::ColorF(0.95f, 0.25f, 0.35f, 0.20f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.04f));
    r.DrawRoundedRect(delPlRect, 7.0f * dp, vm.isDeletePlHovered ? D2D1::ColorF(0.95f, 0.25f, 0.35f, 0.60f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), 1.0f);
    r.DrawIcon(IconType::Trash, InsetRectF(delPlRect, 8.0f * dp, 8.0f * dp), vm.isDeletePlHovered ? D2D1::ColorF(0.95f, 0.25f, 0.35f) : th.textMuted);
    curY += headerH + 16.0f * dp;
    if (vm.isEmpty) {
        float dropBoxH = 220.0f * dp;
        D2D1_RECT_F dropBoxRect = D2D1::RectF(vm.listClipBounds.left, curY, paneRight, curY + dropBoxH);
        r.FillRoundedRect(dropBoxRect, 14.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.02f));
        r.DrawDashedRoundedRect(dropBoxRect, 14.0f * dp, ColorWithAlpha(th.accent, 0.60f), 1.5f);
        float iconSz = 36.0f * dp;
        float cx = (dropBoxRect.left + dropBoxRect.right) * 0.5f;
        D2D1_RECT_F dropIconRect = D2D1::RectF(cx - iconSz * 0.5f, dropBoxRect.top + 46.0f * dp, cx + iconSz * 0.5f, dropBoxRect.top + 46.0f * dp + iconSz);
        r.DrawIcon(IconType::FolderPlaylist, dropIconRect, th.accent);
        D2D1_RECT_F dropTextRect = D2D1::RectF(dropBoxRect.left + 20.0f * dp, dropBoxRect.top + 96.0f * dp, dropBoxRect.right - 20.0f * dp, dropBoxRect.top + 124.0f * dp);
        if (vm.isSmart) {
            r.DrawString(L"No media matches the rules for this smart playlist", dropTextRect, th.text, FontType::Subheading, DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D1_RECT_F dropSubTextRect = D2D1::RectF(dropBoxRect.left + 20.0f * dp, dropBoxRect.top + 128.0f * dp, dropBoxRect.right - 20.0f * dp, dropBoxRect.top + 152.0f * dp);
            r.DrawString(L"Use 'Edit Rules' to adjust your filter criteria, or add matching media to your library.", dropSubTextRect, th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            r.DrawString(L"Drag and drop video files or folders here to add them to this playlist", dropTextRect, th.text, FontType::Subheading, DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D1_RECT_F dropSubTextRect = D2D1::RectF(dropBoxRect.left + 20.0f * dp, dropBoxRect.top + 128.0f * dp, dropBoxRect.right - 20.0f * dp, dropBoxRect.top + 152.0f * dp);
            r.DrawString(L"or drag cards from the Media tab, or right-click any video to add", dropSubTextRect, th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER);
            const float addFilesW = 118.0f * dp, addFilesH = 34.0f * dp;
            D2D1_RECT_F addFilesRect = D2D1::RectF(cx - addFilesW * 0.5f, dropBoxRect.top + 168.0f * dp,
                                                   cx + addFilesW * 0.5f, dropBoxRect.top + 168.0f * dp + addFilesH);
            r.FillRoundedRect(addFilesRect, 8.0f * dp,
                vm.isAddFilesHovered ? ColorWithAlpha(th.accent, 0.18f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.035f));
            r.DrawRoundedRect(addFilesRect, 8.0f * dp,
                vm.isAddFilesHovered ? ColorWithAlpha(th.accent, 0.72f) : ColorWithAlpha(th.border, 0.90f), 1.0f);
            D2D1_RECT_F addIconRect = D2D1::RectF(addFilesRect.left + 12.0f * dp, addFilesRect.top + 9.0f * dp,
                                                  addFilesRect.left + 28.0f * dp, addFilesRect.bottom - 9.0f * dp);
            r.DrawIcon(IconType::Plus, addIconRect, vm.isAddFilesHovered ? th.accent : th.textMuted);
            D2D1_RECT_F addTextRect = D2D1::RectF(addFilesRect.left + 31.0f * dp, addFilesRect.top,
                                                  addFilesRect.right - 10.0f * dp, addFilesRect.bottom);
            r.DrawString(L"Add Files", addTextRect, vm.isAddFilesHovered ? th.text : th.textMuted,
                         FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        return;
    }
    float resumeH = 112.0f * dp;
    if (vm.resumeCard.hasResumeCard) {
        SurfaceStyle resStyle;
        resStyle.radius = 12.0f * dp;
        resStyle.backdropFile = vm.resumeCard.filePath;
        resStyle.pLivePreviewBitmap = vm.resumeCard.pLivePreviewBitmap;
        resStyle.livePreviewOpacity = vm.resumeCard.previewOpacity;
        resStyle.backdropOpacity = vm.resumeCard.isResumeHovered ? 0.35f : 0.18f;
        resStyle.hasInnerHighlight = true;
        resStyle.innerHighlight = D2D1::ColorF(1.0f, 1.0f, 1.0f, vm.resumeCard.isResumeHovered ? 0.15f : 0.07f);
        if (vm.resumeCard.isResumeFocused) {
            resStyle.fill = ColorWithAlpha(th.surfaceElevated, 0.85f);
            resStyle.border = th.accent;
            resStyle.borderWidth = 1.5f * dp;
            resStyle.glowColor = ColorWithAlpha(th.accent, 0.25f);
            resStyle.glowBlur = 8.0f * dp;
            r.DrawSurface(vm.resumeCard.cardRect, resStyle);
        } else if (vm.resumeCard.isResumeHovered) {
            resStyle.fill = ColorWithAlpha(th.surfaceElevated, 0.80f);
            resStyle.border = ColorWithAlpha(th.accent, 0.70f);
            resStyle.borderWidth = 1.0f * dp;
            resStyle.glowColor = ColorWithAlpha(th.accent, 0.18f);
            resStyle.glowBlur = 6.0f * dp;
            r.DrawSurface(vm.resumeCard.cardRect, resStyle);
        } else {
            resStyle.fill = ColorWithAlpha(th.surfaceElevated, 0.50f);
            resStyle.border = th.borderSubtle;
            resStyle.borderWidth = 1.0f * dp;
            r.DrawSurface(vm.resumeCard.cardRect, resStyle);
        }
        // Backdrop and live preview wash on the entire card surface (matching Media cards)
        ComPtr<ID2D1Bitmap> bm = r.GetOrCreateVideoThumbnail(vm.resumeCard.filePath, 320, 180);
        if (bm) {
            const D2D1_RECT_F& cr = vm.resumeCard.cardRect;
            r.DrawBitmapRounded(cr, 12.0f * dp, bm.Get(), vm.resumeCard.isResumeHovered ? 0.32f : 0.16f, true);
            r.FillRoundedGradientRect(cr, 12.0f * dp, D2D1::ColorF(0.02f, 0.02f, 0.03f, 0.35f), D2D1::ColorF(0.02f, 0.02f, 0.03f, 0.92f), false);
            r.FillRoundedGradientRect(cr, 12.0f * dp, ColorWithAlpha(th.accent, 0.0f), ColorWithAlpha(th.accent, 0.10f), true);
            r.DrawRoundedRect(cr, 12.0f * dp, vm.resumeCard.isResumeHovered || vm.resumeCard.isResumeFocused ? th.accent : ColorWithAlpha(th.accent, 0.30f), 1.0f * dp);
        }
        if (vm.resumeCard.pLivePreviewBitmap && vm.resumeCard.previewOpacity > 0.01f) {
            r.DrawBitmapRounded(vm.resumeCard.cardRect, 12.0f * dp, vm.resumeCard.pLivePreviewBitmap, vm.resumeCard.previewOpacity * 0.45f, true);
        }
        r.DrawDropShadow(vm.resumeCard.thumbRect, 8.0f * dp, 14.0f * dp, 4.0f * dp, D2D1::ColorF(0, 0, 0, 0.55f));
        r.FillRoundedRect(vm.resumeCard.thumbRect, 8.0f * dp, D2D1::ColorF(0.08f, 0.10f, 0.13f, 0.90f));
        if (bm) {
            r.DrawBitmapRounded(vm.resumeCard.thumbRect, 8.0f * dp, bm.Get(), 1.0f, true);
        } else { r.DrawIcon(IconType::MediaFilm, InsetRectF(vm.resumeCard.thumbRect, 60.0f * dp, 30.0f * dp), th.accentSubtle); }
        // Live hover preview fades in over the static thumbnail.
        if (vm.resumeCard.pLivePreviewBitmap && vm.resumeCard.previewOpacity > 0.01f)
            r.DrawBitmapRounded(vm.resumeCard.thumbRect, 8.0f * dp, vm.resumeCard.pLivePreviewBitmap, vm.resumeCard.previewOpacity, true);
        r.DrawRoundedRect(vm.resumeCard.thumbRect, 8.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), 1.0f);
        float playBtnSz = 36.0f * dp;
        float pCX = (vm.resumeCard.thumbRect.left + vm.resumeCard.thumbRect.right) * 0.5f;
        float pCY = (vm.resumeCard.thumbRect.top + vm.resumeCard.thumbRect.bottom) * 0.5f;
        r.FillCircle(pCX, pCY, playBtnSz * 0.5f, th.accent);
        D2D1_RECT_F pIconRect = D2D1::RectF(pCX - 7.0f * dp, pCY - 7.0f * dp, pCX + 7.0f * dp, pCY + 7.0f * dp);
        r.DrawIcon(IconType::Play, pIconRect, D2D1::ColorF(0.06f, 0.05f, 0.04f, 1.0f));
        if (vm.resumeCard.progressRatio > 0.0) {
            float thumbW = RectWidth(vm.resumeCard.thumbRect);
            D2D1_RECT_F barRect = D2D1::RectF(vm.resumeCard.thumbRect.left, vm.resumeCard.thumbRect.bottom - 3.0f * dp, vm.resumeCard.thumbRect.left + thumbW * (float)vm.resumeCard.progressRatio, vm.resumeCard.thumbRect.bottom);
            r.FillRect(barRect, th.accent);
        }
        r.DrawString(vm.resumeCard.tagText, vm.resumeCard.tagRect, th.accent, FontType::Caption, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        r.DrawString(vm.resumeCard.title, vm.resumeCard.titleRect, th.text, FontType::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        r.DrawString(vm.resumeCard.timeText, vm.resumeCard.timeRect, th.textMuted, FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    curY += resumeH + 16.0f * dp;
    float tbH = 32.0f * dp;
    D2D1_RECT_F tbRect = D2D1::RectF(vm.listClipBounds.left, curY, paneRight, curY + tbH);
    float searchW = 230.0f * dp;
    D2D1_RECT_F searchRect = D2D1::RectF(tbRect.left, tbRect.top, tbRect.left + searchW, tbRect.bottom);
    r.FillRoundedRect(searchRect, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.03f));
    r.DrawRoundedRect(searchRect, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), 1.0f);
    D2D1_RECT_F searchIconRect = D2D1::RectF(searchRect.left + 8.0f * dp, searchRect.top + 8.0f * dp, searchRect.left + 24.0f * dp, searchRect.top + 24.0f * dp);
    r.DrawIcon(IconType::Search, searchIconRect, th.textMuted);
    D2D1_RECT_F searchTxtRect = D2D1::RectF(searchRect.left + 30.0f * dp, searchRect.top, searchRect.right - 8.0f * dp, searchRect.bottom);
    DrawTextInput(r, s, searchTxtRect, vm.searchVisual, L"Filter episodes\x2026", FontType::Small, 0.0f);
    std::wstring countStr = std::to_wstring(vm.visibleCount) + L" videos";
    D2D1_RECT_F epCountRect = D2D1::RectF(searchRect.right + 14.0f * dp, tbRect.top, searchRect.right + 160.0f * dp, tbRect.bottom);
    r.DrawString(countStr, epCountRect, th.textMuted, FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    float toggleW = 64.0f * dp;
    D2D1_RECT_F toggleRect = D2D1::RectF(tbRect.right - toggleW, tbRect.top, tbRect.right, tbRect.bottom);
    r.FillRoundedRect(toggleRect, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.03f));
    r.DrawRoundedRect(toggleRect, 7.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), 1.0f);
    D2D1_RECT_F listBtnRect = D2D1::RectF(toggleRect.left + 2.0f * dp, toggleRect.top + 2.0f * dp, toggleRect.left + toggleW * 0.5f - 1.0f * dp, toggleRect.bottom - 2.0f * dp);
    D2D1_RECT_F gridBtnRect = D2D1::RectF(toggleRect.left + toggleW * 0.5f + 1.0f * dp, toggleRect.top + 2.0f * dp, toggleRect.right - 2.0f * dp, toggleRect.bottom - 2.0f * dp);
    // Sliding selection pill for List vs Grid
    const float vPos = std::clamp(vm.viewModeIndicatorPos, 0.0f, 1.0f);
    const float vLeft = listBtnRect.left + vPos * (gridBtnRect.left - listBtnRect.left);
    const float vRight = listBtnRect.right + vPos * (gridBtnRect.right - listBtnRect.right);
    const D2D1_RECT_F vPill = D2D1::RectF(vLeft, listBtnRect.top, vRight, listBtnRect.bottom);
    r.FillRoundedRect(vPill, 5.0f * dp, LerpColor(th.cardBg, th.text, 0.10f));
    r.DrawRoundedRect(vPill, 5.0f * dp, ColorWithAlpha(th.accent, 0.38f), 1.0f);
    r.DrawIcon(IconType::FolderPlaylist, InsetRectF(listBtnRect, 6.0f * dp, 6.0f * dp), (1.0f - vPos) > 0.5f ? th.accent : th.textMuted);
    r.DrawIcon(IconType::MediaFilm, InsetRectF(gridBtnRect, 6.0f * dp, 6.0f * dp), vPos > 0.5f ? th.accent : th.textMuted);
    curY += tbH + 12.0f * dp;
    r.PushClip(D2D1::RectF(vm.listClipBounds.left, curY, vm.listClipBounds.right, vm.listClipBounds.bottom));
    if (vm.hasDragClone) {
        r.FillRoundedRect(vm.dropSlotRect, 9.0f * dp, ColorWithAlpha(th.accent, 0.08f));
        r.DrawDashedRoundedRect(vm.dropSlotRect, 9.0f * dp, ColorWithAlpha(th.accent, 0.7f), 1.5f);
    }
    const VisiblePlaylistEpisodeViewModel* popoverEp = nullptr;
    float popoverExpandP = 0.0f;
    for (const auto& ep : vm.visibleEpisodes) {
        SurfaceStyle iStyle;
        iStyle.radius = 9.0f * dp;
        if (ep.isSelected) {
            iStyle.fill = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f);
            iStyle.border = th.accent;
            iStyle.borderWidth = 1.5f;
            iStyle.glowColor = ColorWithAlpha(th.accent, 0.22f);
            iStyle.glowBlur = 6.0f * dp;
            r.DrawSurface(ep.itemRect, iStyle);
        } else if (ep.isHovered) {
            iStyle.fill = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.04f);
            iStyle.border = th.accent;
            iStyle.borderWidth = 1.0f;
            iStyle.glowColor = ColorWithAlpha(th.accent, 0.15f);
            iStyle.glowBlur = 4.0f * dp;
            r.DrawSurface(ep.itemRect, iStyle);
        } else {
            iStyle.fill = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.02f);
            iStyle.border = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f);
            iStyle.borderWidth = 1.0f;
            r.DrawSurface(ep.itemRect, iStyle);
        }
        if (vm.gridView) {
            float thumbPad = 8.0f * dp;
            float thumbW = std::max(100.0f * dp, RectWidth(ep.itemRect) - thumbPad * 2.0f);
            float thumbH = thumbW * 9.0f / 16.0f;
            thumbH = std::min(104.0f * dp, thumbH);
            D2D1_RECT_F thumbRect = D2D1::RectF(ep.itemRect.left + thumbPad, ep.itemRect.top + thumbPad,
                                               ep.itemRect.left + thumbPad + thumbW, ep.itemRect.top + thumbPad + thumbH);

            const float textLeft = ep.itemRect.left + thumbPad;
            const float textRight = ep.itemRect.right - thumbPad;
            const float normalTextTop = thumbRect.bottom + 7.0f * dp;
            const float maxTextBottom = ep.itemRect.bottom - 6.0f * dp;
            const float textWidth = SafeWidth(textLeft, textRight);
            const D2D1_COLOR_F titleCol = ep.isHovered ? th.accent : th.text;

            const float expandP = std::clamp(ep.titleExpandProgress, 0.0f, 1.0f);
            const std::wstring singleLineText = FitSingleLine(r, ep.title, FontType::Medium, textWidth);
            const bool isLongTitle = (singleLineText != ep.title);

            // Dynamic Hover Behavior Determination:
            // Measure title at card's existing text width using same font, size, wrapping rules, and DirectWrite layout.
            const float measuredH = isLongTitle ? r.MeasureStringWrappedHeight(ep.title, FontType::Medium, textWidth) : 23.0f * dp;
            const float availableBase = std::max(0.0f, maxTextBottom - (normalTextTop + 2.5f * dp));
            const float maxBorrow = 0.20f * thumbH;
            const float availableWithBorrow = availableBase + maxBorrow;

            int hoverState = 1;
            float borrowNeeded = 0.0f;

            if (!isLongTitle || measuredH <= availableBase) {
                // State 1: Use existing empty title/card space
                hoverState = 1;
                borrowNeeded = 0.0f;
            } else if (measuredH <= availableWithBorrow) {
                // State 2: Borrow from thumbnail (up to 20%)
                hoverState = 2;
                borrowNeeded = std::min(maxBorrow, (measuredH - availableBase) + 2.0f * dp);
            } else {
                // State 3: Floating hover card (popover)
                hoverState = 3;
                borrowNeeded = 0.0f;
                if (ep.isHovered && expandP > 0.0f) {
                    popoverEp = &ep;
                    popoverExpandP = expandP;
                }
            }

            // Thumbnail rendering:
            // In State 2, active borrow contracts visible thumbnail bottom smoothly.
            const float activeBorrow = (hoverState == 2) ? (borrowNeeded * expandP) : 0.0f;
            const D2D1_RECT_F visibleThumbRect = D2D1::RectF(thumbRect.left, thumbRect.top, thumbRect.right, thumbRect.bottom - activeBorrow);

            r.FillRoundedRect(visibleThumbRect, 7.0f * dp, D2D1::ColorF(0.08f, 0.10f, 0.13f, 0.90f));
            ComPtr<ID2D1Bitmap> bm = r.GetOrCreateVideoThumbnail(ep.filePath, 240, 135);
            if (bm) {
                if (activeBorrow > 0.0f) {
                    r.DrawThumbnailCroppedBottom(visibleThumbRect, thumbRect, 7.0f * dp, bm.Get(), 1.0f);
                } else {
                    r.DrawBitmapRounded(visibleThumbRect, 7.0f * dp, bm.Get(), 1.0f, true);
                }
            } else {
                r.PushClip(visibleThumbRect);
                r.DrawIcon(IconType::MediaFilm, InsetRectF(thumbRect, 36.0f * dp, 20.0f * dp), th.accentSubtle);
                r.PopClip();
            }
            r.DrawRoundedRect(visibleThumbRect, 7.0f * dp, D2D1::ColorF(1, 1, 1, 0.10f), 1.0f);

            if (ep.progressRatio > 0.0) {
                r.FillRect(D2D1::RectF(visibleThumbRect.left, visibleThumbRect.bottom - 3.0f * dp,
                                       visibleThumbRect.left + thumbW * (float)ep.progressRatio, visibleThumbRect.bottom), th.accent);
            }
            D2D1_RECT_F badgeRect = D2D1::RectF(thumbRect.left + 5.0f * dp, thumbRect.top + 5.0f * dp, thumbRect.left + 25.0f * dp, thumbRect.top + 21.0f * dp);
            DrawMetadataPill(r, s, badgeRect, std::to_wstring(ep.originalIndex + 1));

            // Duration on bottom right:
            // In State 1 and State 2, smoothly fades out and slides down on hover.
            // In State 3, card internal layout is NOT modified, so duration remains visible.
            if (hoverState != 3 && expandP < 1.0f) {
                const std::wstring dur = FormatDuration(ep.durationSec);
                const float durAlpha = (1.0f - expandP);
                const float durSlideY = 3.0f * dp * expandP;
                const D2D1_RECT_F durRect = D2D1::RectF(textLeft, ep.itemRect.bottom - 24.0f * dp + durSlideY,
                                                        textRight, ep.itemRect.bottom - 7.0f * dp + durSlideY);
                r.DrawString(dur, durRect, ColorWithAlpha(th.textMuted, th.textMuted.a * durAlpha),
                             FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            } else if (hoverState == 3) {
                const std::wstring dur = FormatDuration(ep.durationSec);
                const D2D1_RECT_F durRect = D2D1::RectF(textLeft, ep.itemRect.bottom - 24.0f * dp,
                                                        textRight, ep.itemRect.bottom - 7.0f * dp);
                r.DrawString(dur, durRect, th.textMuted,
                             FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            // Title rendering:
            if (hoverState == 3 || expandP <= 0.0f || !isLongTitle) {
                // Collapsed resting single-line (or State 3 where card internal layout is not modified)
                const D2D1_RECT_F episodeTitleRect = D2D1::RectF(textLeft, normalTextTop, textRight, normalTextTop + 23.0f * dp);
                r.DrawString(singleLineText, episodeTitleRect, titleCol, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            } else {
                // State 1 or State 2 expansion:
                const float currentTextTop = normalTextTop - activeBorrow;
                const D2D1_RECT_F singleLineRect = D2D1::RectF(textLeft, currentTextTop, textRight, currentTextTop + 23.0f * dp);
                const D2D1_RECT_F fullRect = D2D1::RectF(textLeft, currentTextTop + 2.5f * dp, textRight, maxTextBottom);

                if (expandP >= 1.0f) {
                    r.DrawStringWrapped(ep.title, fullRect, titleCol, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                } else {
                    r.DrawString(singleLineText, singleLineRect, ColorWithAlpha(titleCol, titleCol.a * (1.0f - expandP)),
                                 FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    r.DrawStringWrapped(ep.title, fullRect, ColorWithAlpha(titleCol, titleCol.a * expandP),
                                        FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                }
            }
            continue;
        }
        float dotRadius = 1.6f * dp, dotCX1 = ep.itemRect.left + 10.0f * dp, dotCX2 = ep.itemRect.left + 16.0f * dp;
        float dotCY1 = ep.itemRect.top + RectHeight(ep.itemRect) * 0.5f - 7.0f * dp;
        float dotCY2 = ep.itemRect.top + RectHeight(ep.itemRect) * 0.5f;
        float dotCY3 = ep.itemRect.top + RectHeight(ep.itemRect) * 0.5f + 7.0f * dp;
        D2D1_COLOR_F dotCol = ep.isHovered ? th.accent : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f);
        r.FillCircle(dotCX1, dotCY1, dotRadius, dotCol);
        r.FillCircle(dotCX2, dotCY1, dotRadius, dotCol);
        r.FillCircle(dotCX1, dotCY2, dotRadius, dotCol);
        r.FillCircle(dotCX2, dotCY2, dotRadius, dotCol);
        r.FillCircle(dotCX1, dotCY3, dotRadius, dotCol);
        r.FillCircle(dotCX2, dotCY3, dotRadius, dotCol);
        float thumbW = 90.0f * dp, thumbH = 50.0f * dp, thumbX = ep.itemRect.left + 28.0f * dp;
        float thumbY = ep.itemRect.top + (RectHeight(ep.itemRect) - thumbH) * 0.5f;
        D2D1_RECT_F thumbRect = D2D1::RectF(thumbX, thumbY, thumbX + thumbW, thumbY + thumbH);
        r.FillRoundedRect(thumbRect, 6.0f * dp, D2D1::ColorF(0.08f, 0.10f, 0.13f, 0.90f));
        ComPtr<ID2D1Bitmap> bm = r.GetOrCreateVideoThumbnail(ep.filePath, 180, 100);
        if (bm) {
            r.DrawBitmapRounded(thumbRect, 6.0f * dp, bm.Get(), 1.0f, true);
        } else { r.DrawIcon(IconType::MediaFilm, InsetRectF(thumbRect, 22.0f * dp, 12.0f * dp), th.accentSubtle); }
        r.DrawRoundedRect(thumbRect, 6.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), 1.0f);
        std::wstring idxStr = std::to_wstring(ep.originalIndex + 1);
        float badgeW = 18.0f * dp, badgeH = 14.0f * dp;
        D2D1_RECT_F badgeRect = D2D1::RectF(thumbRect.left + 4.0f * dp, thumbRect.top + 4.0f * dp, thumbRect.left + 4.0f * dp + badgeW, thumbRect.top + 4.0f * dp + badgeH);
        DrawMetadataPill(r, s, badgeRect, idxStr);
        if (ep.progressRatio > 0.0) {
            D2D1_RECT_F epBarRect = D2D1::RectF(thumbRect.left, thumbRect.bottom - 2.5f * dp, thumbRect.left + thumbW * (float)ep.progressRatio, thumbRect.bottom);
            r.FillRect(epBarRect, th.accent);
        }
        D2D1_RECT_F tRect = D2D1::RectF(thumbRect.right + 14.0f * dp, ep.itemRect.top + 10.0f * dp, ep.itemRect.right - 100.0f * dp, ep.itemRect.top + 30.0f * dp);
        r.DrawString(FitSingleLine(r, ep.title, FontType::Medium, SafeWidth(tRect.left, tRect.right)), tRect, ep.isHovered ? th.accent : th.text, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F pRect = D2D1::RectF(thumbRect.right + 14.0f * dp, ep.itemRect.top + 30.0f * dp, ep.itemRect.right - 100.0f * dp, ep.itemRect.top + 48.0f * dp);
        r.DrawString(FitSingleLine(r, ep.subtitle, FontType::Small, SafeWidth(pRect.left, pRect.right)), pRect, th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        std::wstring durStr = FormatDuration(ep.durationSec);
        D2D1_RECT_F durRect = D2D1::RectF(ep.itemRect.right - 90.0f * dp, ep.itemRect.top, ep.itemRect.right - 36.0f * dp, ep.itemRect.bottom);
        r.DrawString(durStr, durRect, th.textMuted, FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (ep.isHovered) {
            float delBtnSize = 26.0f * dp;
            D2D1_RECT_F delBtnRect = D2D1::RectF(ep.itemRect.right - 32.0f * dp, ep.itemRect.top + (RectHeight(ep.itemRect) - delBtnSize) * 0.5f, ep.itemRect.right - 6.0f * dp, ep.itemRect.top + (RectHeight(ep.itemRect) + delBtnSize) * 0.5f);
            if (ep.isDeleteHovered) {
                r.FillRoundedRect(delBtnRect, 5.0f * dp, D2D1::ColorF(0.95f, 0.25f, 0.35f, 0.25f));
                r.DrawRoundedRect(delBtnRect, 5.0f * dp, D2D1::ColorF(0.95f, 0.25f, 0.35f, 0.70f), 1.0f);
            }
            r.DrawIcon(IconType::Trash, InsetRectF(delBtnRect, 5.0f * dp, 5.0f * dp), ep.isDeleteHovered ? D2D1::ColorF(0.95f, 0.25f, 0.35f) : th.textMuted);
        }
    }
    if (vm.hasDragClone) {
        SurfaceStyle dragStyle;
        dragStyle.radius = 9.0f * dp;
        dragStyle.fill = ColorWithAlpha(th.surfaceFloating, 0.95f);
        dragStyle.border = th.accent;
        dragStyle.borderWidth = 1.5f;
        r.DrawSurface(vm.dragCloneRect, dragStyle);
        if (vm.gridView) {
            const float pad = 8.0f * dp;
            const float width = RectWidth(vm.dragCloneRect) - 2 * pad;
            const float height = std::min(104.0f * dp, width * 9.0f / 16.0f);
            auto thumb = D2D1::RectF(vm.dragCloneRect.left + pad, vm.dragCloneRect.top + pad,
                vm.dragCloneRect.right - pad, vm.dragCloneRect.top + pad + height);
            r.FillRoundedRect(thumb, 7.0f * dp, th.surfaceElevated);
            auto bitmap = r.GetOrCreateVideoThumbnail(vm.dragCloneEpisode.filePath, 240, 135);
            if (bitmap) r.DrawBitmapRounded(thumb, 7.0f * dp, bitmap.Get(), 1.0f, true);
            else r.DrawIcon(IconType::MediaFilm, InsetRectF(thumb, 36.0f * dp, 20.0f * dp), th.accentSubtle);
            auto title = D2D1::RectF(thumb.left, thumb.bottom + 7.0f * dp, thumb.right, thumb.bottom + 30.0f * dp);
            r.DrawString(FitSingleLine(r, vm.dragCloneEpisode.title, FontType::Medium, width), title,
                th.text, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else {
        float thumbW = 90.0f * dp, thumbH = 50.0f * dp, thumbX = vm.dragCloneRect.left + 28.0f * dp;
        float thumbY = vm.dragCloneRect.top + (RectHeight(vm.dragCloneRect) - thumbH) * 0.5f;
        D2D1_RECT_F thumbRect = D2D1::RectF(thumbX, thumbY, thumbX + thumbW, thumbY + thumbH);
        r.FillRoundedRect(thumbRect, 6.0f * dp, ColorWithAlpha(th.surfaceElevated, 0.90f));
        ComPtr<ID2D1Bitmap> bm = r.GetOrCreateVideoThumbnail(vm.dragCloneEpisode.filePath, 180, 100);
        if (bm) {
            r.DrawBitmapRounded(thumbRect, 6.0f * dp, bm.Get(), 1.0f, true);
        } else { r.DrawIcon(IconType::MediaFilm, InsetRectF(thumbRect, 22.0f * dp, 12.0f * dp), th.accentSubtle); }
        D2D1_RECT_F tRect = D2D1::RectF(thumbRect.right + 14.0f * dp, vm.dragCloneRect.top + 10.0f * dp, vm.dragCloneRect.right - 100.0f * dp, vm.dragCloneRect.top + 30.0f * dp);
        r.DrawString(vm.dragCloneEpisode.title, tRect, th.accent, FontType::Medium, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F pRect = D2D1::RectF(thumbRect.right + 14.0f * dp, vm.dragCloneRect.top + 30.0f * dp, vm.dragCloneRect.right - 100.0f * dp, vm.dragCloneRect.top + 48.0f * dp);
        r.DrawString(vm.dragCloneEpisode.subtitle, pRect, th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    r.PopClip();
    if (vm.isDropTargetActive) {
        r.FillRoundedRect(vm.listClipBounds, 14.0f * dp, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.03f));
        r.DrawDashedRoundedRect(vm.listClipBounds, 14.0f * dp, th.accent, 2.0f);
        D2D1_RECT_F bannerRect = D2D1::RectF(paneRight - 360.0f * dp, vm.listClipBounds.top + 6.0f * dp, paneRight, vm.listClipBounds.top + 38.0f * dp);
        r.FillRoundedRect(bannerRect, 7.0f * dp, ColorWithAlpha(th.surfaceFloating, 0.95f));
        r.DrawRoundedRect(bannerRect, 7.0f * dp, th.accent, 1.5f);
        std::wstring dropLabel = vm.isFolderDrop
            ? L"+ Drop folder to add its videos into \"" + Utf8ToUtf16(vm.playlistName) + L"\""
            : L"+ Drop files here to add into \"" + Utf8ToUtf16(vm.playlistName) + L"\"";
        r.DrawString(dropLabel, bannerRect, th.accent, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // State 3: Floating Hover Card (Browser-tab style unclipped popover)
    if (popoverEp && popoverExpandP > 0.001f) {
        DrawPlaylistCardHoverPopover(r, s, *popoverEp, popoverExpandP);
    }
}

void UIComponents::DrawHistoryList(Renderer& r, const Settings& s, const HistoryTabViewModel& vm) {
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    float headerY = vm.bounds.top + 26.0f * dp;
    // Header title and count on the left
    D2D1_RECT_F titleRect = D2D1::RectF(vm.bounds.left + 12.0f * dp, headerY + 4.0f * dp, vm.bounds.left + 300.0f * dp, headerY + 36.0f * dp);
    r.DrawString(L"WATCH HISTORY", titleRect, th.text, FontType::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    size_t totalHistoryCount = vm.pHistory ? vm.pHistory->size() : 0;
    if (totalHistoryCount == 0 && !vm.visibleItems.empty()) totalHistoryCount = vm.visibleItems.size();
    if (totalHistoryCount > 0) {
        std::wstring countStr = std::to_wstring(totalHistoryCount) + (totalHistoryCount == 1 ? L" item" : L" items");
        D2D1_SIZE_F titleSz = r.MeasureString(L"WATCH HISTORY", FontType::Title);
        D2D1_RECT_F countRect = D2D1::RectF(vm.bounds.left + 20.0f * dp + titleSz.width, headerY + 4.0f * dp, vm.bounds.left + 450.0f * dp, headerY + 36.0f * dp);
        r.DrawString(countStr, countRect, th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // Right-aligned Clear Watch History action with intentional destructive styling
    D2D1_SIZE_F clearSize = r.MeasureString(L"Clear Watch History", FontType::Small);
    float clearW = clearSize.width + 32.0f * dp, clearRight = vm.bounds.right - 24.0f * dp, clearLeft = clearRight - clearW;
    D2D1_RECT_F clearRect = D2D1::RectF(clearLeft, headerY + 5.0f * dp, clearRight, headerY + 35.0f * dp);
    D2D1_COLOR_F clearBg = vm.isClearPressed ? ColorWithAlpha(th.red, 0.38f) : (vm.isClearHovered ? ColorWithAlpha(th.red, 0.22f) : ColorWithAlpha(th.red, 0.10f));
    D2D1_COLOR_F clearBorder = vm.isClearPressed ? ColorWithAlpha(th.red, 0.95f) : (vm.isClearHovered ? ColorWithAlpha(th.red, 0.70f) : ColorWithAlpha(th.red, 0.35f));
    D2D1_COLOR_F clearFg = vm.isClearPressed ? D2D1::ColorF(1.0f, 1.0f, 1.0f) : (vm.isClearHovered ? th.red : ColorWithAlpha(th.red, 0.88f));
    r.FillRoundedRect(clearRect, 6.0f * dp, clearBg);
    r.DrawRoundedRect(clearRect, 6.0f * dp, clearBorder, 1.0f);
    r.DrawIcon(IconType::Trash, D2D1::RectF(clearRect.left + 10.0f * dp, clearRect.top + 8.0f * dp, clearRect.left + 23.0f * dp, clearRect.top + 21.0f * dp), clearFg);
    r.DrawString(L"Clear Watch History", D2D1::RectF(clearRect.left + 28.0f * dp, clearRect.top, clearRect.right - 8.0f * dp, clearRect.bottom), clearFg, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    r.DrawLine(vm.bounds.left, vm.bounds.top + 72.0f * dp, vm.bounds.right, vm.bounds.top + 72.0f * dp, th.border, 1.0f);
    if (vm.isEmpty || (totalHistoryCount == 0 && vm.visibleItems.empty())) { DrawEmptyState(r, s, vm.emptyState); return; }
    auto formatTimestamp = [](int64_t timestamp) -> std::wstring {
        if (timestamp <= 0) return L"";
        std::time_t tt = (std::time_t)timestamp;
        std::tm tmLocal{};
        if (localtime_s(&tmLocal, &tt) != 0) return L"";
        wchar_t buf[64]{};
        swprintf_s(buf, L"%d/%d/%d %02d:%02d", tmLocal.tm_mon + 1, tmLocal.tm_mday, tmLocal.tm_year + 1900, tmLocal.tm_hour, tmLocal.tm_min);
        return buf;
    };
    D2D1_RECT_F clip = D2D1::RectF(vm.bounds.left + 2.0f * dp, vm.bounds.top + 73.0f * dp, vm.bounds.right - 8.0f * dp, vm.bounds.bottom);
    r.PushClip(clip);
    if (vm.hasProjectedLayout || !vm.visibleItems.empty() || !vm.groups.empty()) {
        for (const auto& grp : vm.groups) {
            if (grp.headerRect.bottom >= clip.top && grp.headerRect.top <= clip.bottom) { DrawHistoryGroupHeader(r, s, grp); }
        }
        for (const auto& item : vm.visibleItems) {
            if (item.itemRect.bottom >= clip.top && item.itemRect.top <= clip.bottom) {
                SurfaceStyle rowStyle;
                rowStyle.radius = 12.0f * dp;
                rowStyle.fill = item.isSelected ? ColorWithAlpha(th.accent, 0.10f) : (item.isHovered ? D2D1::ColorF(1, 1, 1, 0.05f) : th.cardBg);
                rowStyle.border = item.isSelected ? th.accent : (item.isHovered ? D2D1::ColorF(1, 1, 1, 0.16f) : th.border);
                rowStyle.borderWidth = item.isSelected ? 1.5f : 1.0f;
                rowStyle.shadowColor = D2D1::ColorF(0, 0, 0, item.isHovered ? 0.20f : 0.06f);
                rowStyle.shadowBlur = item.isHovered ? 12.0f * dp : 4.0f * dp;
                rowStyle.shadowOffsetY = item.isHovered ? 2.0f * dp : 1.0f * dp;
                r.DrawSurface(item.itemRect, rowStyle);
                if (item.isKeyboardFocused)
                    r.DrawRoundedRect(InsetRectF(item.itemRect, 2.5f * dp, 2.5f * dp), 9.5f * dp, th.focusRing, 1.5f);
                std::wstring title = FitSingleLine(r, item.displayTitle, FontType::Subheading, SafeWidth(item.itemRect.left + 16.0f * dp, item.itemRect.right - 200.0f * dp));
                r.DrawString(title, D2D1::RectF(item.itemRect.left + 16.0f * dp, item.itemRect.top + 9.0f * dp, item.itemRect.right - 190.0f * dp, item.itemRect.top + 34.0f * dp), item.isHovered ? th.accent : th.text, FontType::Subheading, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                std::wstring date = formatTimestamp(item.pRecord->watchedTimestamp);
                r.DrawString(date, D2D1::RectF(item.itemRect.right - 180.0f * dp, item.itemRect.top + 9.0f * dp, item.itemRect.right - 16.0f * dp, item.itemRect.top + 34.0f * dp), th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                r.DrawString(item.formattedDuration.empty() ? FormatHistoryProgress(*item.pRecord) : item.formattedDuration, D2D1::RectF(item.itemRect.left + 16.0f * dp, item.itemRect.top + 38.0f * dp, item.itemRect.left + 250.0f * dp, item.itemRect.bottom - 9.0f * dp), item.pRecord->lastPosSec > 0.0 ? th.accent : th.textMuted, FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                if (item.isHovered) {
                    const float delSize = 28.0f * dp;
                    D2D1_RECT_F delRect = D2D1::RectF(item.itemRect.right - delSize - 10.0f * dp,
                        item.itemRect.top + (RectHeight(item.itemRect) - delSize) * 0.5f, item.itemRect.right - 10.0f * dp,
                        item.itemRect.top + (RectHeight(item.itemRect) + delSize) * 0.5f);
                    if (item.isDeleteHovered) {
                        r.FillRoundedRect(delRect, 6.0f * dp, ColorWithAlpha(th.red, 0.22f));
                        r.DrawRoundedRect(delRect, 6.0f * dp, ColorWithAlpha(th.red, 0.74f), 1.0f);
                    }
                    r.DrawIcon(IconType::CloseX, InsetRectF(delRect, 7.0f * dp, 7.0f * dp), item.isDeleteHovered ? th.red : th.textMuted);
                }
            }
        }
    } else {
        static const std::vector<HistoryRecord> kEmptyHistory;
        auto proj = ProjectHistoryTimeline(vm.pHistory ? *vm.pHistory : kEmptyHistory);
        float groupHeaderY = vm.bounds.top + kHistoryContentInset * dp - vm.scrollY;
        for (size_t g = 0; g < proj.groups.size(); ++g) {
            HistoryGroupSummary grp = proj.groups[g];
            const HistoryGroupLayout layout(groupHeaderY, grp.count, dp);
            groupHeaderY = layout.NextHeaderTop();
            grp.headerRect = D2D1::RectF(vm.bounds.left + 12.0f * dp, layout.headerTop,
                vm.bounds.right - 24.0f * dp, layout.headerTop + layout.headerHeight);
            if (grp.headerRect.bottom >= clip.top && grp.headerRect.top <= clip.bottom) { DrawHistoryGroupHeader(r, s, grp); }
            const auto [firstRow, lastRow] = layout.VisibleRows(clip.top, clip.bottom);
            for (size_t row = firstRow; row < lastRow; ++row) {
                const size_t idx = grp.startIndex + row;
                size_t origIdx = proj.itemIndices[idx];
                if (!vm.pHistory || origIdx >= vm.pHistory->size()) continue;
                const auto& rec = (*vm.pHistory)[origIdx];
                const float rowY = layout.RowTop(row);
                D2D1_RECT_F itemRect = D2D1::RectF(vm.bounds.left + 12.0f * dp, rowY,
                    vm.bounds.right - 24.0f * dp, rowY + layout.rowHeight);
                if (itemRect.bottom >= clip.top && itemRect.top <= clip.bottom) {
                    bool hovered = (vm.hoveredIndex == (int)origIdx);
                    bool selected = (vm.selectedIndices.count(origIdx) > 0);
                    SurfaceStyle rowStyle;
                    rowStyle.radius = 12.0f * dp;
                    rowStyle.fill = selected ? ColorWithAlpha(th.accent, 0.10f) : (hovered ? D2D1::ColorF(1, 1, 1, 0.05f) : th.cardBg);
                    rowStyle.border = selected ? th.accent : (hovered ? D2D1::ColorF(1, 1, 1, 0.16f) : th.border);
                    rowStyle.borderWidth = selected ? 1.5f : 1.0f;
                    rowStyle.shadowColor = D2D1::ColorF(0, 0, 0, hovered ? 0.20f : 0.06f);
                    rowStyle.shadowBlur = hovered ? 12.0f * dp : 4.0f * dp;
                    rowStyle.shadowOffsetY = hovered ? 2.0f * dp : 1.0f * dp;
                    r.DrawSurface(itemRect, rowStyle);
                    std::wstring titleText = rec.title.empty() ? GetFileNameFromPath(rec.filePath) : rec.title;
                    std::wstring title = FitSingleLine(r, titleText, FontType::Subheading, SafeWidth(itemRect.left + 16.0f * dp, itemRect.right - 200.0f * dp));
                    r.DrawString(title, D2D1::RectF(itemRect.left + 16.0f * dp, itemRect.top + 9.0f * dp, itemRect.right - 190.0f * dp, itemRect.top + 34.0f * dp), hovered ? th.accent : th.text, FontType::Subheading, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    std::wstring date = formatTimestamp(rec.watchedTimestamp);
                    r.DrawString(date, D2D1::RectF(itemRect.right - 180.0f * dp, itemRect.top + 9.0f * dp, itemRect.right - 16.0f * dp, itemRect.top + 34.0f * dp), th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    r.DrawString(FormatHistoryProgress(rec), D2D1::RectF(itemRect.left + 16.0f * dp, itemRect.top + 38.0f * dp, itemRect.left + 250.0f * dp, itemRect.bottom - 9.0f * dp), rec.lastPosSec > 0.0 ? th.accent : th.textMuted, FontType::MonoSmall, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    if (hovered) {
                        const float delSize = 28.0f * dp;
                        D2D1_RECT_F delRect = D2D1::RectF(itemRect.right - delSize - 10.0f * dp,
                            itemRect.top + (layout.rowHeight - delSize) * 0.5f,
                            itemRect.right - 10.0f * dp, itemRect.top + (layout.rowHeight + delSize) * 0.5f);
                        bool delHover = (vm.hoveredDeleteIndex == (int)origIdx);
                        if (delHover) {
                            r.FillRoundedRect(delRect, 6.0f * dp, ColorWithAlpha(th.red, 0.22f));
                            r.DrawRoundedRect(delRect, 6.0f * dp, ColorWithAlpha(th.red, 0.74f), 1.0f);
                        }
                        r.DrawIcon(IconType::CloseX, InsetRectF(delRect, 7.0f * dp, 7.0f * dp), delHover ? th.red : th.textMuted);
                    }
                }
            }
        }
    }
    r.PopClip();
}

void UIComponents::DrawOSDBadge(Renderer& r, const Settings& s, const OsdBadgeViewModel& vm) {
    if (!vm.visible) return;
    // Hub toasts keep the compact pill style.
    DrawOSDBadge(r, s, vm.bounds, vm.icon, vm.text, vm.opacity, true);
}

void UIComponents::DrawOSDBadge(OverlayRenderer& r, const Settings& s, const OsdBadgeViewModel& vm) {
    if (!vm.visible) return;
    // The player badge sizes by presentation: compact pill in PiP, full badge otherwise.
    DrawOSDBadge(r, s, vm.bounds, vm.icon, vm.text, vm.opacity, vm.isPiP);
}

template <typename R>
static void DrawSplitIrisContextMenuImpl(R& r, const Settings& s, const ContextMenuViewModel& vm) {
    namespace layout = SplitIrisMenuLayout;
    const float dp = r.GetDpiScale();
    const float alpha = std::clamp(vm.animProgress, 0.0f, 1.0f);
    constexpr float pi = 3.14159265358979323846f;
    const auto px = [&](float x) { return vm.origin.x + x * dp; };
    const auto py = [&](float y) { return vm.origin.y + y * dp; };
    // Scale the entire drawing about the iris, including text and icons.
    const auto transform = r.ApplyTransform(D2D1::Matrix3x2F::Scale(
        layout::Scale(alpha), layout::Scale(alpha), D2D1::Point2F(px(layout::Center), py(layout::Center))));
    const auto box = [&](float x0, float y0, float x1, float y1) {
        return D2D1::RectF(px(x0), py(y0), px(x1), py(y1));
    };
    const D2D1_RECT_F svgBox = box(0, 0, layout::Width, layout::Width);
    const auto color = [&](UINT32 rgb, float opacity = 1.0f) { return D2D1::ColorF(rgb, alpha * opacity); };
    const auto accent = [&](float opacity) { return ColorWithAlpha(s.currentTheme.accent, alpha * opacity); };
    const auto text = color(0xe6e7ea), muted = color(0x9aa0a6), shortcut = color(0x757b82);
    const auto danger = color(0xf26d6d);

    // One opaque plate under the arch, iris and body. Adjacent antialiased
    // cutouts leave translucent pixels around the iris and reveal the video.
    const float bottom = layout::Bottom(vm.items);
    char bodyPath[256];
    std::snprintf(bodyPath, sizeof(bodyPath),
        "M 0.5 119 A 118.5 118.5 0 0 1 237.5 119 L 237.5 %.3f A 10 10 0 0 1 227.5 %.3f L 10.5 %.3f A 10 10 0 0 1 0.5 %.3f Z",
        bottom - 10.5f, bottom - 0.5f, bottom - 0.5f, bottom - 10.5f);
    r.DrawSvgPath(bodyPath, svgBox, color(0x1a1b1e), layout::Width, true);
    r.DrawSvgPath(bodyPath, svgBox, color(0xffffff, 0.09f), layout::Width, false, dp);

    // These are the reference's two shoulder paths, translated by the body's border.
    const char* shoulderPaths[] = {
        "M 12 125 L 76.3 125 A 43 43 0 0 0 95.6 156 L 12 156 A 5 5 0 0 1 7 151 L 7 130 A 5 5 0 0 1 12 125 Z",
        "M 161.7 125 L 226 125 A 5 5 0 0 1 231 130 L 231 151 A 5 5 0 0 1 226 156 L 142.4 156 A 43 43 0 0 0 161.7 125 Z"
    };
    const IrisShoulderButton* shoulders[] = { &vm.leftShoulder, &vm.rightShoulder };
    for (int i = 0; i < 2; ++i) {
        const auto& button = *shoulders[i];
        if (button.label.empty()) continue;
        const bool hovered = vm.hoveredShoulder == i + 1 && !button.isDisabled;
        r.DrawSvgPath(shoulderPaths[i], svgBox, color(0xffffff, hovered ? 0.09f : 0.035f), layout::Width, true);
        r.DrawSvgPath(shoulderPaths[i], svgBox, color(0xffffff, hovered ? 0.16f : 0.07f), layout::Width, false, dp);
        constexpr float iconSize = 12.0f, gap = 6.0f;
        const float labelLeft = i == 0 ? 7.0f : 159.0f;
        const float labelRight = labelLeft + 72.0f;
        const float iconSpace = button.hasIcon ? iconSize + gap : 0.0f;
        const std::wstring fitted = FitSingleLine(r, button.label, FontType::MenuShoulder, (72.0f - iconSpace) * dp);
        const float textWidth = r.MeasureString(fitted, FontType::MenuShoulder).width / dp;
        const float groupLeft = (labelLeft + labelRight - textWidth - iconSpace) * 0.5f;
        const float textLeft = groupLeft + (button.hasIcon && !button.iconOnRight ? iconSpace : 0.0f);
        const auto textColor = button.isDisabled ? color(0xe6e7ea, 0.40f) : text;
        const auto iconColor = button.isDisabled ? color(0x9aa0a6, 0.38f) : muted;
        if (button.hasIcon) {
            const float iconLeft = button.iconOnRight ? groupLeft + textWidth + gap : groupLeft;
            r.DrawIcon(button.icon, box(iconLeft, 134.5f, iconLeft + iconSize, 146.5f), iconColor);
        }
        r.DrawString(fitted, box(textLeft, 125, labelRight, 156), textColor, FontType::MenuShoulder,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    r.DrawLine(px(12), py(164.5f), px(226), py(164.5f), color(0xffffff, 0.065f), dp);

    float rowTop = layout::RowsTop;
    for (size_t i = 0; i < vm.items.size(); ++i) {
        const auto& item = vm.items[i];
        if (item.isSeparator) {
            r.DrawLine(px(12), py(rowTop + 4.5f), px(226), py(rowTop + 4.5f), color(0xffffff, 0.065f), dp);
            rowTop += layout::SeparatorHeight;
            continue;
        }
        const auto row = box(6, rowTop, 232, rowTop + layout::RowHeight);
        const bool hovered = vm.hoveredIndex == static_cast<int>(i) && !item.isDisabled;
        if (hovered) r.FillRoundedRect(row, 5.0f * dp,
            item.isDanger ? color(0xf26d6d, 0.10f) : color(0xffffff, 0.07f));
        const auto iconRect = box(15, rowTop + 7.5f, 29, rowTop + 21.5f);
        const auto iconColor = item.isDisabled ? color(0x9aa0a6, 0.38f) : item.isDanger ? danger : muted;
        if (item.showCheckedIndicator) {
            if (item.isChecked) r.DrawIcon(IconType::Checkmark, iconRect, accent(1.0f));
        } else {
            r.DrawIcon(item.icon, iconRect, iconColor);
        }
        // Reserve only the measured shortcut width; arrows and single letters need very little space.
        const float shortcutWidth = item.shortcut.empty() ? 0.0f : r.MeasureString(item.shortcut, FontType::MenuShortcut).width;
        const float labelRight = px(223) - (item.shortcut.empty() ? 0.0f : shortcutWidth + 10.0f * dp);
        const auto labelRect = D2D1::RectF(px(39), row.top, std::max(px(40), labelRight), row.bottom);
        r.DrawString(FitSingleLine(r, item.label, FontType::MenuLabel, RectWidth(labelRect)), labelRect,
            item.isDisabled ? color(0xe6e7ea, 0.42f) : item.isDanger ? danger : text, FontType::MenuLabel,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (!item.shortcut.empty()) {
            r.DrawString(item.shortcut, D2D1::RectF(px(223) - shortcutWidth - dp, row.top, px(223), row.bottom),
                item.isDisabled ? color(0x757b82, 0.35f) : shortcut, FontType::MenuShortcut,
                DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        rowTop += layout::RowPitch;
    }

    if (vm.activeDialIndex >= 0) {
        const bool settledWedge = vm.activeDialIndex < static_cast<int>(vm.dials.size()) &&
            std::abs(vm.currentWedgeAngle - vm.dials[vm.activeDialIndex].centerDeg) < 0.001f;
        const float a0 = (vm.currentWedgeAngle - 18.0f) * pi / 180.0f;
        const float a1 = (vm.currentWedgeAngle + 18.0f) * pi / 180.0f;
        char wedge[256];
        std::snprintf(wedge, sizeof(wedge),
            "M %.3f %.3f A 118.5 118.5 0 0 1 %.3f %.3f L %.3f %.3f A 38 38 0 0 0 %.3f %.3f Z",
            119 + 118.5f * std::sin(a0), 119 - 118.5f * std::cos(a0),
            119 + 118.5f * std::sin(a1), 119 - 118.5f * std::cos(a1),
            119 + 38 * std::sin(a1), 119 - 38 * std::cos(a1),
            119 + 38 * std::sin(a0), 119 - 38 * std::cos(a0));
        r.DrawSvgPath(wedge, svgBox, accent(0.14f), layout::Width, true, 1.4f, settledWedge);
        const float rim0 = (vm.currentWedgeAngle - 16.5f) * pi / 180.0f;
        const float rim1 = (vm.currentWedgeAngle + 16.5f) * pi / 180.0f;
        char rim[160];
        std::snprintf(rim, sizeof(rim), "M %.3f %.3f A 118 118 0 0 1 %.3f %.3f",
            119 + 118 * std::sin(rim0), 119 - 118 * std::cos(rim0),
            119 + 118 * std::sin(rim1), 119 - 118 * std::cos(rim1));
        r.DrawSvgPath(rim, svgBox, accent(1.0f), layout::Width, false, 2.0f * dp, settledWedge);
    }
    for (int i = 1; i < 5; ++i) {
        const float angle = (-90.0f + i * 36.0f) * pi / 180.0f;
        r.DrawLine(px(119 + 38 * std::sin(angle)), py(119 - 38 * std::cos(angle)),
            px(119 + 118.5f * std::sin(angle)), py(119 - 118.5f * std::cos(angle)), color(0xffffff, 0.06f), dp);
    }
    for (int i = 0; i < 5; ++i) {
        const float angle = vm.dials[i].centerDeg * pi / 180.0f;
        const float x = 119 + 80 * std::sin(angle), y = 119 - 80 * std::cos(angle);
        const bool active = vm.activeDialIndex == i;
        const float nameWidth = r.MeasureString(vm.dials[i].name, FontType::MenuDial).width / dp + 1.0f;
        float labelY = y, widest = -1.0f;
        layout::LabelRowBounds nameBounds{};
        // Move long names towards the wider outer part of their wedge. Retain
        // the readable font size and full name whenever the sector permits it.
        for (int offset = 0; offset <= 24; ++offset) {
            const auto candidate = layout::DialLabelRow(vm.dials[i].centerDeg, y - 14 - offset, y + 1 - offset);
            if (candidate.Width() > widest) {
                widest = candidate.Width();
                nameBounds = candidate;
                labelY = y - offset;
            }
            if (candidate.Width() >= nameWidth) break;
        }
        const auto centerInRow = [](layout::LabelRowBounds bounds, float preferred, float width) {
            if (bounds.Width() <= width) return (bounds.left + bounds.right) * 0.5f;
            const float half = width * 0.5f;
            return std::clamp(preferred, bounds.left + half, bounds.right - half);
        };
        float nameX = centerInRow(nameBounds, x, nameWidth);
        const size_t wordBreak = vm.dials[i].name.find(L' ');
        if (nameBounds.Width() < nameWidth && wordBreak != std::wstring::npos) {
            // The central wedge cannot contain the full horizontal "Audio Delay"
            // at this font size. Stack the words instead of shrinking or losing it.
            for (int line = 0; line < 2; ++line) {
                const auto word = line == 0 ? vm.dials[i].name.substr(0, wordBreak) : vm.dials[i].name.substr(wordBreak + 1);
                const float top = y - 28 + line * 14;
                const auto bounds = layout::DialLabelRow(vm.dials[i].centerDeg, top, top + 14);
                const float width = r.MeasureString(word, FontType::MenuDial).width / dp + 1.0f;
                const float center = centerInRow(bounds, x, width);
                const float half = std::max(0.0f, std::min(center - bounds.left, bounds.right - center));
                const auto rect = box(center - half, top, center + half, top + 14);
                r.DrawString(FitSingleLine(r, word, FontType::MenuDial, RectWidth(rect)), rect, active ? text : muted,
                    FontType::MenuDial, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            nameX = x;
            labelY = y - 1;
        } else {
            const float nameHalf = std::max(0.0f, std::min(nameX - nameBounds.left, nameBounds.right - nameX));
            const auto nameRect = box(nameX - nameHalf, labelY - 14, nameX + nameHalf, labelY + 1);
            r.DrawString(FitSingleLine(r, vm.dials[i].name, FontType::MenuDial, RectWidth(nameRect)), nameRect, active ? text : muted,
                FontType::MenuDial, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        const auto valueBounds = layout::DialLabelRow(vm.dials[i].centerDeg, labelY + 1, labelY + 16);
        const float valueWidth = r.MeasureString(vm.dials[i].value, FontType::MenuValue).width / dp + 1.0f;
        const float valueX = centerInRow(valueBounds, nameX, valueWidth);
        const float valueHalf = std::max(0.0f, std::min(valueX - valueBounds.left, valueBounds.right - valueX));
        const auto valueRect = box(valueX - valueHalf, labelY + 1, valueX + valueHalf, labelY + 16);
        r.DrawString(FitSingleLine(r, vm.dials[i].value, FontType::MenuValue, RectWidth(valueRect)), valueRect,
            active || vm.dials[i].isModified ? accent(1.0f) : color(0x90969e), FontType::MenuValue,
            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // The reference's six tangent blades share a fill. Draw that disc and its
    // hexagonal opening first, then all six seams so later blades cannot cover them.
    constexpr float radius = layout::ChamberRadius;
    r.FillCircle(px(119), py(119), radius * dp, color(0x15171b));
    const float fraction = std::clamp(vm.apertureOpenFraction, 0.06f, 0.90f);
    const float hole = 2.6f + fraction * (radius - 7.5f);
    const float twist = std::acos(std::clamp(hole / radius, 0.08f, 0.96f)) * 0.74f;
    const float vertexOffset = hole * std::tan(pi / 6.0f);
    const float tangentExit = std::sqrt(std::max(0.0f, radius * radius - hole * hole));
    D2D1_POINT_2F vertices[6], exits[6];
    for (int i = 0; i < 6; ++i) {
        const float angle = i * pi / 3.0f + twist;
        const float nx = std::cos(angle), ny = std::sin(angle), tx = -ny, ty = nx;
        vertices[i] = D2D1::Point2F(px(119 + nx * hole - tx * vertexOffset), py(119 + ny * hole - ty * vertexOffset));
        exits[i] = D2D1::Point2F(px(119 + nx * hole + tx * tangentExit), py(119 + ny * hole + ty * tangentExit));
    }
    // Floating-point geometry avoids the old per-frame SVG rounding and cache churn.
    r.FillPolygon(vertices, 6, color(0x0b0c0f));
    for (int i = 0; i < 6; ++i) {
        r.DrawLine(vertices[i].x, vertices[i].y, exits[i].x, exits[i].y, color(0x2d323b), 1.2f * dp);
    }
    r.DrawCircle(px(119), py(119), radius * dp, color(0xffffff, 0.11f), 1.1f * dp);
}

void UIComponents::DrawContextMenu(Renderer& r, const Settings& s, const ContextMenuViewModel& vm) {
    if (!vm.isOpen) return;
    if (vm.isSplitIris) {
        DrawSplitIrisContextMenuImpl(r, s, vm);
        return;
    }
    DrawContextMenu(r, s, vm.origin, vm.items, vm.hoveredIndex, vm.animProgress);
}

void UIComponents::DrawOverlayContextMenu(OverlayRenderer& r, const Settings& s, const ContextMenuViewModel& vm) {
    if (!vm.isOpen) return;
    if (vm.isSplitIris) {
        DrawSplitIrisContextMenuImpl(r, s, vm);
        return;
    }
    DrawOverlayContextMenu(r, s, vm.origin, vm.items, vm.hoveredIndex, vm.animProgress);
}

void UIComponents::DrawImportProgress(Renderer& r, const Settings& s, const ImportProgressViewModel& vm) {
    if (!vm.isActive) return;
    float dp = r.GetDpiScale();
    const auto& th = s.currentTheme;
    r.FillRoundedRect(D2D1::RectF(0, 0, r.Scale(r.GetDpiScale() > 0 ? (float)r.GetDpiScale() : 1.0f), 0), 0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.42f));
    r.FillRoundedRect(vm.cardBounds, 14.0f * dp, th.cardBg);
    r.DrawRoundedRect(vm.cardBounds, 14.0f * dp, th.border, 1.0f * dp);
    r.DrawString(L"Importing media", D2D1::RectF(vm.cardBounds.left + 24.0f * dp, vm.cardBounds.top + 18.0f * dp, vm.cardBounds.right - 24.0f * dp, vm.cardBounds.top + 48.0f * dp), th.text, FontType::Subheading);
    r.DrawString(vm.progressText, D2D1::RectF(vm.cardBounds.left + 24.0f * dp, vm.cardBounds.top + 54.0f * dp, vm.cardBounds.right - 24.0f * dp, vm.cardBounds.top + 78.0f * dp), th.textMuted, FontType::Small);
    r.DrawProgressBar(D2D1::RectF(vm.cardBounds.left + 24.0f * dp, vm.cardBounds.top + 86.0f * dp, vm.cardBounds.right - 24.0f * dp, vm.cardBounds.top + 94.0f * dp), vm.progressRatio, D2D1::ColorF(1, 1, 1, 0.08f), th.accent, 4.0f * dp);
    r.FillRoundedRect(vm.cancelBounds, 7.0f * dp, vm.isCancelHovered ? D2D1::ColorF(0.95f, 0.28f, 0.35f, 0.22f) : D2D1::ColorF(1, 1, 1, 0.05f));
    r.DrawRoundedRect(vm.cancelBounds, 7.0f * dp, vm.isCancelHovered ? D2D1::ColorF(0.95f, 0.35f, 0.40f, 0.72f) : th.border, 1.0f * dp);
    r.DrawString(L"Cancel", vm.cancelBounds, vm.isCancelHovered ? th.red : th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void UIComponents::DrawPlayerOverlay(OverlayRenderer& r, const Settings& s, const PlayerRenderViewModel& vm) {
    r.BeginDraw();
    if (vm.subtitle.visible && !vm.subtitle.text.empty())
        r.DrawSubtitle(vm.subtitle.text, vm.subtitle.bounds, vm.subtitle.bgStyle, vm.subtitle.textColor, vm.subtitle.fontSizePoints, vm.subtitle.position);
    DrawDoubleTapFeedback(r, s, vm.doubleTap);
    DrawCenterPlayPauseBadge(r, s, vm.playPauseBadge);
    if (vm.isPiP) {
        DrawPiPOverlay(r, s, vm.pipOverlay);
    } else { DrawPlayerTopBar(r, s, vm.topBar); DrawPlayerBottomBar(r, s, vm.bottomBar); }
    DrawSettingsFlyout(r, s, vm.settingsFlyout);
    DrawJumpAheadButton(r, s, vm.jumpAhead);
    DrawOSDBadge(r, s, vm.osdBadge);
    DrawPlaybackDiagnostics(r, s, vm.diagnosticsOverlay);
    DrawLogOverlay(r, s, vm.logOverlay);
    DrawOverlayContextMenu(r, s, vm.contextMenu);
    if (vm.modal.type == ModalType::Confirmation || vm.modal.type == ModalType::DeletePlaylist)
        DrawConfirmModal(r, s, vm.winBounds, vm.modal);
    else if (vm.modal.type != ModalType::None)
        DrawInputModal(r, s, vm.winBounds, vm.modal);
}

void UIComponents::DrawHub(Renderer& r, const Settings& s, const HubRenderViewModel& vm) {
    const auto& th = s.currentTheme;
    float dp = r.GetDpiScale();
    r.Clear(th.bg);
    r.DrawAmbientBackground(vm.winBounds, s);
    DrawNavBar(r, s, vm.navBar);
    switch (vm.currentTab) {
        case TabView::Media: {
            DrawMediaToolbar(r, s, vm.mediaTab);
            r.PushClip(vm.mediaTab.clipBounds);
            if (vm.mediaTab.isEmpty) { DrawEmptyState(r, s, vm.mediaTab.emptyState); } else {
                Renderer::ScopedPrimitiveSnapping scopedSnapping(r, false);
                for (const auto& header : vm.mediaTab.visibleGroupHeaders) { DrawMediaGroupHeader(r, s, header); }
                for (const auto& card : vm.mediaTab.visibleCards) { DrawMediaItemCard(r, s, card); }
                if (vm.mediaTab.hasDragClone && vm.mediaTab.pDragCloneItem)
                    DrawMediaItemCard(r, s, vm.mediaTab.dragCloneRect, *vm.mediaTab.pDragCloneItem, true, true, false, 0.0f, nullptr, false);
                if (vm.mediaTab.isFolderDrag && vm.mediaTab.dragFromFolderIndex >= 0)
                    DrawMediaGroupHeader(r, s, vm.mediaTab.dragCloneFolder);
            }
            r.PopClip();
            break;
        }
        case TabView::Playlists: {
            DrawPlaylistsRail(r, s, vm.playlistsTab.rail);
            DrawPlaylistDetailPane(r, s, vm.playlistsTab.detail);
            break;
        }
        case TabView::History: { DrawHistoryList(r, s, vm.historyTab); break; }
        case TabView::Settings: { DrawSettingsView(r, s, vm.settingsTab); break; }
    }
    DrawAutoscrollAnchor(r, s, vm.autoscroll);
    if (vm.importProgress.isActive) {
        r.FillRoundedRect(D2D1::RectF(0, 0, vm.winBounds.right, vm.winBounds.bottom), 0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.42f));
        DrawImportProgress(r, s, vm.importProgress);
    }
    DrawOSDBadge(r, s, vm.osdBadge);
    DrawContextMenu(r, s, vm.contextMenu);
    if (vm.modal.type == ModalType::NewPlaylist || vm.modal.type == ModalType::NewSmartPlaylist || vm.modal.type == ModalType::EditSmartPlaylist || vm.modal.type == ModalType::SeekStepInput || vm.modal.type == ModalType::FrameSeekInput || vm.modal.type == ModalType::CustomSettingInput || vm.modal.type == ModalType::BookmarkInput || vm.modal.type == ModalType::EditMetadataOverrides || vm.modal.type == ModalType::SaveSubtitleProfile || vm.modal.type == ModalType::RenameSubtitleProfile || vm.modal.type == ModalType::JumpToTime || vm.modal.type == ModalType::CustomAspectInput || vm.modal.type == ModalType::CustomRotationInput) {
        DrawInputModal(r, s, vm.winBounds, vm.modal);
    } else if (vm.modal.type == ModalType::DeletePlaylist || vm.modal.type == ModalType::Confirmation) {
        DrawConfirmModal(r, s, vm.winBounds, vm.modal);
    }
    if (vm.globalSearch.isOpen) { DrawGlobalSearchSurface(r, s, vm.globalSearch); }
}

void UIComponents::DrawGlobalSearchSurface(Renderer& r, const Settings& s, const GlobalSearchSurfaceViewModel& vm) {
    if (!vm.isOpen) return;
    const auto& th = s.currentTheme;
    float dp = r.GetDpiScale();
    // Compact, left-anchored search palette. Keep the library visible and
    // avoid the heavy full-window modal treatment used by confirmation flows.
    D2D1_RECT_F cardBounds = vm.searchBoxBounds;
    const D2D1_RECT_F shadowBounds = D2D1::RectF(cardBounds.left + 2.0f * dp, cardBounds.top + 4.0f * dp,
        cardBounds.right + 2.0f * dp, cardBounds.bottom + 4.0f * dp);
    r.FillRoundedRect(shadowBounds, 9.0f * dp, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.22f));
    r.FillRoundedRect(cardBounds, 9.0f * dp, th.cardBg);
    r.DrawRoundedRect(cardBounds, 9.0f * dp, th.border, 1.0f * dp);
    D2D1_RECT_F inputBounds = vm.inputBounds;
    r.FillRoundedRect(inputBounds, 6.0f * dp, th.bg);
    r.DrawRoundedRect(inputBounds, 6.0f * dp, vm.isFocused ? th.accent : th.border, 1.0f * dp);
    D2D1_RECT_F iconRect = D2D1::RectF(inputBounds.left + 8.0f * dp, inputBounds.top + 7.0f * dp,
        inputBounds.left + 26.0f * dp, inputBounds.top + 25.0f * dp);
    r.DrawIcon(IconType::Search, iconRect, th.textMuted);
    D2D1_RECT_F textRect = D2D1::RectF(inputBounds.left + 30.0f * dp, inputBounds.top + 1.0f * dp,
        inputBounds.right - 8.0f * dp, inputBounds.bottom - 1.0f * dp);
    DrawTextInput(r, s, textRect, vm.searchVisual, L"Find media or history", FontType::Regular, 0.0f);
    if (vm.isCloseHovered) { r.FillRoundedRect(vm.closeBounds, 6.0f * dp, D2D1::ColorF(th.text.r, th.text.g, th.text.b, 0.08f)); }
    r.DrawIcon(IconType::CloseX, InsetRectF(vm.closeBounds, 8.0f * dp, 8.0f * dp), th.textMuted);
    if (vm.visibleItems.empty()) {
        if (!vm.query.empty()) {
            D2D1_RECT_F emptyRect = D2D1::RectF(cardBounds.left + 18.0f * dp, cardBounds.top + 62.0f * dp,
                cardBounds.right - 18.0f * dp, cardBounds.bottom - 28.0f * dp);
            r.DrawString(L"No matches", emptyRect, th.textMuted, FontType::Regular, DWRITE_TEXT_ALIGNMENT_LEADING);
        } else {
            D2D1_RECT_F hintRect = D2D1::RectF(cardBounds.left + 18.0f * dp, cardBounds.top + 62.0f * dp,
                cardBounds.right - 18.0f * dp, cardBounds.bottom - 28.0f * dp);
            r.DrawString(L"Type to search your library", hintRect, th.textMuted, FontType::Regular, DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    } else {
        r.PushClip(vm.listClipBounds);
        for (const auto& item : vm.visibleItems) {
            if (item.itemRect.bottom > vm.listClipBounds.bottom) continue;
            D2D1_COLOR_F itemBg = item.isSelected ? D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, 0.22f)
                                                  : (item.isHovered ? D2D1::ColorF(th.text.r, th.text.g, th.text.b, 0.08f) : D2D1::ColorF(0, 0, 0, 0));
            if (item.isSelected || item.isHovered) { r.FillRoundedRect(item.itemRect, 5.0f * dp, itemBg); }
            if (item.isSelected)
                r.DrawRoundedRect(item.itemRect, 5.0f * dp, D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, 0.65f), 1.0f * dp);
            D2D1_RECT_F titleRect = D2D1::RectF(item.itemRect.left + 10.0f * dp, item.itemRect.top + 4.0f * dp,
                item.itemRect.right - 92.0f * dp, item.itemRect.top + 23.0f * dp);
            r.DrawString(item.title, titleRect, item.existsOnDisk ? th.text : th.textMuted, FontType::Medium);
            std::wstring subText = item.existsOnDisk ? item.fileName : (L"[Missing] " + item.fileName);
            D2D1_RECT_F subRect = D2D1::RectF(item.itemRect.left + 10.0f * dp, item.itemRect.top + 24.0f * dp,
                item.itemRect.right - 92.0f * dp, item.itemRect.bottom - 3.0f * dp);
            r.DrawString(subText, subRect, th.textMuted, FontType::Small);
            D2D1_RECT_F durRect = D2D1::RectF(item.itemRect.right - 82.0f * dp, item.itemRect.top + 5.0f * dp,
                item.itemRect.right - 10.0f * dp, item.itemRect.top + 23.0f * dp);
            r.DrawString(item.durationText, durRect, th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        r.PopClip();
    }
    D2D1_RECT_F footerRect = D2D1::RectF(cardBounds.left + 10.0f * dp, cardBounds.bottom - 23.0f * dp,
        cardBounds.right - 10.0f * dp, cardBounds.bottom - 5.0f * dp);
    r.DrawString(L"\x2191\x2193 select    Enter open    Esc close", footerRect, th.textMuted, FontType::Small, DWRITE_TEXT_ALIGNMENT_LEADING);
}

} // namespace Aperture
