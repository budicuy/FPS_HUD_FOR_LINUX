#include "vulkan_player_stream.hpp"

#include "gui_menu.hpp"

#include "backends/imgui_impl_vulkan.h"

#include <cstring>

namespace fpshud::vk {

VulkanPlayerState g_player_vk_state;
namespace {
SwapchainData* g_current_rendering_sd = nullptr;
}  // namespace

void set_current_rendering_swapchain(SwapchainData* sd) { g_current_rendering_sd = sd; }

void vulkan_player_release(DeviceData* dd, VulkanPlayerState& state) {
  state.pending_release = false;
  if (!state.device || !dd) return;

  if (dd->DeviceWaitIdle) {
    dd->DeviceWaitIdle(state.device);
  }

  if (state.descriptor_set != VK_NULL_HANDLE) {
    ImGui_ImplVulkan_RemoveTexture(state.descriptor_set);
    state.descriptor_set = VK_NULL_HANDLE;
  }
  if (state.image_view != VK_NULL_HANDLE && dd->DestroyImageView) {
    dd->DestroyImageView(state.device, state.image_view, nullptr);
    state.image_view = VK_NULL_HANDLE;
  }
  if (state.image != VK_NULL_HANDLE && dd->DestroyImage) {
    dd->DestroyImage(state.device, state.image, nullptr);
    state.image = VK_NULL_HANDLE;
  }
  if (state.image_memory != VK_NULL_HANDLE && dd->FreeMemory) {
    dd->FreeMemory(state.device, state.image_memory, nullptr);
    state.image_memory = VK_NULL_HANDLE;
  }
  if (state.sampler != VK_NULL_HANDLE && dd->DestroySampler) {
    dd->DestroySampler(state.device, state.sampler, nullptr);
    state.sampler = VK_NULL_HANDLE;
  }
  for (size_t i = 0; i < VulkanPlayerState::kStagingRingCount; ++i) {
    if (state.staging[i].mapped && dd->UnmapMemory) {
      dd->UnmapMemory(state.device, state.staging[i].memory);
      state.staging[i].mapped = nullptr;
    }
    if (state.staging[i].buffer && dd->DestroyBuffer) {
      dd->DestroyBuffer(state.device, state.staging[i].buffer, nullptr);
      state.staging[i].buffer = VK_NULL_HANDLE;
    }
    if (state.staging[i].memory && dd->FreeMemory) {
      dd->FreeMemory(state.device, state.staging[i].memory, nullptr);
      state.staging[i].memory = VK_NULL_HANDLE;
    }
    state.staging[i].size = 0;
    state.staging[i].is_coherent = false;
  }
  state.width = 0;
  state.height = 0;
  state.last_seq = 0;
  state.needs_copy = false;
  state.image_initialized = false;
  state.device = VK_NULL_HANDLE;
  state.dd = nullptr;
  state.owner_sd = nullptr;
}

void cleanup_player_texture(DeviceData* dd) { vulkan_player_release(dd, g_player_vk_state); }

void check_and_perform_player_release(DeviceData* dd) {
  if (g_player_vk_state.pending_release && dd) {
    vulkan_player_release(dd, g_player_vk_state);
  }
}

ImTextureID vulkan_player_texture_provider(const uint8_t* rgba, uint32_t w, uint32_t h,
                                           uint64_t seq) {
  if (!rgba || w == 0 || h == 0) return 0;
  if (!g_current_rendering_sd || !g_current_rendering_sd->device_data) return 0;

  DeviceData* dd = g_current_rendering_sd->device_data;
  VulkanPlayerState& state = g_player_vk_state;

  if (!dd->CreateImage || !dd->DestroyImage || !dd->GetImageMemoryRequirements ||
      !dd->BindImageMemory || !dd->CreateSampler || !dd->DestroySampler ||
      !dd->CmdCopyBufferToImage) {
    return 0;
  }

  if (state.device != dd->device || state.width != w || state.height != h ||
      state.image == VK_NULL_HANDLE) {
    vulkan_player_release(dd, state);

    state.device = dd->device;
    state.dd = dd;
    state.owner_sd = g_current_rendering_sd;
    state.width = w;
    state.height = h;

    size_t required_bytes = static_cast<size_t>(w) * h * 4;

    for (size_t i = 0; i < VulkanPlayerState::kStagingRingCount; ++i) {
      VkBufferCreateInfo binfo{};
      binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      binfo.size = required_bytes;
      binfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (dd->CreateBuffer(dd->device, &binfo, nullptr, &state.staging[i].buffer) != VK_SUCCESS) {
        vulkan_player_release(dd, state);
        return 0;
      }

      VkMemoryRequirements breq{};
      dd->GetBufferMemoryRequirements(dd->device, state.staging[i].buffer, &breq);
      bool is_coherent = true;
      uint32_t host_mem_type = find_memory_type_index(
          dd, breq.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (host_mem_type == 0xFFFFFFFF) {
        host_mem_type =
            find_memory_type_index(dd, breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        is_coherent = false;
      }
      if (host_mem_type == 0xFFFFFFFF) {
        vulkan_player_release(dd, state);
        return 0;
      }

      VkMemoryAllocateInfo alloc_info{};
      alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      alloc_info.allocationSize = breq.size;
      alloc_info.memoryTypeIndex = host_mem_type;
      if (dd->AllocateMemory(dd->device, &alloc_info, nullptr, &state.staging[i].memory) !=
          VK_SUCCESS) {
        vulkan_player_release(dd, state);
        return 0;
      }
      dd->BindBufferMemory(dd->device, state.staging[i].buffer, state.staging[i].memory, 0);
      dd->MapMemory(dd->device, state.staging[i].memory, 0, VK_WHOLE_SIZE, 0,
                    &state.staging[i].mapped);
      state.staging[i].size = required_bytes;
      state.staging[i].is_coherent = is_coherent;
    }

    VkImageCreateInfo img_info{};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent = {w, h, 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dd->CreateImage(dd->device, &img_info, nullptr, &state.image) != VK_SUCCESS) {
      vulkan_player_release(dd, state);
      return 0;
    }

    VkMemoryRequirements img_req{};
    dd->GetImageMemoryRequirements(dd->device, state.image, &img_req);
    uint32_t dev_mem_type =
        find_memory_type_index(dd, img_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (dev_mem_type == 0xFFFFFFFF) {
      dev_mem_type = find_memory_type_index(dd, img_req.memoryTypeBits, 0);
    }
    if (dev_mem_type == 0xFFFFFFFF) {
      vulkan_player_release(dd, state);
      return 0;
    }

    VkMemoryAllocateInfo img_alloc{};
    img_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    img_alloc.allocationSize = img_req.size;
    img_alloc.memoryTypeIndex = dev_mem_type;
    if (dd->AllocateMemory(dd->device, &img_alloc, nullptr, &state.image_memory) != VK_SUCCESS) {
      vulkan_player_release(dd, state);
      return 0;
    }
    dd->BindImageMemory(dd->device, state.image, state.image_memory, 0);

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = state.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (dd->CreateImageView(dd->device, &view_info, nullptr, &state.image_view) != VK_SUCCESS) {
      vulkan_player_release(dd, state);
      return 0;
    }

    VkSamplerCreateInfo samp_info{};
    samp_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp_info.magFilter = VK_FILTER_LINEAR;
    samp_info.minFilter = VK_FILTER_LINEAR;
    samp_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samp_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp_info.minLod = -1000.0f;
    samp_info.maxLod = 1000.0f;
    samp_info.maxAnisotropy = 1.0f;
    if (dd->CreateSampler(dd->device, &samp_info, nullptr, &state.sampler) != VK_SUCCESS) {
      vulkan_player_release(dd, state);
      return 0;
    }

    state.descriptor_set = ImGui_ImplVulkan_AddTexture(state.sampler, state.image_view,
                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    state.image_initialized = false;
  }

  if (seq != state.last_seq || !state.image_initialized) {
    state.current_staging_idx =
        (state.current_staging_idx + 1) % VulkanPlayerState::kStagingRingCount;
    auto& st = state.staging[state.current_staging_idx];
    if (st.mapped) {
      std::memcpy(st.mapped, rgba, static_cast<size_t>(w) * h * 4);
      if (!st.is_coherent && dd->FlushMappedMemoryRanges) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = st.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        dd->FlushMappedMemoryRanges(dd->device, 1, &range);
      }
    }
    state.needs_copy = true;
    state.copy_staging_idx = state.current_staging_idx;
    state.last_seq = seq;
  }

  return (ImTextureID)state.descriptor_set;
}

void vulkan_player_texture_releaser() { g_player_vk_state.pending_release = true; }

void register_vulkan_player_texture_backend() {
  fpshud::register_player_texture_backend(vulkan_player_texture_provider,
                                          vulkan_player_texture_releaser);
}

void record_player_texture_upload(DeviceData* dd, VkCommandBuffer cmd) {
  VulkanPlayerState& state = g_player_vk_state;
  if (!state.needs_copy || state.image == VK_NULL_HANDLE || !dd || !dd->CmdPipelineBarrier ||
      !dd->CmdCopyBufferToImage) {
    return;
  }

  size_t sidx = state.copy_staging_idx;
  VkBuffer src_buf = state.staging[sidx].buffer;

  VkImageMemoryBarrier b1{};
  b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b1.oldLayout = state.image_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                         : VK_IMAGE_LAYOUT_UNDEFINED;
  b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b1.image = state.image;
  b1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b1.subresourceRange.baseMipLevel = 0;
  b1.subresourceRange.levelCount = 1;
  b1.subresourceRange.baseArrayLayer = 0;
  b1.subresourceRange.layerCount = 1;
  b1.srcAccessMask = state.image_initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
  b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

  dd->CmdPipelineBarrier(cmd,
                         state.image_initialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                                 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
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
  region.imageExtent = {state.width, state.height, 1};

  dd->CmdCopyBufferToImage(cmd, src_buf, state.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);

  VkImageMemoryBarrier b2{};
  b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b2.image = state.image;
  b2.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b2.subresourceRange.baseMipLevel = 0;
  b2.subresourceRange.levelCount = 1;
  b2.subresourceRange.baseArrayLayer = 0;
  b2.subresourceRange.layerCount = 1;
  b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  dd->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b2);

  state.image_initialized = true;
  state.needs_copy = false;
}

}  // namespace fpshud::vk
