#pragma once

#include <cstdint>

namespace fpshud {

// Entrypoint for dedicated player worker process
// Called via: fpshud_recorder_bin --player <shm_name> <socket_path> <video_file_path>
int run_player_worker(int argc, char** argv);

}  // namespace fpshud
