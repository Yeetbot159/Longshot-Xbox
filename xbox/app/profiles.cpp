#include "profiles.h"

#include <algorithm>
#include <sstream>

namespace longshot_xbox {

Model model_from_device_id(bool microsoft, uint32_t device_id) {
    if (!microsoft) return Model::unknown;
    switch (device_id) {
    case kDeviceSeriesS: return Model::series_s;
    case kDeviceSeriesX: return Model::series_x;
    case kDeviceSeriesXDevkit: return Model::series_x_devkit;
    case 0x768BAE26: case 0x2A7361D9: case 0x5AD617C7: case 0x10F7CDE3: return Model::xbox_one;   // (One, One S, One X, its dev kit)
    default: return Model::unknown;
    }
}

const char* model_name(Model m) {
    switch (m) {
    case Model::series_s: return "Xbox Series S";
    case Model::series_x: return "Xbox Series X";
    case Model::series_x_devkit: return "Xbox Series X dev kit";
    case Model::xbox_one: return "Xbox One family";
    default: return "unknown console";
    }
}

const char* mode_key(Mode m) {
    return m == Mode::series_s ? "series_s" : m == Mode::series_x ? "series_x" : "auto";
}

Mode mode_from_key(const std::string& key) {
    if (key == "series_s") return Mode::series_s;
    if (key == "series_x") return Mode::series_x;
    return Mode::automatic;
}

// (quality_preset: 1 Minimum (4GB), 2 Low (6GB), gpu/vk/vk_menu.cpp tx_preset; frame_rate_cap 0 is 60, the safe one;
// RX_VK_VRAM_MB the texture budget; RX_VK_MIRROR_MB caps imported-pool mirrors against the app's shared-memory limit;
// RX_VK_IMAGE_PREALLOC=0 drops the start-up's image pool, which on a PC took GPU memory the console does not have,
// xbox/README.md "Memory on a PC")
const Profile& series_s() {
    static const Profile p{
        "series_s",
        "Series S",
        "1080p, Minimum preset, 60 fps, 768 MB of textures, 256 MB mirror cap; builds one file at a time",
        5120,
        1,
        {{"quality_preset", "1"}, {"output_res", "1920x1080"}, {"frame_rate_cap", "0"}, {"handheld_profile", "0"}},
        {{"RX_VK_VRAM_MB", "768"}, {"RX_VK_MIRROR_MB", "256"}, {"RX_VK_IMAGE_PREALLOC", "0"},
         {"RX_VK_WORKERS", "1"}, {"RX_VK_LOAD_WORKERS", "1"}},
    };
    return p;
}

const Profile& series_x() {
    static const Profile p{
        "series_x",
        "Series X",
        "1440p (the game's own), Low preset, 60 fps, 1.5 GB of textures, 512 MB mirror cap; builds two files at a time",
        8192,
        2,
        {{"quality_preset", "2"}, {"output_res", "2560x1440"}, {"frame_rate_cap", "0"}, {"handheld_profile", "0"}},
        {{"RX_VK_VRAM_MB", "1536"}, {"RX_VK_MIRROR_MB", "512"}, {"RX_VK_IMAGE_PREALLOC", "0"},
         {"RX_VK_WORKERS", "2"}, {"RX_VK_LOAD_WORKERS", "2"}},
    };
    return p;
}

const Profile& profile_for(Mode mode, Model console) {
    if (mode == Mode::series_s) return series_s();
    if (mode == Mode::series_x) return series_x();
    return console == Model::series_x || console == Model::series_x_devkit ? series_x() : series_s();
}

std::string apply_options(const std::string& ini, const Profile& p) {
    std::vector<bool> done(p.options.size(), false);
    std::string out;
    std::istringstream in(ini);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        bool replaced = false;
        if (eq != std::string::npos) {
            const std::string key = line.substr(0, eq);
            for (size_t i = 0; i < p.options.size(); ++i) {
                if (p.options[i].first != key) continue;
                if (!done[i]) out += key + "=" + p.options[i].second + "\n";   // (a key twice in the file: kept once)
                done[i] = replaced = true;
                break;
            }
        }
        if (!replaced) out += line + "\n";
    }
    for (size_t i = 0; i < p.options.size(); ++i)
        if (!done[i]) out += p.options[i].first + "=" + p.options[i].second + "\n";
    return out;
}

double build_hours(double pc_minutes, double pc_bench_all_ms, unsigned pc_threads, double console_bench_one_ms, unsigned jobs) {
    if (pc_minutes <= 0 || pc_bench_all_ms <= 0 || pc_threads == 0 || console_bench_one_ms <= 0 || jobs == 0) return 0;
    // (the PC compiled with every thread: its work in "one-thread benchmark units" is minutes * threads / its time per
    // thread when all ran; the console does that work jobs at a time, each at its one-thread speed)
    const double pc_work = pc_minutes * pc_threads / pc_bench_all_ms;
    return pc_work * console_bench_one_ms / jobs / 60.0;
}

}  // namespace longshot_xbox
