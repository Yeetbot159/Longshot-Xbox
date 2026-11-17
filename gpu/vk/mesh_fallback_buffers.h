// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/shader_info.h"

#include <vector>

namespace Shader::MeshFallbackBuffers {

inline bool FitsDescriptorSetStorageBuffers(const std::vector<u32>& per_stage, u32 limit) {
    u64 total = 0;
    for (const u32 count : per_stage) {
        total += count;
        if (total > limit) {
            return false;
        }
    }
    return true;
}

inline bool AddDescriptors(Info& info, const HostTranslateInfo& host, bool has_indirect_input) {
    constexpr u32 kFallbackBuffers = 2;
    const u32 added_descriptors = kFallbackBuffers + (has_indirect_input ? 1U : 0U);
    if (info.storage_buffers_descriptors.size() + 1U + static_cast<u32>(has_indirect_input) > Info::MAX_SSBOS) {
        return false;
    }

    const u32 storage = NumDescriptors(info.storage_buffers_descriptors);
    if (storage > Info::MAX_SSBOS - added_descriptors ||
        host.max_per_stage_descriptor_storage_buffers < added_descriptors ||
        storage > host.max_per_stage_descriptor_storage_buffers - added_descriptors ||
        host.max_descriptor_set_storage_buffers < added_descriptors ||
        storage > host.max_descriptor_set_storage_buffers - added_descriptors) {
        return false;
    }

    const u64 resources = static_cast<u64>(NumDescriptors(info.constant_buffer_descriptors)) + storage +
                          NumDescriptors(info.texture_buffer_descriptors) +
                          NumDescriptors(info.image_buffer_descriptors) +
                          NumDescriptors(info.texture_descriptors) + NumDescriptors(info.image_descriptors);
    if (host.max_per_stage_resources < added_descriptors ||
        resources > host.max_per_stage_resources - added_descriptors) {
        return false;
    }

    info.storage_buffers_descriptors.push_back({kBuffersArrayCbufIndex, 0, kFallbackBuffers, false});
    if (has_indirect_input) {
        info.storage_buffers_descriptors.push_back({kIndirectInputCbufIndex, 0, 1, false});
    }
    info.used_storage_buffer_types |= IR::Type::U32;
    return true;
}

} // namespace Shader::MeshFallbackBuffers
