// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/shader_info.h"

namespace Shader::GuestAddressTable {

inline bool AddDescriptors(Info& info, const HostTranslateInfo& host, bool enabled, bool follow_stores) {
    constexpr u32 added_storage = kPoolCount + 1;
    constexpr u32 added_resources = added_storage;
    if (!enabled || !info.uses_global_memory || !host.support_int64 ||
        info.storage_buffers_descriptors.size() + 2 > Info::MAX_SSBOS) {
        return false;
    }

    const u32 storage = NumDescriptors(info.storage_buffers_descriptors);
    if (storage > Info::MAX_SSBOS - added_storage) {
        return false;
    }
    if (host.max_per_stage_descriptor_storage_buffers < added_storage ||
        storage > host.max_per_stage_descriptor_storage_buffers - added_storage ||
        host.max_descriptor_set_storage_buffers < added_storage ||
        storage > host.max_descriptor_set_storage_buffers - added_storage) {
        return false;
    }

    const u64 resources = (u64)NumDescriptors(info.constant_buffer_descriptors) + storage +
                          NumDescriptors(info.texture_buffer_descriptors) +
                          NumDescriptors(info.image_buffer_descriptors) +
                          NumDescriptors(info.texture_descriptors) + NumDescriptors(info.image_descriptors);
    if (host.max_per_stage_resources < added_resources ||
        resources > host.max_per_stage_resources - added_resources) {
        return false;
    }

    const bool written = follow_stores && info.stores_global_memory;
    info.storage_buffers_descriptors.push_back({kMetadataCbufIndex, 0, 1, written});
    info.storage_buffers_descriptors.push_back({kPoolArrayCbufIndex, 0, kPoolCount, written});
    info.used_storage_buffer_types |= IR::Type::U64;
    return true;
}

} // namespace Shader::GuestAddressTable
