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

namespace Aperture::Test {

// Helper to format values for error messages, including std::wstring and enum classes
template <typename T, typename Enable = void>
struct Formatter {
    static void Format(std::ostream& os, const T& value) {
        if constexpr (std::is_enum_v<T>) {
            os << static_cast<std::underlying_type_t<T>>(value);
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
        failures.push_back({ testName, file, line, cond, msg });
    }

    const std::vector<TestFailure>& GetFailures() const {
        return failures;
    }

    void ClearFailures() {
        failures.clear();
    }

    void SetCurrentTest(const std::string& name) {
        currentTest = name;
    }

    const std::string& GetCurrentTest() const {
        return currentTest;
    }

private:
    std::vector<TestCase> tests;
    std::vector<TestFailure> failures;
    std::string currentTest;
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
        if (!(cond)) { \
            ::Aperture::Test::TestRegistry::Instance().AddFailure( \
                ::Aperture::Test::TestRegistry::Instance().GetCurrentTest(), \
                __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define TEST_ASSERT_MSG(cond, msg) \
    do { \
        if (!(cond)) { \
            ::Aperture::Test::TestRegistry::Instance().AddFailure( \
                ::Aperture::Test::TestRegistry::Instance().GetCurrentTest(), \
                __FILE__, __LINE__, #cond, msg); \
        } \
    } while (0)

#define TEST_ASSERT_EQ(a, b) \
    do { \
        if (!((a) == (b))) { \
            std::ostringstream _oss; \
            _oss << "Expected: " << ::Aperture::Test::ToString(a) << " == " << ::Aperture::Test::ToString(b); \
            ::Aperture::Test::TestRegistry::Instance().AddFailure( \
                ::Aperture::Test::TestRegistry::Instance().GetCurrentTest(), \
                __FILE__, __LINE__, #a " == " #b, _oss.str()); \
        } \
    } while (0)

#define TEST_ASSERT_NEQ(a, b) \
    do { \
        if (!((a) != (b))) { \
            std::ostringstream _oss; \
            _oss << "Expected: " << ::Aperture::Test::ToString(a) << " != " << ::Aperture::Test::ToString(b); \
            ::Aperture::Test::TestRegistry::Instance().AddFailure( \
                ::Aperture::Test::TestRegistry::Instance().GetCurrentTest(), \
                __FILE__, __LINE__, #a " != " #b, _oss.str()); \
        } \
    } while (0)
