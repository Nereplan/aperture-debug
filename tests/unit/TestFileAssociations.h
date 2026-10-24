#pragma once

#include "../support/TestFramework.h"
#include "../../src/Common.h"
#include "../../src/FileAssociations.h"
#include "../../src/MediaLibrary.h"
#include <string>

namespace Aperture::Test {

// Pure-logic coverage for the registry key-name derivation used by the opt-in
// file-type registration. The registry operations themselves are intentionally
// not unit-tested (they are explicit user-initiated actions with user-scope,
// reversible writes).
TEST_CASE(FileAssociations, ExtensionKeyDerivation) {
    // Canonical supported extensions arrive lowercase with a leading dot.
    TEST_ASSERT_EQ(FileAssociationExtensionKey(L".mkv"), std::wstring(L"mkv"));
    TEST_ASSERT_EQ(FileAssociationExtensionKey(L".mp4"), std::wstring(L"mp4"));
    // Case is normalized and every leading dot is stripped.
    TEST_ASSERT_EQ(FileAssociationExtensionKey(L".MKV"), std::wstring(L"mkv"));
    TEST_ASSERT_EQ(FileAssociationExtensionKey(L".M4A"), std::wstring(L"m4a"));
    TEST_ASSERT_EQ(FileAssociationExtensionKey(L"..ts"), std::wstring(L"ts"));
    // Degenerate input yields an empty key (callers skip it).
    TEST_ASSERT(FileAssociationExtensionKey(L"").empty());
    TEST_ASSERT(FileAssociationExtensionKey(L"...").empty());
    // Every canonical supported extension derives a non-empty lowercase key
    // without dots, suitable as a registry key name.
    for (const std::wstring& dotExtension : MediaLibrary::GetSupportedExtensions()) {
        const std::wstring key = FileAssociationExtensionKey(dotExtension);
        TEST_ASSERT(!key.empty());
        TEST_ASSERT(key.find(L'.') == std::wstring::npos);
        for (wchar_t c : key) {
            TEST_ASSERT((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'));
        }
    }
}

} // namespace Aperture::Test
