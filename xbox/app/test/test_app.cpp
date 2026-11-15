// The launcher's portable parts on a PC: the console models, the two modes' settings and how they are written into
// longshot_options.ini, the build estimate, and finding game folders on a drive (a temporary folder standing in for it).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../game_files.h"
#include "../profiles.h"

namespace fs = std::filesystem;
namespace lx = longshot_xbox;

static int failures;

#define CHECK(cond, what)                                      \
    do {                                                       \
        bool ok_ = (cond);                                     \
        std::printf("%s: %s\n", ok_ ? "ok  " : "FAIL", what);  \
        if (!ok_) ++failures;                                  \
    } while (0)

static void touch(const fs::path& p, size_t bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << std::string(bytes, 'x');
}

int main() {
    CHECK(lx::model_from_device_id(true, lx::kDeviceSeriesS) == lx::Model::series_s, "Series S id");
    CHECK(lx::model_from_device_id(true, lx::kDeviceSeriesX) == lx::Model::series_x, "Series X id");
    CHECK(lx::model_from_device_id(true, 0x12345678) == lx::Model::unknown, "an unknown id is unknown");
    CHECK(lx::model_from_device_id(false, lx::kDeviceSeriesX) == lx::Model::unknown, "another vendor is unknown");

    CHECK(&lx::profile_for(lx::Mode::automatic, lx::Model::series_x) == &lx::series_x(), "Auto on a Series X: Series X");
    CHECK(&lx::profile_for(lx::Mode::automatic, lx::Model::series_x_devkit) == &lx::series_x(), "Auto on a dev kit: Series X");
    CHECK(&lx::profile_for(lx::Mode::automatic, lx::Model::series_s) == &lx::series_s(), "Auto on a Series S: Series S");
    CHECK(&lx::profile_for(lx::Mode::automatic, lx::Model::unknown) == &lx::series_s(), "Auto on an unknown console: Series S");
    CHECK(&lx::profile_for(lx::Mode::series_x, lx::Model::series_s) == &lx::series_x(), "a chosen mode wins over the console");
    CHECK(lx::mode_from_key(lx::mode_key(lx::Mode::series_x)) == lx::Mode::series_x && lx::mode_from_key("junk") == lx::Mode::automatic,
          "mode keys round-trip, junk is Auto");
    CHECK(lx::series_s().memory_mb < lx::series_x().memory_mb && lx::series_s().compile_jobs < lx::series_x().compile_jobs,
          "the Series S asks for less than the Series X");

    const std::string before = "pad_a=0\r\nquality_preset=5\nsharpness=40\nquality_preset=4\nkey_b=66\n";
    const std::string after = lx::apply_options(before, lx::series_s());
    CHECK(after.find("quality_preset=1\n") != std::string::npos && after.find("quality_preset=5") == std::string::npos &&
              after.find("quality_preset=4") == std::string::npos,
          "the preset is replaced, a doubled key kept once");
    CHECK(after.find("pad_a=0\n") == 0 && after.find("sharpness=40\n") != std::string::npos && after.find("key_b=66\n") != std::string::npos,
          "the player's other lines are kept, in order");
    CHECK(after.find("output_res=1920x1080\n") != std::string::npos && after.find("frame_rate_cap=0\n") != std::string::npos,
          "missing keys are added");
    CHECK(lx::apply_options(after, lx::series_s()) == after, "applying twice changes nothing");
    CHECK(lx::apply_options(after, lx::series_x()).find("output_res=2560x1440\n") != std::string::npos, "switching modes rewrites");

    // (a console as fast per thread as the PC, one file at a time: the PC's 15 minutes times its 24 threads, scaled by
    // 587 ms with all threads against 259 ms on one)
    const double h = lx::build_hours(15, 587, 24, 259, 1);
    CHECK(h > 2.5 && h < 2.9, "the build estimate scales by threads and benchmark");
    CHECK(lx::build_hours(15, 587, 24, 259, 2) < h, "two files at a time is quicker");
    CHECK(lx::build_hours(0, 587, 24, 259, 1) == 0, "no estimate without figures");

    const fs::path drive = fs::temp_directory_path() / "longshot_xbox_test_drive";
    fs::remove_all(drive);
    touch(drive / "OoT" / "romfs" / "a.bin", 1000);
    touch(drive / "OoT" / "exefs" / "main", 10);
    touch(drive / "games" / "copy2" / "romfs" / "b.bin", 500);
    touch(drive / "games" / "copy2" / "exefs" / "main", 10);
    touch(drive / "a" / "b" / "c" / "romfs" / "deep.bin", 1);   // (three levels down: past the search)
    touch(drive / "a" / "b" / "c" / "exefs" / "main", 1);
    touch(drive / "half" / "romfs" / "x.bin", 1);                // (no exefs\main)
    touch(drive / "Longshot" / "work" / "romfs" / "y.bin", 1);   // (the build's own folder is never a game)
    touch(drive / "Longshot" / "work" / "exefs" / "main", 1);
    auto found = lx::find_game_folders({drive});
    bool oot = false, copy2 = false;
    for (auto& g : found) {
        oot |= g.folder == drive / "OoT" && g.romfs_bytes == 1000;
        copy2 |= g.folder == drive / "games" / "copy2" && g.romfs_bytes == 500;
    }
    CHECK(found.size() == 2 && oot && copy2, "two game folders found, their romfs sized; too deep, half and the work folder not");
    CHECK(lx::module_path(drive) == drive / "Longshot" / "game" / "longshot_game.dll", "the module's place on the drive");
    fs::remove_all(drive);

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
