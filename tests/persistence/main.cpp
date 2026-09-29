#include "../support/TestFramework.h"
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

#include <windows.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <chrono>

int main(int argc, char* argv[]) {
    // Initialize COM for Media Foundation or Shell APIs if needed
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
            std::cout << "Available persistence tests (" << tests.size() << "):\n";
            for (const auto& t : tests) {
                std::cout << "  [" << t.suite << "] " << t.name << "\n";
            }
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: Aperture_Persistence_Tests.exe [options]\n";
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

    SetColor(FOREGROUND_INTENSITY | FOREGROUND_BLUE | FOREGROUND_GREEN);
    std::cout << "========================================================\n";
    std::cout << "  Aperture Native - Focused Persistence Test Runner     \n";
    std::cout << "========================================================\n";
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    size_t passedCount = 0;
    size_t failedCount = 0;
    size_t skippedCount = 0;

    auto startTime = std::chrono::high_resolution_clock::now();

    for (const auto& t : tests) {
        if (!suiteFilter.empty() && t.suite != suiteFilter) {
            skippedCount++;
            continue;
        }
        if (!testFilter.empty() && t.name.find(testFilter) == std::string::npos) {
            skippedCount++;
            continue;
        }

        std::string fullName = "[" + t.suite + "] " + t.name;
        Aperture::Test::TestRegistry::Instance().SetCurrentTest(fullName);

        size_t failBefore = Aperture::Test::TestRegistry::Instance().GetFailures().size();

        auto testStart = std::chrono::high_resolution_clock::now();
        try {
            t.fn();
        } catch (const std::exception& ex) {
            Aperture::Test::TestRegistry::Instance().AddFailure(fullName, __FILE__, __LINE__, "std::exception", ex.what());
        } catch (...) {
            Aperture::Test::TestRegistry::Instance().AddFailure(fullName, __FILE__, __LINE__, "unknown exception", "Unhandled exception thrown in test");
        }
        auto testEnd = std::chrono::high_resolution_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(testEnd - testStart).count();

        size_t failAfter = Aperture::Test::TestRegistry::Instance().GetFailures().size();

        if (failAfter == failBefore) {
            SetColor(FOREGROUND_INTENSITY | FOREGROUND_GREEN);
            std::cout << "  [PASS] ";
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::cout << std::left << std::setw(55) << fullName
                      << " (" << std::fixed << std::setprecision(2) << elapsedMs << " ms)\n";
            passedCount++;
        } else {
            SetColor(FOREGROUND_INTENSITY | FOREGROUND_RED);
            std::cout << "  [FAIL] ";
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::cout << std::left << std::setw(55) << fullName
                      << " (" << std::fixed << std::setprecision(2) << elapsedMs << " ms)\n";
            failedCount++;
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    double totalElapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    const auto& failures = Aperture::Test::TestRegistry::Instance().GetFailures();

    if (!failures.empty()) {
        std::cout << "\n";
        SetColor(FOREGROUND_INTENSITY | FOREGROUND_RED);
        std::cout << "Failures (" << failures.size() << "):\n";
        SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        for (size_t i = 0; i < failures.size(); ++i) {
            const auto& f = failures[i];
            std::cout << "  " << (i + 1) << ") " << f.testName << "\n";
            std::cout << "     Location: " << f.file << ":" << f.line << "\n";
            std::cout << "     Condition: " << f.condition << "\n";
            if (!f.message.empty()) {
                std::cout << "     Message: " << f.message << "\n";
            }
            std::cout << "\n";
        }
    }

    std::cout << "--------------------------------------------------------\n";
    if (failedCount == 0) {
        SetColor(FOREGROUND_INTENSITY | FOREGROUND_GREEN);
        std::cout << "ALL TESTS PASSED: " << passedCount << " passed";
        if (skippedCount > 0) std::cout << ", " << skippedCount << " skipped";
        std::cout << " (Total time: " << std::fixed << std::setprecision(1) << totalElapsedMs << " ms)\n";
    } else {
        SetColor(FOREGROUND_INTENSITY | FOREGROUND_RED);
        std::cout << "TESTS FAILED: " << failedCount << " failed, " << passedCount << " passed";
        if (skippedCount > 0) std::cout << ", " << skippedCount << " skipped";
        std::cout << " (Total time: " << std::fixed << std::setprecision(1) << totalElapsedMs << " ms)\n";
    }
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    std::cout << "========================================================\n";

    CoUninitialize();

    return failedCount == 0 ? 0 : 1;
}
