// The platform table for the game module (host/uwp/uwp_platform.h): xbox/app/platform.h.
#include "platform.h"

#include <windows.h>
#include <xaudio2.h>

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <new>

#include "dcomp_shim.h"

namespace longshot_xbox {

namespace {

using namespace winrt;
using winrt::Windows::UI::Core::CoreDispatcherPriority;
namespace wgi = winrt::Windows::Gaming::Input;

winrt::Windows::UI::Core::CoreWindow g_window{nullptr};
PlatformPaths g_paths;
std::function<void(const std::string&)> g_note;
volatile uint8_t g_keys[256];
std::atomic<int> g_width{1920}, g_height{1080}, g_refresh_mhz{0};
std::mutex g_events_mu;
std::deque<LongshotUwpEvent> g_events;
LongshotUwpPlatform g_table{};

void push(int type, int a = 0, int b = 0) {
    std::lock_guard lk(g_events_mu);
    if (g_events.size() < 1024) g_events.push_back({type, a, b});
}

std::wstring widen(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// (WinRT's window, clipboard and launcher calls belong on the window's thread: run there, waited for)
template <class F>
void on_window_thread(F&& f) {
    if (!g_window) return;
    auto d = g_window.Dispatcher();
    if (d.HasThreadAccess()) {
        f();
        return;
    }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    d.RunAsync(CoreDispatcherPriority::Normal, [&] {
        try { f(); } catch (...) {}
        SetEvent(done);
    });
    WaitForSingleObject(done, 5000);
    CloseHandle(done);
}

// ---- the table's callbacks ----

void window_size(int* w, int* h) { *w = g_width, *h = g_height; }
int refresh_mhz() { return g_refresh_mhz; }

int poll_event(LongshotUwpEvent* out) {
    std::lock_guard lk(g_events_mu);
    if (g_events.empty()) return 0;
    *out = g_events.front();
    g_events.pop_front();
    return 1;
}

int clipboard_get(char* out, size_t size) {
    std::string text;
    on_window_thread([&] {
        auto content = winrt::Windows::ApplicationModel::DataTransfer::Clipboard::GetContent();
        if (content.Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::Text()))
            text = narrow(content.GetTextAsync().get());
    });
    if (text.empty() || !size) return 0;
    snprintf(out, size, "%s", text.c_str());
    return 1;
}

void clipboard_set(const char* utf8) {
    std::wstring w = widen(utf8);
    on_window_thread([&] {
        winrt::Windows::ApplicationModel::DataTransfer::DataPackage p;
        p.SetText(w);
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(p);
    });
}

void open_uri(const char* utf8) {
    std::wstring w = widen(utf8);
    on_window_thread([&] { winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(w)); });
}

void log_line(const char* utf8) {
    OutputDebugStringA(utf8);
    OutputDebugStringA("\n");
}

void message(const char* title, const char* utf8) {
    log_line(utf8);
    if (g_note) g_note(std::string(title && *title ? title : "Longshot") + ": " + (utf8 ? utf8 : ""));
}

// ---- sound: one XAudio2 source voice per stream ----

struct Stream : IXAudio2VoiceCallback {
    IXAudio2SourceVoice* voice = nullptr;
    std::atomic<int> queued{0};
    std::mutex drain_mutex;
    std::condition_variable drained;
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) override {
        auto* job = static_cast<std::pair<void (*)(void*), void*>*>(ctx);
        if (job) {
            if (job->first) job->first(job->second);
            delete job;
        }
        {
            std::lock_guard lk(drain_mutex);
            --queued;
        }
        drained.notify_all();
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

IXAudio2* g_xaudio = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
std::mutex g_audio_mutex;

void log_audio_failure(const char* operation, HRESULT result) {
    char line[128];
    std::snprintf(line, sizeof(line), "audio: %s failed (0x%08lx)", operation, static_cast<unsigned long>(result));
    log_line(line);
}

void* audio_open(uint32_t rate, uint32_t channels, uint32_t bits) {
    if (!rate || rate > 192000 || !channels || channels > 8 || bits != 16) return nullptr;
    {
        std::lock_guard lk(g_audio_mutex);
        if (!g_xaudio) {
            const HRESULT created = XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
            if (FAILED(created)) {
                log_audio_failure("XAudio2Create", created);
                g_xaudio = nullptr;
                return nullptr;
            }
        }
        if (!g_master) {
            const HRESULT created = g_xaudio->CreateMasteringVoice(&g_master);
            if (FAILED(created) || !g_master) {
                if (FAILED(created)) log_audio_failure("CreateMasteringVoice", created);
                if (g_master) g_master->DestroyVoice();
                g_master = nullptr;
                g_xaudio->Release();
                g_xaudio = nullptr;
                return nullptr;
            }
        }
    }
    WAVEFORMATEX f{WAVE_FORMAT_PCM, WORD(channels), rate, rate * channels * bits / 8, WORD(channels * bits / 8), WORD(bits), 0};
    auto* s = new (std::nothrow) Stream;
    if (!s) return nullptr;
    HRESULT result = g_xaudio->CreateSourceVoice(&s->voice, &f, 0, XAUDIO2_DEFAULT_FREQ_RATIO, s);
    if (FAILED(result)) {
        log_audio_failure("CreateSourceVoice", result);
        delete s;
        return nullptr;
    }
    result = s->voice->Start();
    if (FAILED(result)) {
        log_audio_failure("source voice Start", result);
        s->voice->DestroyVoice();
        delete s;
        return nullptr;
    }
    return s;
}

int audio_submit(void* stream, const void* data, uint32_t bytes, void (*done)(void*), void* ctx) {
    if (!stream || !data || !bytes || !done) return 0;
    auto* s = static_cast<Stream*>(stream);
    auto* job = new (std::nothrow) std::pair<void (*)(void*), void*>(done, ctx);
    if (!job) return 0;
    XAUDIO2_BUFFER b{};
    b.AudioBytes = bytes;
    b.pAudioData = static_cast<const BYTE*>(data);
    b.pContext = job;
    ++s->queued;
    const HRESULT result = s->voice->SubmitSourceBuffer(&b);
    if (FAILED(result)) {
        --s->queued;
        delete job;
        log_audio_failure("SubmitSourceBuffer", result);
        return 0;
    }
    return 1;
}

void audio_reset(void* stream) {
    if (!stream) return;
    auto* s = static_cast<Stream*>(stream);
    const HRESULT stopped = s->voice->Stop();
    const HRESULT flushed = s->voice->FlushSourceBuffers();
    const HRESULT started = s->voice->Start();
    if (FAILED(stopped) || FAILED(flushed) || FAILED(started)) {
        if (FAILED(stopped)) log_audio_failure("source voice Stop", stopped);
        if (FAILED(flushed)) log_audio_failure("FlushSourceBuffers", flushed);
        if (FAILED(started)) log_audio_failure("source voice Start", started);
        return;
    }
    std::unique_lock lk(s->drain_mutex);
    if (!s->drained.wait_for(lk, std::chrono::seconds(5), [s] { return s->queued.load() == 0; }))
        log_line("audio: source voice buffers did not drain after reset; keeping the stream alive");
}

void audio_volume(void* stream, float v) {
    if (!stream) return;
    const HRESULT result = static_cast<Stream*>(stream)->voice->SetVolume(v);
    if (FAILED(result)) log_audio_failure("source voice SetVolume", result);
}

void audio_close(void* stream) {
    if (!stream) return;
    auto* s = static_cast<Stream*>(stream);
    audio_reset(s);
    if (s->queued.load() != 0) return;
    s->voice->DestroyVoice();
    delete s;
}

// ---- gamepads (Windows.Gaming.Input) ----

int gamepad_read(uint32_t user_index, LongshotUwpGamepad* out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof *out);
    try {
        auto pads = wgi::Gamepad::Gamepads();
        if (user_index >= pads.Size()) return 0;
        auto pad = pads.GetAt(user_index);
        auto r = pad.GetCurrentReading();
        out->connected = 1;
        uint32_t b = 0;
        auto held = [&](wgi::GamepadButtons btn) { return (r.Buttons & btn) != wgi::GamepadButtons::None; };
        if (held(wgi::GamepadButtons::DPadUp)) b |= 0x0001;
        if (held(wgi::GamepadButtons::DPadDown)) b |= 0x0002;
        if (held(wgi::GamepadButtons::DPadLeft)) b |= 0x0004;
        if (held(wgi::GamepadButtons::DPadRight)) b |= 0x0008;
        if (held(wgi::GamepadButtons::Menu)) b |= 0x0010;
        if (held(wgi::GamepadButtons::View)) b |= 0x0020;
        if (held(wgi::GamepadButtons::LeftThumbstick)) b |= 0x0040;
        if (held(wgi::GamepadButtons::RightThumbstick)) b |= 0x0080;
        if (held(wgi::GamepadButtons::LeftShoulder)) b |= 0x0100;
        if (held(wgi::GamepadButtons::RightShoulder)) b |= 0x0200;
        if (held(wgi::GamepadButtons::A)) b |= 0x1000;
        if (held(wgi::GamepadButtons::B)) b |= 0x2000;
        if (held(wgi::GamepadButtons::X)) b |= 0x4000;
        if (held(wgi::GamepadButtons::Y)) b |= 0x8000;
        out->buttons = b;
        out->left_trigger = (uint8_t)(std::clamp(r.LeftTrigger, 0.0, 1.0) * 255.0);
        out->right_trigger = (uint8_t)(std::clamp(r.RightTrigger, 0.0, 1.0) * 255.0);
        out->thumb_lx = (int16_t)(std::clamp(r.LeftThumbstickX, -1.0, 1.0) * 32767.0);
        out->thumb_ly = (int16_t)(std::clamp(r.LeftThumbstickY, -1.0, 1.0) * 32767.0);
        out->thumb_rx = (int16_t)(std::clamp(r.RightThumbstickX, -1.0, 1.0) * 32767.0);
        out->thumb_ry = (int16_t)(std::clamp(r.RightThumbstickY, -1.0, 1.0) * 32767.0);
        return 1;
    } catch (...) {
        return 0;
    }
}

void gamepad_rumble(uint32_t user_index, uint16_t left_motor, uint16_t right_motor) {
    try {
        auto pads = wgi::Gamepad::Gamepads();
        if (user_index >= pads.Size()) return;
        wgi::GamepadVibration vib{};
        vib.LeftMotor = double(left_motor) / 65535.0;
        vib.RightMotor = double(right_motor) / 65535.0;
        pads.GetAt(user_index).Vibration(vib);
    } catch (...) {}
}

}  // namespace

const LongshotUwpPlatform* platform_init(winrt::Windows::UI::Core::CoreWindow const& window, const PlatformPaths& paths, uint32_t model,
                                         uint64_t memory_limit, std::function<void(const std::string&)> note) {
    g_window = window;
    g_paths = paths;
    g_note = std::move(note);
    try {
        auto di = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
        auto b = window.Bounds();
        const double scale = di.RawPixelsPerViewPixel();
        g_width = int(b.Width * scale), g_height = int(b.Height * scale);
    } catch (...) {}
    g_table.version = LONGSHOT_UWP_PLATFORM_VERSION;
    g_table.window_size = window_size;
    g_table.refresh_mhz = refresh_mhz;
    g_table.create_vk_surface = dcomp::create_vk_surface;   // (Dozen's, on no window: its swap chain comes back through DComp)
    g_table.poll_event = poll_event;
    g_table.keys = g_keys;
    g_table.gamepad_read = gamepad_read;
    g_table.gamepad_rumble = gamepad_rumble;
    g_table.clipboard_get = clipboard_get;
    g_table.clipboard_set = clipboard_set;
    g_table.open_uri = open_uri;
    g_table.message = message;
    g_table.log = log_line;
    g_table.audio_open = audio_open;
    g_table.audio_submit = audio_submit;
    g_table.audio_reset = audio_reset;
    g_table.audio_volume = audio_volume;
    g_table.audio_close = audio_close;
    g_table.local_folder = g_paths.local_folder.c_str();
    g_table.game_folder = g_paths.game_folder.c_str();
    g_table.work_folder = g_paths.work_folder.c_str();
    g_table.model = model;
    g_table.memory_limit = memory_limit;
    return &g_table;
}

void platform_on_key(unsigned vk, bool down) {
    if (vk < 256) g_keys[vk] = down ? 1 : 0;
}
void platform_on_char(uint32_t c) { push(LONGSHOT_UWP_EV_TEXT, int(c)); }
void platform_on_resize(int w, int h) {
    g_width = w, g_height = h;
    push(LONGSHOT_UWP_EV_RESIZE, w, h);
}
void platform_on_focus(bool f) {
    if (!f)
        for (auto& k : g_keys) k = 0;
    push(LONGSHOT_UWP_EV_FOCUS, f ? 1 : 0);
}
void platform_on_suspend() { push(LONGSHOT_UWP_EV_SUSPEND); }
void platform_on_resume() { push(LONGSHOT_UWP_EV_RESUME); }
void platform_on_close() { push(LONGSHOT_UWP_EV_CLOSE); }

}  // namespace longshot_xbox
