#include "../support/TestRunner.h"
#include "TestPortableRoot.h"
#include "TestUnwritableRoot.h"
#include "TestSchemaMigration.h"
#include "TestRotatingBackups.h"

#include "TestJsonLimits.h"




#include "TestRelativePaths.h"

#include "TestImportConflictPolicy.h"

int main(int argc, char* argv[]) {
    return Aperture::Test::RunRegisteredTests(argc, argv, "Aperture persistence tests");
}
