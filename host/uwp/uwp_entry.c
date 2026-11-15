/* (xbox) The game module's two exports, which the app (xbox/app) calls once it has loaded it (xbox/loader):
   longshot_uwp_set_platform with its table (uwp_platform.h), then longshot_uwp_main on a thread of its own, which is the
   game's main (host/hos_main.c, compiled with -Dmain=hos_uwp_game_main in a UWP build) and does not return while the
   game runs. The app gives the game's command line as a PC's launcher does: the image beside the build, --romfs the
   player's romfs folder on the drive, --user the app's LocalState. */
#include <windows.h>

#include "uwp_platform.h"

const LongshotUwpPlatform *hos_uwp;

int hos_uwp_game_main(int argc, char **argv);

__declspec(dllexport) int longshot_uwp_set_platform(const LongshotUwpPlatform *p) {
    if (!p || p->version != LONGSHOT_UWP_PLATFORM_VERSION) return 0;
    hos_uwp = p;
    return 1;
}

__declspec(dllexport) int longshot_uwp_main(int argc, char **argv) {
    if (!hos_uwp) return 1;
    return hos_uwp_game_main(argc, argv);
}
