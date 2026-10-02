#include "../support/TestFramework.h"
#include "TestPlaybackSession.h"
#include "TestCommandDispatcher.h"
#include "TestLongPathSupport.h"
#include "TestRenderViewModel.h"
#include "TestMediaTransport.h"
#include "TestSeekbarThumbnailPreview.h"
#include "TestScreenshotCapture.h"
#include "TestColorManagement.h"
#include "TestGlobalSearch.h"
#include "TestAudioManagement.h"
#include "TestSubtitlesDual.h"
#include "TestSubtitleDiscovery.h"
#include "TestSubtitleProfiles.h"
#include "TestOnlineSubtitles.h"
#include "TestLibraryQuery.h"
#include "TestNetworkStream.h"
#include "TestTaskbarController.h"
#include "TestSmartPlaylists.h"
#include "TestBookmarks.h"
#include "TestHistoryGrouping.h"
#include "TestBulkLibraryOperations.h"
#include "TestDuplicateDetection.h"
#include "TestRendererCache.h"
#include "TestPresentation.h"
#include "TestAnimationPerformance.h"
#include "TestReorder.h"
#include "TestTouchInput.h"
#include "TestMetadataOverrides.h"
#include "TestVideoAdjustments.h"
#include "TestCustomMpvConfig.h"
#include "TestAccessibility.h"
#include "TestLocalization.h"
#include "TestRtlComplexScripts.h"
#include "TestDeferredServices.h"
#include "TestLibrarySnapshot.h"
#include "TestPlaylistIO.h"
#include "TestFolderOperations.h"
#include "TestEvaluationFixes.h"
#include "TestHttpTransportFixes.h"
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
            std::cout << "Available Aperture Unit Tests:\n";
            for (const auto& t : Aperture::Test::TestRegistry::Instance().GetTests()) {
                std::cout << "  " << t.suite << "::" << t.name << "\n";
            }
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: Aperture_Unit_Tests.exe [options]\n"
                      << "  --suite <name>   Run only tests in specified suite\n"
                      << "  --test <name>    Run only tests matching name\n"
                      << "  --list           List all registered tests\n"
                      << "  --help           Show this help message\n";
            return 0;
        }
    }

    const auto& tests = Aperture::Test::TestRegistry::Instance().GetTests();

    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    auto SetColor = [hConsole](WORD color) {
        if (hConsole && hConsole != INVALID_HANDLE_VALUE) {
            SetConsoleTextAttribute(hConsole, color);
        }
    };

    SetColor(FOREGROUND_INTENSITY | FOREGROUND_BLUE | FOREGROUND_GREEN);
    std::cout << "========================================================\n";
    std::cout << "  Aperture Native - Focused Component Unit Test Runner  \n";
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

        std::string testFullName = t.suite + "::" + t.name;
        Aperture::Test::TestRegistry::Instance().SetCurrentTest(testFullName);
        size_t failCountBefore = Aperture::Test::TestRegistry::Instance().GetFailures().size();

        std::cout << "[ RUN      ] " << std::left << std::setw(50) << testFullName << "\r" << std::flush;

        auto testStart = std::chrono::high_resolution_clock::now();
        try {
            t.fn();
        } catch (const std::exception& ex) {
            Aperture::Test::TestRegistry::Instance().AddFailure(
                testFullName, "unknown", 0, "Uncaught std::exception", ex.what());
        } catch (...) {
            Aperture::Test::TestRegistry::Instance().AddFailure(
                testFullName, "unknown", 0, "Uncaught non-std exception", "Unknown exception type");
        }
        auto testEnd = std::chrono::high_resolution_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(testEnd - testStart).count();

        size_t failCountAfter = Aperture::Test::TestRegistry::Instance().GetFailures().size();
        if (failCountAfter == failCountBefore) {
            SetColor(FOREGROUND_INTENSITY | FOREGROUND_GREEN);
            std::cout << "[       OK ] ";
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::cout << std::left << std::setw(50) << testFullName
                      << " (" << std::fixed << std::setprecision(2) << elapsedMs << " ms)\n";
            passedCount++;
        } else {
            SetColor(FOREGROUND_INTENSITY | FOREGROUND_RED);
            std::cout << "[  FAILED  ] ";
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::cout << std::left << std::setw(50) << testFullName
                      << " (" << std::fixed << std::setprecision(2) << elapsedMs << " ms)\n";
            failedCount++;
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    double totalElapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    const auto& failures = Aperture::Test::TestRegistry::Instance().GetFailures();
    if (!failures.empty()) {
        std::cout << "\n--------------------------------------------------------\n";
        SetColor(FOREGROUND_INTENSITY | FOREGROUND_RED);
        std::cout << "Failures summary:\n";
        SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        for (const auto& f : failures) {
            std::cout << "\n  [" << f.testName << "]\n";
            std::cout << "  File: " << f.file << ":" << f.line << "\n";
            std::cout << "  Condition: " << f.condition << "\n";
            if (!f.message.empty()) {
                std::cout << "  Detail: " << f.message << "\n";
            }
        }
    }

    std::cout << "--------------------------------------------------------\n";
    if (failedCount == 0) {
        SetColor(FOREGROUND_INTENSITY | FOREGROUND_GREEN);
        std::cout << "  ALL " << passedCount << " TESTS PASSED";
        SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << " in " << std::fixed << std::setprecision(2) << totalElapsedMs << " ms\n";
    } else {
        SetColor(FOREGROUND_INTENSITY | FOREGROUND_RED);
        std::cout << "  " << failedCount << " OF " << (passedCount + failedCount) << " TESTS FAILED";
        SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << " (" << passedCount << " passed, " << skippedCount << " skipped)\n";
    }
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    std::cout << "========================================================\n";

    CoUninitialize();
    return failedCount == 0 ? 0 : 1;
}
