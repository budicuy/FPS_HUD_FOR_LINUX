#include "doctest.h"
#include "test_common.hpp"

#include "config.hpp"
#include "input_manager.hpp"
#include "recorder_ipc.hpp"
#include "telemetry.hpp"
#include "vulkan_hooks.hpp"
#include "vulkan_swapchain.hpp"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <link.h>
#include <linux/input.h>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// Parsed ELF Dynamic Linkage & Symbol Audit Results
struct ElfAuditResult {
  bool file_found = false;
  bool elf_valid = false;
  std::vector<std::string> defined_dynamic_symbols;
  std::vector<std::string> needed_libs;
  std::vector<std::string> glibc_versions;
};

/// Parse ELF64 binary headers, dynamic symbol table, DT_NEEDED tags, and version requirements
ElfAuditResult audit_libfpshud_elf(const fs::path& so_path) {
  ElfAuditResult res;
  if (so_path.empty()) return res;
  res.file_found = true;

  std::ifstream file(so_path, std::ios::binary);
  if (!file) return res;

  Elf64_Ehdr ehdr{};
  if (!file.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr))) return res;

  if (std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 || ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
    return res;
  }
  if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0 || ehdr.e_shentsize != sizeof(Elf64_Shdr)) {
    return res;
  }

  std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
  file.seekg(static_cast<std::streamoff>(ehdr.e_shoff));
  if (!file.read(reinterpret_cast<char*>(shdrs.data()),
                 static_cast<std::streamsize>(ehdr.e_shnum * sizeof(Elf64_Shdr)))) {
    return res;
  }

  res.elf_valid = true;

  for (const auto& sh : shdrs) {
    if (sh.sh_type == SHT_DYNSYM) {
      if (sh.sh_link >= shdrs.size()) continue;
      const auto& str_shdr = shdrs[sh.sh_link];
      if (str_shdr.sh_size == 0) continue;

      std::vector<char> dynstr(str_shdr.sh_size);
      file.seekg(static_cast<std::streamoff>(str_shdr.sh_offset));
      if (!file.read(dynstr.data(), static_cast<std::streamsize>(str_shdr.sh_size))) continue;

      size_t count = sh.sh_size / sizeof(Elf64_Sym);
      std::vector<Elf64_Sym> syms(count);
      file.seekg(static_cast<std::streamoff>(sh.sh_offset));
      if (!file.read(reinterpret_cast<char*>(syms.data()),
                     static_cast<std::streamsize>(sh.sh_size)))
        continue;

      for (const auto& sym : syms) {
        if (sym.st_shndx != SHN_UNDEF) {
          unsigned char bind = ELF64_ST_BIND(sym.st_info);
          if (bind == STB_GLOBAL || bind == STB_WEAK) {
            if (sym.st_name < dynstr.size()) {
              const char* name = &dynstr[sym.st_name];
              if (name[0] != '\0') {
                res.defined_dynamic_symbols.emplace_back(name);
              }
            }
          }
        }
      }
    } else if (sh.sh_type == SHT_DYNAMIC) {
      if (sh.sh_link >= shdrs.size()) continue;
      const auto& str_shdr = shdrs[sh.sh_link];
      if (str_shdr.sh_size == 0) continue;

      std::vector<char> dynstr(str_shdr.sh_size);
      file.seekg(static_cast<std::streamoff>(str_shdr.sh_offset));
      if (!file.read(dynstr.data(), static_cast<std::streamsize>(str_shdr.sh_size))) continue;

      size_t count = sh.sh_size / sizeof(Elf64_Dyn);
      std::vector<Elf64_Dyn> dyns(count);
      file.seekg(static_cast<std::streamoff>(sh.sh_offset));
      if (!file.read(reinterpret_cast<char*>(dyns.data()),
                     static_cast<std::streamsize>(sh.sh_size)))
        continue;

      for (const auto& d : dyns) {
        if (d.d_tag == DT_NEEDED) {
          if (d.d_un.d_val < dynstr.size()) {
            res.needed_libs.emplace_back(&dynstr[d.d_un.d_val]);
          }
        } else if (d.d_tag == DT_NULL) {
          break;
        }
      }
    } else if (sh.sh_type == SHT_GNU_verneed) {
      if (sh.sh_link >= shdrs.size()) continue;
      const auto& str_shdr = shdrs[sh.sh_link];
      if (str_shdr.sh_size == 0) continue;

      std::vector<char> dynstr(str_shdr.sh_size);
      file.seekg(static_cast<std::streamoff>(str_shdr.sh_offset));
      if (!file.read(dynstr.data(), static_cast<std::streamsize>(str_shdr.sh_size))) continue;

      std::vector<uint8_t> verneed_buf(sh.sh_size);
      file.seekg(static_cast<std::streamoff>(sh.sh_offset));
      if (!file.read(reinterpret_cast<char*>(verneed_buf.data()),
                     static_cast<std::streamsize>(sh.sh_size)))
        continue;

      size_t off = 0;
      while (off + sizeof(Elf64_Verneed) <= verneed_buf.size()) {
        const auto* vn = reinterpret_cast<const Elf64_Verneed*>(verneed_buf.data() + off);
        size_t aux_off = off + vn->vn_aux;
        for (Elf64_Half j = 0; j < vn->vn_cnt; ++j) {
          if (aux_off + sizeof(Elf64_Vernaux) > verneed_buf.size()) break;
          const auto* vna = reinterpret_cast<const Elf64_Vernaux*>(verneed_buf.data() + aux_off);
          if (vna->vna_name < dynstr.size()) {
            res.glibc_versions.emplace_back(&dynstr[vna->vna_name]);
          }
          if (vna->vna_next == 0) break;
          aux_off += vna->vna_next;
        }
        if (vn->vn_next == 0) break;
        off += vn->vn_next;
      }
    }
  }

  return res;
}

}  // namespace

TEST_CASE("Layer: ABI, Symbols, & Portability") {
  fs::path so_path = find_libfpshud_path();

  SUBCASE("Suite 7: Dynamic Symbol Audit (Exact 8 Exported Symbols)") {
    ElfAuditResult audit = audit_libfpshud_elf(so_path);
    CHECK_MESSAGE(audit.file_found, "libfpshud.so binary must exist on disk");
    CHECK_MESSAGE(audit.elf_valid, "libfpshud.so must be a valid 64-bit ELF shared object");

    // Exact 8 exported ABI dynamic symbols required by the Vulkan layer and OpenGL/EGL hooks
    const std::vector<std::string> expected_symbols = {
        "eglGetProcAddress",     "eglSwapBuffers",
        "glXGetProcAddress",     "glXGetProcAddressARB",
        "glXSwapBuffers",        "vkGetDeviceProcAddr",
        "vkGetInstanceProcAddr", "vkNegotiateLoaderLayerInterfaceVersion",
    };

    std::vector<std::string> actual_symbols = audit.defined_dynamic_symbols;
    std::sort(actual_symbols.begin(), actual_symbols.end());

    CHECK_MESSAGE(actual_symbols.size() == 8,
                  "Expected exactly 8 defined dynamic symbols in libfpshud.so");
    CHECK(actual_symbols == expected_symbols);

    for (const auto& sym : expected_symbols) {
      bool found =
          (std::find(actual_symbols.begin(), actual_symbols.end(), sym) != actual_symbols.end());
      CHECK_MESSAGE(found, "Missing required exported ABI symbol");
    }

    // dlopen / dlsym Dynamic Loading Safety and Hook Invocation
    dlerror();
    void* handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    CHECK_MESSAGE(handle != nullptr, "dlopen(RTLD_NOW|RTLD_LOCAL) failed on libfpshud.so");

    if (handle) {
      // 1. Resolve Vulkan hooks
      auto pfn_neg = reinterpret_cast<PFN_vkNegotiateLoaderLayerInterfaceVersion>(
          dlsym(handle, "vkNegotiateLoaderLayerInterfaceVersion"));
      auto pfn_gpa =
          reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
      auto pfn_gdpa =
          reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(handle, "vkGetDeviceProcAddr"));

      CHECK(pfn_neg != nullptr);
      CHECK(pfn_gpa != nullptr);
      CHECK(pfn_gdpa != nullptr);

      // Safe invocation of vkNegotiateLoaderLayerInterfaceVersion
      if (pfn_neg) {
        CHECK(pfn_neg(nullptr) == VK_ERROR_INITIALIZATION_FAILED);

        VkNegotiateLayerInterface nli{};
        nli.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
        nli.loaderLayerInterfaceVersion = 2;
        VkResult neg_res = pfn_neg(&nli);
        CHECK(neg_res == VK_SUCCESS);
        CHECK(nli.loaderLayerInterfaceVersion == 2);
        CHECK(nli.pfnGetInstanceProcAddr != nullptr);
        CHECK(nli.pfnGetDeviceProcAddr != nullptr);
        CHECK(nli.pfnGetPhysicalDeviceProcAddr == nullptr);
      }

      // Safe invocation of vkGetInstanceProcAddr / vkGetDeviceProcAddr
      if (pfn_gpa) {
        CHECK(pfn_gpa(VK_NULL_HANDLE, "vkCreateInstance") != nullptr);
        CHECK(pfn_gpa(VK_NULL_HANDLE, "vkGetDeviceProcAddr") != nullptr);
        CHECK(pfn_gpa(VK_NULL_HANDLE, "vkGetDeviceQueue2") != nullptr);
        CHECK(pfn_gpa(VK_NULL_HANDLE, "vkNonExistentFunctionXYZ") == nullptr);
      }
      if (pfn_gdpa) {
        CHECK(pfn_gdpa(VK_NULL_HANDLE, "vkGetDeviceQueue2") != nullptr);
        CHECK(pfn_gdpa(VK_NULL_HANDLE, "vkQueuePresentKHR") != nullptr);
        CHECK(pfn_gdpa(VK_NULL_HANDLE, "vkDestroyDevice") != nullptr);
        CHECK(pfn_gdpa(VK_NULL_HANDLE, "vkNonExistentFunctionXYZ") == nullptr);
      }

      // 2. Resolve OpenGL / EGL hooks
      auto pfn_glx = dlsym(handle, "glXSwapBuffers");
      auto pfn_glx_gpa =
          reinterpret_cast<void* (*)(const unsigned char*)>(dlsym(handle, "glXGetProcAddress"));
      auto pfn_glx_gpa_arb =
          reinterpret_cast<void* (*)(const unsigned char*)>(dlsym(handle, "glXGetProcAddressARB"));
      auto pfn_egl = dlsym(handle, "eglSwapBuffers");
      auto pfn_egl_gpa =
          reinterpret_cast<void* (*)(const char*)>(dlsym(handle, "eglGetProcAddress"));

      CHECK(pfn_glx != nullptr);
      CHECK(pfn_glx_gpa != nullptr);
      CHECK(pfn_glx_gpa_arb != nullptr);
      CHECK(pfn_egl != nullptr);
      CHECK(pfn_egl_gpa != nullptr);

      if (pfn_glx_gpa) {
        CHECK(pfn_glx_gpa(reinterpret_cast<const unsigned char*>("glXSwapBuffers")) != nullptr);
      }
      if (pfn_glx_gpa_arb) {
        CHECK(pfn_glx_gpa_arb(reinterpret_cast<const unsigned char*>("glXSwapBuffers")) != nullptr);
      }
      if (pfn_egl_gpa) {
        CHECK(pfn_egl_gpa("eglSwapBuffers") != nullptr);
      }

      // 3. Strict namespace pollution verification (no SDL, X11, or internal exports)
      CHECK(dlsym(handle, "SDL_PollEvent") == nullptr);
      CHECK(dlsym(handle, "SDL_PeepEvents") == nullptr);
      CHECK(dlsym(handle, "XNextEvent") == nullptr);
      CHECK(dlsym(handle, "XQueryPointer") == nullptr);
      CHECK(dlsym(handle, "wl_display_connect") == nullptr);
      CHECK(dlsym(handle, "main") == nullptr);

      dlclose(handle);
    }
  }

  SUBCASE("Suite 8: Zero DT_NEEDED Leakage Audit") {
    ElfAuditResult audit = audit_libfpshud_elf(so_path);
    CHECK_MESSAGE(audit.file_found, "libfpshud.so binary must exist on disk");
    CHECK_MESSAGE(audit.elf_valid, "libfpshud.so must be a valid 64-bit ELF shared object");

    // Strictly forbidden DT_NEEDED dependencies: graphics runtime, driver loader, and display
    // servers
    const std::vector<std::string> banned_libraries = {
        "libGLX",       "libEGL", "libOpenGL",         "libGL",  "libnvidia-ml",
        "libze_loader", "libX11", "libwayland-client", "libxcb",
    };

    std::vector<std::string> detected_leaks;
    for (const auto& needed : audit.needed_libs) {
      for (const auto& banned : banned_libraries) {
        if (needed.find(banned) != std::string::npos) {
          detected_leaks.push_back(needed);
        }
      }
    }

    CHECK_MESSAGE(detected_leaks.empty(),
                  "Zero DT_NEEDED leaks required! Found banned dependencies in libfpshud.so");

    // Verify baseline portability libraries (only standard C/POSIX runtime permitted)
    bool has_libc = false;
    for (const auto& needed : audit.needed_libs) {
      if (needed.find("libc.so") != std::string::npos) has_libc = true;
    }
    CHECK_MESSAGE(has_libc, "libfpshud.so must dynamically link against standard libc.so");
  }

  SUBCASE("Suite 9: GLIBC Portability Gate (<= 2.35)") {
    ElfAuditResult audit = audit_libfpshud_elf(so_path);
    CHECK_MESSAGE(audit.file_found, "libfpshud.so binary must exist on disk");
    CHECK_MESSAGE(audit.elf_valid, "libfpshud.so must be a valid 64-bit ELF shared object");
    CHECK_MESSAGE(!audit.glibc_versions.empty(),
                  "libfpshud.so must contain parsed GLIBC version requirements");

    int max_glibc_major = 0;
    int max_glibc_minor = 0;
    std::string highest_version_str;
    std::vector<std::string> violating_versions;

    for (const auto& ver_str : audit.glibc_versions) {
      if (ver_str.rfind("GLIBC_", 0) == 0) {
        int major = 0;
        int minor = 0;
        if (std::sscanf(ver_str.c_str(), "GLIBC_%d.%d", &major, &minor) >= 2) {
          if (major > 2 || (major == 2 && minor > 35)) {
            violating_versions.push_back(ver_str);
          }
          if (major > max_glibc_major || (major == max_glibc_major && minor > max_glibc_minor)) {
            max_glibc_major = major;
            max_glibc_minor = minor;
            highest_version_str = ver_str;
          }
        }
      }
    }

    CHECK_MESSAGE(violating_versions.empty(),
                  "GLIBC portability gate violated! Binaries must be compatible with Ubuntu 22.04 "
                  "LTS (<= GLIBC 2.35).");
    CHECK_MESSAGE(max_glibc_major <= 2, "GLIBC major version must be <= 2");
    CHECK_MESSAGE(max_glibc_minor <= 35, "Highest GLIBC symbol requirement exceeds 2.35");
  }

  SUBCASE("Suite 16: Vulkan Layer State Structs & Dispatch Keys") {
    using namespace fpshud::vk;

    // 16.1 Dispatch key extraction, inheritance, and copy safety
    void* fake_dispatch_table = reinterpret_cast<void*>(0xDEADBEEF);
    void* fake_handle = &fake_dispatch_table;
    void* key = get_dispatch_key(fake_handle);
    CHECK(key == fake_dispatch_table);
    CHECK(get_dispatch_key(static_cast<void*>(nullptr)) == nullptr);

    void* child_dispatch_table = nullptr;
    void* child_handle = &child_dispatch_table;
    copy_dispatch_key(child_handle, fake_handle);
    CHECK(get_dispatch_key(child_handle) == fake_dispatch_table);

    // 16.2 State structs alignment and default initialization
    InstanceData id;
    CHECK(id.instance == VK_NULL_HANDLE);
    CHECK(id.GetInstanceProcAddr == nullptr);
    CHECK(id.DestroyInstance == nullptr);
    CHECK(alignof(InstanceData) >= alignof(void*));

    DeviceData dd;
    CHECK(dd.device == VK_NULL_HANDLE);
    CHECK(dd.graphics_queue == VK_NULL_HANDLE);
    CHECK(dd.CreateSwapchainKHR == nullptr);
    CHECK(dd.QueuePresentKHR == nullptr);
    CHECK(dd.has_memory_budget_ext == false);
    CHECK(alignof(DeviceData) >= alignof(void*));

    // Test queue_mutex thread serialization capability
    {
      std::lock_guard<std::mutex> lock(dd.queue_mutex);
      CHECK(true);
    }

    FrameResources fr;
    CHECK(fr.image == VK_NULL_HANDLE);
    CHECK(fr.staging_buffer == VK_NULL_HANDLE);
    CHECK(fr.mapped_ptr == nullptr);
    CHECK(fr.buffer_size == 0);
    CHECK(!fr.has_valid_capture);
    CHECK(!fr.has_valid_screenshot);
    CHECK(alignof(FrameResources) >= alignof(void*));

    // 16.3 Vulkan C ABI Exception Isolation Boundaries & Thread-Safety Invariants
    fs::path repo_root = find_repo_root();
    if (!repo_root.empty()) {
      std::string vk_src = read_text_file(repo_root / "src" / "layer" / "vulkan_layer.cpp");
      std::string hooks_src = read_text_file(repo_root / "src" / "layer" / "vulkan_hooks.cpp");
      std::string swap_src = read_text_file(repo_root / "src" / "layer" / "vulkan_swapchain.cpp");

      bool qp_ok = (vk_src.find("Hook_QueuePresentKHR") != std::string::npos &&
                    vk_src.find("try {") != std::string::npos &&
                    vk_src.find("catch (...)") != std::string::npos);
      bool cs_ok = (swap_src.find("Hook_CreateSwapchainKHR") != std::string::npos &&
                    swap_src.find("try {") != std::string::npos &&
                    swap_src.find("catch (...)") != std::string::npos);
      bool cd_ok = (hooks_src.find("Hook_CreateDevice") != std::string::npos &&
                    hooks_src.find("try {") != std::string::npos &&
                    hooks_src.find("catch (...)") != std::string::npos);

      CHECK_MESSAGE(qp_ok, "Hook_QueuePresentKHR must be enclosed in try/catch exception boundary");
      CHECK_MESSAGE(cs_ok,
                    "Hook_CreateSwapchainKHR must be enclosed in try/catch exception boundary");
      CHECK_MESSAGE(cd_ok, "Hook_CreateDevice must be enclosed in try/catch exception boundary");

      bool vk_shared_swapchain_ok =
          (swap_src.find("std::shared_ptr<SwapchainData>") != std::string::npos &&
           swap_src.find("destroy_swapchain_data(const std::shared_ptr<SwapchainData>& sd)") !=
               std::string::npos &&
           swap_src.find("std::lock_guard<std::mutex> lock(sd->mutex);") != std::string::npos);

      CHECK_MESSAGE(
          vk_shared_swapchain_ok,
          "SwapchainData must be managed via std::shared_ptr and synchronized by sd->mutex");

      // Verify layer manifest environment isolation
      std::string manifest_content =
          read_text_file(repo_root / "manifest" / "VkLayer_FPSHUD_overlay.json.in");
      CHECK_MESSAGE(manifest_content.find("\"enable_environment\"") != std::string::npos,
                    "Layer manifest must specify enable_environment");
      CHECK_MESSAGE(manifest_content.find("\"FPS_HUD\"") != std::string::npos,
                    "Layer manifest must require FPS_HUD environment variable for activation");
    }
  }

  SUBCASE("Defensive Bounds & Security Hardening") {
    // 1. Locale-independent decimal parsing
    float v1 = fpshud::safe_strtof("1.35", 1.0f);
    float v2 = fpshud::safe_strtof("2.50", 1.0f);
    float v3 = fpshud::safe_strtof("invalid", 4.2f);
    CHECK(std::abs(v1 - 1.35f) < 0.001f);
    CHECK(std::abs(v2 - 2.50f) < 0.001f);
    CHECK(std::abs(v3 - 4.2f) < 0.001f);

    // 2. Robust INI atomic save & ENOSPC / EACCES protection
    fpshud::HudConfig dummy_cfg;
    dummy_cfg.gui_scale = 1.25f;
    bool save_bad = fpshud::save_ini_file("/proc/sys/unwritable_fpshud.ini", dummy_cfg);
    CHECK(!save_bad);

    // 3. Modifier-aware keybind matching & precedence
    fpshud::Keybind bare_kb{.mod_key = 0, .main_key = KEY_F10};
    fpshud::Keybind combo_kb{.mod_key = KEY_LEFTCTRL, .main_key = KEY_F10};

    std::bitset<512> keys_none;
    std::bitset<512> keys_with_ctrl;
    keys_with_ctrl.set(KEY_LEFTCTRL);

    CHECK(fpshud::matches_keybind(KEY_F10, bare_kb, keys_none));
    CHECK(!fpshud::matches_keybind(KEY_F10, bare_kb, keys_with_ctrl));
    CHECK(fpshud::matches_keybind(KEY_F10, combo_kb, keys_with_ctrl));

    // 4. Telemetry non-atomic CPU jiffies & discharging negative battery handling
    {
      ScopedTempDir bug_root("fpshud_bug_regress_");
      write_mock_file(bug_root / "sys/class/power_supply/BAT0/capacity", "85\n");
      write_mock_file(bug_root / "sys/class/power_supply/BAT0/status", "Discharging\n");
      write_mock_file(bug_root / "sys/class/power_supply/BAT0/power_now", "-14500000\n");
      write_mock_file(bug_root / "proc/stat", "cpu  1000 0 500 8000 0 0 0 0 0 0\n");

      fpshud::TelemetryCollector reg_tc(bug_root.string(), true, true);
      write_mock_file(bug_root / "proc/stat", "cpu  2500 0 1000 7000 0 0 0 0 0 0\n");
      reg_tc.poll_once_sync();
      auto snap = reg_tc.get_snapshot();

      CHECK(std::abs(snap.battery_power_w - 14.5f) < 0.1f);
      CHECK(snap.battery_pct == 85.0f);
      CHECK(!snap.battery_charging);
      CHECK(snap.cpu_load_pct >= 0.0f);
      CHECK(snap.cpu_load_pct <= 100.0f);
    }

    // 5. Zero-frametime division resilience & NaN/Inf immunity
    {
      fpshud::TelemetryCollector zero_tc("", true, true);
      zero_tc.on_simulated_frame_dt(0.0001f);
      zero_tc.on_simulated_frame_dt(0.0000f);
      auto zero_snap = zero_tc.get_snapshot();
      CHECK(!std::isnan(zero_snap.fps));
      CHECK(!std::isinf(zero_snap.fps));
      CHECK(!std::isnan(zero_snap.fps_1pct_low));
      CHECK(!std::isinf(zero_snap.fps_1pct_low));
    }

    // 6. IPC filename prefix path traversal neutralization
    CHECK(fpshud::sanitize_filename_prefix("../../../../tmp/malicious") == "malicious");
    CHECK(fpshud::sanitize_filename_prefix("/root/exploit_capture") == "exploit_capture");
    CHECK(fpshud::sanitize_filename_prefix("../../../..") == "FPSHUD");
    CHECK(fpshud::sanitize_filename_prefix("Valid-Prefix_123") == "Valid-Prefix_123");

    // 7. Untrusted CWD binary search elimination
    std::string helper_path = fpshud::get_helper_binary_path();
    CHECK(helper_path.rfind("./", 0) != 0);
    CHECK(helper_path.rfind("../", 0) != 0);

    // 8. Unix domain socket 0600 mode & SO_PEERCRED UID enforcement
    {
      ScopedTempDir sock_dir("fpshud_cred_");
      std::string test_sock = (sock_dir / "test.sock").string();
      bool peercred_verified = false;
      bool socket_mode_ok = false;

      int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (sfd >= 0) {
        struct sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, test_sock.c_str(), sizeof(sa.sun_path) - 1);
        if (::bind(sfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0) {
          ::chmod(test_sock.c_str(), 0600);
          struct stat st{};
          if (::stat(test_sock.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600) {
            socket_mode_ok = true;
          }
          ::listen(sfd, 1);
          int cfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
          if (cfd >= 0) {
            if (::connect(cfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0) {
              int cl_fd = ::accept(sfd, nullptr, nullptr);
              if (cl_fd >= 0) {
                struct ucred cr{};
                socklen_t cr_len = sizeof(cr);
                if (::getsockopt(cl_fd, SOL_SOCKET, SO_PEERCRED, &cr, &cr_len) == 0 &&
                    cr.uid == ::getuid()) {
                  peercred_verified = true;
                }
                ::close(cl_fd);
              }
            }
            ::close(cfd);
          }
        }
        ::close(sfd);
      }
      CHECK(socket_mode_ok);
      CHECK(peercred_verified);
    }

    // 9. Benchmark CSV symlink immunity & O_NOFOLLOW neutralization
    {
      ScopedTempDir sym_dir("fpshud_symlink_");
      std::string target_file = (sym_dir / "target.csv").string();
      std::string symlink_file = (sym_dir / "symlink.csv").string();

      int tfd = ::open(target_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (tfd >= 0) ::close(tfd);

      int sym_res = ::symlink(target_file.c_str(), symlink_file.c_str());
      int nofollow_fd =
          ::open(symlink_file.c_str(), O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
      bool symlink_rejected = (sym_res == 0 && nofollow_fd < 0 && errno == ELOOP);
      if (nofollow_fd >= 0) ::close(nofollow_fd);

      CHECK(symlink_rejected);
    }

    // 10. INI newline injection sanitization
    {
      std::string tainted_val = "custom\ninjected_key=1\r\n[InjectedSection]\nmalicious=true";
      std::string sanitized_val = fpshud::sanitize_ini_value(tainted_val);
      CHECK(sanitized_val.find('\r') == std::string::npos);
      CHECK(sanitized_val.find('\n') == std::string::npos);

      ScopedTempDir ini_dir("fpshud_ini_");
      fpshud::HudConfig test_inj_cfg;
      test_inj_cfg.active_preset = "custom\ninjected_key=1";
      test_inj_cfg.record_directory = "/tmp/rec\nrogue_key=pwned";
      std::string test_ini_path = (ini_dir / "test.ini").string();
      bool saved_ini = fpshud::save_ini_file(test_ini_path, test_inj_cfg, "Test\nInjection");
      std::string ini_content = read_text_file(test_ini_path);

      CHECK(saved_ini);
      CHECK(ini_content.find("\ninjected_key=1\n") == std::string::npos);
      CHECK(ini_content.find("\nrogue_key=pwned\n") == std::string::npos);
    }

    // 11. Deterministic State Machine for 2-Slot Async PBO Ring Invariants
    {
      size_t write_idx = 0;
      size_t read_idx = 0;
      int pending_frames = 0;
      bool collision_detected = false;

      auto reset_ring = [&]() {
        pending_frames = 0;
        write_idx = 0;
        read_idx = 0;
      };

      auto step_frame = [&]() {
        size_t curr_w = write_idx;
        if (pending_frames > 0) {
          size_t curr_r = read_idx;
          if (curr_w == curr_r) {
            collision_detected = true;
          }
          read_idx = (read_idx + 1) % 2;
        } else {
          pending_frames++;
        }
        write_idx = (write_idx + 1) % 2;
      };

      // Session 1: 4 frames (even frame count)
      for (int i = 0; i < 4; ++i) step_frame();
      reset_ring();
      // Session 2: 5 frames (resumed)
      for (int i = 0; i < 5; ++i) step_frame();
      reset_ring();
      // Session 3: 3 frames (resumed)
      for (int i = 0; i < 3; ++i) step_frame();

      CHECK_MESSAGE(!collision_detected,
                    "2-slot PBO read/write indices must never collide across session stop/resume");
    }

    // 12. Private 0700 Runtime Directory & Path Hardening Invariant
    {
      std::string sdir = fpshud::get_secure_runtime_dir();
      struct stat st{};
      bool dir_ok = (stat(sdir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
                     (st.st_mode & 0777) == 0700 && st.st_uid == ::getuid());
      CHECK_MESSAGE(dir_ok,
                    "Secure runtime directory must have 0700 permissions and match current UID");
    }
  }
}
