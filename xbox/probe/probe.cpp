// Longshot Xbox probe: what an Xbox Series S in Developer Mode gives a UWP game, measured on the console.
// Every answer goes on the screen and into LocalState\probe.txt and probe.json (written after each section, so a crash
// or a kill by the memory limit still leaves what came before). Nothing of the game is in here.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_3.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <xaudio2.h>
#include <dxcapi.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.Graphics.Display.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bench.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;
namespace wgi = winrt::Windows::Gaming::Input;

namespace {

constexpr uint64_t kGuestBase = 0x100000000000ull;   // A64_HOST_BASE in third_party/a64-kit/rt/a64.h
constexpr uint64_t kGuestSize = 1ull << 39;          // the 512 GB window host/hos_main.c reserves
constexpr uint64_t MB = 1ull << 20;

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string hr_text(HRESULT hr) { return fmt("0x%08X", (unsigned)hr); }

// ---- the report ------------------------------------------------------------------------------------------------

struct Entry { std::string section, key, value; };

class Report {
public:
    void set_folder(std::wstring folder) { folder_ = std::move(folder); }
    void section(const std::string& name) {
        std::lock_guard<std::mutex> l(m_);
        section_ = name;
        lines_.push_back("");
        lines_.push_back("== " + name);
    }
    void add(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> l(m_);
        entries_.push_back({section_, key, value});
        lines_.push_back("  " + key + ": " + value);
    }
    void note(const std::string& text) {
        std::lock_guard<std::mutex> l(m_);
        lines_.push_back("  " + text);
    }
    std::vector<std::string> lines() {
        std::lock_guard<std::mutex> l(m_);
        return lines_;
    }
    void flush() {
        std::lock_guard<std::mutex> l(m_);
        if (folder_.empty()) return;
        write(folder_ + L"\\probe.txt", text());
        write(folder_ + L"\\probe.json", json());
    }
    // (on the way down: the lock may be held by the thread that failed, so it is only tried)
    void stopped(const std::string& why) {
        std::unique_lock<std::mutex> l(m_, std::try_to_lock);
        lines_.push_back("  STOPPED in \"" + section_ + "\": " + why);
        if (!folder_.empty()) write(folder_ + L"\\probe.txt", text());
    }

private:
    std::string text() const {
        std::string s;
        for (auto& l : lines_) s += l + "\n";
        return s;
    }
    static std::string esc(const std::string& v) {
        std::string o;
        for (char c : v) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if ((unsigned char)c < 0x20) o += fmt("\\u%04x", c);
            else o += c;
        }
        return o;
    }
    std::string json() const {
        std::string s = "[\n";
        for (size_t i = 0; i < entries_.size(); ++i)
            s += fmt("  {\"section\": \"%s\", \"key\": \"%s\", \"value\": \"%s\"}%s\n", esc(entries_[i].section).c_str(),
                     esc(entries_[i].key).c_str(), esc(entries_[i].value).c_str(), i + 1 < entries_.size() ? "," : "");
        return s + "]\n";
    }
    static void write(const std::wstring& path, const std::string& data) {
        HANDLE h = CreateFile2(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD n = 0;
        WriteFile(h, data.data(), (DWORD)data.size(), &n, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);
    }

    std::mutex m_;
    std::wstring folder_;
    std::string section_;
    std::vector<Entry> entries_;
    std::vector<std::string> lines_;
};

Report R;

// ---- looking functions up at run time (so a missing one is an answer, not a failed start) -----------------------

FARPROC sym(const wchar_t* dll, const char* name) {
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? GetProcAddress(m, name) : nullptr;
}

template <class F>
F kfn(const char* name) {
    FARPROC p = sym(L"kernel32.dll", name);
    if (!p) p = sym(L"kernelbase.dll", name);
    return reinterpret_cast<F>(p);
}

using VirtualAlloc_t = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
using VirtualFree_t = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
using VirtualProtect_t = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
using VirtualAllocFromApp_t = PVOID(WINAPI*)(PVOID, SIZE_T, ULONG, ULONG);
using VirtualAlloc2FromApp_t = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using AddVEH_t = PVOID(WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
using RemoveVEH_t = ULONG(WINAPI*)(PVOID);
using GetWriteWatch_t = UINT(WINAPI*)(DWORD, PVOID, SIZE_T, PVOID*, ULONG_PTR*, LPDWORD);

VirtualAlloc_t pVirtualAlloc;
VirtualFree_t pVirtualFree;
VirtualProtect_t pVirtualProtect;

// ---- 1. the system ---------------------------------------------------------------------------------------------

void probe_system() {
    R.section("system");
    try {
        auto v = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo();
        R.add("device family", narrow(v.DeviceFamily()));
        uint64_t ver = std::stoull(narrow(v.DeviceFamilyVersion()));
        R.add("version", fmt("%llu.%llu.%llu.%llu", ver >> 48, (ver >> 32) & 0xFFFF, (ver >> 16) & 0xFFFF, ver & 0xFFFF));
    } catch (...) {
        R.add("device family", "unknown");
    }
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    R.add("processors (GetNativeSystemInfo)", fmt("%u", si.dwNumberOfProcessors));
    R.add("page size / allocation granularity", fmt("%u / %u", si.dwPageSize, si.dwAllocationGranularity));
    R.add("address range", fmt("%p - %p", si.lpMinimumApplicationAddress, si.lpMaximumApplicationAddress));
    if (auto f = kfn<DWORD(WINAPI*)(WORD)>("GetActiveProcessorCount")) R.add("GetActiveProcessorCount", fmt("%u", f(ALL_PROCESSOR_GROUPS)));
    if (auto f = kfn<BOOL(WINAPI*)(HANDLE, PDWORD_PTR, PDWORD_PTR)>("GetProcessAffinityMask")) {
        DWORD_PTR p = 0, s = 0;
        BOOL ok = f(GetCurrentProcess(), &p, &s);
        R.add("process / system affinity mask", ok ? fmt("0x%llx / 0x%llx", (unsigned long long)p, (unsigned long long)s) : "call failed");
    }
    int regs[4]{};
    __cpuidex(regs, 7, 0);
    R.add("AVX2", (regs[1] & (1 << 5)) ? "yes" : "no");
    char brand[49]{};
    for (int i = 0; i < 3; ++i) __cpuid(reinterpret_cast<int*>(brand + 16 * i), 0x80000002 + i);
    R.add("CPU", brand);
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    R.add("QPC frequency", fmt("%lld", f.QuadPart));
    R.flush();
}

// ---- 2. every Windows function Longshot imports ----------------------------------------------------------------

struct Import { const char* dll; const char* name; };
const Import kImports[] = {
#include "imports.inc"
};

void probe_imports() {
    R.section("imports (a built Longshot's Windows functions)");
    int have = 0, total = 0;
    std::string missing_dlls;
    std::vector<std::string> missing;
    const char* last_dll = "";
    bool dll_ok = false;
    for (auto& i : kImports) {
        ++total;
        if (strcmp(i.dll, last_dll) != 0) {
            last_dll = i.dll;
            std::wstring w(i.dll, i.dll + strlen(i.dll));
            HMODULE m = GetModuleHandleW(w.c_str());
            if (!m) m = LoadLibraryExW(w.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            dll_ok = m != nullptr;
            if (!dll_ok) missing_dlls += std::string(missing_dlls.empty() ? "" : ", ") + i.dll;
        }
        std::wstring w(i.dll, i.dll + strlen(i.dll));
        FARPROC p = dll_ok ? sym(w.c_str(), i.name) : nullptr;
        if (!p && !_stricmp(i.dll, "kernel32.dll")) p = sym(L"kernelbase.dll", i.name);
        if (p) ++have;
        else missing.push_back(std::string(i.dll) + "!" + i.name);
    }
    R.add("present", fmt("%d of %d", have, total));
    R.add("libraries that do not load", missing_dlls.empty() ? "none" : missing_dlls);
    std::string all;
    for (auto& m : missing) all += (all.empty() ? "" : " ") + m;
    R.add("missing", all.empty() ? "none" : all);
    const wchar_t* extra[] = {L"xinput1_4.dll", L"imm32.dll", L"vulkan-1.dll", L"xaudio2_9.dll", L"winmm.dll",
                              L"user32.dll", L"dbghelp.dll", L"winhttp.dll", L"mfplat.dll", L"d3d12.dll", L"dxgi.dll"};
    for (auto e : extra) {
        HMODULE m = LoadLibraryExW(e, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        R.add(narrow(e), m ? "loads" : fmt("does not load (%lu)", GetLastError()));
    }
    R.flush();
}

// ---- 3. storage --------------------------------------------------------------------------------------------------

void list_dir(const std::wstring& dir, const char* label) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        R.add(label, fmt("cannot list (error %lu)", GetLastError()));
        return;
    }
    std::string names;
    int n = 0;
    do {
        if (n < 12) names += (names.empty() ? "" : ", ") + narrow(fd.cFileName);
        ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    R.add(label, fmt("%d entries: %s", n, names.c_str()));
}

void probe_storage() {
    R.section("storage");
    try {
        auto local = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
        R.add("LocalState", narrow(local));
        std::wstring test = std::wstring(local) + L"\\write-test.bin";
        HANDLE h = CreateFile2(test.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        R.add("write in LocalState", h != INVALID_HANDLE_VALUE ? "yes" : fmt("no (%lu)", GetLastError()));
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); DeleteFileW(test.c_str()); }
        ULARGE_INTEGER freeb{}, total{};
        if (GetDiskFreeSpaceExW(local.c_str(), &freeb, &total, nullptr))
            R.add("LocalState drive free / total", fmt("%.1f GB / %.1f GB", freeb.QuadPart / 1e9, total.QuadPart / 1e9));
        R.add("install folder", narrow(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path()));
    } catch (winrt::hresult_error const& e) {
        R.add("ApplicationData", "error " + narrow(e.message()));
    }
    list_dir(L"E:\\", "E:\\ (Win32)");
    list_dir(L"D:\\", "D:\\ (Win32)");
    list_dir(L"D:\\Drives", "D:\\Drives (Win32)");
    try {
        auto folders = winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync().get();
        std::string names;
        for (auto const& f : folders) names += (names.empty() ? "" : ", ") + narrow(f.Path()) + " " + narrow(f.DisplayName());
        R.add("removable devices (WinRT)", names.empty() ? "none" : names);
    } catch (winrt::hresult_error const& e) {
        R.add("removable devices (WinRT)", "error " + hr_text(e.code()) + " " + narrow(e.message()));
    }
    R.flush();
}

// ---- 4. audio and input -------------------------------------------------------------------------------------------

void probe_audio_input() {
    R.section("audio");
    using XAudio2Create_t = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
    auto create = reinterpret_cast<XAudio2Create_t>(sym(L"xaudio2_9.dll", "XAudio2Create"));
    if (!create) {
        R.add("XAudio2Create", "not found");
    } else {
        com_ptr<IXAudio2> xa;
        HRESULT hr = create(xa.put(), 0, XAUDIO2_DEFAULT_PROCESSOR);
        R.add("XAudio2Create", hr_text(hr));
        if (SUCCEEDED(hr)) {
            IXAudio2MasteringVoice* mv = nullptr;
            hr = xa->CreateMasteringVoice(&mv);
            R.add("mastering voice", hr_text(hr));
            if (mv) {
                XAUDIO2_VOICE_DETAILS d{};
                mv->GetVoiceDetails(&d);
                R.add("output channels / rate", fmt("%u / %u Hz", d.InputChannels, d.InputSampleRate));
                mv->DestroyVoice();
            }
        }
    }
    R.section("input");
    try {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // (pads are announced shortly after start)
        R.add("Gamepad count", fmt("%u", wgi::Gamepad::Gamepads().Size()));
        R.add("RawGameController count", fmt("%u", wgi::RawGameController::RawGameControllers().Size()));
    } catch (winrt::hresult_error const& e) {
        R.add("Windows.Gaming.Input", "error " + narrow(e.message()));
    }
    R.flush();
}

// ---- 5. threads, fibers, timers ------------------------------------------------------------------------------------

void* g_main_fiber;
std::atomic<int> g_fiber_runs;

void WINAPI fiber_proc(void*) {
    for (;;) {
        g_fiber_runs++;
        SwitchToFiber(g_main_fiber);
    }
}

void probe_threads() {
    R.section("threads");
    std::atomic<int> ran{0};
    std::vector<HANDLE> hs;
    for (int i = 0; i < 16; ++i) {
        HANDLE h = CreateThread(nullptr, 64 * MB, [](void* p) -> DWORD { ++*static_cast<std::atomic<int>*>(p); return 0; },
                                &ran, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (h) hs.push_back(h);
    }
    for (HANDLE h : hs) { WaitForSingleObject(h, 5000); CloseHandle(h); }
    R.add("16 threads with 64 MB reserved stacks", fmt("%d created, %d ran", (int)hs.size(), ran.load()));

    if (auto f = kfn<DWORD_PTR(WINAPI*)(HANDLE, DWORD_PTR)>("SetThreadAffinityMask")) {
        DWORD_PTR old = f(GetCurrentThread(), 1);
        R.add("SetThreadAffinityMask(1)", old ? fmt("ok (was 0x%llx)", (unsigned long long)old) : fmt("failed (%lu)", GetLastError()));
        if (old) f(GetCurrentThread(), old);
    } else {
        R.add("SetThreadAffinityMask", "not found");
    }
    BOOL pr = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    R.add("SetThreadPriority(HIGHEST)", pr ? "ok" : fmt("failed (%lu)", GetLastError()));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    auto conv = kfn<LPVOID(WINAPI*)(LPVOID, DWORD)>("ConvertThreadToFiberEx");
    auto make = kfn<LPVOID(WINAPI*)(SIZE_T, SIZE_T, DWORD, LPFIBER_START_ROUTINE, LPVOID)>("CreateFiberEx");
    if (conv && make) {
        std::thread t([&] {
            g_main_fiber = conv(nullptr, 0);
            void* fb = g_main_fiber ? make(64 * 1024, 64 * MB, 0, fiber_proc, nullptr) : nullptr;
            if (fb) {
                for (int i = 0; i < 1000; ++i) SwitchToFiber(fb);
                DeleteFiber(fb);
            }
            R.add("fibers (1000 switches)", fb ? fmt("ok, %d runs", g_fiber_runs.load()) : fmt("failed (%lu)", GetLastError()));
        });
        t.join();
    } else {
        R.add("fibers", "ConvertThreadToFiberEx/CreateFiberEx not found");
    }

    using CreateWT_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
    auto cwt = kfn<CreateWT_t>("CreateWaitableTimerExW");
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    if (cwt) {
        HANDLE t = cwt(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (t) {
            QueryPerformanceCounter(&a);
            for (int i = 0; i < 100; ++i) {
                LARGE_INTEGER due{};
                due.QuadPart = -10000;   // 1 ms
                SetWaitableTimerEx(t, &due, 0, nullptr, nullptr, nullptr, 0);
                WaitForSingleObject(t, INFINITE);
            }
            QueryPerformanceCounter(&b);
            R.add("high-resolution timer, 1 ms wait", fmt("%.3f ms average", (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart / 100));
            CloseHandle(t);
        } else {
            R.add("high-resolution timer", fmt("failed (%lu)", GetLastError()));
        }
    }
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 100; ++i) Sleep(1);
    QueryPerformanceCounter(&b);
    R.add("Sleep(1)", fmt("%.3f ms average", (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart / 100));
    if (auto tbp = reinterpret_cast<UINT(WINAPI*)(UINT)>(sym(L"winmm.dll", "timeBeginPeriod")))
        R.add("timeBeginPeriod(1)", fmt("%u", tbp(1)));
    R.flush();
}

// ---- 6. the guest memory window -------------------------------------------------------------------------------------

std::atomic<uint8_t*> g_veh_lo, g_veh_hi;
std::atomic<int> g_veh_commits, g_veh_unprotects;
uint8_t* g_protected;

LONG WINAPI veh(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    auto* at = reinterpret_cast<uint8_t*>(e->ExceptionRecord->ExceptionInformation[1]);
    if (g_protected && at >= g_protected && at < g_protected + 4096) {
        DWORD old;
        pVirtualProtect(g_protected, 4096, PAGE_READWRITE, &old);
        ++g_veh_unprotects;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (at >= g_veh_lo.load() && at < g_veh_hi.load()) {
        auto* page = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(at) & ~uintptr_t(0xFFFF));
        if (pVirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
            ++g_veh_commits;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

uint8_t* g_guest;   // the reservation at kGuestBase, when it worked

void probe_memory() {
    R.section("memory");
    try {
        using winrt::Windows::System::MemoryManager;
        R.add("AppMemoryUsageLimit", fmt("%llu MB", MemoryManager::AppMemoryUsageLimit() / MB));
        R.add("AppMemoryUsage", fmt("%llu MB", MemoryManager::AppMemoryUsage() / MB));
        R.add("ExpectedAppMemoryUsageLimit", fmt("%llu MB", MemoryManager::ExpectedAppMemoryUsageLimit() / MB));
    } catch (...) {
        R.add("MemoryManager", "unavailable");
    }
    MEMORYSTATUSEX ms{sizeof ms};
    if (GlobalMemoryStatusEx(&ms))
        R.add("GlobalMemoryStatusEx phys total/avail, commit limit/avail",
              fmt("%llu / %llu MB, %llu / %llu MB", ms.ullTotalPhys / MB, ms.ullAvailPhys / MB, ms.ullTotalPageFile / MB, ms.ullAvailPageFile / MB));

    pVirtualAlloc = kfn<VirtualAlloc_t>("VirtualAlloc");
    pVirtualFree = kfn<VirtualFree_t>("VirtualFree");
    pVirtualProtect = kfn<VirtualProtect_t>("VirtualProtect");
    auto vafa = kfn<VirtualAllocFromApp_t>("VirtualAllocFromApp");
    auto va2fa = kfn<VirtualAlloc2FromApp_t>("VirtualAlloc2FromApp");
    R.add("VirtualAlloc / VirtualAllocFromApp / VirtualAlloc2FromApp found",
          fmt("%s / %s / %s", pVirtualAlloc ? "yes" : "no", vafa ? "yes" : "no", va2fa ? "yes" : "no"));
    if (!pVirtualAlloc || !pVirtualFree || !pVirtualProtect) { R.flush(); return; }

    void* p = pVirtualAlloc(reinterpret_cast<void*>(kGuestBase), kGuestSize, MEM_RESERVE, PAGE_NOACCESS);
    R.add("VirtualAlloc 512 GB reserve at 0x100000000000", p == reinterpret_cast<void*>(kGuestBase) ? "ok" : fmt("failed (%lu)", GetLastError()));
    if (!p && vafa) {
        p = vafa(reinterpret_cast<void*>(kGuestBase), kGuestSize, MEM_RESERVE, PAGE_NOACCESS);
        R.add("VirtualAllocFromApp 512 GB reserve at 0x100000000000", p == reinterpret_cast<void*>(kGuestBase) ? "ok" : fmt("failed (%lu)", GetLastError()));
    }
    if (!p) {
        void* any = pVirtualAlloc(nullptr, kGuestSize, MEM_RESERVE, PAGE_NOACCESS);
        R.add("512 GB reserve anywhere", any ? fmt("ok at %p", any) : fmt("failed (%lu)", GetLastError()));
        if (any) pVirtualFree(any, 0, MEM_RELEASE);
        for (uint64_t gb = 256; gb >= 1 && !p; gb /= 2) {
            p = pVirtualAlloc(reinterpret_cast<void*>(kGuestBase), gb << 30, MEM_RESERVE, PAGE_NOACCESS);
            if (p) R.add("largest reserve at 0x100000000000", fmt("%llu GB", gb));
        }
    }
    g_guest = static_cast<uint8_t*>(p);
    if (!g_guest) { R.flush(); return; }

    uint8_t* c = static_cast<uint8_t*>(pVirtualAlloc(g_guest + 0x80000000ull, 16 * MB, MEM_COMMIT, PAGE_READWRITE));
    if (c) { memset(c, 0x5A, 16 * MB); }
    R.add("commit 16 MB inside the window", c && c[16 * MB - 1] == 0x5A ? "ok" : fmt("failed (%lu)", GetLastError()));

    auto add = kfn<AddVEH_t>("AddVectoredExceptionHandler");
    auto remove = kfn<RemoveVEH_t>("RemoveVectoredExceptionHandler");
    if (add) {
        g_veh_lo = g_guest + (64ull << 30);
        g_veh_hi = g_guest + (65ull << 30);
        PVOID h = add(1, veh);
        volatile uint8_t* touch = g_guest + (64ull << 30) + 12345;
        *touch = 7;
        int got = *touch;
        R.add("commit-on-touch by a vectored handler", fmt("%s (%d commits)", got == 7 ? "ok" : "failed", g_veh_commits.load()));
        g_protected = c;
        DWORD old;
        if (c && pVirtualProtect(c, 4096, PAGE_READONLY, &old)) {
            volatile uint8_t* w = c;
            *w = 9;
            R.add("write trap on a read-only page", fmt("%s (%d unprotects)", c[0] == 9 ? "ok" : "failed", g_veh_unprotects.load()));
        }
        g_protected = nullptr;
        if (remove) remove(h);
    } else {
        R.add("AddVectoredExceptionHandler", "not found");
    }

    void* ww = pVirtualAlloc(nullptr, 64 * MB, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    auto gww = kfn<GetWriteWatch_t>("GetWriteWatch");
    if (ww && gww) {
        static_cast<uint8_t*>(ww)[4096 * 3] = 1;
        void* addrs[16];
        ULONG_PTR n = 16;
        DWORD gran = 0;
        UINT r = gww(0, ww, 64 * MB, addrs, &n, &gran);
        R.add("MEM_WRITE_WATCH + GetWriteWatch", r == 0 ? fmt("ok (%llu pages written)", (unsigned long long)n) : "GetWriteWatch failed");
    } else {
        R.add("MEM_WRITE_WATCH", ww ? "GetWriteWatch not found" : fmt("reserve failed (%lu)", GetLastError()));
    }
    if (ww) pVirtualFree(ww, 0, MEM_RELEASE);
    R.flush();
}

// ---- 7. running code that is not in the package --------------------------------------------------------------------
// The package can never hold the game (it is made from the game), so the person who installs it builds the game module
// on their own PC and puts it on the USB drive. This asks whether the app can run it: either Windows loads a DLL from
// outside the package, or the app maps it itself into memory it makes executable (the codeGeneration capability, the
// Dev Mode emulators' JIT permission).

void probe_codegen() {
    R.section("code from outside the package");
    using VirtualProtectFromApp_t = BOOL(WINAPI*)(PVOID, SIZE_T, ULONG, PULONG);
    auto vafa = kfn<VirtualAllocFromApp_t>("VirtualAllocFromApp");
    auto vpfa = kfn<VirtualProtectFromApp_t>("VirtualProtectFromApp");
    if (!vafa || !vpfa) {
        R.add("VirtualAllocFromApp / VirtualProtectFromApp", "not found");
    } else {
        static const uint8_t ret42[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};   // mov eax, 42; ret
        auto* p = static_cast<uint8_t*>(vafa(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        ULONG old = 0;
        if (p) {
            memcpy(p, ret42, sizeof ret42);
            BOOL ok = vpfa(p, 0x10000, PAGE_EXECUTE_READ, &old);
            R.add("make written memory executable", ok ? "ok" : fmt("refused (%lu)", GetLastError()));
            if (ok) {
                FlushInstructionCache(GetCurrentProcess(), p, sizeof ret42);
                R.add("run it", fmt("returned %d", reinterpret_cast<int (*)()>(p)()));
            }
        } else {
            R.add("VirtualAllocFromApp 64 KB", fmt("failed (%lu)", GetLastError()));
        }
        const uint64_t big = 640 * MB;   // (the game exe's code is about 430 MB)
        auto* b = static_cast<uint8_t*>(vafa(nullptr, big, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (b) {
            memset(b, 0xC3, big);
            BOOL ok = vpfa(b, big, PAGE_EXECUTE_READ, &old);
            if (ok) {
                FlushInstructionCache(GetCurrentProcess(), b, big);
                reinterpret_cast<void (*)()>(b + big - 64)();
            }
            R.add("640 MB made executable", ok ? "ok, and its last page runs" : fmt("refused (%lu)", GetLastError()));
            VirtualFree(b, 0, MEM_RELEASE);
        } else {
            R.add("640 MB for code", fmt("allocation failed (%lu)", GetLastError()));
        }
    }
    // (loaded code needs its unwind tables registered, or an exception through it ends the app; LLVM maps files it reads)
    using RtlAddFunctionTable_t = BOOLEAN(WINAPI*)(PRUNTIME_FUNCTION, DWORD, DWORD64);
    auto add_table = reinterpret_cast<RtlAddFunctionTable_t>(sym(L"ntdll.dll", "RtlAddFunctionTable"));
    if (!add_table) add_table = kfn<RtlAddFunctionTable_t>("RtlAddFunctionTable");
    R.add("RtlAddFunctionTable", add_table ? "found" : "not found");
    R.add("CreateFileMappingFromApp / MapViewOfFileFromApp",
          fmt("%s / %s", kfn<FARPROC>("CreateFileMappingFromApp") ? "found" : "not found", kfn<FARPROC>("MapViewOfFileFromApp") ? "found" : "not found"));
    R.add("CreateProcessW (would let a builder run clang as a program)", kfn<FARPROC>("CreateProcessW") ? "found" : "not found");
    try {
        std::wstring local(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
        std::wstring from = std::wstring(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path()) + L"\\dxil.dll";
        std::wstring to = local + L"\\copy-of-dxil.dll";
        COPYFILE2_EXTENDED_PARAMETERS cp{sizeof cp};
        HRESULT hr = CopyFile2(from.c_str(), to.c_str(), &cp);
        if (SUCCEEDED(hr)) {
            HMODULE m = LoadLibraryExW(to.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            R.add("LoadLibraryExW of a DLL in LocalState", m ? "loads" : fmt("refused (%lu)", GetLastError()));
            if (m) FreeLibrary(m);
            DeleteFileW(to.c_str());
        } else {
            R.add("copy a DLL to LocalState", hr_text(hr));
        }
    } catch (winrt::hresult_error const& e) {
        R.add("LoadLibraryExW of a DLL in LocalState", "error " + narrow(e.message()));
    }
    R.flush();
}

// ---- 8. building on the console --------------------------------------------------------------------------------------
// A builder on the console lifts and compiles the game from the player's own files on the USB drive: how fast the CPU
// is next to a PC (the same benchmark as bench_pc.exe), whether the app may start a program (clang as a program rather
// than a library), and how fast the USB drive reads and writes (the build's work folder would live there).

void probe_builder() {
    R.section("building on the console");
    auto b = bench::run();
    R.add("CPU benchmark", fmt("one thread %.0f ms; %u threads at once %.0f ms (bench_pc.exe gives a PC's)", b.one_ms, b.threads, b.all_ms));
    R.flush();

    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    using CreateProcessW_t = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
                                           LPSTARTUPINFOW, LPPROCESS_INFORMATION);
    auto cp = kfn<CreateProcessW_t>("CreateProcessW");
    if (!n || !cp) {
        R.add("start a program", cp ? "own path unknown" : "CreateProcessW not found");
    } else {
        std::wstring cmd = L"\"" + std::wstring(self) + L"\" --child";
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        if (cp(self, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 10000);
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            R.add("start a program (the probe itself)", fmt("started, exit code %lu (7 expected)", code));
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            R.add("start a program (the probe itself)", fmt("refused (%lu)", GetLastError()));
        }
    }
    R.flush();

    const wchar_t* dir = L"E:\\LongshotProbe";
    CreateDirectoryW(dir, nullptr);
    std::wstring file = std::wstring(dir) + L"\\speed.bin";
    HANDLE h = CreateFile2(file.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        R.add("USB drive speed", fmt("cannot write E:\\LongshotProbe (%lu)", GetLastError()));
        R.flush();
        return;
    }
    std::vector<uint8_t> buf(8 * MB, 0xA5);
    LARGE_INTEGER f, a, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    DWORD done = 0;
    bool ok = true;
    for (int i = 0; i < 64 && ok; ++i) ok = WriteFile(h, buf.data(), DWORD(buf.size()), &done, nullptr) && done == buf.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    QueryPerformanceCounter(&c);
    double write_s = double(c.QuadPart - a.QuadPart) / f.QuadPart;
    h = CreateFile2(file.c_str(), GENERIC_READ, 0, OPEN_EXISTING, nullptr);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 64 && ok && h != INVALID_HANDLE_VALUE; ++i) ok = ReadFile(h, buf.data(), DWORD(buf.size()), &done, nullptr) && done == buf.size();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    QueryPerformanceCounter(&c);
    double read_s = double(c.QuadPart - a.QuadPart) / f.QuadPart;
    DeleteFileW(file.c_str());
    RemoveDirectoryW(dir);
    R.add("USB drive, 512 MB written / read", ok ? fmt("%.0f MB/s / %.0f MB/s (the read may come from the cache)", 512 / write_s, 512 / read_s) : "I/O failed");
    R.flush();
}

// ---- 9. Direct3D 12 ----------------------------------------------------------------------------------------------

com_ptr<IDxcCompiler3> g_dxc;

bool load_dxc() {
    HMODULE m = LoadPackagedLibrary(L"dxcompiler.dll", 0);
    if (!m) { R.add("dxcompiler.dll (packaged)", fmt("does not load (%lu)", GetLastError())); return false; }
    auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(m, "DxcCreateInstance"));
    if (!create || FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), g_dxc.put_void()))) {
        R.add("dxcompiler.dll (packaged)", "DxcCreateInstance failed");
        return false;
    }
    HMODULE dxil = LoadPackagedLibrary(L"dxil.dll", 0);
    R.add("dxcompiler.dll / dxil.dll (packaged)", dxil ? "load" : "dxcompiler loads, dxil.dll does not (unsigned DXIL)");
    return true;
}

com_ptr<IDxcBlob> compile(const char* src, const wchar_t* entry, const wchar_t* target, const char* what) {
    if (!g_dxc) return nullptr;
    DxcBuffer b{src, strlen(src), DXC_CP_UTF8};
    LPCWSTR args[] = {L"-E", entry, L"-T", target, L"-HV", L"2021", L"-O3"};
    com_ptr<IDxcResult> res;
    if (FAILED(g_dxc->Compile(&b, args, _countof(args), nullptr, __uuidof(IDxcResult), res.put_void()))) {
        R.add(std::string("compile ") + what, "Compile call failed");
        return nullptr;
    }
    HRESULT st = E_FAIL;
    res->GetStatus(&st);
    if (FAILED(st)) {
        com_ptr<IDxcBlobUtf8> err;
        res->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), err.put_void(), nullptr);
        R.add(std::string("compile ") + what, std::string("failed: ") + (err ? err->GetStringPointer() : "?"));
        return nullptr;
    }
    com_ptr<IDxcBlob> obj;
    res->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), obj.put_void(), nullptr);
    R.add(std::string("compile ") + what, fmt("ok (%zu bytes)", obj ? obj->GetBufferSize() : 0));
    return obj;
}

const char* kCompute = R"HLSL(
[RootSignature("RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    RWByteAddressBuffer o = ResourceDescriptorHeap[0];
    o.Store(id.x * 4, id.x);
}
)HLSL";

const char* kComputeRootUav = R"HLSL(
RWByteAddressBuffer o : register(u0);
[RootSignature("UAV(u0)")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint64_t v = (uint64_t)id.x * 0x100000001ull;
    o.Store<uint64_t>(id.x * 8, v + WaveActiveSum(1u));
}
)HLSL";

// The Series S UWP probe reports shader model 6.4, so Longshot cannot use
// ResourceDescriptorHeap directly. Check whether a bounded descriptor table
// with a non-uniform index can still be used for a future address-table path.
const char* kComputeDescriptorArray = R"HLSL(
ByteAddressBuffer resources[4] : register(t0);
RWByteAddressBuffer output : register(u0);
[RootSignature("DescriptorTable(SRV(t0, numDescriptors=4)), UAV(u0)")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint slot = NonUniformResourceIndex(id.x & 3);
    output.Store(id.x * 4, resources[slot].Load(0));
}
)HLSL";

const char* kMesh = R"HLSL(
struct V { float4 p : SV_Position; };
[RootSignature("RootFlags(0)")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void ms(out vertices V v[3], out indices uint3 t[1]) {
    SetMeshOutputCounts(3, 1);
    v[0].p = float4(0, 0, 0, 1); v[1].p = float4(1, 0, 0, 1); v[2].p = float4(0, 1, 0, 1);
    t[0] = uint3(0, 1, 2);
}
float4 ps() : SV_Target { return 1; }
)HLSL";

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE T, class V>
struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = T;
    V value;
};

struct Gpu {
    com_ptr<ID3D12Device> dev;
    com_ptr<ID3D12CommandQueue> q;
    com_ptr<ID3D12CommandAllocator> alloc;
    com_ptr<ID3D12GraphicsCommandList> list;
    com_ptr<ID3D12Fence> fence;
    uint64_t value = 0;

    bool init() {
        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
        return SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), q.put_void())) &&
               SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), alloc.put_void())) &&
               SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.get(), nullptr, __uuidof(ID3D12GraphicsCommandList), list.put_void())) &&
               SUCCEEDED(list->Close()) &&
               SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), fence.put_void()));
    }
    void begin() { alloc->Reset(); list->Reset(alloc.get(), nullptr); }
    HRESULT run() {
        HRESULT hr = list->Close();
        if (FAILED(hr)) return hr;
        ID3D12CommandList* l[] = {list.get()};
        q->ExecuteCommandLists(1, l);
        q->Signal(fence.get(), ++value);
        for (int i = 0; i < 5000 && fence->GetCompletedValue() < value; ++i) Sleep(1);
        if (fence->GetCompletedValue() < value) return E_PENDING;
        return dev->GetDeviceRemovedReason();
    }
};

// A buffer over memory the CPU allocated (ID3D12Device3::OpenExistingHeapFromAddress): the GPU copies the first MB to
// the second, and a shader writes the third, on memory that is not copied anywhere, as Longshot's NVN pools need.
void existing_heap_test(Gpu& g, uint8_t* mem, const char* label, ID3D12PipelineState* pso, ID3D12RootSignature* rs) {
    com_ptr<ID3D12Device3> d3 = g.dev.try_as<ID3D12Device3>();
    if (!d3) { R.add(label, "no ID3D12Device3"); return; }
    for (uint64_t i = 0; i < MB; ++i) mem[i] = uint8_t(i * 7 + 3);
    memset(mem + MB, 0, 2 * MB);
    com_ptr<ID3D12Heap> heap;
    HRESULT hr = d3->OpenExistingHeapFromAddress(mem, __uuidof(ID3D12Heap), heap.put_void());
    if (FAILED(hr)) { R.add(label, "OpenExistingHeapFromAddress " + hr_text(hr)); return; }
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 16 * MB, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    com_ptr<ID3D12Resource> buf;
    hr = g.dev->CreatePlacedResource(heap.get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), buf.put_void());
    if (FAILED(hr)) { R.add(label, "CreatePlacedResource " + hr_text(hr)); return; }
    g.begin();
    g.list->CopyBufferRegion(buf.get(), MB, buf.get(), 0, MB);
    if (pso && rs) {
        D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_UAV};
        g.list->ResourceBarrier(1, &b);
        g.list->SetComputeRootSignature(rs);
        g.list->SetPipelineState(pso);
        g.list->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress() + 2 * MB);
        g.list->Dispatch(16, 1, 1);
    }
    hr = g.run();
    if (FAILED(hr)) { R.add(label, "GPU work " + hr_text(hr)); return; }
    bool copy_ok = memcmp(mem, mem + MB, MB) == 0;
    std::string shader = "not run";
    if (pso && rs) {
        auto* o = reinterpret_cast<uint64_t*>(mem + 2 * MB);
        uint64_t lanes = o[0];
        bool ok = lanes == 32 || lanes == 64;
        for (uint64_t i = 1; i < 1024 && ok; ++i) ok = o[i] == i * 0x100000001ull + lanes;
        shader = ok ? fmt("ok (wave of %llu)", (unsigned long long)lanes) : "wrong values";
    }
    R.add(label, fmt("heap ok, GPU copy %s, shader write %s", copy_ok ? "ok" : "WRONG", shader.c_str()));
}

void probe_d3d12() {
    R.section("Direct3D 12");
    com_ptr<IDXGIFactory4> fac;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), fac.put_void()))) { R.add("DXGI factory", "failed"); R.flush(); return; }
    com_ptr<IDXGIAdapter1> ad;
    if (FAILED(fac->EnumAdapters1(0, ad.put()))) { R.add("adapter", "none"); R.flush(); return; }
    DXGI_ADAPTER_DESC1 desc{};
    ad->GetDesc1(&desc);
    R.add("adapter", narrow(desc.Description));
    R.add("dedicated video / dedicated system / shared", fmt("%llu / %llu / %llu MB", (unsigned long long)desc.DedicatedVideoMemory / MB,
                                                              (unsigned long long)desc.DedicatedSystemMemory / MB, (unsigned long long)desc.SharedSystemMemory / MB));
    if (auto a3 = ad.try_as<IDXGIAdapter3>()) {
        DXGI_QUERY_VIDEO_MEMORY_INFO li{}, ni{};
        a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li);
        a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &ni);
        R.add("video memory budget local / non-local", fmt("%llu / %llu MB", li.Budget / MB, ni.Budget / MB));
    }

    Gpu g;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    for (auto fl : levels) {
        if (SUCCEEDED(D3D12CreateDevice(ad.get(), fl, __uuidof(ID3D12Device), g.dev.put_void()))) {
            R.add("device feature level", fmt("%x_%x", fl >> 12, (fl >> 8) & 0xF));
            break;
        }
    }
    if (!g.dev) { R.add("D3D12CreateDevice", "failed at every level"); R.flush(); return; }
    auto& dev = g.dev;

    D3D12_FEATURE_DATA_SHADER_MODEL sm{};
    const D3D_SHADER_MODEL models[] = {(D3D_SHADER_MODEL)0x69, (D3D_SHADER_MODEL)0x68, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
                                       D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
                                       D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1};
    for (auto m : models) {
        sm.HighestShaderModel = m;
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) break;
    }
    R.add("highest shader model", fmt("%x.%x", sm.HighestShaderModel >> 4, sm.HighestShaderModel & 0xF));

    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    R.add("resource binding tier / tiled resources tier", fmt("%d / %d", o.ResourceBindingTier, o.TiledResourcesTier));
    R.add("typed UAV load additional formats / double precision", fmt("%d / %d", o.TypedUAVLoadAdditionalFormats, o.DoublePrecisionFloatShaderOps));
    R.add("ROVs / standard swizzle 64KB", fmt("%d / %d", o.ROVsSupported, o.StandardSwizzle64KBSupported));
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1);
    R.add("wave ops / lanes min-max / int64 shader ops", fmt("%d / %u-%u / %d", o1.WaveOps, o1.WaveLaneCountMin, o1.WaveLaneCountMax, o1.Int64ShaderOps));
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof o4);
    R.add("native 16-bit shader ops", fmt("%d", o4.Native16BitShaderOpsSupported));
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7);
    R.add("mesh shader tier / sampler feedback tier", fmt("%d / %d", o7.MeshShaderTier, o7.SamplerFeedbackTier));
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof o9);
    R.add("int64 atomics typed / group-shared, wave MMA tier",
          fmt("%d / %d, %d", o9.AtomicInt64OnTypedResourceSupported, o9.AtomicInt64OnGroupSharedSupported, o9.WaveMMATier));
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 o11{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &o11, sizeof o11);
    R.add("int64 atomics on descriptor-heap resources", fmt("%d", o11.AtomicInt64OnDescriptorHeapResourceSupported));
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12);
    R.add("enhanced barriers / relaxed format casting", fmt("%d / %d", o12.EnhancedBarriersSupported, o12.RelaxedFormatCastingSupported));
    D3D12_FEATURE_DATA_ROOT_SIGNATURE rsv{D3D_ROOT_SIGNATURE_VERSION_1_2};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rsv, sizeof rsv))) {
        rsv.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        dev->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rsv, sizeof rsv);
    }
    R.add("root signature version", fmt("0x%x", rsv.HighestVersion));
    D3D12_FEATURE_DATA_ARCHITECTURE1 ar{};
    dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &ar, sizeof ar);
    R.add("UMA / cache-coherent UMA / isolated MMU", fmt("%d / %d / %d", ar.UMA, ar.CacheCoherentUMA, ar.IsolatedMMU));
    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va{};
    dev->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &va, sizeof va);
    R.add("GPU virtual address bits per resource / process", fmt("%u / %u", va.MaxGPUVirtualAddressBitsPerResource, va.MaxGPUVirtualAddressBitsPerProcess));
    D3D12_FEATURE_DATA_EXISTING_HEAPS eh{};
    dev->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &eh, sizeof eh);
    R.add("existing heaps (host memory import)", eh.Supported ? "supported" : "not supported");
    R.flush();

    if (!g.init()) { R.add("queue/list/fence", "failed"); R.flush(); return; }
    load_dxc();
    com_ptr<ID3D12RootSignature> rs;
    com_ptr<ID3D12PipelineState> pso;
    // The measured adapter supports shader model 6.4. Compiling a 6.6 shader succeeds in DXC but the adapter
    // rejects its pipeline, which only says that the requested shader model is too new.
    if (auto cs = compile(kComputeRootUav, L"main", L"cs_6_4", "compute cs_6_4 (int64, wave ops)")) {
        HRESULT hr = dev->CreateRootSignature(0, cs->GetBufferPointer(), cs->GetBufferSize(), __uuidof(ID3D12RootSignature), rs.put_void());
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs.get();
        pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        if (SUCCEEDED(hr)) hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), pso.put_void());
        R.add("compute pipeline", hr_text(hr));
        if (FAILED(hr)) pso = nullptr;
    }
    if (auto cs = compile(kComputeDescriptorArray, L"main", L"cs_6_4", "cs_6_4 bounded non-uniform SRV array")) {
        com_ptr<ID3D12RootSignature> array_rs;
        com_ptr<ID3D12PipelineState> array_pso;
        HRESULT hr = dev->CreateRootSignature(0, cs->GetBufferPointer(), cs->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                              array_rs.put_void());
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = array_rs.get();
        pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        if (SUCCEEDED(hr)) hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), array_pso.put_void());
        R.add("bounded non-uniform SRV array pipeline", hr_text(hr));
    }
    compile(kCompute, L"main", L"cs_6_6", "DXC-only cs_6_6 (bindless ResourceDescriptorHeap; adapter reports max 6.4)");
    R.flush();

    uint8_t* plain = static_cast<uint8_t*>(pVirtualAlloc ? pVirtualAlloc(nullptr, 16 * MB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : nullptr);
    if (plain) existing_heap_test(g, plain, "host memory import (ordinary allocation)", pso.get(), rs.get());
    R.flush();
    if (g_guest) {
        uint8_t* in = static_cast<uint8_t*>(pVirtualAlloc(g_guest + (100ull << 30), 16 * MB, MEM_COMMIT, PAGE_READWRITE));
        if (in) existing_heap_test(g, in, "host memory import (inside the 512 GB window)", pso.get(), rs.get());
        else R.add("host memory import (inside the 512 GB window)", "commit failed");
    }
    R.flush();

    if (o7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1) {
        auto ms = compile(kMesh, L"ms", L"ms_6_5", "mesh shader ms_6_5");
        auto ps = compile(kMesh, L"ps", L"ps_6_5", "pixel shader ps_6_5");
        com_ptr<ID3D12Device2> d2 = dev.try_as<ID3D12Device2>();
        if (ms && ps && d2) {
            com_ptr<ID3D12RootSignature> mrs;
            HRESULT hr = dev->CreateRootSignature(0, ms->GetBufferPointer(), ms->GetBufferSize(), __uuidof(ID3D12RootSignature), mrs.put_void());
            struct {
                Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> rs;
                Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
                Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
                Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
                Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
                Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> ds;
                Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rt;
                Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sd;
                Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
            } stream{};
            stream.rs.value = mrs.get();
            stream.ms.value = {ms->GetBufferPointer(), ms->GetBufferSize()};
            stream.ps.value = {ps->GetBufferPointer(), ps->GetBufferSize()};
            stream.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            stream.rast.value.FillMode = D3D12_FILL_MODE_SOLID;
            stream.rast.value.CullMode = D3D12_CULL_MODE_NONE;
            stream.rast.value.DepthClipEnable = TRUE;
            stream.rt.value.NumRenderTargets = 1;
            stream.rt.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            stream.sd.value = {1, 0};
            stream.mask.value = UINT_MAX;
            D3D12_PIPELINE_STATE_STREAM_DESC sdsc{sizeof stream, &stream};
            com_ptr<ID3D12PipelineState> mpso;
            if (SUCCEEDED(hr)) hr = d2->CreatePipelineState(&sdsc, __uuidof(ID3D12PipelineState), mpso.put_void());
            R.add("mesh shader pipeline", hr_text(hr));
        }
    }
    R.flush();

    // How much GPU memory the game can allocate (it comes out of the same budget as the CPU's on the console).
    std::vector<com_ptr<ID3D12Resource>> bufs;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256 * MB, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    HRESULT last = S_OK;
    for (int i = 0; i < 32; ++i) {
        com_ptr<ID3D12Resource> b;
        last = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), b.put_void());
        if (FAILED(last)) break;
        bufs.push_back(b);
        if (i % 4 == 3) {
            R.note(fmt("GPU allocations so far: %zu MB", bufs.size() * 256));
            R.flush();
        }
    }
    R.add("GPU memory allocated in 256 MB buffers", fmt("%zu MB (stopped: %s)", bufs.size() * 256, FAILED(last) ? hr_text(last).c_str() : "cap of 8 GB"));
    try {
        R.add("AppMemoryUsage with them", fmt("%llu MB", winrt::Windows::System::MemoryManager::AppMemoryUsage() / MB));
    } catch (...) {}
    bufs.clear();
    R.flush();
}

// ---- 10. how much memory the CPU can commit and touch (last: it may end the app) ----------------------------------

void probe_commit() {
    R.section("CPU commit limit (256 MB steps, every page touched)");
    if (!pVirtualAlloc) return;
    std::vector<void*> blocks;
    for (int i = 0; i < 48; ++i) {
        void* p = pVirtualAlloc(nullptr, 256 * MB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) {
            R.add("stopped", fmt("at %zu MB (error %lu)", blocks.size() * 256, GetLastError()));
            break;
        }
        for (uint64_t o = 0; o < 256 * MB; o += 4096) static_cast<volatile uint8_t*>(p)[o] = 1;
        blocks.push_back(p);
        uint64_t usage = 0;
        try { usage = winrt::Windows::System::MemoryManager::AppMemoryUsage() / MB; } catch (...) {}
        R.note(fmt("%zu MB committed, AppMemoryUsage %llu MB", blocks.size() * 256, usage));
        R.flush();
    }
    R.add("largest commit", fmt("%zu MB", blocks.size() * 256));
    for (void* p : blocks) pVirtualFree(p, 0, MEM_RELEASE);
    R.flush();
}

// ---- the window: the report as text --------------------------------------------------------------------------------

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
    }

    void Run() {
        window_.Activate();
        R.section("Longshot Xbox probe");
        R.note("Results: LocalState\\probe.txt and probe.json (Device Portal, File explorer).");
        R.flush();
        std::thread worker([] {
            try {
                winrt::init_apartment();
                probe_system();
                probe_imports();
                probe_storage();
                probe_audio_input();
                probe_threads();
                probe_memory();
                probe_codegen();
                probe_builder();
                probe_d3d12();
                probe_commit();
                R.section("done");
                R.flush();
            } catch (...) {
                R.stopped(what());
            }
        });
        worker.detach();
        try {
            init_graphics();
        } catch (...) {
            R.note("The screen could not be set up, the probe goes on without it: " + what());
            R.flush();
            ctx_ = nullptr;
        }
        while (true) {
            window_.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            try {
                scroll();
            } catch (...) {}
            draw();
        }
    }

    // (the exception being handled, as text)
    static std::string what() {
        try {
            throw;
        } catch (winrt::hresult_error const& e) {
            return hr_text(e.code()) + " " + narrow(e.message());
        } catch (std::exception const& e) {
            return e.what();
        } catch (...) {
            return "an unknown exception";
        }
    }

private:
    void init_graphics() {
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, d3d_.put(), nullptr, nullptr);
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
        fac->CreateSwapChainForCoreWindow(d3d_.get(), winrt::get_unknown(window_), &sd, nullptr, swap_.put());
        if (!swap_) return;
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
        dw_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 17.0f, L"en-us", fmt_.put());
        ctx_->CreateSolidColorBrush({0.9f, 0.95f, 0.9f, 1}, brush_.put());
        width_ = float(sd.Width);
        height_ = float(sd.Height);
    }

    void scroll() {
        auto pads = wgi::Gamepad::Gamepads();
        if (pads.Size() == 0) return;
        auto r = pads.GetAt(0).GetCurrentReading();
        auto held = [&](wgi::GamepadButtons b) { return (r.Buttons & b) != wgi::GamepadButtons::None; };
        bool down = held(wgi::GamepadButtons::DPadDown) || r.LeftThumbstickY < -0.5;
        bool up = held(wgi::GamepadButtons::DPadUp) || r.LeftThumbstickY > 0.5;
        if (down || up) follow_ = false;
        if (down) ++top_;
        if (up && top_ > 0) --top_;
        if (held(wgi::GamepadButtons::A)) follow_ = true;
    }

    void draw() {
        if (!ctx_) { Sleep(16); return; }
        auto lines = R.lines();
        const float line_h = 20.0f, margin = 60.0f;
        int rows = int((height_ - 2 * margin) / line_h);
        if (follow_) top_ = std::max(0, int(lines.size()) - rows);
        top_ = std::min(top_, std::max(0, int(lines.size()) - 1));
        ctx_->BeginDraw();
        ctx_->Clear({0.08f, 0.09f, 0.1f, 1});
        for (int i = 0; i < rows && top_ + i < int(lines.size()); ++i) {
            std::wstring w(lines[top_ + i].begin(), lines[top_ + i].end());
            D2D1_RECT_F rc{margin, margin + i * line_h, width_ - margin, margin + (i + 1) * line_h};
            ctx_->DrawText(w.c_str(), UINT32(w.size()), fmt_.get(), rc, brush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
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
    com_ptr<IDWriteTextFormat> fmt_;
    com_ptr<ID2D1SolidColorBrush> brush_;
    float width_ = 1920, height_ = 1080;
    int top_ = 0;
    bool follow_ = true;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    if (cmd && wcsstr(cmd, L"--child")) return 7;   // (probe_builder starting a program)
    winrt::init_apartment();   // (CoreApplication refuses a main thread that is not in the multi-threaded apartment)
    try {
        R.set_folder(std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
    } catch (...) {}
    std::set_terminate([] {
        std::string why = "std::terminate";
        if (std::current_exception()) {
            try {
                throw;
            } catch (winrt::hresult_error const& e) {
                why += ": " + hr_text(e.code()) + " " + narrow(e.message());
            } catch (std::exception const& e) {
                why += std::string(": ") + e.what();
            } catch (...) {}
        }
        R.stopped(why);
        TerminateProcess(GetCurrentProcess(), 3);
    });
    try {
        CoreApplication::Run(make<App>());
    } catch (winrt::hresult_error const& e) {
        R.stopped("the app ended with " + hr_text(e.code()) + " " + narrow(e.message()));
        return 1;
    }
    return 0;
}
