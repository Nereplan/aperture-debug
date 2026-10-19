#pragma once

#include "../support/TestFramework.h"
#include "../../src/MediaFoundation.h"
#include "../../src/TaskbarController.h"
#include "../../src/MediaTransport.h"
#include "../../src/TooltipManager.h"
#include "../../src/Renderer.h"

#include <windows.h>

namespace Aperture::Test {
static void step_DeferredServices_RendererConstructionAndUnusedShutdownStayLazy() {
    Renderer renderer;
    TEST_ASSERT(renderer.GetD2DFactory() == nullptr);
    TEST_ASSERT(renderer.GetDWriteFactory() == nullptr);
    TEST_ASSERT(!renderer.Initialize(nullptr));
    renderer.Resize(640, 480);
    renderer.SetDpi(144.0f);
    TEST_ASSERT(renderer.GetD2DFactory() == nullptr);
    TEST_ASSERT(renderer.GetDWriteFactory() == nullptr);
    renderer.Shutdown();
    renderer.Shutdown();
}
static void step_DeferredServices_RendererInitializesAndDrawsAfterDeferredConstruction() {
    struct Window {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Renderer startup test", WS_POPUP,
            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    TEST_ASSERT(window.handle != nullptr);
    Renderer renderer;
    TEST_ASSERT(renderer.Initialize(window.handle));
    TEST_ASSERT(renderer.GetD2DFactory() != nullptr);
    TEST_ASSERT(renderer.GetDWriteFactory() != nullptr);
    renderer.BeginDraw();
    renderer.Clear(D2D1::ColorF(D2D1::ColorF::Black));
    renderer.DrawString(L"Ready", D2D1::RectF(0, 0, 200, 50), D2D1::ColorF(D2D1::ColorF::White));
    TEST_ASSERT(renderer.EndDraw(false));
}


// deferred cold-path services must initialize exactly once when
// requested and shut down safely when never initialized (idle hub path).
static void step_DeferredServices_MediaFoundationLazyRefcount() {
    // Safe to shut down when never acquired (idle hub teardown path).
    MediaFoundation::ShutdownIfInitialized();
    TEST_ASSERT(!MediaFoundation::IsInitialized());

    // First acquire initializes; nested acquires only bump the count.
    bool first = MediaFoundation::Acquire();
    TEST_ASSERT(first);
    TEST_ASSERT(MediaFoundation::IsInitialized());
    bool second = MediaFoundation::Acquire();
    TEST_ASSERT(second);
    TEST_ASSERT(MediaFoundation::IsInitialized());

    // One release keeps MF alive (refcount 2 -> 1).
    MediaFoundation::Release();
    TEST_ASSERT(MediaFoundation::IsInitialized());

    // ScopedAcquire nests safely.
    {
        MediaFoundation::ScopedAcquire guard;
        TEST_ASSERT(guard.held());
        TEST_ASSERT(MediaFoundation::IsInitialized());
    }
    TEST_ASSERT(MediaFoundation::IsInitialized());

    // Final release shuts down; extra shutdown is a safe no-op.
    MediaFoundation::Release();
    TEST_ASSERT(!MediaFoundation::IsInitialized());
    MediaFoundation::ShutdownIfInitialized();
    TEST_ASSERT(!MediaFoundation::IsInitialized());
    // Unbalanced release never goes negative or crashes.
    MediaFoundation::Release();
    TEST_ASSERT(!MediaFoundation::IsInitialized());
}
static void step_DeferredServices_TaskbarNeverInitializedShutdownSafe() {
    TaskbarController controller;
    TEST_ASSERT(!controller.IsInitialized());
    // Idle hub: Clear/Shutdown with no window must not create COM objects.
    controller.Clear();
    TEST_ASSERT(!controller.IsInitialized());
    controller.Shutdown();
    TEST_ASSERT(!controller.IsInitialized());
    // Null window can never initialize.
    TEST_ASSERT(!controller.EnsureInitialized(nullptr));
    TEST_ASSERT(!controller.IsInitialized());
    controller.Shutdown();
    TEST_ASSERT(!controller.IsInitialized());
}
static void step_DeferredServices_MediaTransportNeverInitializedShutdownSafe() {
    MediaTransport transport;
    TEST_ASSERT(!transport.IsInitialized());
    // Idle hub: Clear/Shutdown with no SMTC must be safe no-ops.
    transport.Clear();
    TEST_ASSERT(!transport.IsInitialized());
    transport.Shutdown();
    TEST_ASSERT(!transport.IsInitialized());
    TEST_ASSERT(!transport.EnsureInitialized(nullptr));
    TEST_ASSERT(!transport.IsInitialized());
    transport.Shutdown();
    TEST_ASSERT(!transport.IsInitialized());
}
static void step_DeferredServices_TooltipNeverCreatedShutdownSafe() {
    TooltipManager manager;
    TEST_ASSERT(!manager.IsCreated());
    // All entry points must tolerate a never-created manager (hub with no
    // dwell/keyboard tooltip yet).
    TEST_ASSERT(!manager.SetActive(true));
    TEST_ASSERT(!manager.ShowForKeyboard(1001));
    manager.HideForKeyboard();
    manager.RelayMouseMessage(WM_MOUSEMOVE, 0, 0);
    TEST_ASSERT(!manager.AddOrUpdate(1001, L"Media", RECT{ 0, 0, 10, 10 }));
    TEST_ASSERT_EQ(manager.ToolCount(), (size_t)0);
    manager.Clear();
    manager.Destroy();
    TEST_ASSERT(!manager.IsCreated());
    // Null owner can never create.
    TEST_ASSERT(!manager.Create(nullptr));
    TEST_ASSERT(!manager.IsCreated());
    manager.Destroy();
    TEST_ASSERT(!manager.IsCreated());
}
static void step_DeferredServices_TooltipCreateIdempotentSingleWindow() {
    // A console-host window belongs to another process and may be a pseudoconsole
    // placeholder. Test with a real window owned by this thread, as the app does.
    struct Window {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Tooltip test", WS_POPUP,
            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    HWND owner = window.handle;
    TEST_ASSERT(owner != nullptr);
    TooltipManager manager;
    TEST_ASSERT(manager.Create(owner, 500, 360));
    TEST_ASSERT(manager.IsCreated());
    HWND firstHandle = manager.GetHandle();
    TEST_ASSERT(firstHandle != nullptr);
    // Second create for the same owner must not create a second window.
    TEST_ASSERT(manager.Create(owner, 500, 360));
    TEST_ASSERT(manager.IsCreated());
    TEST_ASSERT_EQ(manager.GetHandle(), firstHandle);
    TEST_ASSERT(manager.AddOrUpdate(1001, L"Media", RECT{ 0, 0, 100, 32 }));
    TEST_ASSERT_EQ(manager.ToolCount(), (size_t)1);
    manager.Destroy();
    TEST_ASSERT(!manager.IsCreated());
    // Double destroy is a safe no-op.
    manager.Destroy();
    TEST_ASSERT(!manager.IsCreated());
}


TEST_CASE(DeferredServices, LazySubsystemInitAndSafeShutdown) {
    step_DeferredServices_RendererConstructionAndUnusedShutdownStayLazy();
    step_DeferredServices_RendererInitializesAndDrawsAfterDeferredConstruction();
    step_DeferredServices_MediaFoundationLazyRefcount();
    step_DeferredServices_TaskbarNeverInitializedShutdownSafe();
    step_DeferredServices_MediaTransportNeverInitializedShutdownSafe();
    step_DeferredServices_TooltipNeverCreatedShutdownSafe();
    step_DeferredServices_TooltipCreateIdempotentSingleWindow();
}

} // namespace Aperture::Test
