#include "../support/TestRunner.h"
#include "TestChannelPolicy.h"
#include "TestManifestValidation.h"
#include "TestCryptoSignature.h"
#include "TestAuthenticodePe.h"
#include "TestNetworkBounds.h"
#include "TestSwapRollback.h"
#include "TestHelperValidation.h"
#include "TestTempCleanup.h"
#include "TestPrivateModeIsolation.h"

int main(int argc, char* argv[]) {
    return Aperture::Test::RunRegisteredTests(argc, argv, "Aperture updater tests");
}
