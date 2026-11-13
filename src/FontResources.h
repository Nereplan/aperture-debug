#pragma once

#include "Common.h"
#include "Localization.h"
#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <string_view>

namespace Aperture {

enum class FontType {
    Hero,        // 22px SemiBold - Hero titles, big modal headers
    Title,       // 16px SemiBold - Card titles, section headers
    Subheading,  // 14.5px SemiBold - Sub-headers & prominent labels
    Medium,      // 13.5px Medium - Button labels, tab titles, settings rows
    Regular,     // 13px Regular - Body text, descriptions
    Small,       // 12px Medium - Format tags, badges, secondary metadata
    Caption,     // 10.5px SemiBold - Eyebrows, category headers, section tags
    Mono,        // 12px Mono - Timestamps, file paths, codecs, bitrates
    MonoSmall,   // 10.5px Mono - Small timestamps, counter badges
    Bold,        // 15px Bold - Prominent cards, CTA text
    Subtitle,    // 18px Bold - Player OSD & subtitles
    MenuLabel, MenuShoulder, MenuDial, MenuValue, MenuShortcut
};

enum class TextLayoutMode { SingleLine, SingleLineEllipsis, Wrapped, WrappedEllipsis };

// Each backend owns its resources; construction and font fallback policy are shared.
class FontResources {
protected:
    float dpiScale = 1.0f, textScale = 1.0f;
    ComPtr<IDWriteFactory3> dwriteFactory;
    ComPtr<IDWriteFontCollection1> customFontCollection;
    std::map<std::pair<int, int>, ComPtr<IDWriteTextFormat>> sizedFormatCache;
    struct LayoutKey {
        std::wstring text;
        FontType font;
        int maxW = 0, maxH = 0;
        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
        DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        TextLayoutMode mode = TextLayoutMode::SingleLine;
    };
    struct LayoutKeyProbe {
        std::wstring_view text;
        FontType font;
        int maxW = 0, maxH = 0;
        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
        DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        TextLayoutMode mode = TextLayoutMode::SingleLine;
    };
    struct LayoutKeyLess {
        using is_transparent = void;
        template <typename A, typename B>
        bool operator()(const A& a, const B& b) const {
            if (a.font != b.font) return a.font < b.font;
            if (a.maxW != b.maxW) return a.maxW < b.maxW;
            if (a.maxH != b.maxH) return a.maxH < b.maxH;
            if (a.align != b.align) return a.align < b.align;
            if (a.valign != b.valign) return a.valign < b.valign;
            if (a.mode != b.mode) return a.mode < b.mode;
            return a.text < b.text;
        }
    };
    struct LayoutCacheEntry { ComPtr<IDWriteTextLayout> layout; std::list<LayoutKey>::iterator lruIt; };
    static constexpr size_t kLayoutCacheEntryLimit = 512;
    static constexpr uint64_t kLayoutCacheByteLimit = 3ull * 1024ull * 1024ull;
    static constexpr size_t kTransientLayoutCacheLimit = 96;
    std::map<LayoutKey, LayoutCacheEntry, LayoutKeyLess> layoutCache;
    std::list<LayoutKey> layoutLru;
    uint64_t layoutCacheBytes = 0;
    std::map<LayoutKey, LayoutCacheEntry, LayoutKeyLess> transientLayoutCache;
    std::list<LayoutKey> transientLayoutLru;
    struct SizedLayoutKey {
        std::wstring text;
        int pxHalfSteps = 0;
        int weight = 400;
        int maxWQ = 0;
        int maxHQ = 0;
        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
        DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    };
    struct SizedLayoutKeyProbe {
        std::wstring_view text;
        int pxHalfSteps = 0;
        int weight = 400;
        int maxWQ = 0;
        int maxHQ = 0;
        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
        DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    };
    struct SizedLayoutKeyLess {
        using is_transparent = void;
        template <typename A, typename B>
        bool operator()(const A& a, const B& b) const {
            if (a.pxHalfSteps != b.pxHalfSteps) return a.pxHalfSteps < b.pxHalfSteps;
            if (a.weight != b.weight) return a.weight < b.weight;
            if (a.maxWQ != b.maxWQ) return a.maxWQ < b.maxWQ;
            if (a.maxHQ != b.maxHQ) return a.maxHQ < b.maxHQ;
            if (a.align != b.align) return a.align < b.align;
            if (a.valign != b.valign) return a.valign < b.valign;
            return a.text < b.text;
        }
    };
    struct SizedLayoutEntry {
        ComPtr<IDWriteTextLayout> layout;
        std::list<SizedLayoutKey>::iterator lruIt;
    };
    static constexpr size_t kSizedLayoutCacheLimit = 64;
    std::map<SizedLayoutKey, SizedLayoutEntry, SizedLayoutKeyLess> sizedLayoutCache;
    std::list<SizedLayoutKey> sizedLayoutLru;
    std::wstring selectedFontFamily;
    ComPtr<IDWriteTextFormat> tfHero, tfTitle, tfSubheading, tfMedium;
    ComPtr<IDWriteTextFormat> tfRegular, tfSmall, tfCaption, tfMono;
    ComPtr<IDWriteTextFormat> tfMonoSmall, tfBold, tfSubtitle;
    ComPtr<IDWriteTextFormat> tfMenuLabel, tfMenuShoulder, tfMenuDial, tfMenuValue, tfMenuShortcut;
    static void MakeFormat( IDWriteFactory* factory, IDWriteFontCollection* customCollection, const wchar_t* family,
        DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch, float size, const wchar_t* locale,
        IDWriteTextFormat** ppFormat ) {
        if (!factory || !ppFormat || !family) return;
        *ppFormat = nullptr;
        IDWriteFontCollection* collectionToUse = nullptr;
        if (customCollection && family) {
            BOOL exists = FALSE;
            UINT32 index = 0;
            if (SUCCEEDED(customCollection->FindFamilyName(family, &index, &exists)) && exists) { collectionToUse = customCollection; }
        }
        HRESULT hr = factory->CreateTextFormat(family, collectionToUse, weight, style, stretch, size, locale, ppFormat);
        if (FAILED(hr) || !*ppFormat) {
            hr = factory->CreateTextFormat(L"Segoe UI", NULL, weight, style, stretch, size, locale, ppFormat);
            if (FAILED(hr) || !*ppFormat) { factory->CreateTextFormat(L"Arial", NULL, weight, style, stretch, size, locale, ppFormat); }
        }
    }
    void LoadCustomFontCollection() {
        if (!dwriteFactory) return;
        if (customFontCollection) return; // Already built
        std::vector<std::wstring> searchDirs = { GetExeDir() + L"\\fonts\\", GetExeDir() + L"\\..\\fonts\\", L".\\fonts\\", L"fonts\\" };
        std::wstring fontDir;
        for (const auto& dir : searchDirs) {
            if (FileExists(dir + L"Inter.ttf")) { fontDir = dir; break; }
        }
        if (fontDir.empty()) { fontDir = GetExeDir() + L"\\fonts\\"; }
        const std::wstring fontPaths[] = { fontDir + L"Inter.ttf", fontDir + L"SpaceGrotesk.ttf", fontDir + L"JetBrainsMono.ttf" };
        // Build DirectWrite Custom Font Collection via IDWriteFactory5 / FontSetBuilder
        ComPtr<IDWriteFactory5> factory5;
        if (SUCCEEDED(dwriteFactory.As(&factory5)) && factory5) {
            ComPtr<IDWriteFontSetBuilder1> fontSetBuilder;
            if (SUCCEEDED(factory5->CreateFontSetBuilder(&fontSetBuilder)) && fontSetBuilder) {
                for (const auto& path : fontPaths) {
                    const std::wstring fullFontPath = GetFullPathDynamic(path);
                    if (FileExists(fullFontPath)) {
                        ComPtr<IDWriteFontFile> fontFile;
                        if (SUCCEEDED(factory5->CreateFontFileReference(fullFontPath.c_str(), nullptr, &fontFile)) && fontFile)
                            fontSetBuilder->AddFontFile(fontFile.Get());
                    }
                }
                ComPtr<IDWriteFontSet> fontSet;
                if (SUCCEEDED(fontSetBuilder->CreateFontSet(&fontSet)) && fontSet) {
                    ComPtr<IDWriteFontCollection1> collection1;
                    if (SUCCEEDED(factory5->CreateFontCollectionFromFontSet(fontSet.Get(), &collection1)) && collection1)
                        customFontCollection = collection1;
                }
            }
        }
    }
    IDWriteTextFormat* GetTextFormat(FontType font) {
        switch (font) {
            case FontType::Hero:       return tfHero ? tfHero.Get() : tfTitle.Get();
            case FontType::Title:      return tfTitle ? tfTitle.Get() : tfBold.Get();
            case FontType::Subheading: return tfSubheading ? tfSubheading.Get() : tfBold.Get();
            case FontType::Medium:     return tfMedium ? tfMedium.Get() : tfRegular.Get();
            case FontType::Regular:    return tfRegular ? tfRegular.Get() : tfMedium.Get();
            case FontType::Small:      return tfSmall ? tfSmall.Get() : tfRegular.Get();
            case FontType::Caption:    return tfCaption ? tfCaption.Get() : tfSmall.Get();
            case FontType::Mono:       return tfMono ? tfMono.Get() : tfRegular.Get();
            case FontType::MonoSmall:  return tfMonoSmall ? tfMonoSmall.Get() : tfMono.Get();
            case FontType::Bold:       return tfBold ? tfBold.Get() : tfTitle.Get();
            case FontType::Subtitle:   return tfSubtitle ? tfSubtitle.Get() : tfTitle.Get();
            case FontType::MenuLabel: return tfMenuLabel.Get();
            case FontType::MenuShoulder: return tfMenuShoulder.Get();
            case FontType::MenuDial: return tfMenuDial.Get();
            case FontType::MenuValue: return tfMenuValue.Get();
            case FontType::MenuShortcut: return tfMenuShortcut.Get();
        }
        return tfRegular.Get();
    }
    IDWriteTextFormat* GetOrCreateSizedFormat(float sizeDp, DWRITE_FONT_WEIGHT weight) {
        if (!dwriteFactory) return nullptr;
        float dp = std::max(0.5f, dpiScale * textScale);
        float px = std::max(6.0f, sizeDp * dp);
        auto key = std::make_pair((int)std::lround(px * 2.0f), (int)weight);
        auto it = sizedFormatCache.find(key);
        if (it != sizedFormatCache.end()) return it->second.Get();
        std::wstring locName = Utf8ToUtf16(Localization::Instance().GetCurrentLocale());
        if (locName.empty()) locName = L"en-us";
        ComPtr<IDWriteTextFormat> fmt;
        MakeFormat(dwriteFactory.Get(), customFontCollection.Get(), selectedFontFamily.c_str(),
                   weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, locName.c_str(), &fmt);
        if (!fmt) return nullptr;
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt->SetReadingDirection(Localization::Instance().IsRtl() ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
        fmt->SetFlowDirection(DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM);
        if (sizedFormatCache.size() >= 32) sizedFormatCache.erase(sizedFormatCache.begin());
        IDWriteTextFormat* raw = fmt.Get();
        sizedFormatCache.emplace(key, std::move(fmt));
        return raw;
    }
    void ClearLayoutCaches() {
        layoutCache.clear();
        layoutLru.clear();
        layoutCacheBytes = 0;
        transientLayoutCache.clear();
        transientLayoutLru.clear();
    }
    void ClearSizedCaches() {
        sizedFormatCache.clear();
        sizedLayoutCache.clear();
        sizedLayoutLru.clear();
    }
    ComPtr<IDWriteTextLayout> GetOrCreateTextLayout(const std::wstring& text, FontType font, float maxW, float maxH,
                                                    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                                                    DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                                                    TextLayoutMode mode = TextLayoutMode::SingleLine) {
        if (!dwriteFactory || text.empty()) return nullptr;
        bool isTransient = false;
        if (text.size() <= 32) {
            bool hasDigit = false, hasColon = false;
            for (wchar_t c : text) {
                if (c >= L'0' && c <= L'9') hasDigit = true;
                if (c == L':') hasColon = true;
            }
            if ((hasDigit && hasColon) || (hasDigit && text.back() == L'%')) isTransient = true;
        }
        IDWriteTextFormat* tf = GetTextFormat(font);
        if (!tf) return nullptr;
        auto createLayout = [&]() -> ComPtr<IDWriteTextLayout> {
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.length()), tf, maxW, maxH, &layout)) || !layout)
                return nullptr;
            const bool isRtl = Localization::Instance().IsRtl();
            layout->SetReadingDirection(isRtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
            layout->SetFlowDirection(DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM);
            layout->SetTextAlignment(align);
            layout->SetParagraphAlignment(valign);
            if (mode == TextLayoutMode::Wrapped || mode == TextLayoutMode::WrappedEllipsis) {
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
            }
            if (mode == TextLayoutMode::SingleLineEllipsis || mode == TextLayoutMode::WrappedEllipsis) {
                DWRITE_TRIMMING trimming{};
                trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                ComPtr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(dwriteFactory->CreateEllipsisTrimmingSign(tf, &ellipsis))) {
                    layout->SetTrimming(&trimming, ellipsis.Get());
                }
            }
            return layout;
        };
        const LayoutKeyProbe probe{ std::wstring_view(text), font,
            static_cast<int>(std::lround(maxW * 4.0f)), static_cast<int>(std::lround(maxH * 4.0f)), align, valign, mode };
        if (isTransient) {
            auto tit = transientLayoutCache.find(probe);
            if (tit != transientLayoutCache.end()) {
                transientLayoutLru.splice(transientLayoutLru.begin(), transientLayoutLru, tit->second.lruIt);
                tit->second.lruIt = transientLayoutLru.begin();
                return tit->second.layout;
            }
            auto layout = createLayout();
            if (layout) {
                while (transientLayoutCache.size() >= kTransientLayoutCacheLimit && !transientLayoutLru.empty()) {
                    transientLayoutCache.erase(transientLayoutLru.back());
                    transientLayoutLru.pop_back();
                }
                LayoutKey key{ text, font, probe.maxW, probe.maxH, align, valign, mode };
                transientLayoutLru.push_front(key);
                LayoutCacheEntry entry{ layout, transientLayoutLru.begin() };
                transientLayoutCache.emplace(std::move(key), std::move(entry));
            }
            return layout;
        }
        auto it = layoutCache.find(probe);
        if (it != layoutCache.end()) {
            layoutLru.splice(layoutLru.begin(), layoutLru, it->second.lruIt);
            it->second.lruIt = layoutLru.begin();
            return it->second.layout;
        }
        auto layout = createLayout();
        if (!layout) return nullptr;
        auto estimateBytes = [](const LayoutKey& k) -> uint64_t {
            return static_cast<uint64_t>(k.text.size()) * sizeof(wchar_t) * 2ull + 2048ull + sizeof(LayoutKey);
        };
        LayoutKey key{ text, font, probe.maxW, probe.maxH, align, valign, mode };
        const uint64_t incoming = estimateBytes(key);
        while (!layoutCache.empty() && (layoutCache.size() >= kLayoutCacheEntryLimit ||
                layoutCacheBytes + incoming > kLayoutCacheByteLimit) && !layoutLru.empty()) {
            const LayoutKey& oldKey = layoutLru.back();
            auto oldIt = layoutCache.find(oldKey);
            if (oldIt != layoutCache.end()) {
                const uint64_t oldBytes = estimateBytes(oldIt->first);
                layoutCacheBytes = (oldBytes > layoutCacheBytes) ? 0 : (layoutCacheBytes - oldBytes);
                layoutCache.erase(oldIt);
            }
            layoutLru.pop_back();
        }
        layoutLru.push_front(key);
        LayoutCacheEntry entry{ layout, layoutLru.begin() };
        layoutCacheBytes += incoming;
        layoutCache.emplace(std::move(key), std::move(entry));
        return layout;
    }
    ComPtr<IDWriteTextLayout> GetOrCreateSizedLayout(const std::wstring& text, float sizeDp, DWRITE_FONT_WEIGHT weight,
                                                     float maxW, float maxH, DWRITE_TEXT_ALIGNMENT align,
                                                     DWRITE_PARAGRAPH_ALIGNMENT valign) {
        if (!dwriteFactory || text.empty()) return nullptr;
        const float dp = std::max(0.5f, dpiScale * textScale);
        const float px = std::max(6.0f, sizeDp * dp);
        const int pxHalfSteps = static_cast<int>(std::lround(px * 2.0f));
        const int weightInt = static_cast<int>(weight);
        const int maxWQ = static_cast<int>(std::lround(maxW * 4.0f));
        const int maxHQ = static_cast<int>(std::lround(maxH * 4.0f));
        const SizedLayoutKeyProbe probe{ std::wstring_view(text), pxHalfSteps, weightInt, maxWQ, maxHQ, align, valign };
        auto it = sizedLayoutCache.find(probe);
        if (it != sizedLayoutCache.end()) {
            sizedLayoutLru.splice(sizedLayoutLru.begin(), sizedLayoutLru, it->second.lruIt);
            it->second.lruIt = sizedLayoutLru.begin();
            return it->second.layout;
        }
        IDWriteTextFormat* tf = GetOrCreateSizedFormat(sizeDp, weight);
        if (!tf) return nullptr;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.length()), tf, maxW, maxH, &layout)) || !layout)
            return nullptr;
        layout->SetReadingDirection(Localization::Instance().IsRtl() ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
        layout->SetFlowDirection(DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM);
        layout->SetTextAlignment(align);
        layout->SetParagraphAlignment(valign);
        while (sizedLayoutCache.size() >= kSizedLayoutCacheLimit && !sizedLayoutLru.empty()) {
            sizedLayoutCache.erase(sizedLayoutLru.back());
            sizedLayoutLru.pop_back();
        }
        SizedLayoutKey key{ text, pxHalfSteps, weightInt, maxWQ, maxHQ, align, valign };
        sizedLayoutLru.push_front(key);
        SizedLayoutEntry entry{ layout, sizedLayoutLru.begin() };
        sizedLayoutCache.emplace(std::move(key), std::move(entry));
        return layout;
    }
    D2D1_SIZE_F MeasureStringSized(const std::wstring& text, float sizeDp, DWRITE_FONT_WEIGHT weight) {
        D2D1_SIZE_F zero = D2D1::SizeF(0.0f, 0.0f);
        if (!dwriteFactory || text.empty()) return zero;
        ComPtr<IDWriteTextLayout> layout = GetOrCreateSizedLayout(
            text, sizeDp, weight, 10000.0f, 10000.0f, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (!layout) return zero;
        DWRITE_TEXT_METRICS metrics;
        if (FAILED(layout->GetMetrics(&metrics))) return zero;
        return D2D1::SizeF(metrics.width, metrics.height);
    }
    D2D1_SIZE_F MeasureString(const std::wstring& text, FontType font, float maxW = 10000.0f, float maxH = 10000.0f) {
        if (!dwriteFactory || text.empty()) return D2D1::SizeF(0.0f, 0.0f);
        ComPtr<IDWriteTextLayout> layout = GetOrCreateTextLayout(
            text, font, maxW, maxH, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (!layout) return D2D1::SizeF(0.0f, 0.0f);
        DWRITE_TEXT_METRICS tm{};
        if (FAILED(layout->GetMetrics(&tm))) return D2D1::SizeF(0.0f, 0.0f);
        return D2D1::SizeF(tm.widthIncludingTrailingWhitespace, tm.height);
    }
    float MeasureStringWrappedHeight(const std::wstring& text, FontType font, float maxWidth) {
        if (!dwriteFactory || text.empty() || maxWidth <= 1.0f) return 0.0f;
        ComPtr<IDWriteTextLayout> layout = GetOrCreateTextLayout(
            text, font, maxWidth, 10000.0f, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
            TextLayoutMode::Wrapped);
        if (!layout) return 0.0f;
        DWRITE_TEXT_METRICS tm{};
        if (FAILED(layout->GetMetrics(&tm))) return 0.0f;
        return tm.height;
    }
    void CreateFontFormats() {
        float dp = dpiScale * textScale;
        IDWriteFactory* fac = dwriteFactory.Get();
        IDWriteFontCollection* col = customFontCollection.Get();
        std::wstring sans = selectedFontFamily;
        std::wstring display = selectedFontFamily;
        std::wstring mono = L"JetBrains Mono";
        // One family carries the whole UI, including display/hero/subtitle roles;
        // a separate display face reads as branding, not as a tool. Only the mono
        // role is resolved separately (fixed-width timestamps/metadata).
        if (selectedFontFamily == L"Inter" || selectedFontFamily.empty()) {
            sans = L"Inter";
            display = L"Inter";
        } else if (selectedFontFamily == L"JetBrains Mono") { sans = L"JetBrains Mono"; display = L"JetBrains Mono"; }
        tfHero.Reset();      tfTitle.Reset();    tfSubheading.Reset();
        tfMedium.Reset();    tfRegular.Reset();  tfSmall.Reset();
        tfCaption.Reset();   tfMono.Reset();     tfMonoSmall.Reset();
        tfBold.Reset();      tfSubtitle.Reset();
        tfMenuLabel.Reset(); tfMenuShoulder.Reset(); tfMenuDial.Reset(); tfMenuValue.Reset(); tfMenuShortcut.Reset();
        std::wstring locName = Utf8ToUtf16(Localization::Instance().GetCurrentLocale());
        if (locName.empty()) locName = L"en-us";
        MakeFormat(fac, col, display.c_str(), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 22.0f * dp, locName.c_str(), &tfHero);
        MakeFormat(fac, col, sans.c_str(),    DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f * dp, locName.c_str(), &tfTitle);
        MakeFormat(fac, col, sans.c_str(),    DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.5f * dp, locName.c_str(), &tfSubheading);
        MakeFormat(fac, col, sans.c_str(),    DWRITE_FONT_WEIGHT_MEDIUM,    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.5f * dp, locName.c_str(), &tfMedium);
        MakeFormat(fac, col, sans.c_str(),    DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * dp, locName.c_str(), &tfRegular);
        MakeFormat(fac, col, sans.c_str(),    DWRITE_FONT_WEIGHT_MEDIUM,    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * dp, locName.c_str(), &tfSmall);
        MakeFormat(fac, col, sans.c_str(),    DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.5f * dp, locName.c_str(), &tfCaption);
        MakeFormat(fac, col, mono.c_str(),    DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * dp, locName.c_str(), &tfMono);
        MakeFormat(fac, col, mono.c_str(),    DWRITE_FONT_WEIGHT_MEDIUM,    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.5f * dp, locName.c_str(), &tfMonoSmall);
        MakeFormat(fac, col, sans.c_str(),    DWRITE_FONT_WEIGHT_BOLD,      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f * dp, locName.c_str(), &tfBold);
        MakeFormat(fac, col, display.c_str(), DWRITE_FONT_WEIGHT_BOLD,      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f * dp, locName.c_str(), &tfSubtitle);
        // The compact player menu follows the native typography of the HTML reference.
#if defined(_WIN32)
        const wchar_t* menuFamily = L"Segoe UI";
#else
        const wchar_t* menuFamily = sans.c_str();
#endif
        MakeFormat(fac, col, menuFamily, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f * dp, locName.c_str(), &tfMenuLabel);
        MakeFormat(fac, col, menuFamily, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * dp, locName.c_str(), &tfMenuShoulder);
        MakeFormat(fac, col, menuFamily, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f * dp, locName.c_str(), &tfMenuDial);
        MakeFormat(fac, col, menuFamily, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f * dp, locName.c_str(), &tfMenuValue);
        MakeFormat(fac, col, menuFamily, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f * dp, locName.c_str(), &tfMenuShortcut);
        bool isRtl = Localization::Instance().IsRtl();
        DWRITE_READING_DIRECTION readingDir = isRtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
        IDWriteTextFormat *formats[] = {tfHero.Get(),    tfTitle.Get(), tfSubheading.Get(), tfMedium.Get(), tfRegular.Get(), tfSmall.Get(),
                                        tfCaption.Get(), tfMono.Get(),  tfMonoSmall.Get(),  tfBold.Get(),   tfSubtitle.Get(),
                                        tfMenuLabel.Get(), tfMenuShoulder.Get(), tfMenuDial.Get(), tfMenuValue.Get(), tfMenuShortcut.Get()};
        for (auto* f : formats) {
            if (f) {
                f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                f->SetReadingDirection(readingDir);
                f->SetFlowDirection(DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM);
            }
        }
    }
};

} // namespace Aperture
