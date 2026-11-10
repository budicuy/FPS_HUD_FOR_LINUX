#pragma once

#include "vulkan_swapchain.hpp"

#include <vulkan/vulkan_wayland.h>

namespace fpshud::vk {

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkInstance* pInstance);

VKAPI_ATTR void VKAPI_CALL Hook_DestroyInstance(VkInstance instance,
                                                const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL Hook_EnumeratePhysicalDevices(VkInstance instance,
                                                             uint32_t* pPhysicalDeviceCount,
                                                             VkPhysicalDevice* pPhysicalDevices);

VKAPI_ATTR VkResult VKAPI_CALL
Hook_EnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount,
                                   VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties);

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice physicalDevice,
                                                 const VkDeviceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkDevice* pDevice);

VKAPI_ATTR void VKAPI_CALL Hook_DestroyDevice(VkDevice device,
                                              const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                               uint32_t queueIndex, VkQueue* pQueue);

VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue2(VkDevice device,
                                                const VkDeviceQueueInfo2* pQueueInfo,
                                                VkQueue* pQueue);

VKAPI_ATTR VkResult VKAPI_CALL
Hook_CreateWaylandSurfaceKHR(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
                             const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);

VKAPI_ATTR void VKAPI_CALL Hook_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                  const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue,
                                                    const VkPresentInfoKHR* pPresentInfo);

}  // namespace fpshud::vk

extern "C" {

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName);

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName);

__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct);

}  // extern "C"
