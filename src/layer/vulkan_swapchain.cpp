#include "vulkan_swapchain.hpp"

#include "config.hpp"
#include "overlay_ui.hpp"
#include "telemetry.hpp"
#include "vulkan_player_stream.hpp"

#include "backends/imgui_impl_vulkan.h"

#include <array>
#include <cstring>

namespace fpshud::vk {

std::mutex g_map_mutex;
std::mutex g_vulkan_imgui_mutex;
std::unordered_map<void*, InstanceData*> g_instance_map;
std::unordered_map<void*, DeviceData*> g_device_map;
std::unordered_map<VkSwapchainKHR, std::shared_ptr<SwapchainData>> g_swapchain_map;
std::atomic<bool> g_bound_discrete_gpu{false};

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

std::shared_ptr<SwapchainData> find_swapchain_data(VkSwapchainKHR swapchain) {
  std::lock_guard<std::mutex> lock(g_map_mutex);
  auto it = g_swapchain_map.find(swapchain);
  return (it != g_swapchain_map.end()) ? it->second : nullptr;
}

uint32_t find_memory_type_index(DeviceData* dd, uint32_t type_filter,
                                VkMemoryPropertyFlags properties) {
  if (!dd || !dd->instance_data || !dd->instance_data->GetPhysicalDeviceMemoryProperties) {
    return 0xFFFFFFFF;
  }
  VkPhysicalDeviceMemoryProperties mem_props{};
  dd->instance_data->GetPhysicalDeviceMemoryProperties(dd->physical_device, &mem_props);
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((type_filter & (1u << i)) &&
        (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  return 0xFFFFFFFF;
}

static VKAPI_ATTR VkResult VKAPI_CALL
ImGui_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo,
                             VkCommandBuffer* pCommandBuffers) {
  try {
    DeviceData* dd = find_device_data(get_dispatch_key(device));
    if (!dd || !dd->AllocateCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = dd->AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
    if (res == VK_SUCCESS && pAllocateInfo && pCommandBuffers) {
      for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
        copy_dispatch_key(pCommandBuffers[i], device);
      }
    }
    return res;
  } catch (...) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}

static bool is_instance_level_function(const char* name) {
  return std::strncmp(name, "vkGetPhysicalDevice", 19) == 0 ||
         std::strncmp(name, "vkEnumerate", 11) == 0 || std::strcmp(name, "vkCreateInstance") == 0 ||
         std::strcmp(name, "vkDestroyInstance") == 0 ||
         std::strcmp(name, "vkDestroySurfaceKHR") == 0 ||
         std::strcmp(name, "vkCreateWaylandSurfaceKHR") == 0 ||
         std::strcmp(name, "vkCreateDevice") == 0;
}

static PFN_vkVoidFunction imgui_vk_loader(const char* function_name, void* user_data) {
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

void destroy_swapchain_data(const std::shared_ptr<SwapchainData>& sd) {
  if (!sd) return;
  std::lock_guard<std::mutex> lock(sd->mutex);
  if (!sd->device_data) return;
  DeviceData* dd = sd->device_data;

  if (dd->DeviceWaitIdle) {
    dd->DeviceWaitIdle(dd->device);
  }

  if (sd->imgui_context) {
    std::lock_guard<std::mutex> imgui_lock(g_vulkan_imgui_mutex);
    ImGui::SetCurrentContext(sd->imgui_context);
    if (sd->imgui_initialized) {
      if (g_player_vk_state.owner_sd == sd.get()) {
        cleanup_player_texture(dd);
      }
      ImGui_ImplVulkan_Shutdown();
      sd->imgui_initialized = false;
    }
    ImGui::DestroyContext(sd->imgui_context);
    sd->imgui_context = nullptr;
  }

  for (auto& f : sd->frames) {
    if (f.mapped_ptr && dd->UnmapMemory) {
      dd->UnmapMemory(dd->device, f.staging_memory);
      f.mapped_ptr = nullptr;
    }
    if (f.staging_buffer && dd->DestroyBuffer) {
      dd->DestroyBuffer(dd->device, f.staging_buffer, nullptr);
      f.staging_buffer = VK_NULL_HANDLE;
    }
    if (f.staging_memory && dd->FreeMemory) {
      dd->FreeMemory(dd->device, f.staging_memory, nullptr);
      f.staging_memory = VK_NULL_HANDLE;
    }
    f.buffer_size = 0;
    f.has_valid_capture = false;

    if (f.fence && dd->DestroyFence) dd->DestroyFence(dd->device, f.fence, nullptr);
    if (f.semaphore && dd->DestroySemaphore) dd->DestroySemaphore(dd->device, f.semaphore, nullptr);
    if (f.command_pool && dd->DestroyCommandPool)
      dd->DestroyCommandPool(dd->device, f.command_pool, nullptr);
    if (f.framebuffer && dd->DestroyFramebuffer)
      dd->DestroyFramebuffer(dd->device, f.framebuffer, nullptr);
    if (f.image_view && dd->DestroyImageView)
      dd->DestroyImageView(dd->device, f.image_view, nullptr);
  }
  sd->frames.clear();

  if (sd->render_pass && dd->DestroyRenderPass) {
    dd->DestroyRenderPass(dd->device, sd->render_pass, nullptr);
    sd->render_pass = VK_NULL_HANDLE;
  }
  sd->device_data = nullptr;
}

bool init_swapchain_overlay(SwapchainData* sd) {
  DeviceData* dd = sd->device_data;
  if (!dd || !dd->graphics_queue) return false;

  // Handle zero-extent swapchains cleanly (e.g., minimized window) without registration failure
  if (sd->extent.width == 0 || sd->extent.height == 0) {
    sd->imgui_initialized = false;
    return true;
  }

  uint32_t img_count = 0;
  if (dd->GetSwapchainImagesKHR(dd->device, sd->swapchain, &img_count, nullptr) != VK_SUCCESS ||
      img_count == 0) {
    return false;
  }
  std::vector<VkImage> images(img_count);
  if (dd->GetSwapchainImagesKHR(dd->device, sd->swapchain, &img_count, images.data()) !=
      VK_SUCCESS) {
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

  std::array<VkSubpassDependency, 2> dependencies{};
  // Ingress: wait for external color attachment output before drawing HUD
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].srcAccessMask = 0;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  // Egress: transition color attachment writes to presentation engine before present
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstAccessMask = 0;

  VkRenderPassCreateInfo rp_info{};
  rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rp_info.attachmentCount = 1;
  rp_info.pAttachments = &attachment;
  rp_info.subpassCount = 1;
  rp_info.pSubpasses = &subpass;
  rp_info.dependencyCount = static_cast<uint32_t>(dependencies.size());
  rp_info.pDependencies = dependencies.data();

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
    // Crucial for Vulkan Layers: initialize dispatch table pointer on newly allocated command
    // buffer
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

  {
    std::lock_guard<std::mutex> imgui_lock(g_vulkan_imgui_mutex);
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
    if (sd->imgui_initialized) {
      register_vulkan_player_texture_backend();
    }
  }
  return sd->imgui_initialized;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice device,
                                                       const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkSwapchainKHR* pSwapchain) {
  try {
    DeviceData* dd = find_device_data(get_dispatch_key(device));
    if (!dd || !dd->CreateSwapchainKHR) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE) {
      std::shared_ptr<SwapchainData> old_sd = nullptr;
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

    // When overlay is disabled, bypass all imageUsage mutations and overlay allocations
    if (!fpshud::get_global_config().enabled) {
      return dd->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    // Ensure Telemetry is bound to the GPU that actually creates the presentation swapchain
    // (still prioritizing Discrete GPU if one was bound on hybrid/Optimus systems)
    if (dd->vendor_id != 0) {
      if (dd->device_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        g_bound_discrete_gpu.store(true, std::memory_order_relaxed);
        fpshud::get_telemetry().notify_active_gpu(dd->vendor_id, dd->device_id);
      } else if (!g_bound_discrete_gpu.load(std::memory_order_relaxed)) {
        fpshud::get_telemetry().notify_active_gpu(dd->vendor_id, dd->device_id);
      }
    }

    VkSwapchainCreateInfoKHR modified_info = *pCreateInfo;
    modified_info.imageUsage |=
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    bool supports_transfer = true;

    VkResult res = dd->CreateSwapchainKHR(device, &modified_info, pAllocator, pSwapchain);
    if (res != VK_SUCCESS) {
      modified_info.imageUsage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      res = dd->CreateSwapchainKHR(device, &modified_info, pAllocator, pSwapchain);
      supports_transfer = false;
      if (res != VK_SUCCESS) {
        res = dd->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        if (res != VK_SUCCESS) return res;
      }
    }

    auto sd = std::make_shared<SwapchainData>();
    sd->swapchain = *pSwapchain;
    sd->device_data = dd;
    sd->extent = pCreateInfo->imageExtent;
    sd->format = pCreateInfo->imageFormat;
    sd->supports_transfer_src = supports_transfer;

    if (init_swapchain_overlay(sd.get())) {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      g_swapchain_map[*pSwapchain] = sd;
    } else {
      destroy_swapchain_data(sd);
    }

    return VK_SUCCESS;
  } catch (...) {
    DeviceData* dd = find_device_data(get_dispatch_key(device));
    if (dd && dd->CreateSwapchainKHR) {
      return dd->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks* pAllocator) {
  try {
    DeviceData* dd = find_device_data(get_dispatch_key(device));
    std::shared_ptr<SwapchainData> sd = nullptr;
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
  } catch (...) {
  }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue queue, uint32_t submitCount,
                                                const VkSubmitInfo* pSubmits, VkFence fence) {
  try {
    if (!queue) return VK_ERROR_INITIALIZATION_FAILED;
    DeviceData* dd = find_device_data(get_dispatch_key(queue));
    if (!dd) {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      if (g_device_map.size() == 1) {
        dd = g_device_map.begin()->second;
      }
    }
    if (!dd || !dd->QueueSubmit) return VK_ERROR_INITIALIZATION_FAILED;

    std::lock_guard<std::mutex> lock(dd->queue_mutex);
    return dd->QueueSubmit(queue, submitCount, pSubmits, fence);
  } catch (...) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue queue, uint32_t submitCount,
                                                 const VkSubmitInfo2* pSubmits, VkFence fence) {
  try {
    if (!queue) return VK_ERROR_INITIALIZATION_FAILED;
    DeviceData* dd = find_device_data(get_dispatch_key(queue));
    if (!dd) {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      if (g_device_map.size() == 1) {
        dd = g_device_map.begin()->second;
      }
    }
    if (!dd) return VK_ERROR_INITIALIZATION_FAILED;

    std::lock_guard<std::mutex> lock(dd->queue_mutex);
    if (dd->QueueSubmit2) {
      return dd->QueueSubmit2(queue, submitCount, pSubmits, fence);
    }
    if (dd->QueueSubmit2KHR) {
      return dd->QueueSubmit2KHR(queue, submitCount, pSubmits, fence);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
  } catch (...) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2KHR(VkQueue queue, uint32_t submitCount,
                                                    const VkSubmitInfo2KHR* pSubmits,
                                                    VkFence fence) {
  try {
    if (!queue) return VK_ERROR_INITIALIZATION_FAILED;
    DeviceData* dd = find_device_data(get_dispatch_key(queue));
    if (!dd) {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      if (g_device_map.size() == 1) {
        dd = g_device_map.begin()->second;
      }
    }
    if (!dd) return VK_ERROR_INITIALIZATION_FAILED;

    std::lock_guard<std::mutex> lock(dd->queue_mutex);
    if (dd->QueueSubmit2KHR) {
      return dd->QueueSubmit2KHR(queue, submitCount, pSubmits, fence);
    }
    if (dd->QueueSubmit2) {
      return dd->QueueSubmit2(queue, submitCount, pSubmits, fence);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
  } catch (...) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueWaitIdle(VkQueue queue) {
  try {
    if (!queue) return VK_ERROR_INITIALIZATION_FAILED;
    DeviceData* dd = find_device_data(get_dispatch_key(queue));
    if (!dd) {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      if (g_device_map.size() == 1) {
        dd = g_device_map.begin()->second;
      }
    }
    if (!dd || !dd->QueueWaitIdle) return VK_ERROR_INITIALIZATION_FAILED;

    std::lock_guard<std::mutex> lock(dd->queue_mutex);
    return dd->QueueWaitIdle(queue);
  } catch (...) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}

}  // namespace fpshud::vk
