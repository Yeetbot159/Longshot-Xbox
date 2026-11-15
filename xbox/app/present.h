// What the CoreWindow shows: a Windows.UI.Composition tree with the launcher's picture at the bottom and the game's on top.
// Both are swap chains made for composition: the launcher's (Direct2D on Direct3D 11) and the game's, which Dozen makes
// on Direct3D 12 and hands over through DComp.DLL (dcomp_shim.h).
#pragma once
#include <unknwn.h>
#include <winrt/Windows.UI.Core.h>

namespace longshot_xbox::present {

// On the window's thread, once, before the others
void init(winrt::Windows::UI::Core::CoreWindow const& window);

// The launcher's swap chain (IDXGISwapChain1, CreateSwapChainForComposition); on the window's thread
void set_launcher(IUnknown* swapchain);

// The game's, or nullptr to take it away. Any thread: done on the window's, waited for (dcomp::Show)
void set_game(IUnknown* swapchain);

// Whether the game's picture is up (the launcher stops drawing under it)
bool game_shown();

}  // namespace longshot_xbox::present
