# SPDX-License-Identifier: GPL-3.0-or-later
"""The no-BDA guest-pool fallback must fit descriptor limits and use storage-buffer pointers.

The fallback is intentionally bounded to a fixed descriptor array. This compiles the production
eligibility function with small stand-ins for its descriptor types, exercises exact-limit and
rejection cases, then checks that the SPIR-V emitter keeps the pool-pointer path distinct from
the BDA page-table path.
    python -I -B tests/test_guest_address_fallback.py [--cxx g++]
"""
import argparse
from pathlib import Path
import re
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


def check_eligibility(cxx):
    source = (ROOT / "gpu/vk/guest_address_table.h").read_text(encoding="utf-8")
    function = block(source, "inline bool AddDescriptors(")
    prelude = r"""
#include <cstdint>
#include <iostream>
#include <vector>
using u32 = uint32_t;
using u64 = uint64_t;
namespace IR { enum Type : u32 { U64 = 1 }; }
namespace Shader {
struct Descriptor { u32 index{}, offset{}, count{}; bool written{}; };
template <typename T> u32 NumDescriptors(const std::vector<T>& descriptors) {
    u32 count = 0;
    for (const auto& descriptor : descriptors) count += descriptor.count;
    return count;
}
struct Info {
    static constexpr u32 MAX_SSBOS = 32;
    bool uses_global_memory{}, stores_global_memory{};
    std::vector<Descriptor> storage_buffers_descriptors, constant_buffer_descriptors;
    std::vector<Descriptor> texture_buffer_descriptors, image_buffer_descriptors;
    std::vector<Descriptor> texture_descriptors, image_descriptors;
    u32 used_storage_buffer_types{};
};
struct HostTranslateInfo {
    u32 max_per_stage_descriptor_storage_buffers{}, max_descriptor_set_storage_buffers{};
    u32 max_per_stage_resources{};
    bool support_int64{};
};
}
namespace Shader::GuestAddressTable {
constexpr u32 kPoolCount = 8, kMetadataCbufIndex = 90, kPoolArrayCbufIndex = 91;
"""
    checks = r"""
int main() {
    using namespace Shader::GuestAddressTable;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
    };
    auto host = [] {
        Shader::HostTranslateInfo h{};
        h.max_per_stage_descriptor_storage_buffers = 18;
        h.max_descriptor_set_storage_buffers = 18;
        h.max_per_stage_resources = 18;
        h.support_int64 = true;
        return h;
    };
    auto info = [] {
        Shader::Info i{};
        i.uses_global_memory = true;
        i.stores_global_memory = true;
        return i;
    };
    {
        auto h = host(); auto i = info();
        check(AddDescriptors(i, h, true, false), "exact descriptor/resource limits are accepted");
        check(i.storage_buffers_descriptors.size() == 2, "metadata and pool-array descriptors are appended");
        check(i.storage_buffers_descriptors[0].count == 1 &&
              i.storage_buffers_descriptors[1].count == kPoolCount, "descriptor counts match the fixed table");
        check(!i.storage_buffers_descriptors[0].written &&
              !i.storage_buffers_descriptors[1].written, "stores stay disabled unless opted in");
        check(i.used_storage_buffer_types == IR::U64, "metadata uses 64-bit storage words");
    }
    {
        auto h = host(); auto i = info();
        check(AddDescriptors(i, h, true, true), "opt-in store descriptors fit");
        check(i.storage_buffers_descriptors[0].written &&
              i.storage_buffers_descriptors[1].written, "opt-in stores mark both descriptors writable");
    }
    {
        auto h = host(); auto i = info(); i.stores_global_memory = false;
        check(AddDescriptors(i, h, true, true), "read-only shaders remain eligible");
        check(!i.storage_buffers_descriptors[0].written &&
              !i.storage_buffers_descriptors[1].written, "no writes are declared for read-only shaders");
    }
    {
        auto h = host(); auto i = info();
        check(!AddDescriptors(i, h, false, true), "disabled fallback is rejected");
        check(i.storage_buffers_descriptors.empty(), "rejection leaves descriptors unchanged");
        i.uses_global_memory = false;
        check(!AddDescriptors(i, h, true, true), "shaders without global memory are rejected");
        i.uses_global_memory = true; h.support_int64 = false;
        check(!AddDescriptors(i, h, true, true), "hosts without shader int64 are rejected");
    }
    {
        auto h = host(); auto i = info();
        i.storage_buffers_descriptors.push_back({0, 0, 9, false});
        h.max_per_stage_descriptor_storage_buffers = 17;
        check(!AddDescriptors(i, h, true, false), "per-stage storage-buffer limit is enforced");
        h = host(); h.max_descriptor_set_storage_buffers = 17;
        check(!AddDescriptors(i, h, true, false), "descriptor-set storage-buffer limit is enforced");
        h = host(); h.max_per_stage_resources = 17;
        check(!AddDescriptors(i, h, true, false), "per-stage resource limit is enforced");
    }
    {
        auto h = host(); auto i = info();
        i.storage_buffers_descriptors.resize(31);
        for (auto& d : i.storage_buffers_descriptors) d.count = 1;
        check(!AddDescriptors(i, h, true, false), "SSBO metadata capacity is enforced");
        check(i.storage_buffers_descriptors.size() == 31, "capacity rejection does not append descriptors");
    }
    return failures ? 1 : 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="guest-address-fallback-") as tmp:
        src = Path(tmp) / "check.cpp"
        exe = Path(tmp) / "check.exe"
        src.write_text(prelude + function + "\n}\n" + checks, encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)],
                       check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=10)


def check_spirv_path():
    source = (ROOT / "gpu/vk/eden/shader_recompiler/backend/spirv/spirv_emit_context.cpp").read_text(
        encoding="utf-8")
    if "const bool guest_pool_fallback{Sirit::ValidId(guest_pool_metadata)};" not in source:
        raise SystemExit("the fallback mode is not selected by the presence of its metadata descriptor")
    lookup = block(source, "const auto guest_lookup =")
    if "hit(OpAccessChain(element_pointer, pool_table, Const(pool), Const(0U), index));" not in lookup:
        raise SystemExit("the fixed-pool lookup does not return a storage-buffer element pointer")
    if "follow_pointers" in lookup:
        raise SystemExit("pointer-vs-address mode must not depend on follow_pointers")
    load = block(source, "const auto define_load{")
    if "if (guest_pool_fallback) OpReturnValue(OpLoad(type, target));" not in load:
        raise SystemExit("the no-BDA load does not use its storage-buffer pointer directly")
    if "else if (gp_level >= 3) OpReturnValue(psb_load(type, target, 4U));" not in load:
        raise SystemExit("the BDA page-table load no longer converts its physical address")
    write = block(source, "const auto define_write{")
    if "if (guest_pool_fallback) OpStore(target, data);" not in write:
        raise SystemExit("the no-BDA store does not use its storage-buffer pointer directly")
    if "else if (gp_level >= 3) OpStore(psb_member(type, target)" not in write:
        raise SystemExit("the BDA page-table store no longer converts its physical address")
    if not re.search(r"if \(follow_stores\)\s+guest_lookup", write):
        raise SystemExit("global stores are no longer opt-in")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_eligibility(args.cxx)
    check_spirv_path()
    print("PASS: guest-pool descriptor limits and no-BDA storage-pointer path")


if __name__ == "__main__":
    main()
