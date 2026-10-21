#pragma once

#include "../support/TestFramework.h"
#include "../../src/Renderer.h"
#include "../../src/OverlayRenderer.h"

namespace Aperture::Test {
TEST_CASE(PlaybackRenderer, FactoriesStayCpuOnlyUntilGraphicsIntent) {
    struct Window {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Deferred hub test", WS_POPUP,
            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    TEST_ASSERT(window.handle != nullptr);
    Renderer renderer;
    TEST_ASSERT(renderer.Initialize(window.handle, "", false));
    TEST_ASSERT(renderer.GetD2DFactory() != nullptr);
    TEST_ASSERT(renderer.GetDWriteFactory() != nullptr);
    TEST_ASSERT(renderer.GetD2DContext() == nullptr);
    TEST_ASSERT(!renderer.AreDeviceWorkersRunning());
    TEST_ASSERT(!renderer.HasPresentedFrame());
    renderer.TrimHiddenLibraryCaches();
    TEST_ASSERT(renderer.GetD2DContext() == nullptr);
    TEST_ASSERT(!renderer.AreDeviceWorkersRunning());

    // Preview intent and returning to the library can acquire the context later.
    TEST_ASSERT(renderer.EnsureGraphicsContext());
    auto* context = renderer.GetD2DContext();
    TEST_ASSERT(context != nullptr);
    TEST_ASSERT(renderer.EnsureGraphicsContext());
    TEST_ASSERT(renderer.GetD2DContext() == context);
    renderer.BeginDraw();
    renderer.Clear(D2D1::ColorF(D2D1::ColorF::Black));
    TEST_ASSERT(renderer.EndDraw(false));

    for (int cycle = 0; cycle < 3; ++cycle) {
        renderer.TrimHiddenLibraryCaches(0, true);
        TEST_ASSERT(renderer.GetD2DContext() == nullptr);
        TEST_ASSERT(renderer.GetD2DFactory() != nullptr);
        TEST_ASSERT(renderer.GetDWriteFactory() != nullptr);
        TEST_ASSERT(!renderer.HasPresentedFrame());
        TEST_ASSERT(renderer.EnsureGraphicsContext());
        renderer.BeginDraw();
        renderer.Clear(D2D1::ColorF(D2D1::ColorF::Black));
        TEST_ASSERT(renderer.EndDraw(false));
        const auto size = renderer.GetD2DContext()->GetSize();
        TEST_ASSERT_EQ(size.width, 320.0f);
        TEST_ASSERT_EQ(size.height, 240.0f);
    }
}

TEST_CASE(PlaybackRenderer, SvgGeometryStaysWithItsIndependentFactory) {
    struct Window {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Hidden SVG factory test", WS_POPUP,
            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    TEST_ASSERT(window.handle != nullptr);
    Microsoft::WRL::ComPtr<ID2D1Factory1> previousFactory;
    for (int owner = 0; owner < 2; ++owner) {
        Renderer renderer;
        TEST_ASSERT(renderer.Initialize(window.handle, "", false));
        TEST_ASSERT(renderer.GetD2DFactory() != previousFactory.Get());
        previousFactory = renderer.GetD2DFactory();
        for (int cycle = 0; cycle < 2; ++cycle) {
            TEST_ASSERT(renderer.EnsureGraphicsContext());
            renderer.BeginDraw();
            renderer.Clear(D2D1::ColorF(D2D1::ColorF::Black));
            renderer.DrawSvgPath("M2,2 L22,2 L22,22 L2,22 Z", D2D1::RectF(10, 10, 110, 110),
                D2D1::ColorF(D2D1::ColorF::White));
            // EndDraw reports factory/resource-domain errors from the real D2D draw.
            TEST_ASSERT(renderer.EndDraw(false));
            renderer.TrimHiddenLibraryCaches(0, true);
            TEST_ASSERT(renderer.GetD2DFactory() == previousFactory.Get());
        }
        TEST_ASSERT(!IsWindowVisible(window.handle));
    }
}

TEST_CASE(PlaybackRenderer, OverlaySvgGeometryFollowsFactoryAcrossRestarts) {
    struct Window {
        HWND handle = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"STATIC", L"Hidden overlay SVG test",
            WS_POPUP, 0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    TEST_ASSERT(window.handle != nullptr);
    Renderer factories[2];
    for (auto& renderer : factories) TEST_ASSERT(renderer.Initialize(window.handle, "", false));
    TEST_ASSERT(factories[0].GetD2DFactory() != factories[1].GetD2DFactory());
    OverlayRenderer overlay;
    for (int cycle = 0; cycle < 4; ++cycle) {
        auto& owner = factories[cycle % 2];
        TEST_ASSERT(overlay.Initialize(owner.GetD2DFactory(), owner.GetDWriteFactory()));
        overlay.Resize(320, 240);
        overlay.BeginDraw();
        overlay.DrawSvgPath("M3,3 L21,3 L21,21 L3,21 Z", D2D1::RectF(10, 10, 110, 110),
            D2D1::ColorF(D2D1::ColorF::White));
        TEST_ASSERT(overlay.EndDrawAndPresent(window.handle, 0, 0));
        TEST_ASSERT(!overlay.ReleaseEmptySurface());
        TEST_ASSERT(!IsWindowVisible(window.handle));
        overlay.Shutdown();
    }
}
} // namespace Aperture::Test
