/* host/uwp/win32_uwp.c on a PC, against a stand-in for the app's platform table: what each replaced Windows function does
   with it. Linked against the UWP import library (libwindowsapp.a), not user32, winmm or shell32: a function this file
   calls that is not replaced fails the link. */
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "../uwp_platform.h"

int longshot_uwp_set_platform(const LongshotUwpPlatform *p);
int longshot_uwp_main(int argc, char **argv);

static int game_argc;
int hos_uwp_game_main(int argc, char **argv) {   /* (the game's main, host/hos_main.c, in the module) */
    (void)argv;
    game_argc = argc;
    return 7;
}

static int failures;
#define CHECK(cond, what)                                      \
    do {                                                       \
        int ok_ = (cond);                                      \
        printf("%s: %s\n", ok_ ? "ok  " : "FAIL", what);       \
        if (!ok_) ++failures;                                  \
    } while (0)

/* ---- the stand-in app ---- */
static char said_title[256], said_text[256], opened[256], clip[256];
static volatile uint8_t keys[256];
static void fake_message(const char *t, const char *s) { snprintf(said_title, sizeof said_title, "%s", t), snprintf(said_text, sizeof said_text, "%s", s); }
static void fake_open(const char *u) { snprintf(opened, sizeof opened, "%s", u); }
static int fake_clip_get(char *out, size_t n) { if (!clip[0]) return 0; snprintf(out, n, "%s", clip); return 1; }
static void fake_clip_set(const char *s) { snprintf(clip, sizeof clip, "%s", s); }
static void fake_size(int *w, int *h) { *w = 2560, *h = 1440; }
static int fake_mhz(void) { return 59940; }

typedef struct { void (*done)(void *); void *ctx; uint32_t bytes; } Queued;
static Queued queue[16];
static int nqueued, opened_rate, opened_channels, closed;
static float volume = -1;
static void *fake_audio_open(uint32_t rate, uint32_t ch, uint32_t bits) { (void)bits; opened_rate = (int)rate, opened_channels = (int)ch; return (void *)queue; }
static int fake_submit(void *s, const void *d, uint32_t n, void (*done)(void *), void *ctx) {
    (void)s, (void)d;
    if (nqueued == 16) return 0;
    queue[nqueued++] = (Queued){done, ctx, n};
    return 1;
}
static void play_first(void) {
    if (!nqueued) return;
    Queued q = queue[0];
    memmove(queue, queue + 1, sizeof *queue * (size_t)--nqueued);
    q.done(q.ctx);
}
static void fake_reset(void *s) { (void)s; while (nqueued) play_first(); }
static void fake_volume(void *s, float v) { (void)s; volume = v; }
static void fake_close(void *s) { (void)s; closed = 1; }

static LongshotUwpGamepad fake_pad = {0x1000 /* A */, 128, 200, 1000, -2000, 3000, -4000, 1};
static int fake_gamepad_read(uint32_t user_index, LongshotUwpGamepad *out) {
    if (user_index != 0 || !out) return 0;
    *out = fake_pad;
    return 1;
}
static uint16_t rumble_left, rumble_right;
static void fake_gamepad_rumble(uint32_t user_index, uint16_t left, uint16_t right) {
    if (user_index == 0) {
        rumble_left = left;
        rumble_right = right;
    }
}

static LongshotUwpPlatform platform = {
    LONGSHOT_UWP_PLATFORM_VERSION,
    fake_size,
    fake_mhz,
    NULL,
    NULL,
    keys,
    fake_gamepad_read,
    fake_gamepad_rumble,
    fake_clip_get,
    fake_clip_set,
    fake_open,
    fake_message,
    NULL,
    fake_audio_open,
    fake_submit,
    fake_reset,
    fake_volume,
    fake_close,
    "C:\\Packages\\Longshot\\LocalState",
    "",
    "",
    0,
    0,
};

void *longshot_uwp_win32(const char *name);
HRESULT WINAPI SHGetFolderPathA(HWND w, int csidl, HANDLE token, DWORD flags, LPSTR path);
DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR file, LPDWORD handle);
typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} UwpTestGamepad;
typedef struct {
    DWORD dwPacketNumber;
    UwpTestGamepad Gamepad;
} UwpTestState;
typedef struct {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
} UwpTestVibration;
DWORD WINAPI XInputGetState(DWORD dwUserIndex, UwpTestState *pState);
DWORD WINAPI XInputSetState(DWORD dwUserIndex, UwpTestVibration *pVibration);

int main(void) {
    char *args[] = {"longshot", "--romfs", "E:\\OoT\\romfs"};
    CHECK(longshot_uwp_main(3, args) == 1 && game_argc == 0, "no game before the table");
    LongshotUwpPlatform old = platform;
    old.version = 0;
    CHECK(!longshot_uwp_set_platform(&old), "a table of another version is refused");
    CHECK(longshot_uwp_set_platform(&platform), "the table is taken");
    CHECK(longshot_uwp_main(3, args) == 7 && game_argc == 3, "the game's main gets the command line");

    CHECK(MessageBoxA(NULL, "the text", "the title", MB_OK | MB_ICONERROR) == IDOK && !strcmp(said_title, "the title") &&
              !strcmp(said_text, "the text"),
          "MessageBoxA is shown by the app, IDOK");
    CHECK(MessageBoxW(NULL, L"caf\u00e9", L"t", MB_OK) == IDOK && !strcmp(said_text, "caf\xc3\xa9"), "MessageBoxW in UTF-8");
    CHECK((INT_PTR)ShellExecuteA(NULL, "open", "https://example.org/a", NULL, NULL, SW_SHOW) > 32 && !strcmp(opened, "https://example.org/a"),
          "ShellExecuteA opens the link");
    CHECK((INT_PTR)ShellExecuteW(NULL, L"open", L"https://example.org/w", NULL, NULL, SW_SHOW) > 32 && !strcmp(opened, "https://example.org/w"),
          "ShellExecuteW opens the link");

    keys[VK_F1] = 1;
    CHECK((GetAsyncKeyState(VK_F1) & 0x8000) && !GetAsyncKeyState(VK_F2) && (GetKeyState(VK_F1) & 0x8000) && !GetAsyncKeyState(999),
          "keys from the app's table");

    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, 32);
    wcscpy((WCHAR *)GlobalLock(g), L"zelda \u00e9");
    GlobalUnlock(g);
    CHECK(OpenClipboard(NULL) && EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, g) && CloseClipboard() &&
              !strcmp(clip, "zelda \xc3\xa9"),
          "SetClipboardData gives the app UTF-8");
    strcpy(clip, "link");
    OpenClipboard(NULL);
    HANDLE got = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *gw = got ? (const WCHAR *)GlobalLock(got) : NULL;
    CHECK(gw && !wcscmp(gw, L"link"), "GetClipboardData gives the app's text");
    if (got) GlobalUnlock(got);
    CloseClipboard();
    CHECK(GetClipboardData(CF_TEXT) == NULL, "only CF_UNICODETEXT");

    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    CHECK(EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth == 2560 && dm.dmPelsHeight == 1440 &&
              dm.dmDisplayFrequency == 60 && !EnumDisplaySettingsW(NULL, 1, &dm),
          "the screen is the app's window, one mode");
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    CHECK(GetMonitorInfoA(MonitorFromWindow(NULL, 0), &mi) && mi.rcMonitor.right == 2560, "the monitor too");
    RECT rc;
    CHECK(GetClientRect(NULL, &rc) && rc.right == 2560 && rc.bottom == 1440, "the client area too");
    CHECK(CreateWindowExA(0, "c", "w", 0, 0, 0, 1, 1, NULL, NULL, NULL, NULL) == NULL && GetLastError() == ERROR_NOT_SUPPORTED,
          "no desktop windows");
    CHECK(GetActiveProcessorCount(0) > 0 && GetActiveProcessorGroupCount() == 1, "processors, one group");
    int here = 1;
    CHECK(!IsBadReadPtr(&here, sizeof here) && IsBadReadPtr(NULL, 4), "IsBadReadPtr");

    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    HWAVEOUT w = NULL;
    CHECK(waveOutGetNumDevs() == 1, "one sound device");
    WAVEOUTCAPSA caps;
    CHECK(waveOutGetDevCapsA(0, &caps, sizeof caps) == MMSYSERR_NOERROR && caps.szPname[0], "its name");
    CHECK(waveOutOpen(&w, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR && w && opened_rate == 48000 && opened_channels == 2,
          "waveOutOpen opens the app's output, 48 kHz stereo");
    WAVEFORMATEX f8 = f;
    f8.wBitsPerSample = 8;
    HWAVEOUT w8 = NULL;
    CHECK(waveOutOpen(&w8, WAVE_MAPPER, &f8, 0, 0, CALLBACK_NULL) == WAVERR_BADFORMAT, "8-bit refused");
    CHECK(waveOutOpen(&w8, WAVE_MAPPER, &f, 1, 0, CALLBACK_FUNCTION) == MMSYSERR_NOTSUPPORTED, "callbacks refused (polled only)");

    static short pcm[3][960];
    WAVEHDR h[3];
    memset(h, 0, sizeof h);
    for (int i = 0; i < 3; i++) {
        h[i].lpData = (LPSTR)pcm[i], h[i].dwBufferLength = sizeof pcm[i];
        if (!i) CHECK(waveOutWrite(w, &h[i], sizeof h[i]) == WAVERR_UNPREPARED, "a write before prepare is refused");
        waveOutPrepareHeader(w, &h[i], sizeof h[i]);
        waveOutWrite(w, &h[i], sizeof h[i]);
    }
    CHECK(nqueued == 3 && (h[0].dwFlags & WHDR_INQUEUE) && !(h[0].dwFlags & WHDR_DONE), "three buffers queued, not done");
    CHECK(waveOutUnprepareHeader(w, &h[0], sizeof h[0]) == WAVERR_STILLPLAYING, "a queued buffer cannot be unprepared");
    CHECK(waveOutClose(w) == WAVERR_STILLPLAYING, "nor the device closed");
    play_first();
    CHECK((h[0].dwFlags & WHDR_DONE) && !(h[0].dwFlags & WHDR_INQUEUE) && (h[1].dwFlags & WHDR_INQUEUE), "the first played: done");
    CHECK(waveOutUnprepareHeader(w, &h[0], sizeof h[0]) == MMSYSERR_NOERROR && !(h[0].dwFlags & WHDR_PREPARED), "then unprepared");
    waveOutSetVolume(w, 0x8000FFFF);
    CHECK(volume > 0.74f && volume < 0.76f, "the volume, both channels' mean");
    CHECK(waveOutReset(w) == MMSYSERR_NOERROR && (h[1].dwFlags & WHDR_DONE) && (h[2].dwFlags & WHDR_DONE) && nqueued == 0,
          "a reset leaves every buffer done");
    CHECK(waveOutClose(w) == MMSYSERR_NOERROR && closed, "and the device closes");

    char folder[MAX_PATH];
    CHECK(SHGetFolderPathA(NULL, 0x1c /* CSIDL_LOCAL_APPDATA */, NULL, 0, folder) == S_OK &&
              !strcmp(folder, "C:\\Packages\\Longshot\\LocalState"),
          "SHGetFolderPathA: the app's LocalState (Dozen's shader cache)");
    DWORD vh = 1;
    CHECK(GetDC(NULL) == NULL && !CreateCompatibleDC(NULL) && !GetFileVersionInfoSizeW(L"d3d12core.dll", &vh) && vh == 0,
          "Dozen's GDI presentation and version check: none");
    CHECK(longshot_uwp_win32("GetClientRect") == (void *)GetClientRect && longshot_uwp_win32("waveOutClose") == (void *)waveOutClose &&
              longshot_uwp_win32("SHGetFolderPathA") == (void *)SHGetFolderPathA &&
              longshot_uwp_win32("XInputGetState") == (void *)XInputGetState &&
              longshot_uwp_win32("XInputSetState") == (void *)XInputSetState && !longshot_uwp_win32("CreateFileW") &&
              !longshot_uwp_win32(NULL),
          "longshot_uwp_win32 finds these by name, and nothing else");

    UwpTestState xs;
    memset(&xs, 0, sizeof xs);
    CHECK(XInputGetState(0, &xs) == 0 && xs.Gamepad.wButtons == 0x1000 && xs.Gamepad.bLeftTrigger == 128 &&
              xs.Gamepad.sThumbLX == 1000 && xs.Gamepad.sThumbRY == -4000,
          "XInputGetState reads gamepad through the table");
    CHECK(XInputGetState(1, &xs) == 1167, "unconnected pad returns ERROR_DEVICE_NOT_CONNECTED");
    UwpTestVibration xv = {32768, 65535};
    CHECK(XInputSetState(0, &xv) == 0 && rumble_left == 32768 && rumble_right == 65535, "XInputSetState sets rumble");

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
