// The app's side of host/uwp/uwp_platform.h: the table the game module is given, made from the CoreWindow, XAudio2 and
// WinRT. The window's events come in on the app's thread (platform_on_*); the game takes them on its own (poll_event).
#pragma once
#include <winrt/Windows.UI.Core.h>

#include <functional>
#include <string>

#include "../../host/uwp/uwp_platform.h"

namespace longshot_xbox {

struct PlatformPaths {
    std::string local_folder, game_folder, work_folder;
};

// The table, valid for the app's life. `note` shows the game's messages over the screen.
const LongshotUwpPlatform* platform_init(winrt::Windows::UI::Core::CoreWindow const& window, const PlatformPaths& paths, uint32_t model,
                                         uint64_t memory_limit, std::function<void(const std::string&)> note);

// The window's thread: what the game must hear.
void platform_on_key(unsigned vk, bool down);
void platform_on_char(uint32_t code_point);
void platform_on_resize(int width, int height);
void platform_on_focus(bool focused);
void platform_on_suspend();
void platform_on_resume();
void platform_on_close();

}  // namespace longshot_xbox
