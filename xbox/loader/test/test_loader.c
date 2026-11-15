/* The loader's test on a PC: load test_mod.dll away from its image base, twice, from a file and from memory, and call
 * every export; and refuse test_tls.dll. Prints one line per check and exits non-zero on a failure. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pe_loader.h"

static int failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        int ok_ = (cond);                                   \
        printf("%s: ", ok_ ? "ok  " : "FAIL");              \
        printf(__VA_ARGS__);                                \
        printf("\n");                                       \
        if (!ok_) ++failures;                               \
    } while (0)

static int host_add_impl(int a, int b) { return a + b + 1000; }

static void *resolve(void *user, const char *dll, const char *name, unsigned ordinal) {
    (void)user, (void)ordinal;
    if (_stricmp(dll, "host.dll") == 0 && name && strcmp(name, "host_add") == 0) return (void *)host_add_impl;
    return NULL;
}

typedef int (*int2_t)(int, int);
typedef int (*int1_t)(int);
typedef int (*int0_t)(void);
typedef void (*set_t)(int);
typedef const char *(*name_t)(int);

static int0_t tls_bump;

static DWORD WINAPI other_thread(LPVOID out) {
    *(int *)out = tls_bump();
    return 0;
}

static void exercise(pel_module *m, const char *label) {
    int2_t add = (int2_t)pel_symbol(m, "mod_add");
    name_t name = (name_t)pel_symbol(m, "mod_name");
    int0_t ctor = (int0_t)pel_symbol(m, "mod_ctor_value");
    int1_t thrown = (int1_t)pel_symbol(m, "mod_throw_catch");
    int2_t host_call = (int2_t)pel_symbol(m, "mod_host_call");
    tls_bump = (int0_t)pel_symbol(m, "mod_tls_bump");
    CHECK(add && name && ctor && thrown && host_call && tls_bump, "%s: every export found", label);
    if (!(add && name && ctor && thrown && host_call && tls_bump)) return;
    CHECK(pel_symbol(m, "mod_missing") == NULL, "%s: a missing export is NULL", label);
    CHECK(add(2, 3) == 5, "%s: mod_add(2, 3) = %d", label, add(2, 3));
    CHECK(strcmp(name(2), "two") == 0, "%s: relocated table, mod_name(2) = %s", label, name(2));
    CHECK(ctor() == 42, "%s: constructor ran, value %d", label, ctor());
    CHECK(host_call(3, 4) == 2014, "%s: import resolved to the host, mod_host_call(3, 4) = %d", label, host_call(3, 4));
    CHECK(thrown(5) == 6, "%s: C++ exception thrown and caught in the module, %d", label, thrown(5));
    int a = tls_bump(), b = tls_bump(), c = 0;
    HANDLE t = CreateThread(NULL, 0, other_thread, &c, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(a == 1 && b == 2 && c == 1, "%s: emulated thread-locals, this thread %d %d, another thread %d", label, a, b, c);
}

static IMAGE_NT_HEADERS64 *headers_of(void *base) {
    return (IMAGE_NT_HEADERS64 *)((char *)base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: test_loader <test_mod.dll> <test_tls.dll>\n");
        return 2;
    }
    wchar_t mod[MAX_PATH], tls[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, mod, MAX_PATH);
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, tls, MAX_PATH);
    char err[512];
    pel_options opt = {resolve, NULL, 1};

    pel_module *m1 = pel_load_file(mod, &opt, err, sizeof err);
    CHECK(m1 != NULL, "load from file%s%s", m1 ? "" : ": ", m1 ? "" : err);
    if (!m1) return 1;
    void *base = pel_base(m1);
    CHECK((unsigned long long)(size_t)base != headers_of(base)->OptionalHeader.ImageBase, "loaded away from the image base (%p)", base);
    exercise(m1, "file");

    FILE *f = _wfopen(mod, L"rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *bytes = malloc((size_t)n);
    fread(bytes, 1, (size_t)n, f);
    fclose(f);
    pel_module *m2 = pel_load_memory(bytes, (size_t)n, &opt, err, sizeof err);
    free(bytes);
    CHECK(m2 != NULL, "second copy from memory%s%s", m2 ? "" : ": ", m2 ? "" : err);
    if (m2) {
        ((set_t)pel_symbol(m1, "mod_set_ctor_value"))(7);
        CHECK(((int0_t)pel_symbol(m2, "mod_ctor_value"))() == 42, "the two copies keep their own data");
        pel_unload(m2);
    }
    pel_unload(m1);

    pel_module *bad = pel_load_file(tls, &opt, err, sizeof err);
    CHECK(bad == NULL && strstr(err, "thread-local"), "a module with thread-local data is refused (%s)", bad ? "loaded" : err);
    if (bad) pel_unload(bad);

    pel_options plain = {NULL, NULL, 0};
    pel_module *unresolved = pel_load_file(mod, &plain, err, sizeof err);
    CHECK(unresolved == NULL && strstr(err, "host_add"), "an unresolved import is named (%s)", unresolved ? "loaded" : err);
    if (unresolved) pel_unload(unresolved);

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
