#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Updater.h"
#include <windows.h>
#include <vector>
#include <string>

namespace Aperture::Test {

static void CreateMockPeFile(const std::wstring& path, WORD machine, WORD magic, WORD subsystem) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    IMAGE_DOS_HEADER dosHdr = {};
    dosHdr.e_magic = IMAGE_DOS_SIGNATURE; // 'MZ'
    dosHdr.e_lfanew = sizeof(IMAGE_DOS_HEADER) + 64; // Place NT headers after DOS stub

    DWORD written = 0;
    WriteFile(hFile, &dosHdr, sizeof(dosHdr), &written, NULL);

    // Padding for DOS stub
    std::vector<uint8_t> stub(64, 0);
    WriteFile(hFile, stub.data(), (DWORD)stub.size(), &written, NULL);

    DWORD peSignature = IMAGE_NT_SIGNATURE; // 'PE\0\0'
    WriteFile(hFile, &peSignature, sizeof(peSignature), &written, NULL);

    IMAGE_FILE_HEADER fileHdr = {};
    fileHdr.Machine = machine;
    fileHdr.NumberOfSections = 1;
    fileHdr.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    fileHdr.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    WriteFile(hFile, &fileHdr, sizeof(fileHdr), &written, NULL);

    IMAGE_OPTIONAL_HEADER64 optHdr = {};
    optHdr.Magic = magic;
    optHdr.Subsystem = subsystem;
    optHdr.SizeOfCode = 1024;
    optHdr.ImageBase = 0x0000000140000000ULL;
    optHdr.SectionAlignment = 0x1000;
    optHdr.FileAlignment = 0x200;
    WriteFile(hFile, &optHdr, sizeof(optHdr), &written, NULL);

    CloseHandle(hFile);
}

TEST_CASE(UpdaterAuthenticodePe, ValidPeArchitectureX64) {
    ScopedTempDir tempDir;
    std::wstring pePath = tempDir.SubPath(L"ValidAperture.exe");

    CreateMockPeFile(pePath, IMAGE_FILE_MACHINE_AMD64, IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_SUBSYSTEM_WINDOWS_GUI);

    std::string err;
    TEST_ASSERT(VerifyPeArchitectureX64(pePath, err));
    TEST_ASSERT(err.empty());
}

TEST_CASE(UpdaterAuthenticodePe, NonPeFileRejected) {
    ScopedTempDir tempDir;
    std::wstring txtPath = tempDir.SubPath(L"NotAPe.exe");
    tempDir.WriteFile(L"NotAPe.exe", "This is plain text and definitely not a portable executable.");

    std::string err;
    TEST_ASSERT(!VerifyPeArchitectureX64(txtPath, err));
    TEST_ASSERT(!err.empty());
}

TEST_CASE(UpdaterAuthenticodePe, IncompatiblePeMachineArchitectureRejected) {
    ScopedTempDir tempDir;

    // x86 (32-bit i386) Machine type: 0x014c
    std::wstring x86Path = tempDir.SubPath(L"Aperture_x86.exe");
    CreateMockPeFile(x86Path, 0x014c, IMAGE_NT_OPTIONAL_HDR32_MAGIC, IMAGE_SUBSYSTEM_WINDOWS_GUI);

    std::string err;
    TEST_ASSERT(!VerifyPeArchitectureX64(x86Path, err));
    TEST_ASSERT(!err.empty());

    // ARM64 Machine type: 0xaa64
    std::wstring arm64Path = tempDir.SubPath(L"Aperture_arm64.exe");
    CreateMockPeFile(arm64Path, 0xaa64, IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_SUBSYSTEM_WINDOWS_GUI);

    TEST_ASSERT(!VerifyPeArchitectureX64(arm64Path, err));
    TEST_ASSERT(!err.empty());
}

TEST_CASE(UpdaterAuthenticodePe, AuthenticodeThumbprintMismatch) {
    ScopedTempDir tempDir;
    std::wstring pePath = tempDir.SubPath(L"UnsignedAperture.exe");
    CreateMockPeFile(pePath, IMAGE_FILE_MACHINE_AMD64, IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_SUBSYSTEM_WINDOWS_GUI);

    std::string actualThumbprint;
    std::string err;
    // An unsigned binary should fail validation when a signed thumbprint is expected
    bool ok = VerifyAuthenticodeAndPin(pePath, "a1b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef0123456789abcdef0", actualThumbprint, err);
    TEST_ASSERT(!ok);
    TEST_ASSERT(!err.empty());
}

} // namespace Aperture::Test
