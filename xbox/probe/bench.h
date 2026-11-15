// One CPU benchmark, the same on the console (probe.cpp) and on a PC (bench_pc.cpp), so that the console's time can be
// set against a PC whose Longshot build time is known: a compiler-like load (sorting, hashing and branching over a few
// MB per thread), timed on one thread and on every thread at once.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bench {

inline uint64_t work(uint32_t seed) {
    uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1, sum = 0;
    for (int round = 0; round < 4; ++round) {
        std::vector<uint32_t> v(1 << 20);
        for (auto& e : v) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; e = uint32_t(x); }
        std::sort(v.begin(), v.end());
        std::unordered_map<uint32_t, uint32_t> m;
        m.reserve(1 << 17);
        for (size_t i = 0; i < v.size(); i += 8) m[v[i] >> 8] += uint32_t(i);
        std::string s;
        for (int i = 0; i < 1 << 16; ++i) s += char('a' + (v[i] % 26));
        for (char c : s) sum = sum * 31 + uint8_t(c);
        sum += m.size() + v[v.size() / 2];
    }
    return sum;
}

struct Result { double one_ms, all_ms; unsigned threads; };

inline Result run() {
    using clock = std::chrono::steady_clock;
    Result r{};
    r.threads = std::max(1u, std::thread::hardware_concurrency());
    work(1);   // (warm)
    auto t0 = clock::now();
    volatile uint64_t keep = work(2);
    r.one_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    std::atomic<uint64_t> total{0};
    std::vector<std::thread> ts;
    t0 = clock::now();
    for (unsigned i = 0; i < r.threads; ++i) ts.emplace_back([&, i] { total += work(3 + i); });
    for (auto& t : ts) t.join();
    r.all_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    (void)keep;
    return r;
}

}  // namespace bench
