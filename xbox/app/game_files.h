// Where the player's own game files and the build's results are on their USB drive. The PC launcher takes a folder holding
// the game's romfs and exefs folders; the console takes the same, anywhere in the drive's first two levels. The build
// works in <drive>\Longshot\work and installs the game in <drive>\Longshot\game (the module beside image.bin, as
// nx2/make_game.py installs it with LONGSHOT_XBOX=1); nothing of either is ever copied off the drive.
#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace longshot_xbox {

struct GameFolder {
    std::filesystem::path folder;   // (holds romfs\ and exefs\)
    std::uintmax_t romfs_bytes;
};

// The folders under each root (a drive, E:\) that hold romfs\ and exefs\main, down to `depth` levels.
std::vector<GameFolder> find_game_folders(const std::vector<std::filesystem::path>& roots, int depth = 2);

std::filesystem::path work_folder(const std::filesystem::path& drive);   // <drive>\Longshot\work
std::filesystem::path game_out(const std::filesystem::path& drive);      // <drive>\Longshot\game
std::filesystem::path module_path(const std::filesystem::path& drive);   // <drive>\Longshot\game\longshot_game.dll

}  // namespace longshot_xbox
