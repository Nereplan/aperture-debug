#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <chrono>
#include <type_traits>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace Aperture::Test {

// Desktop-dependent tests opt in explicitly; background runs report them skipped.
inline bool g_allowVisibleTestWindows = false;

// Helper to format values for error messages, including std::wstring and enum classes
template <typename T, typename Enable = void>
struct Formatter {
    static void Format(std::ostream& os, const T& value) {
        if constexpr (std::is_enum_v<T>) {
            os << +static_cast<std::underlying_type_t<T>>(value);
        } else {
            os << value;
        }
    }
};

template <>
struct Formatter<std::wstring> {
    static void Format(std::ostream& os, const std::wstring& ws) {
        for (wchar_t c : ws) {
            if (c >= 32 && c <= 126) os << (char)c;
            else os << "\\u" << std::hex << (int)c << std::dec;
        }
    }
};

template <>
struct Formatter<const wchar_t*> {
    static void Format(std::ostream& os, const wchar_t* ws) {
        if (!ws) { os << "(null)"; return; }
        Formatter<std::wstring>::Format(os, std::wstring(ws));
    }
};

template <>
struct Formatter<wchar_t*> {
    static void Format(std::ostream& os, wchar_t* ws) {
        Formatter<const wchar_t*>::Format(os, ws);
    }
};

template <size_t N>
struct Formatter<wchar_t[N]> {
    static void Format(std::ostream& os, const wchar_t (&value)[N]) {
        Formatter<const wchar_t*>::Format(os, value);
    }
};

template <typename T>
std::string ToString(const T& val) {
    std::ostringstream oss;
    Formatter<T>::Format(oss, val);
    return oss.str();
}

struct TestFailure {
    std::string testName;
    std::string file;
    int line;
    std::string condition;
    std::string message;
};

class TestRegistry {
public:
    using TestFn = void(*)();

    struct TestCase {
        std::string suite;
        std::string name;
        TestFn fn;
    };

    static TestRegistry& Instance() {
        static TestRegistry instance;
        return instance;
    }

    void Register(const std::string& suite, const std::string& name, TestFn fn) {
        tests.push_back({ suite, name, fn });
    }

    const std::vector<TestCase>& GetTests() const {
        return tests;
    }

    void AddFailure(const std::string& testName, const std::string& file, int line, const std::string& cond, const std::string& msg = "") {
        std::lock_guard<std::mutex> lock(failureMutex);
        failures.push_back({ testName, file, line, cond, msg });
    }

    std::vector<TestFailure> GetFailures() const {
        std::lock_guard<std::mutex> lock(failureMutex);
        return failures;
    }

    size_t FailureCount() const {
        std::lock_guard<std::mutex> lock(failureMutex);
        return failures.size();
    }

    void RecordAssertion() { assertions.fetch_add(1, std::memory_order_relaxed); }
    size_t AssertionCount() const { return assertions.load(std::memory_order_relaxed); }

    void ClearFailures() {
        std::lock_guard<std::mutex> lock(failureMutex);
        failures.clear();
    }

    void SetCurrentTest(const std::string& name) {
        std::lock_guard<std::mutex> lock(failureMutex);
        currentTest = name;
    }

    std::string GetCurrentTest() const {
        std::lock_guard<std::mutex> lock(failureMutex);
        return currentTest;
    }

private:
    mutable std::mutex failureMutex;
    std::atomic<size_t> assertions{0};
    std::vector<TestCase> tests;
    std::vector<TestFailure> failures;
    std::string currentTest;
};

// Compare inside one full expression so references to elements of temporary
// containers stay alive through both comparison and failure formatting.
template <bool Equal, typename A, typename B>
void AssertComparison(const A& actual, const B& expected, const char* file, int line, const char* condition) {
    auto& registry = TestRegistry::Instance();
    registry.RecordAssertion();
    bool matches;
    if constexpr (Equal) matches = actual == expected;
    else matches = actual != expected;
    if (!matches) {
        registry.AddFailure(registry.GetCurrentTest(), file, line, condition,
            "Expected: " + ToString(actual) + (Equal ? " == " : " != ") + ToString(expected));
    }
}

struct TestSkipped : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct TestRegistrar {
    TestRegistrar(const std::string& suite, const std::string& name, TestRegistry::TestFn fn) {
        TestRegistry::Instance().Register(suite, name, fn);
    }
};

} // namespace Aperture::Test

#define TEST_CASE(suite, name) \
    static void test_##suite##_##name(); \
    static ::Aperture::Test::TestRegistrar reg_##suite##_##name(#suite, #name, test_##suite##_##name); \
    static void test_##suite##_##name()

#define TEST_ASSERT(cond) \
    do { \
        ::Aperture::Test::TestRegistry::Instance().RecordAssertion(); \
        if (!(cond)) { \
            ::Aperture::Test::TestRegistry::Instance().AddFailure( \
                ::Aperture::Test::TestRegistry::Instance().GetCurrentTest(), \
                __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define TEST_ASSERT_MSG(cond, msg) \
    do { \
        ::Aperture::Test::TestRegistry::Instance().RecordAssertion(); \
        if (!(cond)) { \
            ::Aperture::Test::TestRegistry::Instance().AddFailure( \
                ::Aperture::Test::TestRegistry::Instance().GetCurrentTest(), \
                __FILE__, __LINE__, #cond, msg); \
        } \
    } while (0)

#define TEST_ASSERT_EQ(a, b) \
    do { ::Aperture::Test::AssertComparison<true>((a), (b), __FILE__, __LINE__, #a " == " #b); } while (0)

#define TEST_ASSERT_NEQ(a, b) \
    do { ::Aperture::Test::AssertComparison<false>((a), (b), __FILE__, __LINE__, #a " != " #b); } while (0)

#define TEST_SKIP(reason) throw ::Aperture::Test::TestSkipped(reason)
