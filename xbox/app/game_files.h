// Where the player's own game files and the build's results are on removable storage. ROMFS and ExeFS are selected
// independently; build output and work stay on the ROMFS volume.
#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace longshot_xbox {

struct DataFolder {
    std::filesystem::path folder;
    std::uintmax_t bytes;   // (ROMFS size; zero for ExeFS)
    std::filesystem::path drive_root;
};

// Find ROMFS/ExeFS folders whose parent is at most `depth` levels below each removable root.
std::vector<DataFolder> find_romfs_folders(const std::vector<std::filesystem::path>& roots, int depth = 2);
std::vector<DataFolder> find_exefs_folders(const std::vector<std::filesystem::path>& roots, int depth = 2);

std::filesystem::path work_folder(const std::filesystem::path& drive);   // <drive>\Longshot\work
std::filesystem::path game_out(const std::filesystem::path& drive);      // <drive>\Longshot\game
std::filesystem::path module_path(const std::filesystem::path& drive);   // <drive>\Longshot\game\longshot_game.dll

}  // namespace longshot_xbox
