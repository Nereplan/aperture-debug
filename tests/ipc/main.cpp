#include "../support/TestFramework.h"
#include "TestIpcEnvelope.h"
#include <windows.h>
#include <objbase.h>
#include <iomanip>
#include <chrono>

int main(int argc, char* argv[]) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    const auto& tests = Aperture::Test::TestRegistry::Instance().GetTests();

    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    auto SetColor = [hConsole](WORD color) {
        if (hConsole != INVALID_HANDLE_VALUE) {
            SetConsoleTextAttribute(hConsole, color);
        }
    };

    SetColor(FOREGROUND_INTENSITY | FOREGROUND_BLUE | FOREGROUND_GREEN);
    std::cout << "========================================================\n";
    std::cout << "  Aperture Native - Focused IPC Test Runner             \n";
    std::cout << "========================================================\n";
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    size_t passedCount = 0;
    size_t failedCount = 0;

    auto startTime = std::chrono::high_resolution_clock::now();

    for (const auto& t : tests) {
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
        std::cout << " (Total time: " << std::fixed << std::setprecision(1) << totalElapsedMs << " ms)\n";
    } else {
        SetColor(FOREGROUND_INTENSITY | FOREGROUND_RED);
        std::cout << "TESTS FAILED: " << failedCount << " failed, " << passedCount << " passed";
        std::cout << " (Total time: " << std::fixed << std::setprecision(1) << totalElapsedMs << " ms)\n";
    }
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    std::cout << "========================================================\n";

    CoUninitialize();

    return failedCount == 0 ? 0 : 1;
}
