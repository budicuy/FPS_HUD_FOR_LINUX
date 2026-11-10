#pragma once

#include "vulkan_swapchain.hpp"

#include "config.hpp"

namespace fpshud::vk {

bool ensure_frame_capture_buffer(DeviceData* dd, SwapchainData* sd, FrameResources& frame);

void record_image_copy_to_staging(DeviceData* dd, SwapchainData* sd, FrameResources& frame);

void process_frame_capture_readback(DeviceData* dd, SwapchainData* sd, FrameResources& frame,
                                    const HudConfig& cfg);

}  // namespace fpshud::vk
