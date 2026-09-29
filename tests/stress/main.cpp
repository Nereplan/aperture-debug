#include "StressHarness.h"
#include "TestFolderWatcherStress.h"
#include "TestMediaBackfillStress.h"
#include "TestPlayerCallbacksStress.h"
#include "TestUpdaterServiceStress.h"
#include "TestOnlineSubtitlesStress.h"
#include "TestDiagnosticsStress.h"
#include "TestNetworkStreamLoaderStress.h"
#include "TestCompositeShutdownStress.h"

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

    auto& stressConfig = Aperture::Test::Stress::GetGlobalStressConfig();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--suite" && i + 1 < argc) {
            suiteFilter = argv[++i];
        } else if (arg == "--test" && i + 1 < argc) {
            testFilter = argv[++i];
        } else if (arg == "--iterations" && i + 1 < argc) {
            stressConfig.iterations = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--seed" && i + 1 < argc) {
            stressConfig.seed = std::stoull(argv[++i], nullptr, 0);
        } else if (arg == "--soak") {
            stressConfig.isSoak = true;
            stressConfig.iterations = 50; // 50 soak iterations
        } else if (arg == "--list") {
            const auto& tests = Aperture::Test::TestRegistry::Instance().GetTests();
            std::cout << "Available stress tests (" << tests.size() << "):\n";
            for (const auto& t : tests) {
                std::cout << "  [" << t.suite << "] " << t.name << "\n";
            }
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: Aperture_Stress_Tests.exe [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --iterations <N>  Number of iterations per test (default: " << stressConfig.iterations << ")\n";
            std::cout << "  --seed <S>        Deterministic PRNG seed (default: 0x" << std::hex << stressConfig.seed << std::dec << ")\n";
            std::cout << "  --soak            Run extended soak suite (50 iterations)\n";
            std::cout << "  --suite <name>    Run only tests from matching suite\n";
            std::cout << "  --test <name>     Run only tests matching name\n";
            std::cout << "  --list            List all registered tests\n";
            std::cout << "  --help, -h        Show this help message\n";
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
    std::cout << "  Aperture Deterministic Async Shutdown Stress Test Suite\n";
    std::cout << "  Iterations per test: " << stressConfig.iterations << " | Base Seed: 0x" << std::hex << stressConfig.seed << std::dec;
    if (stressConfig.isSoak) std::cout << " (SOAK MODE)";
    std::cout << "\n================================================================\n\n";

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

        std::cout << "  RUNNING  " << std::left << std::setw(50) << (test.suite + "." + test.name) << std::flush;

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

        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start).count();

        size_t failCountAfter = Aperture::Test::TestRegistry::Instance().GetFailures().size();

        if (failCountAfter == failCountBefore) {
            SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::cout << "\r  [PASS]   " << std::left << std::setw(50) << (test.suite + "." + test.name);
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::cout << " (" << elapsedMs << " ms, " << stressConfig.iterations << " iters)\n";
            passed++;
        } else {
            SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
            std::cout << "\r  [FAIL]   " << std::left << std::setw(50) << (test.suite + "." + test.name);
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::cout << " (" << elapsedMs << " ms)\n";
            failed++;
        }
    }

    auto totalElapsedSec = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - totalStart).count() / 1000.0;

    std::cout << "\n----------------------------------------------------------------\n";
    std::cout << "  Results: ";
    if (failed == 0) {
        SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        std::cout << passed << " passed";
    } else {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cout << failed << " failed";
        SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << ", " << passed << " passed";
    }
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    if (skipped > 0) std::cout << ", " << skipped << " skipped";
    std::cout << " (" << std::fixed << std::setprecision(2) << totalElapsedSec << "s total)\n";

    const auto& failures = Aperture::Test::TestRegistry::Instance().GetFailures();
    if (!failures.empty()) {
        std::cout << "\nFailures:\n";
        for (const auto& f : failures) {
            SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
            std::cout << "  " << f.testName << "\n";
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::cout << "    " << f.file << ":" << f.line << "\n";
            std::cout << "    Condition: " << f.condition << "\n";
            if (!f.message.empty()) {
                std::cout << "    Message:   " << f.message << "\n";
            }
        }
    }

    std::cout << "================================================================\n";

    CoUninitialize();
    return (failed == 0) ? 0 : 1;
}
