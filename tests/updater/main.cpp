#include "../support/TestFramework.h"
#include "TestChannelPolicy.h"
#include "TestManifestValidation.h"
#include "TestCryptoSignature.h"
#include "TestAuthenticodePe.h"
#include "TestNetworkBounds.h"
#include "TestSwapRollback.h"
#include "TestHelperValidation.h"
#include "TestTempCleanup.h"
#include "TestPrivateModeIsolation.h"

#include <windows.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <chrono>

int main(int argc, char* argv[]) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    std::string suiteFilter = "";
    std::string testFilter = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--suite" && i + 1 < argc) {
            suiteFilter = argv[++i];
        } else if (arg == "--test" && i + 1 < argc) {
            testFilter = argv[++i];
        } else if (arg == "--list") {
            const auto& tests = Aperture::Test::TestRegistry::Instance().GetTests();
            std::cout << "Available updater tests (" << tests.size() << "):\n";
            for (const auto& t : tests) {
                std::cout << "  [" << t.suite << "] " << t.name << "\n";
            }
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: Aperture_Updater_Tests.exe [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --suite <name>   Run only tests from matching suite\n";
            std::cout << "  --test <name>    Run only tests matching name\n";
            std::cout << "  --list           List all registered tests\n";
            std::cout << "  --help, -h       Show this help message\n";
            return 0;
        }
    }

    const auto& tests = Aperture::Test::TestRegistry::Instance().GetTests();

    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    auto SetColor = [hConsole](WORD color) {
        if (hConsole != INVALID_HANDLE_VALUE) {
            SetConsoleTextAttribute(hConsole, color);
        }
    };

    std::cout << "================================================================\n";
    std::cout << "  Aperture Updater Deterministic Test Suite\n";
    std::cout << "================================================================\n\n";

    int passed = 0;
    int failed = 0;
    int skipped = 0;

    auto totalStart = std::chrono::high_resolution_clock::now();

    for (const auto& test : tests) {
        if (!suiteFilter.empty() && test.suite.find(suiteFilter) == std::string::npos) {
            skipped++;
            continue;
        }
        if (!testFilter.empty() && test.name.find(testFilter) == std::string::npos) {
            skipped++;
            continue;
        }

        Aperture::Test::TestRegistry::Instance().SetCurrentTest(test.suite + "." + test.name);
        size_t failCountBefore = Aperture::Test::TestRegistry::Instance().GetFailures().size();

        auto start = std::chrono::high_resolution_clock::now();

        try {
            test.fn();
        } catch (const std::exception& e) {
            Aperture::Test::TestRegistry::Instance().AddFailure(
                test.suite + "." + test.name, __FILE__, __LINE__, "Unhandled exception", e.what());
        } catch (...) {
            Aperture::Test::TestRegistry::Instance().AddFailure(
                test.suite + "." + test.name, __FILE__, __LINE__, "Unhandled exception", "Unknown exception type");
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start).count();

        size_t failCountAfter = Aperture::Test::TestRegistry::Instance().GetFailures().size();

        if (failCountAfter == failCountBefore) {
            passed++;
            SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::cout << "  [PASS] ";
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::cout << test.suite << "." << test.name;
            std::cout << " (" << std::fixed << std::setprecision(2) << (elapsed / 1000.0) << " ms)\n";
        } else {
            failed++;
            SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
            std::cout << "  [FAIL] ";
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::cout << test.suite << "." << test.name << "\n";

            const auto& allFailures = Aperture::Test::TestRegistry::Instance().GetFailures();
            for (size_t i = failCountBefore; i < allFailures.size(); ++i) {
                SetColor(FOREGROUND_RED);
                std::cout << "         " << allFailures[i].file << ":" << allFailures[i].line
                          << " -> ASSERTION FAILED: " << allFailures[i].condition;
                if (!allFailures[i].message.empty()) {
                    std::cout << " (" << allFailures[i].message << ")";
                }
                std::cout << "\n";
            }
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        }
    }

    auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - totalStart).count();

    std::cout << "\n----------------------------------------------------------------\n";
    std::cout << "  Results: ";
    SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << passed << " passed";
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    std::cout << ", ";
    if (failed > 0) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cout << failed << " failed";
    } else {
        std::cout << "0 failed";
    }
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    std::cout << ", " << skipped << " skipped (" << totalElapsed << " ms)\n";
    std::cout << "----------------------------------------------------------------\n";

    CoUninitialize();
    return failed > 0 ? 1 : 0;
}
