// The loader's test module, built like the game module (llvm-mingw, -femulated-tls): one export for each thing the
// loader must get right.
#include <stdexcept>

extern "C" __declspec(dllimport) int host_add(int a, int b);

static const char *const names[] = {"zero", "one", "two"};   // (a table of pointers: base relocations)
static int ctor_value;
static struct Init { Init() { ctor_value = 42; } } init;     // (the CRT's start-up runs constructors)
static thread_local int tls_count;                           // (emulated TLS: TlsAlloc, no TLS directory data)

extern "C" __declspec(dllexport) int mod_add(int a, int b) { return a + b; }
extern "C" __declspec(dllexport) const char *mod_name(int i) { return names[i]; }
extern "C" __declspec(dllexport) int mod_ctor_value() { return ctor_value; }
extern "C" __declspec(dllexport) void mod_set_ctor_value(int v) { ctor_value = v; }
extern "C" __declspec(dllexport) int mod_tls_bump() { return ++tls_count; }
extern "C" __declspec(dllexport) int mod_host_call(int a, int b) { return host_add(a, b) * 2; }

extern "C" __declspec(dllexport) int mod_throw_catch(int v) {   // (unwinding needs the registered exception table)
    try {
        if (v) throw std::runtime_error("thrown in the module");
        return 0;
    } catch (const std::exception &) {
        return v + 1;
    }
}
