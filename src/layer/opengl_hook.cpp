#include "config.hpp"
#include "input_manager.hpp"
#include "overlay_ui.hpp"
#include "recorder_ipc.hpp"
#include "telemetry.hpp"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include <GL/gl.h>
#include <GL/glx.h>
#include <EGL/egl.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <chrono>

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_PIXEL_PACK_BUFFER_BINDING
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_READ_ONLY
#define GL_READ_ONLY 0x88B8
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif
#ifndef GL_VERSION
#define GL_VERSION 0x1F02
#endif
#ifndef GL_VIEWPORT
#define GL_VIEWPORT 0x0BA2
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT 0x0D05
#endif
#ifndef GL_PACK_ROW_LENGTH
#define GL_PACK_ROW_LENGTH 0x0D02
#endif

namespace {

using PFN_glXSwapBuffers = void (*)(Display*, GLXDrawable);
using PFN_glXGetProcAddress = __GLXextFuncPtr (*)(const GLubyte*);
using PFN_glXGetProcAddressARB = __GLXextFuncPtr (*)(const GLubyte*);
using PFN_eglSwapBuffers = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PFN_eglGetProcAddress = __eglMustCastToProperFunctionPointerType (*)(const char*);
using PFN_eglGetPlatformDisplay = EGLDisplay (*)(EGLenum, void*, const intptr_t*);
using PFN_eglGetPlatformDisplayEXT = EGLDisplay (*)(EGLenum, void*, const EGLint*);

PFN_glXSwapBuffers real_glXSwapBuffers = nullptr;
PFN_glXGetProcAddress real_glXGetProcAddress = nullptr;
PFN_glXGetProcAddressARB real_glXGetProcAddressARB = nullptr;
PFN_eglSwapBuffers real_eglSwapBuffers = nullptr;
PFN_eglGetProcAddress real_eglGetProcAddress = nullptr;
PFN_eglGetPlatformDisplay real_eglGetPlatformDisplay = nullptr;
PFN_eglGetPlatformDisplayEXT real_eglGetPlatformDisplayEXT = nullptr;

void ensure_egl_resolved();

static EGLDisplay Hook_eglGetPlatformDisplay(EGLenum platform, void* native_display,
                                             const intptr_t* attrib_list) {
  try {
    ensure_egl_resolved();
    if (platform == 0x31D8 && native_display) {  // EGL_PLATFORM_WAYLAND_KHR
      fpshud::get_input_manager().attach_wayland_display(native_display);
    }
  } catch (...) {
  }
  return real_eglGetPlatformDisplay
             ? real_eglGetPlatformDisplay(platform, native_display, attrib_list)
             : EGL_NO_DISPLAY;
}

static EGLDisplay Hook_eglGetPlatformDisplayEXT(EGLenum platform, void* native_display,
                                                const EGLint* attrib_list) {
  try {
    ensure_egl_resolved();
    if (platform == 0x31D8 && native_display) {  // EGL_PLATFORM_WAYLAND_EXT
      fpshud::get_input_manager().attach_wayland_display(native_display);
    }
  } catch (...) {
  }
  return real_eglGetPlatformDisplayEXT
             ? real_eglGetPlatformDisplayEXT(platform, native_display, attrib_list)
             : EGL_NO_DISPLAY;
}

struct GlDispatchTable {
  using PFN_glGetString = const GLubyte* (*)(GLenum name);
  using PFN_glGetIntegerv = void (*)(GLenum pname, GLint* data);
  using PFN_glReadPixels = void (*)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                                    GLenum type, void* pixels);
  using PFN_glGenBuffers = void (*)(GLsizei n, GLuint* buffers);
  using PFN_glDeleteBuffers = void (*)(GLsizei n, const GLuint* buffers);
  using PFN_glBindBuffer = void (*)(GLenum target, GLuint buffer);
  using PFN_glBufferData = void (*)(GLenum target, ptrdiff_t size, const void* data, GLenum usage);
  using PFN_glMapBufferRange = void* (*)(GLenum target, ptrdiff_t offset, ptrdiff_t length,
                                         GLbitfield access);
  using PFN_glMapBuffer = void* (*)(GLenum target, GLenum access);
  using PFN_glUnmapBuffer = GLboolean (*)(GLenum target);
  using PFN_glBindFramebuffer = void (*)(GLenum target, GLuint framebuffer);
  using PFN_glPixelStorei = void (*)(GLenum pname, GLint param);

  using PFN_glXGetCurrentContext = GLXContext (*)();
  using PFN_glXQueryDrawable = void (*)(Display* dpy, GLXDrawable draw, int attribute,
                                        unsigned int* value);

  using PFN_eglGetCurrentContext = EGLContext (*)();
  using PFN_eglQuerySurface = EGLBoolean (*)(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
                                             EGLint* value);

  PFN_glGetString glGetString = nullptr;
  PFN_glGetIntegerv glGetIntegerv = nullptr;
  PFN_glReadPixels glReadPixels = nullptr;
  PFN_glGenBuffers glGenBuffers = nullptr;
  PFN_glDeleteBuffers glDeleteBuffers = nullptr;
  PFN_glBindBuffer glBindBuffer = nullptr;
  PFN_glBufferData glBufferData = nullptr;
  PFN_glMapBufferRange glMapBufferRange = nullptr;
  PFN_glMapBuffer glMapBuffer = nullptr;
  PFN_glUnmapBuffer glUnmapBuffer = nullptr;
  PFN_glBindFramebuffer glBindFramebuffer = nullptr;
  PFN_glPixelStorei glPixelStorei = nullptr;

  PFN_glXGetCurrentContext glXGetCurrentContext = nullptr;
  PFN_glXQueryDrawable glXQueryDrawable = nullptr;

  PFN_eglGetCurrentContext eglGetCurrentContext = nullptr;
  PFN_eglQuerySurface eglQuerySurface = nullptr;

  bool pbo_supported() const noexcept {
    return glGenBuffers && glDeleteBuffers && glBindBuffer && glBufferData &&
           (glMapBufferRange || glMapBuffer) && glUnmapBuffer;
  }
};

GlDispatchTable g_gl{};

std::mutex g_gl_mutex;
ImGuiContext* g_gl_imgui_ctx = nullptr;
void* g_last_gl_context = nullptr;
bool g_gl_backend_initialized = false;

void* get_glx_lib() {
  static void* handle = []() {
    void* h = dlopen("libGLX.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libGL.so", RTLD_LAZY | RTLD_LOCAL);
    return h;
  }();
  return handle;
}

void* get_egl_lib() {
  static void* handle = []() {
    void* h = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);
    return h;
  }();
  return handle;
}

void* get_gl_lib() {
  static void* handle = []() {
    void* h = dlopen("libOpenGL.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libGL.so", RTLD_LAZY | RTLD_LOCAL);
    return h;
  }();
  return handle;
}

bool is_valid_gl_proc(void* p) noexcept {
  auto v = reinterpret_cast<uintptr_t>(p);
  return (v > 4 && v != static_cast<uintptr_t>(-1));
}

void* resolve_proc(const char* name) {
  if (!name) return nullptr;

  // 1. Try RTLD_DEFAULT
  void* sym = dlsym(RTLD_DEFAULT, name);
  if (is_valid_gl_proc(sym)) return sym;

  // 2. Try real_glXGetProcAddress / real_glXGetProcAddressARB
  if (real_glXGetProcAddress) {
    auto p =
        reinterpret_cast<void*>(real_glXGetProcAddress(reinterpret_cast<const GLubyte*>(name)));
    if (is_valid_gl_proc(p)) return p;
  }
  if (real_glXGetProcAddressARB) {
    auto p =
        reinterpret_cast<void*>(real_glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name)));
    if (is_valid_gl_proc(p)) return p;
  }

  // 3. Try real_eglGetProcAddress
  if (real_eglGetProcAddress) {
    auto p = reinterpret_cast<void*>(real_eglGetProcAddress(name));
    if (is_valid_gl_proc(p)) return p;
  }

  // 4. Try dynamic libraries
  if (void* h = get_glx_lib()) {
    sym = dlsym(h, name);
    if (is_valid_gl_proc(sym)) return sym;
  }
  if (void* h = get_egl_lib()) {
    sym = dlsym(h, name);
    if (is_valid_gl_proc(sym)) return sym;
  }
  if (void* h = get_gl_lib()) {
    sym = dlsym(h, name);
    if (is_valid_gl_proc(sym)) return sym;
  }

  return nullptr;
}

static std::once_flag g_glx_resolve_once;
static std::once_flag g_egl_resolve_once;

void ensure_glx_resolved() {
  std::call_once(g_glx_resolve_once, []() {
    if (!real_glXSwapBuffers) {
      real_glXSwapBuffers =
          reinterpret_cast<PFN_glXSwapBuffers>(dlsym(RTLD_NEXT, "glXSwapBuffers"));
      if (!real_glXSwapBuffers && get_glx_lib()) {
        real_glXSwapBuffers =
            reinterpret_cast<PFN_glXSwapBuffers>(dlsym(get_glx_lib(), "glXSwapBuffers"));
      }
    }
    if (!real_glXGetProcAddress) {
      real_glXGetProcAddress =
          reinterpret_cast<PFN_glXGetProcAddress>(dlsym(RTLD_NEXT, "glXGetProcAddress"));
      if (!real_glXGetProcAddress && get_glx_lib()) {
        real_glXGetProcAddress =
            reinterpret_cast<PFN_glXGetProcAddress>(dlsym(get_glx_lib(), "glXGetProcAddress"));
      }
    }
    if (!real_glXGetProcAddressARB) {
      real_glXGetProcAddressARB =
          reinterpret_cast<PFN_glXGetProcAddressARB>(dlsym(RTLD_NEXT, "glXGetProcAddressARB"));
      if (!real_glXGetProcAddressARB && get_glx_lib()) {
        real_glXGetProcAddressARB = reinterpret_cast<PFN_glXGetProcAddressARB>(
            dlsym(get_glx_lib(), "glXGetProcAddressARB"));
      }
    }
    if (!g_gl.glXGetCurrentContext) {
      g_gl.glXGetCurrentContext = reinterpret_cast<GlDispatchTable::PFN_glXGetCurrentContext>(
          resolve_proc("glXGetCurrentContext"));
    }
    if (!g_gl.glXQueryDrawable) {
      g_gl.glXQueryDrawable =
          reinterpret_cast<GlDispatchTable::PFN_glXQueryDrawable>(resolve_proc("glXQueryDrawable"));
    }
  });
}

void ensure_egl_resolved() {
  std::call_once(g_egl_resolve_once, []() {
    if (!real_eglSwapBuffers) {
      real_eglSwapBuffers =
          reinterpret_cast<PFN_eglSwapBuffers>(dlsym(RTLD_NEXT, "eglSwapBuffers"));
      if (!real_eglSwapBuffers && get_egl_lib()) {
        real_eglSwapBuffers =
            reinterpret_cast<PFN_eglSwapBuffers>(dlsym(get_egl_lib(), "eglSwapBuffers"));
      }
    }
    if (!real_eglGetProcAddress) {
      real_eglGetProcAddress =
          reinterpret_cast<PFN_eglGetProcAddress>(dlsym(RTLD_NEXT, "eglGetProcAddress"));
      if (!real_eglGetProcAddress && get_egl_lib()) {
        real_eglGetProcAddress =
            reinterpret_cast<PFN_eglGetProcAddress>(dlsym(get_egl_lib(), "eglGetProcAddress"));
      }
    }
    if (!g_gl.eglGetCurrentContext) {
      g_gl.eglGetCurrentContext = reinterpret_cast<GlDispatchTable::PFN_eglGetCurrentContext>(
          resolve_proc("eglGetCurrentContext"));
    }
    if (!g_gl.eglQuerySurface) {
      g_gl.eglQuerySurface =
          reinterpret_cast<GlDispatchTable::PFN_eglQuerySurface>(resolve_proc("eglQuerySurface"));
    }
    if (!real_eglGetPlatformDisplay) {
      real_eglGetPlatformDisplay =
          reinterpret_cast<PFN_eglGetPlatformDisplay>(resolve_proc("eglGetPlatformDisplay"));
    }
    if (!real_eglGetPlatformDisplayEXT) {
      real_eglGetPlatformDisplayEXT =
          reinterpret_cast<PFN_eglGetPlatformDisplayEXT>(resolve_proc("eglGetPlatformDisplayEXT"));
    }
  });
}

static bool g_core_gl_resolved = false;

void ensure_core_gl_resolved() {
  if (g_core_gl_resolved) return;

  g_gl.glGetString =
      reinterpret_cast<GlDispatchTable::PFN_glGetString>(resolve_proc("glGetString"));
  g_gl.glGetIntegerv =
      reinterpret_cast<GlDispatchTable::PFN_glGetIntegerv>(resolve_proc("glGetIntegerv"));
  g_gl.glReadPixels =
      reinterpret_cast<GlDispatchTable::PFN_glReadPixels>(resolve_proc("glReadPixels"));
  g_gl.glGenBuffers =
      reinterpret_cast<GlDispatchTable::PFN_glGenBuffers>(resolve_proc("glGenBuffers"));
  g_gl.glDeleteBuffers =
      reinterpret_cast<GlDispatchTable::PFN_glDeleteBuffers>(resolve_proc("glDeleteBuffers"));
  g_gl.glBindBuffer =
      reinterpret_cast<GlDispatchTable::PFN_glBindBuffer>(resolve_proc("glBindBuffer"));
  g_gl.glBufferData =
      reinterpret_cast<GlDispatchTable::PFN_glBufferData>(resolve_proc("glBufferData"));
  g_gl.glMapBufferRange =
      reinterpret_cast<GlDispatchTable::PFN_glMapBufferRange>(resolve_proc("glMapBufferRange"));
  g_gl.glMapBuffer =
      reinterpret_cast<GlDispatchTable::PFN_glMapBuffer>(resolve_proc("glMapBuffer"));
  g_gl.glUnmapBuffer =
      reinterpret_cast<GlDispatchTable::PFN_glUnmapBuffer>(resolve_proc("glUnmapBuffer"));
  g_gl.glBindFramebuffer =
      reinterpret_cast<GlDispatchTable::PFN_glBindFramebuffer>(resolve_proc("glBindFramebuffer"));
  g_gl.glPixelStorei =
      reinterpret_cast<GlDispatchTable::PFN_glPixelStorei>(resolve_proc("glPixelStorei"));

  g_core_gl_resolved = true;
}

class GlAsyncPboRing {
public:
  static constexpr size_t kSlotCount = 2;

  void cleanup(const GlDispatchTable& gl) {
    if (!m_initialized) return;
    if (gl.glDeleteBuffers) {
      gl.glDeleteBuffers(static_cast<GLsizei>(kSlotCount), m_pbos);
    }
    m_pbos[0] = 0;
    m_pbos[1] = 0;
    m_width = 0;
    m_height = 0;
    m_buffer_size = 0;
    m_write_idx = 0;
    m_read_idx = 0;
    m_pending_frames = 0;
    m_initialized = false;
    m_owner_ctx = nullptr;
  }

  void reset_pending() noexcept {
    m_pending_frames = 0;
    m_write_idx = 0;
    m_read_idx = 0;
  }

  bool ensure_allocated(const GlDispatchTable& gl, void* ctx, uint32_t width, uint32_t height) {
    if (!gl.pbo_supported()) {
      return false;
    }

    size_t needed_size = static_cast<size_t>(width) * height * 4;
    if (needed_size == 0) return false;

    if (m_initialized && m_owner_ctx == ctx && m_width == width && m_height == height &&
        m_buffer_size >= needed_size) {
      return true;
    }

    cleanup(gl);

    m_owner_ctx = ctx;
    m_width = width;
    m_height = height;
    m_buffer_size = needed_size;
    m_write_idx = 0;
    m_read_idx = 0;
    m_pending_frames = 0;

    gl.glGenBuffers(static_cast<GLsizei>(kSlotCount), m_pbos);
    for (size_t i = 0; i < kSlotCount; ++i) {
      gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbos[i]);
      gl.glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<ptrdiff_t>(m_buffer_size), nullptr,
                      GL_STREAM_READ);
    }
    gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    m_initialized = (m_pbos[0] != 0 && m_pbos[1] != 0);
    return m_initialized;
  }

  void capture_frame(const GlDispatchTable& gl, void* ctx, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    // Save game's existing GL state (PBO, Read FBO, and Pack Alignment/RowLength)
    GLint prev_pbo = 0;
    GLint prev_read_fbo = 0;
    GLint prev_pack_align = 4;
    GLint prev_row_len = 0;
    if (gl.glGetIntegerv) {
      gl.glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pbo);
      gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
      gl.glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack_align);
      gl.glGetIntegerv(GL_PACK_ROW_LENGTH, &prev_row_len);
    }

    // Ensure reading from default backbuffer and standard tightly packed RGBA/BGRA
    if (gl.glBindFramebuffer && prev_read_fbo != 0) {
      gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }
    if (gl.glPixelStorei) {
      gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
      gl.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    }

    if (ensure_allocated(gl, ctx, width, height)) {
      // Step 1: Async DMA read into pbo[write_idx]
      gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbos[m_write_idx]);
      gl.glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_BGRA,
                      GL_UNSIGNED_BYTE, nullptr);

      // Step 2: Map & submit previous completed frame from pbo[read_idx]
      if (m_pending_frames > 0) {
        gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbos[m_read_idx]);
        void* mapped_ptr = nullptr;
        if (gl.glMapBufferRange) {
          mapped_ptr = gl.glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                           static_cast<ptrdiff_t>(m_buffer_size), GL_MAP_READ_BIT);
        } else if (gl.glMapBuffer) {
          mapped_ptr = gl.glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
        }

        if (mapped_ptr) {
          fpshud::RecorderController::instance().submit_frame(
              reinterpret_cast<const uint8_t*>(mapped_ptr), width, height, 0 /* BGRA */,
              true /* flip_y */
          );
          gl.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        m_read_idx = (m_read_idx + 1) % kSlotCount;
      } else {
        m_pending_frames++;
      }

      m_write_idx = (m_write_idx + 1) % kSlotCount;
    } else {
      // Fallback: Synchronous RAM capture when PBO is unsupported
      if (gl.glBindBuffer && prev_pbo != 0) {
        gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      }
      static std::vector<uint8_t> s_client_pixels;
      size_t needed = static_cast<size_t>(width) * height * 4;
      if (s_client_pixels.size() < needed) {
        s_client_pixels.resize(needed);
      }
      if (gl.glReadPixels) {
        gl.glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_BGRA,
                        GL_UNSIGNED_BYTE, s_client_pixels.data());
        fpshud::RecorderController::instance().submit_frame(
            s_client_pixels.data(), width, height, 0 /* BGRA */, true /* flip_y */
        );
      }
    }

    // Restore game's previous GL state
    if (gl.glBindFramebuffer && prev_read_fbo != 0) {
      gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read_fbo));
    }
    if (gl.glPixelStorei) {
      gl.glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_align);
      gl.glPixelStorei(GL_PACK_ROW_LENGTH, prev_row_len);
    }
    if (gl.glBindBuffer) {
      gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prev_pbo));
    }
  }

private:
  GLuint m_pbos[kSlotCount] = {0, 0};
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  size_t m_buffer_size = 0;
  size_t m_write_idx = 0;
  size_t m_read_idx = 0;
  int m_pending_frames = 0;
  bool m_initialized = false;
  void* m_owner_ctx = nullptr;
};

GlAsyncPboRing g_gl_pbo_ring;

void render_gl_frame(void* current_ctx, unsigned int fallback_w, unsigned int fallback_h) {
  auto& cfg = fpshud::get_mutable_config();
  if (!cfg.enabled || !current_ctx) return;

  auto& telemetry = fpshud::get_telemetry();
  telemetry.set_render_api("OPENGL");
  telemetry.on_frame();

  std::lock_guard<std::mutex> lock(g_gl_mutex);

  if (g_last_gl_context != current_ctx) {
    if (g_gl_backend_initialized && g_gl_imgui_ctx) {
      ImGui::SetCurrentContext(g_gl_imgui_ctx);
      ImGui_ImplOpenGL3_Shutdown();
      ImGui::DestroyContext(g_gl_imgui_ctx);
      g_gl_imgui_ctx = nullptr;
      g_gl_backend_initialized = false;
    }
    g_gl_pbo_ring.cleanup(g_gl);
    g_core_gl_resolved = false;
    g_last_gl_context = current_ctx;
  }

  ensure_core_gl_resolved();

  if (!g_gl_imgui_ctx) {
    g_gl_imgui_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_gl_imgui_ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    fpshud::setup_overlay_font(io);

    const char* gl_ver =
        g_gl.glGetString ? reinterpret_cast<const char*>(g_gl.glGetString(GL_VERSION)) : nullptr;
    const char* glsl_version = "#version 130";
    if (gl_ver && std::strstr(gl_ver, "OpenGL ES")) {
      glsl_version =
          (std::strstr(gl_ver, "OpenGL ES 2") != nullptr) ? "#version 100" : "#version 300 es";
    }
    g_gl_backend_initialized = ImGui_ImplOpenGL3_Init(glsl_version);
  }

  if (!g_gl_backend_initialized) return;

  ImGui::SetCurrentContext(g_gl_imgui_ctx);

  GLint vp[4] = {0, 0, 0, 0};
  if (g_gl.glGetIntegerv) {
    g_gl.glGetIntegerv(GL_VIEWPORT, vp);
  }
  float width = (vp[2] > 0) ? static_cast<float>(vp[2]) : static_cast<float>(fallback_w);
  float height = (vp[3] > 0) ? static_cast<float>(vp[3]) : static_cast<float>(fallback_h);

  if (width <= 0.0f || height <= 0.0f) return;

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(width, height);

  {
    std::lock_guard<std::mutex> cfg_lock(fpshud::get_config_mutex());
    fpshud::get_input_manager().update_for_frame(io, cfg, width, height);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    fpshud::render_overlay_ui(cfg, telemetry.get_snapshot(), width, height);

    ImGui::Render();
  }
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  if (fpshud::RecorderController::instance().is_recording() && width > 0.0f && height > 0.0f) {
    static auto s_last_gl_capture = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    int rec_fps = (cfg.record_fps > 0) ? cfg.record_fps : 60;
    auto min_interval = std::chrono::microseconds(1000000 / (rec_fps + 2));
    if (now - s_last_gl_capture >= min_interval) {
      s_last_gl_capture = now;
      g_gl_pbo_ring.capture_frame(g_gl, current_ctx, static_cast<uint32_t>(width),
                                  static_cast<uint32_t>(height));
    }
  } else {
    g_gl_pbo_ring.reset_pending();
  }
}

}  // namespace

extern "C" {

__attribute__((visibility("default"))) void glXSwapBuffers(Display* dpy, GLXDrawable drawable) {
  try {
    ensure_glx_resolved();
    if (fpshud::get_global_config().enabled) {
      GLXContext ctx = g_gl.glXGetCurrentContext ? g_gl.glXGetCurrentContext() : nullptr;
      unsigned int w = 0, h = 0;
      if (dpy && drawable && g_gl.glXQueryDrawable) {
        g_gl.glXQueryDrawable(dpy, drawable, GLX_WIDTH, &w);
        g_gl.glXQueryDrawable(dpy, drawable, GLX_HEIGHT, &h);
      }
      render_gl_frame(reinterpret_cast<void*>(ctx), w, h);
    }
  } catch (...) {
    // Exception safety at C ABI boundary
  }
  if (real_glXSwapBuffers) {
    real_glXSwapBuffers(dpy, drawable);
  }
}

__attribute__((visibility("default"))) __GLXextFuncPtr glXGetProcAddress(const GLubyte* procName) {
  try {
    ensure_glx_resolved();
    if (procName) {
      const char* name = reinterpret_cast<const char*>(procName);
      if (std::strcmp(name, "glXSwapBuffers") == 0) {
        return reinterpret_cast<__GLXextFuncPtr>(&glXSwapBuffers);
      }
    }
    return real_glXGetProcAddress ? real_glXGetProcAddress(procName) : nullptr;
  } catch (...) {
    return real_glXGetProcAddress ? real_glXGetProcAddress(procName) : nullptr;
  }
}

__attribute__((visibility("default"))) __GLXextFuncPtr
glXGetProcAddressARB(const GLubyte* procName) {
  try {
    ensure_glx_resolved();
    if (procName) {
      const char* name = reinterpret_cast<const char*>(procName);
      if (std::strcmp(name, "glXSwapBuffers") == 0) {
        return reinterpret_cast<__GLXextFuncPtr>(&glXSwapBuffers);
      }
    }
    return real_glXGetProcAddressARB ? real_glXGetProcAddressARB(procName) : nullptr;
  } catch (...) {
    return real_glXGetProcAddressARB ? real_glXGetProcAddressARB(procName) : nullptr;
  }
}

__attribute__((visibility("default"))) EGLBoolean eglSwapBuffers(EGLDisplay dpy,
                                                                 EGLSurface surface) {
  try {
    ensure_egl_resolved();
    if (fpshud::get_global_config().enabled) {
      EGLContext ctx = g_gl.eglGetCurrentContext ? g_gl.eglGetCurrentContext() : nullptr;
      EGLint w = 0, h = 0;
      if (dpy != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE && g_gl.eglQuerySurface) {
        g_gl.eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
        g_gl.eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
      }
      render_gl_frame(reinterpret_cast<void*>(ctx), static_cast<unsigned int>(w),
                      static_cast<unsigned int>(h));
    }
  } catch (...) {
    // Exception safety at C ABI boundary
  }
  return real_eglSwapBuffers ? real_eglSwapBuffers(dpy, surface) : EGL_FALSE;
}

__attribute__((visibility("default"))) __eglMustCastToProperFunctionPointerType
eglGetProcAddress(const char* procName) {
  try {
    ensure_egl_resolved();
    if (procName) {
      if (std::strcmp(procName, "eglSwapBuffers") == 0) {
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&eglSwapBuffers);
      }
      if (std::strcmp(procName, "eglGetPlatformDisplay") == 0) {
        if (!real_eglGetPlatformDisplay && real_eglGetProcAddress) {
          real_eglGetPlatformDisplay = reinterpret_cast<PFN_eglGetPlatformDisplay>(
              real_eglGetProcAddress("eglGetPlatformDisplay"));
        }
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(
            &Hook_eglGetPlatformDisplay);
      }
      if (std::strcmp(procName, "eglGetPlatformDisplayEXT") == 0) {
        if (!real_eglGetPlatformDisplayEXT && real_eglGetProcAddress) {
          real_eglGetPlatformDisplayEXT = reinterpret_cast<PFN_eglGetPlatformDisplayEXT>(
              real_eglGetProcAddress("eglGetPlatformDisplayEXT"));
        }
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(
            &Hook_eglGetPlatformDisplayEXT);
      }
    }
    return real_eglGetProcAddress ? real_eglGetProcAddress(procName) : nullptr;
  } catch (...) {
    return real_eglGetProcAddress ? real_eglGetProcAddress(procName) : nullptr;
  }
}

}  // extern "C"
