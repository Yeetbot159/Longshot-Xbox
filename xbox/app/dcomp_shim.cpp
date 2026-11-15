// DComp.DLL's three objects for Dozen: xbox/app/dcomp_shim.h.
#include "dcomp_shim.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "modules.h"

namespace longshot_xbox::dcomp {

namespace {

std::atomic<Show> g_show{nullptr};

// (COM objects by hand: a vtable of dcomp.h's slots, IUnknown's three first. The slots Dozen does not call answer
// E_NOTIMPL; on x64 one calling convention serves every signature)
struct Obj {
    void* const* vtbl;
    std::atomic<LONG> refs{1};
    void (*destroy)(Obj*);
};

HRESULT STDMETHODCALLTYPE query(Obj* o, REFIID, void** out) {
    if (!out) return E_POINTER;
    ++o->refs;
    *out = o;
    return S_OK;
}
ULONG STDMETHODCALLTYPE add_ref(Obj* o) { return ULONG(++o->refs); }
ULONG STDMETHODCALLTYPE release(Obj* o) {
    const LONG n = --o->refs;
    if (!n) o->destroy(o);
    return ULONG(n);
}
HRESULT STDMETHODCALLTYPE not_impl() { return E_NOTIMPL; }

template <size_t N>
struct Table {
    void* slot[N];
    Table() {
        for (auto& s : slot) s = (void*)not_impl;
        slot[0] = (void*)query, slot[1] = (void*)add_ref, slot[2] = (void*)release;
    }
};

// IDCompositionVisual: 20 slots, SetContent the 16th (after SetOffsetX x2, SetOffsetY x2, SetTransform x2,
// SetTransformParent, SetEffect, SetBitmapInterpolationMode, SetBorderMode, SetClip x2)
struct Visual : Obj {
    IUnknown* content = nullptr;
};
HRESULT STDMETHODCALLTYPE visual_set_content(Visual* v, IUnknown* content) {
    if (content) content->AddRef();
    if (v->content) v->content->Release();
    v->content = content;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE visual_ok() { return S_OK; }   // (offsets, modes and the rest: the visual fills the window)
const Table<20>& visual_table() {
    static Table<20> t = [] {
        Table<20> table;
        for (int i = 3; i < 15; ++i) table.slot[i] = (void*)visual_ok;
        table.slot[15] = (void*)visual_set_content;
        return table;
    }();
    return t;
}
void visual_destroy(Obj* o) {
    auto* v = static_cast<Visual*>(o);
    if (v->content) v->content->Release();
    delete v;
}

// IDCompositionTarget: SetRoot
struct Target : Obj {
    Visual* root = nullptr;
};
HRESULT STDMETHODCALLTYPE target_set_root(Target* t, Visual* v) {
    if (v) ++v->refs;
    if (t->root) release(t->root);
    t->root = v;
    return S_OK;
}
const Table<4>& target_table() {
    static Table<4> t = [] {
        Table<4> table;
        table.slot[3] = (void*)target_set_root;
        return table;
    }();
    return t;
}
void target_destroy(Obj* o) {
    auto* t = static_cast<Target*>(o);
    if (t->root) release(t->root);
    delete t;
}

// IDCompositionDevice: 27 slots; Commit, WaitForCommitCompletion, CreateTargetForHwnd, CreateVisual, CheckDeviceState
struct Device : Obj {
    Target* target = nullptr;   // (Dozen makes one per surface; the app has one window)
    IUnknown* shown = nullptr;  // (not held: compared only)
};
HRESULT STDMETHODCALLTYPE device_commit(Device* d) {
    IUnknown* now = d->target && d->target->root ? d->target->root->content : nullptr;
    if (now != d->shown) {
        d->shown = now;
        if (Show show = g_show.load()) show(now);
    }
    return S_OK;
}
HRESULT STDMETHODCALLTYPE device_wait() { return S_OK; }
HRESULT STDMETHODCALLTYPE device_target(Device* d, HWND, BOOL, Target** out) {
    if (!out) return E_POINTER;
    auto* t = new Target;
    t->vtbl = target_table().slot, t->destroy = target_destroy;
    ++t->refs;   // (the device's)
    if (d->target) release(d->target);
    d->target = t;
    *out = t;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE device_visual(Device*, Visual** out) {
    if (!out) return E_POINTER;
    auto* v = new Visual;
    v->vtbl = visual_table().slot, v->destroy = visual_destroy;
    *out = v;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE device_state(Device*, BOOL* valid) {
    if (valid) *valid = TRUE;
    return S_OK;
}
const Table<27>& device_table() {
    static Table<27> t = [] {
        Table<27> table;
        table.slot[3] = (void*)device_commit;
        table.slot[4] = (void*)device_wait;
        table.slot[6] = (void*)device_target;
        table.slot[7] = (void*)device_visual;
        table.slot[26] = (void*)device_state;
        return table;
    }();
    return t;
}
void device_destroy(Obj* o) {
    auto* d = static_cast<Device*>(o);
    if (d->target) release(d->target);
    delete d;
}

HRESULT WINAPI DCompositionCreateDevice(IUnknown*, REFIID, void** out) {
    if (!out) return E_POINTER;
    auto* d = new Device;
    d->vtbl = device_table().slot, d->destroy = device_destroy;
    *out = d;
    return S_OK;
}

}  // namespace

void set_show(Show show) { g_show = show; }

int create_vk_surface(void* instance, void* surface_out) {
    struct Win32SurfaceInfo {   // (VkWin32SurfaceCreateInfoKHR)
        int32_t type;
        const void* next;
        uint32_t flags;
        HINSTANCE instance;
        HWND window;
    };
    constexpr int32_t kWin32SurfaceInfo = 1000009000;   // (VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR)
    using gipa_t = void* (*)(void*, const char*);
    using create_t = int32_t (*)(void*, const Win32SurfaceInfo*, const void*, void*);
    void* vk = modules::load_library("vulkan-1.dll");
    auto gipa = vk ? (gipa_t)modules::proc_address(vk, "vkGetInstanceProcAddr") : nullptr;
    auto create = gipa ? (create_t)gipa(instance, "vkCreateWin32SurfaceKHR") : nullptr;
    if (!create) return 0;
    const Win32SurfaceInfo info{kWin32SurfaceInfo, nullptr, 0, GetModuleHandleW(nullptr), kWindowToken};
    return create(instance, &info, nullptr, surface_out) == 0;
}

void* lookup(const char* name) {
    if (name && !strcmp(name, "DCompositionCreateDevice")) return (void*)DCompositionCreateDevice;
    return nullptr;
}

}  // namespace longshot_xbox::dcomp
