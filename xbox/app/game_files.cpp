#include "game_files.h"

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace longshot_xbox {

namespace {

enum class Kind { romfs, exefs };

bool named(const fs::path& p, const wchar_t* expected) {
    std::wstring name = p.filename().wstring();
    size_t i = 0;
    for (; i < name.size() && expected[i]; ++i) {
        wchar_t c = name[i];
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        if (c != expected[i]) return false;
    }
    return i == name.size() && expected[i] == L'\0';
}

bool is_data_folder(const fs::path& p, Kind kind) {
    std::error_code ec;
    if (kind == Kind::romfs) return named(p, L"romfs") && fs::is_directory(p, ec);
    return named(p, L"exefs") && fs::is_directory(p, ec) && fs::is_regular_file(p / "main", ec);
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

void scan(const fs::path& root, const fs::path& dir, int depth, Kind kind, std::vector<DataFolder>& out) {
    if (is_data_folder(dir, kind)) {
        out.push_back({dir, kind == Kind::romfs ? tree_bytes(dir) : 0, root});
        return;
    }
    if (depth <= 0) return;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const fs::path name = it->path().filename();
        if (!it->is_directory(e2) || name == "Longshot" || name == "System Volume Information" || name == "$RECYCLE.BIN") continue;
        scan(root, it->path(), depth - 1, kind, out);
    }
}

std::vector<DataFolder> find_folders(const std::vector<fs::path>& roots, int depth, Kind kind) {
    std::vector<DataFolder> out;
    for (const fs::path& root : roots) scan(root, root, depth + 1, kind, out);
    std::sort(out.begin(), out.end(), [](const DataFolder& a, const DataFolder& b) { return a.folder.wstring() < b.folder.wstring(); });
    return out;
}

}  // namespace

std::vector<DataFolder> find_romfs_folders(const std::vector<fs::path>& roots, int depth) { return find_folders(roots, depth, Kind::romfs); }

std::vector<DataFolder> find_exefs_folders(const std::vector<fs::path>& roots, int depth) { return find_folders(roots, depth, Kind::exefs); }

fs::path work_folder(const fs::path& drive) { return drive / "Longshot" / "work"; }
fs::path game_out(const fs::path& drive) { return drive / "Longshot" / "game"; }
fs::path module_path(const fs::path& drive) { return game_out(drive) / "longshot_game.dll"; }

}  // namespace longshot_xbox
