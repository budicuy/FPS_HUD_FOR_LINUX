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
#include <cstring>
#include <mutex>

namespace {

using PFN_glXSwapBuffers = void (*)(Display*, GLXDrawable);
using PFN_glXGetProcAddress = __GLXextFuncPtr (*)(const GLubyte*);
using PFN_glXGetProcAddressARB = __GLXextFuncPtr (*)(const GLubyte*);
using PFN_eglSwapBuffers = EGLBoolean (*)(EGLDisplay, EGLSurface);

PFN_glXSwapBuffers real_glXSwapBuffers = nullptr;
PFN_glXGetProcAddress real_glXGetProcAddress = nullptr;
PFN_glXGetProcAddressARB real_glXGetProcAddressARB = nullptr;
PFN_eglSwapBuffers real_eglSwapBuffers = nullptr;

std::mutex g_gl_mutex;
ImGuiContext* g_gl_imgui_ctx = nullptr;
void* g_last_gl_context = nullptr;
bool g_gl_backend_initialized = false;

void* get_glx_lib() {
    static void* handle = []() {
        void* h = dlopen("libGLX.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!h) h = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
        return h;
    }();
    return handle;
}

void* get_egl_lib() {
    static void* handle = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    return handle;
}

void ensure_glx_resolved() {
    if (!real_glXSwapBuffers) {
        real_glXSwapBuffers = reinterpret_cast<PFN_glXSwapBuffers>(dlsym(RTLD_NEXT, "glXSwapBuffers"));
        if (!real_glXSwapBuffers && get_glx_lib()) {
            real_glXSwapBuffers = reinterpret_cast<PFN_glXSwapBuffers>(dlsym(get_glx_lib(), "glXSwapBuffers"));
        }
    }
    if (!real_glXGetProcAddress) {
        real_glXGetProcAddress = reinterpret_cast<PFN_glXGetProcAddress>(dlsym(RTLD_NEXT, "glXGetProcAddress"));
        if (!real_glXGetProcAddress && get_glx_lib()) {
            real_glXGetProcAddress = reinterpret_cast<PFN_glXGetProcAddress>(dlsym(get_glx_lib(), "glXGetProcAddress"));
        }
    }
    if (!real_glXGetProcAddressARB) {
        real_glXGetProcAddressARB = reinterpret_cast<PFN_glXGetProcAddressARB>(dlsym(RTLD_NEXT, "glXGetProcAddressARB"));
        if (!real_glXGetProcAddressARB && get_glx_lib()) {
            real_glXGetProcAddressARB = reinterpret_cast<PFN_glXGetProcAddressARB>(dlsym(get_glx_lib(), "glXGetProcAddressARB"));
        }
    }
}

void ensure_egl_resolved() {
    if (!real_eglSwapBuffers) {
        real_eglSwapBuffers = reinterpret_cast<PFN_eglSwapBuffers>(dlsym(RTLD_NEXT, "eglSwapBuffers"));
        if (!real_eglSwapBuffers && get_egl_lib()) {
            real_eglSwapBuffers = reinterpret_cast<PFN_eglSwapBuffers>(dlsym(get_egl_lib(), "eglSwapBuffers"));
        }
    }
}

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
        g_last_gl_context = current_ctx;
    }

    if (!g_gl_imgui_ctx) {
        g_gl_imgui_ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_gl_imgui_ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImGui::StyleColorsDark();
        fpshud::setup_overlay_font(io);
        g_gl_backend_initialized = ImGui_ImplOpenGL3_Init("#version 130");
    }

    if (!g_gl_backend_initialized) return;

    ImGui::SetCurrentContext(g_gl_imgui_ctx);

    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
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
    if (fpshud::RecorderController::instance().is_recording() && width > 0 && height > 0) {
        static std::vector<uint8_t> s_gl_pixels;
        size_t needed = static_cast<size_t>(width) * height * 4;
        if (s_gl_pixels.size() < needed) {
            s_gl_pixels.resize(needed);
        }
        glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, s_gl_pixels.data());
        fpshud::RecorderController::instance().submit_frame(s_gl_pixels.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height), 0 /* BGRA */, true /* flip_y */);
    }
}

} // namespace

extern "C" {

__attribute__((visibility("default")))
void glXSwapBuffers(Display* dpy, GLXDrawable drawable) {
    ensure_glx_resolved();
    if (fpshud::get_global_config().enabled) {
        GLXContext ctx = glXGetCurrentContext();
        unsigned int w = 0, h = 0;
        if (dpy && drawable) {
            glXQueryDrawable(dpy, drawable, GLX_WIDTH, &w);
            glXQueryDrawable(dpy, drawable, GLX_HEIGHT, &h);
        }
        render_gl_frame(reinterpret_cast<void*>(ctx), w, h);
    }
    if (real_glXSwapBuffers) {
        real_glXSwapBuffers(dpy, drawable);
    }
}

__attribute__((visibility("default")))
__GLXextFuncPtr glXGetProcAddress(const GLubyte* procName) {
    ensure_glx_resolved();
    if (procName) {
        const char* name = reinterpret_cast<const char*>(procName);
        if (std::strcmp(name, "glXSwapBuffers") == 0) {
            return reinterpret_cast<__GLXextFuncPtr>(&glXSwapBuffers);
        }
    }
    return real_glXGetProcAddress ? real_glXGetProcAddress(procName) : nullptr;
}

__attribute__((visibility("default")))
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte* procName) {
    ensure_glx_resolved();
    if (procName) {
        const char* name = reinterpret_cast<const char*>(procName);
        if (std::strcmp(name, "glXSwapBuffers") == 0) {
            return reinterpret_cast<__GLXextFuncPtr>(&glXSwapBuffers);
        }
    }
    return real_glXGetProcAddressARB ? real_glXGetProcAddressARB(procName) : nullptr;
}

__attribute__((visibility("default")))
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    ensure_egl_resolved();
    if (fpshud::get_global_config().enabled) {
        EGLContext ctx = eglGetCurrentContext();
        EGLint w = 0, h = 0;
        if (dpy != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE) {
            eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
            eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
        }
        render_gl_frame(reinterpret_cast<void*>(ctx), static_cast<unsigned int>(w), static_cast<unsigned int>(h));
    }
    return real_eglSwapBuffers ? real_eglSwapBuffers(dpy, surface) : EGL_FALSE;
}

} // extern "C"
