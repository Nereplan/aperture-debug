#pragma once

// Deterministically testable shuffle sequence over queue indices: no index
// repeats until every other index has been visited; refills after exhaustion.
#include <random>
#include <vector>
#include <cstddef>
#include <cstdint>

namespace Aperture {

class ShuffleSequence {
public:
    explicit ShuffleSequence(uint32_t seed = 0) {
        if (seed == 0) {
            std::random_device device;
            rng = std::mt19937(device());
        } else {
            rng = std::mt19937(seed);
        }
    }
    void Reset() { unvisited.clear(); }
    size_t Next(size_t currentIndex, size_t count) {
        if (count <= 1) { unvisited.clear(); return count == 0 ? 0 : currentIndex; }
        if (unvisited.empty()) {
            unvisited.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                if (i != currentIndex) unvisited.push_back(i);
            }
        }
        std::uniform_int_distribution<size_t> pick(0, unvisited.size() - 1);
        const size_t slot = pick(rng);
        const size_t chosen = unvisited[slot];
        unvisited.erase(unvisited.begin() + static_cast<std::ptrdiff_t>(slot));
        return chosen;
    }

private:
    std::vector<size_t> unvisited;
    std::mt19937 rng;
};

} // namespace Aperture
