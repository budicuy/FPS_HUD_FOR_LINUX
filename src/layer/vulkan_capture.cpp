#include "vulkan_capture.hpp"

#include "recorder_ipc.hpp"

namespace fpshud::vk {

bool ensure_frame_capture_buffer(DeviceData* dd, SwapchainData* sd, FrameResources& frame) {
  if (!dd || !sd || sd->extent.width == 0 || sd->extent.height == 0) {
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
  bool is_coherent = false;
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
        is_coherent = true;
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
          is_coherent = false;
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
          is_coherent = true;
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
          is_coherent =
              (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
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

  if (dd->MapMemory(dd->device, frame.staging_memory, 0, VK_WHOLE_SIZE, 0, &frame.mapped_ptr) !=
      VK_SUCCESS) {
    dd->FreeMemory(dd->device, frame.staging_memory, nullptr);
    dd->DestroyBuffer(dd->device, frame.staging_buffer, nullptr);
    frame.staging_buffer = VK_NULL_HANDLE;
    frame.staging_memory = VK_NULL_HANDLE;
    frame.mapped_ptr = nullptr;
    return false;
  }

  frame.buffer_size = mem_req.size;
  frame.is_coherent = is_coherent;
  return true;
}

void record_image_copy_to_staging(DeviceData* dd, SwapchainData* sd, FrameResources& frame) {
  if (!dd || !sd || !dd->CmdPipelineBarrier || !dd->CmdCopyImageToBuffer) {
    return;
  }

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

  dd->CmdCopyImageToBuffer(frame.command_buffer, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           frame.staging_buffer, 1, &region);

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
  b2.dstAccessMask = 0;

  dd->CmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b2);
}

void process_frame_capture_readback(DeviceData* dd, SwapchainData* sd, FrameResources& frame,
                                    const HudConfig& cfg) {
  if (!dd || !sd || (!frame.has_valid_capture && !frame.has_valid_screenshot) ||
      !frame.mapped_ptr) {
    return;
  }

  if (!frame.is_coherent && dd->InvalidateMappedMemoryRanges) {
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
    fpshud::RecorderController::instance().submit_screenshot(frame.mapped_ptr, sd->extent.width,
                                                             sd->extent.height, fmt, false,
                                                             fpshud::get_effective_record_dir(cfg));
    frame.has_valid_screenshot = false;
  }

  if (frame.has_valid_capture && fpshud::RecorderController::instance().is_capturing_frames()) {
    fpshud::RecorderController::instance().submit_frame(frame.mapped_ptr, sd->extent.width,
                                                        sd->extent.height, fmt, false);
  }
  frame.has_valid_capture = false;
}

}  // namespace fpshud::vk
