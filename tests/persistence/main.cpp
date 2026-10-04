#include "../support/TestRunner.h"
#include "TestPortableRoot.h"
#include "TestRelativePaths.h"
#include "TestAbsoluteProfile.h"
#include "TestUnwritableRoot.h"
#include "TestJsonLimits.h"
#include "TestUnknownFields.h"
#include "TestPathRelocation.h"
#include "TestSchemaMigration.h"
#include "TestRotatingBackups.h"
#include "TestPartialRecovery.h"
#include "TestCacheIsolation.h"
#include "TestCacheBudget.h"
#include "TestImportConflictPolicy.h"
#include "TestFileIdentity.h"
#include "TestBulkRelocation.h"
#include "TestMediaPreferences.h"
#include "TestFolderWatcher.h"
#include "TestMetadataBackfill.h"

int main(int argc, char* argv[]) {
    return Aperture::Test::RunRegisteredTests(argc, argv, "Aperture persistence tests");
}
