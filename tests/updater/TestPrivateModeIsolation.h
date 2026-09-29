#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Updater.h"
#include "../../src/Common.h"

namespace Aperture::Test {

TEST_CASE(UpdaterPrivateMode, PrivateModeSuppressesDurableHistory) {
    ScopedTempDir tempDir;
    std::wstring updaterJsonPath = tempDir.SubPath(L"data\\updater.json");

    UpdaterState state;
    state.lastCheckUtc = "2026-08-31T12:00:00Z";
    state.lastCheckStatus = "up_to_date";
    state.lastAvailableVersion = SemanticVersion::Current().ToString();
    state.lastError = "";

    // When private mode is active, SaveUpdaterState MUST NOT write anything to disk
    StorageStatus statusPrivate = SaveUpdaterState(state, true /* isPrivateMode */, tempDir.GetPath());
    TEST_ASSERT(statusPrivate.is_ok());
    TEST_ASSERT(!FileExists(updaterJsonPath));

    // When private mode is inactive, SaveUpdaterState writes to disk
    StorageStatus statusNormal = SaveUpdaterState(state, false /* isPrivateMode */, tempDir.GetPath());
    TEST_ASSERT(statusNormal.is_ok());
    TEST_ASSERT(FileExists(updaterJsonPath));

    // Verify loaded state matches
    UpdaterState loaded;
    StorageStatus loadStatus = LoadUpdaterState(loaded, tempDir.GetPath());
    TEST_ASSERT(loadStatus.is_ok());
    TEST_ASSERT_EQ(loaded.lastCheckUtc, "2026-08-31T12:00:00Z");
    TEST_ASSERT_EQ(loaded.lastCheckStatus, "up_to_date");
    TEST_ASSERT_EQ(loaded.lastAvailableVersion, SemanticVersion::Current().ToString());
}

} // namespace Aperture::Test
