#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include "test_common.hpp"
#include "tools/sensor_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" const char* __lsan_default_suppressions() {
  return "leak:libnvidia-ml.so\n"
         "leak:init_gpu_sensors_locked\n"
         "leak:libGLX_nvidia.so\n"
         "leak:libcuda.so\n"
         "leak:libnvidia-encode.so\n"
         "leak:nvidia_drv_video.so\n"
         "leak:libva.so\n"
         "leak:libva-drm.so\n"
         "leak:old_pthread_cond_broadcast\n";
}

int main(int argc, char** argv) {
  // 1. Live sensor diagnostic tool intercept
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--probe-sensors") == 0 || std::strcmp(argv[i], "--status") == 0) {
      fpshud::tools::run_sensor_probe();
      return 0;
    }
  }

  // 2. Filter positional argument '1' (FPS_HUD=1 backward compatibility)
  std::vector<const char*> filtered_argv;
  filtered_argv.reserve(static_cast<size_t>(argc));

  bool found_positional_one = false;
  for (int i = 0; i < argc; ++i) {
    if (i > 0 && std::strcmp(argv[i], "1") == 0) {
      found_positional_one = true;
      continue;
    }
    filtered_argv.push_back(argv[i]);
  }

  if (found_positional_one) {
    ::setenv("FPS_HUD", "1", 1);
  } else if (std::getenv("FPS_HUD") == nullptr) {
    ::setenv("FPS_HUD", "1", 0);
  }

  // 3. Initialize and execute doctest Context
  doctest::Context context(static_cast<int>(filtered_argv.size()), filtered_argv.data());
  int res = context.run();
  if (context.shouldExit()) {
    return res;
  }
  return res;
}
