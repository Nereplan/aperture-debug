#pragma once

#include <cstddef>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Aperture {

// Indices refer to the original vector. Preserve survivor order and ignore out-of-range selections, moving each survivor at most once.
template <class T> size_t EraseSelectedIndices(std::vector<T> &items, const std::unordered_set<size_t> &selected) {
    if (selected.empty()) return 0;
    size_t write = 0;
    for (size_t read = 0; read < items.size(); ++read) {
        if (selected.count(read)) continue;
        if (write != read) items[write] = std::move(items[read]);
        ++write;
    }
    const size_t removed = items.size() - write;
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(write), items.end());
    return removed;
}

} // namespace Aperture
