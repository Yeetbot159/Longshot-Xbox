// The two console modes, Series S and Series X: what the game is set to on each, how the builder runs on each, and how
// the app tells them apart. Portable C++ (no WinRT), so a PC test checks it (xbox/app/test).
//
// What the consoles give a UWP Game in Developer Mode (Microsoft's "System resources for UWP apps and games on Xbox", and
// reports from developers for the Series X's larger figure; the probe measures the real one, xbox/README.md):
//
//                  Series S                           Series X
//   CPU            8 Zen 2 cores, 3.4 GHz with SMT    8 Zen 2 cores, 3.66 GHz with SMT
//   for a Game     4 cores of its own, 2 shared       the same
//   GPU            RDNA 2, 20 CUs, 4 TFLOPS           RDNA 2, 52 CUs, 12.15 TFLOPS
//   memory         10 GB, 224 GB/s                    16 GB, 560 GB/s
//   for a Game     about 5 GB, CPU and GPU together   about 8 GB reported (5 GB documented)
//   Direct3D 12    feature level 11.0, shader model 6.4 at most, on both
//   aimed at       1440p (Microsoft's advice)         4K
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace longshot_xbox {

enum class Model { unknown, series_s, series_x, series_x_devkit, xbox_one };

// GetGamingDeviceModelInformation's deviceId (gamingdeviceinformation.h), as reported since the April 2022 recovery.
constexpr uint32_t kDeviceSeriesS = 0x1D27FABB;
constexpr uint32_t kDeviceSeriesX = 0x2F7A3DFF;
constexpr uint32_t kDeviceSeriesXDevkit = 0xDE8A5661;

Model model_from_device_id(bool microsoft, uint32_t device_id);   // (vendorId GAMING_DEVICE_VENDOR_ID_MICROSOFT)
const char* model_name(Model m);

enum class Mode { automatic, series_s, series_x };
const char* mode_key(Mode m);   // (launcher.ini's mode=)
Mode mode_from_key(const std::string& key);

struct Profile {
    const char* key;            // "series_s", "series_x"
    const char* name;           // "Series S"
    const char* summary;        // one line for the launcher
    uint32_t memory_mb;         // what a Game is expected to get, CPU and GPU together
    uint32_t renderer_memory_mb; // maximum VMA allocation budget for this profile
    unsigned compile_jobs;      // files the on-console builder compiles at once (0.8-1.7 GB each)
    std::vector<std::pair<std::string, std::string>> options;   // longshot_options.ini lines the mode sets
    std::vector<std::pair<std::string, std::string>> env;       // environment for the game's start
};

const Profile& series_s();
const Profile& series_x();
// The mode the player chose; Auto picks by the console, and an unknown console gets the Series S's (the safer one).
const Profile& profile_for(Mode mode, Model console);
uint32_t renderer_memory_cap_mb(const Profile& profile, uint64_t expected_limit_bytes);

// longshot_options.ini with the profile's lines set: each of its keys replaced where the file has it, added where not,
// every other line kept as it was (the player's controls, their other settings).
std::string apply_options(const std::string& ini, const Profile& p);

// Hours the on-console build is expected to take: the PC's known build time scaled by the CPU benchmark (bench.h) and
// by the files compiled at once. pc_minutes and pc_bench_all_ms are a PC's, console_bench_one_ms the console's.
double build_hours(double pc_minutes, double pc_bench_all_ms, unsigned pc_threads, double console_bench_one_ms, unsigned jobs);

}  // namespace longshot_xbox
