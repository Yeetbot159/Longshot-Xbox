"""Exercise the renderer's quad conversion and unsupported index-type guard."""
import argparse
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "{cxx}"]

HARNESS = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

using VkIndexType = int;
constexpr int VK_INDEX_TYPE_UINT8_EXT = 0, VK_INDEX_TYPE_UINT16 = 1, VK_INDEX_TYPE_UINT32 = 2;
constexpr int NVN_DRAW_PRIMITIVE_QUADS = 7;
struct Draw {
    int prim, count, itype, instances, base_vertex, base_instance, first;
    bool indexed, indirect;
    uint64_t ibuf;
};
struct State { bool restart = false; };
struct Device { bool index_uint8 = false; } dev;
struct Features { bool indexTypeUint8; };
void apply_features(const Features& u8) {
    {feature_guard}
}
struct Stream { uint8_t* mapped; int stream = 1; } storage, *cur = &storage;
struct Pass { int draws = 0; } ps;
int st_draws = 0, bound_type = -1, calls = 0, vertex_offset = 0;
uint32_t drawn_count = 0;
bool passed_regular = false;
alignas(16) uint8_t bytes[128];
void logf(const char*, ...) {}
bool memory_read_as_submitted(uint64_t a, void* dst, uint64_t n) {
    if (!a) return false;
    std::memcpy(dst, reinterpret_cast<const void*>(a), n);
    return true;
}
uint64_t stream_alloc(uint64_t n, uint64_t) { return n <= sizeof bytes ? 0 : UINT64_MAX; }
void bind_index(int, int, uint64_t, VkIndexType type) { bound_type = type; }
struct Occ { void issue() {} } occ;
void vkCmdDrawIndexed(int, uint32_t count, uint32_t, uint32_t, int32_t base, uint32_t) {
    ++calls; drawn_count = count; vertex_offset = base;
}
#define SKIP_HERE() return
void exercise(const Draw& d, const State& s) {
    const int cmd = 0;
""" + "\n{branch}\n" + r"""
    passed_regular = true;
}
int main() {
    dev.index_uint8 = true;
    apply_features(Features{false});
    assert(!dev.index_uint8);
    dev.index_uint8 = true;
    apply_features(Features{true});
    assert(dev.index_uint8);
    dev.index_uint8 = false;
    apply_features(Features{true});
    assert(!dev.index_uint8);
    storage.mapped = bytes;
    uint8_t input[] = {4, 3, 2, 1};
    Draw d{NVN_DRAW_PRIMITIVE_QUADS, 4, 0, 1, -3, 0, 0, true, false,
           reinterpret_cast<uint64_t>(input)};
    exercise(d, State{});
    assert(calls == 1 && drawn_count == 6 && vertex_offset == -3 && bound_type == VK_INDEX_TYPE_UINT32);
    const uint32_t expected[] = {4, 3, 2, 4, 2, 1};
    assert(std::memcmp(bytes, expected, sizeof expected) == 0);
    d.prim = 1;
    exercise(d, State{});
    assert(calls == 1 && !passed_regular);
    dev.index_uint8 = true;
    exercise(d, State{});
    assert(passed_regular);
    passed_regular = false;
    dev.index_uint8 = false;
    d.itype = 1;
    exercise(d, State{});
    assert(passed_regular);
    passed_regular = false;
    d.prim = NVN_DRAW_PRIMITIVE_QUADS;
    d.indirect = true;
    exercise(d, State{});
    assert(calls == 1 && !passed_regular);
}
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    start = source.index("    const VkIndexType itype = d.itype")
    end = source.index("\n    if (d.indirect) {", start)
    init = (ROOT / "gpu" / "vk" / "vk_main.cpp").read_text(encoding="utf-8")
    guard = "dev.index_uint8 = dev.index_uint8 && u8.indexTypeUint8;"
    if guard not in init:
        raise SystemExit("8-bit index support must require the queried feature")
    with tempfile.TemporaryDirectory() as tmp:
        cpp = Path(tmp) / "quad.cpp"
        exe = Path(tmp) / "quad.exe"
        cpp.write_text(HARNESS.replace("{branch}", source[start:end]).replace("{feature_guard}", guard), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("quad index conversion and unsupported index guard: ok")


if __name__ == "__main__":
    main()
