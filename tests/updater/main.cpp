#include "../support/TestRunner.h"
#include "TestManifestValidation.h"
#include "TestCryptoSignature.h"
#include "TestSwapRollback.h"

int main(int argc, char* argv[]) {
    return Aperture::Test::RunRegisteredTests(argc, argv, "Aperture updater tests");
}
