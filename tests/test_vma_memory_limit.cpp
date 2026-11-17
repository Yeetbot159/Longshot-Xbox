#include "../gpu/vk/vma_heap_limits.h"

#include <cstdio>
#include <vector>

int main() {
    const auto single = mkwvk::vma_heap_rule::split_heap_limits({8}, 5);
    if (single != std::vector<uint64_t>{5}) return 1;

    const auto proportional = mkwvk::vma_heap_rule::split_heap_limits({4, 12}, 8);
    if (proportional != std::vector<uint64_t>{2, 6}) return 1;

    const auto clamped = mkwvk::vma_heap_rule::split_heap_limits({1, 7}, 6);
    if (clamped != std::vector<uint64_t>{1, 5}) return 1;

    const auto all = mkwvk::vma_heap_rule::split_heap_limits({2, 3}, 100);
    if (all != std::vector<uint64_t>{2, 3}) return 1;

    const auto zero = mkwvk::vma_heap_rule::split_heap_limits({2, 3}, 0);
    if (zero != std::vector<uint64_t>{0, 0}) return 1;

    std::puts("ok: VMA heap caps stay within the renderer-wide limit");
    return 0;
}
