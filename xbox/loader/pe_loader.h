/* Loads a 64-bit PE DLL (the game module the console builds, longshot_game.dll) into memory the app allocates itself,
 * where Windows' loader will not go: a UWP app may not LoadLibrary a DLL from the USB drive or LocalState, but with the
 * codeGeneration capability it may make its own memory executable (VirtualAllocFromApp / VirtualProtectFromApp).
 *
 * What it does: map the headers and sections, apply base relocations, resolve imports (through the caller's resolver
 * first, then the DLLs already loaded, the package's and the system's), set each section's protection, register the
 * exception table (RtlAddFunctionTable, or an exception through the module ends the app), run the TLS callbacks and
 * DllMain. What it refuses: thread-local data (Windows' per-thread TLS array has no slot for a module it did not load:
 * build the module with -femulated-tls), delay-load imports, other machines than x64.
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

typedef struct pel_options {
    pel_resolve_fn resolve;
    void *user;
    int avoid_preferred_base;   /* (tests: load away from the image base, so the relocations are exercised) */
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
