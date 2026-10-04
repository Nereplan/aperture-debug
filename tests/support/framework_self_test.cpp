#include "TestRunner.h"
#include <thread>

namespace {
int evaluations = 0;
int NextValue() { ++evaluations; return 1; }
void Good() { TEST_ASSERT_EQ(2 + 2, 4); }
void TemporaryElement() { TEST_ASSERT_EQ(std::vector<std::string>({"retained"}).front(), std::string("retained")); }
void Empty() {}
void Failure() { TEST_ASSERT(false); }
void Skip() { TEST_SKIP("fixture unavailable"); }
void FailureThenSkip() { TEST_ASSERT(false); TEST_SKIP("must not hide a failure"); }
void EvaluateOnce() { TEST_ASSERT_EQ(NextValue(), 2); }
void ThreadFailures() {
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) workers.emplace_back([] { TEST_ASSERT(false); });
    for (auto& worker : workers) worker.join();
}
}
int main() {
    auto& registry = Aperture::Test::TestRegistry::Instance();
    registry.Register("Good", "Assertion", Good);
    registry.Register("Temporary", "ElementLifetime", TemporaryElement);
    registry.Register("Empty", "NoAssertions", Empty);
    registry.Register("Failure", "Assertion", Failure);
    registry.Register("Skip", "Unavailable", Skip);
    registry.Register("FailureThenSkip", "Assertion", FailureThenSkip);
    registry.Register("EvaluateOnce", "FailedComparison", EvaluateOnce);
    registry.Register("ThreadFailures", "ConcurrentAssertions", ThreadFailures);
    int failures = 0;
    auto check = [&](std::vector<std::string> args, int expected, const char* output) {
        args.insert(args.begin(), "framework-self-test");
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        std::ostringstream captured;
        auto* oldOut = std::cout.rdbuf(captured.rdbuf());
        auto* oldError = std::cerr.rdbuf(captured.rdbuf());
        const int actual = Aperture::Test::RunRegisteredTests(static_cast<int>(argv.size()), argv.data(), "fixture");
        std::cout.rdbuf(oldOut);
        std::cerr.rdbuf(oldError);
        if (actual != expected || captured.str().find(output) == std::string::npos) {
            ++failures;
            std::cerr << "Runner self-test failed: " << args[1] << " -> " << actual << "\n" << captured.str();
        }
    };
    check({"--suite", "Good"}, 0, "1 passed, 0 failed, 0 skipped");
    check({"--suite", "Temporary"}, 0, "1 passed, 0 failed");
    check({"--suite", "Empty"}, 1, "Test executed no assertions");
    check({"--suite", "Failure"}, 1, "0 passed, 1 failed");
    check({"--suite", "Skip"}, 0, "0 passed, 0 failed, 1 skipped");
    check({"--suite", "FailureThenSkip"}, 1, "0 passed, 1 failed, 0 skipped");
    check({"--suite", "EvaluateOnce"}, 1, "Expected: 1 == 2");
    if (evaluations != 1) { ++failures; std::cerr << "Assertion evaluated its operand more than once\n"; }
    check({"--suite", "ThreadFailures"}, 1, "0 passed, 1 failed");
    if (registry.FailureCount() != 8) { ++failures; std::cerr << "Concurrent assertion failures were lost\n"; }
    check({"--suite", "Typo"}, 2, "No tests matched");
    check({"--test", "Typo"}, 2, "No tests matched");
    check({"--suite"}, 2, "Missing value");
    check({"--test", "--list"}, 2, "Missing value");
    check({"--typo"}, 2, "Unknown argument");
    check({"--list", "--suite", "Typo"}, 2, "No tests matched");
    std::cout << "Runner self-test: 14 scenarios, " << failures << " failures\n";
    return failures ? 1 : 0;
}
