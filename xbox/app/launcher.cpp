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

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

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
    lx::Mode mode = lx::Mode::automatic;
    std::vector<fs::path> drives;
    std::vector<lx::GameFolder> games;
    int game = 0;
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
    for (const auto& game : S.games)
        if (game.folder == p && !game.drive_root.empty()) return game.drive_root;
    return p.root_path();
}

void save_settings() {
    std::string game;
    {
        std::lock_guard lk(S.m);
        if (S.game < int(S.games.size())) game = narrow(S.games[S.game].folder.wstring());
        write_file(local_folder / "launcher.ini", std::string("mode=") + lx::mode_key(S.mode) + "\ngame=" + game + "\n");
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

void load_settings(std::string& game) {
    std::istringstream in(read_file(local_folder / "launcher.ini"));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("mode=", 0) == 0) S.mode = lx::mode_from_key(line.substr(5));
        if (line.rfind("game=", 0) == 0) game = line.substr(5);
    }
}

lx::Model read_model() {
    GAMING_DEVICE_MODEL_INFORMATION info{};
    if (FAILED(GetGamingDeviceModelInformation(&info))) return lx::Model::unknown;
    return lx::model_from_device_id(info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT, uint32_t(info.deviceId));
}

void scan_drives(const std::string& wanted) {
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
    auto games = lx::find_game_folders(drives);
    {
        std::lock_guard lk(S.m);
        S.drives = drives;
        S.games = games;
        S.game = 0;
        for (size_t i = 0; i < games.size(); ++i)
            if (narrow(games[i].folder.wstring()) == wanted) S.game = int(i);
        S.scanning = false;
    }
    S.note(games.empty() ? "No game files found on the detected storage roots. Put romfs and exefs on an accessible USB drive, then retry Game files."
                         : "Found " + std::to_string(games.size()) + " game folder(s).");
}

void run_or_measure_build() {
    lx::Mode mode;
    lx::Model model;
    fs::path game_folder;
    {
        std::lock_guard lk(S.m);
        if (S.busy || g_playing) return;
        if (S.game < int(S.games.size())) game_folder = S.games[S.game].folder;
        mode = S.mode, model = S.model;
        S.busy = true;
    }
    auto finish = [] {
        std::lock_guard lk(S.m);
        S.busy = false;
    };
    const lx::Profile& p = lx::profile_for(mode, model);

    // Look for tools in package, USB drive, or PC tools folder
    std::wstring pkg_dir = winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str();
    fs::path tools_pkg = fs::path(pkg_dir) / "tools";
    fs::path drive_tools = game_folder.empty() ? fs::path() : (drive_of(game_folder) / "Longshot" / "tools");

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
    if (!game_folder.empty() && !py_exe.empty() && fs::exists(py_exe, ec) && fs::exists(script, ec)) {
        S.note("Starting build on console: " + narrow(game_folder.wstring()) + " in " + p.name + " mode (" + std::to_string(p.compile_jobs) + " job(s))...");
        fs::path drive = drive_of(game_folder);
        fs::path out = drive / "Longshot";
        fs::path work = lx::work_folder(drive);
        fs::path romfs = game_folder / "romfs";
        fs::path exefs = game_folder / "exefs";

        SetEnvironmentVariableW(L"LONGSHOT_XBOX", L"1");
        SetEnvironmentVariableW(L"LONGSHOT_MINGW", mingw_dir.c_str());
        // The Series S probe measured a 5,120 MB app limit and a 4,864 MB commit ceiling. Compile one
        // translation unit at a time and leave headroom for Python, the app, and the linker.
        SetEnvironmentVariableW(L"LONGSHOT_MEM_LIMIT_GB", L"3.0");
        SetEnvironmentVariableW(L"LONGSHOT_MEM_RESERVE_GB", L"1.5");

        std::wstring cmd = L"\"" + py_exe.wstring() + L"\" -I -u \"" + script.wstring() +
                           L"\" --romfs \"" + romfs.wstring() + L"\" --exefs \"" + exefs.wstring() +
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

        std::string line_buf;
        char buf[256];
        DWORD read_bytes = 0;
        while (ReadFile(r_pipe, buf, sizeof buf, &read_bytes, nullptr) && read_bytes > 0) {
            for (DWORD i = 0; i < read_bytes; ++i) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    if (!line_buf.empty()) {
                        if (line_buf.rfind("@@stage", 0) == 0 || line_buf.rfind("@@plan", 0) == 0 ||
                            line_buf.rfind("@@error", 0) == 0 || line_buf.rfind("@@done", 0) == 0) {
                            S.note(line_buf);
                        } else if (line_buf.rfind("@@file", 0) == 0) {
                            S.note("  " + line_buf.substr(7));
                        }
                        line_buf.clear();
                    }
                } else {
                    line_buf.push_back(buf[i]);
                }
            }
        }
        CloseHandle(r_pipe);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (exit_code == 0) {
            S.note("Build complete! longshot_game.dll ready. Press A on Play to start.");
        } else {
            S.note("Build finished with exit code " + std::to_string(exit_code) + ".");
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
    if (game_folder.empty()) {
        S.note("No game files selected. Place your romfs and exefs on a USB drive.");
    } else {
        S.note("Build tools (python, clang/lld) not found in package or drive. You can also build on PC and copy longshot_game.dll to the USB drive.");
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
    fs::path module, game;
    lx::Model model;
    uint64_t memory_limit;
    {
        std::lock_guard lk(S.m);
        if (S.busy || g_playing) return;
        if (S.game >= int(S.games.size())) {
            S.notes.push_back("Choose your game files first.");
            return;
        }
        game = S.games[S.game].folder;
        module = lx::module_path(drive_of(game));
        model = S.model, memory_limit = S.memory_limit;
        S.busy = true;
    }
    auto done = [] {
        std::lock_guard lk(S.m);
        S.busy = false;
    };
    std::error_code ec;
    if (!fs::exists(module, ec)) {
        S.note("No game module yet at " + narrow(module.wstring()) + ": build the game first.");
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
    for (const auto& [name, value] : lx::profile_for(mode, model).env)
        SetEnvironmentVariableW(widen(name).c_str(), widen(value).c_str());
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
    const fs::path work = lx::work_folder(drive_of(game));
    lx::PlatformPaths paths{narrow(local_folder.wstring()), narrow(game.wstring()), narrow(work.wstring())};
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
    const fs::path image = module.parent_path() / "image.bin";
    if (fs::exists(image, ec)) start->args.push_back(narrow(image.wstring()));
    start->args.insert(start->args.end(), {"--romfs", narrow((game / "romfs").wstring()), "--user", narrow(local_folder.wstring())});
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

enum Row { kConsole, kMode, kGame, kBuild, kPlay, kRows };

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
        std::string wanted;
        load_settings(wanted);
        S.model = read_model();
        try { S.memory_limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit(); } catch (...) {}
        apply_mode();
        std::thread([wanted] { winrt::init_apartment(); scan_drives(wanted); }).detach();
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
        } else if (row_ == kGame) {
            {
                std::lock_guard lk(S.m);
                if (S.games.empty()) return;
                S.game = (S.game + int(S.games.size()) + dir) % int(S.games.size());
            }
            save_settings();
        }
    }

    void activate() {
        if (row_ == kMode) change(1);
        if (row_ == kGame) {
            std::string wanted;
            {
                std::lock_guard lk(S.m);
                if (S.game < int(S.games.size())) wanted = narrow(S.games[S.game].folder.wstring());
            }
            std::thread([wanted] { winrt::init_apartment(); scan_drives(wanted); }).detach();
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
        std::string game = S.scanning ? "looking..." : S.games.empty() ? "none found (A: look again)"
                                                                         : narrow(S.games[S.game].folder.wstring()) + "  (" +
                                                                               mb(S.games[S.game].romfs_bytes) + " of romfs)";
        if (S.games.size() > 1) game += "  " + std::to_string(S.game + 1) + "/" + std::to_string(S.games.size());
        return {{"Console", console},
                {"Mode", "< " + mode + " >   " + p.summary},
                {"Game files", game},
                {"Build", S.busy ? "working..." : "A: measure this console and estimate the build"},
                {"Play", g_playing ? "running" : "A: start the built game"}};
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
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw_.put_void()));
        dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              44.0f, L"en-us", title_.put());
        dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              24.0f, L"en-us", text_.put());
        dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              19.0f, L"en-us", small_.put());
        ctx_->CreateSolidColorBrush({0.92f, 0.95f, 0.92f, 1}, fg_.put());
        ctx_->CreateSolidColorBrush({0.55f, 0.62f, 0.58f, 1}, dim_.put());
        ctx_->CreateSolidColorBrush({0.16f, 0.42f, 0.26f, 1}, sel_.put());
        width_ = float(sd.Width);
        height_ = float(sd.Height);
    }

    void text(const std::string& s, IDWriteTextFormat* f, ID2D1Brush* b, float x, float y, float w, float h) {
        std::wstring ws = widen(s);
        ctx_->DrawText(ws.c_str(), UINT32(ws.size()), f, D2D1_RECT_F{x, y, x + w, y + h}, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void draw() {
        if (!ctx_ || lx::present::game_shown()) { Sleep(16); return; }   // (under the game's picture: nothing to draw)
        const float mx = width_ * 0.06f, my = height_ * 0.07f, row_h = 64, label_w = 220;
        auto r = rows();
        std::vector<std::string> notes;
        {
            std::lock_guard lk(S.m);
            notes = S.notes;
        }
        ctx_->BeginDraw();
        ctx_->Clear({0.06f, 0.08f, 0.07f, 1});
        text("Longshot", title_.get(), fg_.get(), mx, my, 600, 60);
        text("Ocarina of Time, built from your own game files on this console", small_.get(), dim_.get(), mx, my + 60, width_ - 2 * mx, 30);
        float y = my + 120;
        for (int i = 0; i < kRows; ++i, y += row_h) {
            if (i == row_) ctx_->FillRectangle(D2D1_RECT_F{mx - 16, y - 8, width_ - mx + 16, y + row_h - 12}, sel_.get());
            text(r[i].first, text_.get(), fg_.get(), mx, y, label_w, row_h);
            text(r[i].second, text_.get(), i == row_ ? fg_.get() : dim_.get(), mx + label_w, y, width_ - 2 * mx - label_w, row_h);
        }
        y += 20;
        for (const std::string& n : notes) {
            text(n, small_.get(), fg_.get(), mx, y, width_ - 2 * mx, 28);
            y += 28;
        }
        text("D-pad: choose   Left / Right: change   A: select", small_.get(), dim_.get(), mx, height_ - my, width_ - 2 * mx, 30);
        ctx_->EndDraw();
        swap_->Present(1, 0);
    }

    CoreWindow window_{nullptr};
    com_ptr<ID3D11Device> d3d_;
    com_ptr<IDXGISwapChain1> swap_;
    com_ptr<ID2D1Factory1> d2f_;
    com_ptr<ID2D1DeviceContext> ctx_;
    com_ptr<ID2D1Bitmap1> target_;
    com_ptr<IDWriteFactory> dw_;
    com_ptr<IDWriteTextFormat> title_, text_, small_;
    com_ptr<ID2D1SolidColorBrush> fg_, dim_, sel_;
    float width_ = 1920, height_ = 1080;
    int row_ = kMode;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment();   // (CoreApplication refuses a main thread that is not in the multi-threaded apartment)
    CoreApplication::Run(make<App>());
    return 0;
}
