#include "../support/TestRunner.h"
#include "TestIpcEnvelope.h"

int main(int argc, char* argv[]) {
    return Aperture::Test::RunRegisteredTests(argc, argv, "Aperture ipc tests");
}
