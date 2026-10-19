#pragma once

#include "Common.h"
#include "Localization.h"
#include <map>

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

// Each backend owns its resources; construction and font fallback policy are shared.
class FontResources {
protected:
    float dpiScale = 1.0f, textScale = 1.0f;
    ComPtr<IDWriteFactory3> dwriteFactory;
    ComPtr<IDWriteFontCollection1> customFontCollection;
    std::map<std::pair<int, int>, ComPtr<IDWriteTextFormat>> sizedFormatCache;
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
        if (sizedFormatCache.size() >= 32) sizedFormatCache.clear();
        IDWriteTextFormat* raw = fmt.Get();
        sizedFormatCache.emplace(key, std::move(fmt));
        return raw;
    }
    D2D1_SIZE_F MeasureStringSized(const std::wstring& text, float sizeDp, DWRITE_FONT_WEIGHT weight) {
        D2D1_SIZE_F zero = D2D1::SizeF(0.0f, 0.0f);
        if (!dwriteFactory || text.empty()) return zero;
        IDWriteTextFormat* tf = GetOrCreateSizedFormat(sizeDp, weight);
        if (!tf) return zero;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory->CreateTextLayout(text.c_str(), (UINT32)text.length(), tf, 10000.0f, 10000.0f, &layout)) || !layout) return zero;
        DWRITE_TEXT_METRICS metrics;
        if (FAILED(layout->GetMetrics(&metrics))) return zero;
        return D2D1::SizeF(metrics.width, metrics.height);
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
        MakeFormat(fac, col, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f * dp, locName.c_str(), &tfMenuLabel);
        MakeFormat(fac, col, L"Segoe UI", DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * dp, locName.c_str(), &tfMenuShoulder);
        MakeFormat(fac, col, L"Segoe UI", DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f * dp, locName.c_str(), &tfMenuDial);
        MakeFormat(fac, col, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f * dp, locName.c_str(), &tfMenuValue);
        MakeFormat(fac, col, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f * dp, locName.c_str(), &tfMenuShortcut);
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
