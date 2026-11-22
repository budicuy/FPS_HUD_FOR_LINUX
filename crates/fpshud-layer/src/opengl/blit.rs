use crate::runtime::LayerRuntime;
use fpshud_screenshot::{PixelFormat, RawFrameCapture};
use glow::HasContext;
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{CStr, c_char, c_void};
use std::num::NonZeroU32;
use std::sync::{Mutex, OnceLock};

const QUAD_VERT_GL: &str = r"#version 130
out vec2 v_uv;
void main() {
    float x = float((gl_VertexID & 1) << 2) - 1.0;
    float y = float((gl_VertexID & 2) << 1) - 1.0;
    v_uv = vec2((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    gl_Position = vec4(x, y, 0.0, 1.0);
}
";

const QUAD_FRAG_GL: &str = r"#version 130
uniform sampler2D u_hud_tex;
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec4 col = texture(u_hud_tex, v_uv);
    if (col.a <= 0.001) discard;
    o_color = col;
}
";

const QUAD_VERT_GLES: &str = r"#version 300 es
precision mediump float;
out vec2 v_uv;
void main() {
    float x = float((gl_VertexID & 1) << 2) - 1.0;
    float y = float((gl_VertexID & 2) << 1) - 1.0;
    v_uv = vec2((x + 1.0) * 0.5, (1.0 - y) * 0.5);
    gl_Position = vec4(x, y, 0.0, 1.0);
}
";

const QUAD_FRAG_GLES: &str = r"#version 300 es
precision mediump float;
uniform sampler2D u_hud_tex;
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec4 col = texture(u_hud_tex, v_uv);
    if (col.a <= 0.001) discard;
    o_color = col;
}
";

#[must_use]
pub fn parse_gl_version_is_es(version_str: &str) -> bool {
    let lower = version_str.trim().to_ascii_lowercase();
    lower.contains("opengl es")
}

#[must_use]
pub const fn select_gl_quad_shaders(is_es: bool) -> (&'static str, &'static str) {
    if is_es {
        (QUAD_VERT_GLES, QUAD_FRAG_GLES)
    } else {
        (QUAD_VERT_GL, QUAD_FRAG_GL)
    }
}

#[must_use]
pub fn build_gl_readpixels_capture(
    width: u32,
    height: u32,
    raw: Vec<u8>,
) -> Option<RawFrameCapture> {
    let expected = (width as usize).checked_mul(height as usize)?.checked_mul(4)?;
    if expected == 0 || raw.len() != expected {
        return None;
    }
    Some(RawFrameCapture {
        width,
        height,
        format: PixelFormat::Rgba8,
        flip_y: true,
        pixels: raw,
    })
}

pub trait GlStateOps {
    fn get_integerv(&self, pname: u32) -> i32;
    fn get_viewport(&self) -> [i32; 4];
    fn is_enabled(&self, cap: u32) -> bool;
    fn use_program(&self, prog: u32);
    fn bind_vertex_array(&self, vao: u32);
    fn bind_framebuffer(&self, fbo: u32);
    fn bind_unpack_buffer(&self, buf: u32);
    fn set_enabled(&self, cap: u32, enable: bool);
    fn blend_func(&self, src: u32, dst: u32);
    fn viewport(&self, vp: [i32; 4]);
    fn active_texture(&self, unit: u32);
    fn bind_texture_2d(&self, tex: u32);
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GlStateBackup {
    pub prev_program: i32,
    pub prev_vao: i32,
    pub prev_fbo: i32,
    pub unpack_pbo: i32,
    pub blend_enabled: bool,
    pub blend_src_rgb: u32,
    pub blend_dst_rgb: u32,
    pub scissor_enabled: bool,
    pub viewport: [i32; 4],
}

impl GlStateBackup {
    #[must_use]
    pub fn capture(ops: &dyn GlStateOps) -> Self {
        Self {
            prev_program: ops.get_integerv(glow::CURRENT_PROGRAM),
            prev_vao: ops.get_integerv(glow::VERTEX_ARRAY_BINDING),
            prev_fbo: ops.get_integerv(glow::DRAW_FRAMEBUFFER_BINDING),
            unpack_pbo: ops.get_integerv(glow::PIXEL_UNPACK_BUFFER_BINDING),
            blend_enabled: ops.is_enabled(glow::BLEND),
            blend_src_rgb: ops.get_integerv(glow::BLEND_SRC_RGB) as u32,
            blend_dst_rgb: ops.get_integerv(glow::BLEND_DST_RGB) as u32,
            scissor_enabled: ops.is_enabled(glow::SCISSOR_TEST),
            viewport: ops.get_viewport(),
        }
    }

    pub fn restore(&self, ops: &dyn GlStateOps) {
        ops.use_program(self.prev_program.max(0) as u32);
        ops.bind_vertex_array(self.prev_vao.max(0) as u32);
        ops.bind_framebuffer(self.prev_fbo.max(0) as u32);
        ops.bind_unpack_buffer(self.unpack_pbo.max(0) as u32);
        ops.set_enabled(glow::BLEND, self.blend_enabled);
        ops.blend_func(self.blend_src_rgb, self.blend_dst_rgb);
        ops.set_enabled(glow::SCISSOR_TEST, self.scissor_enabled);
        ops.viewport(self.viewport);
    }
}

impl GlStateOps for glow::Context {
    fn get_integerv(&self, pname: u32) -> i32 {
        unsafe { <Self as HasContext>::get_parameter_i32(self, pname) }
    }

    fn get_viewport(&self) -> [i32; 4] {
        let mut vp = [0_i32; 4];
        unsafe { self.get_parameter_i32_slice(glow::VIEWPORT, &mut vp) };
        vp
    }

    fn is_enabled(&self, cap: u32) -> bool {
        unsafe { <Self as HasContext>::is_enabled(self, cap) }
    }

    fn use_program(&self, prog: u32) {
        unsafe { <Self as HasContext>::use_program(self, NonZeroU32::new(prog).map(glow::NativeProgram)) };
    }

    fn bind_vertex_array(&self, vao: u32) {
        unsafe {
            <Self as HasContext>::bind_vertex_array(
                self,
                NonZeroU32::new(vao).map(glow::NativeVertexArray),
            );
        }
    }

    fn bind_framebuffer(&self, fbo: u32) {
        unsafe {
            <Self as HasContext>::bind_framebuffer(
                self,
                glow::DRAW_FRAMEBUFFER,
                NonZeroU32::new(fbo).map(glow::NativeFramebuffer),
            );
        }
    }

    fn bind_unpack_buffer(&self, buf: u32) {
        unsafe {
            <Self as HasContext>::bind_buffer(
                self,
                glow::PIXEL_UNPACK_BUFFER,
                NonZeroU32::new(buf).map(glow::NativeBuffer),
            );
        }
    }

    fn set_enabled(&self, cap: u32, enable: bool) {
        if enable {
            unsafe { self.enable(cap) };
        } else {
            unsafe { self.disable(cap) };
        }
    }

    fn blend_func(&self, src: u32, dst: u32) {
        unsafe { <Self as HasContext>::blend_func(self, src, dst) };
    }

    fn viewport(&self, vp: [i32; 4]) {
        unsafe { <Self as HasContext>::viewport(self, vp[0], vp[1], vp[2], vp[3]) };
    }

    fn active_texture(&self, unit: u32) {
        unsafe { <Self as HasContext>::active_texture(self, unit) };
    }

    fn bind_texture_2d(&self, tex: u32) {
        unsafe {
            <Self as HasContext>::bind_texture(
                self,
                glow::TEXTURE_2D,
                NonZeroU32::new(tex).map(glow::NativeTexture),
            );
        }
    }
}

/// Switches to `GL_TEXTURE0`, saves `TEXTURE0`'s 2D binding, executes `draw`, and restores `TEXTURE0` before restoring `ACTIVE_TEXTURE`.
pub fn with_overlay_texture_unit<F: FnOnce()>(ops: &dyn GlStateOps, draw: F) {
    let prev_active_tex = ops.get_integerv(glow::ACTIVE_TEXTURE);
    ops.active_texture(glow::TEXTURE0);
    let prev_tex0 = ops.get_integerv(glow::TEXTURE_BINDING_2D);
    draw();
    ops.bind_texture_2d(prev_tex0.max(0) as u32);
    ops.active_texture(prev_active_tex.max(glow::TEXTURE0 as i32) as u32);
}

/// Converts a top-left origin HUD bounding box `(dst_x, dst_y, box_w, box_h)` into OpenGL's
/// bottom-left `glViewport` `[x, y, w, h]`, safely clamped to `[0..win_w, 0..win_h]`.
#[must_use]
pub fn compute_gl_overlay_viewport(
    win_w: u32,
    win_h: u32,
    dst_x: i32,
    dst_y: i32,
    box_w: u32,
    box_h: u32,
) -> [i32; 4] {
    let max_w = win_w.max(1);
    let max_h = win_h.max(1);
    let w = box_w.clamp(1, max_w);
    let h = box_h.clamp(1, max_h);
    let x = dst_x.clamp(0, (max_w.saturating_sub(w)) as i32);
    let top_y = dst_y.clamp(0, (max_h.saturating_sub(h)) as i32);
    let gl_y = (max_h as i32) - top_y - (h as i32);
    [x, gl_y.max(0), w as i32, h as i32]
}

/// Returns `true` when the active OpenGL context pointer changed, requiring handle re-creation.
#[must_use]
pub const fn should_reset_gl_renderer(cached_ctx: usize, current_ctx: usize) -> bool {
    cached_ctx != 0 && current_ctx != 0 && cached_ctx != current_ctx
}

unsafe fn query_current_gl_context(
    api_name: &str,
    proc_lookup: unsafe extern "C" fn(*const c_char) -> *const c_void,
) -> usize {
    let sym_name = if api_name.contains("EGL") {
        c"eglGetCurrentContext"
    } else {
        c"glXGetCurrentContext"
    };
    let sym = unsafe { proc_lookup(sym_name.as_ptr()) };
    if sym.is_null() {
        return 0;
    }
    let get_ctx: unsafe extern "C" fn() -> *mut c_void = unsafe { std::mem::transmute(sym) };
    (unsafe { get_ctx() }) as usize
}

/// Bounded per-context cache so alternating live OpenGL contexts retain their own handles
/// without leaking or recompiling shaders on every context switch.
pub struct GlContextCache<T> {
    map: HashMap<usize, T>,
    max_entries: usize,
}

impl<T: Default> GlContextCache<T> {
    #[must_use]
    pub fn new(max_entries: usize) -> Self {
        Self {
            map: HashMap::new(),
            max_entries: max_entries.max(1),
        }
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.map.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }

    pub fn entry_or_default(&mut self, ctx_id: usize) -> &mut T {
        if !self.map.contains_key(&ctx_id) && self.map.len() >= self.max_entries {
            self.map.clear();
        }
        self.map.entry(ctx_id).or_default()
    }
}

#[derive(Default)]
struct GlOverlayRenderer {
    program: Option<glow::Program>,
    vao: Option<glow::VertexArray>,
    texture: Option<glow::Texture>,
    sampler_loc: Option<glow::UniformLocation>,
    tex_w: u32,
    tex_h: u32,
    uploaded_generation: u64,
    initialized: bool,
}

static GL_RENDERERS: OnceLock<Mutex<GlContextCache<GlOverlayRenderer>>> = OnceLock::new();

thread_local! {
    static THREAD_GL_CTX: RefCell<Option<(&'static str, glow::Context)>> = const { RefCell::new(None) };
}

fn is_parseable_gl_version(mut src: &str) -> bool {
    let webgl_sig = "WebGL ";
    if src.starts_with(webgl_sig) {
        let pos = src.rfind(webgl_sig).unwrap_or(0);
        src = &src[pos + webgl_sig.len()..];
    } else if let Some(pos) = src.rfind(" ES ") {
        src = &src[pos + " ES ".len()..];
    }
    if let Some(pos) = src.find("GLSL ES ") {
        src = &src[pos + "GLSL ES ".len()..];
    }
    let version = src.split_once(' ').map_or(src, |(v, _)| v);
    let mut it = version.split('.');
    let major_ok = it.next().is_some_and(|s| s.parse::<u32>().is_ok());
    let minor_ok = it.next().is_some_and(|s| {
        let trimmed = if s.starts_with('0') {
            "0"
        } else {
            s.trim_end_matches('0')
        };
        trimmed.parse::<u32>().is_ok()
    });
    major_ok && minor_ok
}

unsafe fn has_active_gl_context(
    proc_lookup: unsafe extern "C" fn(*const c_char) -> *const c_void,
) -> bool {
    let sym = unsafe { proc_lookup(c"glGetString".as_ptr()) };
    if sym.is_null() {
        return false;
    }
    let get_string: unsafe extern "C" fn(u32) -> *const c_char =
        unsafe { std::mem::transmute(sym) };
    let ver = unsafe { get_string(glow::VERSION) };
    if ver.is_null() {
        return false;
    }
    // SAFETY: `glGetString(GL_VERSION)` returns a static NUL-terminated string while context is current.
    unsafe { CStr::from_ptr(ver) }
        .to_str()
        .is_ok_and(is_parseable_gl_version)
}

fn try_with_thread_gl_context<R>(
    api_name: &'static str,
    proc_lookup: unsafe extern "C" fn(*const c_char) -> *const c_void,
    f: impl FnOnce(&glow::Context) -> R,
) -> Option<R> {
    if !unsafe { has_active_gl_context(proc_lookup) } {
        return None;
    }
    THREAD_GL_CTX.with(|cell| {
        {
            let mut slot = cell.try_borrow_mut().ok()?;
            let needs_init = !matches!(&*slot, Some((cached_api, _)) if *cached_api == api_name);
            if needs_init {
                let ctx = unsafe {
                    glow::Context::from_loader_function_cstr(|sym| proc_lookup(sym.as_ptr()))
                };
                *slot = Some((api_name, ctx));
            }
        }
        let slot = cell.try_borrow().ok()?;
        let (_, gl) = slot.as_ref()?;
        Some(f(gl))
    })
}

pub fn try_with_thread_gl_context_for_test(
    api_name: &'static str,
    proc_lookup: unsafe extern "C" fn(*const c_char) -> *const c_void,
) -> Option<()> {
    try_with_thread_gl_context(api_name, proc_lookup, |_| ())
}

impl GlOverlayRenderer {
    fn compile_shader(gl: &glow::Context, kind: u32, src: &str) -> Option<glow::Shader> {
        let shader = unsafe { gl.create_shader(kind) }.ok()?;
        unsafe {
            gl.shader_source(shader, src);
            gl.compile_shader(shader);
            if !gl.get_shader_compile_status(shader) {
                gl.delete_shader(shader);
                return None;
            }
        }
        Some(shader)
    }

    fn ensure_initialized(&mut self, gl: &glow::Context) -> bool {
        if self.initialized {
            return true;
        }
        let ver_str = unsafe { gl.get_parameter_string(glow::VERSION) };
        let is_es = parse_gl_version_is_es(&ver_str);
        let (v_src, f_src) = select_gl_quad_shaders(is_es);

        let Some(vs) = Self::compile_shader(gl, glow::VERTEX_SHADER, v_src) else {
            return false;
        };
        let Some(fs) = Self::compile_shader(gl, glow::FRAGMENT_SHADER, f_src) else {
            unsafe { gl.delete_shader(vs) };
            return false;
        };

        let Ok(prog) = (unsafe { gl.create_program() }) else {
            unsafe {
                gl.delete_shader(vs);
                gl.delete_shader(fs);
            }
            return false;
        };

        unsafe {
            gl.attach_shader(prog, vs);
            gl.attach_shader(prog, fs);
            gl.link_program(prog);
            gl.delete_shader(vs);
            gl.delete_shader(fs);
            if !gl.get_program_link_status(prog) {
                gl.delete_program(prog);
                return false;
            }
        }

        let vao = unsafe { gl.create_vertex_array() }.ok();
        let texture = unsafe { gl.create_texture() }.ok();
        if let Some(tex) = texture {
            unsafe {
                gl.bind_texture(glow::TEXTURE_2D, Some(tex));
                gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MIN_FILTER, glow::LINEAR as i32);
                gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MAG_FILTER, glow::LINEAR as i32);
                gl.tex_parameter_i32(
                    glow::TEXTURE_2D,
                    glow::TEXTURE_WRAP_S,
                    glow::CLAMP_TO_EDGE as i32,
                );
                gl.tex_parameter_i32(
                    glow::TEXTURE_2D,
                    glow::TEXTURE_WRAP_T,
                    glow::CLAMP_TO_EDGE as i32,
                );
            }
        }

        self.sampler_loc = unsafe { gl.get_uniform_location(prog, "u_hud_tex") };
        self.program = Some(prog);
        self.vao = vao;
        self.texture = texture;
        self.initialized = true;
        true
    }
}

fn capture_gl_screenshot(gl: &glow::Context, width: u32, height: u32) {
    let prev_read_fbo = gl.get_integerv(glow::READ_FRAMEBUFFER_BINDING);
    let prev_pack_pbo = gl.get_integerv(glow::PIXEL_PACK_BUFFER_BINDING);
    let prev_align = gl.get_integerv(glow::PACK_ALIGNMENT);
    let prev_row_len = gl.get_integerv(glow::PACK_ROW_LENGTH);

    unsafe {
        if prev_read_fbo != 0 {
            HasContext::bind_framebuffer(gl, glow::READ_FRAMEBUFFER, None);
        }
        if prev_pack_pbo != 0 {
            gl.bind_buffer(glow::PIXEL_PACK_BUFFER, None);
        }
        gl.pixel_store_i32(glow::PACK_ALIGNMENT, 4);
        gl.pixel_store_i32(glow::PACK_ROW_LENGTH, 0);
    }

    let len = (width as usize).saturating_mul(height as usize).saturating_mul(4);
    let mut raw = vec![0_u8; len];
    unsafe {
        gl.read_pixels(
            0,
            0,
            width as i32,
            height as i32,
            glow::RGBA,
            glow::UNSIGNED_BYTE,
            glow::PixelPackData::Slice(Some(&mut raw)),
        );
    }

    if let Some(capture) = build_gl_readpixels_capture(width, height, raw)
        && let Ok(rt) = LayerRuntime::global().lock()
    {
        rt.submit_captured_screenshot(
            capture.width,
            capture.height,
            PixelFormat::Rgba8,
            capture.pixels,
            true,
        );
    }

    unsafe {
        if prev_read_fbo != 0 {
            HasContext::bind_framebuffer(
                gl,
                glow::READ_FRAMEBUFFER,
                NonZeroU32::new(prev_read_fbo.max(0) as u32).map(glow::NativeFramebuffer),
            );
        }
        gl.pixel_store_i32(glow::PACK_ALIGNMENT, prev_align.max(1));
        gl.pixel_store_i32(glow::PACK_ROW_LENGTH, prev_row_len.max(0));
        if prev_pack_pbo != 0 {
            gl.bind_buffer(
                glow::PIXEL_PACK_BUFFER,
                NonZeroU32::new(prev_pack_pbo.max(0) as u32).map(glow::NativeBuffer),
            );
        }
    }
}

/// Pumps telemetry/IPC and draws Pure-Rust CPU RGBA overlay onto current OpenGL context before swap.
///
/// # Safety
/// Must be called from the active OpenGL thread inside `glXSwapBuffers` or `eglSwapBuffers`.
pub(crate) unsafe fn on_swap_buffers(
    proc_lookup: unsafe extern "C" fn(*const c_char) -> *const c_void,
    hint_w: u32,
    hint_h: u32,
    api_name: &'static str,
) {
    let enabled = LayerRuntime::global()
        .lock()
        .ok()
        .is_some_and(|rt| rt.config.enabled);
    if !enabled {
        return;
    }

    try_with_thread_gl_context(api_name, proc_lookup, |gl| {
        let (w, h) = if hint_w > 0 && hint_h > 0 {
            (hint_w, hint_h)
        } else {
            let vp = gl.get_viewport();
            (
                if hint_w > 0 { hint_w } else { vp[2].max(0) as u32 },
                if hint_h > 0 { hint_h } else { vp[3].max(0) as u32 },
            )
        };
        if w == 0 || h == 0 {
            return;
        }

        let (draw_overlay, take_shot, overlay) = {
            let Ok(mut rt) = LayerRuntime::global().lock() else {
                return;
            };
            if !rt.config.enabled {
                return;
            }
            rt.pump_frame(w, h, api_name);
            let shot = rt.consume_screenshot_request();
            let show_overlay = rt.config.hud_visible
                || (rt.config.gui_open && !rt.input.is_external_overlay_active());
            let ov = if show_overlay {
                let l = &rt.overlay_sync.latest;
                (!l.rgba.is_empty()).then(|| {
                    (
                        l.dst_x,
                        l.dst_y,
                        l.width,
                        l.height,
                        l.generation,
                        l.rgba.clone(),
                    )
                })
            } else {
                None
            };
            (show_overlay, shot, ov)
        };

        if let Some(false) = take_shot {
            capture_gl_screenshot(gl, w, h);
        }

        if draw_overlay && let Some((dst_x, dst_y, ow, oh, ogen, rgba)) = overlay {
            let backup = GlStateBackup::capture(gl);
            let prev_row_len = gl.get_integerv(glow::UNPACK_ROW_LENGTH);
            let prev_align = gl.get_integerv(glow::UNPACK_ALIGNMENT);
            let prev_depth = GlStateOps::is_enabled(gl, glow::DEPTH_TEST);
            let prev_cull = GlStateOps::is_enabled(gl, glow::CULL_FACE);

            let cur_ctx = unsafe { query_current_gl_context(api_name, proc_lookup) };
            let renderers = GL_RENDERERS.get_or_init(|| Mutex::new(GlContextCache::new(8)));

            with_overlay_texture_unit(gl, || {
                if let Ok(mut cache) = renderers.lock() {
                    let gr = cache.entry_or_default(cur_ctx);
                    if gr.ensure_initialized(gl) {
                    GlStateOps::bind_framebuffer(gl, 0);
                    gl.bind_unpack_buffer(0);
                    unsafe {
                        gl.pixel_store_i32(glow::UNPACK_ROW_LENGTH, 0);
                        gl.pixel_store_i32(glow::UNPACK_ALIGNMENT, 4);
                        gl.bind_texture(glow::TEXTURE_2D, gr.texture);
                    }

                    let expected = (ow as usize).saturating_mul(oh as usize).saturating_mul(4);
                    if rgba.len() >= expected && expected > 0 {
                        let slice = &rgba[..expected];
                        if gr.tex_w != ow || gr.tex_h != oh {
                            unsafe {
                                gl.tex_image_2d(
                                    glow::TEXTURE_2D,
                                    0,
                                    glow::RGBA8 as i32,
                                    ow as i32,
                                    oh as i32,
                                    0,
                                    glow::RGBA,
                                    glow::UNSIGNED_BYTE,
                                    glow::PixelUnpackData::Slice(Some(slice)),
                                );
                            }
                            gr.tex_w = ow;
                            gr.tex_h = oh;
                            gr.uploaded_generation = ogen;
                        } else if gr.uploaded_generation != ogen {
                            unsafe {
                                gl.tex_sub_image_2d(
                                    glow::TEXTURE_2D,
                                    0,
                                    0,
                                    0,
                                    ow as i32,
                                    oh as i32,
                                    glow::RGBA,
                                    glow::UNSIGNED_BYTE,
                                    glow::PixelUnpackData::Slice(Some(slice)),
                                );
                            }
                            gr.uploaded_generation = ogen;
                        }
                    }

                    gl.set_enabled(glow::SCISSOR_TEST, false);
                    gl.set_enabled(glow::DEPTH_TEST, false);
                    gl.set_enabled(glow::CULL_FACE, false);
                    gl.set_enabled(glow::BLEND, true);
                    GlStateOps::blend_func(gl, glow::ONE, glow::ONE_MINUS_SRC_ALPHA);
                    GlStateOps::viewport(
                        gl,
                        compute_gl_overlay_viewport(w, h, dst_x, dst_y, ow, oh),
                    );
                    unsafe {
                        <glow::Context as HasContext>::use_program(gl, gr.program);
                        <glow::Context as HasContext>::bind_vertex_array(gl, gr.vao);
                        gl.uniform_1_i32(gr.sampler_loc.as_ref(), 0);
                        gl.draw_arrays(glow::TRIANGLES, 0, 3);
                    }
                    }
                }
            });

            unsafe {
                gl.pixel_store_i32(glow::UNPACK_ROW_LENGTH, prev_row_len.max(0));
                gl.pixel_store_i32(glow::UNPACK_ALIGNMENT, prev_align.max(1));
            }
            gl.set_enabled(glow::DEPTH_TEST, prev_depth);
            gl.set_enabled(glow::CULL_FACE, prev_cull);
            backup.restore(gl);
        }

        if let Some(true) = take_shot {
            capture_gl_screenshot(gl, w, h);
        }
    });
}
