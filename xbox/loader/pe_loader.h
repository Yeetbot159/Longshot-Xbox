/* Loads a 64-bit PE DLL (the game module the console builds, longshot_game.dll) into memory the app allocates itself,
 * where Windows' loader will not go: a UWP app may not LoadLibrary a DLL from the USB drive or LocalState, but with the
 * codeGeneration capability it may make its own memory executable (VirtualAllocFromApp / VirtualProtectFromApp).
 *
 * What it does: map the headers and sections, apply base relocations, resolve imports (through the caller's resolver
 * first, then the DLLs already loaded, the package's and the system's), set each section's protection, register the
 * exception table (RtlAddFunctionTable, or an exception through the module ends the app), run the TLS callbacks and
 * DllMain. What it refuses: thread-local data without a slot to put it in (Windows' per-thread TLS array has no slot
 * for a module it did not load: build the module with -femulated-tls, or give it a pel_tls_slot), delay-load imports,
 * other machines than x64.
 *
 * The same file builds into a desktop program (its test, xbox/loader/test) and into the UWP host: every system function
 * past GetModuleHandleW / GetProcAddress is looked up at run time. */
#pragma once
#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pel_module pel_module;

/* An import: its DLL's name as the module names it, and the function's name, or NULL and its ordinal. Returns the
 * address, or NULL for the default lookup. */
typedef void *(*pel_resolve_fn)(void *user, const char *dll, const char *name, unsigned ordinal);

/* A thread-local slot of Windows' own for a module whose thread-local data cannot be emulated (one built with MSVC:
 * Mesa's Dozen): a DLL Windows loaded whose TLS block is one buffer and nothing else (xbox/app/tls_slot.c). The module
 * gets its index, and its initial data goes into the slot DLL's template, which Windows copies into each new thread's
 * block; the loading thread's own block is filled at load. A thread that existed before the load, other than the one
 * loading, finds zeros there instead: load such a module before starting the threads that call it. One module a slot. */
typedef struct pel_tls_slot {
    unsigned long index;   /* the slot DLL's _tls_index */
    void *template_data;   /* its template (StartAddressOfRawData), writable */
    size_t capacity;       /* bytes of it */
    int taken;             /* (the loader's: a module is using it) */
} pel_tls_slot;

typedef struct pel_options {
    pel_resolve_fn resolve;
    void *user;
    int avoid_preferred_base;   /* (tests: load away from the image base, so the relocations are exercised) */
    pel_tls_slot *tls;          /* for a module with thread-local data; NULL refuses one */
} pel_options;

/* NULL on failure, with the reason in err. */
pel_module *pel_load_file(const wchar_t *path, const pel_options *opt, char *err, size_t errlen);
pel_module *pel_load_memory(const void *image, size_t size, const pel_options *opt, char *err, size_t errlen);

void *pel_symbol(pel_module *m, const char *name);
void *pel_base(pel_module *m);
size_t pel_size(pel_module *m);
void pel_unload(pel_module *m);

#ifdef __cplusplus
}
#endif
