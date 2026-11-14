#pragma once

#include "imgui.h"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fpshud::vk {

template <typename T>
inline void* get_dispatch_key(T obj) {
  return obj ? *reinterpret_cast<void* const*>(obj) : nullptr;
}

template <typename ChildT, typename ParentT>
inline void copy_dispatch_key(ChildT child, ParentT parent) {
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
  bool is_coherent = false;
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
  PFN_vkQueueSubmit2 QueueSubmit2 = nullptr;
  PFN_vkQueueSubmit2KHR QueueSubmit2KHR = nullptr;
  std::mutex queue_mutex;
  PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
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
  PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges = nullptr;

  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkDestroyImage DestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
  PFN_vkBindImageMemory BindImageMemory = nullptr;
  PFN_vkCreateSampler CreateSampler = nullptr;
  PFN_vkDestroySampler DestroySampler = nullptr;
  PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges = nullptr;

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

extern std::mutex g_map_mutex;
extern std::mutex g_vulkan_imgui_mutex;
extern std::unordered_map<void*, InstanceData*> g_instance_map;
extern std::unordered_map<void*, DeviceData*> g_device_map;
extern std::vector<InstanceData*> g_instances;
extern std::vector<DeviceData*> g_devices;
extern std::unordered_map<VkSwapchainKHR, std::shared_ptr<SwapchainData>> g_swapchain_map;
extern std::atomic<bool> g_bound_discrete_gpu;

InstanceData* find_instance_data(void* key);
DeviceData* find_device_data(void* key);
std::shared_ptr<SwapchainData> find_swapchain_data(VkSwapchainKHR swapchain);

uint32_t find_memory_type_index(DeviceData* dd, uint32_t type_filter,
                                VkMemoryPropertyFlags properties);

bool init_swapchain_overlay(SwapchainData* sd);
void destroy_swapchain_data(const std::shared_ptr<SwapchainData>& sd);

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice device,
                                                       const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkSwapchainKHR* pSwapchain);

VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue queue, uint32_t submitCount,
                                                const VkSubmitInfo* pSubmits, VkFence fence);

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue queue, uint32_t submitCount,
                                                 const VkSubmitInfo2* pSubmits, VkFence fence);

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2KHR(VkQueue queue, uint32_t submitCount,
                                                    const VkSubmitInfo2KHR* pSubmits,
                                                    VkFence fence);

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueWaitIdle(VkQueue queue);

}  // namespace fpshud::vk
