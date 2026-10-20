#pragma once

// Included by a single benchmark translation unit per executable. Counts
// requested C++ allocations, excluding pre-existing input and process RSS.
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
struct AllocationStats { size_t calls = 0, bytes = 0, live = 0, peak = 0; } stats;
bool measuring = false;
struct alignas(std::max_align_t) AllocationHeader { size_t bytes; bool tracked; };
}

void* operator new(size_t size) {
    if (size > SIZE_MAX - sizeof(AllocationHeader)) throw std::bad_alloc();
    auto* header = static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader) + size));
    if (!header) throw std::bad_alloc();
    header->bytes = size;
    header->tracked = measuring;
    if (measuring) {
        ++stats.calls;
        stats.bytes += size;
        stats.live += size;
        stats.peak = (std::max)(stats.peak, stats.live);
    }
    return header + 1;
}
void operator delete(void* ptr) noexcept {
    if (!ptr) return;
    auto* header = static_cast<AllocationHeader*>(ptr) - 1;
    if (header->tracked) stats.live -= header->bytes;
    std::free(header);
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* ptr) noexcept { ::operator delete(ptr); }
void operator delete(void* ptr, size_t) noexcept { ::operator delete(ptr); }
void operator delete[](void* ptr, size_t) noexcept { ::operator delete(ptr); }
