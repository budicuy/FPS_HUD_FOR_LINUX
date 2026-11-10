#pragma once

#include "vulkan_swapchain.hpp"

#include "imgui.h"

namespace fpshud::vk {

struct VulkanPlayerState {
  VkDevice device = VK_NULL_HANDLE;
  DeviceData* dd = nullptr;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory image_memory = VK_NULL_HANDLE;
  VkImageView image_view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

  static constexpr size_t kStagingRingCount = 2;
  struct StagingBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
  };
  StagingBuffer staging[kStagingRingCount];
  size_t current_staging_idx = 0;

  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t last_seq = 0;
  bool needs_copy = false;
  bool image_initialized = false;
  bool pending_release = false;
  size_t copy_staging_idx = 0;
};

VulkanPlayerState& get_player_vk_state();
void set_current_rendering_swapchain(SwapchainData* sd);

void vulkan_player_release(DeviceData* dd, VulkanPlayerState& state);
void cleanup_player_texture(DeviceData* dd);
void check_and_perform_player_release(DeviceData* dd);

ImTextureID vulkan_player_texture_provider(const uint8_t* rgba, uint32_t w, uint32_t h,
                                           uint64_t seq);
void vulkan_player_texture_releaser();
void register_vulkan_player_texture_backend();

void record_player_texture_upload(DeviceData* dd, VkCommandBuffer cmd);

}  // namespace fpshud::vk
