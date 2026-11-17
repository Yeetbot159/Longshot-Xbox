# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the fixed mesh-fallback storage-buffer descriptor eligibility helper."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/mesh_fallback_buffers.h").read_text(encoding="utf-8")
    shader_source = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    assert "FitsDescriptorSetStorageBuffers(" in shader_source
    assert "mesh fallback storage buffers exceed the descriptor-set limit" in shader_source
    helper = block(source, "inline bool AddDescriptors(")
    set_limit_helper = block(source, "inline bool FitsDescriptorSetStorageBuffers(")
    prelude = r"""
#include <cstdint>
#include <vector>
using u32 = uint32_t;
using u64 = uint64_t;
namespace IR { enum Type : u32 { U32 = 1 }; }
namespace Shader {
struct StorageBufferDescriptor {
    u32 cbuf_index{}, cbuf_offset{}, count{};
    bool is_written{};
};
template <typename T> u32 NumDescriptors(const std::vector<T>& descriptors) {
    u32 count = 0;
    for (const auto& descriptor : descriptors) count += descriptor.count;
    return count;
}
struct Info {
    static constexpr u32 MAX_SSBOS = 64;
    std::vector<StorageBufferDescriptor> storage_buffers_descriptors;
    std::vector<StorageBufferDescriptor> constant_buffer_descriptors, texture_buffer_descriptors;
    std::vector<StorageBufferDescriptor> image_buffer_descriptors, texture_descriptors, image_descriptors;
    u32 used_storage_buffer_types{};
};
struct HostTranslateInfo {
    u32 max_per_stage_descriptor_storage_buffers{}, max_descriptor_set_storage_buffers{}, max_per_stage_resources{};
};
}
namespace Shader::MeshFallbackBuffers {
constexpr u32 kBuffersArrayCbufIndex = ~u32{2};
constexpr u32 kIndirectInputCbufIndex = ~u32{3};
"""
    checks = r"""
}
int main() {
    using namespace Shader;
    auto host = [](u32 limit) {
        return HostTranslateInfo{limit, limit, limit};
    };
    auto check = [](bool condition) { return condition ? 0 : 1; };
    int failures = 0;
    failures += check(MeshFallbackBuffers::FitsDescriptorSetStorageBuffers({4, 3, 5}, 12));
    failures += check(!MeshFallbackBuffers::FitsDescriptorSetStorageBuffers({4, 3, 6}, 12));
    failures += check(!MeshFallbackBuffers::FitsDescriptorSetStorageBuffers({1}, 0));
    {
        Info info;
        auto h = host(3);
        failures += check(MeshFallbackBuffers::AddDescriptors(info, h, true));
        failures += check(info.storage_buffers_descriptors.size() == 2);
        failures += check(info.storage_buffers_descriptors[0].count == 2);
        failures += check(info.storage_buffers_descriptors[1].cbuf_index == MeshFallbackBuffers::kIndirectInputCbufIndex);
        failures += check(info.used_storage_buffer_types == IR::U32);
    }
    {
        Info info;
        auto h = host(2);
        failures += check(MeshFallbackBuffers::AddDescriptors(info, h, false));
        failures += check(info.storage_buffers_descriptors.size() == 1);
        failures += check(info.storage_buffers_descriptors[0].count == 2);
    }
    {
        Info info;
        auto h = host(2);
        failures += check(!MeshFallbackBuffers::AddDescriptors(info, h, true));
        failures += check(info.storage_buffers_descriptors.empty());
    }
    {
        Info info;
        info.storage_buffers_descriptors.push_back({0, 0, 2, false});
        auto h = host(4);
        failures += check(!MeshFallbackBuffers::AddDescriptors(info, h, true));
        failures += check(info.storage_buffers_descriptors.size() == 1);
    }
    return failures;
}
"""
    with tempfile.TemporaryDirectory(prefix="mesh-fallback-descriptors-") as tmp:
        cpp = Path(tmp) / "check.cpp"
        exe = Path(tmp) / "check.exe"
        cpp.write_text(prelude + set_limit_helper + "\n" + helper + "\n" + checks, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)],
                       check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=10)
    print("PASS: fixed mesh fallback descriptors fit exact limits and reject overflow")


if __name__ == "__main__":
    main()
