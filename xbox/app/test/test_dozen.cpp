// Mesa's Dozen (Vulkan on Direct3D 12) through the app's module set on a PC: vulkan-1.dll asked for by name and mapped
// by hand from vulkan_dzn.dll, its thread-locals in longshot_tls.dll's slot, a Vulkan 1.3 instance and device, what of
// the renderer's needs the device has (gpu/vk/vk_main.cpp: the three it requires, the optional extensions), and a
// compute program through Dozen's SPIR-V to DXIL compiler, run on a thread started after Dozen was loaded.
//     test_dozen.exe <folder with vulkan_dzn.dll, dxil.dll, longshot_tls.dll>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dxgi1_2.h>

#include "../dcomp_shim.h"
#include "../modules.h"
#include "fill_spv.h"

namespace mod = longshot_xbox::modules;
namespace dcomp = longshot_xbox::dcomp;

static int failures;

#define CHECK(cond, what)                                      \
    do {                                                       \
        bool ok_ = (cond);                                     \
        std::printf("%s: %s\n", ok_ ? "ok  " : "FAIL", what);  \
        if (!ok_) ++failures;                                  \
    } while (0)

static PFN_vkGetInstanceProcAddr gipa;

#define VKF(name) auto name = (PFN_##name)gipa(instance, #name)

static bool has(const std::vector<VkExtensionProperties>& all, const char* name) {
    for (const auto& e : all)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

// out[i] = i * 3 + add, 4096 values, on the device's first compute queue; the values it got wrong (-1: a call failed)
static int fill(VkInstance instance, VkPhysicalDevice phys, uint32_t family) {
    VKF(vkCreateDevice);
    VKF(vkGetDeviceProcAddr);
    VKF(vkGetPhysicalDeviceMemoryProperties);
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = f13.synchronization2 = VK_TRUE;
    f13.pNext = &f12;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family, q.queueCount = 1, q.pQueuePriorities = &prio;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &f13, di.queueCreateInfoCount = 1, di.pQueueCreateInfos = &q;
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &di, nullptr, &dev) != VK_SUCCESS) return -1;
#define VKD(name) auto name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    VKD(vkGetDeviceQueue);
    VKD(vkCreateBuffer);
    VKD(vkGetBufferMemoryRequirements);
    VKD(vkAllocateMemory);
    VKD(vkBindBufferMemory);
    VKD(vkMapMemory);
    VKD(vkCreateShaderModule);
    VKD(vkCreateDescriptorSetLayout);
    VKD(vkCreatePipelineLayout);
    VKD(vkCreateComputePipelines);
    VKD(vkCreateDescriptorPool);
    VKD(vkAllocateDescriptorSets);
    VKD(vkUpdateDescriptorSets);
    VKD(vkCreateCommandPool);
    VKD(vkAllocateCommandBuffers);
    VKD(vkBeginCommandBuffer);
    VKD(vkCmdBindPipeline);
    VKD(vkCmdBindDescriptorSets);
    VKD(vkCmdPushConstants);
    VKD(vkCmdDispatch);
    VKD(vkCmdPipelineBarrier2);
    VKD(vkEndCommandBuffer);
    VKD(vkCreateSemaphore);
    VKD(vkQueueSubmit2);
    VKD(vkWaitSemaphores);
    VKD(vkDeviceWaitIdle);
    VKD(vkDestroyDevice);
#undef VKD
    int wrong = -1;
    const uint32_t n = 4096, add = 7;
    VkQueue queue;
    vkGetDeviceQueue(dev, family, 0, &queue);

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = n * 4, bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buf;
    vkCreateBuffer(dev, &bi, nullptr, &buf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = type;
    VkDeviceMemory mem;
    uint32_t* out = nullptr;
    if (type == UINT32_MAX || vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) goto done;
    vkBindBufferMemory(dev, buf, mem, 0);
    vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&out);
    memset(out, 0xff, n * 4);

    {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = sizeof kFillSpv, si.pCode = kFillSpv;
        VkShaderModule sm;
        if (vkCreateShaderModule(dev, &si, nullptr, &sm) != VK_SUCCESS) goto done;
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1, li.pBindings = &b;
        VkDescriptorSetLayout dsl;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl);
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1, pli.pSetLayouts = &dsl, pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &pr;
        VkPipelineLayout pl;
        vkCreatePipelineLayout(dev, &pli, nullptr, &pl);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
        ci.layout = pl;
        VkPipeline pipe;
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) != VK_SUCCESS) goto done;

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1, dpi.poolSizeCount = 1, dpi.pPoolSizes = &ps;
        VkDescriptorPool dp;
        vkCreateDescriptorPool(dev, &dpi, nullptr, &dp);
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dp, dai.descriptorSetCount = 1, dai.pSetLayouts = &dsl;
        VkDescriptorSet ds;
        vkAllocateDescriptorSets(dev, &dai, &ds);
        VkDescriptorBufferInfo dbi{buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = ds, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = family;
        VkCommandPool cp;
        vkCreateCommandPool(dev, &cpi, nullptr, &cp);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = cp, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
        VkCommandBuffer cb;
        vkAllocateCommandBuffers(dev, &cai, &cb);
        VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &cbi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &add);
        vkCmdDispatch(cb, n / 64, 1, 1);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1, dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cb, &dep);
        vkEndCommandBuffer(cb);

        VkSemaphoreTypeCreateInfo sti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sci.pNext = &sti;
        VkSemaphore timeline;
        vkCreateSemaphore(dev, &sci, nullptr, &timeline);
        VkCommandBufferSubmitInfo cbs{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbs.commandBuffer = cb;
        VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        sig.semaphore = timeline, sig.value = 1, sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 sub{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        sub.commandBufferInfoCount = 1, sub.pCommandBufferInfos = &cbs, sub.signalSemaphoreInfoCount = 1, sub.pSignalSemaphoreInfos = &sig;
        if (vkQueueSubmit2(queue, 1, &sub, VK_NULL_HANDLE) != VK_SUCCESS) goto done;
        const uint64_t value = 1;
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1, wi.pSemaphores = &timeline, wi.pValues = &value;
        if (vkWaitSemaphores(dev, &wi, 5'000'000'000ull) != VK_SUCCESS) goto done;
        wrong = 0;
        for (uint32_t i = 0; i < n; ++i) wrong += out[i] != i * 3 + add;
    }
done:
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, nullptr);   // (the test's other objects go with the process)
    return wrong;
}

// ---- presenting, as on the console: a surface on no window, the window's size from the game's GetClientRect, the swap
// chain handed over by DComp.DLL (the app's, dcomp_shim.cpp) ----

const HWND kToken = dcomp::kWindowToken;
constexpr LONG kWidth = 1920, kHeight = 1080;
IUnknown* g_shown;
int g_shows;

BOOL WINAPI client_rect(HWND w, RECT* r) {   // (host/uwp/win32_uwp.c's: the platform's window size)
    if (w != kToken || !r) return FALSE;
    *r = {0, 0, kWidth, kHeight};
    return TRUE;
}
void* win32(const char* name) { return !strcmp(name, "GetClientRect") ? (void*)client_rect : nullptr; }

void show(IUnknown* swapchain) {
    if (swapchain) swapchain->AddRef();
    if (g_shown) g_shown->Release();
    g_shown = swapchain;
    ++g_shows;
}

// A swap chain on the token window, one image cleared and presented; what failed, or nullptr
static const char* present_once(VkInstance instance, VkPhysicalDevice phys, uint32_t family) {
    VKF(vkGetPhysicalDeviceSurfaceSupportKHR);
    VKF(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    VKF(vkGetPhysicalDeviceFeatures2);
    VKF(vkCreateDevice);
    VKF(vkGetDeviceProcAddr);
    VkSurfaceKHR surface;
    if (!dcomp::create_vk_surface(instance, &surface)) return "dcomp::create_vk_surface (vkCreateWin32SurfaceKHR)";
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys, family, surface, &ok);
    if (!ok) return "the queue cannot present";
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps) != VK_SUCCESS) return "the surface's capabilities";
    if (caps.currentExtent.width != kWidth || caps.currentExtent.height != kHeight) return "the surface's size is not GetClientRect's";

    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.pNext = &f11, f11.pNext = &f12, f12.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(phys, &features);
    features.features.robustBufferAccess = VK_FALSE;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family, q.queueCount = 1, q.pQueuePriorities = &prio;
    const char* ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &features, di.queueCreateInfoCount = 1, di.pQueueCreateInfos = &q, di.enabledExtensionCount = 1, di.ppEnabledExtensionNames = &ext;
    VkDevice dev;
    if (vkCreateDevice(phys, &di, nullptr, &dev) != VK_SUCCESS) return "vkCreateDevice with VK_KHR_swapchain";
#define VKD(name) auto name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    VKD(vkGetDeviceQueue);
    VKD(vkCreateSwapchainKHR);
    VKD(vkGetSwapchainImagesKHR);
    VKD(vkAcquireNextImageKHR);
    VKD(vkQueuePresentKHR);
    VKD(vkCreateSemaphore);
    VKD(vkDestroySemaphore);
    VKD(vkCreateImageView);
    VKD(vkDestroyImageView);
    VKD(vkCreateCommandPool);
    VKD(vkAllocateCommandBuffers);
    VKD(vkBeginCommandBuffer);
    VKD(vkCmdPipelineBarrier2);
    VKD(vkEndCommandBuffer);
    VKD(vkQueueSubmit2);
    VKD(vkDeviceWaitIdle);
    VKD(vkDestroySwapchainKHR);
    VKD(vkDestroyDevice);
#undef VKD
    const char* failed = nullptr;
    auto timeline_works = [&]() {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        info.pNext = &type;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        VkResult result = vkCreateSemaphore(dev, &info, nullptr, &semaphore);
        if (result == VK_SUCCESS) vkDestroySemaphore(dev, semaphore, nullptr);
        return result == VK_SUCCESS;
    };
    if (!timeline_works()) failed = "timeline semaphore before swapchain";
    VkQueue queue;
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface, ci.minImageCount = caps.minImageCount, ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, ci.imageExtent = caps.currentExtent, ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, ci.presentMode = VK_PRESENT_MODE_FIFO_KHR, ci.clipped = VK_TRUE;
    VkSwapchainKHR chain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    const int shows = g_shows;
    if (!failed && vkCreateSwapchainKHR(dev, &ci, nullptr, &chain) != VK_SUCCESS) {
        failed = "vkCreateSwapchainKHR";
    } else if (!failed && (g_shows == shows || !g_shown)) {
        failed = "DComp's Commit did not hand over the swap chain";
    } else if (!failed) {
        IDXGISwapChain1* dxgi = nullptr;
        DXGI_SWAP_CHAIN_DESC1 d{};
        if (FAILED(g_shown->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&dxgi)) || FAILED(dxgi->GetDesc1(&d)))
            failed = "what DComp was given is not a DXGI swap chain";
        else if (d.Width != UINT(kWidth) || d.Height != UINT(kHeight))
            failed = "the DXGI swap chain is not the window's size";
        if (dxgi) dxgi->Release();
    }
    if (!failed) {
        uint32_t n = 0;
        if (!timeline_works()) failed = "timeline semaphore after swapchain";
        else if (vkGetSwapchainImagesKHR(dev, chain, &n, nullptr) != VK_SUCCESS || !n)
            failed = "swapchain image count";
        else {
            images.resize(n);
            if (vkGetSwapchainImagesKHR(dev, chain, &n, images.data()) != VK_SUCCESS || !n)
                failed = "swapchain images";
            else images.resize(n);
        }
        for (auto image : images) {
            if (failed) break;
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = ci.imageFormat;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS) {
                failed = "swapchain image view";
                break;
            }
            views.push_back(view);
        }
        if (!failed && !timeline_works()) failed = "timeline semaphore after swapchain image views";
    }
    if (!failed) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore acquired, drawn;
        vkCreateSemaphore(dev, &sci, nullptr, &acquired);
        vkCreateSemaphore(dev, &sci, nullptr, &drawn);
        uint32_t index = 0;
        if (vkAcquireNextImageKHR(dev, chain, 5'000'000'000ull, acquired, VK_NULL_HANDLE, &index) != VK_SUCCESS) {
            failed = "vkAcquireNextImageKHR";
        } else {
            VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            cpi.queueFamilyIndex = family;
            VkCommandPool cp;
            vkCreateCommandPool(dev, &cpi, nullptr, &cp);
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = cp, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
            VkCommandBuffer cb;
            vkAllocateCommandBuffers(dev, &cai, &cb);
            VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            vkBeginCommandBuffer(cb, &cbi);
            VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            ib.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, ib.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, ib.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = images[index], ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1, dep.pImageMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(cb, &dep);
            vkEndCommandBuffer(cb);
            VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            wait.semaphore = acquired, wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            sig.semaphore = drawn, sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkCommandBufferSubmitInfo cbs{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            cbs.commandBuffer = cb;
            VkSubmitInfo2 sub{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            sub.waitSemaphoreInfoCount = 1, sub.pWaitSemaphoreInfos = &wait, sub.commandBufferInfoCount = 1, sub.pCommandBufferInfos = &cbs;
            sub.signalSemaphoreInfoCount = 1, sub.pSignalSemaphoreInfos = &sig;
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1, pi.pWaitSemaphores = &drawn, pi.swapchainCount = 1, pi.pSwapchains = &chain, pi.pImageIndices = &index;
            if (vkQueueSubmit2(queue, 1, &sub, VK_NULL_HANDLE) != VK_SUCCESS) failed = "the submit before the present";
            else if (vkQueuePresentKHR(queue, &pi) != VK_SUCCESS) failed = "vkQueuePresentKHR";
        }
    }
    vkDeviceWaitIdle(dev);
    for (auto view : views) vkDestroyImageView(dev, view, nullptr);
    if (chain) vkDestroySwapchainKHR(dev, chain, nullptr);
    vkDestroyDevice(dev, nullptr);
    VKF(vkDestroySurfaceKHR);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    return failed;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_dozen <folder with vulkan_dzn.dll, dxil.dll, longshot_tls.dll>\n");
        return 2;
    }
    mod::Config cfg;
    {
        const std::string a = argv[1];
        cfg.folder.assign(a.begin(), a.end());
        cfg.local_state = cfg.folder;
    }
    cfg.log = [](const std::string& s) { std::printf("      %s\n", s.c_str()); };
    mod::configure(cfg);
    mod::set_win32(win32);
    mod::add_virtual("dcomp.dll", dcomp::lookup);
    dcomp::set_show(show);
    CHECK(mod::where("DComp.DLL") == "virtual", "DComp.DLL is the app's own");

    CHECK(mod::where("vulkan-1.dll").rfind("pel ", 0) == 0, "vulkan-1.dll is Dozen, mapped by hand");
    void* vk = mod::load_library("vulkan-1.dll");
    CHECK(vk != nullptr, "vulkan_dzn.dll loads through the module set (its TLS in longshot_tls.dll's slot)");
    if (!vk) return 1;
    CHECK(mod::load_library("VULKAN-1") == vk, "asked again, by another spelling: the same module");
    gipa = (PFN_vkGetInstanceProcAddr)mod::proc_address(vk, "vkGetInstanceProcAddr");
    CHECK(gipa != nullptr, "vkGetInstanceProcAddr (Dozen's vk_icdGetInstanceProcAddr)");
    if (!gipa) return 1;
    VkInstance instance = VK_NULL_HANDLE;

    VKF(vkEnumerateInstanceVersion);
    uint32_t version = 0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
    std::printf("      instance version %u.%u.%u\n", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    CHECK(version >= VK_API_VERSION_1_2, "instance: Vulkan 1.2 (the Xbox build's kVulkanNeeded: Dozen says 1.2)");
    VKF(vkEnumerateInstanceExtensionProperties);
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> iext(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, iext.data());
    CHECK(has(iext, VK_KHR_SURFACE_EXTENSION_NAME) && has(iext, VK_KHR_WIN32_SURFACE_EXTENSION_NAME), "surface extensions");

    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Longshot", app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app, ii.enabledExtensionCount = 2, ii.ppEnabledExtensionNames = exts;
    VKF(vkCreateInstance);
    CHECK(vkCreateInstance(&ii, nullptr, &instance) == VK_SUCCESS, "vkCreateInstance");
    if (!instance) return 1;

    VKF(vkEnumeratePhysicalDevices);
    VKF(vkGetPhysicalDeviceProperties2);
    VKF(vkGetPhysicalDeviceFeatures2);
    VKF(vkEnumerateDeviceExtensionProperties);
    VKF(vkGetPhysicalDeviceQueueFamilyProperties);
    count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    CHECK(count > 0, "a Direct3D 12 device");

    for (VkPhysicalDevice phys : devices) {
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(phys, &p2);
        const auto& p = p2.properties;
        std::printf("\n      %s (%s %s), Vulkan %u.%u, %s\n", p.deviceName, drv.driverName, drv.driverInfo,
                    VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                    p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "software (WARP)" : "hardware");
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;   // (WARP: the console has no such device)
        CHECK(drv.driverID == VK_DRIVER_ID_MESA_DOZEN, "the driver is Dozen");

        VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.pNext = &mesh;
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.pNext = &f13;
        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        f11.pNext = &f12;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f11;
        vkGetPhysicalDeviceFeatures2(phys, &f2);
        CHECK(p.apiVersion >= VK_API_VERSION_1_2, "device: Vulkan 1.2");
        CHECK(f13.dynamicRendering && f13.synchronization2 && f12.timelineSemaphore,
              "device: dynamic rendering, synchronization2, timeline semaphores (the renderer's must-haves)");
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> dext(ne);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, dext.data());
        CHECK(has(dext, VK_KHR_SWAPCHAIN_EXTENSION_NAME), "device: VK_KHR_swapchain");

        // (the rest: what the renderer uses where it has them, and its fallbacks where not; a report, not a check)
        std::printf("      features: bufferDeviceAddress %d, descriptorIndexing %d, shaderInt64 %d, shaderInt16 %d, "
                    "shaderFloat16 %d, storageBuffer16BitAccess %d, subgroupSizeControl %d, maintenance4 %d, "
                    "geometryShader %d, mesh shaders %d/%d\n",
                    f12.bufferDeviceAddress, f12.descriptorIndexing, f2.features.shaderInt64, f2.features.shaderInt16,
                    f12.shaderFloat16, f11.storageBuffer16BitAccess, f13.subgroupSizeControl, f13.maintenance4,
                    f2.features.geometryShader, mesh.taskShader, mesh.meshShader);
        for (const char* e : {VK_EXT_MESH_SHADER_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
                              VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME,
                              VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
                              VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
                              VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
                              VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                              VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
                              VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME,
                              VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME,
                              VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME,
                              VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME})
            std::printf("      %-40s %s\n", e, has(dext, e) ? "yes" : "-");

        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < nq && family == UINT32_MAX; ++i)
            if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) family = i;
        CHECK(family != UINT32_MAX, "device: a compute queue");
        if (family == UINT32_MAX) continue;
        if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) continue;

        int wrong = -1;
        std::thread([&] { wrong = fill(instance, phys, family); }).join();   // (Dozen's thread-locals on a new thread)
        if (wrong > 0) std::printf("      %d of 4096 values wrong\n", wrong);
        CHECK(wrong == 0, "a compute program (SPIR-V to DXIL), timeline semaphore and submit2, on a new thread");

        const char* failed = nullptr;
        std::thread([&] { failed = present_once(instance, phys, family); }).join();
        if (failed) std::printf("      failed: %s\n", failed);
        CHECK(!failed, "a swap chain on no window, handed over by DComp.DLL at the window's size, and a present");
    }
    if (g_shown) g_shown->Release();

    VKF(vkDestroyInstance);
    vkDestroyInstance(instance, nullptr);
    std::printf("\n%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
