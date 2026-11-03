#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>

int main(int argc, char* argv[]) {
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
    char new_ld[8192] = {0};
    const char* cur_ld = getenv("LD_LIBRARY_PATH");
    if (stat("/run/host/usr/lib", &st) == 0) {
        snprintf(new_ld, sizeof(new_ld), "/run/host/usr/lib/pulseaudio:/run/host/usr/lib%s%s",
                 (cur_ld && *cur_ld) ? ":" : "",
                 (cur_ld && *cur_ld) ? cur_ld : "");
        setenv("LD_LIBRARY_PATH", new_ld, 1);
    } else if (stat("/var/run/host/usr/lib", &st) == 0) {
        snprintf(new_ld, sizeof(new_ld), "/var/run/host/usr/lib/pulseaudio:/var/run/host/usr/lib%s%s",
                 (cur_ld && *cur_ld) ? ":" : "",
                 (cur_ld && *cur_ld) ? cur_ld : "");
        setenv("LD_LIBRARY_PATH", new_ld, 1);
    }

    // Never leak game hooks or preloads into recorder process
    unsetenv("LD_PRELOAD");

    // 4. Execute the real binary
    argv[0] = target;
    execv(target, argv);

    // If execv failed, print diagnostic
    perror("fpshud_recorder: execv failed");
    return 127;
}
