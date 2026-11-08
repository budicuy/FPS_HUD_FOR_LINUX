#include "recorder_player.hpp"
#include "recorder_playback_protocol.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if defined(FPSHUD_RECORDER_DAEMON)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace fpshud {

int run_player_worker(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr,
                 "[FPSHUD Player] Usage: %s --player <shm_name> <socket_path> <video_file_path>\n",
                 argv[0]);
    return 1;
  }

  const std::string shm_name = argv[2];
  const std::string socket_path = argv[3];
  const std::string video_file = argv[4];

#if !defined(FPSHUD_RECORDER_DAEMON)
  // Non-daemon build (e.g. unit tests): validate arguments and return success/error cleanly
  if (video_file.empty() || shm_name.empty() || socket_path.empty()) {
    return 1;
  }
  return 0;
#else
  std::fprintf(stdout, "[FPSHUD Player] Starting playback engine for: %s\n", video_file.c_str());

  // 1. Open video container
  AVFormatContext* fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, video_file.c_str(), nullptr, nullptr) < 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: Could not open input file: %s\n",
                 video_file.c_str());
    return 2;
  }

  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: Failed to retrieve stream info.\n");
    avformat_close_input(&fmt_ctx);
    return 3;
  }

  int video_stream_idx = -1;
  for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_idx = static_cast<int>(i);
      break;
    }
  }

  if (video_stream_idx < 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: No video stream found in container.\n");
    avformat_close_input(&fmt_ctx);
    return 4;
  }

  AVStream* vid_stream = fmt_ctx->streams[video_stream_idx];
  AVCodecParameters* codec_par = vid_stream->codecpar;
  const AVCodec* decoder = avcodec_find_decoder(codec_par->codec_id);
  if (!decoder) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: Unsupported codec ID: %d\n", codec_par->codec_id);
    avformat_close_input(&fmt_ctx);
    return 5;
  }

  AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
  if (!codec_ctx || avcodec_parameters_to_context(codec_ctx, codec_par) < 0 ||
      avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: Failed to initialize video decoder.\n");
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return 6;
  }

  uint32_t width = static_cast<uint32_t>(codec_ctx->width);
  uint32_t height = static_cast<uint32_t>(codec_ctx->height);
  if (width == 0 || height == 0 || width > 7680 || height > 4320) {
    width = 1280;
    height = 720;
  }

  float duration_sec = 0.0f;
  if (fmt_ctx->duration > 0) {
    duration_sec = static_cast<float>(fmt_ctx->duration) / static_cast<float>(AV_TIME_BASE);
  } else if (vid_stream->duration > 0) {
    duration_sec = static_cast<float>(vid_stream->duration) *
                   static_cast<float>(av_q2d(vid_stream->time_base));
  }

  // 2. Setup POSIX Shared Memory
  size_t pixel_bytes = static_cast<size_t>(width) * height * 4;
  size_t shm_total_size = sizeof(PlaybackShmHeader) + pixel_bytes;

  int shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0600);
  if (shm_fd < 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: shm_open failed for %s\n", shm_name.c_str());
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return 7;
  }

  if (ftruncate(shm_fd, static_cast<off_t>(shm_total_size)) != 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: ftruncate failed.\n");
    close(shm_fd);
    shm_unlink(shm_name.c_str());
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return 8;
  }

  void* map_ptr = mmap(nullptr, shm_total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  if (map_ptr == MAP_FAILED) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: mmap failed.\n");
    close(shm_fd);
    shm_unlink(shm_name.c_str());
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return 9;
  }

  auto* header = reinterpret_cast<PlaybackShmHeader*>(map_ptr);
  header->magic = kPlaybackShmMagic;
  header->width = width;
  header->height = height;
  header->duration_sec = duration_sec;
  header->current_time_sec.store(0.0f, std::memory_order_relaxed);
  header->is_playing.store(true, std::memory_order_relaxed);
  header->is_ended.store(false, std::memory_order_relaxed);
  header->frame_seq.store(0, std::memory_order_relaxed);
  header->write_index.store(0, std::memory_order_relaxed);

  uint8_t* pixel_dest = reinterpret_cast<uint8_t*>(map_ptr) + sizeof(PlaybackShmHeader);
  std::memset(pixel_dest, 0, pixel_bytes);

  // 3. Setup Unix Domain Socket for Commands
  int server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_sock < 0) {
    munmap(map_ptr, shm_total_size);
    close(shm_fd);
    shm_unlink(shm_name.c_str());
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return 10;
  }

  sockaddr_un saddr{};
  saddr.sun_family = AF_UNIX;
  std::strncpy(saddr.sun_path, socket_path.c_str(), sizeof(saddr.sun_path) - 1);
  unlink(socket_path.c_str());

  if (bind(server_sock, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) < 0) {
    close(server_sock);
    munmap(map_ptr, shm_total_size);
    close(shm_fd);
    shm_unlink(shm_name.c_str());
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return 11;
  }
  chmod(socket_path.c_str(), 0600);
  listen(server_sock, 1);

  // 4. Initialize SwsContext for RGBA conversion
  SwsContext* sws =
      sws_getContext(codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt, width, height,
                     AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: sws_getContext failed.\n");
    close(server_sock);
    unlink(socket_path.c_str());
    munmap(map_ptr, shm_total_size);
    close(shm_fd);
    shm_unlink(shm_name.c_str());
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return 12;
  }

  AVFrame* raw_frame = av_frame_alloc();
  AVFrame* rgba_frame = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();

  uint8_t* rgba_buffers[4] = {pixel_dest, nullptr, nullptr, nullptr};
  int rgba_linesizes[4] = {static_cast<int>(width * 4), 0, 0, 0};

  AVRational avg_fps = av_guess_frame_rate(fmt_ctx, vid_stream, nullptr);
  double frame_interval =
      (avg_fps.num > 0 && avg_fps.den > 0) ? (1.0 / av_q2d(avg_fps)) : (1.0 / 60.0);
  float playback_speed = 1.0f;

  std::fprintf(
      stdout, "[FPSHUD Player] Ready: %ux%u @ %.2f fps (duration: %.1f s). Listening on %s\n",
      width, height, (avg_fps.num > 0 ? av_q2d(avg_fps) : 60.0), duration_sec, socket_path.c_str());

  int client_sock = -1;
  bool running = true;
  bool seek_pending = false;
  auto last_frame_time = std::chrono::steady_clock::now();

  while (running) {
    // 5. Handle incoming IPC commands without blocking
    if (client_sock < 0) {
      struct pollfd pfd{};
      pfd.fd = server_sock;
      pfd.events = POLLIN;
      if (poll(&pfd, 1, 1) > 0 && (pfd.revents & POLLIN)) {
        client_sock = accept(server_sock, nullptr, nullptr);
      }
    }

    if (client_sock >= 0) {
      struct pollfd pfd{};
      pfd.fd = client_sock;
      pfd.events = POLLIN;
      while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        IpcPlaybackRequest req{};
        ssize_t n = recv(client_sock, &req, sizeof(req), MSG_DONTWAIT);
        if (n <= 0) {
          close(client_sock);
          client_sock = -1;
          break;
        }
        if (n == sizeof(req)) {
          if (req.action == IpcPlaybackAction::Play) {
            header->is_playing.store(true, std::memory_order_relaxed);
          } else if (req.action == IpcPlaybackAction::Pause) {
            header->is_playing.store(false, std::memory_order_relaxed);
          } else if (req.action == IpcPlaybackAction::Seek) {
            float target_sec = std::max(0.0f, std::min(req.value, header->duration_sec));
            int64_t target_ts = static_cast<int64_t>(target_sec / av_q2d(vid_stream->time_base));
            av_seek_frame(fmt_ctx, video_stream_idx, target_ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(codec_ctx);
            header->current_time_sec.store(target_sec, std::memory_order_relaxed);
            header->is_ended.store(false, std::memory_order_relaxed);
            seek_pending = true;
          } else if (req.action == IpcPlaybackAction::SetSpeed) {
            if (req.value >= 0.25f && req.value <= 4.0f) {
              playback_speed = req.value;
            }
          } else if (req.action == IpcPlaybackAction::Close) {
            running = false;
            break;
          }
        }
      }
    }

    if (!running) break;

    // 6. Decode and display next video frame if playing or seek pending
    if (header->is_playing.load(std::memory_order_relaxed) || seek_pending) {
      auto now = std::chrono::steady_clock::now();
      double elapsed = std::chrono::duration<double>(now - last_frame_time).count();
      double target_delay = frame_interval / static_cast<double>(playback_speed);

      if (seek_pending || elapsed >= target_delay) {
        last_frame_time = now;
        seek_pending = false;

        int read_res = 0;
        bool frame_decoded = false;

        while ((read_res = av_read_frame(fmt_ctx, packet)) >= 0) {
          if (packet->stream_index == video_stream_idx) {
            if (avcodec_send_packet(codec_ctx, packet) == 0) {
              if (avcodec_receive_frame(codec_ctx, raw_frame) == 0) {
                // Scale YUV -> RGBA directly into SHM pixel buffer
                sws_scale(sws, raw_frame->data, raw_frame->linesize, 0, codec_ctx->height,
                          rgba_buffers, rgba_linesizes);

                float cur_sec =
                    (raw_frame->pts != AV_NOPTS_VALUE)
                        ? static_cast<float>(raw_frame->pts * av_q2d(vid_stream->time_base))
                        : header->current_time_sec.load(std::memory_order_relaxed) +
                              static_cast<float>(frame_interval);

                header->current_time_sec.store(cur_sec, std::memory_order_relaxed);
                header->frame_seq.fetch_add(1, std::memory_order_release);
                frame_decoded = true;
                av_packet_unref(packet);
                break;
              }
            }
          }
          av_packet_unref(packet);
        }

        if (read_res < 0 && !frame_decoded) {
          // Reached end of file
          header->is_ended.store(true, std::memory_order_relaxed);
          header->is_playing.store(false, std::memory_order_relaxed);
        }
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  // 7. Cleanup
  if (client_sock >= 0) close(client_sock);
  close(server_sock);
  unlink(socket_path.c_str());

  av_packet_free(&packet);
  av_frame_free(&raw_frame);
  av_frame_free(&rgba_frame);
  sws_freeContext(sws);
  avcodec_free_context(&codec_ctx);
  avformat_close_input(&fmt_ctx);

  munmap(map_ptr, shm_total_size);
  close(shm_fd);
  shm_unlink(shm_name.c_str());

  std::fprintf(stdout, "[FPSHUD Player] Playback worker exited cleanly.\n");
  return 0;
#endif
}

}  // namespace fpshud
