#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>

namespace fpshud {

// Fast zero-allocation sysfs reader using stack buffer
inline bool read_sysfs_buf(const char* path, char* buf, size_t buf_size) noexcept {
  if (!path || !*path || buf_size == 0) return false;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  ssize_t n = read(fd, buf, buf_size - 1);
  close(fd);
  if (n <= 0) {
    buf[0] = '\0';
    return false;
  }
  buf[n] = '\0';
  for (ssize_t i = 0; i < n; ++i) {
    if (buf[i] == '\n' || buf[i] == '\r') {
      buf[i] = '\0';
      break;
    }
  }
  return true;
}

inline bool read_sysfs_u64(const char* path, uint64_t& out) noexcept {
  char buf[64]{};
  if (!read_sysfs_buf(path, buf, sizeof(buf))) return false;
  char* end = nullptr;
  unsigned long long val = std::strtoull(buf, &end, 10);
  if (end == buf) return false;
  out = static_cast<uint64_t>(val);
  return true;
}

inline bool read_sysfs_uint64(const char* path, uint64_t& out) noexcept {
  return read_sysfs_u64(path, out);
}

inline bool is_sysfs_readable(const char* path) noexcept {
  if (!path || !*path) return false;
  struct stat st{};
  if (::stat(path, &st) != 0) return false;
  if ((st.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) == 0) return false;
  return ::access(path, R_OK) == 0;
}

// Shortens verbose CPU/GPU marketing strings so they fit cleanly in the GUI header
inline void clean_hardware_name(const char* raw, char* out, size_t out_size) noexcept {
  if (!raw || !*raw || out_size == 0) return;
  std::string s(raw);

  const char* strip_tokens[] = {
      "(R)",       "(TM)",      "(tm)",      "CPU",       "Processor",
      "12th Gen ", "13th Gen ", "14th Gen ", "11th Gen ", "10th Gen ",
      "Intel ",    "NVIDIA ",   "GeForce ",  "AMD ",      "Advanced Micro Devices, Inc. "};
  for (const char* tok : strip_tokens) {
    size_t pos = 0;
    size_t len = std::strlen(tok);
    while ((pos = s.find(tok, pos)) != std::string::npos) {
      s.erase(pos, len);
    }
  }
  // Strip trailing "@ 2.50GHz" or "with Radeon Graphics"
  if (size_t at = s.find('@'); at != std::string::npos) {
    s.erase(at);
  }
  if (size_t w = s.find("with "); w != std::string::npos) {
    s.erase(w);
  }
  // Collapse multiple spaces and trim
  std::string compact;
  bool prev_space = true;
  for (char c : s) {
    if (c == ' ' || c == '\t') {
      if (!prev_space) compact.push_back(' ');
      prev_space = true;
    } else {
      compact.push_back(c);
      prev_space = false;
    }
  }
  while (!compact.empty() && compact.back() == ' ') compact.pop_back();
  std::snprintf(out, out_size, "%s", compact.empty() ? raw : compact.c_str());
}

// Resolves PCI (vendor_id, device_id) such as ("0x1002", "0x747e") against system pci.ids
inline bool lookup_pci_device_name(const char* vendor_hex, const char* device_hex, char* out,
                                   size_t out_size) noexcept {
  if (!vendor_hex || !device_hex || out_size == 0) return false;
  const char* v_id =
      (std::strncmp(vendor_hex, "0x", 2) == 0 || std::strncmp(vendor_hex, "0X", 2) == 0)
          ? vendor_hex + 2
          : vendor_hex;
  const char* d_id =
      (std::strncmp(device_hex, "0x", 2) == 0 || std::strncmp(device_hex, "0X", 2) == 0)
          ? device_hex + 2
          : device_hex;
  if (std::strlen(v_id) != 4 || std::strlen(d_id) != 4) return false;

  const char* pci_db_paths[] = {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
                                "/usr/share/pci.ids"};

  for (const char* db_path : pci_db_paths) {
    FILE* fp = std::fopen(db_path, "r");
    if (!fp) continue;

    bool in_vendor = false;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), fp)) {
      std::string line(buf);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      if (line.empty() || line[0] == '#') continue;

      if (line[0] != '\t') {
        if (in_vendor) break;  // Reached next vendor section
        if (line.size() >= 4 && strncasecmp(line.c_str(), v_id, 4) == 0 &&
            (line[4] == ' ' || line[4] == '\t')) {
          in_vendor = true;
        }
      } else if (in_vendor && line.size() > 6 && line[0] == '\t' && line[1] != '\t') {
        if (strncasecmp(line.c_str() + 1, d_id, 4) == 0 && (line[5] == ' ' || line[5] == '\t')) {
          std::string desc = line.substr(5);
          while (!desc.empty() && (desc.front() == ' ' || desc.front() == '\t')) {
            desc.erase(desc.begin());
          }
          // Prefer marketing name inside brackets [...] if present
          auto lb = desc.rfind('[');
          auto rb = desc.rfind(']');
          if (lb != std::string::npos && rb != std::string::npos && rb > lb + 1) {
            desc = desc.substr(lb + 1, rb - lb - 1);
          }
          clean_hardware_name(desc.c_str(), out, out_size);
          std::fclose(fp);
          return true;
        }
      }
    }
    std::fclose(fp);
  }
  return false;
}

}  // namespace fpshud
