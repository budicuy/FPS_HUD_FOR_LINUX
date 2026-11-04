#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>

static void inject_container_host_env(void) {
  struct stat st;
  const char* host_root = NULL;
  if (stat("/run/host", &st) == 0) {
    host_root = "/run/host";
  } else if (stat("/var/run/host", &st) == 0) {
    host_root = "/var/run/host";
  }

  if (!host_root) {
    return;
  }

  static const char* const ld_candidates[] = {"/usr/lib/x86_64-linux-gnu",
                                              "/usr/lib/x86_64-linux-gnu/pulseaudio",
                                              "/usr/lib64",
                                              "/usr/lib64/pulseaudio",
                                              "/usr/lib",
                                              "/usr/lib/pulseaudio"};
  static const size_t num_ld = sizeof(ld_candidates) / sizeof(ld_candidates[0]);

  static const char* const libva_candidates[] = {"/usr/lib/x86_64-linux-gnu/dri", "/usr/lib64/dri",
                                                 "/usr/lib/dri"};
  static const size_t num_libva = sizeof(libva_candidates) / sizeof(libva_candidates[0]);

  char host_ld[4096] = {0};
  size_t host_ld_len = 0;
  for (size_t i = 0; i < num_ld; ++i) {
    char path[1024];
    snprintf(path, sizeof(path), "%s%s", host_root, ld_candidates[i]);
    if (stat(path, &st) == 0) {
      if (host_ld_len > 0 && host_ld_len < sizeof(host_ld) - 1) {
        host_ld[host_ld_len++] = ':';
        host_ld[host_ld_len] = '\0';
      }
      size_t plen = strlen(path);
      if (host_ld_len + plen < sizeof(host_ld) - 1) {
        memcpy(host_ld + host_ld_len, path, plen + 1);
        host_ld_len += plen;
      }
    }
  }

  if (host_ld_len > 0) {
    const char* cur_ld = getenv("LD_LIBRARY_PATH");
    char new_ld[8192] = {0};
    if (cur_ld && *cur_ld) {
      snprintf(new_ld, sizeof(new_ld), "%s:%s", host_ld, cur_ld);
    } else {
      snprintf(new_ld, sizeof(new_ld), "%s", host_ld);
    }
    setenv("LD_LIBRARY_PATH", new_ld, 1);
  }

  const char* cur_libva = getenv("LIBVA_DRIVERS_PATH");
  if (!cur_libva || !*cur_libva) {
    char host_libva[4096] = {0};
    size_t host_libva_len = 0;
    for (size_t i = 0; i < num_libva; ++i) {
      char path[1024];
      snprintf(path, sizeof(path), "%s%s", host_root, libva_candidates[i]);
      if (stat(path, &st) == 0) {
        if (host_libva_len > 0 && host_libva_len < sizeof(host_libva) - 1) {
          host_libva[host_libva_len++] = ':';
          host_libva[host_libva_len] = '\0';
        }
        size_t plen = strlen(path);
        if (host_libva_len + plen < sizeof(host_libva) - 1) {
          memcpy(host_libva + host_libva_len, path, plen + 1);
          host_libva_len += plen;
        }
      }
    }
    if (host_libva_len > 0) {
      setenv("LIBVA_DRIVERS_PATH", host_libva, 1);
    }
  }
}

int main(int argc, char* argv[]) {
  (void)argc;
  // 1. Determine directory where launcher is located
  char exe_path[4096] = {0};
  ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  char dir[4096] = {0};
  if (len > 0) {
    exe_path[len] = '\0';
    char* d = dirname(exe_path);
    if (d) {
      strncpy(dir, d, sizeof(dir) - 1);
    }
  }

  // 2. Locate the real binary fpshud_recorder_bin
  char target[4096] = {0};
  struct stat st;

  // Check same directory as launcher
  if (dir[0]) {
    snprintf(target, sizeof(target), "%s/fpshud_recorder_bin", dir);
  }
  // Check ~/.local/lib/fpshud/fpshud_recorder_bin or ~/.local/bin/fpshud_recorder_bin
  if (stat(target, &st) != 0) {
    const char* home = getenv("HOME");
    if (home && *home) {
      snprintf(target, sizeof(target), "%s/.local/lib/fpshud/fpshud_recorder_bin", home);
      if (stat(target, &st) != 0) {
        snprintf(target, sizeof(target), "%s/.local/bin/fpshud_recorder_bin", home);
      }
    }
  }

  // 3. Inject container host library paths if inside pressure-vessel / bubblewrap / flatpak
  inject_container_host_env();

  // Never leak game hooks or preloads into recorder process
  unsetenv("LD_PRELOAD");

  // 4. Execute the real binary
  argv[0] = target;
  execv(target, argv);

  // If execv failed, print diagnostic
  perror("fpshud_recorder: execv failed");
  return 127;
}
