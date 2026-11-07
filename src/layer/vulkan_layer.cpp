#include "config.hpp"
#if FPSHUD_ENABLE_EXPERIMENTAL
#include "adaptive_vfr.hpp"
#endif
#include "input_manager.hpp"
#include "overlay_ui.hpp"
#include "recorder_ipc.hpp"
#include "telemetry.hpp"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

template <typename T>
void* get_dispatch_key(T obj) {
  return obj ? *reinterpret_cast<void**>(obj) : nullptr;
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
  PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
  PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties2 GetPhysicalDeviceMemoryProperties2 = nullptr;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
  PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
  PFN_vkEnumeratePhysicalDeviceGroups EnumeratePhysicalDeviceGroups = nullptr;
};

struct FrameResources {
  VkImage image = VK_NULL_HANDLE;
  VkImageView image_view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkSemaphore semaphore = VK_NULL_HANDLE;

  // Staging resources for screen recording & screenshot capture
  VkBuffer staging_buffer = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  void* mapped_ptr = nullptr;
  VkDeviceSize buffer_size = 0;
  bool has_valid_capture = false;
  bool has_valid_screenshot = false;
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
  PFN_vkGetDeviceQueue2 GetDeviceQueue2 = nullptr;
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

  // Frame capture device function pointers
  PFN_vkCreateBuffer CreateBuffer = nullptr;
  PFN_vkDestroyBuffer DestroyBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkBindBufferMemory BindBufferMemory = nullptr;
  PFN_vkMapMemory MapMemory = nullptr;
  PFN_vkUnmapMemory UnmapMemory = nullptr;
  PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges = nullptr;

  // Zero-Copy DMA-BUF export support (VK_KHR_external_memory_fd)
  bool has_dma_buf_support = false;
  PFN_vkGetMemoryFdKHR GetMemoryFdKHR = nullptr;

  // VRAM memory budget extension support (VK_EXT_memory_budget) & throttling
  bool has_memory_budget_ext = false;
  std::chrono::steady_clock::time_point last_vram_update{};
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  VkPhysicalDeviceType device_type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
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
  bool supports_transfer_src = false;
  std::chrono::steady_clock::time_point last_capture_time{};
};

std::mutex g_map_mutex;
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

// Wrapper for vkAllocateCommandBuffers called by ImGui so child VkCommandBuffer gets the loader
// dispatch table pointer
VKAPI_ATTR VkResult VKAPI_CALL
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

bool is_instance_level_function(const char* name) {
  return std::strncmp(name, "vkGetPhysicalDevice", 19) == 0 ||
         std::strncmp(name, "vkEnumerate", 11) == 0 || std::strcmp(name, "vkCreateInstance") == 0 ||
         std::strcmp(name, "vkDestroyInstance") == 0 ||
         std::strcmp(name, "vkDestroySurfaceKHR") == 0 ||
         std::strcmp(name, "vkCreateWaylandSurfaceKHR") == 0 ||
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

void destroy_swapchain_data(const std::shared_ptr<SwapchainData>& sd) {
  if (!sd) return;
  std::lock_guard<std::mutex> lock(sd->mutex);
  if (!sd->device_data) return;
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

static bool ensure_frame_capture_buffer(DeviceData* dd, SwapchainData* sd, FrameResources& frame) {
  if (sd->extent.width == 0 || sd->extent.height == 0) {
    return false;
  }
  // Only capture 32-bit swapchain formats (RGBA8, BGRA8, A2B10G10, A2R10G10).
  // 64-bit float formats (e.g. VK_FORMAT_R16G16B16A16_SFLOAT) are unsupported for 32-bit SHM
  // buffer.
  if (sd->format != VK_FORMAT_B8G8R8A8_UNORM && sd->format != VK_FORMAT_B8G8R8A8_SRGB &&
      sd->format != VK_FORMAT_R8G8B8A8_UNORM && sd->format != VK_FORMAT_R8G8B8A8_SRGB &&
      sd->format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
      sd->format != VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
    return false;
  }
  VkDeviceSize required_size = static_cast<VkDeviceSize>(sd->extent.width) * sd->extent.height * 4;
  if (frame.staging_buffer != VK_NULL_HANDLE && frame.buffer_size >= required_size) {
    return true;
  }

  if (frame.mapped_ptr && dd->UnmapMemory) {
    dd->UnmapMemory(dd->device, frame.staging_memory);
    frame.mapped_ptr = nullptr;
  }
  if (frame.staging_buffer && dd->DestroyBuffer) {
    dd->DestroyBuffer(dd->device, frame.staging_buffer, nullptr);
    frame.staging_buffer = VK_NULL_HANDLE;
  }
  if (frame.staging_memory && dd->FreeMemory) {
    dd->FreeMemory(dd->device, frame.staging_memory, nullptr);
    frame.staging_memory = VK_NULL_HANDLE;
  }
  frame.buffer_size = 0;

  if (!dd->CreateBuffer || !dd->AllocateMemory || !dd->BindBufferMemory || !dd->MapMemory) {
    return false;
  }

  VkBufferCreateInfo buf_info{};
  buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buf_info.size = required_size;
  buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dd->CreateBuffer(dd->device, &buf_info, nullptr, &frame.staging_buffer) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements mem_req{};
  dd->GetBufferMemoryRequirements(dd->device, frame.staging_buffer, &mem_req);

  uint32_t mem_type = 0;
  bool found = false;
  if (dd->instance_data && dd->instance_data->GetPhysicalDeviceMemoryProperties) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    dd->instance_data->GetPhysicalDeviceMemoryProperties(dd->physical_device, &mem_props);

    // Priority 1: Host Visible + Host Coherent + Host Cached (Fastest CPU readback, ~0.4ms)
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      if ((mem_req.memoryTypeBits & (1u << i)) &&
          (mem_props.memoryTypes[i].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
               VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
        mem_type = i;
        found = true;
        break;
      }
    }

    // Priority 2: Host Visible + Host Cached (Requires explicit cache invalidation)
    if (!found) {
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
          mem_type = i;
          found = true;
          break;
        }
      }
    }

    // Priority 3: Host Visible + Host Coherent (Fallback: uncached write-combined)
    if (!found) {
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
          mem_type = i;
          found = true;
          break;
        }
      }
    }

    // Priority 4: Host Visible (General fallback)
    if (!found) {
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
          mem_type = i;
          found = true;
          break;
        }
      }
    }
  }

  if (!found) {
    dd->DestroyBuffer(dd->device, frame.staging_buffer, nullptr);
    frame.staging_buffer = VK_NULL_HANDLE;
    return false;
  }

  VkMemoryAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = mem_req.size;
  alloc_info.memoryTypeIndex = mem_type;
  if (dd->AllocateMemory(dd->device, &alloc_info, nullptr, &frame.staging_memory) != VK_SUCCESS) {
    dd->DestroyBuffer(dd->device, frame.staging_buffer, nullptr);
    frame.staging_buffer = VK_NULL_HANDLE;
    return false;
  }

  if (dd->BindBufferMemory(dd->device, frame.staging_buffer, frame.staging_memory, 0) !=
      VK_SUCCESS) {
    dd->FreeMemory(dd->device, frame.staging_memory, nullptr);
    dd->DestroyBuffer(dd->device, frame.staging_buffer, nullptr);
    frame.staging_buffer = VK_NULL_HANDLE;
    frame.staging_memory = VK_NULL_HANDLE;
    return false;
  }

  if (dd->MapMemory(dd->device, frame.staging_memory, 0, required_size, 0, &frame.mapped_ptr) !=
      VK_SUCCESS) {
    dd->FreeMemory(dd->device, frame.staging_memory, nullptr);
    dd->DestroyBuffer(dd->device, frame.staging_buffer, nullptr);
    frame.staging_buffer = VK_NULL_HANDLE;
    frame.staging_memory = VK_NULL_HANDLE;
    frame.mapped_ptr = nullptr;
    return false;
  }

  frame.buffer_size = required_size;
  return true;
}

// ============================================================================
// Intercepted Vulkan Functions
// ============================================================================

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkInstance* pInstance) {
  try {
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

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr =
        chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
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
    id->EnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        fpGetInstanceProcAddr(*pInstance, "vkEnumeratePhysicalDevices"));
    id->GetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceProperties"));
    id->GetPhysicalDeviceQueueFamilyProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    id->GetPhysicalDeviceMemoryProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceMemoryProperties"));
    id->GetPhysicalDeviceMemoryProperties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
            fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceMemoryProperties2"));
    if (!id->GetPhysicalDeviceMemoryProperties2) {
      id->GetPhysicalDeviceMemoryProperties2 =
          reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
              fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceMemoryProperties2KHR"));
    }
    id->EnumerateDeviceExtensionProperties =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            fpGetInstanceProcAddr(*pInstance, "vkEnumerateDeviceExtensionProperties"));
    id->EnumeratePhysicalDeviceGroups = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(
        fpGetInstanceProcAddr(*pInstance, "vkEnumeratePhysicalDeviceGroups"));
    if (!id->EnumeratePhysicalDeviceGroups) {
      id->EnumeratePhysicalDeviceGroups = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(
          fpGetInstanceProcAddr(*pInstance, "vkEnumeratePhysicalDeviceGroupsKHR"));
    }

    std::lock_guard<std::mutex> lock(g_map_mutex);
    g_instance_map[get_dispatch_key(*pInstance)] = id;
    return VK_SUCCESS;
  } catch (...) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyInstance(VkInstance instance,
                                                const VkAllocationCallbacks* pAllocator) {
  try {
    fpshud::get_input_manager().detach_wayland_display(nullptr);
    if (!instance) return;

    InstanceData* id = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      auto it = g_instance_map.find(get_dispatch_key(instance));
      if (it != g_instance_map.end()) {
        id = it->second;
      } else {
        for (auto& kv : g_instance_map) {
          if (kv.second && kv.second->instance == instance) {
            id = kv.second;
            break;
          }
        }
      }

      if (id) {
        for (auto iter = g_instance_map.begin(); iter != g_instance_map.end();) {
          if (iter->second == id) {
            iter = g_instance_map.erase(iter);
          } else {
            ++iter;
          }
        }
      }
    }

    if (id) {
      if (id->DestroyInstance) {
        id->DestroyInstance(instance, pAllocator);
      }
      delete id;
    }
  } catch (...) {
  }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_EnumeratePhysicalDevices(VkInstance instance,
                                                             uint32_t* pPhysicalDeviceCount,
                                                             VkPhysicalDevice* pPhysicalDevices) {
  try {
    if (!instance) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    InstanceData* id = find_instance_data(get_dispatch_key(instance));
    if (!id || !id->EnumeratePhysicalDevices) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = id->EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
    if (res == VK_SUCCESS && pPhysicalDevices && pPhysicalDeviceCount &&
        *pPhysicalDeviceCount > 0) {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      for (uint32_t i = 0; i < *pPhysicalDeviceCount; ++i) {
        if (pPhysicalDevices[i] != VK_NULL_HANDLE) {
          g_instance_map[get_dispatch_key(pPhysicalDevices[i])] = id;
        }
      }
    }
    return res;
  } catch (...) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_EnumeratePhysicalDeviceGroups(
    VkInstance instance, uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {
  try {
    if (!instance) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    InstanceData* id = find_instance_data(get_dispatch_key(instance));
    if (!id || !id->EnumeratePhysicalDeviceGroups) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = id->EnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount,
                                                     pPhysicalDeviceGroupProperties);
    if (res == VK_SUCCESS && pPhysicalDeviceGroupProperties && pPhysicalDeviceGroupCount &&
        *pPhysicalDeviceGroupCount > 0) {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      for (uint32_t i = 0; i < *pPhysicalDeviceGroupCount; ++i) {
        for (uint32_t j = 0; j < pPhysicalDeviceGroupProperties[i].physicalDeviceCount; ++j) {
          if (pPhysicalDeviceGroupProperties[i].physicalDevices[j] != VK_NULL_HANDLE) {
            g_instance_map[get_dispatch_key(pPhysicalDeviceGroupProperties[i].physicalDevices[j])] =
                id;
          }
        }
      }
    }
    return res;
  } catch (...) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice physicalDevice,
                                                 const VkDeviceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkDevice* pDevice) {
  try {
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

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr =
        chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr =
        chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
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
    if (!dd->instance_data) {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      if (!g_instance_map.empty()) {
        dd->instance_data = g_instance_map.begin()->second;
      }
    }

#define LOAD_DEV_FN(name) \
  dd->name = reinterpret_cast<PFN_vk##name>(fpGetDeviceProcAddr(*pDevice, "vk" #name))
    LOAD_DEV_FN(DestroyDevice);
    LOAD_DEV_FN(GetDeviceQueue);
    LOAD_DEV_FN(GetDeviceQueue2);
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
    LOAD_DEV_FN(CreateBuffer);
    LOAD_DEV_FN(DestroyBuffer);
    LOAD_DEV_FN(GetBufferMemoryRequirements);
    LOAD_DEV_FN(AllocateMemory);
    LOAD_DEV_FN(FreeMemory);
    LOAD_DEV_FN(BindBufferMemory);
    LOAD_DEV_FN(MapMemory);
    LOAD_DEV_FN(UnmapMemory);
    LOAD_DEV_FN(CmdCopyImageToBuffer);
    LOAD_DEV_FN(CmdPipelineBarrier);
    LOAD_DEV_FN(InvalidateMappedMemoryRanges);
#undef LOAD_DEV_FN

    dd->GetMemoryFdKHR =
        reinterpret_cast<PFN_vkGetMemoryFdKHR>(fpGetDeviceProcAddr(*pDevice, "vkGetMemoryFdKHR"));
    if (dd->GetMemoryFdKHR) {
      dd->has_dma_buf_support = true;
    }

    // Check VK_EXT_memory_budget extension presence
    if (pCreateInfo && pCreateInfo->ppEnabledExtensionNames) {
      for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        if (pCreateInfo->ppEnabledExtensionNames[i] &&
            std::strcmp(pCreateInfo->ppEnabledExtensionNames[i], "VK_EXT_memory_budget") == 0) {
          dd->has_memory_budget_ext = true;
          break;
        }
      }
    }
    if (!dd->has_memory_budget_ext && dd->instance_data &&
        dd->instance_data->EnumerateDeviceExtensionProperties) {
      uint32_t ext_count = 0;
      if (dd->instance_data->EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ext_count,
                                                                nullptr) == VK_SUCCESS &&
          ext_count > 0) {
        std::vector<VkExtensionProperties> exts(ext_count);
        if (dd->instance_data->EnumerateDeviceExtensionProperties(
                physicalDevice, nullptr, &ext_count, exts.data()) == VK_SUCCESS) {
          for (const auto& ext : exts) {
            if (std::strcmp(ext.extensionName, "VK_EXT_memory_budget") == 0) {
              dd->has_memory_budget_ext = true;
              break;
            }
          }
        }
      }
    }

    uint32_t chosen_family = 0;
    VkDeviceQueueCreateFlags queue_flags = 0;
    if (dd->instance_data && dd->instance_data->GetPhysicalDeviceQueueFamilyProperties) {
      uint32_t qf_count = 0;
      dd->instance_data->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qf_count, nullptr);
      std::vector<VkQueueFamilyProperties> qf_props(qf_count);
      dd->instance_data->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qf_count,
                                                                qf_props.data());

      for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
        uint32_t fam = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
        if (fam < qf_count && (qf_props[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
          chosen_family = fam;
          queue_flags = pCreateInfo->pQueueCreateInfos[i].flags;
          break;
        }
      }
    } else if (pCreateInfo->queueCreateInfoCount > 0) {
      chosen_family = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
      queue_flags = pCreateInfo->pQueueCreateInfos[0].flags;
    }

    dd->graphics_queue_family = chosen_family;
    if (queue_flags != 0 && dd->GetDeviceQueue2) {
      VkDeviceQueueInfo2 qinfo{};
      qinfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
      qinfo.flags = queue_flags;
      qinfo.queueFamilyIndex = dd->graphics_queue_family;
      qinfo.queueIndex = 0;
      dd->GetDeviceQueue2(*pDevice, &qinfo, &dd->graphics_queue);
    } else if (dd->GetDeviceQueue) {
      dd->GetDeviceQueue(*pDevice, dd->graphics_queue_family, 0, &dd->graphics_queue);
    }

    if (!dd->graphics_queue && dd->GetDeviceQueue2) {
      VkDeviceQueueInfo2 qinfo{};
      qinfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
      qinfo.flags = queue_flags;
      qinfo.queueFamilyIndex = dd->graphics_queue_family;
      qinfo.queueIndex = 0;
      dd->GetDeviceQueue2(*pDevice, &qinfo, &dd->graphics_queue);
    }

    if (dd->graphics_queue) {
      copy_dispatch_key(dd->graphics_queue, *pDevice);
    }

    // Bind Active GPU to Telemetry only if layer is enabled (prioritize Discrete GPU over iGPU
    // probes)
    if (dd->instance_data && dd->instance_data->GetPhysicalDeviceProperties) {
      VkPhysicalDeviceProperties props{};
      dd->instance_data->GetPhysicalDeviceProperties(physicalDevice, &props);
      dd->vendor_id = props.vendorID;
      dd->device_id = props.deviceID;
      dd->device_type = props.deviceType;
      if (fpshud::get_global_config().enabled) {
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
          g_bound_discrete_gpu.store(true, std::memory_order_relaxed);
          fpshud::get_telemetry().notify_active_gpu(props.vendorID, props.deviceID);
        } else if (!g_bound_discrete_gpu.load(std::memory_order_relaxed)) {
          fpshud::get_telemetry().notify_active_gpu(props.vendorID, props.deviceID);
        }
      }
    }

    if (fpshud::get_global_config().enabled) {
      // Query initial VRAM budget
      uint64_t total_vram = 0;
      uint64_t used_vram = 0;
      if (dd->has_memory_budget_ext && dd->instance_data &&
          dd->instance_data->GetPhysicalDeviceMemoryProperties2) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_props{};
        budget_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 mem_props2{};
        mem_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        mem_props2.pNext = &budget_props;

        dd->instance_data->GetPhysicalDeviceMemoryProperties2(physicalDevice, &mem_props2);
        for (uint32_t i = 0; i < mem_props2.memoryProperties.memoryHeapCount; ++i) {
          if (mem_props2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            total_vram += mem_props2.memoryProperties.memoryHeaps[i].size;
            used_vram += budget_props.heapUsage[i];
          }
        }
      } else if (dd->instance_data && dd->instance_data->GetPhysicalDeviceMemoryProperties) {
        VkPhysicalDeviceMemoryProperties mem_props{};
        dd->instance_data->GetPhysicalDeviceMemoryProperties(physicalDevice, &mem_props);
        for (uint32_t i = 0; i < mem_props.memoryHeapCount; ++i) {
          if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            total_vram += mem_props.memoryHeaps[i].size;
          }
        }
      }
      if (total_vram > 0) {
        fpshud::get_telemetry().update_api_vram_info(used_vram, total_vram);
      }
    }
    dd->last_vram_update = std::chrono::steady_clock::now();

    {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      g_device_map[get_dispatch_key(*pDevice)] = dd;
      if (dd->graphics_queue) {
        g_device_map[get_dispatch_key(dd->graphics_queue)] = dd;
      }
    }

    return VK_SUCCESS;
  } catch (...) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyDevice(VkDevice device,
                                              const VkAllocationCallbacks* pAllocator) {
  try {
    if (!device) return;

    DeviceData* dd = nullptr;
    std::vector<std::shared_ptr<SwapchainData>> swapchains_to_destroy;

    {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      auto it = g_device_map.find(get_dispatch_key(device));
      if (it != g_device_map.end()) {
        dd = it->second;
      } else {
        for (auto& kv : g_device_map) {
          if (kv.second && kv.second->device == device) {
            dd = kv.second;
            break;
          }
        }
      }

      if (dd) {
        // 1. Gather all swapchains for this device and remove from map
        for (auto sit = g_swapchain_map.begin(); sit != g_swapchain_map.end();) {
          if (sit->second && sit->second->device_data == dd) {
            swapchains_to_destroy.push_back(sit->second);
            sit = g_swapchain_map.erase(sit);
          } else {
            ++sit;
          }
        }

        // 2. Erase ALL device map entries pointing to dd (device handle AND all queue handles)
        for (auto dit = g_device_map.begin(); dit != g_device_map.end();) {
          if (dit->second == dd) {
            dit = g_device_map.erase(dit);
          } else {
            ++dit;
          }
        }
      }
    }

    // 3. Destroy swapchains OUTSIDE g_map_mutex to avoid deadlocks
    for (const auto& sd : swapchains_to_destroy) {
      destroy_swapchain_data(sd);
    }

    // 4. Destroy underlying device and free dd
    if (dd) {
      if (dd->DestroyDevice) {
        dd->DestroyDevice(device, pAllocator);
      }
      delete dd;
    }
  } catch (...) {
  }
}

VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                               uint32_t queueIndex, VkQueue* pQueue) {
  try {
    if (!device) return;

    DeviceData* dd = find_device_data(get_dispatch_key(device));
    if (dd && dd->GetDeviceQueue) {
      dd->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
      if (pQueue && *pQueue) {
        copy_dispatch_key(*pQueue, device);
        std::lock_guard<std::mutex> lock(g_map_mutex);
        g_device_map[get_dispatch_key(*pQueue)] = dd;
      }
    }
  } catch (...) {
  }
}

VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue2(VkDevice device,
                                                const VkDeviceQueueInfo2* pQueueInfo,
                                                VkQueue* pQueue) {
  try {
    if (!device) return;

    DeviceData* dd = find_device_data(get_dispatch_key(device));
    if (dd && dd->GetDeviceQueue2) {
      dd->GetDeviceQueue2(device, pQueueInfo, pQueue);
      if (pQueue && *pQueue) {
        copy_dispatch_key(*pQueue, device);
        std::lock_guard<std::mutex> lock(g_map_mutex);
        g_device_map[get_dispatch_key(*pQueue)] = dd;
      }
    } else if (dd && dd->GetDeviceQueue && pQueueInfo) {
      dd->GetDeviceQueue(device, pQueueInfo->queueFamilyIndex, pQueueInfo->queueIndex, pQueue);
      if (pQueue && *pQueue) {
        copy_dispatch_key(*pQueue, device);
        std::lock_guard<std::mutex> lock(g_map_mutex);
        g_device_map[get_dispatch_key(*pQueue)] = dd;
      }
    }
  } catch (...) {
  }
}

VKAPI_ATTR VkResult VKAPI_CALL
Hook_CreateWaylandSurfaceKHR(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
                             const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
  try {
    if (pCreateInfo && pCreateInfo->display) {
      fpshud::get_input_manager().attach_wayland_display(pCreateInfo->display);
    }
    InstanceData* id = find_instance_data(get_dispatch_key(instance));
    if (!id || !id->GetInstanceProcAddr) return VK_ERROR_INITIALIZATION_FAILED;
    auto fpCreateWaylandSurfaceKHR = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
        id->GetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR"));
    if (!fpCreateWaylandSurfaceKHR) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fpCreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
  } catch (...) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                  const VkAllocationCallbacks* pAllocator) {
  try {
    fpshud::get_input_manager().detach_wayland_display(nullptr);
    InstanceData* id = find_instance_data(get_dispatch_key(instance));
    if (id && id->GetInstanceProcAddr) {
      auto fpDestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
          id->GetInstanceProcAddr(instance, "vkDestroySurfaceKHR"));
      if (fpDestroySurfaceKHR) {
        fpDestroySurfaceKHR(instance, surface, pAllocator);
      }
    }
  } catch (...) {
  }
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

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue,
                                                    const VkPresentInfoKHR* pPresentInfo) {
  try {
    DeviceData* dd = find_device_data(get_dispatch_key(queue));
    if (!dd && pPresentInfo && pPresentInfo->swapchainCount > 0 && pPresentInfo->pSwapchains) {
      VkSwapchainKHR sc0 = pPresentInfo->pSwapchains[0];
      std::lock_guard<std::mutex> lock(g_map_mutex);
      auto it = g_swapchain_map.find(sc0);
      if (it != g_swapchain_map.end() && it->second && it->second->device_data) {
        dd = it->second->device_data;
        g_device_map[get_dispatch_key(queue)] = dd;
      }
    }
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

    // Periodic ~500 ms VRAM usage query (zero runtime heap allocation)
    auto now_vram = std::chrono::steady_clock::now();
    if (now_vram - dd->last_vram_update >= std::chrono::milliseconds(500)) {
      dd->last_vram_update = now_vram;
      uint64_t total_vram = 0;
      uint64_t used_vram = 0;
      if (dd->has_memory_budget_ext && dd->instance_data &&
          dd->instance_data->GetPhysicalDeviceMemoryProperties2) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_props{};
        budget_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 mem_props2{};
        mem_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        mem_props2.pNext = &budget_props;

        dd->instance_data->GetPhysicalDeviceMemoryProperties2(dd->physical_device, &mem_props2);
        for (uint32_t i = 0; i < mem_props2.memoryProperties.memoryHeapCount; ++i) {
          if (mem_props2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            total_vram += mem_props2.memoryProperties.memoryHeaps[i].size;
            used_vram += budget_props.heapUsage[i];
          }
        }
      } else if (dd->instance_data && dd->instance_data->GetPhysicalDeviceMemoryProperties) {
        VkPhysicalDeviceMemoryProperties mem_props{};
        dd->instance_data->GetPhysicalDeviceMemoryProperties(dd->physical_device, &mem_props);
        for (uint32_t i = 0; i < mem_props.memoryHeapCount; ++i) {
          if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            total_vram += mem_props.memoryHeaps[i].size;
          }
        }
      }
      if (total_vram > 0) {
        telemetry.update_api_vram_info(used_vram, total_vram);
      }
    }

    VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
    uint32_t image_idx = pPresentInfo->pImageIndices[0];

    std::shared_ptr<SwapchainData> sd;
    {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      auto it = g_swapchain_map.find(swapchain);
      if (it != g_swapchain_map.end()) sd = it->second;
    }

    if (!sd) {
      return dd->QueuePresentKHR(queue, pPresentInfo);
    }

    std::lock_guard<std::mutex> render_lock(sd->mutex);
    if (!sd->imgui_initialized || image_idx >= sd->frames.size()) {
      return dd->QueuePresentKHR(queue, pPresentInfo);
    }
    FrameResources& frame = sd->frames[image_idx];

    // Bounded fence wait (2 seconds) to guarantee the game's render thread never deadlocks
    VkResult wait_res = dd->WaitForFences(dd->device, 1, &frame.fence, VK_TRUE, 2000000000ULL);
    if (wait_res != VK_SUCCESS) {
      return dd->QueuePresentKHR(queue, pPresentInfo);
    }
    dd->ResetFences(dd->device, 1, &frame.fence);
    dd->ResetCommandBuffer(frame.command_buffer, 0);

    // If recording/replay or screenshot copy finished on previous fence, submit to controller
    if ((frame.has_valid_capture || frame.has_valid_screenshot) && frame.mapped_ptr) {
      if (dd->InvalidateMappedMemoryRanges) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = frame.staging_memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        dd->InvalidateMappedMemoryRanges(dd->device, 1, &range);
      }
      uint32_t fmt = 0;  // 0 = BGRA
      // Autodetect Vulkan swapchain pixel format (8-bit and 10-bit HDR/wide gamut)
      // VK_FORMAT_A2B10G10R10_UNORM_PACK32 stores R in bits 0..9, G in bits 10..19, B in
      // bits 20..29 (little-endian X2BGR10LE) VK_FORMAT_A2R10G10B10_UNORM_PACK32 stores B in bits
      // 0..9, G in bits 10..19, R in bits 20..29 (little-endian X2RGB10LE)
      if (sd->format == VK_FORMAT_R8G8B8A8_UNORM || sd->format == VK_FORMAT_R8G8B8A8_SRGB) {
        fmt = 1;  // 1 = RGBA
      } else if (sd->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
        fmt = 2;  // 2 = X2BGR10LE
      } else if (sd->format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
        fmt = 3;  // 3 = X2RGB10LE
      }
      if (frame.has_valid_screenshot) {
        fpshud::RecorderController::instance().submit_screenshot(
            frame.mapped_ptr, sd->extent.width, sd->extent.height, fmt, false,
            fpshud::get_effective_record_dir(cfg));
        frame.has_valid_screenshot = false;
      }
      if (frame.has_valid_capture && fpshud::RecorderController::instance().is_capturing_frames()) {
        fpshud::RecorderController::instance().submit_frame(frame.mapped_ptr, sd->extent.width,
                                                            sd->extent.height, fmt, false);
      }
      frame.has_valid_capture = false;
    }

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

    bool take_shot = fpshud::RecorderController::instance().consume_screenshot_request() &&
                     sd->supports_transfer_src;
    bool clean_shot_recorded = false;

    auto record_image_copy = [&]() {
      VkImageMemoryBarrier b1{};
      b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b1.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b1.image = frame.image;
      b1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      b1.subresourceRange.baseMipLevel = 0;
      b1.subresourceRange.levelCount = 1;
      b1.subresourceRange.baseArrayLayer = 0;
      b1.subresourceRange.layerCount = 1;
      b1.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

      dd->CmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b1);

      VkBufferImageCopy region{};
      region.bufferOffset = 0;
      region.bufferRowLength = 0;
      region.bufferImageHeight = 0;
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.mipLevel = 0;
      region.imageSubresource.baseArrayLayer = 0;
      region.imageSubresource.layerCount = 1;
      region.imageOffset = {0, 0, 0};
      region.imageExtent = {sd->extent.width, sd->extent.height, 1};

      dd->CmdCopyImageToBuffer(frame.command_buffer, frame.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame.staging_buffer, 1,
                               &region);

      VkImageMemoryBarrier b2{};
      b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b2.image = frame.image;
      b2.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      b2.subresourceRange.baseMipLevel = 0;
      b2.subresourceRange.levelCount = 1;
      b2.subresourceRange.baseArrayLayer = 0;
      b2.subresourceRange.layerCount = 1;
      b2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      b2.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;

      dd->CmdPipelineBarrier(
          frame.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
          0, nullptr, 0, nullptr, 1, &b2);
    };

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dd->BeginCommandBuffer(frame.command_buffer, &begin_info);

    // Clean Game Screenshot (Tanpa HUD): copy swapchain image BEFORE ImGui render pass!
    if (take_shot && !cfg.screenshot_with_hud && ensure_frame_capture_buffer(dd, sd.get(), frame)) {
      record_image_copy();
      frame.has_valid_screenshot = true;
      clean_shot_recorded = true;
    }

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = sd->render_pass;
    rp_begin.framebuffer = frame.framebuffer;
    rp_begin.renderArea.extent = sd->extent;
    dd->CmdBeginRenderPass(frame.command_buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.command_buffer);

    dd->CmdEndRenderPass(frame.command_buffer);

    // Record GPU copy of swapchain image to staging buffer if recording/replay is active (with
    // jitter-resilient pacing / adaptive VFR) or if Screenshot With HUD was requested
    bool should_capture_frame = false;
    if (fpshud::RecorderController::instance().is_capturing_frames() && sd->supports_transfer_src) {
      auto now = std::chrono::steady_clock::now();
#if FPSHUD_ENABLE_EXPERIMENTAL
      if (fpshud::experimental::AdaptiveVfrController::is_vfr_active(cfg)) {
        if (fpshud::experimental::AdaptiveVfrController::should_capture_frame(
                now, sd->last_capture_time, cfg.record_vfr_max_fps)) {
          sd->last_capture_time = now;
          should_capture_frame = true;
        }
      } else
#endif
      {
        int rec_fps = (cfg.record_fps > 0) ? cfg.record_fps : 60;
        auto target_interval = std::chrono::microseconds(1000000 / rec_fps);
        auto min_interval = target_interval * 70 / 100;
        if (sd->last_capture_time == std::chrono::steady_clock::time_point{} ||
            now - sd->last_capture_time >= min_interval) {
          sd->last_capture_time = now;
          should_capture_frame = true;
        }
      }
    } else {
      sd->last_capture_time = std::chrono::steady_clock::time_point{};
    }

    bool want_hud_shot = (take_shot && cfg.screenshot_with_hud);
    if (!clean_shot_recorded && (should_capture_frame || want_hud_shot) &&
        ensure_frame_capture_buffer(dd, sd.get(), frame)) {
      record_image_copy();
      frame.has_valid_capture = should_capture_frame;
      if (want_hud_shot) {
        frame.has_valid_screenshot = true;
      }
    } else if (!clean_shot_recorded) {
      frame.has_valid_capture = false;
      frame.has_valid_screenshot = false;
    } else {
      frame.has_valid_capture = false;
    }

    dd->EndCommandBuffer(frame.command_buffer);

    VkPipelineStageFlags stack_wait_stages[8] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    std::vector<VkPipelineStageFlags> dyn_wait_stages;
    const VkPipelineStageFlags* wait_stages_ptr = stack_wait_stages;
    uint32_t wait_count = pPresentInfo->waitSemaphoreCount;
    if (wait_count > 8) {
      dyn_wait_stages.assign(wait_count, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
      wait_stages_ptr = dyn_wait_stages.data();
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = wait_count;
    submit_info.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
    submit_info.pWaitDstStageMask = wait_stages_ptr;
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
      // QueueSubmit failed: frame.fence was reset and remains unsignaled.
      // Submit an empty batch or recreate in signaled state so future frames do not deadlock
      VkSubmitInfo empty_sub{};
      empty_sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      if (dd->QueueSubmit(target_queue, 1, &empty_sub, frame.fence) != VK_SUCCESS) {
        dd->DestroyFence(dd->device, frame.fence, nullptr);
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        dd->CreateFence(dd->device, &fence_info, nullptr, &frame.fence);
      }
      pres_res = dd->QueuePresentKHR(queue, pPresentInfo);
    }

    fpshud::RecorderController::instance().sync_replay_state(cfg, w, h);
    fpshud::RecorderController::instance().on_frame_present(sd->extent.width, sd->extent.height);
    return pres_res;
  } catch (...) {
    DeviceData* dd = find_device_data(get_dispatch_key(queue));
    if (dd && dd->QueuePresentKHR) {
      return dd->QueuePresentKHR(queue, pPresentInfo);
    }
    return VK_ERROR_DEVICE_LOST;
  }
}

}  // namespace

// ============================================================================
// Exported Vulkan Layer Entrypoints
// ============================================================================

extern "C" {

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName) {
  try {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_DestroyDevice);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_GetDeviceQueue);
    if (std::strcmp(pName, "vkGetDeviceQueue2") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_GetDeviceQueue2);
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_CreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_DestroySwapchainKHR);
    if (std::strcmp(pName, "vkCreateWaylandSurfaceKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_CreateWaylandSurfaceKHR);
    if (std::strcmp(pName, "vkDestroySurfaceKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_DestroySurfaceKHR);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_QueuePresentKHR);

    if (!device) return nullptr;

    DeviceData* dd = find_device_data(get_dispatch_key(device));
    if (dd && dd->GetDeviceProcAddr) {
      return dd->GetDeviceProcAddr(device, pName);
    }
    return nullptr;
  } catch (...) {
    return nullptr;
  }
}

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
  try {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkCreateInstance") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_CreateInstance);
    if (std::strcmp(pName, "vkDestroyInstance") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_DestroyInstance);
    if (std::strcmp(pName, "vkEnumeratePhysicalDevices") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_EnumeratePhysicalDevices);
    if (std::strcmp(pName, "vkEnumeratePhysicalDeviceGroups") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_EnumeratePhysicalDeviceGroups);
    if (std::strcmp(pName, "vkEnumeratePhysicalDeviceGroupsKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_EnumeratePhysicalDeviceGroups);
    if (std::strcmp(pName, "vkCreateDevice") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_CreateDevice);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_DestroyDevice);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_GetDeviceQueue);
    if (std::strcmp(pName, "vkGetDeviceQueue2") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_GetDeviceQueue2);
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_CreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_DestroySwapchainKHR);
    if (std::strcmp(pName, "vkCreateWaylandSurfaceKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_CreateWaylandSurfaceKHR);
    if (std::strcmp(pName, "vkDestroySurfaceKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_DestroySurfaceKHR);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&Hook_QueuePresentKHR);

    if (!instance) return nullptr;

    InstanceData* id = find_instance_data(get_dispatch_key(instance));
    if (id && id->GetInstanceProcAddr) {
      return id->GetInstanceProcAddr(instance, pName);
    }
    return nullptr;
  } catch (...) {
    return nullptr;
  }
}

__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
  try {
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
  } catch (...) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
}

}  // extern "C"
