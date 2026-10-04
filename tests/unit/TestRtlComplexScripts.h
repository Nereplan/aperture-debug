#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Localization.h"
#include "../../src/Settings.h"
#include "../../src/Json.h"
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <unordered_set>

namespace Aperture::Test {

TEST_CASE(RtlComplexScripts, DirectionMetadataParsingAndValidation) {
    // 1. Valid LTR
    std::string validLtrJson = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"tag\": \"en-US\",\n"
        "  \"displayName\": \"English (US)\",\n"
        "  \"direction\": \"ltr\",\n"
        "  \"strings\": { \"test.key\": \"Hello\" }\n"
        "}";
    LocaleInfo info;
    std::unordered_map<std::string, std::string> strings;
    std::unordered_map<std::string, PluralEntry> plurals;
    std::string err;
    TEST_ASSERT(Localization::ParseCatalogJson(validLtrJson, info, strings, plurals, err));
    TEST_ASSERT_EQ(info.direction, "ltr");

    // 2. Valid RTL (lowercase and uppercase)
    std::string validRtlJson = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"tag\": \"ar-SA\",\n"
        "  \"displayName\": \"العربية\",\n"
        "  \"direction\": \"rtl\",\n"
        "  \"strings\": { \"test.key\": \"مرحبا\" }\n"
        "}";
    TEST_ASSERT(Localization::ParseCatalogJson(validRtlJson, info, strings, plurals, err));
    TEST_ASSERT_EQ(info.direction, "rtl");

    std::string upperRtlJson = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"tag\": \"he-IL\",\n"
        "  \"displayName\": \"עברית\",\n"
        "  \"direction\": \"RTL\",\n"
        "  \"strings\": { \"test.key\": \"שלום\" }\n"
        "}";
    TEST_ASSERT(Localization::ParseCatalogJson(upperRtlJson, info, strings, plurals, err));
    TEST_ASSERT_EQ(info.direction, "rtl");

    // 3. Missing direction defaults to "ltr"
    std::string missingDirJson = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"tag\": \"fr-FR\",\n"
        "  \"displayName\": \"Français\",\n"
        "  \"strings\": { \"test.key\": \"Bonjour\" }\n"
        "}";
    TEST_ASSERT(Localization::ParseCatalogJson(missingDirJson, info, strings, plurals, err));
    TEST_ASSERT_EQ(info.direction, "ltr");

    // 4. Malformed direction is strictly rejected
    std::string malformedDir1 = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"tag\": \"xx-YY\",\n"
        "  \"displayName\": \"Invalid\",\n"
        "  \"direction\": \"top-to-bottom\",\n"
        "  \"strings\": { \"test.key\": \"Test\" }\n"
        "}";
    TEST_ASSERT(!Localization::ParseCatalogJson(malformedDir1, info, strings, plurals, err));
    TEST_ASSERT(!err.empty());

    std::string malformedDir2 = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"tag\": \"xx-YY\",\n"
        "  \"displayName\": \"Invalid\",\n"
        "  \"direction\": \"vertical\",\n"
        "  \"strings\": { \"test.key\": \"Test\" }\n"
        "}";
    TEST_ASSERT(!Localization::ParseCatalogJson(malformedDir2, info, strings, plurals, err));

    std::string malformedDir3 = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"tag\": \"xx-YY\",\n"
        "  \"displayName\": \"Invalid\",\n"
        "  \"direction\": \"bidi\",\n"
        "  \"strings\": { \"test.key\": \"Test\" }\n"
        "}";
    TEST_ASSERT(!Localization::ParseCatalogJson(malformedDir3, info, strings, plurals, err));
}

TEST_CASE(RtlComplexScripts, FallbackOnMalformedOrMissingRtlCatalog) {
    ScopedTempDir tempDir(L"Aperture_Rtl_Fallback_Test_");
    tempDir.CreateSubDir(L"locales");

    // Write a malformed RTL catalog
    std::string badCatalog = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"tag\": \"ar-SA\",\n"
        "  \"displayName\": \"العربية\",\n"
        "  \"direction\": \"invalid_direction_mode\",\n"
        "  \"strings\": { \"nav.media\": \"الوسائط\" }\n"
        "}";
    tempDir.WriteFile(L"locales\\ar-SA.json", badCatalog);

    auto& loc = Localization::Instance();
    loc.Initialize(tempDir.GetPath(), "ar-SA");

    // Must gracefully fall back to en-US / English built-in without crash
    TEST_ASSERT_EQ(loc.GetCurrentLocale(), "en-US");
    TEST_ASSERT(!loc.IsRtl());
    TEST_ASSERT_EQ(loc.Get("nav.media"), L"Media");
    TEST_ASSERT(loc.HadFallback());

    // Reset back
    loc.Initialize(GetExeDir(), "en-US");
}

inline void WriteRtlCatalogFixtures(ScopedTempDir& root) {
    TEST_ASSERT(root.WriteFile(LR"(locales\ar-SA.json)", u8R"({"schemaVersion":1,"tag":"ar-SA","displayName":"العربية","direction":"rtl","strings":{"nav.media":"الوسائط","nav.settings":"الإعدادات","nav.playlists":"قوائم التشغيل","playlist.drop_banner":"إضافة إلى {0}"}})"));
    TEST_ASSERT(root.WriteFile(LR"(locales\he-IL.json)", u8R"({"schemaVersion":1,"tag":"he-IL","displayName":"עברית","direction":"rtl","strings":{"nav.media":"מדיה"}})"));
}

TEST_CASE(RtlComplexScripts, NoManualUnicodeReversalInvariant) {
    ScopedTempDir root(L"Aperture_Rtl_Order_");
    WriteRtlCatalogFixtures(root);
    auto& loc = Localization::Instance();
    loc.Initialize(root.GetPath(), "ar-SA");
    TEST_ASSERT_EQ(loc.GetCurrentLocale(), std::string("ar-SA"));
    TEST_ASSERT(loc.IsRtl());
    TEST_ASSERT_EQ(loc.Get("nav.media"), std::wstring(L"الوسائط"));
    TEST_ASSERT_EQ(loc.Get("nav.settings"), std::wstring(L"الإعدادات"));
    TEST_ASSERT_EQ(loc.Get("nav.playlists"), std::wstring(L"قوائم التشغيل"));
    TEST_ASSERT(loc.SetLocale("he-IL"));
    TEST_ASSERT(loc.IsRtl());
    TEST_ASSERT_EQ(loc.Get("nav.media"), std::wstring(L"מדיה"));
    loc.Initialize(GetExeDir(), "en-US");
}

TEST_CASE(RtlComplexScripts, DirectWriteComplexScriptShapingAndMetrics) {
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dwriteFactory);
    TEST_ASSERT(SUCCEEDED(hr));
    TEST_ASSERT(dwriteFactory != nullptr);

    Microsoft::WRL::ComPtr<IDWriteTextFormat> tf;
    hr = dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        16.0f, L"ar-sa", &tf
    );
    TEST_ASSERT(SUCCEEDED(hr));
    tf->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
    tf->SetFlowDirection(DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM);

    // 1. Arabic text with ligatures (Lam-Alef لا, لآ, لأ) and tashkeel (combining diacritics)
    std::wstring arabicComplex = L"\x0628\x0650\x0633\x0652\x0645\x0650 \x0671\x0644\x0644\x064e\x0651\x0670\x0647\x0650 \x062a\x064e\x0634\x0652\x063a\x0650\x064a\x0644\x064f \x0627\x0644\x0643\x064f\x0644\x0651\x0650";
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    hr = dwriteFactory->CreateTextLayout(
        arabicComplex.c_str(), static_cast<UINT32>(arabicComplex.length()),
        tf.Get(), 800.0f, 200.0f, &layout
    );
    TEST_ASSERT(SUCCEEDED(hr));
    TEST_ASSERT(layout != nullptr);

    DWRITE_TEXT_METRICS tm{};
    hr = layout->GetMetrics(&tm);
    TEST_ASSERT(SUCCEEDED(hr));
    TEST_ASSERT(tm.width > 0.0f);
    TEST_ASSERT(tm.height > 0.0f);
    TEST_ASSERT(tm.lineCount >= 1);

    // Verify cluster metrics (ligatures and combining marks coalesce)
    UINT32 clusterCount = 0;
    layout->GetClusterMetrics(nullptr, 0, &clusterCount);
    TEST_ASSERT(clusterCount > 0);
    std::vector<DWRITE_CLUSTER_METRICS> clusters(clusterCount);
    hr = layout->GetClusterMetrics(clusters.data(), clusterCount, &clusterCount);
    TEST_ASSERT(SUCCEEDED(hr));
    TEST_ASSERT_EQ(clusters.size(), clusterCount);

    // Verify right-to-left clusters have isRightToLeft set
    bool foundRtlCluster = false;
    for (const auto& c : clusters) {
        if (c.isRightToLeft) {
            foundRtlCluster = true;
            break;
        }
    }
    TEST_ASSERT(foundRtlCluster);

    // 2. Hebrew text with Nikkud (vowel points)
    std::wstring hebrewComplex = L"\x05e9\x05b8\x05c1\x05dc\x05d5\x05b9\x05dd \x05e2\x05b2\x05dc\x05d4 - \x05e1\x05b4\x05e4\x05b0\x05e8\x05b4\x05d9\x05b7\x05bc\x05ea \x05de\x05b6\x05d3\x05b0\x05d9\x05b8\x05d4";
    Microsoft::WRL::ComPtr<IDWriteTextLayout> heLayout;
    hr = dwriteFactory->CreateTextLayout(
        hebrewComplex.c_str(), static_cast<UINT32>(hebrewComplex.length()),
        tf.Get(), 800.0f, 200.0f, &heLayout
    );
    TEST_ASSERT(SUCCEEDED(hr));
    DWRITE_TEXT_METRICS heTm{};
    hr = heLayout->GetMetrics(&heTm);
    TEST_ASSERT(SUCCEEDED(hr));
    TEST_ASSERT(heTm.width > 0.0f);
    TEST_ASSERT(heTm.height > 0.0f);
}

TEST_CASE(RtlComplexScripts, MixedBidiAndUnicodeIsolation) {
    // Validate Bidi isolation helpers
    std::wstring path = L"C:\\Media\\Cosmos_4K.mkv";
    std::wstring isolatedPath = Localization::BidiIsolateLtr(path);
    TEST_ASSERT_EQ(isolatedPath, L"\x2066" + path + L"\x2069");

    std::string url = "https://aperture.local/stream";
    std::string isolatedUrl = Localization::BidiIsolateLtrUtf8(url);
    TEST_ASSERT_EQ(isolatedUrl, "\xE2\x81\xA6" + url + "\xE2\x81\xA9");

    // In RTL context, inserting dynamic LTR paths into formatted strings
    ScopedTempDir root(L"Aperture_Rtl_Mixed_");
    WriteRtlCatalogFixtures(root);
    auto& loc = Localization::Instance();
    loc.Initialize(root.GetPath(), "ar-SA");
    TEST_ASSERT_EQ(loc.GetCurrentLocale(), std::string("ar-SA"));

    if (loc.GetCurrentLocale() == "ar-SA") {
        std::wstring formattedBanner = loc.Format("playlist.drop_banner", Localization::BidiIsolateLtr(L"Action Movies (2026)"));
        TEST_ASSERT(!formattedBanner.empty());
        TEST_ASSERT(formattedBanner.find(L"\x2066") != std::wstring::npos);
        TEST_ASSERT(formattedBanner.find(L"\x2069") != std::wstring::npos);

        Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dwriteFactory);
        Microsoft::WRL::ComPtr<IDWriteTextFormat> tf;
        dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"ar-sa", &tf);
        tf->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);

        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = dwriteFactory->CreateTextLayout(formattedBanner.c_str(), static_cast<UINT32>(formattedBanner.length()), tf.Get(), 600.0f, 100.0f, &layout);
        TEST_ASSERT(SUCCEEDED(hr));
        DWRITE_TEXT_METRICS tm{};
        hr = layout->GetMetrics(&tm);
        TEST_ASSERT(SUCCEEDED(hr));
        TEST_ASSERT(tm.width > 0.0f);
    }

    loc.Initialize(GetExeDir(), "en-US");
}

TEST_CASE(RtlComplexScripts, DirectWriteRasterizationEvidence) {
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, &d2dFactory);
    TEST_ASSERT(SUCCEEDED(hr));

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
    TEST_ASSERT(SUCCEEDED(hr));

    const UINT width = 400;
    const UINT height = 100;
    Microsoft::WRL::ComPtr<IWICBitmap> wicBitmap;
    hr = wicFactory->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &wicBitmap);
    TEST_ASSERT(SUCCEEDED(hr));

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt;
    hr = d2dFactory->CreateWicBitmapRenderTarget(wicBitmap.Get(), rtProps, &rt);
    TEST_ASSERT(SUCCEEDED(hr));

    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &dwriteFactory);
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tf;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"ar-sa", &tf);
    tf->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &textBrush);

    std::wstring complexArabic = L"\x0645\x0643\x062a\x0628\x0629 \x0627\x0644\x0648\x0633\x0627\x0626\x0637 - \x062a\x064e\x0634\x0652\x063a\x0650\x064a\x0644";
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    dwriteFactory->CreateTextLayout(complexArabic.c_str(), (UINT32)complexArabic.length(), tf.Get(), (float)width, (float)height, &layout);
    layout->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
    rt->DrawTextLayout(D2D1::Point2F(0, 0), layout.Get(), textBrush.Get());
    hr = rt->EndDraw();
    TEST_ASSERT(SUCCEEDED(hr));

    // Inspect rasterized buffer to guarantee actual non-black pixels exist
    WICRect lockRect = { 0, 0, (INT)width, (INT)height };
    Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
    hr = wicBitmap->Lock(&lockRect, WICBitmapLockRead, &lock);
    TEST_ASSERT(SUCCEEDED(hr));
    UINT bufferSize = 0;
    BYTE* pData = nullptr;
    hr = lock->GetDataPointer(&bufferSize, &pData);
    TEST_ASSERT(SUCCEEDED(hr));

    size_t nonZeroPixels = 0;
    for (UINT i = 0; i < bufferSize; i += 4) {
        BYTE b = pData[i];
        BYTE g = pData[i + 1];
        BYTE rVal = pData[i + 2];
        if (rVal > 10 || g > 10 || b > 10) {
            nonZeroPixels++;
        }
    }

    // Must have rendered text glyph pixels
    TEST_ASSERT(nonZeroPixels > 50);
}

} // namespace Aperture::Test
