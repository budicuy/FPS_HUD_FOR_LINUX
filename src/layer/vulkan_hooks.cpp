#include "vulkan_hooks.hpp"

#include "config.hpp"
#include "input_manager.hpp"
#include "logger.hpp"
#include "telemetry.hpp"

#include <cstring>
#include <memory>
#include <vector>

namespace fpshud::vk {

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

    auto id = std::make_unique<InstanceData>();
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
    g_instance_map[get_dispatch_key(*pInstance)] = id.release();
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

    auto dd = std::make_unique<DeviceData>();
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
    LOAD_DEV_FN(QueueSubmit2);
    LOAD_DEV_FN(QueueSubmit2KHR);
    LOAD_DEV_FN(QueueWaitIdle);
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
    LOAD_DEV_FN(CmdCopyBufferToImage);
    LOAD_DEV_FN(CmdPipelineBarrier);
    LOAD_DEV_FN(InvalidateMappedMemoryRanges);
    LOAD_DEV_FN(CreateImage);
    LOAD_DEV_FN(DestroyImage);
    LOAD_DEV_FN(GetImageMemoryRequirements);
    LOAD_DEV_FN(BindImageMemory);
    LOAD_DEV_FN(CreateSampler);
    LOAD_DEV_FN(DestroySampler);
    LOAD_DEV_FN(FlushMappedMemoryRanges);
#undef LOAD_DEV_FN

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
    bool found_graphics_queue = false;
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
          found_graphics_queue = true;
          break;
        }
      }
    } else if (pCreateInfo->queueCreateInfoCount > 0) {
      chosen_family = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
      queue_flags = pCreateInfo->pQueueCreateInfos[0].flags;
      found_graphics_queue = true;
    }

    dd->graphics_queue_family = chosen_family;
    if (found_graphics_queue) {
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
    } else {
      dd->graphics_queue = VK_NULL_HANDLE;
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

    FPSHUD_LOG_DEBUG(fpshud::log::Subsystem::Vulkan,
                     "Device created: dev=%p, graphics_queue_found=%d, family=%u", *pDevice,
                     found_graphics_queue ? 1 : 0, chosen_family);

    {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      void* dev_key = get_dispatch_key(*pDevice);
      g_device_map[dev_key] = dd.get();
      if (dd->graphics_queue) {
        g_device_map[get_dispatch_key(dd->graphics_queue)] = dd.get();
      }
      dd.release();
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

}  // namespace fpshud::vk

extern "C" {

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName) {
  try {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_DestroyDevice);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_GetDeviceQueue);
    if (std::strcmp(pName, "vkGetDeviceQueue2") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_GetDeviceQueue2);
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_CreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_DestroySwapchainKHR);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_QueuePresentKHR);
    if (std::strcmp(pName, "vkQueueSubmit") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_QueueSubmit);
    if (std::strcmp(pName, "vkQueueSubmit2") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_QueueSubmit2);
    if (std::strcmp(pName, "vkQueueSubmit2KHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_QueueSubmit2KHR);
    if (std::strcmp(pName, "vkQueueWaitIdle") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_QueueWaitIdle);

    if (!device) return nullptr;

    fpshud::vk::DeviceData* dd = fpshud::vk::find_device_data(fpshud::vk::get_dispatch_key(device));
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
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_CreateInstance);
    if (std::strcmp(pName, "vkDestroyInstance") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_DestroyInstance);
    if (std::strcmp(pName, "vkEnumeratePhysicalDevices") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_EnumeratePhysicalDevices);
    if (std::strcmp(pName, "vkEnumeratePhysicalDeviceGroups") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_EnumeratePhysicalDeviceGroups);
    if (std::strcmp(pName, "vkEnumeratePhysicalDeviceGroupsKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_EnumeratePhysicalDeviceGroups);
    if (std::strcmp(pName, "vkCreateDevice") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_CreateDevice);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_DestroyDevice);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_GetDeviceQueue);
    if (std::strcmp(pName, "vkGetDeviceQueue2") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_GetDeviceQueue2);
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_CreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_DestroySwapchainKHR);
    if (std::strcmp(pName, "vkCreateWaylandSurfaceKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_CreateWaylandSurfaceKHR);
    if (std::strcmp(pName, "vkDestroySurfaceKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_DestroySurfaceKHR);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_QueuePresentKHR);
    if (std::strcmp(pName, "vkQueueSubmit") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_QueueSubmit);
    if (std::strcmp(pName, "vkQueueSubmit2") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_QueueSubmit2);
    if (std::strcmp(pName, "vkQueueSubmit2KHR") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_QueueSubmit2KHR);
    if (std::strcmp(pName, "vkQueueWaitIdle") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&fpshud::vk::Hook_QueueWaitIdle);

    if (!instance) return nullptr;

    fpshud::vk::InstanceData* id =
        fpshud::vk::find_instance_data(fpshud::vk::get_dispatch_key(instance));
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
