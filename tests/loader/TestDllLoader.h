#pragma once

#include "../support/TestFramework.h"
#include "../../src/DllLoader.h"
#include "../../src/MpvPlayer.h"
#include <windows.h>
#include <string>
#include <vector>

namespace Aperture::Test {

TEST_CASE(DllLoader, ProcessDllSearchHardening) {
    bool ok = DllLoader::HardenProcessDllSearch();
    TEST_ASSERT(ok);
}

TEST_CASE(DllLoader, ResolvePackageDllPath) {
    std::wstring exeDir = GetExeDir();
    TEST_ASSERT(!exeDir.empty());

    std::wstring mpvPath = DllLoader::ResolvePackageDllPath(L"libmpv-2.dll");
    TEST_ASSERT(!mpvPath.empty());
    TEST_ASSERT(mpvPath.find(exeDir) == 0);
    TEST_ASSERT(mpvPath.find(L"libmpv-2.dll") != std::wstring::npos);

    // Test with leading backslash
    std::wstring mpvPath2 = DllLoader::ResolvePackageDllPath(L"\\libmpv-2.dll");
    TEST_ASSERT_EQ(mpvPath, mpvPath2);
}

TEST_CASE(DllLoader, CwdSearchOrderBlocked) {
    // 1. Ensure process DLL search hardening is active
    DllLoader::HardenProcessDllSearch();

    // 2. Save original CWD
    wchar_t origCwd[32768] = { 0 };
    GetCurrentDirectoryW(32768, origCwd);

    // 3. Create a temporary folder outside the package
    wchar_t tempPath[32768] = { 0 };
    GetTempPathW(32768, tempPath);
    std::wstring testDir = std::wstring(tempPath) + L"Aperture_Dll_Cwd_Test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(testDir.c_str(), NULL);

    // 4. Create a dummy test file pretending to be a DLL in CWD
    std::wstring dummyDllPath = testDir + L"\\cwd_rogue_test.dll";
    HANDLE hFile = CreateFileW(dummyDllPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        const char dummyBytes[] = "MZ_NOT_A_REAL_DLL_FOR_TEST";
        DWORD written = 0;
        WriteFile(hFile, dummyBytes, sizeof(dummyBytes), &written, NULL);
        CloseHandle(hFile);
    }

    // 5. Change CWD to the temporary directory
    SetCurrentDirectoryW(testDir.c_str());

    // 6. Attempt to load by bare name using LoadPackageLocalDll
    HMODULE hLoaded = DllLoader::LoadPackageLocalDll(L"cwd_rogue_test.dll");
    // MUST BE NULL: Must not look in CWD
    TEST_ASSERT(hLoaded == NULL);

    // 7. Also verify LoadLibraryEx with restricted flags rejects CWD lookups
    HMODULE hDirect = LoadLibraryExW(
        L"cwd_rogue_test.dll",
        NULL,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32
    );
    TEST_ASSERT(hDirect == NULL);

    // 8. Restore original CWD and clean up
    SetCurrentDirectoryW(origCwd);
    DeleteFileW(dummyDllPath.c_str());
    RemoveDirectoryW(testDir.c_str());
}

TEST_CASE(DllLoader, PackageLocalLibmpvLoads) {
    HMODULE hMpv = DllLoader::LoadPackageLocalDll(L"libmpv-2.dll");
    TEST_ASSERT(hMpv != NULL);

    if (hMpv) {
        FARPROC pCreate = GetProcAddress(hMpv, "mpv_create");
        FARPROC pInit = GetProcAddress(hMpv, "mpv_initialize");
        FARPROC pDestroy = GetProcAddress(hMpv, "mpv_destroy");
        FARPROC pTermDestroy = GetProcAddress(hMpv, "mpv_terminate_destroy");
        FARPROC pCommand = GetProcAddress(hMpv, "mpv_command");
        FARPROC pRenderCreate = GetProcAddress(hMpv, "mpv_render_context_create");

        TEST_ASSERT(pCreate != NULL);
        TEST_ASSERT(pInit != NULL);
        TEST_ASSERT(pDestroy != NULL || pTermDestroy != NULL);
        TEST_ASSERT(pCommand != NULL);
        TEST_ASSERT(pRenderCreate != NULL);

        FreeLibrary(hMpv);
    }
}

TEST_CASE(DllLoader, MpvPlayerLifecycleWithHardenedLoading) {
    // Create a dummy hidden message-only window for testing
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"Aperture_Test_Dummy_Class";
    RegisterClassEx(&wc);

    HWND hParent = CreateWindowEx(0, wc.lpszClassName, L"TestParent", 0, 0, 0, 100, 100, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    HWND hChild = CreateWindowEx(0, L"STATIC", L"TestChild", WS_CHILD, 0, 0, 100, 100, hParent, NULL, wc.hInstance, NULL);

    TEST_ASSERT(hParent != NULL);

    MpvPlayer player;
    bool initOk = player.Initialize(hParent, hChild);
    TEST_ASSERT(initOk);
    TEST_ASSERT(player.GetMpvDll() != NULL);

    // Verify player functions
    player.SetVolume(0.8);
    TEST_ASSERT_EQ(player.GetVolume(), 0.8);

    player.SetLoop(true);
    TEST_ASSERT(player.GetLoop());

    player.Shutdown();

    if (hParent) DestroyWindow(hParent);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
}

} // namespace Aperture::Test
