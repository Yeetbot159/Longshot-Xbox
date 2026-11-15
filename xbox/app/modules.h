// The modules the app loads by hand (xbox/loader): the game module from the player's drive, the renderer and Mesa's Dozen
// (Vulkan on Direct3D 12) from the package. A UWP app may not LoadLibrary them, and they import desktop functions the
// app container lacks; one resolver answers every import of theirs:
//
//   LoadLibrary*, GetProcAddress, FreeLibrary, GetModuleHandle*, GetModuleFileName*: these modules' own, so the game's
//     LoadLibrary("longshot_vk.dll") and the renderer's LoadLibrary("vulkan-1.dll") load by hand too; vulkan-1.dll is
//     Dozen (vulkan_dzn.dll), its vk_icdGetInstanceProcAddr given out as vkGetInstanceProcAddr, no Vulkan loader between
//   a DLL the app answers itself (add_virtual: DirectComposition, xbox/app/dcomp_shim.cpp; Shell32 and Version are
//     handled here for Dozen's LocalState cache and optional file-version queries)
//   the desktop's window, sound and shell functions: the game module's stand-ins (host/uwp/win32_uwp.c,
//     longshot_uwp_win32), once it is loaded
//   the rest: the system's, or the package's (libc++.dll, dxil.dll); a desktop function that is in none of these
//     returns 0, and is named in the log when bound
//
// Portable: no WinRT, every system function past GetModuleHandleW / GetProcAddress looked up at run time, so the PC test
// (xbox/app/test) drives it with Dozen on a desktop GPU.
#pragma once
#include <functional>
#include <string>

#include "../loader/pe_loader.h"

namespace longshot_xbox::modules {

struct Config {
    std::wstring folder;                              // the package's folder: longshot_vk.dll, vulkan_dzn.dll, longshot_tls.dll
    std::wstring local_state;                         // Mesa's cache path for SHGetFolderPathA(CSIDL_LOCAL_APPDATA)
    std::function<void(const std::string&)> log;      // (may be empty)
};
void configure(const Config& c);

// A DLL the app answers itself: GetProcAddress on it, and imports from it, ask lookup
using Lookup = void* (*)(const char* name);
void add_virtual(const char* dll, Lookup lookup);

// The game module's stand-ins for the desktop's functions (its longshot_uwp_win32 export)
void set_win32(Lookup lookup);

// Loads a module by hand with this resolver; NULL and the reason in err
pel_module* load(const std::wstring& path, char* err, size_t errlen);

// What a module's LoadLibraryA and GetProcAddress do; the app's own calls into these modules go through them too
void* load_library(const char* name);
void* proc_address(void* module, const char* name);

// What a name given to LoadLibrary opens here, for the log and the test: "pel <path>", "virtual", "system", "package"
std::string where(const char* name);

}  // namespace longshot_xbox::modules
