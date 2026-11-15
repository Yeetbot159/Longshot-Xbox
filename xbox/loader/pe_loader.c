/* The game-module loader: pe_loader.h says what and why. */
#include "pe_loader.h"

#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- system functions, looked up at run time (the UWP import library lacks the desktop ones) ---- */

typedef PVOID(WINAPI *VirtualAllocFromApp_t)(PVOID, SIZE_T, ULONG, ULONG);
typedef BOOL(WINAPI *VirtualProtectFromApp_t)(PVOID, SIZE_T, ULONG, PULONG);
typedef LPVOID(WINAPI *VirtualAlloc_t)(LPVOID, SIZE_T, DWORD, DWORD);
typedef BOOL(WINAPI *VirtualProtect_t)(LPVOID, SIZE_T, DWORD, PDWORD);
typedef BOOL(WINAPI *VirtualFree_t)(LPVOID, SIZE_T, DWORD);
typedef BOOLEAN(WINAPI *RtlAddFunctionTable_t)(PRUNTIME_FUNCTION, DWORD, DWORD64);
typedef BOOLEAN(WINAPI *RtlDeleteFunctionTable_t)(PRUNTIME_FUNCTION);
typedef HMODULE(WINAPI *LoadPackagedLibrary_t)(LPCWSTR, DWORD);
typedef HMODULE(WINAPI *LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
typedef BOOL(WINAPI *FreeLibrary_t)(HMODULE);
typedef BOOL(WINAPI *FlushInstructionCache_t)(HANDLE, LPCVOID, SIZE_T);
typedef BOOL(WINAPI *DllMain_t)(HINSTANCE, DWORD, LPVOID);

static struct {
    int ready;
    VirtualAllocFromApp_t alloc_app;
    VirtualProtectFromApp_t protect_app;
    VirtualAlloc_t alloc;
    VirtualProtect_t protect;
    VirtualFree_t free_;
    RtlAddFunctionTable_t add_table;
    RtlDeleteFunctionTable_t delete_table;
    LoadPackagedLibrary_t load_packaged;
    LoadLibraryExW_t load_ex;
    FreeLibrary_t free_library;
    FlushInstructionCache_t flush_icache;
} sys;

static FARPROC find_sys(const char *name) {
    static const wchar_t *dlls[] = {L"kernelbase.dll", L"kernel32.dll", L"ntdll.dll"};
    for (size_t i = 0; i < sizeof dlls / sizeof *dlls; ++i) {
        HMODULE h = GetModuleHandleW(dlls[i]);
        FARPROC p = h ? GetProcAddress(h, name) : NULL;
        if (p) return p;
    }
    return NULL;
}

static void sys_init(void) {
    if (sys.ready) return;
    sys.alloc_app = (VirtualAllocFromApp_t)find_sys("VirtualAllocFromApp");
    sys.protect_app = (VirtualProtectFromApp_t)find_sys("VirtualProtectFromApp");
    sys.alloc = (VirtualAlloc_t)find_sys("VirtualAlloc");
    sys.protect = (VirtualProtect_t)find_sys("VirtualProtect");
    sys.free_ = (VirtualFree_t)find_sys("VirtualFree");
    sys.add_table = (RtlAddFunctionTable_t)find_sys("RtlAddFunctionTable");
    sys.delete_table = (RtlDeleteFunctionTable_t)find_sys("RtlDeleteFunctionTable");
    sys.load_packaged = (LoadPackagedLibrary_t)find_sys("LoadPackagedLibrary");
    sys.load_ex = (LoadLibraryExW_t)find_sys("LoadLibraryExW");
    sys.free_library = (FreeLibrary_t)find_sys("FreeLibrary");
    sys.flush_icache = (FlushInstructionCache_t)find_sys("FlushInstructionCache");
    sys.ready = 1;
}

static void *mem_alloc(void *at, size_t size) {
    if (sys.alloc_app) return sys.alloc_app(at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return sys.alloc ? sys.alloc(at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : NULL;
}

static int mem_protect(void *at, size_t size, DWORD prot) {
    ULONG old_app;
    DWORD old;
    if (sys.protect_app) return sys.protect_app(at, size, prot, &old_app) != 0;
    return sys.protect && sys.protect(at, size, prot, &old);
}

/* ---- the module ---- */

#define PEL_MAX_DLLS 64

struct pel_module {
    uint8_t *base;
    size_t size;
    IMAGE_NT_HEADERS64 *nt;
    PRUNTIME_FUNCTION table;
    int attached;
    HMODULE dlls[PEL_MAX_DLLS];
    int ndlls;
};

typedef struct {
    int (*read)(void *src, uint64_t offset, void *dst, size_t size);
    void *src;
    uint64_t size;
} reader;

static void fail(char *err, size_t errlen, const char *f, ...) {
    if (!err || !errlen) return;
    va_list ap;
    va_start(ap, f);
    vsnprintf(err, errlen, f, ap);
    va_end(ap);
}

static int in_image(const pel_module *m, uint64_t rva, uint64_t size) {
    return rva <= m->size && size <= m->size - rva;
}

static IMAGE_DATA_DIRECTORY *dir(pel_module *m, int which) {
    if ((DWORD)which >= m->nt->OptionalHeader.NumberOfRvaAndSizes) return NULL;
    IMAGE_DATA_DIRECTORY *d = &m->nt->OptionalHeader.DataDirectory[which];
    return d->VirtualAddress && d->Size && in_image(m, d->VirtualAddress, d->Size) ? d : NULL;
}

static HMODULE open_dll(pel_module *m, const char *name) {
    wchar_t w[MAX_PATH];
    size_t n = strlen(name);
    if (n >= MAX_PATH) return NULL;
    for (size_t i = 0; i <= n; ++i) w[i] = (wchar_t)(unsigned char)name[i];
    HMODULE h = GetModuleHandleW(w);
    if (h) return h;
    if (sys.load_packaged) h = sys.load_packaged(w, 0);
    if (!h && sys.load_ex) h = sys.load_ex(w, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (h && m->ndlls < PEL_MAX_DLLS) m->dlls[m->ndlls++] = h;
    return h;
}

static void *export_of(pel_module *m, const char *name, unsigned ordinal);

static void *default_import(pel_module *m, const char *dll, const char *name, unsigned ordinal) {
    HMODULE h = open_dll(m, dll);
    if (!h) return NULL;
    return (void *)GetProcAddress(h, name ? name : (LPCSTR)(uintptr_t)ordinal);
}

static int map_image(pel_module *m, const reader *r, const pel_options *opt, char *err, size_t errlen) {
    IMAGE_DOS_HEADER dos;
    if (r->size < sizeof dos || !r->read(r->src, 0, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return fail(err, errlen, "not a PE file (no MZ header)"), 0;
    IMAGE_NT_HEADERS64 nt;
    if ((uint64_t)dos.e_lfanew + sizeof nt > r->size || !r->read(r->src, (uint64_t)dos.e_lfanew, &nt, sizeof nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE)
        return fail(err, errlen, "not a PE file (no PE header)"), 0;
    if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return fail(err, errlen, "not an x64 module (machine 0x%04x)", nt.FileHeader.Machine), 0;
    if (!(nt.FileHeader.Characteristics & IMAGE_FILE_DLL)) return fail(err, errlen, "not a DLL"), 0;
    const IMAGE_OPTIONAL_HEADER64 *oh = &nt.OptionalHeader;
    if (oh->SizeOfHeaders > oh->SizeOfImage || oh->SizeOfHeaders > r->size) return fail(err, errlen, "bad header size"), 0;

    m->size = oh->SizeOfImage;
    if (!opt || !opt->avoid_preferred_base) m->base = (uint8_t *)mem_alloc((void *)(uintptr_t)oh->ImageBase, m->size);
    if (!m->base) m->base = (uint8_t *)mem_alloc(NULL, m->size);
    if (opt && opt->avoid_preferred_base && m->base == (uint8_t *)(uintptr_t)oh->ImageBase) {
        uint8_t *other = (uint8_t *)mem_alloc(NULL, m->size);
        sys.free_(m->base, 0, MEM_RELEASE);
        m->base = other;
    }
    if (!m->base) return fail(err, errlen, "cannot allocate %zu MB for the image (%lu)", m->size >> 20, GetLastError()), 0;

    if (!r->read(r->src, 0, m->base, oh->SizeOfHeaders)) return fail(err, errlen, "cannot read the headers"), 0;
    m->nt = (IMAGE_NT_HEADERS64 *)(m->base + dos.e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(m->nt);
    if (!in_image(m, (uint64_t)((uint8_t *)(s + m->nt->FileHeader.NumberOfSections) - m->base), 0))
        return fail(err, errlen, "bad section table"), 0;
    for (WORD i = 0; i < m->nt->FileHeader.NumberOfSections; ++i, ++s) {
        uint64_t n = s->SizeOfRawData;
        if (s->Misc.VirtualSize && s->Misc.VirtualSize < n) n = s->Misc.VirtualSize;
        if (!n) continue;
        if (!in_image(m, s->VirtualAddress, n) || (uint64_t)s->PointerToRawData + n > r->size)
            return fail(err, errlen, "section %.8s lies outside the file or image", (const char *)s->Name), 0;
        if (!r->read(r->src, s->PointerToRawData, m->base + s->VirtualAddress, (size_t)n))
            return fail(err, errlen, "cannot read section %.8s", (const char *)s->Name), 0;
    }
    return 1;
}

static int relocate(pel_module *m, char *err, size_t errlen) {
    int64_t delta = (int64_t)((uint64_t)(uintptr_t)m->base - m->nt->OptionalHeader.ImageBase);
    if (!delta) return 1;
    IMAGE_DATA_DIRECTORY *d = dir(m, IMAGE_DIRECTORY_ENTRY_BASERELOC);
    if (!d || (m->nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED))
        return fail(err, errlen, "the module has no relocations and its image base is taken"), 0;
    uint8_t *p = m->base + d->VirtualAddress, *end = p + d->Size;
    while (p + sizeof(IMAGE_BASE_RELOCATION) <= end) {
        IMAGE_BASE_RELOCATION *b = (IMAGE_BASE_RELOCATION *)p;
        if (b->SizeOfBlock < sizeof *b || p + b->SizeOfBlock > end) return fail(err, errlen, "bad relocation block"), 0;
        WORD *e = (WORD *)(b + 1);
        size_t n = (b->SizeOfBlock - sizeof *b) / sizeof(WORD);
        for (size_t i = 0; i < n; ++i) {
            unsigned type = e[i] >> 12, off = e[i] & 0xfff;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            uint64_t rva = (uint64_t)b->VirtualAddress + off;
            if (type == IMAGE_REL_BASED_DIR64 && in_image(m, rva, 8)) {
                uint64_t v;
                memcpy(&v, m->base + rva, 8);
                v += (uint64_t)delta;
                memcpy(m->base + rva, &v, 8);
            } else if (type == IMAGE_REL_BASED_HIGHLOW && in_image(m, rva, 4)) {
                uint32_t v;
                memcpy(&v, m->base + rva, 4);
                v += (uint32_t)delta;
                memcpy(m->base + rva, &v, 4);
            } else {
                return fail(err, errlen, "relocation type %u at 0x%llx not handled", type, (unsigned long long)rva), 0;
            }
        }
        p += b->SizeOfBlock;
    }
    return 1;
}

static const char *str_at(pel_module *m, DWORD rva) {
    if (!in_image(m, rva, 1)) return NULL;
    const char *s = (const char *)m->base + rva;
    return memchr(s, 0, m->size - rva) ? s : NULL;
}

static int bind_imports(pel_module *m, const pel_options *opt, char *err, size_t errlen) {
    if (dir(m, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)) return fail(err, errlen, "delay-load imports are not handled"), 0;
    IMAGE_DATA_DIRECTORY *d = dir(m, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (!d) return 1;
    for (IMAGE_IMPORT_DESCRIPTOR *id = (IMAGE_IMPORT_DESCRIPTOR *)(m->base + d->VirtualAddress);
         in_image(m, (uint64_t)((uint8_t *)(id + 1) - m->base), 0) && id->Name; ++id) {
        const char *dll = str_at(m, id->Name);
        if (!dll) return fail(err, errlen, "bad import name"), 0;
        DWORD lookup_rva = id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk;
        for (size_t i = 0;; ++i) {
            uint64_t lrva = lookup_rva + i * 8, irva = id->FirstThunk + i * 8;
            if (!in_image(m, lrva, 8) || !in_image(m, irva, 8)) return fail(err, errlen, "bad import table of %s", dll), 0;
            uint64_t t;
            memcpy(&t, m->base + lrva, 8);
            if (!t) break;
            const char *name = NULL;
            unsigned ordinal = 0;
            if (IMAGE_SNAP_BY_ORDINAL64(t)) {
                ordinal = (unsigned)IMAGE_ORDINAL64(t);
            } else {
                name = str_at(m, (DWORD)t + 2);   /* (IMAGE_IMPORT_BY_NAME: a 2-byte hint, then the name) */
                if (!name) return fail(err, errlen, "bad import entry of %s", dll), 0;
            }
            void *fn = opt && opt->resolve ? opt->resolve(opt->user, dll, name, ordinal) : NULL;
            if (!fn) fn = default_import(m, dll, name, ordinal);
            if (!fn) {
                if (name) return fail(err, errlen, "unresolved import %s!%s", dll, name), 0;
                return fail(err, errlen, "unresolved import %s!#%u", dll, ordinal), 0;
            }
            uint64_t v = (uint64_t)(uintptr_t)fn;
            memcpy(m->base + irva, &v, 8);
        }
    }
    return 1;
}

static int check_tls(pel_module *m, char *err, size_t errlen) {
    IMAGE_DATA_DIRECTORY *d = dir(m, IMAGE_DIRECTORY_ENTRY_TLS);
    if (!d) return 1;
    IMAGE_TLS_DIRECTORY64 *t = (IMAGE_TLS_DIRECTORY64 *)(m->base + d->VirtualAddress);
    uint64_t data = t->EndAddressOfRawData - t->StartAddressOfRawData + t->SizeOfZeroFill;
    if (data) return fail(err, errlen, "the module has %llu bytes of thread-local data: build it with -femulated-tls", (unsigned long long)data), 0;
    uint64_t index = t->AddressOfIndex - (uint64_t)(uintptr_t)m->base;
    if (t->AddressOfIndex && in_image(m, index, 4)) memset(m->base + index, 0, 4);
    return 1;
}

static void tls_callbacks(pel_module *m, DWORD reason) {
    IMAGE_DATA_DIRECTORY *d = dir(m, IMAGE_DIRECTORY_ENTRY_TLS);
    if (!d) return;
    IMAGE_TLS_DIRECTORY64 *t = (IMAGE_TLS_DIRECTORY64 *)(m->base + d->VirtualAddress);
    if (!t->AddressOfCallBacks) return;
    for (uint64_t at = t->AddressOfCallBacks - (uint64_t)(uintptr_t)m->base; in_image(m, at, 8); at += 8) {
        uint64_t cb;
        memcpy(&cb, m->base + at, 8);
        if (!cb) break;
        ((PIMAGE_TLS_CALLBACK)(uintptr_t)cb)(m->base, reason, NULL);
    }
}

static int protect_sections(pel_module *m, char *err, size_t errlen) {
    DWORD page = 4096, align = m->nt->OptionalHeader.SectionAlignment;
    if (align < page) align = page;
    if (!mem_protect(m->base, m->nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY))
        return fail(err, errlen, "cannot protect the headers (%lu)", GetLastError()), 0;
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(m->nt);
    for (WORD i = 0; i < m->nt->FileHeader.NumberOfSections; ++i, ++s) {
        uint64_t n = s->Misc.VirtualSize > s->SizeOfRawData ? s->Misc.VirtualSize : s->SizeOfRawData;
        n = (n + align - 1) & ~(uint64_t)(align - 1);
        if (!n) continue;
        if (n > m->size - s->VirtualAddress) n = m->size - s->VirtualAddress;
        DWORD c = s->Characteristics, prot;
        int x = (c & IMAGE_SCN_MEM_EXECUTE) != 0, w = (c & IMAGE_SCN_MEM_WRITE) != 0;
        prot = x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (w ? PAGE_READWRITE : PAGE_READONLY);
        if (!mem_protect(m->base + s->VirtualAddress, (size_t)n, prot))
            return fail(err, errlen, "cannot make section %.8s %s (%lu)%s", (const char *)s->Name, x ? "executable" : "read-only",
                        GetLastError(), x ? ": the app needs the codeGeneration capability" : ""), 0;
    }
    if (sys.flush_icache) sys.flush_icache(GetCurrentProcess(), m->base, m->size);
    return 1;
}

static int register_unwind(pel_module *m, char *err, size_t errlen) {
    IMAGE_DATA_DIRECTORY *d = dir(m, IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    if (!d) return 1;
    if (!sys.add_table) return fail(err, errlen, "RtlAddFunctionTable is missing: exceptions could not unwind through the module"), 0;
    PRUNTIME_FUNCTION f = (PRUNTIME_FUNCTION)(m->base + d->VirtualAddress);
    if (!sys.add_table(f, d->Size / sizeof(RUNTIME_FUNCTION), (DWORD64)(uintptr_t)m->base))
        return fail(err, errlen, "RtlAddFunctionTable refused the module's table"), 0;
    m->table = f;
    return 1;
}

static pel_module *load(const reader *r, const pel_options *opt, char *err, size_t errlen) {
    sys_init();
    if (err && errlen) err[0] = 0;
    pel_module *m = (pel_module *)calloc(1, sizeof *m);
    if (!m) return fail(err, errlen, "out of memory"), NULL;
    if (map_image(m, r, opt, err, errlen) && relocate(m, err, errlen) && bind_imports(m, opt, err, errlen) &&
        check_tls(m, err, errlen) && protect_sections(m, err, errlen) && register_unwind(m, err, errlen)) {
        tls_callbacks(m, DLL_PROCESS_ATTACH);
        DWORD entry = m->nt->OptionalHeader.AddressOfEntryPoint;
        if (!entry || ((DllMain_t)(void *)(m->base + entry))((HINSTANCE)m->base, DLL_PROCESS_ATTACH, NULL)) {
            m->attached = 1;
            return m;
        }
        fail(err, errlen, "the module's DllMain failed");
        tls_callbacks(m, DLL_PROCESS_DETACH);
    }
    pel_unload(m);
    return NULL;
}

static int read_memory(void *src, uint64_t offset, void *dst, size_t size) {
    memcpy(dst, (const uint8_t *)src + offset, size);
    return 1;
}

static int read_file(void *src, uint64_t offset, void *dst, size_t size) {
    HANDLE h = (HANDLE)src;
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, at, NULL, FILE_BEGIN)) return 0;
    while (size) {
        DWORD want = size > (1u << 30) ? (1u << 30) : (DWORD)size, got = 0;
        if (!ReadFile(h, dst, want, &got, NULL) || !got) return 0;
        dst = (uint8_t *)dst + got;
        size -= got;
    }
    return 1;
}

pel_module *pel_load_memory(const void *image, size_t size, const pel_options *opt, char *err, size_t errlen) {
    reader r = {read_memory, (void *)image, size};
    return load(&r, opt, err, errlen);
}

pel_module *pel_load_file(const wchar_t *path, const pel_options *opt, char *err, size_t errlen) {
    HANDLE h = CreateFile2(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
    if (h == INVALID_HANDLE_VALUE) return fail(err, errlen, "cannot open the module (%lu)", GetLastError()), NULL;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return fail(err, errlen, "cannot size the module (%lu)", GetLastError()), NULL;
    }
    reader r = {read_file, h, (uint64_t)size.QuadPart};
    pel_module *m = load(&r, opt, err, errlen);
    CloseHandle(h);
    return m;
}

static void *export_of(pel_module *m, const char *name, unsigned ordinal) {
    IMAGE_DATA_DIRECTORY *d = dir(m, IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!d) return NULL;
    IMAGE_EXPORT_DIRECTORY *e = (IMAGE_EXPORT_DIRECTORY *)(m->base + d->VirtualAddress);
    if (!in_image(m, e->AddressOfFunctions, (uint64_t)e->NumberOfFunctions * 4) ||
        !in_image(m, e->AddressOfNames, (uint64_t)e->NumberOfNames * 4) ||
        !in_image(m, e->AddressOfNameOrdinals, (uint64_t)e->NumberOfNames * 2))
        return NULL;
    const DWORD *funcs = (const DWORD *)(m->base + e->AddressOfFunctions);
    const DWORD *names = (const DWORD *)(m->base + e->AddressOfNames);
    const WORD *ords = (const WORD *)(m->base + e->AddressOfNameOrdinals);
    DWORD index = (DWORD)-1;
    if (name) {
        DWORD lo = 0, hi = e->NumberOfNames;   /* (the names are sorted) */
        while (lo < hi) {
            DWORD mid = lo + (hi - lo) / 2;
            const char *s = str_at(m, names[mid]);
            int c = s ? strcmp(name, s) : -1;
            if (!c) { index = ords[mid]; break; }
            if (c < 0) hi = mid; else lo = mid + 1;
        }
    } else {
        index = ordinal - e->Base;
    }
    if (index >= e->NumberOfFunctions || !funcs[index]) return NULL;
    DWORD rva = funcs[index];
    if (rva >= d->VirtualAddress && rva < d->VirtualAddress + d->Size) {   /* (a forwarder, "DLL.Function") */
        const char *fwd = str_at(m, rva), *dot = fwd ? strchr(fwd, '.') : NULL;
        if (!dot || dot - fwd > MAX_PATH - 5) return NULL;
        char dll[MAX_PATH];
        memcpy(dll, fwd, (size_t)(dot - fwd));
        memcpy(dll + (dot - fwd), ".dll", 5);
        return default_import(m, dll, dot[1] == '#' ? NULL : dot + 1, dot[1] == '#' ? (unsigned)atoi(dot + 2) : 0);
    }
    return m->base + rva;
}

void *pel_symbol(pel_module *m, const char *name) { return m && name ? export_of(m, name, 0) : NULL; }
void *pel_base(pel_module *m) { return m ? m->base : NULL; }
size_t pel_size(pel_module *m) { return m ? m->size : 0; }

void pel_unload(pel_module *m) {
    if (!m) return;
    if (m->attached) {
        DWORD entry = m->nt->OptionalHeader.AddressOfEntryPoint;
        if (entry) ((DllMain_t)(void *)(m->base + entry))((HINSTANCE)m->base, DLL_PROCESS_DETACH, NULL);
        tls_callbacks(m, DLL_PROCESS_DETACH);
    }
    if (m->table && sys.delete_table) sys.delete_table(m->table);
    for (int i = 0; i < m->ndlls; ++i)
        if (sys.free_library) sys.free_library(m->dlls[i]);
    if (m->base && sys.free_) sys.free_(m->base, 0, MEM_RELEASE);
    free(m);
}
