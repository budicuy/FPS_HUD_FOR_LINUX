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
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// PulseAudio Simple API for lightweight desktop/game audio recording
#include <pulse/error.h>
#include <pulse/simple.h>

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_running{true};

void sigint_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

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

} // namespace

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
    int quality_mode = 0; // 0 = CQP, 1 = Fixed VBR
    int target_cq = 22;   // 16 - 30
    std::string save_dir;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) socket_path = argv[++i];
        else if (arg == "--shm" && i + 1 < argc) shm_path = argv[++i];
        else if (arg == "--width" && i + 1 < argc) target_width = std::max(640, std::atoi(argv[++i]));
        else if (arg == "--height" && i + 1 < argc) target_height = std::max(360, std::atoi(argv[++i]));
        else if (arg == "--fps" && i + 1 < argc) target_fps = std::clamp(std::atoi(argv[++i]), 15, 240);
        else if (arg == "--bitrate" && i + 1 < argc) target_bitrate_kbps = std::clamp(std::atoi(argv[++i]), 1000, 100000);
        else if (arg == "--codec" && i + 1 < argc) codec_choice = argv[++i];
        else if (arg == "--cqp-mode" && i + 1 < argc) quality_mode = std::clamp(std::atoi(argv[++i]), 0, 1);
        else if (arg == "--cqp" && i + 1 < argc) target_cq = std::clamp(std::atoi(argv[++i]), 16, 30);
        else if (arg == "--dir" && i + 1 < argc) save_dir = argv[++i];
    }

    if (socket_path.empty()) {
        std::fprintf(stderr, "Usage: fpshud_recorder --socket <path> [--shm <path>] [--width W] [--height H] [--fps F] [--bitrate B] [--dir D]\n");
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
        if (start_cfg.width > 0) target_width = start_cfg.width;
        if (start_cfg.height > 0) target_height = start_cfg.height;
        if (start_cfg.fps > 0) target_fps = start_cfg.fps;
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

    // Ensure width and height are even for YUV420p video encoding
    target_width &= ~1;
    target_height &= ~1;

    std::string ext = (start_cfg.container == 1) ? ".mkv" : ((start_cfg.container == 2) ? ".webm" : ".mp4");
    std::string out_filename = (fs::path(save_dir) / generate_timestamp_filename("FPSHUD", ext)).string();

    std::fprintf(stderr, "[FPSHUD Recorder] Starting session: %dx%d @ %d FPS [%s, %s] -> %s\n",
                 target_width, target_height, target_fps, codec_choice.c_str(),
                 (quality_mode == 0) ? ("CQP " + std::to_string(target_cq)).c_str() : (std::to_string(target_bitrate_kbps) + "k VBR").c_str(),
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
    // HEVC:  {"hevc_nvenc", "libx265", "hevc", "h264_nvenc", "libx264", "h264"}
    // H.264: {"h264_nvenc", "libx264", "h264"}
    std::vector<const char*> candidates;
    if (codec_choice == "hevc") {
        candidates = {"hevc_nvenc", "libx265", "hevc", "h264_nvenc", "libx264", "h264"};
    } else {
        candidates = {"h264_nvenc", "libx264", "h264"};
    }

    const AVCodec* vcodec = nullptr;
    AVCodecContext* vctx = nullptr;

    for (const char* cand : candidates) {
        const AVCodec* c = avcodec_find_encoder_by_name(cand);
        if (!c) {
            if (std::strcmp(cand, "hevc") == 0) c = avcodec_find_encoder(AV_CODEC_ID_HEVC);
            else if (std::strcmp(cand, "h264") == 0) c = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
        if (!c) continue;

        AVCodecContext* ctx = avcodec_alloc_context3(c);
        if (!ctx) continue;

        bool is_nv = (std::strstr(c->name, "nvenc") != nullptr);
        ctx->codec_id = c->id;
        ctx->width = target_width;
        ctx->height = target_height;
        ctx->time_base = AVRational{1, target_fps};
        ctx->framerate = AVRational{target_fps, 1};
        ctx->pix_fmt = is_nv ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        ctx->gop_size = target_fps * 2; // 2-second keyframe interval
        ctx->max_b_frames = 0;          // Low latency for gameplay capture

        if (quality_mode == 0) {
            // CQP Mode (Adaptive Constant Quantization Parameter)
            if (is_nv) {
                av_opt_set(ctx->priv_data, "preset", "p4", 0); // p4 = balanced quality/speed
                av_opt_set(ctx->priv_data, "tune", "ll", 0);   // Low latency
                av_opt_set(ctx->priv_data, "rc", "constqp", 0);
                av_opt_set_int(ctx->priv_data, "qp", target_cq, 0);
                av_opt_set(ctx->priv_data, "spatial-aq", "1", 0);  // Preserves HUD text and sharp UI fonts
                av_opt_set(ctx->priv_data, "temporal-aq", "1", 0); // Bitrate savings on static/dialogue scenes
            } else {
                av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
                av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
                av_opt_set_int(ctx->priv_data, "crf", target_cq, 0);
            }
        } else {
            // VBR Mode (Target Bitrate)
            ctx->bit_rate = static_cast<int64_t>(target_bitrate_kbps) * 1000;
            if (is_nv) {
                av_opt_set(ctx->priv_data, "preset", "p4", 0); // p4 = balanced quality/speed
                av_opt_set(ctx->priv_data, "tune", "ll", 0);   // Low latency
                av_opt_set(ctx->priv_data, "rc", "vbr", 0);
            } else {
                av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
                av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
            }
        }

        if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
            ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

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
                    std::fprintf(stderr, "[FPSHUD Recorder] NVENC Adaptive CQP: CQ=%d, Spatial-AQ=ON, Temporal-AQ=ON\n", target_cq);
                } else {
                    std::fprintf(stderr, "[FPSHUD Recorder] Software CRF: %d\n", target_cq);
                }
            } else {
                if (is_nv) {
                    std::fprintf(stderr, "[FPSHUD Recorder] NVENC VBR: %d kbps\n", target_bitrate_kbps);
                } else {
                    std::fprintf(stderr, "[FPSHUD Recorder] Software VBR: %d kbps\n", target_bitrate_kbps);
                }
            }
            break;
        } else {
            std::fprintf(stderr, "[FPSHUD Recorder] Candidate encoder '%s' failed to open, falling back...\n", cand);
            avcodec_free_context(&ctx);
        }
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
    pa_simple* pa_record = nullptr;
    bool audio_enabled = (start_cfg.audio_source != 3); // 3 = Mute

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

            if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
                actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            if (avcodec_open2(actx, acodec, nullptr) >= 0) {
                avcodec_parameters_from_context(astream->codecpar, actx);

                // Open PulseAudio / PipeWire capture stream
                pa_sample_spec ss{};
                ss.format = PA_SAMPLE_S16LE;
                ss.rate = 48000;
                ss.channels = 2;

                std::string monitor_source = get_default_monitor_source();
                int pa_err = 0;
                pa_record = pa_simple_new(nullptr, "FPSHUD_Recorder", PA_STREAM_RECORD,
                                          monitor_source.c_str(), "Gameplay Audio", &ss,
                                          nullptr, nullptr, &pa_err);
                if (pa_record) {
                    std::fprintf(stderr, "[FPSHUD Recorder] Audio capture initialized on: %s\n", monitor_source.c_str());
                } else {
                    std::fprintf(stderr, "[FPSHUD Recorder] pa_simple_new failed (%s), audio disabled.\n", pa_strerror(pa_err));
                }
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
        if (pa_record) pa_simple_free(pa_record);
        if (vctx) avcodec_free_context(&vctx);
        if (actx) avcodec_free_context(&actx);
        if (oc) {
            if (oc->pb && !(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
        if (client_fd >= 0) close(client_fd);
        if (server_fd >= 0) close(server_fd);
        if (!socket_path.empty()) unlink(socket_path.c_str());
    };

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&oc->pb, out_filename.c_str(), AVIO_FLAG_WRITE) < 0) {
            std::fprintf(stderr, "[FPSHUD Recorder] Could not open output file: %s\n", out_filename.c_str());
            cleanup_early_failure();
            return 1;
        }
    }

    if (avformat_write_header(oc, &opt_dict) < 0) {
        std::fprintf(stderr, "[FPSHUD Recorder] Failed to write format header.\n");
        cleanup_early_failure();
        return 1;
    }
    av_dict_free(&opt_dict);

    // 5. Worker Loop for Video & Audio Multiplexing
    std::mutex mux_mutex;
    std::atomic<int64_t> video_frame_idx{0};

    // Allocate staging video frame (NV12 or YUV420P)
    AVFrame* vframe = av_frame_alloc();
    vframe->format = vctx->pix_fmt;
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

    // SwsContext for RGB/NV12 conversion if needed
    SwsContext* sws_ctx = sws_getContext(target_width, target_height, AV_PIX_FMT_RGBA,
                                         target_width, target_height, vctx->pix_fmt,
                                         SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

    // Spawn Audio Capture Thread
    std::thread audio_thread([&]() {
        if (!pa_record || !actx) return;

        constexpr int kAudioSamplesPerFrame = 1024;
        std::vector<int16_t> pcm_buf(kAudioSamplesPerFrame * 2);
        int64_t audio_pts = 0;

        // Resampler from S16 to FLTP
        AVChannelLayout in_ch_layout;
        av_channel_layout_default(&in_ch_layout, 2);

        SwrContext* swr = nullptr;
        swr_alloc_set_opts2(&swr,
                            &actx->ch_layout, actx->sample_fmt, actx->sample_rate,
                            &in_ch_layout, AV_SAMPLE_FMT_S16, 48000,
                            0, nullptr);
        if (swr) swr_init(swr);

        AVFrame* aframe = av_frame_alloc();
        aframe->nb_samples = kAudioSamplesPerFrame;
        aframe->format = actx->sample_fmt;
        av_channel_layout_copy(&aframe->ch_layout, &actx->ch_layout);
        aframe->sample_rate = actx->sample_rate;
        av_frame_get_buffer(aframe, 0);

        AVPacket* pkt = av_packet_alloc();

        while (g_running.load(std::memory_order_relaxed)) {
            int pa_err = 0;
            if (pa_simple_read(pa_record, pcm_buf.data(), pcm_buf.size() * sizeof(int16_t), &pa_err) < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            av_frame_make_writable(aframe);
            const uint8_t* in_data[1] = { reinterpret_cast<const uint8_t*>(pcm_buf.data()) };
            if (swr) {
                swr_convert(swr, aframe->data, kAudioSamplesPerFrame, in_data, kAudioSamplesPerFrame);
            }

            aframe->pts = audio_pts;
            audio_pts += kAudioSamplesPerFrame;

            if (avcodec_send_frame(actx, aframe) >= 0) {
                while (avcodec_receive_packet(actx, pkt) >= 0) {
                    av_packet_rescale_ts(pkt, actx->time_base, astream->time_base);
                    pkt->stream_index = astream->index;
                    std::lock_guard<std::mutex> lock(mux_mutex);
                    av_interleaved_write_frame(oc, pkt);
                    av_packet_unref(pkt);
                }
            }
        }

        // Flush remaining audio frames in encoder
        avcodec_send_frame(actx, nullptr);
        while (avcodec_receive_packet(actx, pkt) >= 0) {
            av_packet_rescale_ts(pkt, actx->time_base, astream->time_base);
            pkt->stream_index = astream->index;
            std::lock_guard<std::mutex> lock(mux_mutex);
            av_interleaved_write_frame(oc, pkt);
            av_packet_unref(pkt);
        }

        av_packet_free(&pkt);
        av_frame_free(&aframe);
        if (swr) swr_free(&swr);
        pa_simple_free(pa_record);
    });

    // 6. Main Socket Event Loop: Handle Frame Signals & Stop Commands
    AVPacket* v_pkt = av_packet_alloc();
    auto session_start = std::chrono::steady_clock::now();
    bool has_received_game_frame = false;
    AVPixelFormat last_src_fmt = AV_PIX_FMT_NONE;
    int last_src_w = 0, last_src_h = 0;

    auto frame_interval = std::chrono::microseconds(1000000 / target_fps);
    auto next_frame_time = std::chrono::steady_clock::now();
    int64_t next_pts = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        struct timeval sel_tv{};
        if (has_received_game_frame) {
            auto now = std::chrono::steady_clock::now();
            if (next_frame_time > now) {
                auto diff = std::chrono::duration_cast<std::chrono::microseconds>(next_frame_time - now).count();
                sel_tv.tv_sec = static_cast<long>(diff / 1000000);
                sel_tv.tv_usec = static_cast<long>(diff % 1000000);
            } else {
                sel_tv.tv_sec = 0;
                sel_tv.tv_usec = 0;
            }
        } else {
            sel_tv.tv_sec = 0;
            sel_tv.tv_usec = 50000; // 50ms wait for first frame
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);

        int ret = select(client_fd + 1, &read_fds, nullptr, nullptr, &sel_tv);
        if (ret > 0 && FD_ISSET(client_fd, &read_fds)) {
            fpshud::IpcPacketHeader in_hdr{};
            ssize_t rd = recv(client_fd, &in_hdr, sizeof(in_hdr), 0);
            if (rd <= 0) {
                // Client disconnected -> stop recording cleanly
                g_running.store(false, std::memory_order_relaxed);
                break;
            }

            if (in_hdr.magic == fpshud::kRecorderIpcMagic) {
                auto cmd = static_cast<fpshud::RecorderCommand>(in_hdr.command);
                if (cmd == fpshud::RecorderCommand::StopRecord) {
                    g_running.store(false, std::memory_order_relaxed);
                    break;
                } else if (cmd == fpshud::RecorderCommand::SendFrame) {
                    fpshud::IpcFrameNotify notify{};
                    size_t rem = sizeof(notify) - sizeof(fpshud::IpcPacketHeader);
                    ssize_t nr = recv(client_fd, reinterpret_cast<char*>(&notify) + sizeof(fpshud::IpcPacketHeader), rem, MSG_WAITALL);
                    if (nr == static_cast<ssize_t>(rem)) {
                        if (shm_base && shm_hdr && (shm_hdr->width != notify.width || shm_hdr->height != notify.height)) {
                            munmap(shm_base, shm_size);
                            shm_base = nullptr;
                            shm_hdr = nullptr;
                            shm_size = 0;
                        }
                        try_map_shm(shm_path);

                        if (shm_base && shm_hdr && notify.slot < 3) {
                            const uint8_t* src = static_cast<const uint8_t*>(shm_base) + shm_hdr->offset[notify.slot];
                            int stride = notify.width * 4;

                            AVPixelFormat src_fmt = (notify.format == 1) ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA;
                            if (!sws_ctx || last_src_fmt != src_fmt || last_src_w != static_cast<int>(notify.width) || last_src_h != static_cast<int>(notify.height)) {
                                if (sws_ctx) sws_freeContext(sws_ctx);
                                sws_ctx = sws_getContext(
                                    notify.width, notify.height, src_fmt,
                                    target_width, target_height, vctx->pix_fmt,
                                    SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                                );
                                last_src_fmt = src_fmt;
                                last_src_w = notify.width;
                                last_src_h = notify.height;
                            }

                            av_frame_make_writable(vframe);
                            if (notify.flip_y) {
                                const uint8_t* src_slice[1] = { src + static_cast<size_t>(notify.height - 1) * stride };
                                int src_stride[1] = { -stride };
                                sws_scale(sws_ctx, src_slice, src_stride, 0, notify.height, vframe->data, vframe->linesize);
                            } else {
                                const uint8_t* src_slice[1] = { src };
                                int src_stride[1] = { stride };
                                sws_scale(sws_ctx, src_slice, src_stride, 0, notify.height, vframe->data, vframe->linesize);
                            }

                            if (!has_received_game_frame) {
                                has_received_game_frame = true;
                                session_start = std::chrono::steady_clock::now();
                                next_frame_time = session_start;
                                next_pts = 0;
                            }
                        }
                    }
                } else if (cmd == fpshud::RecorderCommand::QueryStatus) {
                    fpshud::IpcStatusResponse status_resp{};
                    status_resp.magic = fpshud::kRecorderIpcMagic;
                    status_resp.status = static_cast<uint32_t>(fpshud::RecorderStatus::Recording);
                    status_resp.frame_count = static_cast<uint32_t>(video_frame_idx.load(std::memory_order_relaxed));
                    auto now = std::chrono::steady_clock::now();
                    status_resp.elapsed_sec = std::chrono::duration<float>(now - session_start).count();
                    send(client_fd, &status_resp, sizeof(status_resp), 0);
                }
            }
        }

        // Emit frames locked to exact wall-clock intervals
        if (has_received_game_frame) {
            auto cur_time = std::chrono::steady_clock::now();
            // Cap lag catch-up if game was paused or loading (> 4 frame intervals) to avoid massive burst
            if (cur_time - next_frame_time > frame_interval * 4) {
                next_frame_time = cur_time;
            }
            while (cur_time >= next_frame_time && g_running.load(std::memory_order_relaxed)) {
                vframe->pts = next_pts++;
                video_frame_idx.store(next_pts, std::memory_order_relaxed);

                if (avcodec_send_frame(vctx, vframe) >= 0) {
                    while (avcodec_receive_packet(vctx, v_pkt) >= 0) {
                        av_packet_rescale_ts(v_pkt, vctx->time_base, vstream->time_base);
                        v_pkt->stream_index = vstream->index;
                        std::lock_guard<std::mutex> lock(mux_mutex);
                        av_interleaved_write_frame(oc, v_pkt);
                        av_packet_unref(v_pkt);
                    }
                }
                next_frame_time += frame_interval;
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
    avcodec_send_frame(vctx, nullptr);
    while (avcodec_receive_packet(vctx, v_pkt) >= 0) {
        av_packet_rescale_ts(v_pkt, vctx->time_base, vstream->time_base);
        v_pkt->stream_index = vstream->index;
        std::lock_guard<std::mutex> lock(mux_mutex);
        av_interleaved_write_frame(oc, v_pkt);
        av_packet_unref(v_pkt);
    }
    av_packet_free(&v_pkt);

    av_write_trailer(oc);

    // Send final saved file path response back to client socket
    fpshud::IpcStatusResponse final_resp{};
    final_resp.magic = fpshud::kRecorderIpcMagic;
    final_resp.status = static_cast<uint32_t>(fpshud::RecorderStatus::Idle);
    final_resp.frame_count = static_cast<uint32_t>(video_frame_idx.load(std::memory_order_relaxed));
    std::snprintf(final_resp.last_saved_file, sizeof(final_resp.last_saved_file), "%s", out_filename.c_str());
    send(client_fd, &final_resp, sizeof(final_resp), 0);

    // Cleanup FFmpeg resources
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&oc->pb);
    }
    avcodec_free_context(&vctx);
    if (actx) avcodec_free_context(&actx);
    avformat_free_context(oc);
    av_frame_free(&vframe);
    if (sws_ctx) sws_freeContext(sws_ctx);

    close(client_fd);
    close(server_fd);
    unlink(socket_path.c_str());

    std::fprintf(stderr, "[FPSHUD Recorder] Successfully finalized video (%ld frames): %s\n",
                 video_frame_idx.load(std::memory_order_relaxed), out_filename.c_str());
    return 0;
}
