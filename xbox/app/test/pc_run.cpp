// The console's chain on a PC, without the console: the game module built for the Xbox (LONGSHOT_XBOX=1) loaded by hand
// through the app's module set, as the app's Play does, its renderer (the package's longshot_vk.dll, LONGSHOT_XBOX=1
// RX_VK_ONLY=1) and Mesa's Dozen loaded by hand by it, Dozen's swap chain handed over through the app's DComp.DLL. What it
// leaves out is the app's WinRT side (CoreWindow, composition, XAudio2): no window shows, no sound plays, and the
// renderer's pictures (RX_VK_SHOTS) say what was drawn. An ordinary desktop process, so the game's switches come from
// the environment as in any PC run.
//
//     pc_run.exe <package folder> <longshot_game.dll> <seconds> <user folder> <romfs folder> [image.bin]
//
// The package folder holds longshot_vk.dll, vulkan_dzn.dll, dxil.dll, longshot_tls.dll, libc++.dll and libunwind.dll
// (xbox/app/test/pc_run.ps1 gathers them). Ends the game after <seconds> with exit status 124.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../../host/uwp/uwp_platform.h"
#include "../dcomp_shim.h"
#include "../modules.h"

namespace mod = longshot_xbox::modules;
namespace dcomp = longshot_xbox::dcomp;

namespace {

int g_width = 1280, g_height = 720;
std::atomic<int> g_shows{0};
std::string g_local, g_game, g_work;
LongshotUwpPlatform g_table{};

std::wstring widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string parent(const std::string& p) {
    const size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

void line(const char* what, const char* text) {
    std::printf("[pc_run] %s%s\n", what, text ? text : "");
    std::fflush(stdout);
}

void show(IUnknown* swapchain) {
    ++g_shows;
    line(swapchain ? "DComp: the game's swap chain is shown" : "DComp: the game's swap chain was taken away", nullptr);
}

void window_size(int* w, int* h) { *w = g_width, *h = g_height; }
int refresh_mhz() { return 60000; }
int poll_event(LongshotUwpEvent*) { return 0; }
volatile uint8_t g_keys[256];
int clipboard_get(char*, size_t) { return 0; }
void clipboard_set(const char*) {}
void open_uri(const char* u) { line("link: ", u); }
void message(const char* title, const char* text) {
    std::printf("[pc_run] message: %s: %s\n", title ? title : "", text ? text : "");
    std::fflush(stdout);
}
void log_line(const char* text) { line("game: ", text); }

struct Start {
    int (*main)(int, char**);
    std::vector<std::string> args;
};

DWORD WINAPI game_thread(void* p) {
    auto* s = static_cast<Start*>(p);
    std::vector<char*> argv;
    for (auto& a : s->args) argv.push_back(a.data());
    argv.push_back(nullptr);
    const int code = s->main(int(s->args.size()), argv.data());
    std::printf("[pc_run] the game ended (%d)\n", code);
    std::fflush(stdout);
    ExitProcess(UINT(code));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: pc_run <package folder> <longshot_game.dll> <seconds> <user folder> <romfs folder> [image.bin]\n");
        return 2;
    }
    if (const char* s = std::getenv("PC_RUN_SIZE")) std::sscanf(s, "%dx%d", &g_width, &g_height);
    const std::string module = argv[2];
    const int seconds = std::atoi(argv[3]);
    g_local = argv[4];
    g_game = parent(argv[5]);
    g_work = parent(parent(module));

    mod::Config cfg;
    cfg.folder = widen(argv[1]);
    cfg.local_state = widen(argv[4]);
    cfg.log = [](const std::string& s) { line("", s.c_str()); };
    mod::configure(cfg);
    mod::add_virtual("dcomp.dll", dcomp::lookup);
    dcomp::set_show(show);

    char err[512];
    const DWORD t0 = GetTickCount();
    pel_module* m = mod::load(widen(module.c_str()), err, sizeof err);
    if (!m) {
        line("the game module did not load: ", err);
        return 1;
    }
    std::printf("[pc_run] the game module loaded: %zu MB at %p in %lu ms\n", pel_size(m) >> 20, pel_base(m), GetTickCount() - t0);
    if (auto win32 = reinterpret_cast<mod::Lookup>(pel_symbol(m, "longshot_uwp_win32"))) mod::set_win32(win32);
    auto set_platform = reinterpret_cast<int (*)(const LongshotUwpPlatform*)>(pel_symbol(m, "longshot_uwp_set_platform"));
    auto game_main = reinterpret_cast<int (*)(int, char**)>(pel_symbol(m, "longshot_uwp_main"));
    if (!set_platform || !game_main) {
        line("not a game module built for the Xbox (no longshot_uwp_main)", nullptr);
        return 1;
    }

    g_table.version = LONGSHOT_UWP_PLATFORM_VERSION;
    g_table.window_size = window_size;
    g_table.refresh_mhz = refresh_mhz;
    g_table.create_vk_surface = dcomp::create_vk_surface;
    g_table.poll_event = poll_event;
    g_table.keys = g_keys;
    g_table.clipboard_get = clipboard_get;
    g_table.clipboard_set = clipboard_set;
    g_table.open_uri = open_uri;
    g_table.message = message;
    g_table.log = log_line;
    g_table.local_folder = g_local.c_str();
    g_table.game_folder = g_game.c_str();
    g_table.work_folder = g_work.c_str();
    if (!set_platform(&g_table)) {
        line("the module wants another platform table version", nullptr);
        return 1;
    }

    auto* start = new Start{game_main, {"longshot"}};
    if (argc > 6) start->args.push_back(argv[6]);
    start->args.insert(start->args.end(), {"--romfs", argv[5], "--user", g_local});
    HANDLE t = CreateThread(nullptr, 64u << 20, game_thread, start, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return 1;
    if (WaitForSingleObject(t, DWORD(seconds) * 1000) == WAIT_TIMEOUT) {
        std::printf("[pc_run] %d s: ending the game (swap chains shown: %d)\n", seconds, g_shows.load());
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 124);
    }
    return 0;
}
