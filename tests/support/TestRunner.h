#pragma once

#include "TestFramework.h"
#include <windows.h>
#include <objbase.h>
#include <iomanip>

namespace Aperture::Test {

inline int RunRegisteredTests(int argc, char* argv[], const char* title) {
    std::string suiteFilter, testFilter;
    bool list = false, help = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--suite" || arg == "--test") {
            if (i + 1 == argc || !*argv[i + 1] || std::string(argv[i + 1]).rfind("--", 0) == 0) {
                std::cerr << "Missing value for " << arg << "\n";
                return 2;
            }
            (arg == "--suite" ? suiteFilter : testFilter) = argv[++i];
        } else if (arg == "--list") list = true;
        else if (arg == "--help" || arg == "-h") help = true;
        else { std::cerr << "Unknown argument: " << arg << "\n"; return 2; }
    }
    if (help) {
        std::cout << "Usage: " << argv[0] << " [--suite <exact name>] [--test <substring>] [--list] [--help]\n";
        return 0;
    }
    auto& registry = TestRegistry::Instance();
    const auto& tests = registry.GetTests();
    std::vector<const TestRegistry::TestCase*> selected;
    for (const auto& test : tests) {
        if ((!suiteFilter.empty() && test.suite != suiteFilter) ||
            (!testFilter.empty() && test.name.find(testFilter) == std::string::npos)) continue;
        selected.push_back(&test);
    }
    if (selected.empty()) {
        std::cerr << "No tests matched the selection.\n";
        return 2;
    }
    if (list) {
        for (const auto* test : selected) std::cout << test->suite << "::" << test->name << "\n";
        return 0;
    }
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com;
    registry.ClearFailures();
    size_t passed = 0, failed = 0, skipped = 0;
    const auto start = std::chrono::steady_clock::now();
    std::cout << title << " (" << selected.size() << " selected, " << tests.size() - selected.size() << " filtered)\n";
    for (const auto* test : selected) {
        const std::string name = test->suite + "::" + test->name;
        registry.SetCurrentTest(name);
        const auto before = registry.FailureCount();
        const auto assertions = registry.AssertionCount();
        const auto testStart = std::chrono::steady_clock::now();
        bool didSkip = false;
        std::string skipReason;
        try { test->fn(); }
        catch (const TestSkipped& ex) { didSkip = true; skipReason = ex.what(); }
        catch (const std::exception& ex) { registry.AddFailure(name, "unknown", 0, "Uncaught std::exception", ex.what()); }
        catch (...) { registry.AddFailure(name, "unknown", 0, "Uncaught exception"); }
        if (!didSkip && registry.AssertionCount() == assertions && registry.FailureCount() == before) {
            registry.AddFailure(name, "unknown", 0, "Test executed no assertions");
        }
        const bool didFail = registry.FailureCount() != before;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - testStart).count();
        if (didFail) ++failed;
        else if (didSkip) ++skipped;
        else ++passed;
        std::cout << (didFail ? "[  FAILED  ] " : didSkip ? "[  SKIPPED ] " : "[       OK ] ")
                  << name << " (" << std::fixed << std::setprecision(2) << ms << " ms)";
        if (didSkip) std::cout << " " << skipReason;
        std::cout << "\n";
    }
    for (const auto& failure : registry.GetFailures()) {
        std::cout << "\n[" << failure.testName << "] " << failure.file << ":" << failure.line
                  << "\n  " << failure.condition;
        if (!failure.message.empty()) std::cout << ": " << failure.message;
        std::cout << "\n";
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << skipped << " skipped in " << ms << " ms\n";
    return failed ? 1 : 0;
}
} // namespace Aperture::Test
