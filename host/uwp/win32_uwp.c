/* (xbox) The Windows functions Longshot calls that a UWP app may not link (xbox/tools/uwp_gap.py: 77 of the 304 it
   imports), defined here so the game module's link takes these and no desktop DLL is named. What each does on the
   console:

   - sound (winmm's waveOut): the app's XAudio2 output (hos_uwp->audio_*), with waveOut's own rules as host/nn_audio.c
     uses them: CALLBACK_NULL, buffers prepared, written, and polled for WHDR_DONE;
   - message boxes: shown over the game by the app (they never wait: nothing on the console answers them), IDOK;
   - links (ShellExecute): the console's browser;
   - the clipboard: the app's (a keyboard's Ctrl+V on the console), as CF_UNICODETEXT;
   - the keyboard (GetAsyncKeyState, GetKeyState): the app's key table;
   - the screen (EnumDisplaySettingsW, GetMonitorInfoA, MonitorFromWindow): the app's window, which fills it;
   - the desktop's windows, cursor, raw input and timers: nothing, as there are none (host/uwp/display_uwp.c is the
     window); creating a window fails;
   - processor groups: one; a crash dump: not written (no dbghelp in the app container); a pipe's security descriptor:
     refused, so the control bridge keeps the default one.

   DECLSPEC_IMPORT is emptied before windows.h: the headers declare these as imported, and here they are defined. */
#define DECLSPEC_IMPORT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <sddl.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

#include "uwp_platform.h"

static void say(const char *title, const char *text) {
    if (hos_uwp && hos_uwp->message) hos_uwp->message(title ? title : "", text ? text : "");
    else if (hos_uwp && hos_uwp->log) hos_uwp->log(text ? text : "");
}

static char *utf8_of(const WCHAR *w) {
    if (!w) return NULL;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = n > 0 ? (char *)malloc((size_t)n) : NULL;
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* ---- the keyboard ---- */

SHORT WINAPI GetAsyncKeyState(int vk) {
    if (!hos_uwp || !hos_uwp->keys || vk < 0 || vk > 255) return 0;
    return hos_uwp->keys[vk] ? (SHORT)0x8000 : 0;
}
SHORT WINAPI GetKeyState(int vk) { return GetAsyncKeyState(vk); }

/* ---- message boxes and links ---- */

int WINAPI MessageBoxA(HWND w, LPCSTR text, LPCSTR title, UINT type) {
    (void)w, (void)type;
    say(title, text);
    return IDOK;
}
int WINAPI MessageBoxW(HWND w, LPCWSTR text, LPCWSTR title, UINT type) {
    (void)w, (void)type;
    char *t = utf8_of(text), *c = utf8_of(title);
    say(c, t);
    free(t), free(c);
    return IDOK;
}

static HINSTANCE open_link(const char *file) {
    if (!file || !hos_uwp || !hos_uwp->open_uri) return (HINSTANCE)(INT_PTR)SE_ERR_NOASSOC;
    hos_uwp->open_uri(file);
    return (HINSTANCE)(INT_PTR)33;   /* (above 32: done) */
}
HINSTANCE WINAPI ShellExecuteA(HWND w, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, INT show) {
    (void)w, (void)op, (void)params, (void)dir, (void)show;
    return open_link(file);
}
HINSTANCE WINAPI ShellExecuteW(HWND w, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show) {
    (void)w, (void)op, (void)params, (void)dir, (void)show;
    char *f = utf8_of(file);
    HINSTANCE r = open_link(f);
    free(f);
    return r;
}

/* ---- the clipboard: one CF_UNICODETEXT, between OpenClipboard and CloseClipboard ---- */

static HGLOBAL clip_read;   /* (what GetClipboardData handed out: the caller does not free it, as with Windows') */

BOOL WINAPI OpenClipboard(HWND w) {
    (void)w;
    return hos_uwp && hos_uwp->clipboard_get ? TRUE : FALSE;
}
BOOL WINAPI CloseClipboard(void) { return TRUE; }
BOOL WINAPI EmptyClipboard(void) { return TRUE; }

HANDLE WINAPI GetClipboardData(UINT format) {
    if (format != CF_UNICODETEXT || !hos_uwp || !hos_uwp->clipboard_get) return NULL;
    char text[4096];
    if (!hos_uwp->clipboard_get(text, sizeof text)) return NULL;
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (clip_read) GlobalFree(clip_read), clip_read = NULL;
    HGLOBAL g = n > 0 ? GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(WCHAR)) : NULL;
    WCHAR *p = g ? (WCHAR *)GlobalLock(g) : NULL;
    if (!p) return g ? (GlobalFree(g), NULL) : NULL;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, p, n);
    GlobalUnlock(g);
    return clip_read = g;
}

HANDLE WINAPI SetClipboardData(UINT format, HANDLE data) {
    if (format != CF_UNICODETEXT || !data || !hos_uwp || !hos_uwp->clipboard_set) return NULL;
    const WCHAR *w = (const WCHAR *)GlobalLock(data);
    char *s = utf8_of(w);
    GlobalUnlock(data);
    if (s) hos_uwp->clipboard_set(s);
    free(s);
    GlobalFree(data);   /* (the clipboard owns what it is given) */
    return data;
}

/* ---- the screen: the app's window fills it ---- */

static void screen_size(int *w, int *h) {
    *w = 1920, *h = 1080;
    if (hos_uwp && hos_uwp->window_size) hos_uwp->window_size(w, h);
}
static void screen_rect(RECT *r) {   /* (not SetRect: user32's) */
    int w, h;
    screen_size(&w, &h);
    r->left = r->top = 0, r->right = w, r->bottom = h;
}

BOOL WINAPI EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW *dm) {
    (void)device;
    if (!dm || (mode != ENUM_CURRENT_SETTINGS && mode != ENUM_REGISTRY_SETTINGS && mode != 0)) return FALSE;
    int w, h;
    screen_size(&w, &h);
    const int mhz = hos_uwp && hos_uwp->refresh_mhz ? hos_uwp->refresh_mhz() : 0;
    const WORD size = dm->dmSize, extra = dm->dmDriverExtra;
    memset(dm, 0, size >= sizeof *dm ? sizeof *dm : size);
    dm->dmSize = size, dm->dmDriverExtra = extra;
    dm->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    dm->dmPelsWidth = (DWORD)w, dm->dmPelsHeight = (DWORD)h, dm->dmBitsPerPel = 32;
    dm->dmDisplayFrequency = mhz > 0 ? (DWORD)((mhz + 500) / 1000) : 60;
    return TRUE;
}

HMONITOR WINAPI MonitorFromWindow(HWND w, DWORD flags) {
    (void)w, (void)flags;
    return (HMONITOR)(INT_PTR)1;
}

BOOL WINAPI GetMonitorInfoA(HMONITOR m, LPMONITORINFO mi) {
    if (!m || !mi || mi->cbSize < sizeof(MONITORINFO)) return FALSE;
    screen_rect(&mi->rcMonitor);
    mi->rcWork = mi->rcMonitor;
    mi->dwFlags = MONITORINFOF_PRIMARY;
    return TRUE;
}

/* ---- the desktop's windows, cursor, raw input and timers: none on the console ---- */

static HWND no_window(void) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}
HWND WINAPI CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu,
                            HINSTANCE inst, LPVOID param) {
    (void)ex, (void)cls, (void)name, (void)style, (void)x, (void)y, (void)w, (void)h, (void)parent, (void)menu, (void)inst, (void)param;
    return no_window();
}
HWND WINAPI CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu,
                            HINSTANCE inst, LPVOID param) {
    (void)ex, (void)cls, (void)name, (void)style, (void)x, (void)y, (void)w, (void)h, (void)parent, (void)menu, (void)inst, (void)param;
    return no_window();
}
ATOM WINAPI RegisterClassA(CONST WNDCLASSA *wc) { (void)wc; return 1; }
LRESULT WINAPI DefWindowProcA(HWND w, UINT m, WPARAM wp, LPARAM lp) { (void)w, (void)m, (void)wp, (void)lp; return 0; }
BOOL WINAPI DestroyWindow(HWND w) { (void)w; return FALSE; }
BOOL WINAPI IsWindow(HWND w) { (void)w; return FALSE; }
BOOL WINAPI IsIconic(HWND w) { (void)w; return FALSE; }
BOOL WINAPI ShowWindow(HWND w, int cmd) { (void)w, (void)cmd; return FALSE; }
BOOL WINAPI SetWindowPos(HWND w, HWND after, int x, int y, int cx, int cy, UINT flags) {
    (void)w, (void)after, (void)x, (void)y, (void)cx, (void)cy, (void)flags;
    return FALSE;
}
BOOL WINAPI GetWindowPlacement(HWND w, WINDOWPLACEMENT *p) { (void)w, (void)p; return FALSE; }
BOOL WINAPI SetWindowPlacement(HWND w, CONST WINDOWPLACEMENT *p) { (void)w, (void)p; return FALSE; }
LONG WINAPI GetWindowLongA(HWND w, int i) { (void)w, (void)i; return 0; }
LONG WINAPI SetWindowLongA(HWND w, int i, LONG v) { (void)w, (void)i, (void)v; return 0; }
LONG_PTR WINAPI GetWindowLongPtrA(HWND w, int i) { (void)w, (void)i; return 0; }
int WINAPI GetClassNameA(HWND w, LPSTR out, int n) { (void)w; if (out && n > 0) out[0] = 0; return 0; }
DWORD WINAPI GetWindowThreadProcessId(HWND w, LPDWORD pid) { (void)w; if (pid) *pid = 0; return 0; }
HWND WINAPI GetDesktopWindow(void) { return NULL; }
HWND WINAPI GetForegroundWindow(void) { return NULL; }
BOOL WINAPI AllowSetForegroundWindow(DWORD pid) { (void)pid; return FALSE; }
HWND WINAPI WindowFromPoint(POINT p) { (void)p; return NULL; }

BOOL WINAPI GetClientRect(HWND w, LPRECT r) {
    (void)w;
    if (!r) return FALSE;
    screen_rect(r);
    return TRUE;
}
BOOL WINAPI AdjustWindowRect(LPRECT r, DWORD style, BOOL menu) { (void)r, (void)style, (void)menu; return TRUE; }
BOOL WINAPI ClientToScreen(HWND w, LPPOINT p) { (void)w, (void)p; return TRUE; }
BOOL WINAPI ScreenToClient(HWND w, LPPOINT p) { (void)w, (void)p; return TRUE; }
int WINAPI MapWindowPoints(HWND from, HWND to, LPPOINT p, UINT n) { (void)from, (void)to, (void)p, (void)n; return 0; }
BOOL WINAPI PtInRect(CONST RECT *r, POINT p) { return r && p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
BOOL WINAPI ValidateRect(HWND w, CONST RECT *r) { (void)w, (void)r; return TRUE; }

BOOL WINAPI PeekMessageA(LPMSG m, HWND w, UINT lo, UINT hi, UINT remove) { (void)m, (void)w, (void)lo, (void)hi, (void)remove; return FALSE; }
BOOL WINAPI GetMessageA(LPMSG m, HWND w, UINT lo, UINT hi) { (void)m, (void)w, (void)lo, (void)hi; return FALSE; }
BOOL WINAPI TranslateMessage(CONST MSG *m) { (void)m; return FALSE; }
LRESULT WINAPI DispatchMessageA(CONST MSG *m) { (void)m; return 0; }
BOOL WINAPI PostMessageA(HWND w, UINT m, WPARAM wp, LPARAM lp) { (void)w, (void)m, (void)wp, (void)lp; return FALSE; }
DWORD WINAPI GetMessagePos(void) { return 0; }
UINT_PTR WINAPI SetTimer(HWND w, UINT_PTR id, UINT ms, TIMERPROC fn) { (void)w, (void)id, (void)ms, (void)fn; return 0; }

HCURSOR WINAPI LoadCursorA(HINSTANCE i, LPCSTR name) { (void)i, (void)name; return NULL; }
HICON WINAPI LoadIconA(HINSTANCE i, LPCSTR name) { (void)i, (void)name; return NULL; }
HCURSOR WINAPI SetCursor(HCURSOR c) { (void)c; return NULL; }
int WINAPI ShowCursor(BOOL show) { return show ? 0 : -1; }
BOOL WINAPI ClipCursor(CONST RECT *r) { (void)r; return TRUE; }
BOOL WINAPI GetCursorPos(LPPOINT p) { if (p) p->x = p->y = 0; return FALSE; }
HWND WINAPI GetCapture(void) { return NULL; }
HWND WINAPI SetCapture(HWND w) { (void)w; return NULL; }
BOOL WINAPI ReleaseCapture(void) { return TRUE; }
BOOL WINAPI TrackMouseEvent(LPTRACKMOUSEEVENT t) { (void)t; return FALSE; }
BOOL WINAPI RegisterRawInputDevices(PCRAWINPUTDEVICE d, UINT n, UINT size) { (void)d, (void)n, (void)size; return FALSE; }
UINT WINAPI GetRawInputData(HRAWINPUT r, UINT cmd, LPVOID data, PUINT size, UINT header) {
    (void)r, (void)cmd, (void)data, (void)header;
    if (size) *size = 0;
    return (UINT)-1;
}
BOOL WINAPI SystemParametersInfoA(UINT action, UINT ui, PVOID pv, UINT win_ini) { (void)action, (void)ui, (void)pv, (void)win_ini; return FALSE; }
HGDIOBJ WINAPI GetStockObject(int i) { (void)i; return NULL; }

/* ---- the rest ---- */

DWORD WINAPI GetActiveProcessorCount(WORD group) {
    if (group != 0 && group != ALL_PROCESSOR_GROUPS) return 0;
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    return si.dwNumberOfProcessors;
}
WORD WINAPI GetActiveProcessorGroupCount(void) { return 1; }

BOOL WINAPI IsBadReadPtr(CONST VOID *p, UINT_PTR n) {
    if (!n) return FALSE;
    if (!p) return TRUE;
    MEMORY_BASIC_INFORMATION mi;
    const char *at = (const char *)p, *end = at + n;
    while (at < end) {
        if (!VirtualQuery(at, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return TRUE;
        at = (const char *)mi.BaseAddress + mi.RegionSize;
    }
    return FALSE;
}

BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorA(LPCSTR s, DWORD rev, PSECURITY_DESCRIPTOR *sd, PULONG size) {
    (void)s, (void)rev;
    if (sd) *sd = NULL;
    if (size) *size = 0;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI MiniDumpWriteDump(HANDLE process, DWORD pid, HANDLE file, int type, void *exception, void *user, void *callback) {
    (void)process, (void)pid, (void)file, (void)type, (void)exception, (void)user, (void)callback;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/* ---- what Mesa's Dozen imports besides (the Vulkan driver on Direct3D 12, xbox/app/modules.cpp): its GDI
   presentation, which it does not use when DXGI works, the folder of its shader cache, and a version check ---- */

HDC WINAPI GetDC(HWND w) { (void)w; return NULL; }
int WINAPI ReleaseDC(HWND w, HDC dc) { (void)w, (void)dc; return 0; }
HDC WINAPI CreateCompatibleDC(HDC dc) { (void)dc; return NULL; }
BOOL WINAPI DeleteDC(HDC dc) { (void)dc; return FALSE; }
HBITMAP WINAPI CreateDIBSection(HDC dc, CONST BITMAPINFO *bi, UINT usage, VOID **bits, HANDLE section, DWORD offset) {
    (void)dc, (void)bi, (void)usage, (void)section, (void)offset;
    if (bits) *bits = NULL;
    return NULL;
}
HGDIOBJ WINAPI SelectObject(HDC dc, HGDIOBJ o) { (void)dc, (void)o; return NULL; }
BOOL WINAPI DeleteObject(HGDIOBJ o) { (void)o; return FALSE; }
int WINAPI GetObjectA(HANDLE o, int size, LPVOID out) { (void)o, (void)size, (void)out; return 0; }
BOOL WINAPI StretchBlt(HDC to, int x, int y, int w, int h, HDC from, int fx, int fy, int fw, int fh, DWORD rop) {
    (void)to, (void)x, (void)y, (void)w, (void)h, (void)from, (void)fx, (void)fy, (void)fw, (void)fh, (void)rop;
    return FALSE;
}

/* (CSIDL_LOCAL_APPDATA and the rest: the app's LocalState, where a desktop program's per-user files would go. Its path
   is the package's family name under the user's folder, ASCII, so the UTF-8 is the same bytes) */
HRESULT WINAPI SHGetFolderPathA(HWND w, int csidl, HANDLE token, DWORD flags, LPSTR path) {
    (void)w, (void)csidl, (void)token, (void)flags;
    if (!path) return E_INVALIDARG;
    path[0] = 0;
    const char *local = hos_uwp ? hos_uwp->local_folder : NULL;
    if (!local || !*local || strlen(local) >= MAX_PATH) return E_FAIL;
    strcpy(path, local);
    return S_OK;
}

DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR file, LPDWORD handle) {
    (void)file;
    if (handle) *handle = 0;
    SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
    return 0;
}

/* ---- sound: waveOut on the app's output ---- */

typedef struct {
    void *stream;
    volatile LONG queued;
} UwpWave;

static void wave_done(void *ctx) {
    WAVEHDR *h = (WAVEHDR *)ctx;
    UwpWave *w = (UwpWave *)h->reserved;
    /* (one exchange: the game reads the flags from another thread, host/nn_audio.c) */
    LONG old, now;
    do {
        old = *(volatile LONG *)&h->dwFlags;
        now = (old & ~(LONG)WHDR_INQUEUE) | (LONG)WHDR_DONE;
    } while (InterlockedCompareExchange((volatile LONG *)&h->dwFlags, now, old) != old);
    if (w) InterlockedDecrement(&w->queued);
}

UINT WINAPI waveOutGetNumDevs(void) { return hos_uwp && hos_uwp->audio_open ? 1 : 0; }

MMRESULT WINAPI waveOutGetDevCapsA(UINT_PTR dev, LPWAVEOUTCAPSA caps, UINT size) {
    (void)dev;
    if (!caps || size < sizeof *caps) return MMSYSERR_INVALPARAM;
    if (!waveOutGetNumDevs()) return MMSYSERR_NODRIVER;
    memset(caps, 0, sizeof *caps);
    strcpy(caps->szPname, "Xbox audio");
    caps->wChannels = 8;
    caps->dwFormats = WAVE_FORMAT_48S16;
    caps->dwSupport = WAVECAPS_VOLUME | WAVECAPS_LRVOLUME;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutMessage(HWAVEOUT w, UINT msg, DWORD_PTR a, DWORD_PTR b) {
    (void)w, (void)msg, (void)a, (void)b;
    return MMSYSERR_NOTSUPPORTED;   /* (the preferred device: there is one) */
}

MMRESULT WINAPI waveOutOpen(LPHWAVEOUT out, UINT dev, LPCWAVEFORMATEX f, DWORD_PTR cb, DWORD_PTR inst, DWORD flags) {
    (void)dev, (void)cb, (void)inst;
    if (!f) return MMSYSERR_INVALPARAM;
    if (!waveOutGetNumDevs()) return MMSYSERR_NODRIVER;
    const int pcm = f->wFormatTag == WAVE_FORMAT_PCM ||
                    (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22);   /* (the sub-format: PCM, as nn_audio.c asks) */
    if (!pcm || f->wBitsPerSample != 16 || !f->nChannels || f->nChannels > 8) return WAVERR_BADFORMAT;
    if ((flags & 0xF0000) != CALLBACK_NULL && !(flags & WAVE_FORMAT_QUERY)) return MMSYSERR_NOTSUPPORTED;   /* (polled only) */
    if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
    if (!out) return MMSYSERR_INVALPARAM;
    UwpWave *w = (UwpWave *)calloc(1, sizeof *w);
    if (!w) return MMSYSERR_NOMEM;
    w->stream = hos_uwp->audio_open(f->nSamplesPerSec, f->nChannels, f->wBitsPerSample);
    if (!w->stream) {
        free(w);
        return MMSYSERR_ALLOCATED;
    }
    *out = (HWAVEOUT)w;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT wo, LPWAVEHDR h, UINT size) {
    if (!wo || !h || size < sizeof *h) return MMSYSERR_INVALPARAM;
    h->dwFlags = (h->dwFlags | WHDR_PREPARED) & ~(DWORD)WHDR_DONE;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT wo, LPWAVEHDR h, UINT size) {
    if (!wo || !h || size < sizeof *h) return MMSYSERR_INVALPARAM;
    if (h->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    h->dwFlags &= ~(DWORD)WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutWrite(HWAVEOUT wo, LPWAVEHDR h, UINT size) {
    UwpWave *w = (UwpWave *)wo;
    if (!w || !h || size < sizeof *h) return MMSYSERR_INVALPARAM;
    if (!(h->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
    h->reserved = (DWORD_PTR)w;
    h->dwFlags = (h->dwFlags & ~(DWORD)WHDR_DONE) | WHDR_INQUEUE;
    InterlockedIncrement(&w->queued);
    if (!hos_uwp->audio_submit(w->stream, h->lpData, h->dwBufferLength, wave_done, h)) {
        wave_done(h);
        return MMSYSERR_ERROR;
    }
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutReset(HWAVEOUT wo) {
    UwpWave *w = (UwpWave *)wo;
    if (!w) return MMSYSERR_INVALHANDLE;
    hos_uwp->audio_reset(w->stream);   /* (every queued buffer's wave_done before it returns) */
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutSetVolume(HWAVEOUT wo, DWORD volume) {
    UwpWave *w = (UwpWave *)wo;
    if (!w) return MMSYSERR_INVALHANDLE;
    const float left = (float)(volume & 0xFFFF) / 65535.0f, right = (float)(volume >> 16) / 65535.0f;
    if (hos_uwp->audio_volume) hos_uwp->audio_volume(w->stream, (left + right) / 2);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutClose(HWAVEOUT wo) {
    UwpWave *w = (UwpWave *)wo;
    if (!w) return MMSYSERR_INVALHANDLE;
    if (w->queued) return WAVERR_STILLPLAYING;
    hos_uwp->audio_close(w->stream);
    free(w);
    return MMSYSERR_NOERROR;
}

/* ---- gamepads: XInput mapped onto Windows.Gaming.Input through the app's table ---- */

#define XUSER_MAX_COUNT 4
typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} UwpXInputGamepad;

typedef struct {
    DWORD dwPacketNumber;
    UwpXInputGamepad Gamepad;
} UwpXInputState;

typedef struct {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
} UwpXInputVibration;

DWORD WINAPI XInputGetState(DWORD dwUserIndex, UwpXInputState *pState) {
    if (!pState || dwUserIndex >= XUSER_MAX_COUNT) return 87; /* ERROR_INVALID_PARAMETER */
    if (!hos_uwp || !hos_uwp->gamepad_read) return 1167;       /* ERROR_DEVICE_NOT_CONNECTED */
    LongshotUwpGamepad g;
    if (!hos_uwp->gamepad_read(dwUserIndex, &g) || !g.connected) return 1167;
    static DWORD s_packet[XUSER_MAX_COUNT];
    pState->dwPacketNumber = ++s_packet[dwUserIndex];
    pState->Gamepad.wButtons = (WORD)g.buttons;
    pState->Gamepad.bLeftTrigger = g.left_trigger;
    pState->Gamepad.bRightTrigger = g.right_trigger;
    pState->Gamepad.sThumbLX = g.thumb_lx;
    pState->Gamepad.sThumbLY = g.thumb_ly;
    pState->Gamepad.sThumbRX = g.thumb_rx;
    pState->Gamepad.sThumbRY = g.thumb_ry;
    return 0; /* ERROR_SUCCESS */
}

DWORD WINAPI XInputSetState(DWORD dwUserIndex, UwpXInputVibration *pVibration) {
    if (dwUserIndex >= XUSER_MAX_COUNT) return 87;
    if (!hos_uwp || !hos_uwp->gamepad_rumble) return 1167;
    if (pVibration) hos_uwp->gamepad_rumble(dwUserIndex, pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed);
    return 0;
}

/* ---- by name, for the other modules the app loads (the renderer, Dozen): their imports of these come here ---- */

#define UWP_WIN32(X)                                                                                                       \
    X(GetAsyncKeyState) X(GetKeyState) X(MessageBoxA) X(MessageBoxW) X(ShellExecuteA) X(ShellExecuteW) X(OpenClipboard)    \
    X(CloseClipboard) X(EmptyClipboard) X(GetClipboardData) X(SetClipboardData) X(EnumDisplaySettingsW)                     \
    X(MonitorFromWindow) X(GetMonitorInfoA) X(CreateWindowExA) X(CreateWindowExW) X(RegisterClassA) X(DefWindowProcA)       \
    X(DestroyWindow) X(IsWindow) X(IsIconic) X(ShowWindow) X(SetWindowPos) X(GetWindowPlacement) X(SetWindowPlacement)      \
    X(GetWindowLongA) X(SetWindowLongA) X(GetWindowLongPtrA) X(GetClassNameA) X(GetWindowThreadProcessId)                   \
    X(GetDesktopWindow) X(GetForegroundWindow) X(AllowSetForegroundWindow) X(WindowFromPoint) X(GetClientRect)              \
    X(AdjustWindowRect) X(ClientToScreen) X(ScreenToClient) X(MapWindowPoints) X(PtInRect) X(ValidateRect) X(PeekMessageA)  \
    X(GetMessageA) X(TranslateMessage) X(DispatchMessageA) X(PostMessageA) X(GetMessagePos) X(SetTimer) X(LoadCursorA)      \
    X(LoadIconA) X(SetCursor) X(ShowCursor) X(ClipCursor) X(GetCursorPos) X(GetCapture) X(SetCapture) X(ReleaseCapture)     \
    X(TrackMouseEvent) X(RegisterRawInputDevices) X(GetRawInputData) X(SystemParametersInfoA) X(GetStockObject)             \
    X(GetActiveProcessorCount) X(GetActiveProcessorGroupCount) X(IsBadReadPtr)                                              \
    X(ConvertStringSecurityDescriptorToSecurityDescriptorA) X(MiniDumpWriteDump) X(GetDC) X(ReleaseDC)                      \
    X(CreateCompatibleDC) X(DeleteDC) X(CreateDIBSection) X(SelectObject) X(DeleteObject) X(GetObjectA) X(StretchBlt)       \
    X(SHGetFolderPathA) X(GetFileVersionInfoSizeW) X(waveOutGetNumDevs) X(waveOutGetDevCapsA) X(waveOutMessage)             \
    X(waveOutOpen) X(waveOutPrepareHeader) X(waveOutUnprepareHeader) X(waveOutWrite) X(waveOutReset) X(waveOutSetVolume)    \
    X(waveOutClose) X(XInputGetState) X(XInputSetState)

static const struct {
    const char *name;
    void *fn;
} uwp_win32[] = {
#define UWP_WIN32_ENTRY(f) {#f, (void *)f},
    UWP_WIN32(UWP_WIN32_ENTRY)
#undef UWP_WIN32_ENTRY
};

/* The function of this file named so, or NULL */
__declspec(dllexport) void *longshot_uwp_win32(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof uwp_win32 / sizeof *uwp_win32; ++i)
        if (!strcmp(uwp_win32[i].name, name)) return uwp_win32[i].fn;
    return NULL;
}
