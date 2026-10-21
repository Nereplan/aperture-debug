#pragma once

#include "../support/TestFramework.h"
#include "../../src/Renderer.h"

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
} // namespace Aperture::Test
