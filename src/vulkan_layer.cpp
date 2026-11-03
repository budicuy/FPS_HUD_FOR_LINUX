#include "config.hpp"
#include "input_manager.hpp"
#include "overlay_ui.hpp"
#include "recorder_ipc.hpp"
#include "telemetry.hpp"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

template <typename T>
void* get_dispatch_key(T obj) {
    return *reinterpret_cast<void**>(obj);
}

template <typename ChildT, typename ParentT>
void copy_dispatch_key(ChildT child, ParentT parent) {
    if (child && parent) {
        *reinterpret_cast<void**>(child) = *reinterpret_cast<void**>(parent);
    }
}

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
};

struct FrameResources {
    VkImage image = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    InstanceData* instance_data = nullptr;
    uint32_t graphics_queue_family = 0;
    VkQueue graphics_queue = VK_NULL_HANDLE;

    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;

    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
};

struct SwapchainData {
    std::mutex mutex;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    DeviceData* device_data = nullptr;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    std::vector<FrameResources> frames;
    ImGuiContext* imgui_context = nullptr;
    bool imgui_initialized = false;
};

std::mutex g_map_mutex;
std::unordered_map<void*, InstanceData*> g_instance_map;
std::unordered_map<void*, DeviceData*> g_device_map;
std::unordered_map<VkSwapchainKHR, SwapchainData*> g_swapchain_map;

InstanceData* find_instance_data(void* key) {
    std::lock_guard<std::mutex> lock(g_map_mutex);
    auto it = g_instance_map.find(key);
    return (it != g_instance_map.end()) ? it->second : nullptr;
}

DeviceData* find_device_data(void* key) {
    std::lock_guard<std::mutex> lock(g_map_mutex);
    auto it = g_device_map.find(key);
    return (it != g_device_map.end()) ? it->second : nullptr;
}

// Wrapper for vkAllocateCommandBuffers called by ImGui so child VkCommandBuffer gets the loader dispatch table pointer
VKAPI_ATTR VkResult VKAPI_CALL ImGui_AllocateCommandBuffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers) {

    DeviceData* dd = find_device_data(get_dispatch_key(device));
    if (!dd || !dd->AllocateCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = dd->AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
    if (res == VK_SUCCESS && pAllocateInfo && pCommandBuffers) {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            copy_dispatch_key(pCommandBuffers[i], device);
        }
    }
    return res;
}

bool is_instance_level_function(const char* name) {
    return std::strncmp(name, "vkGetPhysicalDevice", 19) == 0 ||
           std::strncmp(name, "vkEnumerate", 11) == 0 ||
           std::strcmp(name, "vkCreateInstance") == 0 ||
           std::strcmp(name, "vkDestroyInstance") == 0 ||
           std::strcmp(name, "vkDestroySurfaceKHR") == 0 ||
           std::strcmp(name, "vkCreateDevice") == 0;
}

PFN_vkVoidFunction imgui_vk_loader(const char* function_name, void* user_data) {
    auto* dd = static_cast<DeviceData*>(user_data);
    if (!dd || !function_name) return nullptr;

    if (std::strcmp(function_name, "vkAllocateCommandBuffers") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&ImGui_AllocateCommandBuffers);
    }

    if (is_instance_level_function(function_name)) {
        if (dd->instance_data && dd->instance_data->GetInstanceProcAddr) {
            return dd->instance_data->GetInstanceProcAddr(dd->instance_data->instance, function_name);
        }
        return nullptr;
    }

    if (dd->GetDeviceProcAddr) {
        PFN_vkVoidFunction fn = dd->GetDeviceProcAddr(dd->device, function_name);
        if (fn) return fn;
    }

    if (dd->instance_data && dd->instance_data->GetInstanceProcAddr) {
        return dd->instance_data->GetInstanceProcAddr(dd->instance_data->instance, function_name);
    }
    return nullptr;
}

void destroy_swapchain_data(SwapchainData* sd) {
    if (!sd || !sd->device_data) return;
    DeviceData* dd = sd->device_data;

    if (dd->DeviceWaitIdle) {
        dd->DeviceWaitIdle(dd->device);
    }

    if (sd->imgui_context) {
        ImGui::SetCurrentContext(sd->imgui_context);
        if (sd->imgui_initialized) {
            ImGui_ImplVulkan_Shutdown();
            sd->imgui_initialized = false;
        }
        ImGui::DestroyContext(sd->imgui_context);
        sd->imgui_context = nullptr;
    }

    for (auto& f : sd->frames) {
        if (f.fence && dd->DestroyFence) dd->DestroyFence(dd->device, f.fence, nullptr);
        if (f.semaphore && dd->DestroySemaphore) dd->DestroySemaphore(dd->device, f.semaphore, nullptr);
        if (f.command_pool && dd->DestroyCommandPool) dd->DestroyCommandPool(dd->device, f.command_pool, nullptr);
        if (f.framebuffer && dd->DestroyFramebuffer) dd->DestroyFramebuffer(dd->device, f.framebuffer, nullptr);
        if (f.image_view && dd->DestroyImageView) dd->DestroyImageView(dd->device, f.image_view, nullptr);
    }
    sd->frames.clear();

    if (sd->render_pass && dd->DestroyRenderPass) {
        dd->DestroyRenderPass(dd->device, sd->render_pass, nullptr);
        sd->render_pass = VK_NULL_HANDLE;
    }

    delete sd;
}

bool init_swapchain_overlay(SwapchainData* sd) {
    DeviceData* dd = sd->device_data;
    if (!dd || !dd->graphics_queue) return false;

    uint32_t img_count = 0;
    if (dd->GetSwapchainImagesKHR(dd->device, sd->swapchain, &img_count, nullptr) != VK_SUCCESS || img_count == 0) {
        return false;
    }
    std::vector<VkImage> images(img_count);
    if (dd->GetSwapchainImagesKHR(dd->device, sd->swapchain, &img_count, images.data()) != VK_SUCCESS) {
        return false;
    }

    VkAttachmentDescription attachment{};
    attachment.format = sd->format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_attachment{};
    color_attachment.attachment = 0;
    color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;

    if (dd->CreateRenderPass(dd->device, &rp_info, nullptr, &sd->render_pass) != VK_SUCCESS) {
        return false;
    }

    sd->frames.resize(img_count);
    for (uint32_t i = 0; i < img_count; ++i) {
        FrameResources& f = sd->frames[i];
        f.image = images[i];

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = f.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = sd->format;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        if (dd->CreateImageView(dd->device, &view_info, nullptr, &f.image_view) != VK_SUCCESS) {
            return false;
        }

        VkFramebufferCreateInfo fb_info{};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass = sd->render_pass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = &f.image_view;
        fb_info.width = sd->extent.width;
        fb_info.height = sd->extent.height;
        fb_info.layers = 1;

        if (dd->CreateFramebuffer(dd->device, &fb_info, nullptr, &f.framebuffer) != VK_SUCCESS) {
            return false;
        }

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = dd->graphics_queue_family;

        if (dd->CreateCommandPool(dd->device, &pool_info, nullptr, &f.command_pool) != VK_SUCCESS) {
            return false;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = f.command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        if (dd->AllocateCommandBuffers(dd->device, &alloc_info, &f.command_buffer) != VK_SUCCESS) {
            return false;
        }
        // Crucial for Vulkan Layers: initialize dispatch table pointer on newly allocated command buffer
        copy_dispatch_key(f.command_buffer, dd->device);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (dd->CreateFence(dd->device, &fence_info, nullptr, &f.fence) != VK_SUCCESS) {
            return false;
        }

        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (dd->CreateSemaphore(dd->device, &sem_info, nullptr, &f.semaphore) != VK_SUCCESS) {
            return false;
        }
    }

    sd->imgui_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(sd->imgui_context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    fpshud::setup_overlay_font(io);

    ImGui_ImplVulkan_LoadFunctions(&imgui_vk_loader, dd);

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = dd->instance_data->instance;
    init_info.PhysicalDevice = dd->physical_device;
    init_info.Device = dd->device;
    init_info.QueueFamily = dd->graphics_queue_family;
    init_info.Queue = dd->graphics_queue;
    init_info.RenderPass = sd->render_pass;
    init_info.MinImageCount = img_count >= 2 ? img_count : 2;
    init_info.ImageCount = img_count >= 2 ? img_count : 2;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.DescriptorPoolSize = 16;

    sd->imgui_initialized = ImGui_ImplVulkan_Init(&init_info);
    return sd->imgui_initialized;
}

// ============================================================================
// Intercepted Vulkan Functions
// ============================================================================

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {

    VkLayerInstanceCreateInfo* chain_info = const_cast<VkLayerInstanceCreateInfo*>(
        reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext));

    while (chain_info && !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                           chain_info->function == VK_LAYER_LINK_INFO)) {
        chain_info = const_cast<VkLayerInstanceCreateInfo*>(
            reinterpret_cast<const VkLayerInstanceCreateInfo*>(chain_info->pNext));
    }

    if (!chain_info || !chain_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    auto fpCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        fpGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!fpCreateInstance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) {
        return res;
    }

    auto* id = new InstanceData();
    id->instance = *pInstance;
    id->GetInstanceProcAddr = fpGetInstanceProcAddr;
    id->DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        fpGetInstanceProcAddr(*pInstance, "vkDestroyInstance"));
    id->GetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties"));

    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_instance_map[get_dispatch_key(*pInstance)] = id;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator) {

    InstanceData* id = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_map_mutex);
        auto it = g_instance_map.find(get_dispatch_key(instance));
        if (it != g_instance_map.end()) {
            id = it->second;
            g_instance_map.erase(it);
        }
    }

    if (id) {
        if (id->DestroyInstance) {
            id->DestroyInstance(instance, pAllocator);
        }
        delete id;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {

    VkLayerDeviceCreateInfo* chain_info = const_cast<VkLayerDeviceCreateInfo*>(
        reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext));

    while (chain_info && !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                           chain_info->function == VK_LAYER_LINK_INFO)) {
        chain_info = const_cast<VkLayerDeviceCreateInfo*>(
            reinterpret_cast<const VkLayerDeviceCreateInfo*>(chain_info->pNext));
    }

    if (!chain_info || !chain_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    auto fpCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
        fpGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!fpCreateDevice) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = fpCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) {
        return res;
    }

    auto* dd = new DeviceData();
    dd->device = *pDevice;
    dd->physical_device = physicalDevice;
    dd->GetDeviceProcAddr = fpGetDeviceProcAddr;
    dd->instance_data = find_instance_data(get_dispatch_key(physicalDevice));

    #define LOAD_DEV_FN(name) dd->name = reinterpret_cast<PFN_vk##name>(fpGetDeviceProcAddr(*pDevice, "vk" #name))
    LOAD_DEV_FN(DestroyDevice);
    LOAD_DEV_FN(GetDeviceQueue);
    LOAD_DEV_FN(CreateSwapchainKHR);
    LOAD_DEV_FN(DestroySwapchainKHR);
    LOAD_DEV_FN(GetSwapchainImagesKHR);
    LOAD_DEV_FN(QueuePresentKHR);
    LOAD_DEV_FN(QueueSubmit);
    LOAD_DEV_FN(DeviceWaitIdle);
    LOAD_DEV_FN(CreateRenderPass);
    LOAD_DEV_FN(DestroyRenderPass);
    LOAD_DEV_FN(CreateImageView);
    LOAD_DEV_FN(DestroyImageView);
    LOAD_DEV_FN(CreateFramebuffer);
    LOAD_DEV_FN(DestroyFramebuffer);
    LOAD_DEV_FN(CreateCommandPool);
    LOAD_DEV_FN(DestroyCommandPool);
    LOAD_DEV_FN(AllocateCommandBuffers);
    LOAD_DEV_FN(ResetCommandBuffer);
    LOAD_DEV_FN(BeginCommandBuffer);
    LOAD_DEV_FN(EndCommandBuffer);
    LOAD_DEV_FN(CmdBeginRenderPass);
    LOAD_DEV_FN(CmdEndRenderPass);
    LOAD_DEV_FN(CreateFence);
    LOAD_DEV_FN(DestroyFence);
    LOAD_DEV_FN(WaitForFences);
    LOAD_DEV_FN(ResetFences);
    LOAD_DEV_FN(CreateSemaphore);
    LOAD_DEV_FN(DestroySemaphore);
    #undef LOAD_DEV_FN

    uint32_t chosen_family = 0;
    if (dd->instance_data && dd->instance_data->GetPhysicalDeviceQueueFamilyProperties) {
        uint32_t qf_count = 0;
        dd->instance_data->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qf_props(qf_count);
        dd->instance_data->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qf_count, qf_props.data());

        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
            uint32_t fam = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
            if (fam < qf_count && (qf_props[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                chosen_family = fam;
                break;
            }
        }
    } else if (pCreateInfo->queueCreateInfoCount > 0) {
        chosen_family = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
    }

    dd->graphics_queue_family = chosen_family;
    if (dd->GetDeviceQueue) {
        dd->GetDeviceQueue(*pDevice, dd->graphics_queue_family, 0, &dd->graphics_queue);
        copy_dispatch_key(dd->graphics_queue, *pDevice);
    }

    {
        std::lock_guard<std::mutex> lock(g_map_mutex);
        g_device_map[get_dispatch_key(*pDevice)] = dd;
        if (dd->graphics_queue) {
            g_device_map[get_dispatch_key(dd->graphics_queue)] = dd;
        }
    }

    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {

    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_map_mutex);
        auto it = g_device_map.find(get_dispatch_key(device));
        if (it != g_device_map.end()) {
            dd = it->second;
            g_device_map.erase(it);
        }
    }

    if (dd) {
        if (dd->DestroyDevice) {
            dd->DestroyDevice(device, pAllocator);
        }
        delete dd;
    }
}

VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue) {

    DeviceData* dd = find_device_data(get_dispatch_key(device));
    if (dd && dd->GetDeviceQueue) {
        dd->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
        if (pQueue && *pQueue) {
            copy_dispatch_key(*pQueue, device);
            std::lock_guard<std::mutex> lock(g_map_mutex);
            g_device_map[get_dispatch_key(*pQueue)] = dd;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain) {

    DeviceData* dd = find_device_data(get_dispatch_key(device));
    if (!dd || !dd->CreateSwapchainKHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE) {
        SwapchainData* old_sd = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            auto it = g_swapchain_map.find(pCreateInfo->oldSwapchain);
            if (it != g_swapchain_map.end()) {
                old_sd = it->second;
                g_swapchain_map.erase(it);
            }
        }
        if (old_sd) {
            destroy_swapchain_data(old_sd);
        }
    }

    VkSwapchainCreateInfoKHR modified_info = *pCreateInfo;
    modified_info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkResult res = dd->CreateSwapchainKHR(device, &modified_info, pAllocator, pSwapchain);
    if (res != VK_SUCCESS) {
        res = dd->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        if (res != VK_SUCCESS) return res;
    }

    if (!fpshud::get_global_config().enabled) {
        return VK_SUCCESS;
    }

    auto* sd = new SwapchainData();
    sd->swapchain = *pSwapchain;
    sd->device_data = dd;
    sd->extent = pCreateInfo->imageExtent;
    sd->format = pCreateInfo->imageFormat;

    if (init_swapchain_overlay(sd)) {
        std::lock_guard<std::mutex> lock(g_map_mutex);
        g_swapchain_map[*pSwapchain] = sd;
    } else {
        destroy_swapchain_data(sd);
    }

    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator) {

    DeviceData* dd = find_device_data(get_dispatch_key(device));
    SwapchainData* sd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_map_mutex);
        auto sit = g_swapchain_map.find(swapchain);
        if (sit != g_swapchain_map.end()) {
            sd = sit->second;
            g_swapchain_map.erase(sit);
        }
    }

    if (sd) {
        destroy_swapchain_data(sd);
    }

    if (dd && dd->DestroySwapchainKHR) {
        dd->DestroySwapchainKHR(device, swapchain, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo) {

    DeviceData* dd = find_device_data(get_dispatch_key(queue));
    if (!dd || !dd->QueuePresentKHR) {
        return VK_ERROR_DEVICE_LOST;
    }

    auto& cfg = fpshud::get_mutable_config();
    if (!cfg.enabled || !pPresentInfo || pPresentInfo->swapchainCount == 0) {
        return dd->QueuePresentKHR(queue, pPresentInfo);
    }

    auto& telemetry = fpshud::get_telemetry();
    telemetry.set_render_api("VULKAN");
    telemetry.on_frame();

    VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
    uint32_t image_idx = pPresentInfo->pImageIndices[0];

    SwapchainData* sd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_map_mutex);
        auto it = g_swapchain_map.find(swapchain);
        if (it != g_swapchain_map.end()) sd = it->second;
    }

    if (!sd || !sd->imgui_initialized || image_idx >= sd->frames.size()) {
        return dd->QueuePresentKHR(queue, pPresentInfo);
    }

    std::lock_guard<std::mutex> render_lock(sd->mutex);
    FrameResources& frame = sd->frames[image_idx];

    dd->WaitForFences(dd->device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
    dd->ResetFences(dd->device, 1, &frame.fence);
    dd->ResetCommandBuffer(frame.command_buffer, 0);

    ImGui::SetCurrentContext(sd->imgui_context);
    ImGuiIO& io = ImGui::GetIO();
    float w = static_cast<float>(sd->extent.width);
    float h = static_cast<float>(sd->extent.height);
    io.DisplaySize = ImVec2(w, h);

    {
        std::lock_guard<std::mutex> cfg_lock(fpshud::get_config_mutex());
        fpshud::get_input_manager().update_for_frame(io, cfg, w, h);

        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();

        fpshud::render_overlay_ui(cfg, telemetry.get_snapshot(), w, h);

        ImGui::Render();
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dd->BeginCommandBuffer(frame.command_buffer, &begin_info);

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = sd->render_pass;
    rp_begin.framebuffer = frame.framebuffer;
    rp_begin.renderArea.extent = sd->extent;
    dd->CmdBeginRenderPass(frame.command_buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.command_buffer);

    dd->CmdEndRenderPass(frame.command_buffer);
    dd->EndCommandBuffer(frame.command_buffer);

    VkPipelineStageFlags stack_wait_stages[8] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
    };
    uint32_t wait_count = (pPresentInfo->waitSemaphoreCount <= 8) ? pPresentInfo->waitSemaphoreCount : 8;

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = wait_count;
    submit_info.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
    submit_info.pWaitDstStageMask = stack_wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &frame.command_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &frame.semaphore;

    VkQueue target_queue = dd->graphics_queue ? dd->graphics_queue : queue;
    VkResult pres_res = VK_SUCCESS;
    if (dd->QueueSubmit(target_queue, 1, &submit_info, frame.fence) == VK_SUCCESS) {
        VkPresentInfoKHR modified_present = *pPresentInfo;
        modified_present.waitSemaphoreCount = 1;
        modified_present.pWaitSemaphores = &frame.semaphore;
        pres_res = dd->QueuePresentKHR(queue, &modified_present);
    } else {
        pres_res = dd->QueuePresentKHR(queue, pPresentInfo);
    }

    fpshud::RecorderController::instance().on_frame_present(sd->extent.width, sd->extent.height);
    return pres_res;
}

} // namespace

// ============================================================================
// Exported Vulkan Layer Entrypoints
// ============================================================================

extern "C" {

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Hook_DestroyDevice);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Hook_GetDeviceQueue);

    if (fpshud::get_global_config().enabled) {
        if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Hook_CreateSwapchainKHR);
        if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Hook_DestroySwapchainKHR);
        if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Hook_QueuePresentKHR);
    }

    if (!device) return nullptr;

    DeviceData* dd = find_device_data(get_dispatch_key(device));
    if (dd && dd->GetDeviceProcAddr) {
        return dd->GetDeviceProcAddr(device, pName);
    }
    return nullptr;
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Hook_CreateInstance);
    if (std::strcmp(pName, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Hook_DestroyInstance);
    if (std::strcmp(pName, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Hook_CreateDevice);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);

    if (fpshud::get_global_config().enabled) {
        if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Hook_CreateSwapchainKHR);
        if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Hook_DestroySwapchainKHR);
        if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Hook_QueuePresentKHR);
    }

    if (!instance) return nullptr;

    InstanceData* id = find_instance_data(get_dispatch_key(instance));
    if (id && id->GetInstanceProcAddr) {
        return id->GetInstanceProcAddr(instance, pName);
    }
    return nullptr;
}

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (pVersionStruct->loaderLayerInterfaceVersion > 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    }

    pVersionStruct->pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = &vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

    return VK_SUCCESS;
}

} // extern "C"
