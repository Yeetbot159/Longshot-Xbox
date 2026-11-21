// Longshot on Xbox Series X|S: the launcher. One screen, driven by the controller (or a keyboard):
//
//   Console     the model, read from the console (GetGamingDeviceModelInformation), and the memory it gives the app
//   Mode        Auto, Series S or Series X: the game's settings and the builder's pace for that console (profiles.h)
//   Game files  the player's romfs and exefs folders, found on their USB drive (game_files.h)
//   Build       uses the package's Python and llvm-mingw toolchain to build the game from them on the console
//   Play        loads the game module the build left on the drive (xbox/loader), and will start the game
//
// The mode is applied to longshot_options.ini in LocalState, the file the game reads, whenever it changes.
#include <windows.h>
#include <d2d1_3.h>
#include <d3d11_3.h>
#include <dwrite_3.h>
#include <dxgi1_6.h>
#include <gamingdeviceinformation.h>
#include <wincodec.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../loader/pe_loader.h"
#include "../probe/bench.h"
#include "dcomp_shim.h"
#include "game_files.h"
#include "modules.h"
#include "platform.h"
#include "present.h"
#include "profiles.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;
using winrt::Windows::System::VirtualKey;
namespace fs = std::filesystem;
namespace lx = longshot_xbox;

namespace {

// (a PC's build for the estimate: docs/getting-started.md's "about 15 minutes on a 12-core desktop", and the benchmark
// on such a PC, 24 threads, xbox/README.md)
constexpr double kPcBuildMinutes = 15, kPcBenchAllMs = 587;
constexpr unsigned kPcThreads = 24;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

void write_file(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << data;
}

std::string mb(uint64_t bytes) {
    char b[32];
    snprintf(b, sizeof b, "%.1f GB", double(bytes) / (1ull << 30));
    return b;
}

// ---- the launcher's state, shared by the window and the worker ----

struct State {
    std::mutex m;
    lx::Model model = lx::Model::unknown;
    uint64_t memory_limit = 0;
    uint64_t expected_memory_limit = 0;
    lx::Mode mode = lx::Mode::automatic;
    std::vector<fs::path> drives;
    std::vector<lx::DataFolder> romfs_folders, exefs_folders;
    int romfs = -1, exefs = -1;
    bool scanning = false, busy = false;
    std::vector<std::string> notes;   // (the panel under the rows)

    void note(const std::string& s) {
        std::lock_guard lk(m);
        notes.push_back(s);
        if (notes.size() > 14) notes.erase(notes.begin());
    }
};

State S;
fs::path local_folder;
std::atomic<bool> g_playing{false};

fs::path drive_of(const fs::path& p) {
    std::lock_guard lk(S.m);
    for (const auto& folder : S.romfs_folders)
        if (folder.folder == p && !folder.drive_root.empty()) return folder.drive_root;
    for (const auto& folder : S.exefs_folders)
        if (folder.folder == p && !folder.drive_root.empty()) return folder.drive_root;
    return p.root_path();
}

void save_settings() {
    std::string romfs, exefs;
    {
        std::lock_guard lk(S.m);
        if (S.romfs >= 0 && S.romfs < int(S.romfs_folders.size())) romfs = narrow(S.romfs_folders[S.romfs].folder.wstring());
        if (S.exefs >= 0 && S.exefs < int(S.exefs_folders.size())) exefs = narrow(S.exefs_folders[S.exefs].folder.wstring());
        write_file(local_folder / "launcher.ini", std::string("mode=") + lx::mode_key(S.mode) + "\nromfs=" + romfs + "\nexefs=" + exefs + "\n");
    }
}

void apply_mode() {
    lx::Mode mode;
    lx::Model model;
    {
        std::lock_guard lk(S.m);
        mode = S.mode, model = S.model;
    }
    const lx::Profile& p = lx::profile_for(mode, model);
    const fs::path ini = local_folder / "longshot_options.ini";
    write_file(ini, lx::apply_options(read_file(ini), p));
    save_settings();
}

void load_settings(std::string& romfs, std::string& exefs) {
    std::istringstream in(read_file(local_folder / "launcher.ini"));
    std::string line;
    std::string legacy_game;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("mode=", 0) == 0) S.mode = lx::mode_from_key(line.substr(5));
        if (line.rfind("romfs=", 0) == 0) romfs = line.substr(6);
        if (line.rfind("exefs=", 0) == 0) exefs = line.substr(6);
        if (line.rfind("game=", 0) == 0) legacy_game = line.substr(5);
    }
    if (romfs.empty() && !legacy_game.empty()) romfs = narrow((fs::path(widen(legacy_game)) / "romfs").wstring());
    if (exefs.empty() && !legacy_game.empty()) exefs = narrow((fs::path(widen(legacy_game)) / "exefs").wstring());
}

lx::Model read_model() {
    GAMING_DEVICE_MODEL_INFORMATION info{};
    if (FAILED(GetGamingDeviceModelInformation(&info))) return lx::Model::unknown;
    return lx::model_from_device_id(info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT, uint32_t(info.deviceId));
}

void scan_drives(const std::string& wanted_romfs, const std::string& wanted_exefs) {
    {
        std::lock_guard lk(S.m);
        if (S.scanning) return;
        S.scanning = true;
    }
    std::vector<fs::path> drives;
    try {
        auto folders = winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync().get();
        for (auto const& f : folders) drives.emplace_back(std::wstring(f.Path()));
    } catch (...) {}
    // On the Series S probe, RemovableDevices was empty and E:\ did not exist, while D:\Drives did.
    // Xbox exposes some attached volumes beneath that directory instead of as a Win32 drive letter.
    if (drives.empty()) {
        std::error_code ec;
        const fs::path xbox_drives = L"D:\\Drives";
        for (fs::directory_iterator it(xbox_drives, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2)) drives.push_back(it->path());
        }
        if (drives.empty() && GetFileAttributesW(L"E:\\") != INVALID_FILE_ATTRIBUTES) drives.emplace_back(L"E:\\");
    }
    auto romfs_folders = lx::find_romfs_folders(drives);
    auto exefs_folders = lx::find_exefs_folders(drives);
    const size_t romfs_count = romfs_folders.size(), exefs_count = exefs_folders.size();
    int romfs = -1, exefs = -1;
    for (size_t i = 0; i < romfs_folders.size(); ++i)
        if (!wanted_romfs.empty() && narrow(romfs_folders[i].folder.wstring()) == wanted_romfs) romfs = int(i);
    for (size_t i = 0; i < exefs_folders.size(); ++i)
        if (!wanted_exefs.empty() && narrow(exefs_folders[i].folder.wstring()) == wanted_exefs) exefs = int(i);
    if (romfs < 0 && exefs < 0) {
        for (size_t i = 0; i < romfs_folders.size() && romfs < 0; ++i)
            for (size_t j = 0; j < exefs_folders.size(); ++j)
                if (romfs_folders[i].folder.parent_path() == exefs_folders[j].folder.parent_path()) {
                    romfs = int(i);
                    exefs = int(j);
                    break;
                }
    }
    if (romfs < 0 && !romfs_folders.empty()) romfs = 0;
    if (exefs < 0 && !exefs_folders.empty()) exefs = 0;
    {
        std::lock_guard lk(S.m);
        S.drives = drives;
        S.romfs_folders = std::move(romfs_folders);
        S.exefs_folders = std::move(exefs_folders);
        S.romfs = romfs;
        S.exefs = exefs;
        S.scanning = false;
    }
    S.note(romfs < 0 && exefs < 0
               ? "No game folders found. Expected Longshot\\game\\romfs and "
                 "Longshot\\game\\ExeFS\\main; press A to rescan."
               : "Found " + std::to_string(romfs_count) + " ROMFS and " + std::to_string(exefs_count) + " ExeFS folder(s).");
}

void run_or_measure_build() {
    lx::Mode mode;
    lx::Model model;
    fs::path romfs_folder, exefs_folder;
    {
        std::lock_guard lk(S.m);
        if (S.busy || g_playing) return;
        if (S.romfs >= 0 && S.romfs < int(S.romfs_folders.size())) romfs_folder = S.romfs_folders[S.romfs].folder;
        if (S.exefs >= 0 && S.exefs < int(S.exefs_folders.size())) exefs_folder = S.exefs_folders[S.exefs].folder;
        mode = S.mode, model = S.model;
        S.busy = true;
    }
    auto finish = [] {
        std::lock_guard lk(S.m);
        S.busy = false;
    };
    const lx::Profile& p = lx::profile_for(mode, model);

    // Look for the packaged builder tools, then the optional copy on the ROMFS drive.
    std::wstring pkg_dir = winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str();
    fs::path tools_pkg = fs::path(pkg_dir) / "tools";
    fs::path drive = romfs_folder.empty() ? fs::path() : drive_of(romfs_folder);
    fs::path drive_tools = drive.empty() ? fs::path() : (drive / "Longshot" / "tools");

    fs::path py_exe, mingw_dir, script;
    std::error_code ec;

    // 1. Check tools in package
    if (fs::exists(tools_pkg / "python" / "python.exe", ec) && fs::exists(tools_pkg / "llvm-mingw", ec)) {
        py_exe = tools_pkg / "python" / "python.exe";
        mingw_dir = tools_pkg / "llvm-mingw";
        script = tools_pkg / "nx2" / "make_game.py";
    }
    // 2. Check tools on USB drive
    else if (!drive_tools.empty() && fs::exists(drive_tools / "python" / "python.exe", ec) && fs::exists(drive_tools / "llvm-mingw", ec)) {
        py_exe = drive_tools / "python" / "python.exe";
        mingw_dir = drive_tools / "llvm-mingw";
        script = drive_tools / "nx2" / "make_game.py";
    }
    if (!romfs_folder.empty() && !exefs_folder.empty() && !py_exe.empty() && fs::exists(py_exe, ec) && fs::exists(script, ec)) {
        S.note(std::string("Building from selected ROMFS and ExeFS on console in ") + p.name + " mode (" +
               std::to_string(p.compile_jobs) + " job(s))...");
        fs::path out = lx::game_out(drive);
        fs::path work = lx::work_folder(drive);

        auto set_build_environment = [](const wchar_t* name, const wchar_t* value) {
            if (SetEnvironmentVariableW(name, value)) return true;
            const DWORD error = GetLastError();
            S.note("Could not configure " + narrow(name) + " for the console builder (" + std::to_string(error) + ").");
            return false;
        };
        if (!set_build_environment(L"LONGSHOT_XBOX", L"1") ||
            !set_build_environment(L"LONGSHOT_MINGW", mingw_dir.c_str()))
            return finish();
        // The Series S probe measured a 5,120 MB app limit and a 4,864 MB commit ceiling. Compile one
        // translation unit at a time and leave headroom for Python, the app, and the linker.
        if (!set_build_environment(L"LONGSHOT_MEM_LIMIT_GB", L"3.0") ||
            !set_build_environment(L"LONGSHOT_MEM_RESERVE_GB", L"1.5"))
            return finish();

        std::wstring cmd = L"\"" + py_exe.wstring() + L"\" -I -u \"" + script.wstring() +
                           L"\" --romfs \"" + romfs_folder.wstring() + L"\" --exefs \"" + exefs_folder.wstring() +
                           L"\" --out \"" + out.wstring() + L"\" --work \"" + work.wstring() +
                           L"\" --jobs " + std::to_wstring(p.compile_jobs) + L" --no-prepare-shaders";

        HANDLE r_pipe, w_pipe;
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        if (!CreatePipe(&r_pipe, &w_pipe, &sa, 0)) {
            S.note("Could not create pipe for build output.");
            return finish();
        }
        SetHandleInformation(r_pipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{sizeof si};
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = w_pipe;
        si.hStdError = w_pipe;
        PROCESS_INFORMATION pi{};

        fs::path cur_dir = script.parent_path().parent_path();
        const wchar_t* cur_dir_ptr = (!cur_dir.empty() && fs::exists(cur_dir, ec)) ? cur_dir.c_str() : nullptr;

        if (!CreateProcessW(py_exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cur_dir_ptr, &si, &pi)) {
            DWORD err = GetLastError();
            S.note("CreateProcessW failed (" + std::to_string(err) + ").");
            CloseHandle(r_pipe);
            CloseHandle(w_pipe);
            return finish();
        }
        CloseHandle(w_pipe);

        const fs::path log_path = local_folder / "build-console.log";
        std::ofstream build_log(log_path, std::ios::binary | std::ios::trunc);
        const bool logging = bool(build_log);
        if (!logging) S.note("Could not save build output to LocalState\\build-console.log.");
        std::string line_buf, last_line;
        bool reported_error = false;
        auto process_line = [&] {
            if (line_buf.empty()) return;
            last_line = line_buf;
            if (line_buf.rfind("@@stage", 0) == 0 || line_buf.rfind("@@plan", 0) == 0 ||
                line_buf.rfind("@@error", 0) == 0 || line_buf.rfind("@@done", 0) == 0) {
                if (line_buf.rfind("@@error", 0) == 0) reported_error = true;
                S.note(line_buf);
            } else if (line_buf.rfind("@@file", 0) == 0) {
                S.note("  " + line_buf.substr(7));
            }
            line_buf.clear();
        };
        char buf[256];
        DWORD read_bytes = 0;
        while (ReadFile(r_pipe, buf, sizeof buf, &read_bytes, nullptr) && read_bytes > 0) {
            if (build_log) build_log.write(buf, read_bytes);
            for (DWORD i = 0; i < read_bytes; ++i) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    process_line();
                } else {
                    line_buf.push_back(buf[i]);
                }
            }
        }
        process_line();
        CloseHandle(r_pipe);
        if (logging) {
            build_log.flush();
            if (!build_log) S.note("Could not finish saving LocalState\\build-console.log.");
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (exit_code == 0) {
            S.note("Build complete! longshot_game.dll ready. Press A on Play to start.");
        } else {
            if (!reported_error && !last_line.empty()) S.note("Builder: " + last_line);
            S.note("Build failed (" + std::to_string(exit_code) + ")." +
                   (logging && build_log ? " Full output: LocalState\\build-console.log" : " Build log unavailable."));
        }
        return finish();
    }

    // Otherwise, measure and provide the estimate
    S.note("Measuring this console's CPU (about ten seconds)...");
    auto b = bench::run();
    double hours = lx::build_hours(kPcBuildMinutes, kPcBenchAllMs, kPcThreads, b.one_ms, p.compile_jobs);
    char line[256];
    snprintf(line, sizeof line, "CPU: %.0f ms on one thread. Building here in %s mode (%u file(s) at a time): roughly %.1f hours.",
             b.one_ms, p.name, p.compile_jobs, hours);
    S.note(line);
    if (romfs_folder.empty() || exefs_folder.empty()) {
        S.note("Select both a ROMFS folder and an ExeFS folder before building.");
    } else {
        S.note("Build tools (Python and llvm-mingw) were not found in the app or USB drive.");
    }
    finish();
}

// ---- the game: the module built on the drive, its main on a thread of its own ----

CoreWindow g_window{nullptr};

struct GameStart {
    int (*main)(int, char**);
    std::vector<std::string> args;
};

DWORD WINAPI game_thread(void* p) {
    auto* g = static_cast<GameStart*>(p);
    std::vector<char*> argv;
    for (auto& a : g->args) argv.push_back(a.data());
    argv.push_back(nullptr);
    const int code = g->main(int(g->args.size()), argv.data());
    S.note("The game ended (" + std::to_string(code) + ").");
    g_playing = false;
    return 0;
}

void play() {
    fs::path module, romfs_folder, drive;
    lx::Model model;
    uint64_t memory_limit, expected_memory_limit;
    {
        std::lock_guard lk(S.m);
        if (S.busy || g_playing) return;
        if (S.romfs < 0 || S.romfs >= int(S.romfs_folders.size())) {
            S.notes.push_back("Select a ROMFS folder first.");
            return;
        }
        romfs_folder = S.romfs_folders[S.romfs].folder;
        drive = S.romfs_folders[S.romfs].drive_root;
        model = S.model, memory_limit = S.memory_limit, expected_memory_limit = S.expected_memory_limit;
        S.busy = true;
    }
    auto done = [] {
        std::lock_guard lk(S.m);
        S.busy = false;
    };
    if (drive.empty()) drive = romfs_folder.root_path();
    module = lx::module_path(drive);
    std::error_code ec;
    if (!fs::exists(module, ec)) {
        S.note("No game module yet at " + narrow(module.wstring()) + ": build the game first.");
        return done();
    }
    const fs::path game_folder = module.parent_path();
    const fs::path image = game_folder / "image.bin";
    if (!fs::is_regular_file(image, ec)) {
        S.note("No game image at " + narrow(image.wstring()) + ": build the game again.");
        return done();
    }
    if (GetACP() != CP_UTF8)
        S.note("UTF-8 paths are unavailable on this system; non-ASCII game paths may not work.");
    if (narrow(game_folder.wstring()).size() + sizeof("\\longshot_game.dll") > MAX_PATH) {
        S.note("The game folder path is too long. Move it to a shorter path and build again.");
        return done();
    }
    if (!SetEnvironmentVariableW(L"LONGSHOT_XBOX_GAME_DIR", game_folder.c_str())) {
        S.note("Could not set the game folder for the renderer (" + std::to_string(GetLastError()) + ").");
        return done();
    }
    S.note("Loading " + narrow(module.wstring()) + "...");
    // (the mode's renderer settings: in the process's environment before the module loads, as its C runtime copies the
    // environment when it starts)
    lx::Mode mode;
    {
        std::lock_guard lk(S.m);
        mode = S.mode;
    }
    const lx::Profile& profile = lx::profile_for(mode, model);
    for (const auto& [name, value] : profile.env) {
        const std::wstring wide_name = widen(name), wide_value = widen(value);
        if (!SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str())) {
            const DWORD error = GetLastError();
            S.note("Could not configure renderer profile " + name + " (" + std::to_string(error) + ").");
            return done();
        }
    }
    const std::string renderer_cap = std::to_string(lx::renderer_memory_cap_mb(profile, expected_memory_limit));
    const std::wstring wide_renderer_cap = widen(renderer_cap);
    if (!SetEnvironmentVariableW(L"RX_VK_MEMORY_MB", wide_renderer_cap.c_str())) {
        const DWORD error = GetLastError();
        S.note("Could not configure the renderer memory limit (" + std::to_string(error) + ").");
        return done();
    }
    // (the package's modules: the renderer, Dozen and the TLS slot; DComp.DLL is the app's own, for Dozen's swap chain)
    static const bool modules_ready = [] {
        lx::modules::Config cfg;
        cfg.folder = std::wstring(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
        cfg.local_state = local_folder.wstring();
        cfg.log = [](const std::string& s) {
            OutputDebugStringA((s + "\n").c_str());
            S.note(s);
        };
        lx::modules::configure(cfg);
        lx::modules::add_virtual("dcomp.dll", lx::dcomp::lookup);
        lx::dcomp::set_show(lx::present::set_game);
        return true;
    }();
    (void)modules_ready;
    char err[512];
    pel_module* m = lx::modules::load(module.wstring(), err, sizeof err);
    if (!m) {
        S.note(std::string("The game module did not load: ") + err);
        return done();
    }
    // (the desktop functions the renderer and Dozen import, answered by the game's stand-ins: host/uwp/win32_uwp.c)
    if (auto win32 = reinterpret_cast<lx::modules::Lookup>(pel_symbol(m, "longshot_uwp_win32"))) lx::modules::set_win32(win32);
    auto set_platform = reinterpret_cast<int (*)(const LongshotUwpPlatform*)>(pel_symbol(m, "longshot_uwp_set_platform"));
    auto game_main = reinterpret_cast<int (*)(int, char**)>(pel_symbol(m, "longshot_uwp_main"));
    if (!set_platform || !game_main) {
        S.note("That module is not a game built for the Xbox (no longshot_uwp_main): build it again.");
        pel_unload(m);
        return done();
    }
    const fs::path work = lx::work_folder(drive);
    lx::PlatformPaths paths{narrow(local_folder.wstring()), narrow(game_folder.wstring()), narrow(work.wstring())};
    const LongshotUwpPlatform* table =
        lx::platform_init(g_window, paths, uint32_t(model), memory_limit, [](const std::string& s) { S.note(s); });
    if (!set_platform(table)) {
        S.note("The game module was built for another version of this app: build it again.");
        pel_unload(m);
        return done();
    }
    char line[200];
    snprintf(line, sizeof line, "The game module loads: %s at %p. Starting...", mb(pel_size(m)).c_str(), pel_base(m));
    S.note(line);
    // (the command line a PC's launcher gives: the built image, the player's romfs, the user folder)
    auto* start = new GameStart{game_main, {"longshot"}};
    start->args.push_back(narrow(image.wstring()));
    start->args.insert(start->args.end(), {"--romfs", narrow(romfs_folder.wstring()), "--user", narrow(local_folder.wstring())});
    g_playing = true;
    // (the game's main thread: a PC's has 8 MB reserved by the build's link; more here, as reserving costs nothing)
    HANDLE t = CreateThread(nullptr, 64u << 20, game_thread, start, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) {
        g_playing = false;
        S.note("The game's thread did not start.");
        delete start;
    } else {
        CloseHandle(t);
    }
    done();   // (the module stays loaded: the game runs in it until the app ends)
}

// ---- the window ----

enum Row { kConsole, kMode, kRomfs, kExefs, kBuild, kPlay, kRows };

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
    IFrameworkView CreateView() { return *this; }
    void Initialize(CoreApplicationView const&) {}
    void Load(hstring const&) {}
    void Uninitialize() {}

    void SetWindow(CoreWindow const& w) {
        window_ = w;
        try {
            winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().SetDesiredBoundsMode(
                winrt::Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
        } catch (...) {}
        g_window = w;
        w.KeyDown([this](auto&&, KeyEventArgs const& e) {
            lx::platform_on_key(unsigned(e.VirtualKey()), true);
            if (!g_playing) key(e.VirtualKey());
        });
        w.KeyUp([](auto&&, KeyEventArgs const& e) { lx::platform_on_key(unsigned(e.VirtualKey()), false); });
        w.CharacterReceived([](auto&&, CharacterReceivedEventArgs const& e) { lx::platform_on_char(e.KeyCode()); });
        w.SizeChanged([](auto&&, WindowSizeChangedEventArgs const& e) {
            const float scale = float(winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel());
            lx::platform_on_resize(int(e.Size().Width * scale), int(e.Size().Height * scale));
        });
        w.Activated([](auto&&, WindowActivatedEventArgs const& e) {
            lx::platform_on_focus(e.WindowActivationState() != CoreWindowActivationState::Deactivated);
        });
        w.Closed([](auto&&, auto&&) { lx::platform_on_close(); });
        CoreApplication::Suspending([](auto&&, auto&&) { lx::platform_on_suspend(); });
        CoreApplication::Resuming([](auto&&, auto&&) { lx::platform_on_resume(); });
        SystemNavigationManager::GetForCurrentView().BackRequested([](auto&&, BackRequestedEventArgs const& e) { e.Handled(true); });   // (B would leave the app)
    }

    void Run() {
        window_.Activate();
        local_folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str();
        std::string wanted_romfs, wanted_exefs;
        load_settings(wanted_romfs, wanted_exefs);
        S.model = read_model();
        try { S.memory_limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit(); } catch (...) {}
        try { S.expected_memory_limit = winrt::Windows::System::MemoryManager::ExpectedAppMemoryUsageLimit(); } catch (...) {}
        apply_mode();
        std::thread([wanted_romfs, wanted_exefs] { winrt::init_apartment(); scan_drives(wanted_romfs, wanted_exefs); }).detach();
        init_graphics();
        while (true) {
            window_.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            draw();
        }
    }

private:
    void key(VirtualKey k) {
        const bool up = k == VirtualKey::Up || k == VirtualKey::GamepadDPadUp || k == VirtualKey::GamepadLeftThumbstickUp;
        const bool down = k == VirtualKey::Down || k == VirtualKey::GamepadDPadDown || k == VirtualKey::GamepadLeftThumbstickDown;
        const bool left = k == VirtualKey::Left || k == VirtualKey::GamepadDPadLeft || k == VirtualKey::GamepadLeftThumbstickLeft;
        const bool right = k == VirtualKey::Right || k == VirtualKey::GamepadDPadRight || k == VirtualKey::GamepadLeftThumbstickRight;
        const bool a = k == VirtualKey::Enter || k == VirtualKey::GamepadA;
        if (up) row_ = (row_ + kRows - 1) % kRows;
        if (down) row_ = (row_ + 1) % kRows;
        if (left || right) change(right ? 1 : -1);
        if (a) activate();
    }

    void change(int dir) {
        if (row_ == kMode) {
            {
                std::lock_guard lk(S.m);
                S.mode = lx::Mode((int(S.mode) + 3 + dir) % 3);
            }
            apply_mode();
        } else if (row_ == kRomfs || row_ == kExefs) {
            {
                std::lock_guard lk(S.m);
                if (row_ == kRomfs) {
                    if (S.romfs_folders.empty()) return;
                    S.romfs = (S.romfs + int(S.romfs_folders.size()) + dir) % int(S.romfs_folders.size());
                } else {
                    if (S.exefs_folders.empty()) return;
                    S.exefs = (S.exefs + int(S.exefs_folders.size()) + dir) % int(S.exefs_folders.size());
                }
            }
            save_settings();
        }
    }

    void activate() {
        if (row_ == kMode) change(1);
        if (row_ == kRomfs || row_ == kExefs) {
            std::string wanted_romfs, wanted_exefs;
            {
                std::lock_guard lk(S.m);
                if (S.romfs >= 0 && S.romfs < int(S.romfs_folders.size()))
                    wanted_romfs = narrow(S.romfs_folders[S.romfs].folder.wstring());
                if (S.exefs >= 0 && S.exefs < int(S.exefs_folders.size()))
                    wanted_exefs = narrow(S.exefs_folders[S.exefs].folder.wstring());
            }
            std::thread([wanted_romfs, wanted_exefs] { winrt::init_apartment(); scan_drives(wanted_romfs, wanted_exefs); }).detach();
        }
        if (row_ == kBuild) std::thread(run_or_measure_build).detach();
        if (row_ == kPlay) std::thread(play).detach();
    }

    std::vector<std::pair<std::string, std::string>> rows() {
        std::lock_guard lk(S.m);
        const lx::Profile& p = lx::profile_for(S.mode, S.model);
        std::string console = lx::model_name(S.model);
        if (S.memory_limit) console += ", gives this app " + mb(S.memory_limit);
        std::string mode = S.mode == lx::Mode::automatic ? std::string("Auto (") + p.name + ")" : p.name;
        auto folder_text = [&](const std::vector<lx::DataFolder>& folders, int selected, bool has_size) {
            if (S.scanning) return std::string("Scanning USB storage...");
            if (selected < 0 || selected >= int(folders.size())) return std::string("Not selected  |  A to rescan");
            std::string value = narrow(folders[selected].folder.wstring());
            if (has_size) value += "  |  " + mb(folders[selected].bytes);
            if (folders.size() > 1) value += "  |  " + std::to_string(selected + 1) + "/" + std::to_string(folders.size());
            return value;
        };
        return {{"Console", console},
                {"Mode", "< " + mode + " >   " + p.summary},
                {"ROMFS folder", folder_text(S.romfs_folders, S.romfs, true)},
                {"ExeFS folder", folder_text(S.exefs_folders, S.exefs, false)},
                {"Build", S.busy ? "Building on console..." : "A: compile the game module on this console"},
                {"Play", g_playing ? "Game running" : "A: start the built game"}};
    }

    void init_graphics() {
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION,
                          d3d_.put(), nullptr, nullptr);
        if (!d3d_) return;
        auto dxgi_dev = d3d_.as<IDXGIDevice>();
        com_ptr<IDXGIAdapter> ad;
        dxgi_dev->GetAdapter(ad.put());
        com_ptr<IDXGIFactory2> fac;
        ad->GetParent(__uuidof(IDXGIFactory2), fac.put_void());
        auto b = window_.Bounds();
        float scale = float(winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel());
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = UINT(b.Width * scale);
        sd.Height = UINT(b.Height * scale);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        // (for composition, not the CoreWindow: the game's picture goes on top of it in the same tree, present.h)
        lx::present::init(window_);
        fac->CreateSwapChainForComposition(d3d_.get(), &sd, nullptr, swap_.put());
        if (!swap_) return;
        lx::present::set_launcher(swap_.get());
        D2D1_FACTORY_OPTIONS fo{};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, d2f_.put_void());
        com_ptr<ID2D1Device> d2dev;
        d2f_->CreateDevice(dxgi_dev.get(), d2dev.put());
        d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, ctx_.put());
        com_ptr<IDXGISurface> surf;
        swap_->GetBuffer(0, __uuidof(IDXGISurface), surf.put_void());
        D2D1_BITMAP_PROPERTIES1 bp{{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE}, 96, 96,
                                   D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW};
        ctx_->CreateBitmapFromDxgiSurface(surf.get(), &bp, target_.put());
        ctx_->SetTarget(target_.get());
        load_logo();
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw_.put_void()));
        dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              48.0f, L"en-us", title_.put());
        dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              22.0f, L"en-us", text_.put());
        dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              17.0f, L"en-us", small_.put());
        dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              17.0f, L"en-us", status_text_.put());
        if (status_text_) status_text_->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
        ctx_->CreateSolidColorBrush({0.91f, 0.95f, 0.91f, 1}, fg_.put());
        ctx_->CreateSolidColorBrush({0.59f, 0.68f, 0.61f, 1}, dim_.put());
        ctx_->CreateSolidColorBrush({0.81f, 0.69f, 0.43f, 1}, accent_.put());
        ctx_->CreateSolidColorBrush({0.075f, 0.13f, 0.10f, 1}, panel_.put());
        ctx_->CreateSolidColorBrush({0.10f, 0.18f, 0.14f, 1}, tile_.put());
        ctx_->CreateSolidColorBrush({0.15f, 0.29f, 0.21f, 1}, selected_.put());
        ctx_->CreateSolidColorBrush({0.24f, 0.39f, 0.29f, 1}, edge_.put());
        width_ = float(sd.Width);
        height_ = float(sd.Height);
    }

    void load_logo() {
        wchar_t exe[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (!length || length >= MAX_PATH) return;
        const fs::path path = fs::path(exe).parent_path() / L"Assets" / L"Square150x150Logo.png";
        com_ptr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                    factory.put_void())))
            return;
        com_ptr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                      decoder.put())))
            return;
        com_ptr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.put()))) return;
        com_ptr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(converter.put())) ||
            FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                         0.0, WICBitmapPaletteTypeCustom)))
            return;
        ctx_->CreateBitmapFromWicBitmap(converter.get(), nullptr, logo_.put());
    }

    void text(const std::string& s, IDWriteTextFormat* f, ID2D1Brush* b, float x, float y, float w, float h) {
        std::wstring ws = widen(s);
        ctx_->DrawText(ws.c_str(), UINT32(ws.size()), f, D2D1_RECT_F{x, y, x + w, y + h}, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void draw() {
        if (!ctx_ || lx::present::game_shown()) { Sleep(16); return; }   // (under the game's picture: nothing to draw)
        const float width_scale = width_ / 1920.0f, height_scale = height_ / 1080.0f;
        const float scale = width_scale < height_scale ? width_scale : height_scale;
        const float margin = 64.0f * scale;
        const float left_x = margin, left_w = width_ * 0.60f;
        const float right_x = left_x + left_w + 24.0f * scale;
        const float right_w = width_ - margin - right_x;
        const float content_y = 184.0f * scale, row_h = 78.0f * scale, row_gap = 11.0f * scale;
        const float radius = 12.0f * scale;
        auto r = rows();
        std::vector<std::string> notes;
        {
            std::lock_guard lk(S.m);
            notes = S.notes;
        }
        ctx_->BeginDraw();
        ctx_->Clear({0.035f, 0.055f, 0.043f, 1});
        if (logo_) {
            ctx_->DrawBitmap(logo_.get(),
                             D2D1_RECT_F{margin - 4 * scale, 34 * scale, margin + 76 * scale, 114 * scale}, 1.0f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            ctx_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1_RECT_F{margin - 10 * scale, 48 * scale, margin + 5 * scale,
                                                                      124 * scale}, 8 * scale, 8 * scale},
                                       accent_.get());
        }
        text("LONGSHOT", title_.get(), fg_.get(), margin + 88 * scale, 42 * scale, 760 * scale, 62 * scale);
        text("XBOX EDITION  /  YOUR GAME FILES STAY ON YOUR STORAGE", small_.get(), dim_.get(), margin + 90 * scale,
             112 * scale, 1000 * scale, 30 * scale);
        text("GAME SETUP", small_.get(), accent_.get(), left_x, 154 * scale, left_w, 24 * scale);
        float y = content_y;
        for (int i = 0; i < kRows; ++i, y += row_h) {
            const auto background = i == row_ ? selected_.get() : tile_.get();
            ctx_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1_RECT_F{left_x, y, left_x + left_w, y + row_h}, radius, radius}, background);
            ctx_->DrawRoundedRectangle(D2D1_ROUNDED_RECT{D2D1_RECT_F{left_x, y, left_x + left_w, y + row_h}, radius, radius},
                                       edge_.get(), 1.0f * scale);
            if (i == row_)
                ctx_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1_RECT_F{left_x + 9 * scale, y + 15 * scale,
                                                                           left_x + 14 * scale, y + row_h - 15 * scale},
                                                              3 * scale, 3 * scale},
                                           accent_.get());
            text(r[i].first, text_.get(), i == row_ ? accent_.get() : dim_.get(), left_x + 26 * scale,
                 y + 7 * scale, 205 * scale, row_h - 14 * scale);
            text(r[i].second, small_.get(), i == row_ ? fg_.get() : dim_.get(), left_x + 235 * scale,
                 y + 6 * scale, left_w - 255 * scale, row_h - 12 * scale);
        }
        const float row_end = y - row_gap;
        ctx_->FillRoundedRectangle(
            D2D1_ROUNDED_RECT{D2D1_RECT_F{right_x, content_y, right_x + right_w, row_end}, radius, radius}, panel_.get());
        ctx_->DrawRoundedRectangle(
            D2D1_ROUNDED_RECT{D2D1_RECT_F{right_x, content_y, right_x + right_w, row_end}, radius, radius}, edge_.get(), 1.0f * scale);
        text("QUICK GUIDE", small_.get(), accent_.get(), right_x + 26 * scale, content_y + 24 * scale, right_w - 52 * scale,
             26 * scale);
        std::string detail_title = "Ready when you are";
        std::string detail_body = "Choose your game folders, build the module on the console, then launch it from the selected USB storage.";
        switch (row_) {
        case kConsole:
            detail_title = "Console profile";
            detail_body = "The app detects the console and applies a conservative graphics profile. Auto is recommended.";
            break;
        case kMode:
            detail_title = "Graphics profile";
            detail_body = "Use left or right to switch between Auto, Series S and Series X settings.";
            break;
        case kRomfs:
            detail_title = "Select ROMFS";
            detail_body = "Use left or right to choose a detected romfs folder. Press A to rescan removable storage.";
            break;
        case kExefs:
            detail_title = "Select ExeFS";
            detail_body = "Choose the matching exefs folder containing main. It can be on a different connected USB drive.";
            break;
        case kBuild:
            detail_title = "Build on console";
            detail_body = "Build reads the selected folders and keeps generated files on the ROMFS drive. Builds may take hours.";
            break;
        case kPlay:
            detail_title = "Start the game";
            detail_body = "Play loads Longshot's module from Longshot/game on the selected ROMFS drive.";
            break;
        }
        text(detail_title, text_.get(), fg_.get(), right_x + 26 * scale, content_y + 68 * scale, right_w - 52 * scale,
             38 * scale);
        text(detail_body, small_.get(), dim_.get(), right_x + 26 * scale, content_y + 122 * scale, right_w - 52 * scale,
             row_end - content_y - 140 * scale);
        const float status_y = 812.0f * scale, status_h = 136.0f * scale;
        ctx_->FillRoundedRectangle(
            D2D1_ROUNDED_RECT{D2D1_RECT_F{margin, status_y, width_ - margin, status_y + status_h}, radius, radius}, panel_.get());
        text("ACTIVITY", small_.get(), accent_.get(), margin + 24 * scale, status_y + 16 * scale, 190 * scale, 24 * scale);
        const size_t first = notes.size() > 2 ? notes.size() - 2 : 0;
        std::string activity;
        for (size_t i = first; i < notes.size(); ++i) {
            if (!activity.empty()) activity += '\n';
            activity += notes[i];
        }
        text(activity, status_text_ ? status_text_.get() : small_.get(), fg_.get(), margin + 24 * scale,
             status_y + 45 * scale, width_ - 2 * margin - 48 * scale, status_h - 48 * scale);
        text("D-pad  Navigate     Left / Right  Change     A  Select / rescan", small_.get(), dim_.get(), margin,
             height_ - 38 * scale, width_ - 2 * margin, 24 * scale);
        ctx_->EndDraw();
        swap_->Present(1, 0);
    }

    CoreWindow window_{nullptr};
    com_ptr<ID3D11Device> d3d_;
    com_ptr<IDXGISwapChain1> swap_;
    com_ptr<ID2D1Factory1> d2f_;
    com_ptr<ID2D1DeviceContext> ctx_;
    com_ptr<ID2D1Bitmap1> target_;
    com_ptr<ID2D1Bitmap> logo_;
    com_ptr<IDWriteFactory> dw_;
    com_ptr<IDWriteTextFormat> title_, text_, small_, status_text_;
    com_ptr<ID2D1SolidColorBrush> fg_, dim_, accent_, panel_, tile_, selected_, edge_;
    float width_ = 1920, height_ = 1080;
    int row_ = kRomfs;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment();   // (CoreApplication refuses a main thread that is not in the multi-threaded apartment)
    CoreApplication::Run(make<App>());
    return 0;
}
