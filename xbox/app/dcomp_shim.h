// DComp.DLL as the app answers it (xbox/app/modules.h add_virtual). Mesa's Dozen presents through DirectComposition
// (src/vulkan/wsi/wsi_common_win32.cpp): it makes a swap chain with CreateSwapChainForComposition, then
// DCompositionCreateDevice, CreateTargetForHwnd, CreateVisual, SetRoot, SetContent(swap chain) and Commit. DirectComposition
// is the desktop's (dcomp.h is WINAPI_PARTITION_DESKTOP); a UWP app has Windows.UI.Composition instead. These objects
// answer those calls and nothing else, and at each Commit hand the target's swap chain to the app (present.h), which
// shows it on the CoreWindow.
//
// Portable: no WinRT, so the PC test (xbox/app/test/test_dozen.cpp) and the PC runner (xbox/app/test/pc_run.cpp) present
// through it on a desktop GPU.
#pragma once
#include <windows.h>
#include <unknwn.h>

namespace longshot_xbox::dcomp {

// The window handle Dozen's surface is made with: no window, a value Dozen only keeps, to give back to GetClientRect (the
// game module's, host/uwp/win32_uwp.c: the platform's window size) and to CreateTargetForHwnd here
inline const HWND kWindowToken = reinterpret_cast<HWND>(uintptr_t(0x4C53));

// The renderer's surface (LongshotUwpPlatform create_vk_surface): vkCreateWin32SurfaceKHR of the vulkan-1.dll the
// module set gives out (Dozen) on kWindowToken. 1 if made
int create_vk_surface(void* instance, void* surface_out);

// What a Commit shows: the swap chain Dozen set on its visual (an IDXGISwapChain1), or nullptr once it took it away. Called
// on the committing thread (the game's render thread), only when it changed.
using Show = void (*)(IUnknown* swapchain);
void set_show(Show show);

// The DLL's exports by name (DCompositionCreateDevice), for modules::add_virtual("dcomp.dll", lookup)
void* lookup(const char* name);

}  // namespace longshot_xbox::dcomp
