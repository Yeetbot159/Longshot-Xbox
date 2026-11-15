// The CoreWindow's composition tree: xbox/app/present.h.
#include "present.h"

#include <windows.h>
#include <windows.ui.composition.interop.h>

#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>

#include <atomic>
#include <memory>

namespace longshot_xbox::present {

namespace {

namespace wuc = winrt::Windows::UI::Composition;

winrt::Windows::UI::Core::CoreWindow g_window{nullptr};
wuc::Compositor g_compositor{nullptr};
wuc::CompositionTarget g_target{nullptr};
wuc::SpriteVisual g_launcher{nullptr}, g_game{nullptr};
std::atomic<bool> g_game_shown{false};

wuc::SpriteVisual full_window_visual() {
    auto v = g_compositor.CreateSpriteVisual();
    v.RelativeSizeAdjustment({1, 1});
    return v;
}

wuc::CompositionBrush brush_for(IUnknown* swapchain) {
    auto interop = g_compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    wuc::ICompositionSurface surface{nullptr};
    winrt::check_hresult(interop->CreateCompositionSurfaceForSwapChain(
        swapchain, reinterpret_cast<ABI::Windows::UI::Composition::ICompositionSurface**>(winrt::put_abi(surface))));
    auto brush = g_compositor.CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    return brush;
}

}  // namespace

void init(winrt::Windows::UI::Core::CoreWindow const& window) {
    g_window = window;
    g_compositor = wuc::Compositor();
    g_target = g_compositor.CreateTargetForCurrentView();
    auto root = g_compositor.CreateContainerVisual();
    root.RelativeSizeAdjustment({1, 1});
    g_launcher = full_window_visual();
    g_game = full_window_visual();
    g_game.IsVisible(false);
    root.Children().InsertAtBottom(g_launcher);
    root.Children().InsertAtTop(g_game);
    g_target.Root(root);
}

void set_launcher(IUnknown* swapchain) {
    if (g_compositor && swapchain) g_launcher.Brush(brush_for(swapchain));
}

static void show_game(IUnknown* swapchain) {
    try {
        if (swapchain) {
            g_game.Brush(brush_for(swapchain));
            g_game.IsVisible(true);
        } else {
            g_game.IsVisible(false);
            g_game.Brush(nullptr);
        }
        g_game_shown = swapchain != nullptr;
    } catch (...) {
    }
}

void set_game(IUnknown* swapchain) {
    if (!g_window) return;
    auto d = g_window.Dispatcher();
    if (d.HasThreadAccess()) return show_game(swapchain);
    // (the swap chain is held by the caller until this returns: Dozen's Commit, inside its vkCreateSwapchainKHR. The
    // work keeps its own reference, for the case the wait gives up)
    struct Job {
        winrt::com_ptr<IUnknown> swapchain;
        winrt::handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    };
    auto job = std::make_shared<Job>();
    job->swapchain.copy_from(swapchain);
    d.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High, [job] {
        show_game(job->swapchain.get());
        SetEvent(job->done.get());
    });
    WaitForSingleObject(job->done.get(), 5000);
}

bool game_shown() { return g_game_shown; }

}  // namespace longshot_xbox::present
