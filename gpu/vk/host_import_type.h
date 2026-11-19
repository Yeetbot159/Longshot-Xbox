#pragma once

#include <cstdint>

namespace mkwvk::host_import {

template <typename MemoryProperties>
uint32_t coherent_type(uint32_t host_types, uint32_t buffer_types, const MemoryProperties& memory, uint32_t required_flags) {
    const uint32_t candidates = host_types & buffer_types;
    for (uint32_t i = 0; i < memory.memoryTypeCount && i < 32; ++i)
        if ((candidates & (uint32_t{1} << i)) &&
            (memory.memoryTypes[i].propertyFlags & required_flags) == required_flags)
            return i;
    return UINT32_MAX;
}

}
