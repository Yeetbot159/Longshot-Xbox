/* Split a renderer-wide VMA cap across Vulkan heaps. No Vulkan dependency: tests can run without a GPU. */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace mkwvk::vma_heap_rule {

inline std::vector<uint64_t> split_heap_limits(const std::vector<uint64_t>& heap_sizes, uint64_t cap) {
    std::vector<uint64_t> limits(heap_sizes.size(), 0);
    uint64_t total = 0;
    for (uint64_t size : heap_sizes) {
        if (size > std::numeric_limits<uint64_t>::max() - total) {
            total = std::numeric_limits<uint64_t>::max();
            break;
        }
        total += size;
    }
    if (!cap || !total) return limits;
    if (cap >= total) return heap_sizes;

    uint64_t remaining = cap;
    for (std::size_t i = 0; i < heap_sizes.size(); ++i) {
        const uint64_t proportional = static_cast<uint64_t>(
            static_cast<long double>(cap) * static_cast<long double>(heap_sizes[i]) / static_cast<long double>(total));
        limits[i] = std::min({heap_sizes[i], proportional, remaining});
        remaining -= limits[i];
    }
    for (std::size_t i = 0; remaining && i < heap_sizes.size(); ++i) {
        const uint64_t extra = std::min(remaining, heap_sizes[i] - limits[i]);
        limits[i] += extra;
        remaining -= extra;
    }
    return limits;
}

}  // namespace mkwvk::vma_heap_rule
