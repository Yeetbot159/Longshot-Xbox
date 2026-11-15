/* (xbox) The game's window on the console: what host/display.c is on Windows and host/posix/display_sdl.c on Linux.
   The window is the app's CoreWindow, on the app's own thread (xbox/app); this side reads its events through the
   platform table (uwp_platform.h) on a thread of its own, starts the renderer with a window bridge (gpu/vk/vk_api.h
   MkwVkWindow) whose surface the app makes, and passes on sizes, focus, typed text and the app's closing.

   There is no mouse on the console: the mouse calls answer "none", and the game is played with the controller
   (host/nn_hid.c). A USB keyboard works, through the app's key table (host/uwp/win32_uwp.c GetAsyncKeyState). */
#include "../hos.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../../gpu/vk/compat/longshot_env.h"
#include "../../gpu/vk/vk_api.h"
#include "uwp_platform.h"

int rx_start(void *window);
void rx_resize(int width, int height);
void rx_close_asked(void);
void hos_window_closed_exit(void);
void hos_input_clear_toggles(void);
int hos_text_focus(void);
void hos_text_put(uint32_t c);
void hos_text_put_utf8(const char *s, int first_line);

volatile LONG64 hos_present_count;   /* (nvn_native.c: the game's presents) */

/* ---- the mouse: none ---- */
int hos_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = *dy = 0;
    return 0;
}
void hos_mouse_give_back(int32_t dx, int32_t dy) { (void)dx, (void)dy; }
int hos_mouse_pointer(MkwVkPointer *out) {
    memset(out, 0, sizeof *out);
    return 0;
}
void hos_mouse_game_menu(void) {}
void hos_mouse_game_menu_gone(void) {}
int hos_mouse_wheel_take(void) { return 0; }
int hos_mouse_game_pointer(int *x, int *y, int *w, int *h, uint32_t *pressed, int *wheel) {
    *x = *y = *w = *h = 0, *pressed = 0, *wheel = 0;
    return 0;
}

/* ---- the clipboard into the Insert menu's text field ---- */
void hos_display_paste(void) {
    char text[1024];
    if (hos_text_focus() && hos_uwp && hos_uwp->clipboard_get && hos_uwp->clipboard_get(text, sizeof text)) hos_text_put_utf8(text, 1);
}

/* ---- the renderer's window bridge ---- */
static int create_surface(void *window, void *instance, void *surface) {
    (void)window;
    return hos_uwp && hos_uwp->create_vk_surface ? hos_uwp->create_vk_surface(instance, surface) : 0;
}
static void drawable_size(void *window, int *width, int *height) {
    (void)window;
    *width = *height = 0;
    if (hos_uwp && hos_uwp->window_size) hos_uwp->window_size(width, height);
}
static int refresh_mhz(void *window) {
    (void)window;
    return hos_uwp && hos_uwp->refresh_mhz ? hos_uwp->refresh_mhz() : 0;
}
static MkwVkWindow vk_window = {MKWVK_WINDOW_VERSION, create_surface, drawable_size, NULL, refresh_mhz, drawable_size};

static volatile int renderer;
static HANDLE renderer_up;

void hos_renderer_wait(void) {
    { const char *e = getenv("HOS_NO_RENDERER_WAIT"); if (e && atoi(e) != 0) return; }
    if (renderer_up) WaitForSingleObject(renderer_up, INFINITE);
}

static DWORD WINAPI display_main(void *p) {
    (void)p;
    renderer = rx_start(&vk_window);
    if (!renderer) hos_log("display: the renderer did not start: the screen stays black");
    SetEvent(renderer_up);
    for (;;) {
        LongshotUwpEvent e;
        while (hos_uwp->poll_event(&e)) {
            switch (e.type) {
            case LONGSHOT_UWP_EV_CLOSE:
                rx_close_asked();
                hos_window_closed_exit();
                break;
            case LONGSHOT_UWP_EV_RESIZE:
                if (renderer) rx_resize(e.a, e.b);
                break;
            case LONGSHOT_UWP_EV_FOCUS:
                if (!e.a) hos_input_clear_toggles();
                break;
            case LONGSHOT_UWP_EV_SUSPEND:
                hos_log("display: suspended by the console");
                hos_log_flush();
                break;
            case LONGSHOT_UWP_EV_TEXT:
                if (hos_text_focus()) hos_text_put((uint32_t)e.a);
                break;
            }
        }
        Sleep(4);
    }
    return 0;
}

void hos_display_start(void) {
    if (env_on("HOS_HEADLESS") || !hos_uwp) {   /* no window: the renderer still draws, offscreen */
        rx_start(NULL);
        return;
    }
    renderer_up = CreateEventA(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, display_main, NULL, 0, NULL));
}
