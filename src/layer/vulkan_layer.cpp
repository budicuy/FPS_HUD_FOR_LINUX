#include "vulkan_capture.hpp"
#include "vulkan_hooks.hpp"
#include "vulkan_player_stream.hpp"
#include "vulkan_swapchain.hpp"

#include "config.hpp"
#if FPSHUD_ENABLE_EXPERIMENTAL
#include "adaptive_vfr.hpp"
#endif
#include "input_manager.hpp"
#include "overlay_ui.hpp"
#include "recorder_ipc.hpp"
#include "telemetry.hpp"

#include "backends/imgui_impl_vulkan.h"

namespace fpshud::vk {

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

    // Periodic ~500 ms VRAM usage query (only when needed by telemetry, zero render-thread driver
    // lock stalls)
    auto now_vram = std::chrono::steady_clock::now();
    if (telemetry.needs_api_vram_query() &&
        (now_vram - dd->last_vram_update >= std::chrono::milliseconds(500))) {
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

    std::shared_ptr<SwapchainData> sd = find_swapchain_data(swapchain);
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
    process_frame_capture_readback(dd, sd.get(), frame, cfg);

    float w = static_cast<float>(sd->extent.width);
    float h = static_cast<float>(sd->extent.height);

    bool take_shot = fpshud::RecorderController::instance().consume_screenshot_request() &&
                     sd->supports_transfer_src;
    bool clean_shot_recorded = false;

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dd->BeginCommandBuffer(frame.command_buffer, &begin_info);

    // Clean Game Screenshot (Tanpa HUD): copy swapchain image BEFORE ImGui render pass!
    if (take_shot && !cfg.screenshot_with_hud && ensure_frame_capture_buffer(dd, sd.get(), frame)) {
      record_image_copy_to_staging(dd, sd.get(), frame);
      frame.has_valid_screenshot = true;
      clean_shot_recorded = true;
    }

    {
      std::lock_guard<std::mutex> imgui_lock(g_vulkan_imgui_mutex);
      set_current_rendering_swapchain(sd.get());
      ImGui::SetCurrentContext(sd->imgui_context);
      ImGuiIO& io = ImGui::GetIO();
      io.DisplaySize = ImVec2(w, h);

      // Safe deferred texture release (vulkan_player_texture_releaser sets pending_release)
      check_and_perform_player_release(dd);

      {
        std::lock_guard<std::mutex> cfg_lock(fpshud::get_config_mutex());
        fpshud::get_input_manager().update_for_frame(io, cfg, w, h);

        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();

        fpshud::render_overlay_ui(cfg, telemetry.get_snapshot(), w, h);

        ImGui::Render();
      }

      // Copy pending video playback frame from staging buffer to optimal VkImage
      record_player_texture_upload(dd, frame.command_buffer);

      VkRenderPassBeginInfo rp_begin{};
      rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      rp_begin.renderPass = sd->render_pass;
      rp_begin.framebuffer = frame.framebuffer;
      rp_begin.renderArea.extent = sd->extent;
      dd->CmdBeginRenderPass(frame.command_buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.command_buffer);

      dd->CmdEndRenderPass(frame.command_buffer);

      set_current_rendering_swapchain(nullptr);
    }

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
      record_image_copy_to_staging(dd, sd.get(), frame);
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
    {
      std::lock_guard<std::mutex> qlock(dd->queue_mutex);
      VkResult sub_res = dd->QueueSubmit(target_queue, 1, &submit_info, frame.fence);
      if (sub_res == VK_SUCCESS) {
        VkPresentInfoKHR modified_present = *pPresentInfo;
        if (pPresentInfo->swapchainCount > 1) {
          uint32_t orig_count = pPresentInfo->waitSemaphoreCount;
          VkSemaphore stack_semaphores[16];
          std::vector<VkSemaphore> dyn_semaphores;
          VkSemaphore* sem_ptr = stack_semaphores;
          if (orig_count + 1 > 16) {
            dyn_semaphores.resize(orig_count + 1);
            sem_ptr = dyn_semaphores.data();
          }
          sem_ptr[0] = frame.semaphore;
          for (uint32_t i = 0; i < orig_count; ++i) {
            sem_ptr[1 + i] = pPresentInfo->pWaitSemaphores[i];
          }
          modified_present.waitSemaphoreCount = orig_count + 1;
          modified_present.pWaitSemaphores = sem_ptr;
          pres_res = dd->QueuePresentKHR(queue, &modified_present);
        } else {
          modified_present.waitSemaphoreCount = 1;
          modified_present.pWaitSemaphores = &frame.semaphore;
          pres_res = dd->QueuePresentKHR(queue, &modified_present);
        }
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
    }

    fpshud::RecorderController::instance().sync_replay_state(cfg, w, h);
    return pres_res;
  } catch (...) {
    DeviceData* dd = find_device_data(get_dispatch_key(queue));
    if (dd && dd->QueuePresentKHR) {
      return dd->QueuePresentKHR(queue, pPresentInfo);
    }
    return VK_ERROR_DEVICE_LOST;
  }
}

}  // namespace fpshud::vk
