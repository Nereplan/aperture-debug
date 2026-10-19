#include "StressHarness.h"
#include "TestCompositeShutdownStress.h"

#include "../support/TestRunner.h"
#include <algorithm>
#include <limits>

int main(int argc, char* argv[]) {
    auto& config = Aperture::Test::Stress::GetGlobalStressConfig();
    std::vector<std::string> forwarded{argv[0]};
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--iterations" || arg == "--seed") {
                if (i + 1 == argc) throw std::invalid_argument("Missing value for " + arg);
                const std::string value = argv[++i];
                if (value.empty() || value[0] == '-') throw std::invalid_argument("Invalid value for " + arg);
                size_t consumed = 0;
                const auto parsed = std::stoull(value, &consumed, arg == "--seed" ? 0 : 10);
                if (consumed != value.size()) throw std::invalid_argument("Invalid value for " + arg);
                if (arg == "--iterations") {
                    if (!parsed || parsed > std::numeric_limits<uint32_t>::max()) throw std::invalid_argument("Iterations must be a positive uint32");
                    config.iterations = static_cast<uint32_t>(parsed);
                } else config.seed = parsed;
            } else if (arg == "--soak") {
                config.isSoak = true;
                config.iterations = 50;
            } else forwarded.push_back(arg);
        }
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 2;
    }
    std::cout << "Stress: " << config.iterations << " iterations, seed " << config.seed << "\n";
    if (std::find(forwarded.begin(), forwarded.end(), "--help") != forwarded.end() ||
        std::find(forwarded.begin(), forwarded.end(), "-h") != forwarded.end())
        std::cout << "Stress options: --iterations <positive count>, --seed <integer>, --soak\n";
    std::vector<char*> args;
    for (auto& arg : forwarded) args.push_back(arg.data());
    return Aperture::Test::RunRegisteredTests(static_cast<int>(args.size()), args.data(), "Aperture stress tests");
}
