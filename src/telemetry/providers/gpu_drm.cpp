#include "providers/gpu_drm.hpp"
#include "logger.hpp"
#include "sysfs_util.hpp"
#include "telemetry.hpp"

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

#ifndef SYS_getdents64
#ifdef __NR_getdents64
#define SYS_getdents64 __NR_getdents64
#endif
#endif

namespace fpshud {

namespace {

struct LinuxDirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[1];
};

}  // namespace

DrmGpuProvider::DrmGpuProvider() = default;

void DrmGpuProvider::shutdown() noexcept {
  drm_gpu_busy_path_.clear();
  drm_vram_used_path_.clear();
  drm_vram_total_path_.clear();
  drm_gtt_used_path_.clear();
  drm_gtt_total_path_.clear();
  drm_mem_busy_path_.clear();
  drm_sclk_freq_path_.clear();
  drm_mclk_freq_path_.clear();
  drm_max_freq_path_.clear();
  drm_pp_dpm_sclk_path_.clear();
  drm_pp_dpm_mclk_path_.clear();
  drm_temp_path_.clear();
  drm_temp_fallback_path_.clear();
  drm_fan_pwm_path_.clear();
  drm_fan_rpm_path_.clear();
  drm_fan_max_path_.clear();
  drm_power_uw_path_.clear();
  drm_energy_uj_path_.clear();
  drm_sclk_in_hz_ = true;

  prev_gpu_energy_uj_ = 0;
  prev_gpu_energy_time_ = {};
  prev_drm_render_ns_ = 0;
  prev_drm_render_time_ = {};

  backend_type_ = GpuBackendType::None;
  detected_vendor_id_ = 0;
  detected_device_id_ = 0;
}

bool DrmGpuProvider::init(const std::string& root_prefix, uint32_t vendor_id, uint32_t device_id) {
  shutdown();
  root_prefix_ = root_prefix;

  std::error_code ec;
  char buf[128];
  std::string drm_base = root_prefix_ + "/sys/class/drm";
  if (!fs::exists(drm_base, ec)) return false;

  int best_card_idx = -1;
  uint64_t best_vram_bytes = 0;
  std::string best_vendor_id;
  std::string best_device_id;

  for (int card_idx = 0; card_idx < 8; ++card_idx) {
    fs::path card_root = fs::path(drm_base) / ("card" + std::to_string(card_idx));
    fs::path dev_dir = card_root / "device";
    if (!fs::exists(dev_dir, ec)) continue;

    std::string v_id_str;
    if (read_sysfs_buf((dev_dir / "vendor").c_str(), buf, sizeof(buf))) {
      v_id_str = buf;
    }
    uint32_t card_vendor = static_cast<uint32_t>(std::strtoul(v_id_str.c_str(), nullptr, 16));
    bool is_amd = (card_vendor == 0x1002);
    bool is_intel = (card_vendor == 0x8086);
    bool is_nvidia = (card_vendor == 0x10de);
    if (!is_amd && !is_intel && !is_nvidia) continue;

    std::string d_id_str;
    if (read_sysfs_buf((dev_dir / "device").c_str(), buf, sizeof(buf))) {
      d_id_str = buf;
    }
    uint32_t card_device = static_cast<uint32_t>(std::strtoul(d_id_str.c_str(), nullptr, 16));

    // Filtering by bound GPU if specified
    if (vendor_id != 0) {
      if (card_vendor != vendor_id) continue;
      if (device_id != 0 && card_device != 0 && card_device != device_id) continue;
    } else {
      // Unbound: if NVIDIA card is suspended, do not pick it over an active iGPU!
      if (is_nvidia) {
        char pbuf[32] = {0};
        if (read_sysfs_buf((dev_dir / "power/runtime_status").c_str(), pbuf, sizeof(pbuf))) {
          if (std::strcmp(pbuf, "suspended") == 0 || std::strstr(pbuf, "suspended") != nullptr) {
            continue;
          }
        }
      }
    }

    uint64_t card_vram = 0;
    if (read_sysfs_buf((dev_dir / "mem_info_vram_total").c_str(), buf, sizeof(buf))) {
      card_vram = std::strtoull(buf, nullptr, 10);
    } else if (read_sysfs_buf((card_root / "lmem_total_bytes").c_str(), buf, sizeof(buf))) {
      card_vram = std::strtoull(buf, nullptr, 10);
    } else if (read_sysfs_buf((dev_dir / "lmem_total_bytes").c_str(), buf, sizeof(buf))) {
      card_vram = std::strtoull(buf, nullptr, 10);
    }

    // If explicitly bound to this vendor/device, immediate preference
    if (vendor_id != 0 && card_vendor == vendor_id) {
      if (best_card_idx < 0 || (device_id != 0 && card_device == device_id) ||
          card_vram > best_vram_bytes) {
        best_card_idx = card_idx;
        best_vram_bytes = card_vram;
        best_vendor_id = v_id_str;
        best_device_id = d_id_str;
        if (device_id != 0 && card_device == device_id) break;
      }
    } else if (best_card_idx < 0 || card_vram > best_vram_bytes) {
      best_card_idx = card_idx;
      best_vram_bytes = card_vram;
      best_vendor_id = v_id_str;
      best_device_id = d_id_str;
    }
  }

  if (best_card_idx < 0) return false;

  fs::path card_root = fs::path(drm_base) / ("card" + std::to_string(best_card_idx));
  fs::path dev_dir = card_root / "device";
  uint32_t best_vendor_num =
      static_cast<uint32_t>(std::strtoul(best_vendor_id.c_str(), nullptr, 16));
  uint32_t best_device_num =
      static_cast<uint32_t>(std::strtoul(best_device_id.c_str(), nullptr, 16));

  detected_vendor_id_ = best_vendor_num;
  detected_device_id_ = best_device_num;

  bool is_amd = (best_vendor_num == 0x1002);
  bool is_intel = (best_vendor_num == 0x8086);
  bool is_nvidia = (best_vendor_num == 0x10de);

  if (is_amd) {
    backend_type_ = GpuBackendType::AmdSysfs;
  } else if (is_intel) {
    backend_type_ = GpuBackendType::IntelSysfs;
  } else {
    backend_type_ = GpuBackendType::AmdSysfs;  // generic DRM sysfs for Nouveau
  }

  std::snprintf(gpu_vendor_, sizeof(gpu_vendor_), "%s",
                is_amd ? "AMD" : (is_intel ? "Intel" : (is_nvidia ? "NVIDIA" : "Unknown")));
  std::snprintf(gpu_name_, sizeof(gpu_name_), "%s",
                is_amd
                    ? "Radeon GPU"
                    : (is_intel ? "Intel Graphics" : (is_nvidia ? "NVIDIA GPU" : "Generic GPU")));
  std::snprintf(gpu_driver_version_, sizeof(gpu_driver_version_), "%s",
                is_amd ? "amdgpu" : (is_intel ? "i915/xe" : (is_nvidia ? "nouveau" : "drm")));

  // 1. Check sysfs product_name first
  if (read_sysfs_buf((dev_dir / "product_name").c_str(), buf, sizeof(buf)) && buf[0] != '\0') {
    clean_hardware_name(buf, gpu_name_, sizeof(gpu_name_));
  } else if (!best_device_id.empty()) {
    // 2. Fallback: Resolve exact marketing name via PCI ID database (/usr/share/hwdata/pci.ids)
    lookup_pci_device_name(best_vendor_id.c_str(), best_device_id.c_str(), gpu_name_,
                           sizeof(gpu_name_));
  }

  // Common DRM busy & VRAM paths (AMD amdgpu & Intel xe)
  if (fs::exists(dev_dir / "gpu_busy_percent", ec)) {
    drm_gpu_busy_path_ = (dev_dir / "gpu_busy_percent").string();
  }
  if (fs::exists(dev_dir / "mem_busy_percent", ec)) {
    drm_mem_busy_path_ = (dev_dir / "mem_busy_percent").string();
  }
  if (fs::exists(dev_dir / "mem_info_vram_used", ec)) {
    drm_vram_used_path_ = (dev_dir / "mem_info_vram_used").string();
  } else if (fs::exists(card_root / "lmem_used_bytes", ec)) {
    drm_vram_used_path_ = (card_root / "lmem_used_bytes").string();
  } else if (fs::exists(dev_dir / "lmem_used_bytes", ec)) {
    drm_vram_used_path_ = (dev_dir / "lmem_used_bytes").string();
  } else if (fs::exists(card_root / "lmem0/lmem_used_bytes", ec)) {
    drm_vram_used_path_ = (card_root / "lmem0/lmem_used_bytes").string();
  }

  if (fs::exists(dev_dir / "mem_info_vram_total", ec)) {
    drm_vram_total_path_ = (dev_dir / "mem_info_vram_total").string();
  } else if (fs::exists(card_root / "lmem_total_bytes", ec)) {
    drm_vram_total_path_ = (card_root / "lmem_total_bytes").string();
  } else if (fs::exists(dev_dir / "lmem_total_bytes", ec)) {
    drm_vram_total_path_ = (dev_dir / "lmem_total_bytes").string();
  } else if (fs::exists(card_root / "lmem0/lmem_total_bytes", ec)) {
    drm_vram_total_path_ = (card_root / "lmem0/lmem_total_bytes").string();
  }
  if (fs::exists(dev_dir / "mem_info_gtt_used", ec)) {
    drm_gtt_used_path_ = (dev_dir / "mem_info_gtt_used").string();
  }
  if (fs::exists(dev_dir / "mem_info_gtt_total", ec)) {
    drm_gtt_total_path_ = (dev_dir / "mem_info_gtt_total").string();
  }
  if (fs::exists(dev_dir / "pp_dpm_sclk", ec)) {
    drm_pp_dpm_sclk_path_ = (dev_dir / "pp_dpm_sclk").string();
  }
  if (fs::exists(dev_dir / "pp_dpm_mclk", ec)) {
    drm_pp_dpm_mclk_path_ = (dev_dir / "pp_dpm_mclk").string();
  }

  // Intel i915 / xe specific frequency paths
  if (fs::exists(dev_dir / "tile0/gt0/freq0/act_freq", ec)) {
    drm_sclk_freq_path_ = (dev_dir / "tile0/gt0/freq0/act_freq").string();
    drm_sclk_in_hz_ = false;
  } else if (fs::exists(dev_dir / "tile0/gt0/freq0/cur_freq", ec)) {
    drm_sclk_freq_path_ = (dev_dir / "tile0/gt0/freq0/cur_freq").string();
    drm_sclk_in_hz_ = false;
  } else if (fs::exists(card_root / "gt_cur_freq_mhz", ec)) {
    drm_sclk_freq_path_ = (card_root / "gt_cur_freq_mhz").string();
    drm_sclk_in_hz_ = false;
  } else if (fs::exists(card_root / "gt/gt0/rps_cur_freq_mhz", ec)) {
    drm_sclk_freq_path_ = (card_root / "gt/gt0/rps_cur_freq_mhz").string();
    drm_sclk_in_hz_ = false;
  }

  if (fs::exists(dev_dir / "tile0/gt0/freq0/max_freq", ec)) {
    drm_max_freq_path_ = (dev_dir / "tile0/gt0/freq0/max_freq").string();
  } else if (fs::exists(dev_dir / "tile0/gt0/freq0/rp0_freq", ec)) {
    drm_max_freq_path_ = (dev_dir / "tile0/gt0/freq0/rp0_freq").string();
  } else if (fs::exists(card_root / "gt_max_freq_mhz", ec)) {
    drm_max_freq_path_ = (card_root / "gt_max_freq_mhz").string();
  } else if (fs::exists(card_root / "gt/gt0/rps_max_freq_mhz", ec)) {
    drm_max_freq_path_ = (card_root / "gt/gt0/rps_max_freq_mhz").string();
  }

  // Discover GPU hwmon subfolder (temp, fan, clock, power/energy)
  fs::path hwmon_root = dev_dir / "hwmon";
  if (fs::exists(hwmon_root, ec)) {
    for (const auto& hm : fs::directory_iterator(hwmon_root, ec)) {
      fs::path hp = hm.path();
      if (fs::exists(hp / "freq1_input", ec)) {
        drm_sclk_freq_path_ = (hp / "freq1_input").string();
        drm_sclk_in_hz_ = true;
      }
      if (fs::exists(hp / "freq2_input", ec)) {
        drm_mclk_freq_path_ = (hp / "freq2_input").string();
      }

      // Junction / Hotspot (temp2_input) detection with fallback to temp1_input
      std::string temp1_path;
      std::string temp2_path;
      if (fs::exists(hp / "temp1_input", ec)) {
        temp1_path = (hp / "temp1_input").string();
      }
      if (fs::exists(hp / "temp2_input", ec)) {
        bool is_junction = false;
        if (fs::exists(hp / "temp2_label", ec)) {
          char lbl[64] = {0};
          if (read_sysfs_buf((hp / "temp2_label").c_str(), lbl, sizeof(lbl))) {
            if (std::strstr(lbl, "junction") != nullptr) {
              is_junction = true;
            }
          }
        }
        if (is_junction || is_amd) {
          temp2_path = (hp / "temp2_input").string();
        }
      }
      if (!temp2_path.empty()) {
        drm_temp_path_ = temp2_path;
        drm_temp_fallback_path_ = temp1_path;
      } else if (!temp1_path.empty()) {
        drm_temp_path_ = temp1_path;
        drm_temp_fallback_path_.clear();
      }

      if (fs::exists(hp / "pwm1", ec)) {
        drm_fan_pwm_path_ = (hp / "pwm1").string();
      }
      if (fs::exists(hp / "fan1_input", ec) && fs::exists(hp / "fan1_max", ec)) {
        drm_fan_rpm_path_ = (hp / "fan1_input").string();
        drm_fan_max_path_ = (hp / "fan1_max").string();
      }
      if (fs::exists(hp / "power1_average", ec)) {
        drm_power_uw_path_ = (hp / "power1_average").string();
      } else if (fs::exists(hp / "power1_input", ec)) {
        drm_power_uw_path_ = (hp / "power1_input").string();
      }
      if (fs::exists(hp / "energy1_input", ec)) {
        drm_energy_uj_path_ = (hp / "energy1_input").string();
      }
      break;
    }
  }

  // Fallback for Intel integrated GPUs: check RAPL uncore/iGPU energy counter (intel-rapl:0:1)
  if (drm_power_uw_path_.empty() && drm_energy_uj_path_.empty() && is_intel) {
    std::string uncore_rapl = root_prefix_ + "/sys/class/powercap/intel-rapl:0:1/energy_uj";
    if (::access(uncore_rapl.c_str(), R_OK) == 0) {
      drm_energy_uj_path_ = std::move(uncore_rapl);
    }
  }

  return true;
}

void DrmGpuProvider::poll(HardwareTelemetryData& out_data) {
  char buf[64];
  float load_pct = -1.0f;
  int clock_mhz = 0;
  float vram_used_gb = 0.0f;
  float vram_total_gb = 0.0f;
  float bw_gbs = 0.0f;
  float max_bw_gbs = 0.0f;
  int temp_c = 0;
  int fan_pct = 0;
  float watts = 0.0f;

  // 1. Clock frequency
  if (!drm_sclk_freq_path_.empty() &&
      read_sysfs_buf(drm_sclk_freq_path_.c_str(), buf, sizeof(buf))) {
    uint64_t raw = std::strtoull(buf, nullptr, 10);
    clock_mhz =
        drm_sclk_in_hz_ ? static_cast<int>((raw + 500000) / 1000000) : static_cast<int>(raw);
  } else if (!drm_pp_dpm_sclk_path_.empty()) {
    if (FILE* fp = std::fopen(drm_pp_dpm_sclk_path_.c_str(), "r")) {
      char line_buf[128];
      while (std::fgets(line_buf, sizeof(line_buf), fp)) {
        std::string line(line_buf);
        if (line.find('*') != std::string::npos) {
          auto colon = line.find(':');
          if (colon != std::string::npos) {
            clock_mhz = static_cast<int>(std::strtol(line.c_str() + colon + 1, nullptr, 10));
            break;
          }
        }
      }
      std::fclose(fp);
    }
  }

  // 2. GPU Utilization / Load
  if (!drm_gpu_busy_path_.empty() && read_sysfs_buf(drm_gpu_busy_path_.c_str(), buf, sizeof(buf))) {
    load_pct = std::strtof(buf, nullptr);
  }

  // Intel i915 / xe fallback utilization
  if (load_pct < 0.0f) {
    char fdinfo_dir[256];
    if (root_prefix_.empty()) {
      std::snprintf(fdinfo_dir, sizeof(fdinfo_dir), "/proc/self/fdinfo");
    } else {
      std::snprintf(fdinfo_dir, sizeof(fdinfo_dir), "%s/proc/self/fdinfo", root_prefix_.c_str());
    }
    int dfd = open(fdinfo_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
      uint64_t total_render_ns = 0;
      bool found_drm_fd = false;
      char d_buf[512];
      long nread = 0;
      while ((nread = syscall(SYS_getdents64, dfd, d_buf, sizeof(d_buf))) > 0) {
        for (long bpos = 0; bpos < nread;) {
          auto* d = reinterpret_cast<const LinuxDirent64*>(d_buf + bpos);
          if (d->d_reclen == 0 || bpos + d->d_reclen > nread) break;
          if (d->d_name[0] != '.') {
            char fd_path[256];
            std::snprintf(fd_path, sizeof(fd_path), "%s/%s", fdinfo_dir, d->d_name);
            int fd = open(fd_path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
              char fdbuf[512];
              ssize_t rn = read(fd, fdbuf, sizeof(fdbuf) - 1);
              close(fd);
              if (rn > 0) {
                fdbuf[rn] = '\0';
                const char* tag = std::strstr(fdbuf, "drm-engine-render:");
                if (tag) {
                  uint64_t ns = std::strtoull(tag + 18, nullptr, 10);
                  total_render_ns += ns;
                  found_drm_fd = true;
                } else if ((tag = std::strstr(fdbuf, "drm-cycles-rcs:"))) {
                  uint64_t ns = std::strtoull(tag + 15, nullptr, 10);
                  total_render_ns += ns;
                  found_drm_fd = true;
                }
              }
            }
          }
          bpos += d->d_reclen;
        }
      }
      close(dfd);

      if (found_drm_fd) {
        auto now = std::chrono::steady_clock::now();
        if (prev_drm_render_time_.time_since_epoch().count() != 0 &&
            total_render_ns >= prev_drm_render_ns_) {
          auto dt_ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(now - prev_drm_render_time_)
                  .count();
          if (dt_ns > 1000000) {
            load_pct = std::clamp(
                static_cast<float>((total_render_ns - prev_drm_render_ns_) * 100.0 / dt_ns), 0.0f,
                100.0f);
          }
        }
        prev_drm_render_ns_ = total_render_ns;
        prev_drm_render_time_ = now;
      }
    }
  }

  if (load_pct < 0.0f) {
    if (!drm_max_freq_path_.empty() &&
        read_sysfs_buf(drm_max_freq_path_.c_str(), buf, sizeof(buf))) {
      int max_mhz = static_cast<int>(std::strtol(buf, nullptr, 10));
      if (max_mhz > 0 && clock_mhz > 0) {
        load_pct = std::clamp(
            (static_cast<float>(clock_mhz) * 100.0f) / static_cast<float>(max_mhz), 0.0f, 100.0f);
      }
    }
    if (load_pct < 0.0f) load_pct = 0.0f;
  }

  // 3. VRAM and GTT Shared Memory
  if (!drm_vram_used_path_.empty() &&
      read_sysfs_buf(drm_vram_used_path_.c_str(), buf, sizeof(buf))) {
    uint64_t bytes = std::strtoull(buf, nullptr, 10);
    vram_used_gb = static_cast<float>(bytes) / (1024.0f * 1024.0f * 1024.0f);
  }
  if (!drm_vram_total_path_.empty() &&
      read_sysfs_buf(drm_vram_total_path_.c_str(), buf, sizeof(buf))) {
    uint64_t bytes = std::strtoull(buf, nullptr, 10);
    vram_total_gb = static_cast<float>(bytes) / (1024.0f * 1024.0f * 1024.0f);
  }

  bool is_apu_gtt =
      (vram_total_gb <= 1.0f) ||
      (!drm_gtt_used_path_.empty() && drm_mclk_freq_path_.empty() && vram_total_gb <= 8.5f);
  if (is_apu_gtt && !drm_gtt_used_path_.empty() &&
      read_sysfs_buf(drm_gtt_used_path_.c_str(), buf, sizeof(buf))) {
    uint64_t gtt_used = std::strtoull(buf, nullptr, 10);
    vram_used_gb += static_cast<float>(gtt_used) / (1024.0f * 1024.0f * 1024.0f);
    if (!drm_gtt_total_path_.empty() &&
        read_sysfs_buf(drm_gtt_total_path_.c_str(), buf, sizeof(buf))) {
      uint64_t gtt_total = std::strtoull(buf, nullptr, 10);
      vram_total_gb += static_cast<float>(gtt_total) / (1024.0f * 1024.0f * 1024.0f);
    }
  }

  if (vram_total_gb <= 0.0f) {
    uint64_t api_tot = api_vram_total_bytes_.load(std::memory_order_relaxed);
    if (api_tot > 0) {
      uint64_t api_usd = api_vram_used_bytes_.load(std::memory_order_relaxed);
      vram_total_gb = static_cast<float>(api_tot) / (1024.0f * 1024.0f * 1024.0f);
      vram_used_gb = static_cast<float>(api_usd) / (1024.0f * 1024.0f * 1024.0f);
    } else {
      float sys_ram_total = out_data.ram_total_gb;
      float sys_game_ram = out_data.game_ram_gb;
      if (sys_ram_total <= 0.0f) {
        char meminfo_path[256];
        if (root_prefix_.empty()) {
          std::snprintf(meminfo_path, sizeof(meminfo_path), "/proc/meminfo");
        } else {
          std::snprintf(meminfo_path, sizeof(meminfo_path), "%s/proc/meminfo",
                        root_prefix_.c_str());
        }
        int mfd = open(meminfo_path, O_RDONLY | O_CLOEXEC);
        if (mfd >= 0) {
          char mbuf[1024];
          ssize_t mn = read(mfd, mbuf, sizeof(mbuf) - 1);
          close(mfd);
          if (mn > 0) {
            mbuf[mn] = '\0';
            if (const char* p = std::strstr(mbuf, "MemTotal:")) {
              unsigned long long kb = std::strtoull(p + 9, nullptr, 10);
              if (kb > 0) {
                sys_ram_total = static_cast<float>(kb) / (1024.0f * 1024.0f);
              }
            }
          }
        }
      }
      if (sys_ram_total > 0.0f) {
        vram_total_gb = sys_ram_total * 0.5f;
        vram_used_gb = std::min(sys_game_ram > 0.0f ? sys_game_ram : 0.0f, vram_total_gb);
      }
    }
  }

  // 4. Memory Bandwidth (GB/s) & VRAM Clock (MHz)
  int mclk_mhz = 0;
  if (!drm_mclk_freq_path_.empty() &&
      read_sysfs_buf(drm_mclk_freq_path_.c_str(), buf, sizeof(buf))) {
    uint64_t raw_hz = std::strtoull(buf, nullptr, 10);
    mclk_mhz = static_cast<int>((raw_hz + 500000) / 1000000);
  } else if (!drm_pp_dpm_mclk_path_.empty()) {
    if (FILE* fp = std::fopen(drm_pp_dpm_mclk_path_.c_str(), "r")) {
      char line_buf[128];
      while (std::fgets(line_buf, sizeof(line_buf), fp)) {
        std::string line(line_buf);
        if (line.find('*') != std::string::npos) {
          auto colon = line.find(':');
          if (colon != std::string::npos) {
            mclk_mhz = static_cast<int>(std::strtol(line.c_str() + colon + 1, nullptr, 10));
            break;
          }
        }
      }
      std::fclose(fp);
    }
  }
  float mem_util_ratio = load_pct / 100.0f;
  if (!drm_mem_busy_path_.empty() && read_sysfs_buf(drm_mem_busy_path_.c_str(), buf, sizeof(buf))) {
    mem_util_ratio = std::clamp(std::strtof(buf, nullptr) / 100.0f, 0.0f, 1.0f);
  }
  if (mclk_mhz > 0) {
    float bus_bytes = 8.0f;
    if (vram_total_gb >= 19.5f)
      bus_bytes = 48.0f;
    else if (vram_total_gb >= 14.5f)
      bus_bytes = 32.0f;
    else if (vram_total_gb >= 10.5f)
      bus_bytes = 24.0f;
    else if (vram_total_gb >= 5.5f)
      bus_bytes = 16.0f;
    max_bw_gbs = (static_cast<float>(mclk_mhz) * 2.0f * bus_bytes) / 1000.0f;
    bw_gbs = max_bw_gbs * mem_util_ratio;
  } else if (clock_mhz > 0) {
    float bus_bytes = (vram_total_gb >= 7.5f) ? 32.0f : 16.0f;
    max_bw_gbs = (static_cast<float>(clock_mhz) * bus_bytes) / 1000.0f;
    bw_gbs = max_bw_gbs * mem_util_ratio;
  }

  // 5. Temperature (with RDNA Hotspot / Junction & Edge Fallback)
  if (!drm_temp_path_.empty() && read_sysfs_buf(drm_temp_path_.c_str(), buf, sizeof(buf))) {
    long millideg = std::strtol(buf, nullptr, 10);
    if (millideg > 0 && millideg < 130000) {
      temp_c = static_cast<int>((millideg + 500) / 1000);
    } else if (!drm_temp_fallback_path_.empty() &&
               read_sysfs_buf(drm_temp_fallback_path_.c_str(), buf, sizeof(buf))) {
      long millideg_fb = std::strtol(buf, nullptr, 10);
      if (millideg_fb > 0 && millideg_fb < 130000) {
        temp_c = static_cast<int>((millideg_fb + 500) / 1000);
      }
    }
  } else if (!drm_temp_fallback_path_.empty() &&
             read_sysfs_buf(drm_temp_fallback_path_.c_str(), buf, sizeof(buf))) {
    long millideg_fb = std::strtol(buf, nullptr, 10);
    if (millideg_fb > 0 && millideg_fb < 130000) {
      temp_c = static_cast<int>((millideg_fb + 500) / 1000);
    }
  }

  if (!drm_fan_pwm_path_.empty() && read_sysfs_buf(drm_fan_pwm_path_.c_str(), buf, sizeof(buf))) {
    long pwm = std::strtol(buf, nullptr, 10);
    fan_pct = std::clamp(static_cast<int>((pwm * 100 + 127) / 255), 0, 100);
  } else if (!drm_fan_rpm_path_.empty() && !drm_fan_max_path_.empty() &&
             read_sysfs_buf(drm_fan_rpm_path_.c_str(), buf, sizeof(buf))) {
    long rpm = std::strtol(buf, nullptr, 10);
    if (read_sysfs_buf(drm_fan_max_path_.c_str(), buf, sizeof(buf))) {
      long max_rpm = std::strtol(buf, nullptr, 10);
      if (max_rpm > 0) fan_pct = std::clamp(static_cast<int>((rpm * 100) / max_rpm), 0, 100);
    }
  }

  if (!drm_power_uw_path_.empty() && read_sysfs_buf(drm_power_uw_path_.c_str(), buf, sizeof(buf))) {
    uint64_t uw = std::strtoull(buf, nullptr, 10);
    watts = static_cast<float>(uw) / 1e6f;
  } else if (!drm_energy_uj_path_.empty() &&
             read_sysfs_buf(drm_energy_uj_path_.c_str(), buf, sizeof(buf))) {
    uint64_t curr_uj = std::strtoull(buf, nullptr, 10);
    auto now = std::chrono::steady_clock::now();
    if (prev_gpu_energy_time_.time_since_epoch().count() != 0 && curr_uj >= prev_gpu_energy_uj_) {
      double dt_sec = std::chrono::duration<double>(now - prev_gpu_energy_time_).count();
      if (dt_sec > 0.01) {
        watts =
            static_cast<float>((static_cast<double>(curr_uj - prev_gpu_energy_uj_) / 1e6) / dt_sec);
      }
    }
    prev_gpu_energy_uj_ = curr_uj;
    prev_gpu_energy_time_ = now;
  }

  int eff_temp = (temp_c > 0) ? temp_c : out_data.cpu_temp_c;

  out_data.gpu_available = true;
  std::snprintf(out_data.gpu_vendor, sizeof(out_data.gpu_vendor), "%s", gpu_vendor_);
  std::snprintf(out_data.gpu_name, sizeof(out_data.gpu_name), "%s", gpu_name_);
  std::snprintf(out_data.gpu_driver_version, sizeof(out_data.gpu_driver_version), "%s",
                gpu_driver_version_);

  out_data.gpu_load_pct = load_pct;
  out_data.gpu_clock_mhz = clock_mhz;
  out_data.gpu_vram_clock_mhz = mclk_mhz;
  out_data.gpu_vram_used_gb = vram_used_gb;
  out_data.gpu_vram_total_gb = vram_total_gb;
  out_data.gpu_bandwidth_gbs = bw_gbs;
  out_data.gpu_max_bandwidth_gbs = max_bw_gbs;
  out_data.gpu_temp_c = eff_temp;
  out_data.gpu_fan_pct = fan_pct;
  out_data.gpu_power_w = watts;
}

}  // namespace fpshud
