// The modules the app loads by hand and the resolver of their imports: xbox/app/modules.h.
#include "modules.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace longshot_xbox::modules {

namespace {

// ---- system functions, looked up at run time (the UWP import library lacks some of them) ----

using LoadPackagedLibrary_t = HMODULE(WINAPI*)(LPCWSTR, DWORD);
using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using GetModuleHandleExW_t = BOOL(WINAPI*)(DWORD, LPCWSTR, HMODULE*);
using GetModuleFileNameW_t = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
using FreeLibrary_t = BOOL(WINAPI*)(HMODULE);

FARPROC find_sys(const char* name) {
    for (const wchar_t* dll : {L"kernelbase.dll", L"kernel32.dll"}) {
        HMODULE h = GetModuleHandleW(dll);
        if (FARPROC p = h ? GetProcAddress(h, name) : nullptr) return p;
    }
    return nullptr;
}

struct Sys {
    LoadPackagedLibrary_t load_packaged = (LoadPackagedLibrary_t)find_sys("LoadPackagedLibrary");
    LoadLibraryExW_t load_ex = (LoadLibraryExW_t)find_sys("LoadLibraryExW");
    GetModuleHandleExW_t handle_ex = (GetModuleHandleExW_t)find_sys("GetModuleHandleExW");
    GetModuleFileNameW_t file_name = (GetModuleFileNameW_t)find_sys("GetModuleFileNameW");
    FreeLibrary_t free_library = (FreeLibrary_t)find_sys("FreeLibrary");
};
Sys& sys() {
    static Sys s;
    return s;
}

std::wstring widen(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// "C:\x\DXGI.DLL" -> "dxgi.dll"; "d3d12" -> "d3d12.dll" (LoadLibrary's rule: no extension, .dll)
std::string base_name(const std::string& name) {
    size_t slash = name.find_last_of("\\/");
    std::string b = slash == std::string::npos ? name : name.substr(slash + 1);
    for (char& c : b) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (b.find('.') == std::string::npos) b += ".dll";
    return b;
}

bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// ---- the set ----

struct Virtual {
    std::string name;
    Lookup lookup;
};

struct Mapped {
    std::string name;   // (as asked for: vulkan-1.dll)
    std::wstring path;
    pel_module* m = nullptr;
    std::map<std::string, std::string> alias;
};

std::recursive_mutex g_mu;
Config g_cfg;
Lookup g_win32 = nullptr;
std::vector<std::unique_ptr<Virtual>> g_virtuals;   // (a virtual DLL's HMODULE is its entry's address)
std::vector<std::unique_ptr<Mapped>> g_mapped;      // (a mapped one's is its image base, as Windows'),
std::set<std::string> g_logged;
pel_tls_slot g_slot{};
bool g_slot_tried = false;

void log(const std::string& s) {
    if (g_cfg.log) g_cfg.log("modules: " + s);
}

Virtual* virtual_of(HMODULE h) {
    for (auto& v : g_virtuals)
        if ((HMODULE)v.get() == h) return v.get();
    return nullptr;
}
Virtual* virtual_named(const std::string& b) {
    for (auto& v : g_virtuals)
        if (v->name == b) return v.get();
    return nullptr;
}
Mapped* mapped_of(HMODULE h) {
    for (auto& m : g_mapped)
        if ((HMODULE)pel_base(m->m) == h) return m.get();
    return nullptr;
}
Mapped* mapped_named(const std::string& b) {
    for (auto& m : g_mapped)
        if (m->name == b) return m.get();
    return nullptr;
}
Mapped* mapped_at(const void* p) {
    for (auto& m : g_mapped) {
        auto* base = static_cast<const char*>(pel_base(m->m));
        if (p >= base && p < base + pel_size(m->m)) return m.get();
    }
    return nullptr;
}

std::wstring in_folder(const char* file) {
    std::wstring f = g_cfg.folder;
    if (!f.empty() && f.back() != L'\\' && f.back() != L'/') f += L'\\';
    return f + widen(file);
}

// A DLL of the package (libc++.dll, dxil.dll, longshot_tls.dll) or the system's, loaded by Windows
HMODULE open_windows(const std::string& b) {
    const std::wstring w = widen(b.c_str());
    if (HMODULE h = GetModuleHandleW(w.c_str())) return h;
    const std::wstring local = in_folder(b.c_str());
    const bool packaged = GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES;
    HMODULE h = nullptr;
    if (packaged && sys().load_packaged) h = sys().load_packaged(w.c_str(), 0);
    if (!h && packaged && sys().load_ex) h = sys().load_ex(local.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);   // (a desktop's test)
    if (!h && !packaged && sys().load_ex) h = sys().load_ex(w.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return h;
}

pel_tls_slot* tls_slot() {
    if (!g_slot_tried) {
        g_slot_tried = true;
        HMODULE h = open_windows("longshot_tls.dll");
        using slot_fn = void (*)(ULONG*, void**, SIZE_T*);
        auto get = h ? (slot_fn)(void*)GetProcAddress(h, "longshot_tls_slot") : nullptr;
        if (get) {
            ULONG index = 0;
            void* data = nullptr;
            SIZE_T capacity = 0;
            get(&index, &data, &capacity);
            g_slot.index = index, g_slot.template_data = data, g_slot.capacity = capacity;
        } else {
            log("no longshot_tls.dll in the package: a module with thread-local data cannot load");
        }
    }
    return g_slot.template_data ? &g_slot : nullptr;
}

void* resolve(void* user, const char* dll, const char* name, unsigned ordinal);

Mapped* map(const std::string& as, const std::wstring& path, std::map<std::string, std::string> alias) {
    char err[512];
    pel_options opt{};
    opt.resolve = resolve;
    opt.tls = tls_slot();
    pel_module* m = pel_load_file(path.c_str(), &opt, err, sizeof err);
    if (!m) {
        log(as + " (" + narrow(path.c_str()) + ") did not load: " + err);
        return nullptr;
    }
    auto e = std::make_unique<Mapped>();
    e->name = as, e->path = path, e->m = m, e->alias = std::move(alias);
    g_mapped.push_back(std::move(e));
    char line[160];
    snprintf(line, sizeof line, "%s loaded: %zu KB at %p", as.c_str(), pel_size(m) >> 10, pel_base(m));
    log(line);
    return g_mapped.back().get();
}

HMODULE open_module(const char* name) {
    if (!name) return nullptr;
    std::lock_guard lk(g_mu);
    const std::string b = base_name(name);
    if (Virtual* v = virtual_named(b)) return (HMODULE)v;
    if (Mapped* m = mapped_named(b)) return (HMODULE)pel_base(m->m);
    if (b == "vulkan-1.dll") {   // (Dozen, the Vulkan driver, in the loader's place)
        Mapped* m = map(b, in_folder("vulkan_dzn.dll"), {{"vkGetInstanceProcAddr", "vk_icdGetInstanceProcAddr"}});
        if (!m) return nullptr;
        if (auto negotiate = (int (*)(uint32_t*))pel_symbol(m->m, "vk_icdNegotiateLoaderICDInterfaceVersion")) {
            uint32_t version = 5;
            negotiate(&version);
        }
        return (HMODULE)pel_base(m->m);
    }
    if (b == "longshot_vk.dll") {
        Mapped* m = map(b, in_folder("longshot_vk.dll"), {});
        return m ? (HMODULE)pel_base(m->m) : nullptr;
    }
    return open_windows(b);
}

// ---- the loader functions the modules import ----

HMODULE WINAPI my_LoadLibraryA(LPCSTR name) {
    HMODULE h = open_module(name);
    if (!h) SetLastError(ERROR_MOD_NOT_FOUND);
    return h;
}
HMODULE WINAPI my_LoadLibraryW(LPCWSTR name) { return my_LoadLibraryA(narrow(name).c_str()); }
HMODULE WINAPI my_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    (void)file;
    if (flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    return my_LoadLibraryA(name);
}
HMODULE WINAPI my_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) { return my_LoadLibraryExA(narrow(name).c_str(), file, flags); }

FARPROC WINAPI my_GetProcAddress(HMODULE h, LPCSTR name) {
    const bool by_name = (ULONG_PTR)name > 0xFFFF;
    {
        std::lock_guard lk(g_mu);
        if (Virtual* v = virtual_of(h)) return by_name ? (FARPROC)v->lookup(name) : nullptr;
        if (Mapped* m = mapped_of(h)) {
            if (!by_name) return nullptr;
            auto a = m->alias.find(name);
            return (FARPROC)pel_symbol(m->m, a != m->alias.end() ? a->second.c_str() : name);
        }
    }
    return GetProcAddress(h, name);
}

BOOL WINAPI my_FreeLibrary(HMODULE h) {
    {
        std::lock_guard lk(g_mu);
        if (virtual_of(h) || mapped_of(h)) return TRUE;   // (they stay loaded until the app ends)
    }
    return sys().free_library ? sys().free_library(h) : FALSE;
}

HMODULE WINAPI my_GetModuleHandleA(LPCSTR name) {
    if (name) {
        std::lock_guard lk(g_mu);
        const std::string b = base_name(name);
        if (Virtual* v = virtual_named(b)) return (HMODULE)v;
        if (Mapped* m = mapped_named(b)) return (HMODULE)pel_base(m->m);
    }
    return GetModuleHandleW(name ? widen(name).c_str() : nullptr);
}
HMODULE WINAPI my_GetModuleHandleW(LPCWSTR name) {
    if (!name) return GetModuleHandleW(nullptr);
    return my_GetModuleHandleA(narrow(name).c_str());
}

BOOL WINAPI my_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* out) {
    if (!out) return FALSE;
    {
        std::lock_guard lk(g_mu);
        if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
            if (Mapped* m = mapped_at(name)) return *out = (HMODULE)pel_base(m->m), TRUE;
        } else if (name) {
            const std::string b = base_name(narrow(name));
            if (Mapped* m = mapped_named(b)) return *out = (HMODULE)pel_base(m->m), TRUE;
            if (Virtual* v = virtual_named(b)) return *out = (HMODULE)v, TRUE;
        }
    }
    return sys().handle_ex ? sys().handle_ex(flags, name, out) : FALSE;
}
BOOL WINAPI my_GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) return my_GetModuleHandleExW(flags, (LPCWSTR)name, out);
    return my_GetModuleHandleExW(flags, name ? widen(name).c_str() : nullptr, out);
}

DWORD WINAPI my_GetModuleFileNameW(HMODULE h, LPWSTR out, DWORD size) {
    std::wstring path;
    {
        std::lock_guard lk(g_mu);
        if (Mapped* m = mapped_of(h)) path = m->path;
    }
    if (path.empty()) return sys().file_name ? sys().file_name(h, out, size) : 0;
    if (!size) return 0;
    const DWORD n = DWORD(path.size()) < size ? DWORD(path.size()) : size - 1;
    memcpy(out, path.c_str(), n * sizeof(wchar_t));
    out[n] = 0;
    if (n < path.size()) SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return n;
}
DWORD WINAPI my_GetModuleFileNameA(HMODULE h, LPSTR out, DWORD size) {
    wchar_t w[MAX_PATH * 2];
    DWORD n = my_GetModuleFileNameW(h, w, MAX_PATH * 2);
    if (!n || !size) return 0;
    w[n < MAX_PATH * 2 ? n : MAX_PATH * 2 - 1] = 0;
    const std::string s = narrow(w);
    const DWORD k = DWORD(s.size()) < size ? DWORD(s.size()) : size - 1;
    memcpy(out, s.c_str(), k);
    out[k] = 0;
    return k;
}

const struct {
    const char* name;
    void* fn;
} kLoader[] = {
    {"LoadLibraryA", (void*)my_LoadLibraryA},
    {"LoadLibraryW", (void*)my_LoadLibraryW},
    {"LoadLibraryExA", (void*)my_LoadLibraryExA},
    {"LoadLibraryExW", (void*)my_LoadLibraryExW},
    {"GetProcAddress", (void*)my_GetProcAddress},
    {"FreeLibrary", (void*)my_FreeLibrary},
    {"GetModuleHandleA", (void*)my_GetModuleHandleA},
    {"GetModuleHandleW", (void*)my_GetModuleHandleW},
    {"GetModuleHandleExA", (void*)my_GetModuleHandleExA},
    {"GetModuleHandleExW", (void*)my_GetModuleHandleExW},
    {"GetModuleFileNameA", (void*)my_GetModuleFileNameA},
    {"GetModuleFileNameW", (void*)my_GetModuleFileNameW},
};

// (the DLLs whose functions the console may lack: the game module's stand-ins first, then the system's, then nothing)
bool desktop_dll(const std::string& d) {
    for (const char* p : {"user32", "gdi32", "shell32", "version", "winmm", "dbghelp", "advapi32", "imm32", "comdlg32", "shlwapi"})
        if (starts(d, p) && d.size() > strlen(p) && d[strlen(p)] == '.') return true;
    return false;
}
bool kernel_dll(const std::string& d) {
    return starts(d, "kernel32.") || starts(d, "kernelbase.") || starts(d, "api-ms-win-core-libraryloader-");
}

INT_PTR WINAPI missing() { return 0; }

void* resolve(void* user, const char* dll, const char* name, unsigned ordinal) {
    (void)user, (void)ordinal;
    if (!dll || !name) return nullptr;
    const std::string d = base_name(dll);
    if (kernel_dll(d))
        for (const auto& f : kLoader)
            if (!strcmp(f.name, name)) return f.fn;
    std::lock_guard lk(g_mu);
    if (Virtual* v = virtual_named(d)) return v->lookup(name);
    if (Mapped* m = mapped_named(d)) return pel_symbol(m->m, name);
    if (desktop_dll(d) && g_win32)
        if (void* f = g_win32(name)) return f;
    HMODULE h = open_windows(d);
    if (void* f = h ? (void*)GetProcAddress(h, name) : nullptr) return f;
    if (kernel_dll(d) && g_win32)
        if (void* f = g_win32(name)) return f;
    if (desktop_dll(d) || kernel_dll(d)) {
        if (g_logged.insert(d + "!" + name).second) log(d + "!" + name + " is not on this console: it returns 0");
        return (void*)missing;
    }
    return nullptr;   // (the loader's own lookup, then its error naming the import)
}

}  // namespace

void configure(const Config& c) {
    std::lock_guard lk(g_mu);
    g_cfg = c;
}

void add_virtual(const char* dll, Lookup lookup) {
    std::lock_guard lk(g_mu);
    auto v = std::make_unique<Virtual>();
    v->name = base_name(dll), v->lookup = lookup;
    g_virtuals.push_back(std::move(v));
}

void set_win32(Lookup lookup) {
    std::lock_guard lk(g_mu);
    g_win32 = lookup;
}

pel_module* load(const std::wstring& path, char* err, size_t errlen) {
    std::lock_guard lk(g_mu);
    pel_options opt{};
    opt.resolve = resolve;
    opt.tls = nullptr;   // (the game module: built with -femulated-tls)
    return pel_load_file(path.c_str(), &opt, err, errlen);
}

void* load_library(const char* name) { return my_LoadLibraryA(name); }
void* proc_address(void* module, const char* name) { return (void*)my_GetProcAddress((HMODULE)module, name); }

std::string where(const char* name) {
    std::lock_guard lk(g_mu);
    const std::string b = base_name(name ? name : "");
    if (virtual_named(b)) return "virtual";
    if (Mapped* m = mapped_named(b)) return "pel " + narrow(m->path.c_str());
    if (b == "vulkan-1.dll") return "pel " + narrow(in_folder("vulkan_dzn.dll").c_str());
    if (b == "longshot_vk.dll") return "pel " + narrow(in_folder("longshot_vk.dll").c_str());
    return GetFileAttributesW(in_folder(b.c_str()).c_str()) != INVALID_FILE_ATTRIBUTES ? "package" : "system";
}

}  // namespace longshot_xbox::modules
