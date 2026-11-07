#include "recorder_ipc.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// FFmpeg C headers
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// PulseAudio Asynchronous API for lightweight desktop/game audio recording
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_running{true};
std::atomic<bool> g_has_received_game_frame{false};

void sigint_handler(int) { g_running.store(false, std::memory_order_relaxed); }

std::string generate_timestamp_filename(const std::string& prefix, const std::string& ext) {
  std::time_t t = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm_buf);
  return prefix + "_" + buf + ext;
}

// Automatically discovers the active default monitor audio device name
std::string get_default_monitor_source() {
  // 1. Try pactl get-default-sink
  FILE* fp = popen("pactl get-default-sink 2>/dev/null", "r");
  if (fp) {
    char buf[256] = {0};
    if (std::fgets(buf, sizeof(buf), fp)) {
      std::string s(buf);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
      if (!s.empty()) {
        pclose(fp);
        return s + ".monitor";
      }
    }
    pclose(fp);
  }
  // 2. PulseAudio / PipeWire standard fallback alias
  return "@DEFAULT_MONITOR@";
}

AVBufferRef* create_vaapi_device() {
  for (int node_id = 128; node_id <= 135; ++node_id) {
    std::string dev_node = "/dev/dri/renderD" + std::to_string(node_id);
    if (access(dev_node.c_str(), R_OK | W_OK) == 0) {
      AVBufferRef* dev_ctx = nullptr;
      if (av_hwdevice_ctx_create(&dev_ctx, AV_HWDEVICE_TYPE_VAAPI, dev_node.c_str(), nullptr, 0) ==
              0 &&
          dev_ctx) {
        return dev_ctx;
      }
    }
  }
  return nullptr;
}

struct PulseAudioCapture {
  pa_mainloop* ml = nullptr;
  pa_context* ctx = nullptr;
  pa_stream* stream = nullptr;

  PulseAudioCapture() = default;
  ~PulseAudioCapture() { cleanup(); }
  PulseAudioCapture(const PulseAudioCapture&) = delete;
  PulseAudioCapture& operator=(const PulseAudioCapture&) = delete;

  void cleanup() {
    if (stream) {
      pa_stream_disconnect(stream);
      pa_stream_unref(stream);
      stream = nullptr;
    }
    if (ctx) {
      pa_context_disconnect(ctx);
      pa_context_unref(ctx);
      ctx = nullptr;
    }
    if (ml) {
      pa_mainloop_free(ml);
      ml = nullptr;
    }
  }

  bool init(const std::string& monitor_source, const std::atomic<bool>& running) {
    ml = pa_mainloop_new();
    if (!ml) return false;
    pa_mainloop_api* api = pa_mainloop_get_api(ml);
    ctx = pa_context_new(api, "FPSHUD_Recorder");
    if (!ctx) {
      cleanup();
      return false;
    }

    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      cleanup();
      return false;
    }

    auto start = std::chrono::steady_clock::now();
    while (running.load(std::memory_order_relaxed)) {
      pa_context_state_t state = pa_context_get_state(ctx);
      if (state == PA_CONTEXT_READY) break;
      if (!PA_CONTEXT_IS_GOOD(state)) {
        cleanup();
        return false;
      }
      pa_mainloop_iterate(ml, 0, nullptr);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                start)
              .count() > 500) {
        cleanup();
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!running.load(std::memory_order_relaxed)) {
      cleanup();
      return false;
    }

    pa_sample_spec ss{};
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 48000;
    ss.channels = 2;

    stream = pa_stream_new(ctx, "Gameplay Audio", &ss, nullptr);
    if (!stream) {
      cleanup();
      return false;
    }

    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.fragsize = 4096;
    attr.tlength = static_cast<uint32_t>(-1);
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);

    if (pa_stream_connect_record(stream, monitor_source.c_str(), &attr, PA_STREAM_ADJUST_LATENCY) <
        0) {
      cleanup();
      return false;
    }

    start = std::chrono::steady_clock::now();
    while (running.load(std::memory_order_relaxed)) {
      pa_stream_state_t state = pa_stream_get_state(stream);
      if (state == PA_STREAM_READY) break;
      if (!PA_STREAM_IS_GOOD(state)) {
        cleanup();
        return false;
      }
      pa_mainloop_iterate(ml, 0, nullptr);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                start)
              .count() > 500) {
        cleanup();
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!running.load(std::memory_order_relaxed) ||
        pa_stream_get_state(stream) != PA_STREAM_READY) {
      cleanup();
      return false;
    }

    return true;
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigint_handler);
  signal(SIGPIPE, SIG_IGN);

  std::string socket_path;
  std::string shm_path;
  int target_width = 1920;
  int target_height = 1080;
  int target_fps = 60;
  int target_bitrate_kbps = 18000;
  std::string codec_choice = "hevc";
  int quality_mode = 0;  // 0 = CQP, 1 = Fixed VBR
  int target_cq = 22;    // 16 - 30
  std::string save_dir;
  bool probe_encoders = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--probe-encoders")
      probe_encoders = true;
    else if (arg == "--socket" && i + 1 < argc)
      socket_path = argv[++i];
    else if (arg == "--shm" && i + 1 < argc)
      shm_path = argv[++i];
    else if (arg == "--width" && i + 1 < argc)
      target_width = std::max(64, std::atoi(argv[++i]));
    else if (arg == "--height" && i + 1 < argc)
      target_height = std::max(64, std::atoi(argv[++i]));
    else if (arg == "--fps" && i + 1 < argc)
      target_fps = std::clamp(std::atoi(argv[++i]), 15, 240);
    else if (arg == "--bitrate" && i + 1 < argc)
      target_bitrate_kbps = std::clamp(std::atoi(argv[++i]), 1000, 100000);
    else if (arg == "--codec" && i + 1 < argc)
      codec_choice = argv[++i];
    else if (arg == "--cqp-mode" && i + 1 < argc)
      quality_mode = std::clamp(std::atoi(argv[++i]), 0, 1);
    else if (arg == "--cqp" && i + 1 < argc)
      target_cq = std::clamp(std::atoi(argv[++i]), 16, 30);
    else if (arg == "--dir" && i + 1 < argc)
      save_dir = argv[++i];
  }

  if (probe_encoders) {
    std::printf("FPSHUD Recorder Encoder Probe:\n");
    std::printf(
        "HEVC Candidates: hevc_nvenc, hevc_vaapi, libx265, hevc, h264_nvenc, h264_vaapi, libx264, "
        "h264\n");
    std::printf("H264 Candidates: h264_nvenc, h264_vaapi, libx264, h264\n");
    std::printf("Compiled Codec Support:\n");
    const char* probe_list[] = {"hevc_vaapi", "h264_vaapi", "hevc_nvenc",
                                "h264_nvenc", "libx265",    "libx264"};
    for (const char* name : probe_list) {
      const AVCodec* c = avcodec_find_encoder_by_name(name);
      std::printf("  %-12s: %s\n", name, c ? "COMPILED" : "NOT_FOUND");
    }
    return 0;
  }

  if (socket_path.empty()) {
    std::fprintf(stderr,
                 "Usage: fpshud_recorder --socket <path> [--shm <path>] [--width W] [--height H] "
                 "[--fps F] [--bitrate B] [--dir D]\n");
    return 1;
  }

  void* shm_base = nullptr;
  size_t shm_size = 0;
  fpshud::ShmFrameHeader* shm_hdr = nullptr;

  auto try_map_shm = [&](const std::string& path) {
    if (shm_base || path.empty()) return;
    int sfd = open(path.c_str(), O_RDONLY);
    if (sfd >= 0) {
      struct stat st{};
      if (fstat(sfd, &st) == 0 && st.st_size > 4096) {
        shm_size = static_cast<size_t>(st.st_size);
        shm_base = mmap(nullptr, shm_size, PROT_READ, MAP_SHARED, sfd, 0);
        if (shm_base != MAP_FAILED) {
          shm_hdr = reinterpret_cast<fpshud::ShmFrameHeader*>(shm_base);
        } else {
          shm_base = nullptr;
        }
      }
      close(sfd);
    }
  };
  try_map_shm(shm_path);

  // 1. Setup Unix Domain Socket Server for On-Demand Control
  unlink(socket_path.c_str());
  int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::perror("[FPSHUD Recorder] socket() failed");
    return 1;
  }

  struct sockaddr_un saddr{};
  saddr.sun_family = AF_UNIX;
  std::strncpy(saddr.sun_path, socket_path.c_str(), sizeof(saddr.sun_path) - 1);

  if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) < 0) {
    std::perror("[FPSHUD Recorder] bind() failed");
    close(server_fd);
    return 1;
  }

  if (listen(server_fd, 2) < 0) {
    std::perror("[FPSHUD Recorder] listen() failed");
    close(server_fd);
    return 1;
  }

  // Wait for libfpshud.so client connection (with 5 second timeout)
  struct timeval tv{};
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  int client_fd = accept(server_fd, nullptr, nullptr);
  if (client_fd < 0) {
    std::fprintf(stderr, "[FPSHUD Recorder] Timeout waiting for libfpshud.so client.\n");
    close(server_fd);
    unlink(socket_path.c_str());
    return 0;
  }

  // 2. Read Start Command & Parameters
  fpshud::IpcPacketHeader hdr{};
  ssize_t n = recv(client_fd, &hdr, sizeof(hdr), 0);
  if (n < static_cast<ssize_t>(sizeof(hdr)) || hdr.magic != fpshud::kRecorderIpcMagic) {
    close(client_fd);
    close(server_fd);
    unlink(socket_path.c_str());
    return 1;
  }

  fpshud::IpcStartPayload start_cfg{};
  if (hdr.payload_size > 0) {
    recv(client_fd, &start_cfg, sizeof(start_cfg), 0);
    if (start_cfg.width > 0) target_width = std::max(64, static_cast<int>(start_cfg.width));
    if (start_cfg.height > 0) target_height = std::max(64, static_cast<int>(start_cfg.height));
    if (start_cfg.fps > 0) target_fps = std::clamp(static_cast<int>(start_cfg.fps), 15, 240);
    if (start_cfg.bitrate_kbps > 0) target_bitrate_kbps = start_cfg.bitrate_kbps;
    if (start_cfg.save_dir[0] != '\0') save_dir = start_cfg.save_dir;
    codec_choice = (start_cfg.codec == 1) ? "hevc" : "h264";
    quality_mode = (start_cfg.quality_mode == 1) ? 1 : 0;
    if (start_cfg.cq_level >= 16 && start_cfg.cq_level <= 30) target_cq = start_cfg.cq_level;
  }

  if (save_dir.empty()) {
    save_dir = "/tmp";
  }
  std::error_code ec;
  fs::create_directories(save_dir, ec);

  // Ensure width and height are even for YUV420p/NV12 video encoding and at least 64
  target_width = std::max(64, target_width & ~1);
  target_height = std::max(64, target_height & ~1);

  std::string ext =
      (start_cfg.container == 1) ? ".mkv" : ((start_cfg.container == 2) ? ".webm" : ".mp4");
  std::string out_filename =
      (fs::path(save_dir) / generate_timestamp_filename("FPSHUD", ext)).string();

  std::fprintf(stderr, "[FPSHUD Recorder] Starting session: %dx%d @ %d FPS [%s, %s] -> %s\n",
               target_width, target_height, target_fps, codec_choice.c_str(),
               (quality_mode == 0) ? ("CQP " + std::to_string(target_cq)).c_str()
                                   : (std::to_string(target_bitrate_kbps) + "k VBR").c_str(),
               out_filename.c_str());

  // 3. Initialize FFmpeg Output Format & Video Encoder
  av_log_set_level(AV_LOG_WARNING);
  AVFormatContext* oc = nullptr;
  if (avformat_alloc_output_context2(&oc, nullptr, nullptr, out_filename.c_str()) < 0 || !oc) {
    std::fprintf(stderr, "[FPSHUD Recorder] Could not allocate output context.\n");
    close(client_fd);
    close(server_fd);
    unlink(socket_path.c_str());
    return 1;
  }

  // Find best video encoder using candidate fallback loop:
  // HEVC:  {"hevc_nvenc", "hevc_vaapi", "libx265", "hevc", "h264_nvenc", "h264_vaapi", "libx264",
  // "h264"} H.264: {"h264_nvenc", "h264_vaapi", "libx264", "h264"}
  std::vector<const char*> candidates;
  if (codec_choice == "hevc") {
    candidates = {"hevc_nvenc", "hevc_vaapi", "libx265", "hevc",
                  "h264_nvenc", "h264_vaapi", "libx264", "h264"};
  } else {
    candidates = {"h264_nvenc", "h264_vaapi", "libx264", "h264"};
  }

  const AVCodec* vcodec = nullptr;
  AVCodecContext* vctx = nullptr;
  AVBufferRef* hw_device_ctx = nullptr;

  for (const char* cand : candidates) {
    const AVCodec* c = avcodec_find_encoder_by_name(cand);
    if (!c) {
      if (std::strcmp(cand, "hevc") == 0)
        c = avcodec_find_encoder(AV_CODEC_ID_HEVC);
      else if (std::strcmp(cand, "h264") == 0)
        c = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!c) continue;

    AVCodecContext* ctx = avcodec_alloc_context3(c);
    if (!ctx) continue;

    bool is_nv = (std::strstr(c->name, "nvenc") != nullptr);
    bool is_va = (std::strstr(c->name, "vaapi") != nullptr);

    if (is_va) {
      if (!hw_device_ctx) {
        hw_device_ctx = create_vaapi_device();
      }
      if (!hw_device_ctx) {
        std::fprintf(stderr, "[FPSHUD Recorder] VAAPI device node inaccessible, skipping %s...\n",
                     cand);
        avcodec_free_context(&ctx);
        continue;
      }

      AVBufferRef* hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
      if (!hw_frames_ref) {
        avcodec_free_context(&ctx);
        continue;
      }
      AVHWFramesContext* frames_ctx = reinterpret_cast<AVHWFramesContext*>(hw_frames_ref->data);
      frames_ctx->format = AV_PIX_FMT_VAAPI;
      frames_ctx->sw_format = AV_PIX_FMT_NV12;
      frames_ctx->width = target_width;
      frames_ctx->height = target_height;
      frames_ctx->initial_pool_size = 20;

      if (av_hwframe_ctx_init(hw_frames_ref) < 0) {
        av_buffer_unref(&hw_frames_ref);
        avcodec_free_context(&ctx);
        continue;
      }

      ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
      av_buffer_unref(&hw_frames_ref);
      ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    }

    bool is_vfr = (start_cfg.vfr_mode == 1);
    int max_fps = (start_cfg.vfr_max_fps > 0) ? static_cast<int>(start_cfg.vfr_max_fps) : 120;

    ctx->codec_id = c->id;
    ctx->width = target_width;
    ctx->height = target_height;
    if (is_vfr) {
      ctx->time_base = AVRational{1, 1000000};  // Microsecond timebase for native VFR
      ctx->framerate = AVRational{max_fps, 1};  // Nominal max framerate
      ctx->gop_size = max_fps * 2;
    } else {
      ctx->time_base = AVRational{1, target_fps};
      ctx->framerate = AVRational{target_fps, 1};
      ctx->gop_size = target_fps * 2;  // 2-second keyframe interval
    }
    if (is_va) {
      ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    } else if (is_nv) {
      ctx->pix_fmt = AV_PIX_FMT_NV12;
    } else {
      ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }
    ctx->max_b_frames = 0;  // Zero B-frames (real-time stream)
    ctx->rc_max_rate = static_cast<int64_t>(target_bitrate_kbps) * 1000;
    ctx->rc_buffer_size = static_cast<int>(ctx->rc_max_rate * 2);

    if (quality_mode == 0) {
      // CQP Mode (Adaptive Constant Quantization Parameter with VBV Maxrate Ceiling)
      if (is_nv) {
        av_opt_set(ctx->priv_data, "preset", "p4", 0);  // p4 = balanced quality/speed
        av_opt_set(ctx->priv_data, "tune", "ll", 0);    // Low latency
        av_opt_set(ctx->priv_data, "rc", "constqp", 0);
        av_opt_set_int(ctx->priv_data, "qp", target_cq, 0);
        av_opt_set_int(ctx->priv_data, "maxrate", ctx->rc_max_rate, 0);
        av_opt_set_int(ctx->priv_data, "bufsize", ctx->rc_buffer_size, 0);
        av_opt_set(ctx->priv_data, "spatial-aq", "1", 0);  // Preserves HUD text and sharp UI fonts
        av_opt_set(ctx->priv_data, "temporal-aq", "1",
                   0);  // Bitrate savings on static/dialogue scenes
      } else if (is_va) {
        ctx->global_quality = target_cq;
        av_opt_set(ctx->priv_data, "rc_mode", "CQP", 0);
        av_opt_set_int(ctx->priv_data, "qp", target_cq, 0);
      } else {
        av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set_int(ctx->priv_data, "crf", target_cq, 0);
        av_opt_set_int(ctx->priv_data, "maxrate", ctx->rc_max_rate, 0);
        av_opt_set_int(ctx->priv_data, "bufsize", ctx->rc_buffer_size, 0);
      }
    } else {
      // VBR Mode (Target Bitrate)
      ctx->bit_rate = static_cast<int64_t>(target_bitrate_kbps) * 1000;
      if (is_nv) {
        av_opt_set(ctx->priv_data, "preset", "p4", 0);  // p4 = balanced quality/speed
        av_opt_set(ctx->priv_data, "tune", "ll", 0);    // Low latency
        av_opt_set(ctx->priv_data, "rc", "vbr", 0);
        av_opt_set_int(ctx->priv_data, "maxrate", ctx->rc_max_rate, 0);
        av_opt_set_int(ctx->priv_data, "bufsize", ctx->rc_buffer_size, 0);
      } else if (is_va) {
        av_opt_set(ctx->priv_data, "rc_mode", "VBR", 0);
      } else {
        av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set_int(ctx->priv_data, "maxrate", ctx->rc_max_rate, 0);
        av_opt_set_int(ctx->priv_data, "bufsize", ctx->rc_buffer_size, 0);
      }
    }

    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(ctx, c, nullptr) >= 0) {
      vcodec = c;
      vctx = ctx;
      if (c->id == AV_CODEC_ID_H264) {
        codec_choice = "h264";
      }
      std::fprintf(stderr, "[FPSHUD Recorder] Successfully initialized video encoder: %s (%s)\n",
                   vcodec->name, codec_choice.c_str());
      if (quality_mode == 0) {
        if (is_nv) {
          std::fprintf(
              stderr,
              "[FPSHUD Recorder] NVENC Adaptive CQP: CQ=%d, Spatial-AQ=ON, Temporal-AQ=ON\n",
              target_cq);
        } else if (is_va) {
          std::fprintf(stderr, "[FPSHUD Recorder] VAAPI CQP: CQ=%d\n", target_cq);
        } else {
          std::fprintf(stderr, "[FPSHUD Recorder] Software CRF: %d\n", target_cq);
        }
      } else {
        if (is_nv) {
          std::fprintf(stderr, "[FPSHUD Recorder] NVENC VBR: %d kbps\n", target_bitrate_kbps);
        } else if (is_va) {
          std::fprintf(stderr, "[FPSHUD Recorder] VAAPI VBR: %d kbps\n", target_bitrate_kbps);
        } else {
          std::fprintf(stderr, "[FPSHUD Recorder] Software VBR: %d kbps\n", target_bitrate_kbps);
        }
      }
      break;
    } else {
      std::fprintf(stderr,
                   "[FPSHUD Recorder] Candidate encoder '%s' failed to open, falling back...\n",
                   cand);
      avcodec_free_context(&ctx);
    }
  }

  if ((!vcodec || std::strstr(vcodec->name, "vaapi") == nullptr) && hw_device_ctx) {
    av_buffer_unref(&hw_device_ctx);
  }

  if (!vctx) {
    std::fprintf(stderr, "[FPSHUD Recorder] No suitable video encoder could be opened.\n");
    avformat_free_context(oc);
    close(client_fd);
    close(server_fd);
    unlink(socket_path.c_str());
    return 1;
  }

  AVStream* vstream = avformat_new_stream(oc, nullptr);
  if (!vstream) {
    std::fprintf(stderr, "[FPSHUD Recorder] Could not allocate video stream.\n");
    avcodec_free_context(&vctx);
    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
    avformat_free_context(oc);
    close(client_fd);
    close(server_fd);
    unlink(socket_path.c_str());
    return 1;
  }
  vstream->id = 0;
  vstream->time_base = vctx->time_base;
  avcodec_parameters_from_context(vstream->codecpar, vctx);
  if (vcodec->id == AV_CODEC_ID_HEVC && (start_cfg.container == 0 || ext == ".mp4")) {
    vstream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
  }

  // 4. Initialize Audio Stream via PulseAudio / PipeWire
  AVStream* astream = nullptr;
  AVCodecContext* actx = nullptr;
  bool audio_enabled = (start_cfg.audio_source != 3);  // 3 = Mute
  std::string monitor_source;

  if (audio_enabled) {
    const AVCodec* acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (acodec) {
      astream = avformat_new_stream(oc, nullptr);
      actx = avcodec_alloc_context3(acodec);
      astream->id = 1;
      actx->sample_rate = 48000;
      av_channel_layout_default(&actx->ch_layout, 2);
      actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
      actx->bit_rate = 192000;
      actx->time_base = AVRational{1, actx->sample_rate};
      astream->time_base = actx->time_base;

      actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

      if (avcodec_open2(actx, acodec, nullptr) >= 0) {
        avcodec_parameters_from_context(astream->codecpar, actx);
        monitor_source = get_default_monitor_source();
      }
    }
  }

  // Set MP4 FastStart flag for instant web / discord playback & disable per-packet flush
  AVDictionary* opt_dict = nullptr;
  av_dict_set(&opt_dict, "movflags", "+faststart", 0);
  av_dict_set(&opt_dict, "flush_packets", "0", 0);

  // If HEVC inside MP4, set universal 'hvc1' tag so Haruna, VLC, MPV, browsers playback correctly
  if (vcodec->id == AV_CODEC_ID_HEVC && (start_cfg.container == 0 || ext == ".mp4")) {
    av_dict_set(&opt_dict, "tag:v", "hvc1", 0);
  }

  auto cleanup_early_failure = [&]() {
    av_dict_free(&opt_dict);
    if (vctx) avcodec_free_context(&vctx);
    if (actx) avcodec_free_context(&actx);
    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
    if (oc) {
      if (oc->pb && !(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
      avformat_free_context(oc);
    }
    if (client_fd >= 0) close(client_fd);
    if (server_fd >= 0) close(server_fd);
    if (!socket_path.empty()) unlink(socket_path.c_str());
  };

  bool is_replay = (start_cfg.replay_mode == 1);
  int replay_sec = std::clamp<int>(
      start_cfg.replay_duration_sec > 0 ? start_cfg.replay_duration_sec : 30, 5, 300);

  if (!is_replay) {
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
      if (avio_open(&oc->pb, out_filename.c_str(), AVIO_FLAG_WRITE) < 0) {
        std::fprintf(stderr, "[FPSHUD Recorder] Could not open output file: %s\n",
                     out_filename.c_str());
        cleanup_early_failure();
        return 1;
      }
    }

    if (avformat_write_header(oc, &opt_dict) < 0) {
      std::fprintf(stderr, "[FPSHUD Recorder] Failed to write format header.\n");
      cleanup_early_failure();
      return 1;
    }
  }
  av_dict_free(&opt_dict);

  // 5. Worker Loop for Video & Audio Multiplexing
  std::mutex mux_mutex;
  std::atomic<int64_t> video_frame_idx{0};
  auto replay_clock_start = std::chrono::steady_clock::now();

  struct BufferedPacket {
    AVPacket* pkt = nullptr;
    int stream_index = 0;
    int64_t timestamp_us = 0;
    bool is_keyframe = false;
  };
  std::deque<BufferedPacket> packet_ring;

  auto push_packet_or_mux = [&](AVPacket* pkt, int stream_idx, AVRational src_tb,
                                AVRational stream_tb) {
    std::lock_guard<std::mutex> lock(mux_mutex);
    if (!is_replay) {
      av_packet_rescale_ts(pkt, src_tb, stream_tb);
      pkt->stream_index = stream_idx;
      av_interleaved_write_frame(oc, pkt);
      return;
    }

    AVPacket* cloned = av_packet_clone(pkt);
    if (!cloned) return;
    cloned->stream_index = stream_idx;
    auto now_tp = std::chrono::steady_clock::now();
    int64_t now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now_tp - replay_clock_start).count();
    bool is_key = (stream_idx == 0) && ((cloned->flags & AV_PKT_FLAG_KEY) != 0);
    packet_ring.push_back(BufferedPacket{cloned, stream_idx, now_us, is_key});

    int64_t cutoff_us = now_us - static_cast<int64_t>(replay_sec) * 1000000LL;
    int64_t hard_cutoff_us = now_us - static_cast<int64_t>(replay_sec + 4) * 1000000LL;

    // Find the newest keyframe at or before cutoff_us so we can safely drop older packets
    size_t keep_from = 0;
    for (size_t i = 0; i < packet_ring.size(); ++i) {
      if (packet_ring[i].timestamp_us <= cutoff_us && packet_ring[i].is_keyframe) {
        keep_from = i;
      } else if (packet_ring[i].timestamp_us > cutoff_us) {
        break;
      }
    }
    while (keep_from > 0 && !packet_ring.empty()) {
      av_packet_free(&packet_ring.front().pkt);
      packet_ring.pop_front();
      --keep_from;
    }
    while (!packet_ring.empty() && packet_ring.front().timestamp_us < hard_cutoff_us) {
      av_packet_free(&packet_ring.front().pkt);
      packet_ring.pop_front();
    }
  };

  bool is_vaapi_encoder = (vctx->pix_fmt == AV_PIX_FMT_VAAPI);
  AVPixelFormat sw_pix_fmt = is_vaapi_encoder ? AV_PIX_FMT_NV12 : vctx->pix_fmt;

  // Allocate staging video frame (NV12 or YUV420P)
  AVFrame* vframe = av_frame_alloc();
  vframe->format = sw_pix_fmt;
  vframe->width = target_width;
  vframe->height = target_height;
  av_frame_get_buffer(vframe, 0);

  // Initialize frame background
  if (vframe->data[0]) {
    std::memset(vframe->data[0], 16, vframe->linesize[0] * target_height);
  }
  if (vframe->data[1]) {
    std::memset(vframe->data[1], 128, vframe->linesize[1] * (target_height / 2));
  }
  if (vframe->data[2]) {
    std::memset(vframe->data[2], 128, vframe->linesize[2] * (target_height / 2));
  }

  AVFrame* hw_frame = nullptr;
  if (is_vaapi_encoder) {
    hw_frame = av_frame_alloc();
  }

  // SwsContext for RGB/NV12 conversion if needed
  SwsContext* sws_ctx =
      sws_getContext(target_width, target_height, AV_PIX_FMT_RGBA, target_width, target_height,
                     sw_pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

  // Spawn Audio Capture Thread (Asynchronous libpulse)
  std::thread audio_thread([&]() {
    if (!actx || !audio_enabled || monitor_source.empty()) return;

    PulseAudioCapture pulse;
    if (!pulse.init(monitor_source, g_running)) {
      std::fprintf(
          stderr,
          "[FPSHUD Recorder] PulseAudio connection failed or timeout, audio capture disabled.\n");
      return;
    }

    std::fprintf(stderr, "[FPSHUD Recorder] Audio capture initialized on: %s\n",
                 monitor_source.c_str());

    constexpr int kAudioSamplesPerFrame = 1024;
    std::vector<int16_t> pcm_ring;
    pcm_ring.reserve(kAudioSamplesPerFrame * 8);
    int64_t audio_pts = 0;

    // Resampler from S16 to FLTP
    AVChannelLayout in_ch_layout;
    av_channel_layout_default(&in_ch_layout, 2);

    SwrContext* swr = nullptr;
    swr_alloc_set_opts2(&swr, &actx->ch_layout, actx->sample_fmt, actx->sample_rate, &in_ch_layout,
                        AV_SAMPLE_FMT_S16, 48000, 0, nullptr);
    if (swr) swr_init(swr);

    AVFrame* aframe = av_frame_alloc();
    aframe->nb_samples = kAudioSamplesPerFrame;
    aframe->format = actx->sample_fmt;
    av_channel_layout_copy(&aframe->ch_layout, &actx->ch_layout);
    aframe->sample_rate = actx->sample_rate;
    av_frame_get_buffer(aframe, 0);

    AVPacket* pkt = av_packet_alloc();

    while (g_running.load(std::memory_order_relaxed)) {
      pa_mainloop_iterate(pulse.ml, 0, nullptr);

      if (!PA_STREAM_IS_GOOD(pa_stream_get_state(pulse.stream))) {
        break;
      }

      const void* data = nullptr;
      size_t nbytes = 0;
      if (pa_stream_peek(pulse.stream, &data, &nbytes) == 0 && nbytes > 0) {
        if (data == nullptr) {
          // Hole in stream: inject silence (zeros)
          size_t samples_to_inject = nbytes / sizeof(int16_t);
          pcm_ring.insert(pcm_ring.end(), samples_to_inject, 0);
        } else {
          const int16_t* samples = static_cast<const int16_t*>(data);
          size_t num_samples = nbytes / sizeof(int16_t);
          pcm_ring.insert(pcm_ring.end(), samples, samples + num_samples);
        }
        pa_stream_drop(pulse.stream);
      }

      // A/V Sync Guard (I1): Discard pre-roll audio samples until the first game video frame
      // arrives so audio starts in exact lockstep with video PTS at 0.0s
      if (!g_has_received_game_frame.load(std::memory_order_relaxed)) {
        pcm_ring.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      while (pcm_ring.size() >= static_cast<size_t>(kAudioSamplesPerFrame * 2)) {
        av_frame_make_writable(aframe);
        const uint8_t* in_data[1] = {reinterpret_cast<const uint8_t*>(pcm_ring.data())};
        if (swr) {
          swr_convert(swr, aframe->data, kAudioSamplesPerFrame, in_data, kAudioSamplesPerFrame);
        }

        aframe->pts = audio_pts;
        audio_pts += kAudioSamplesPerFrame;

        if (avcodec_send_frame(actx, aframe) >= 0) {
          while (avcodec_receive_packet(actx, pkt) >= 0) {
            push_packet_or_mux(pkt, astream->index, actx->time_base, astream->time_base);
            av_packet_unref(pkt);
          }
        }

        pcm_ring.erase(pcm_ring.begin(), pcm_ring.begin() + (kAudioSamplesPerFrame * 2));
      }

      if (nbytes == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    // Flush remaining audio frames in encoder
    avcodec_send_frame(actx, nullptr);
    while (avcodec_receive_packet(actx, pkt) >= 0) {
      push_packet_or_mux(pkt, astream->index, actx->time_base, astream->time_base);
      av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    av_frame_free(&aframe);
    if (swr) swr_free(&swr);
    pulse.cleanup();
  });

  // 6. Main Socket Event Loop: Handle Frame Signals & Stop Commands
  AVPacket* v_pkt = av_packet_alloc();
  auto session_start = std::chrono::steady_clock::now();
  uint64_t session_start_us = 0;
  bool has_received_game_frame = false;
  bool has_new_frame = false;
  AVPixelFormat last_src_fmt = AV_PIX_FMT_NONE;
  int last_src_w = 0, last_src_h = 0;

  bool is_vfr = (start_cfg.vfr_mode == 1);
  int64_t total_encoded_frames = 0;
  auto frame_interval = std::chrono::microseconds(1000000 / target_fps);
  int64_t last_encoded_pts = -1;

  auto encode_frame_to_stream = [&](int64_t pts) {
    vframe->pts = pts;
    total_encoded_frames++;
    video_frame_idx.store(total_encoded_frames, std::memory_order_relaxed);
    has_new_frame = false;

    if (is_vaapi_encoder && hw_frame) {
      if (av_hwframe_get_buffer(vctx->hw_frames_ctx, hw_frame, 0) >= 0) {
        if (av_hwframe_transfer_data(hw_frame, vframe, 0) >= 0) {
          hw_frame->pts = vframe->pts;
          if (avcodec_send_frame(vctx, hw_frame) >= 0) {
            while (avcodec_receive_packet(vctx, v_pkt) >= 0) {
              push_packet_or_mux(v_pkt, vstream->index, vctx->time_base, vstream->time_base);
              av_packet_unref(v_pkt);
            }
          }
        }
        av_frame_unref(hw_frame);
      }
    } else {
      if (avcodec_send_frame(vctx, vframe) >= 0) {
        while (avcodec_receive_packet(vctx, v_pkt) >= 0) {
          push_packet_or_mux(v_pkt, vstream->index, vctx->time_base, vstream->time_base);
          av_packet_unref(v_pkt);
        }
      }
    }
  };

  while (g_running.load(std::memory_order_relaxed)) {
    struct timeval sel_tv{};
    if (has_received_game_frame) {
      if (is_vfr) {
        sel_tv.tv_sec = 0;
        sel_tv.tv_usec = 20000;  // 20ms poll in VFR mode
      } else {
        // Watchdog deadline: wake up if no frame arrives after 1.5 frame intervals from
        // last_encoded_pts
        auto deadline =
            session_start + (last_encoded_pts + 1) * frame_interval + frame_interval / 2;
        auto now = std::chrono::steady_clock::now();
        if (deadline > now) {
          auto diff = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
          sel_tv.tv_sec = static_cast<long>(diff / 1000000);
          sel_tv.tv_usec = static_cast<long>(diff % 1000000);
        } else {
          sel_tv.tv_sec = 0;
          sel_tv.tv_usec = 1000;  // 1ms
        }
      }
    } else {
      sel_tv.tv_sec = 0;
      sel_tv.tv_usec = 50000;  // 50ms wait for first frame
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(client_fd, &read_fds);

    int ret = select(client_fd + 1, &read_fds, nullptr, nullptr, &sel_tv);
    if (ret > 0 && FD_ISSET(client_fd, &read_fds)) {
      fpshud::IpcPacketHeader in_hdr{};
      ssize_t rd = recv(client_fd, &in_hdr, sizeof(in_hdr), MSG_WAITALL);
      if (rd < static_cast<ssize_t>(sizeof(in_hdr))) {
        // Client disconnected or partial stream close -> stop recording cleanly
        g_running.store(false, std::memory_order_relaxed);
        break;
      }

      if (in_hdr.is_valid()) {
        auto cmd = static_cast<fpshud::RecorderCommand>(in_hdr.command);
        if (cmd == fpshud::RecorderCommand::StopRecord) {
          g_running.store(false, std::memory_order_relaxed);
          break;
        } else if (cmd == fpshud::RecorderCommand::SendFrame) {
          fpshud::IpcFrameNotify notify{};
          size_t rem = sizeof(notify) - sizeof(fpshud::IpcPacketHeader);
          ssize_t nr =
              recv(client_fd, reinterpret_cast<char*>(&notify) + sizeof(fpshud::IpcPacketHeader),
                   rem, MSG_WAITALL);
          if (nr == static_cast<ssize_t>(rem)) {
            if (shm_base && shm_hdr &&
                (shm_hdr->width != notify.width || shm_hdr->height != notify.height)) {
              munmap(shm_base, shm_size);
              shm_base = nullptr;
              shm_hdr = nullptr;
              shm_size = 0;
            }
            try_map_shm(shm_path);

            if (shm_base && shm_hdr && notify.slot < 3) {
              size_t frame_bytes = static_cast<size_t>(notify.width) * notify.height * 4;
              size_t slot_offset = shm_hdr->offset[notify.slot];
              if (slot_offset < shm_size && frame_bytes <= (shm_size - slot_offset)) {
                const uint8_t* src = static_cast<const uint8_t*>(shm_base) + slot_offset;
                int stride = notify.width * 4;

                AVPixelFormat src_fmt = AV_PIX_FMT_BGRA;
                if (notify.format == 1) {
                  src_fmt = AV_PIX_FMT_RGBA;
                } else if (notify.format == 2) {
                  src_fmt = AV_PIX_FMT_X2BGR10LE;
                } else if (notify.format == 3) {
                  src_fmt = AV_PIX_FMT_X2RGB10LE;
                }
                if (!sws_ctx || last_src_fmt != src_fmt ||
                    last_src_w != static_cast<int>(notify.width) ||
                    last_src_h != static_cast<int>(notify.height)) {
                  if (sws_ctx) sws_freeContext(sws_ctx);
                  sws_ctx = sws_getContext(notify.width, notify.height, src_fmt, target_width,
                                           target_height, sw_pix_fmt, SWS_FAST_BILINEAR, nullptr,
                                           nullptr, nullptr);
                  last_src_fmt = src_fmt;
                  last_src_w = notify.width;
                  last_src_h = notify.height;
                }

                if (!sws_ctx) {
                  fprintf(stderr,
                          "[FPSHUD Recorder] Warning: Failed to allocate/update swscale context "
                          "(%ux%u -> %dx%d)\n",
                          notify.width, notify.height, target_width, target_height);
                  continue;
                }

                if (!has_received_game_frame) {
                  has_received_game_frame = true;
                  g_has_received_game_frame.store(true, std::memory_order_relaxed);
                  session_start = std::chrono::steady_clock::now();
                  session_start_us =
                      (notify.timestamp_us > 0)
                          ? notify.timestamp_us
                          : static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    session_start.time_since_epoch())
                                    .count());
                  last_encoded_pts = 0;

                  av_frame_make_writable(vframe);
                  if (notify.flip_y) {
                    const uint8_t* src_slice[1] = {src +
                                                   static_cast<size_t>(notify.height - 1) * stride};
                    int src_stride[1] = {-stride};
                    sws_scale(sws_ctx, src_slice, src_stride, 0, notify.height, vframe->data,
                              vframe->linesize);
                  } else {
                    const uint8_t* src_slice[1] = {src};
                    int src_stride[1] = {stride};
                    sws_scale(sws_ctx, src_slice, src_stride, 0, notify.height, vframe->data,
                              vframe->linesize);
                  }
                  encode_frame_to_stream(0);
                } else if (is_vfr) {
                  // Pure VFR Mode: 1:1 frame capture at exact presentation timestamp
                  av_frame_make_writable(vframe);
                  if (notify.flip_y) {
                    const uint8_t* src_slice[1] = {src +
                                                   static_cast<size_t>(notify.height - 1) * stride};
                    int src_stride[1] = {-stride};
                    sws_scale(sws_ctx, src_slice, src_stride, 0, notify.height, vframe->data,
                              vframe->linesize);
                  } else {
                    const uint8_t* src_slice[1] = {src};
                    int src_stride[1] = {stride};
                    sws_scale(sws_ctx, src_slice, src_stride, 0, notify.height, vframe->data,
                              vframe->linesize);
                  }

                  int64_t elapsed_us = 0;
                  if (notify.timestamp_us > 0 && notify.timestamp_us >= session_start_us) {
                    elapsed_us = static_cast<int64_t>(notify.timestamp_us - session_start_us);
                  } else {
                    auto now = std::chrono::steady_clock::now();
                    elapsed_us =
                        std::chrono::duration_cast<std::chrono::microseconds>(now - session_start)
                            .count();
                  }
                  int64_t pts =
                      (elapsed_us > last_encoded_pts) ? elapsed_us : (last_encoded_pts + 1);
                  last_encoded_pts = pts;
                  encode_frame_to_stream(pts);
                } else {
                  auto now = std::chrono::steady_clock::now();
                  int64_t elapsed_us =
                      std::chrono::duration_cast<std::chrono::microseconds>(now - session_start)
                          .count();
                  int64_t target_pts =
                      (elapsed_us + frame_interval.count() / 2) / frame_interval.count();

                  // If game fell behind real-time (e.g. 50 FPS on 60 FPS target), duplicate
                  // previous frame up to target_pts BEFORE overwriting vframe with the new frame!
                  if (target_pts > last_encoded_pts + 1) {
                    int64_t missed_frames = target_pts - (last_encoded_pts + 1);
                    if (missed_frames > 4) {
                      // Cap lag catch-up if game was paused or loading (> 4 frame intervals)
                      last_encoded_pts = target_pts - 1;
                    } else {
                      while (last_encoded_pts + 1 < target_pts &&
                             g_running.load(std::memory_order_relaxed)) {
                        last_encoded_pts++;
                        encode_frame_to_stream(last_encoded_pts);
                      }
                    }
                  }

                  // Scale newly arrived game frame into vframe
                  av_frame_make_writable(vframe);
                  if (notify.flip_y) {
                    const uint8_t* src_slice[1] = {src +
                                                   static_cast<size_t>(notify.height - 1) * stride};
                    int src_stride[1] = {-stride};
                    sws_scale(sws_ctx, src_slice, src_stride, 0, notify.height, vframe->data,
                              vframe->linesize);
                  } else {
                    const uint8_t* src_slice[1] = {src};
                    int src_stride[1] = {stride};
                    sws_scale(sws_ctx, src_slice, src_stride, 0, notify.height, vframe->data,
                              vframe->linesize);
                  }

                  // If target_pts is fresh (next slot or greater), encode the new frame!
                  // If target_pts <= last_encoded_pts (game > 60 FPS), vframe is updated
                  // with latest pixels for the next slot without emitting an extra frame.
                  if (target_pts > last_encoded_pts) {
                    last_encoded_pts = target_pts;
                    encode_frame_to_stream(last_encoded_pts);
                  }
                }
              }
            }
          }
        } else if (cmd == fpshud::RecorderCommand::SaveReplay) {
          fpshud::IpcStatusResponse save_resp{};
          save_resp.magic = fpshud::kRecorderIpcMagic;
          save_resp.status = static_cast<uint32_t>(fpshud::RecorderStatus::Recording);

          if (is_replay) {
            std::string replay_file =
                (fs::path(save_dir) / generate_timestamp_filename("Replay", ext)).string();
            std::lock_guard<std::mutex> lock(mux_mutex);

            // Find newest keyframe at or before win_start_us (or fallback to oldest keyframe)
            auto now_tp = std::chrono::steady_clock::now();
            int64_t now_us =
                std::chrono::duration_cast<std::chrono::microseconds>(now_tp - replay_clock_start)
                    .count();
            int64_t win_start_us = now_us - static_cast<int64_t>(replay_sec) * 1000000LL;
            size_t start_idx = packet_ring.size();
            for (size_t i = 0; i < packet_ring.size(); ++i) {
              if (packet_ring[i].is_keyframe) {
                if (packet_ring[i].timestamp_us <= win_start_us) {
                  start_idx = i;
                } else {
                  if (start_idx == packet_ring.size()) {
                    start_idx = i;
                  }
                  break;
                }
              }
            }

            if (start_idx < packet_ring.size()) {
              AVFormatContext* replay_oc = nullptr;
              if (avformat_alloc_output_context2(&replay_oc, nullptr, nullptr,
                                                 replay_file.c_str()) >= 0 &&
                  replay_oc) {
                AVStream* rv = avformat_new_stream(replay_oc, nullptr);
                rv->id = 0;
                rv->time_base = vctx->time_base;
                avcodec_parameters_from_context(rv->codecpar, vctx);
                if (vcodec->id == AV_CODEC_ID_HEVC && (start_cfg.container == 0 || ext == ".mp4")) {
                  rv->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
                }

                AVStream* ra = nullptr;
                if (actx && audio_enabled) {
                  ra = avformat_new_stream(replay_oc, nullptr);
                  ra->id = 1;
                  ra->time_base = actx->time_base;
                  avcodec_parameters_from_context(ra->codecpar, actx);
                }

                AVDictionary* ropts = nullptr;
                av_dict_set(&ropts, "movflags", "+faststart", 0);
                if (vcodec->id == AV_CODEC_ID_HEVC && (start_cfg.container == 0 || ext == ".mp4")) {
                  av_dict_set(&ropts, "tag:v", "hvc1", 0);
                }

                bool opened = true;
                if (!(replay_oc->oformat->flags & AVFMT_NOFILE)) {
                  opened = (avio_open(&replay_oc->pb, replay_file.c_str(), AVIO_FLAG_WRITE) >= 0);
                }
                if (opened && avformat_write_header(replay_oc, &ropts) >= 0) {
                  int64_t v_base_pts = packet_ring[start_idx].pkt->pts;
                  int64_t a_base_pts = -1;
                  for (size_t i = start_idx; i < packet_ring.size(); ++i) {
                    const auto& bp = packet_ring[i];
                    if (bp.stream_index == 1 && !ra) continue;
                    AVPacket* cp = av_packet_clone(bp.pkt);
                    if (!cp) continue;
                    if (bp.stream_index == 0) {
                      cp->pts = std::max<int64_t>(0, cp->pts - v_base_pts);
                      cp->dts = std::max<int64_t>(0, cp->dts - v_base_pts);
                      av_packet_rescale_ts(cp, vctx->time_base, rv->time_base);
                      cp->stream_index = rv->index;
                    } else if (ra) {
                      if (a_base_pts < 0) a_base_pts = cp->pts;
                      cp->pts = std::max<int64_t>(0, cp->pts - a_base_pts);
                      cp->dts = std::max<int64_t>(0, cp->dts - a_base_pts);
                      av_packet_rescale_ts(cp, actx->time_base, ra->time_base);
                      cp->stream_index = ra->index;
                    }
                    av_interleaved_write_frame(replay_oc, cp);
                    av_packet_free(&cp);
                  }
                  av_write_trailer(replay_oc);
                  std::snprintf(save_resp.last_saved_file, sizeof(save_resp.last_saved_file), "%s",
                                replay_file.c_str());
                }
                av_dict_free(&ropts);
                if (replay_oc->pb && !(replay_oc->oformat->flags & AVFMT_NOFILE)) {
                  avio_closep(&replay_oc->pb);
                }
                avformat_free_context(replay_oc);
              }
            }
          }
          send(client_fd, &save_resp, sizeof(save_resp), 0);
        } else if (cmd == fpshud::RecorderCommand::QueryStatus) {
          fpshud::IpcStatusResponse status_resp{};
          status_resp.magic = fpshud::kRecorderIpcMagic;
          status_resp.status = static_cast<uint32_t>(fpshud::RecorderStatus::Recording);
          status_resp.frame_count =
              static_cast<uint32_t>(video_frame_idx.load(std::memory_order_relaxed));
          auto now = std::chrono::steady_clock::now();
          status_resp.elapsed_sec = std::chrono::duration<float>(now - session_start).count();
          send(client_fd, &status_resp, sizeof(status_resp), 0);
        }
      }
    }

    // Watchdog fallback: if game paused, frozen, or stopped sending frames (> 1.5 intervals),
    // emit duplicate frames locked to real time to prevent video from falling behind audio (CFR
    // only)
    if (has_received_game_frame && ret == 0 && !is_vfr) {
      auto cur_time = std::chrono::steady_clock::now();
      int64_t target_pts =
          (std::chrono::duration_cast<std::chrono::microseconds>(cur_time - session_start).count() +
           frame_interval.count() / 2) /
          frame_interval.count();
      if (target_pts > last_encoded_pts + 4) {
        last_encoded_pts = target_pts - 1;
      }
      while (last_encoded_pts < target_pts && g_running.load(std::memory_order_relaxed)) {
        last_encoded_pts++;
        encode_frame_to_stream(last_encoded_pts);
      }
    }
  }

  if (shm_base && shm_size > 0) {
    munmap(shm_base, shm_size);
    shm_base = nullptr;
  }

  // 7. Finalize & Flush Streams cleanly
  if (audio_thread.joinable()) {
    audio_thread.join();
  }

  // Flush video encoder
  if (!is_replay) {
    avcodec_send_frame(vctx, nullptr);
    while (avcodec_receive_packet(vctx, v_pkt) >= 0) {
      push_packet_or_mux(v_pkt, vstream->index, vctx->time_base, vstream->time_base);
      av_packet_unref(v_pkt);
    }
    av_write_trailer(oc);
  }
  av_packet_free(&v_pkt);

  {
    std::lock_guard<std::mutex> lock(mux_mutex);
    for (auto& bp : packet_ring) {
      if (bp.pkt) av_packet_free(&bp.pkt);
    }
    packet_ring.clear();
  }

  // Send final saved file path response back to client socket
  fpshud::IpcStatusResponse final_resp{};
  final_resp.magic = fpshud::kRecorderIpcMagic;
  final_resp.status = static_cast<uint32_t>(fpshud::RecorderStatus::Idle);
  final_resp.frame_count = static_cast<uint32_t>(video_frame_idx.load(std::memory_order_relaxed));
  if (!is_replay) {
    std::snprintf(final_resp.last_saved_file, sizeof(final_resp.last_saved_file), "%s",
                  out_filename.c_str());
  }
  send(client_fd, &final_resp, sizeof(final_resp), 0);

  // Cleanup FFmpeg resources
  if (!is_replay && !(oc->oformat->flags & AVFMT_NOFILE) && oc->pb) {
    avio_closep(&oc->pb);
  }
  avcodec_free_context(&vctx);
  if (actx) avcodec_free_context(&actx);
  if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
  avformat_free_context(oc);
  av_frame_free(&vframe);
  if (hw_frame) av_frame_free(&hw_frame);
  if (sws_ctx) sws_freeContext(sws_ctx);

  close(client_fd);
  close(server_fd);
  unlink(socket_path.c_str());

  std::fprintf(stderr, "[FPSHUD Recorder] Successfully finalized video (%ld frames): %s\n",
               video_frame_idx.load(std::memory_order_relaxed), out_filename.c_str());
  return 0;
}
