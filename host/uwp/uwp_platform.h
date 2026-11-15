/* (xbox) What the Xbox app (xbox/app, a UWP app built with MSVC) gives the game module (longshot_game.dll, built from the
   player's own files and loaded by xbox/loader): the window, the keyboard, sound, the clipboard, links and messages, and
   where the files are. A UWP app may not call the desktop's window, sound and shell functions (xbox/tools/uwp_gap.py
   lists them); host/uwp puts these in their place, and they come here.

   The app fills one table and passes it to the module's longshot_uwp_set_platform before longshot_uwp_main. Every
   callback may be called from any of the game's threads; the app takes them to its window's thread where WinRT wants
   that. A change to the table bumps LONGSHOT_UWP_PLATFORM_VERSION. */
#ifndef LONGSHOT_UWP_PLATFORM_H
#define LONGSHOT_UWP_PLATFORM_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LONGSHOT_UWP_PLATFORM_VERSION 2

enum {
    LONGSHOT_UWP_EV_NONE,
    LONGSHOT_UWP_EV_CLOSE,     /* the app is closing: keep what must be kept, then end */
    LONGSHOT_UWP_EV_RESIZE,    /* the window's size in pixels changed (a, b) */
    LONGSHOT_UWP_EV_FOCUS,     /* the window got (a = 1) or lost (a = 0) the focus */
    LONGSHOT_UWP_EV_SUSPEND,   /* the console suspends the app (the Guide, another app): save soon, the game is frozen next */
    LONGSHOT_UWP_EV_RESUME,
    LONGSHOT_UWP_EV_TEXT,      /* a typed character, a = its code point */
};

typedef struct LongshotUwpEvent {
    int type;
    int a, b;
} LongshotUwpEvent;

typedef struct LongshotUwpGamepad {
    uint32_t buttons;      /* XINPUT_GAMEPAD_* bitmask */
    uint8_t left_trigger;  /* 0 - 255 */
    uint8_t right_trigger; /* 0 - 255 */
    int16_t thumb_lx;      /* -32768 to 32767 */
    int16_t thumb_ly;
    int16_t thumb_rx;
    int16_t thumb_ry;
    uint32_t connected;    /* 1 if connected, 0 otherwise */
} LongshotUwpGamepad;

typedef struct LongshotUwpPlatform {
    uint32_t version; /* LONGSHOT_UWP_PLATFORM_VERSION */

    /* ---- the window: a CoreWindow on the app's thread ---- */
    void (*window_size)(int *width, int *height);   /* in pixels */
    int (*refresh_mhz)(void);                        /* the display's, 0 when not known */
    /* the renderer's surface: (VkInstance, VkSurfaceKHR *), made by the Direct3D 12 layer on the CoreWindow; 1 if made */
    int (*create_vk_surface)(void *instance, void *surface_out);
    int (*poll_event)(LongshotUwpEvent *out);        /* 1 and the next event, or 0 */
    /* the keyboard (a USB or Bluetooth keyboard on the console) by Windows' virtual-key codes, 1 while held */
    const volatile uint8_t *keys;

    /* ---- input: gamepads (Windows.Gaming.Input) ---- */
    int (*gamepad_read)(uint32_t user_index, LongshotUwpGamepad *out);
    void (*gamepad_rumble)(uint32_t user_index, uint16_t left_motor, uint16_t right_motor);

    /* ---- text ---- */
    int (*clipboard_get)(char *utf8, size_t size);   /* the clipboard's text, its first line; 0 if none */
    void (*clipboard_set)(const char *utf8);

    /* ---- the world outside ---- */
    void (*open_uri)(const char *utf8);                       /* a web page, in the console's browser */
    void (*message)(const char *title, const char *utf8);     /* shown over the game; does not wait */
    void (*log)(const char *utf8);

    /* ---- sound: one output, 16-bit PCM, its buffers played in order ---- */
    void *(*audio_open)(uint32_t rate, uint32_t channels, uint32_t bits);
    /* queue a buffer (kept by the caller until done); done(ctx) once it has played or was dropped. 0 if refused */
    int (*audio_submit)(void *stream, const void *data, uint32_t bytes, void (*done)(void *ctx), void *ctx);
    void (*audio_reset)(void *stream);   /* drops every queued buffer, each done() called before it returns */
    void (*audio_volume)(void *stream, float volume);
    void (*audio_close)(void *stream);

    /* ---- files, UTF-8 ---- */
    const char *local_folder;   /* the app's LocalState: options, saves, caches */
    const char *game_folder;    /* the player's folder holding romfs\ and exefs\, on their drive */
    const char *work_folder;    /* <drive>\Longshot\work */

    /* ---- the console ---- */
    uint32_t model;             /* xbox/app/profiles.h Model */
    uint64_t memory_limit;      /* bytes the console lets the app use */
} LongshotUwpPlatform;

/* (host/uwp/uwp_entry.c) the table, set once before the game starts */
extern const LongshotUwpPlatform *hos_uwp;

#ifdef __cplusplus
}
#endif
#endif
