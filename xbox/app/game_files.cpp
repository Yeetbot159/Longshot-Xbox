#include "game_files.h"

#include <system_error>

namespace fs = std::filesystem;

namespace longshot_xbox {

namespace {

bool is_game_folder(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p / "romfs", ec) && fs::is_regular_file(p / "exefs" / "main", ec);
}

std::uintmax_t tree_bytes(const fs::path& p) {
    std::error_code ec;
    std::uintmax_t n = 0;
    for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) n += it->file_size(e2);
    }
    return n;
}

void scan(const fs::path& dir, int depth, std::vector<GameFolder>& out) {
    if (is_game_folder(dir)) {
        out.push_back({dir, tree_bytes(dir / "romfs")});
        return;
    }
    if (depth <= 0) return;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const fs::path name = it->path().filename();
        if (!it->is_directory(e2) || name == "Longshot" || name == "System Volume Information" || name == "$RECYCLE.BIN") continue;
        scan(it->path(), depth - 1, out);
    }
}

}  // namespace

std::vector<GameFolder> find_game_folders(const std::vector<fs::path>& roots, int depth) {
    std::vector<GameFolder> out;
    for (const fs::path& r : roots) scan(r, depth, out);
    return out;
}

fs::path work_folder(const fs::path& drive) { return drive / "Longshot" / "work"; }
fs::path game_out(const fs::path& drive) { return drive / "Longshot" / "game"; }
fs::path module_path(const fs::path& drive) { return game_out(drive) / "longshot_game.dll"; }

}  // namespace longshot_xbox
