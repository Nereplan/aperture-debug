#pragma once
#include "../support/TestFramework.h"
#include "../../src/OverlayRenderer.h"

namespace Aperture::Test {

TEST_CASE(OverlaySurface, EmptyBuffersReleaseAndVisibleContentSurvivesRecreation) {
    struct Apartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    ComPtr<ID2D1Factory1> d2d;
    ComPtr<IDWriteFactory> dwrite;
    TEST_ASSERT(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())));
    TEST_ASSERT(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))));
    struct Window {
        HWND handle = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"STATIC", L"overlay surface test",
            WS_POPUP, 0, 0, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    TEST_ASSERT(window.handle != nullptr);
    OverlayRenderer overlay;
    TEST_ASSERT(overlay.Initialize(d2d.Get(), dwrite.Get(), 1.0f));
    overlay.Resize(320, 180);

    for (int cycle = 0; cycle < 3; ++cycle) {
        overlay.BeginDraw();
        overlay.FillRect(D2D1::RectF(10, 10, 100, 80), D2D1::ColorF(1, 0, 0, 1));
        TEST_ASSERT(overlay.EndDrawAndPresent(window.handle, 0, 0));
        TEST_ASSERT(!overlay.ReleaseEmptySurface());
        TEST_ASSERT_EQ(overlay.GetFrameStatistics().surfaceBytes, uint64_t(320 * 180 * 4));

        // BeginDraw clears the previous frame. No primitives means the full
        // presented image is transparent, so the drawing storage can retire.
        overlay.BeginDraw();
        TEST_ASSERT(overlay.EndDrawAndPresent(window.handle, 0, 0));
        TEST_ASSERT(overlay.ReleaseEmptySurface());
        TEST_ASSERT_EQ(overlay.GetFrameStatistics().surfaceBytes, uint64_t(0));
        TEST_ASSERT(!overlay.ReleaseEmptySurface());
    }
    TEST_ASSERT_EQ(overlay.GetFrameStatistics().framesPresented, uint64_t(6));
    TEST_ASSERT_EQ(overlay.GetFrameStatistics().presentationFailures, uint64_t(0));
}

} // namespace Aperture::Test
