#include "recorder_player.hpp"
#include "recorder_playback_protocol.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(FPSHUD_RECORDER_DAEMON)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <pulse/error.h>
#include <pulse/pulseaudio.h>

namespace {

class PulseAudioPlayback {
public:
  PulseAudioPlayback() = default;
  ~PulseAudioPlayback() { cleanup(); }
  PulseAudioPlayback(const PulseAudioPlayback&) = delete;
  PulseAudioPlayback& operator=(const PulseAudioPlayback&) = delete;

  bool init(uint32_t sample_rate = 48000, uint8_t channels = 2) {
    cleanup();
    ml_ = pa_mainloop_new();
    if (!ml_) return false;
    pa_mainloop_api* api = pa_mainloop_get_api(ml_);
    ctx_ = pa_context_new(api, "FPSHUD Video Player");
    if (!ctx_) {
      cleanup();
      return false;
    }

    if (pa_context_connect(ctx_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      cleanup();
      return false;
    }

    auto start = std::chrono::steady_clock::now();
    while (true) {
      pa_context_state_t state = pa_context_get_state(ctx_);
      if (state == PA_CONTEXT_READY) break;
      if (!PA_CONTEXT_IS_GOOD(state)) {
        cleanup();
        return false;
      }
      pa_mainloop_iterate(ml_, 0, nullptr);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                start)
              .count() > 600) {
        cleanup();
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    sample_rate_ = sample_rate;
    channels_ = channels;

    pa_sample_spec ss{};
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = sample_rate;
    ss.channels = channels;

    stream_ = pa_stream_new(ctx_, "Video Audio Playback", &ss, nullptr);
    if (!stream_) {
      cleanup();
      return false;
    }

    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = static_cast<uint32_t>(pa_usec_to_bytes(250000, &ss));  // 250ms buffer
    attr.prebuf = static_cast<uint32_t>(pa_usec_to_bytes(100000, &ss));   // 100ms prebuffer
    attr.minreq = static_cast<uint32_t>(pa_usec_to_bytes(20000, &ss));    // 20ms chunks
    attr.fragsize = static_cast<uint32_t>(-1);

    pa_stream_flags_t flags = static_cast<pa_stream_flags_t>(
        PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING);

    if (pa_stream_connect_playback(stream_, nullptr, &attr, flags, nullptr, nullptr) < 0) {
      cleanup();
      return false;
    }

    start = std::chrono::steady_clock::now();
    while (true) {
      pa_stream_state_t state = pa_stream_get_state(stream_);
      if (state == PA_STREAM_READY) break;
      if (!PA_STREAM_IS_GOOD(state)) {
        cleanup();
        return false;
      }
      pa_mainloop_iterate(ml_, 0, nullptr);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                start)
              .count() > 600) {
        cleanup();
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    is_ready_ = true;
    return true;
  }

  [[nodiscard]] bool is_ready() const noexcept { return is_ready_ && stream_ != nullptr; }

  void iterate() {
    if (ml_) {
      pa_mainloop_iterate(ml_, 0, nullptr);
    }
  }

  size_t writable_bytes() {
    if (!is_ready()) return 0;
    iterate();
    if (pa_stream_get_state(stream_) != PA_STREAM_READY) return 0;
    size_t w = pa_stream_writable_size(stream_);
    return (w == static_cast<size_t>(-1)) ? 0 : w;
  }

  bool write(const void* data, size_t bytes) {
    if (!is_ready() || !data || bytes == 0) return false;
    iterate();
    if (pa_stream_get_state(stream_) != PA_STREAM_READY) return false;
    int res = pa_stream_write(stream_, data, bytes, nullptr, 0, PA_SEEK_RELATIVE);
    iterate();
    return res == 0;
  }

  void cork(bool pause) {
    if (!is_ready()) return;
    pa_operation* op = pa_stream_cork(stream_, pause ? 1 : 0, nullptr, nullptr);
    if (op) {
      auto start = std::chrono::steady_clock::now();
      while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_mainloop_iterate(ml_, 0, nullptr);
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  start)
                .count() > 100) {
          break;
        }
      }
      pa_operation_unref(op);
    }
  }

  void flush() {
    if (!is_ready()) return;
    pa_operation* op = pa_stream_flush(stream_, nullptr, nullptr);
    if (op) {
      auto start = std::chrono::steady_clock::now();
      while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_mainloop_iterate(ml_, 0, nullptr);
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  start)
                .count() > 100) {
          break;
        }
      }
      pa_operation_unref(op);
    }
  }

  void cleanup() {
    is_ready_ = false;
    if (stream_) {
      pa_stream_disconnect(stream_);
      pa_stream_unref(stream_);
      stream_ = nullptr;
    }
    if (ctx_) {
      pa_context_disconnect(ctx_);
      pa_context_unref(ctx_);
      ctx_ = nullptr;
    }
    if (ml_) {
      pa_mainloop_free(ml_);
      ml_ = nullptr;
    }
  }

private:
  pa_mainloop* ml_{nullptr};
  pa_context* ctx_{nullptr};
  pa_stream* stream_{nullptr};
  uint32_t sample_rate_{48000};
  uint8_t channels_{2};
  bool is_ready_{false};
};

void drain_audio(PulseAudioPlayback& pulse, std::vector<uint8_t>& pending,
                 size_t& audio_read_offset) {
  if (!pulse.is_ready() || pending.empty() || audio_read_offset >= pending.size()) {
    if (audio_read_offset >= pending.size() && !pending.empty()) {
      pending.clear();
      audio_read_offset = 0;
    }
    pulse.iterate();
    return;
  }
  pulse.iterate();
  size_t writable = pulse.writable_bytes();
  if (writable > 0) {
    size_t available = pending.size() - audio_read_offset;
    size_t to_write = std::min(writable, available);
    if (to_write > 0) {
      if (pulse.write(pending.data() + audio_read_offset, to_write)) {
        audio_read_offset += to_write;
        if (audio_read_offset >= pending.size()) {
          pending.clear();
          audio_read_offset = 0;
        } else if (audio_read_offset >= 4096) {
          pending.erase(pending.begin(), pending.begin() + audio_read_offset);
          audio_read_offset = 0;
        }
      }
    }
  }
}

}  // namespace
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
  auto cleanup_shm_file = [&]() {
    if (!shm_name.empty()) {
      if (shm_name.find('/') != std::string::npos) {
        ::unlink(shm_name.c_str());
      } else {
        ::shm_unlink(shm_name.c_str());
      }
    }
  };

  std::fprintf(stdout, "[FPSHUD Player] Starting playback engine for: %s\n", video_file.c_str());

  // 1. Open video container
  AVFormatContext* fmt_ctx = nullptr;
  int open_res = avformat_open_input(&fmt_ctx, video_file.c_str(), nullptr, nullptr);
  if (open_res < 0) {
    char errbuf[256] = {0};
    av_strerror(open_res, errbuf, sizeof(errbuf));
    std::fprintf(stderr, "[FPSHUD Player] ERROR: Could not open input file: %s (err %d: %s)\n",
                 video_file.c_str(), open_res, errbuf);
    ::unlink(socket_path.c_str());
    cleanup_shm_file();
    return 1;
  }

  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: Failed to retrieve stream info.\n");
    avformat_close_input(&fmt_ctx);
    ::unlink(socket_path.c_str());
    cleanup_shm_file();
    return 1;
  }

  int video_stream_idx = -1;
  int audio_stream_idx = -1;
  for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
    if (video_stream_idx < 0 && fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_idx = static_cast<int>(i);
    } else if (audio_stream_idx < 0 &&
               fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_stream_idx = static_cast<int>(i);
    }
  }

  if (video_stream_idx < 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: No video stream found in container.\n");
    avformat_close_input(&fmt_ctx);
    ::unlink(socket_path.c_str());
    cleanup_shm_file();
    return 1;
  }

  AVStream* vid_stream = fmt_ctx->streams[video_stream_idx];
  AVCodecParameters* codec_par = vid_stream->codecpar;
  const AVCodec* decoder = avcodec_find_decoder(codec_par->codec_id);
  if (!decoder) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: Unsupported codec ID: %d\n", codec_par->codec_id);
    avformat_close_input(&fmt_ctx);
    ::unlink(socket_path.c_str());
    cleanup_shm_file();
    return 1;
  }

  AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
  if (!codec_ctx || avcodec_parameters_to_context(codec_ctx, codec_par) < 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: Failed to initialize video decoder context.\n");
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    ::unlink(socket_path.c_str());
    cleanup_shm_file();
    return 1;
  }
  codec_ctx->thread_count = 0;  // Auto multi-threaded decode across all CPU cores
  codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: Failed to open video decoder.\n");
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    ::unlink(socket_path.c_str());
    cleanup_shm_file();
    return 1;
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

  // Audio Decoder & PulseAudio initialization (optional, only if audio stream exists)
  AVCodecContext* aud_codec_ctx = nullptr;
  SwrContext* swr_audio = nullptr;
  AVChannelLayout out_ch_layout{};
  PulseAudioPlayback pulse_out;
  std::vector<uint8_t> audio_pending;
  size_t audio_read_offset = 0;

  if (audio_stream_idx >= 0) {
    AVStream* aud_stream = fmt_ctx->streams[audio_stream_idx];
    AVCodecParameters* aud_par = aud_stream->codecpar;
    const AVCodec* aud_decoder = avcodec_find_decoder(aud_par->codec_id);
    if (aud_decoder) {
      aud_codec_ctx = avcodec_alloc_context3(aud_decoder);
      if (aud_codec_ctx && avcodec_parameters_to_context(aud_codec_ctx, aud_par) >= 0 &&
          avcodec_open2(aud_codec_ctx, aud_decoder, nullptr) >= 0) {
        if (aud_codec_ctx->ch_layout.nb_channels <= 0) {
          av_channel_layout_default(&aud_codec_ctx->ch_layout, 2);
        }
        av_channel_layout_default(&out_ch_layout, 2);
        int in_rate = aud_codec_ctx->sample_rate > 0 ? aud_codec_ctx->sample_rate : 48000;
        AVSampleFormat in_fmt = aud_codec_ctx->sample_fmt != AV_SAMPLE_FMT_NONE
                                    ? aud_codec_ctx->sample_fmt
                                    : AV_SAMPLE_FMT_FLTP;

        swr_alloc_set_opts2(&swr_audio, &out_ch_layout, AV_SAMPLE_FMT_S16, 48000,
                            &aud_codec_ctx->ch_layout, in_fmt, in_rate, 0, nullptr);
        if (swr_audio) {
          swr_init(swr_audio);
        }
        if (pulse_out.init(48000, 2)) {
          std::fprintf(stdout, "[FPSHUD Player] Audio engine active: PulseAudio 48kHz Stereo\n");
        }
      } else if (aud_codec_ctx) {
        avcodec_free_context(&aud_codec_ctx);
        aud_codec_ctx = nullptr;
      }
    }
  }

  // 2. Setup POSIX Shared Memory (Double-buffered: 2 slots)
  size_t pixel_bytes = static_cast<size_t>(width) * height * 4;
  size_t shm_total_size = sizeof(PlaybackShmHeader) + 2 * pixel_bytes;

  int shm_fd = -1;
  if (shm_name.find('/') != std::string::npos) {
    shm_fd = ::open(shm_name.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
  } else {
    shm_fd = ::shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0600);
  }

  if (shm_fd < 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: shm_open failed for %s\n", shm_name.c_str());
    if (swr_audio) swr_free(&swr_audio);
    av_channel_layout_uninit(&out_ch_layout);
    if (aud_codec_ctx) avcodec_free_context(&aud_codec_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    ::unlink(socket_path.c_str());
    cleanup_shm_file();
    return 1;
  }

  if (ftruncate(shm_fd, static_cast<off_t>(shm_total_size)) != 0) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: ftruncate failed.\n");
    close(shm_fd);
    cleanup_shm_file();
    if (swr_audio) swr_free(&swr_audio);
    av_channel_layout_uninit(&out_ch_layout);
    if (aud_codec_ctx) avcodec_free_context(&aud_codec_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    ::unlink(socket_path.c_str());
    return 1;
  }

  void* map_ptr = mmap(nullptr, shm_total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  if (map_ptr == MAP_FAILED) {
    std::fprintf(stderr, "[FPSHUD Player] ERROR: mmap failed.\n");
    close(shm_fd);
    cleanup_shm_file();
    if (swr_audio) swr_free(&swr_audio);
    av_channel_layout_uninit(&out_ch_layout);
    if (aud_codec_ctx) avcodec_free_context(&aud_codec_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    ::unlink(socket_path.c_str());
    return 1;
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
  std::memset(pixel_dest, 0, 2 * pixel_bytes);

  // 3. Setup Unix Domain Socket for Commands
  int server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_sock < 0) {
    munmap(map_ptr, shm_total_size);
    close(shm_fd);
    cleanup_shm_file();
    if (swr_audio) swr_free(&swr_audio);
    av_channel_layout_uninit(&out_ch_layout);
    if (aud_codec_ctx) avcodec_free_context(&aud_codec_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    ::unlink(socket_path.c_str());
    return 1;
  }

  sockaddr_un saddr{};
  saddr.sun_family = AF_UNIX;
  std::strncpy(saddr.sun_path, socket_path.c_str(), sizeof(saddr.sun_path) - 1);
  unlink(socket_path.c_str());

  if (bind(server_sock, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) < 0) {
    close(server_sock);
    munmap(map_ptr, shm_total_size);
    close(shm_fd);
    cleanup_shm_file();
    if (swr_audio) swr_free(&swr_audio);
    av_channel_layout_uninit(&out_ch_layout);
    if (aud_codec_ctx) avcodec_free_context(&aud_codec_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    ::unlink(socket_path.c_str());
    return 1;
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
    cleanup_shm_file();
    if (swr_audio) swr_free(&swr_audio);
    av_channel_layout_uninit(&out_ch_layout);
    if (aud_codec_ctx) avcodec_free_context(&aud_codec_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return 1;
  }

  AVFrame* raw_frame = av_frame_alloc();
  AVFrame* raw_audio_frame = av_frame_alloc();

  uint8_t* rgba_buffers[4] = {pixel_dest, nullptr, nullptr, nullptr};
  int rgba_linesizes[4] = {static_cast<int>(width * 4), 0, 0, 0};

  AVRational avg_fps = av_guess_frame_rate(fmt_ctx, vid_stream, nullptr);
  double frame_interval =
      (avg_fps.num > 0 && avg_fps.den > 0) ? (1.0 / av_q2d(avg_fps)) : (1.0 / 60.0);

  std::fprintf(
      stdout, "[FPSHUD Player] Ready: %ux%u @ %.2f fps (duration: %.1f s). Listening on %s\n",
      width, height, (avg_fps.num > 0 ? av_q2d(avg_fps) : 60.0), duration_sec, socket_path.c_str());

  int client_sock = -1;
  std::atomic<bool> running{true};
  std::atomic<bool> is_paused{false};
  std::atomic<bool> seek_pending{true};  // Prime initial video frame & audio immediately
  std::atomic<bool> flush_video_codec{false};
  std::atomic<float> seek_request{-1.0f};
  std::atomic<bool> eof_reached{false};

  std::deque<AVPacket*> video_packet_queue;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;

  AVRational vid_tb = vid_stream->time_base;
  float vid_time_base = static_cast<float>(av_q2d(vid_tb));

  // Background Demuxer and PulseAudio Playback Thread
  std::thread demux_thread([&]() {
    AVPacket* pkt = av_packet_alloc();
    while (running.load(std::memory_order_relaxed)) {
      // Handle pending seek requests
      float s_req = seek_request.exchange(-1.0f, std::memory_order_relaxed);
      if (s_req >= 0.0f) {
        int64_t target_ts = static_cast<int64_t>(s_req / vid_time_base);
        av_seek_frame(fmt_ctx, video_stream_idx, target_ts, AVSEEK_FLAG_BACKWARD);
        if (aud_codec_ctx) {
          avcodec_flush_buffers(aud_codec_ctx);
        }
        audio_pending.clear();
        audio_read_offset = 0;
        pulse_out.flush();
        {
          std::lock_guard<std::mutex> lock(queue_mutex);
          for (AVPacket* p : video_packet_queue) {
            av_packet_free(&p);
          }
          video_packet_queue.clear();
        }
        header->current_time_sec.store(s_req, std::memory_order_relaxed);
        header->is_ended.store(false, std::memory_order_relaxed);
        eof_reached.store(false, std::memory_order_relaxed);
        flush_video_codec.store(true, std::memory_order_relaxed);
        seek_pending.store(true, std::memory_order_relaxed);
        queue_cv.notify_one();
      }

      if (is_paused.load(std::memory_order_relaxed)) {
        pulse_out.iterate();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      // Continuously feed audio to PulseAudio
      drain_audio(pulse_out, audio_pending, audio_read_offset);

      // Keep reading packets into queue if not full
      bool should_read = false;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (video_packet_queue.size() < 40 && !eof_reached.load(std::memory_order_relaxed)) {
          should_read = true;
        } else {
          queue_cv.wait_for(lock, std::chrono::milliseconds(4));
        }
      }

      if (should_read) {
        int res = av_read_frame(fmt_ctx, pkt);
        if (res < 0) {
          eof_reached.store(true, std::memory_order_relaxed);
        } else if (audio_stream_idx >= 0 && pkt->stream_index == audio_stream_idx &&
                   aud_codec_ctx && swr_audio) {
          if (avcodec_send_packet(aud_codec_ctx, pkt) == 0) {
            while (avcodec_receive_frame(aud_codec_ctx, raw_audio_frame) == 0) {
              int in_rate = aud_codec_ctx->sample_rate > 0 ? aud_codec_ctx->sample_rate : 48000;
              int max_out =
                  av_rescale_rnd(swr_get_delay(swr_audio, in_rate) + raw_audio_frame->nb_samples,
                                 48000, in_rate, AV_ROUND_UP);
              if (max_out < 256) max_out = 256;

              std::vector<int16_t> pcm_out(max_out * 2);
              uint8_t* out_ptrs[1] = {reinterpret_cast<uint8_t*>(pcm_out.data())};
              int converted = swr_convert(swr_audio, out_ptrs, max_out,
                                          const_cast<const uint8_t**>(raw_audio_frame->data),
                                          raw_audio_frame->nb_samples);
              if (converted > 0) {
                if (audio_read_offset >= 4096) {
                  audio_pending.erase(audio_pending.begin(),
                                      audio_pending.begin() + audio_read_offset);
                  audio_read_offset = 0;
                }
                size_t sample_count = static_cast<size_t>(converted) * 2;
                const uint8_t* bytes_ptr = reinterpret_cast<const uint8_t*>(pcm_out.data());
                audio_pending.insert(audio_pending.end(), bytes_ptr,
                                     bytes_ptr + (sample_count * sizeof(int16_t)));
              }
            }
          }
          av_packet_unref(pkt);
          drain_audio(pulse_out, audio_pending, audio_read_offset);
        } else if (pkt->stream_index == video_stream_idx) {
          AVPacket* vp = av_packet_clone(pkt);
          av_packet_unref(pkt);
          {
            std::lock_guard<std::mutex> lock(queue_mutex);
            video_packet_queue.push_back(vp);
          }
          queue_cv.notify_one();
        } else {
          av_packet_unref(pkt);
        }
      }
    }
    av_packet_free(&pkt);
  });

  auto clock_anchor_time = std::chrono::steady_clock::now();
  float clock_anchor_pts = 0.0f;

  while (running.load(std::memory_order_relaxed)) {
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
            is_paused.store(false, std::memory_order_relaxed);
            header->is_playing.store(true, std::memory_order_relaxed);
            clock_anchor_pts = header->current_time_sec.load(std::memory_order_relaxed);
            clock_anchor_time = std::chrono::steady_clock::now();
            pulse_out.cork(false);
          } else if (req.action == IpcPlaybackAction::Pause) {
            is_paused.store(true, std::memory_order_relaxed);
            header->is_playing.store(false, std::memory_order_relaxed);
            pulse_out.cork(true);
          } else if (req.action == IpcPlaybackAction::Seek) {
            float target_sec = std::max(0.0f, std::min(req.value, header->duration_sec));
            seek_request.store(target_sec, std::memory_order_relaxed);
            clock_anchor_pts = target_sec;
            clock_anchor_time = std::chrono::steady_clock::now();
          } else if (req.action == IpcPlaybackAction::Close) {
            running.store(false, std::memory_order_relaxed);
            break;
          }
        }
      }
    }

    if (!running.load(std::memory_order_relaxed)) break;

    // Check if video codec buffers need flushing after seek
    if (flush_video_codec.exchange(false, std::memory_order_relaxed)) {
      avcodec_flush_buffers(codec_ctx);
    }

    // 6. Decode and display next video frame if playing or priming initial seek frame
    bool playing = header->is_playing.load(std::memory_order_relaxed) &&
                   !is_paused.load(std::memory_order_relaxed);
    bool priming_seek = seek_pending.load(std::memory_order_relaxed);

    if (playing || priming_seek) {
      AVPacket* vp = nullptr;
      bool has_more_in_queue = false;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (!video_packet_queue.empty()) {
          vp = video_packet_queue.front();
          video_packet_queue.pop_front();
          has_more_in_queue = !video_packet_queue.empty();
          queue_cv.notify_one();
        } else if (priming_seek) {
          queue_cv.wait_for(lock, std::chrono::milliseconds(5));
          if (!video_packet_queue.empty()) {
            vp = video_packet_queue.front();
            video_packet_queue.pop_front();
            has_more_in_queue = !video_packet_queue.empty();
            queue_cv.notify_one();
          }
        }
      }

      if (vp) {
        if (avcodec_send_packet(codec_ctx, vp) == 0) {
          while (avcodec_receive_frame(codec_ctx, raw_frame) == 0) {
            float frame_pts = (raw_frame->pts != AV_NOPTS_VALUE)
                                  ? static_cast<float>(raw_frame->pts) * vid_time_base
                                  : header->current_time_sec.load(std::memory_order_relaxed) +
                                        static_cast<float>(frame_interval);

            if (seek_pending.load(std::memory_order_relaxed)) {
              // Immediately render initial frame or seek preview
              uint32_t next_slot = (header->write_index.load(std::memory_order_relaxed) + 1u) & 1u;
              uint8_t* dst = pixel_dest + next_slot * pixel_bytes;
              rgba_buffers[0] = dst;
              sws_scale(sws, raw_frame->data, raw_frame->linesize, 0, codec_ctx->height,
                        rgba_buffers, rgba_linesizes);
              header->current_time_sec.store(frame_pts, std::memory_order_relaxed);
              header->write_index.store(next_slot, std::memory_order_release);
              header->frame_seq.fetch_add(1, std::memory_order_release);
              clock_anchor_pts = frame_pts;
              clock_anchor_time = std::chrono::steady_clock::now();
              seek_pending.store(false, std::memory_order_relaxed);
            } else if (playing) {
              auto now = std::chrono::steady_clock::now();
              float cur_clock =
                  clock_anchor_pts + std::chrono::duration<float>(now - clock_anchor_time).count();

              // If frame is early, wait until presentation time
              if (frame_pts > cur_clock + 0.003f) {
                float wait_sec = std::min(0.04f, frame_pts - cur_clock);
                std::this_thread::sleep_for(
                    std::chrono::microseconds(static_cast<int64_t>(wait_sec * 1000000.0f)));
                now = std::chrono::steady_clock::now();
                cur_clock = clock_anchor_pts +
                            std::chrono::duration<float>(now - clock_anchor_time).count();
              }

              // Resynchronize clock if video fell drastically behind (> 250ms hitch)
              if (cur_clock > frame_pts + 0.250f) {
                clock_anchor_pts = frame_pts;
                clock_anchor_time = now;
                cur_clock = frame_pts;
              }

              // Skip sws_scale only if moderately behind AND fresher frames are queued
              bool is_late = (cur_clock > frame_pts + 0.050f);
              if (is_late && has_more_in_queue) {
                // Drop obsolete frame to let decoder rapidly catch up
              } else {
                uint32_t next_slot =
                    (header->write_index.load(std::memory_order_relaxed) + 1u) & 1u;
                uint8_t* dst = pixel_dest + next_slot * pixel_bytes;
                rgba_buffers[0] = dst;
                sws_scale(sws, raw_frame->data, raw_frame->linesize, 0, codec_ctx->height,
                          rgba_buffers, rgba_linesizes);
                header->current_time_sec.store(frame_pts, std::memory_order_relaxed);
                header->write_index.store(next_slot, std::memory_order_release);
                header->frame_seq.fetch_add(1, std::memory_order_release);
              }
            }
          }
        }
        av_packet_free(&vp);
      } else {
        if (eof_reached.load(std::memory_order_relaxed) && !priming_seek) {
          header->is_ended.store(true, std::memory_order_relaxed);
          header->is_playing.store(false, std::memory_order_relaxed);
          is_paused.store(true, std::memory_order_relaxed);
          pulse_out.cork(true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  // 7. Cleanup
  running.store(false, std::memory_order_relaxed);
  queue_cv.notify_all();
  if (demux_thread.joinable()) {
    demux_thread.join();
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    for (AVPacket* p : video_packet_queue) {
      av_packet_free(&p);
    }
    video_packet_queue.clear();
  }

  if (client_sock >= 0) close(client_sock);
  close(server_sock);
  unlink(socket_path.c_str());

  pulse_out.cleanup();
  audio_pending.clear();
  if (swr_audio) {
    swr_free(&swr_audio);
  }
  av_channel_layout_uninit(&out_ch_layout);
  if (aud_codec_ctx) {
    avcodec_free_context(&aud_codec_ctx);
  }

  av_frame_free(&raw_frame);
  av_frame_free(&raw_audio_frame);
  sws_freeContext(sws);
  avcodec_free_context(&codec_ctx);
  avformat_close_input(&fmt_ctx);

  munmap(map_ptr, shm_total_size);
  close(shm_fd);
  cleanup_shm_file();

  std::fprintf(stdout, "[FPSHUD Player] Playback worker exited cleanly.\n");
  return 0;
#endif
}

}  // namespace fpshud
