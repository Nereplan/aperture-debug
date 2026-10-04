#include "../support/TestRunner.h"
#include "TestDllLoader.h"

int main(int argc, char* argv[]) {
    return Aperture::Test::RunRegisteredTests(argc, argv, "Aperture loader tests");
}
