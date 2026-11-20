use crate::runtime::LayerRuntime;
use fpshud_screenshot::{PixelFormat, RawFrameCapture};
use std::ffi::{CStr, c_char, c_int, c_void};
use std::sync::{Mutex, OnceLock};

pub const GL_FALSE: u8 = 0;
pub const GL_TRUE: u8 = 1;
pub const GL_TRIANGLES: u32 = 0x0004;
pub const GL_ONE: u32 = 1;
pub const GL_ZERO: u32 = 0;
pub const GL_ONE_MINUS_SRC_ALPHA: u32 = 0x0303;
pub const GL_BLEND: u32 = 0x0BE2;
pub const GL_CULL_FACE: u32 = 0x0B44;
pub const GL_DEPTH_TEST: u32 = 0x0B71;
pub const GL_SCISSOR_TEST: u32 = 0x0C11;
pub const GL_TEXTURE_2D: u32 = 0x0DE1;
pub const GL_UNSIGNED_BYTE: u32 = 0x1401;
pub const GL_RGBA: u32 = 0x1908;
pub const GL_RGBA8: u32 = 0x8058;
pub const GL_LINEAR: i32 = 0x2601;
pub const GL_TEXTURE_MAG_FILTER: u32 = 0x2800;
pub const GL_TEXTURE_MIN_FILTER: u32 = 0x2801;
pub const GL_TEXTURE_WRAP_S: u32 = 0x2802;
pub const GL_TEXTURE_WRAP_T: u32 = 0x2803;
pub const GL_CLAMP_TO_EDGE: i32 = 0x812F;
pub const GL_VIEWPORT: u32 = 0x0BA2;
pub const GL_UNPACK_ROW_LENGTH: u32 = 0x0CF2;
pub const GL_UNPACK_ALIGNMENT: u32 = 0x0CF5;
pub const GL_PACK_ROW_LENGTH: u32 = 0x0D02;
pub const GL_PACK_ALIGNMENT: u32 = 0x0D05;
pub const GL_BLEND_SRC_RGB: u32 = 0x80C8;
pub const GL_BLEND_DST_RGB: u32 = 0x80C9;
pub const GL_VERSION: u32 = 0x1F02;
pub const GL_TEXTURE0: u32 = 0x84C0;
pub const GL_ACTIVE_TEXTURE: u32 = 0x84E0;
pub const GL_ARRAY_BUFFER_BINDING: u32 = 0x8894;
pub const GL_VERTEX_ARRAY_BINDING: u32 = 0x85B5;
pub const GL_PIXEL_PACK_BUFFER: u32 = 0x88EB;
pub const GL_PIXEL_UNPACK_BUFFER: u32 = 0x88EC;
pub const GL_PIXEL_PACK_BUFFER_BINDING: u32 = 0x88ED;
pub const GL_PIXEL_UNPACK_BUFFER_BINDING: u32 = 0x88EF;
pub const GL_DRAW_FRAMEBUFFER_BINDING: u32 = 0x8CA6;
pub const GL_READ_FRAMEBUFFER_BINDING: u32 = 0x8CAA;
pub const GL_DRAW_FRAMEBUFFER: u32 = 0x8CA9;
pub const GL_READ_FRAMEBUFFER: u32 = 0x8CA8;
pub const GL_CURRENT_PROGRAM: u32 = 0x8B8D;
pub const GL_FRAGMENT_SHADER: u32 = 0x8B30;
pub const GL_VERTEX_SHADER: u32 = 0x8B31;
pub const GL_COMPILE_STATUS: u32 = 0x8B81;
pub const GL_LINK_STATUS: u32 = 0x8B82;

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
            prev_program: ops.get_integerv(GL_CURRENT_PROGRAM),
            prev_vao: ops.get_integerv(GL_VERTEX_ARRAY_BINDING),
            prev_fbo: ops.get_integerv(GL_DRAW_FRAMEBUFFER_BINDING),
            unpack_pbo: ops.get_integerv(GL_PIXEL_UNPACK_BUFFER_BINDING),
            blend_enabled: ops.is_enabled(GL_BLEND),
            blend_src_rgb: ops.get_integerv(GL_BLEND_SRC_RGB) as u32,
            blend_dst_rgb: ops.get_integerv(GL_BLEND_DST_RGB) as u32,
            scissor_enabled: ops.is_enabled(GL_SCISSOR_TEST),
            viewport: ops.get_viewport(),
        }
    }

    pub fn restore(&self, ops: &dyn GlStateOps) {
        ops.use_program(self.prev_program.max(0) as u32);
        ops.bind_vertex_array(self.prev_vao.max(0) as u32);
        ops.bind_framebuffer(self.prev_fbo.max(0) as u32);
        ops.bind_unpack_buffer(self.unpack_pbo.max(0) as u32);
        ops.set_enabled(GL_BLEND, self.blend_enabled);
        ops.blend_func(self.blend_src_rgb, self.blend_dst_rgb);
        ops.set_enabled(GL_SCISSOR_TEST, self.scissor_enabled);
        ops.viewport(self.viewport);
    }
}

#[allow(clippy::type_complexity)]
struct GlProcs {
    get_string: Option<unsafe extern "C" fn(u32) -> *const u8>,
    get_integerv: Option<unsafe extern "C" fn(u32, *mut i32)>,
    is_enabled: Option<unsafe extern "C" fn(u32) -> u8>,
    enable: Option<unsafe extern "C" fn(u32)>,
    disable: Option<unsafe extern "C" fn(u32)>,
    blend_func: Option<unsafe extern "C" fn(u32, u32)>,
    viewport: Option<unsafe extern "C" fn(i32, i32, i32, i32)>,
    active_texture: Option<unsafe extern "C" fn(u32)>,
    gen_textures: Option<unsafe extern "C" fn(i32, *mut u32)>,
    bind_texture: Option<unsafe extern "C" fn(u32, u32)>,
    tex_parameteri: Option<unsafe extern "C" fn(u32, u32, i32)>,
    tex_image_2d: Option<unsafe extern "C" fn(u32, i32, i32, i32, i32, i32, u32, u32, *const c_void)>,
    tex_sub_image_2d: Option<unsafe extern "C" fn(u32, i32, i32, i32, i32, i32, u32, u32, *const c_void)>,
    pixel_storei: Option<unsafe extern "C" fn(u32, i32)>,
    bind_buffer: Option<unsafe extern "C" fn(u32, u32)>,
    bind_framebuffer: Option<unsafe extern "C" fn(u32, u32)>,
    gen_vertex_arrays: Option<unsafe extern "C" fn(i32, *mut u32)>,
    bind_vertex_array: Option<unsafe extern "C" fn(u32)>,
    create_shader: Option<unsafe extern "C" fn(u32) -> u32>,
    shader_source: Option<unsafe extern "C" fn(u32, i32, *const *const c_char, *const i32)>,
    compile_shader: Option<unsafe extern "C" fn(u32)>,
    get_shaderiv: Option<unsafe extern "C" fn(u32, u32, *mut i32)>,
    delete_shader: Option<unsafe extern "C" fn(u32)>,
    create_program: Option<unsafe extern "C" fn() -> u32>,
    attach_shader: Option<unsafe extern "C" fn(u32, u32)>,
    link_program: Option<unsafe extern "C" fn(u32)>,
    get_programiv: Option<unsafe extern "C" fn(u32, u32, *mut i32)>,
    use_program: Option<unsafe extern "C" fn(u32)>,
    get_uniform_location: Option<unsafe extern "C" fn(u32, *const c_char) -> c_int>,
    uniform1i: Option<unsafe extern "C" fn(c_int, i32)>,
    draw_arrays: Option<unsafe extern "C" fn(u32, i32, i32)>,
    read_pixels: Option<unsafe extern "C" fn(i32, i32, i32, i32, u32, u32, *mut c_void)>,
}

unsafe impl Send for GlProcs {}
unsafe impl Sync for GlProcs {}

unsafe fn resolve_gl_sym<T>(
    proc_lookup: Option<unsafe extern "C" fn(*const c_char) -> *const c_void>,
    sym: &CStr,
) -> Option<T> {
    let mut ptr = proc_lookup.map_or(std::ptr::null(), |f| unsafe { f(sym.as_ptr()) });
    if ptr.is_null() {
        ptr = unsafe { libc::dlsym(libc::RTLD_NEXT, sym.as_ptr()) };
    }
    if ptr.is_null() {
        None
    } else {
        Some(unsafe { std::mem::transmute_copy::<*const c_void, T>(&ptr) })
    }
}

impl GlProcs {
    unsafe fn load(proc_lookup: Option<unsafe extern "C" fn(*const c_char) -> *const c_void>) -> Self {
        unsafe {
            Self {
                get_string: resolve_gl_sym(proc_lookup, c"glGetString"),
                get_integerv: resolve_gl_sym(proc_lookup, c"glGetIntegerv"),
                is_enabled: resolve_gl_sym(proc_lookup, c"glIsEnabled"),
                enable: resolve_gl_sym(proc_lookup, c"glEnable"),
                disable: resolve_gl_sym(proc_lookup, c"glDisable"),
                blend_func: resolve_gl_sym(proc_lookup, c"glBlendFunc"),
                viewport: resolve_gl_sym(proc_lookup, c"glViewport"),
                active_texture: resolve_gl_sym(proc_lookup, c"glActiveTexture"),
                gen_textures: resolve_gl_sym(proc_lookup, c"glGenTextures"),
                bind_texture: resolve_gl_sym(proc_lookup, c"glBindTexture"),
                tex_parameteri: resolve_gl_sym(proc_lookup, c"glTexParameteri"),
                tex_image_2d: resolve_gl_sym(proc_lookup, c"glTexImage2D"),
                tex_sub_image_2d: resolve_gl_sym(proc_lookup, c"glTexSubImage2D"),
                pixel_storei: resolve_gl_sym(proc_lookup, c"glPixelStorei"),
                bind_buffer: resolve_gl_sym(proc_lookup, c"glBindBuffer"),
                bind_framebuffer: resolve_gl_sym(proc_lookup, c"glBindFramebuffer"),
                gen_vertex_arrays: resolve_gl_sym(proc_lookup, c"glGenVertexArrays"),
                bind_vertex_array: resolve_gl_sym(proc_lookup, c"glBindVertexArray"),
                create_shader: resolve_gl_sym(proc_lookup, c"glCreateShader"),
                shader_source: resolve_gl_sym(proc_lookup, c"glShaderSource"),
                compile_shader: resolve_gl_sym(proc_lookup, c"glCompileShader"),
                get_shaderiv: resolve_gl_sym(proc_lookup, c"glGetShaderiv"),
                delete_shader: resolve_gl_sym(proc_lookup, c"glDeleteShader"),
                create_program: resolve_gl_sym(proc_lookup, c"glCreateProgram"),
                attach_shader: resolve_gl_sym(proc_lookup, c"glAttachShader"),
                link_program: resolve_gl_sym(proc_lookup, c"glLinkProgram"),
                get_programiv: resolve_gl_sym(proc_lookup, c"glGetProgramiv"),
                use_program: resolve_gl_sym(proc_lookup, c"glUseProgram"),
                get_uniform_location: resolve_gl_sym(proc_lookup, c"glGetUniformLocation"),
                uniform1i: resolve_gl_sym(proc_lookup, c"glUniform1i"),
                draw_arrays: resolve_gl_sym(proc_lookup, c"glDrawArrays"),
                read_pixels: resolve_gl_sym(proc_lookup, c"glReadPixels"),
            }
        }
    }
}

impl GlStateOps for GlProcs {
    fn get_integerv(&self, pname: u32) -> i32 {
        let mut out = 0_i32;
        if let Some(f) = self.get_integerv {
            unsafe { f(pname, &mut out) };
        }
        out
    }

    fn get_viewport(&self) -> [i32; 4] {
        let mut vp = [0_i32; 4];
        if let Some(f) = self.get_integerv {
            unsafe { f(GL_VIEWPORT, vp.as_mut_ptr()) };
        }
        vp
    }

    fn is_enabled(&self, cap: u32) -> bool {
        self.is_enabled
            .is_some_and(|f| unsafe { f(cap) } != GL_FALSE)
    }

    fn use_program(&self, prog: u32) {
        if let Some(f) = self.use_program {
            unsafe { f(prog) };
        }
    }

    fn bind_vertex_array(&self, vao: u32) {
        if let Some(f) = self.bind_vertex_array {
            unsafe { f(vao) };
        }
    }

    fn bind_framebuffer(&self, fbo: u32) {
        if let Some(f) = self.bind_framebuffer {
            unsafe { f(GL_DRAW_FRAMEBUFFER, fbo) };
        }
    }

    fn bind_unpack_buffer(&self, buf: u32) {
        if let Some(f) = self.bind_buffer {
            unsafe { f(GL_PIXEL_UNPACK_BUFFER, buf) };
        }
    }

    fn set_enabled(&self, cap: u32, enable: bool) {
        if enable {
            if let Some(f) = self.enable {
                unsafe { f(cap) };
            }
        } else if let Some(f) = self.disable {
            unsafe { f(cap) };
        }
    }

    fn blend_func(&self, src: u32, dst: u32) {
        if let Some(f) = self.blend_func {
            unsafe { f(src, dst) };
        }
    }

    fn viewport(&self, vp: [i32; 4]) {
        if let Some(f) = self.viewport {
            unsafe { f(vp[0], vp[1], vp[2], vp[3]) };
        }
    }
}

struct GlOverlayRenderer {
    program: u32,
    vao: u32,
    texture: u32,
    sampler_loc: c_int,
    tex_w: u32,
    tex_h: u32,
    uploaded_generation: u64,
    initialized: bool,
}

static GL_RENDERER: OnceLock<Mutex<GlOverlayRenderer>> = OnceLock::new();

impl GlOverlayRenderer {
    fn compile_shader(procs: &GlProcs, kind: u32, src: &str) -> Option<u32> {
        let shader = unsafe { procs.create_shader?(kind) };
        if shader == 0 {
            return None;
        }
        let ptr = src.as_ptr().cast::<c_char>();
        let len = src.len() as i32;
        unsafe {
            procs.shader_source?(shader, 1, &ptr, &len);
            procs.compile_shader?(shader);
            let mut status = 0_i32;
            procs.get_shaderiv?(shader, GL_COMPILE_STATUS, &mut status);
            if status == 0 {
                procs.delete_shader?(shader);
                return None;
            }
        }
        Some(shader)
    }

    fn ensure_initialized(&mut self, procs: &GlProcs) -> bool {
        if self.initialized {
            return true;
        }
        let ver_str = procs
            .get_string
            .map(|f| unsafe { f(GL_VERSION) })
            .filter(|p| !p.is_null())
            .map(|p| unsafe { CStr::from_ptr(p.cast::<c_char>()) }.to_string_lossy().into_owned())
            .unwrap_or_default();
        let is_es = parse_gl_version_is_es(&ver_str);
        let (v_src, f_src) = select_gl_quad_shaders(is_es);

        let Some(vs) = Self::compile_shader(procs, GL_VERTEX_SHADER, v_src) else {
            return false;
        };
        let Some(fs) = Self::compile_shader(procs, GL_FRAGMENT_SHADER, f_src) else {
            if let Some(del) = procs.delete_shader {
                unsafe { del(vs) };
            }
            return false;
        };

        let prog = procs.create_program.map_or(0, |f| unsafe { f() });
        if prog == 0 {
            return false;
        }
        if let (Some(attach), Some(link), Some(get_p), Some(del)) = (
            procs.attach_shader,
            procs.link_program,
            procs.get_programiv,
            procs.delete_shader,
        ) {
            unsafe {
                attach(prog, vs);
                attach(prog, fs);
                link(prog);
                del(vs);
                del(fs);
                let mut ok = 0_i32;
                get_p(prog, GL_LINK_STATUS, &mut ok);
                if ok == 0 {
                    return false;
                }
            }
        }

        let mut vao = 0_u32;
        if let Some(gen_vao) = procs.gen_vertex_arrays {
            unsafe { gen_vao(1, &mut vao) };
        }

        let mut tex = 0_u32;
        if let (Some(gen_tex), Some(bind_tex), Some(tex_p)) =
            (procs.gen_textures, procs.bind_texture, procs.tex_parameteri)
        {
            unsafe {
                gen_tex(1, &mut tex);
                bind_tex(GL_TEXTURE_2D, tex);
                tex_p(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                tex_p(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                tex_p(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                tex_p(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
        }

        self.sampler_loc = procs
            .get_uniform_location
            .map_or(-1, |f| unsafe { f(prog, c"u_hud_tex".as_ptr()) });
        self.program = prog;
        self.vao = vao;
        self.texture = tex;
        self.initialized = true;
        true
    }
}

fn capture_gl_screenshot(procs: &GlProcs, width: u32, height: u32) {
    let Some(read_pixels) = procs.read_pixels else {
        return;
    };
    let prev_read_fbo = procs.get_integerv(GL_READ_FRAMEBUFFER_BINDING);
    let prev_pack_pbo = procs.get_integerv(GL_PIXEL_PACK_BUFFER_BINDING);
    let prev_align = procs.get_integerv(GL_PACK_ALIGNMENT);
    let prev_row_len = procs.get_integerv(GL_PACK_ROW_LENGTH);

    if let Some(bind_fbo) = procs.bind_framebuffer
        && prev_read_fbo != 0
    {
        unsafe { bind_fbo(GL_READ_FRAMEBUFFER, 0) };
    }
    if let Some(bind_buf) = procs.bind_buffer
        && prev_pack_pbo != 0
    {
        unsafe { bind_buf(GL_PIXEL_PACK_BUFFER, 0) };
    }
    if let Some(store) = procs.pixel_storei {
        unsafe {
            store(GL_PACK_ALIGNMENT, 4);
            store(GL_PACK_ROW_LENGTH, 0);
        }
    }

    let len = (width as usize).saturating_mul(height as usize).saturating_mul(4);
    let mut raw = vec![0_u8; len];
    unsafe {
        read_pixels(
            0,
            0,
            width as i32,
            height as i32,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            raw.as_mut_ptr().cast::<c_void>(),
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

    if let Some(bind_fbo) = procs.bind_framebuffer
        && prev_read_fbo != 0
    {
        unsafe { bind_fbo(GL_READ_FRAMEBUFFER, prev_read_fbo.max(0) as u32) };
    }
    if let Some(store) = procs.pixel_storei {
        unsafe {
            store(GL_PACK_ALIGNMENT, prev_align.max(1));
            store(GL_PACK_ROW_LENGTH, prev_row_len.max(0));
        }
    }
    if let Some(bind_buf) = procs.bind_buffer
        && prev_pack_pbo != 0
    {
        unsafe { bind_buf(GL_PIXEL_PACK_BUFFER, prev_pack_pbo.max(0) as u32) };
    }
}

/// Pumps telemetry/IPC and draws GPUI-CE RGBA overlay onto current OpenGL context before swap.
///
/// # Safety
/// Must be called from the active OpenGL thread inside `glXSwapBuffers` or `eglSwapBuffers`.
pub(crate) unsafe fn on_swap_buffers(
    proc_lookup: Option<unsafe extern "C" fn(*const c_char) -> *const c_void>,
    hint_w: u32,
    hint_h: u32,
    api_name: &str,
) {
    let procs = unsafe { GlProcs::load(proc_lookup) };
    let vp = procs.get_viewport();
    let w = if hint_w > 0 { hint_w } else { vp[2].max(0) as u32 };
    let h = if hint_h > 0 { hint_h } else { vp[3].max(0) as u32 };
    if w == 0 || h == 0 {
        return;
    }

    let (enabled, draw_overlay, take_shot, overlay) = {
        let Ok(mut rt) = LayerRuntime::global().lock() else {
            return;
        };
        if !rt.config.enabled {
            return;
        }
        rt.pump_frame(w, h, api_name);
        let shot = rt.consume_screenshot_request();
        let ov = if rt.config.hud_visible || rt.config.gui_open {
            let l = &rt.overlay_sync.latest;
            (!l.rgba.is_empty()).then(|| (l.width, l.height, l.generation, l.rgba.clone()))
        } else {
            None
        };
        (
            rt.config.enabled,
            rt.config.hud_visible || rt.config.gui_open,
            shot,
            ov,
        )
    };
    if !enabled {
        return;
    }

    if let Some(false) = take_shot {
        capture_gl_screenshot(&procs, w, h);
    }

    if draw_overlay && let Some((ow, oh, ogen, rgba)) = overlay {
        let backup = GlStateBackup::capture(&procs);
        let prev_active_tex = procs.get_integerv(GL_ACTIVE_TEXTURE);
        let prev_tex = procs.get_integerv(0x8069 /* GL_TEXTURE_BINDING_2D */);
        let prev_row_len = procs.get_integerv(GL_UNPACK_ROW_LENGTH);
        let prev_align = procs.get_integerv(GL_UNPACK_ALIGNMENT);
        let prev_depth = procs.is_enabled(GL_DEPTH_TEST);
        let prev_cull = procs.is_enabled(GL_CULL_FACE);

        let renderer = GL_RENDERER.get_or_init(|| {
            Mutex::new(GlOverlayRenderer {
                program: 0,
                vao: 0,
                texture: 0,
                sampler_loc: -1,
                tex_w: 0,
                tex_h: 0,
                uploaded_generation: 0,
                initialized: false,
            })
        });

        if let Ok(mut gr) = renderer.lock()
            && gr.ensure_initialized(&procs)
        {
            procs.bind_framebuffer(0);
            procs.bind_unpack_buffer(0);
            if let Some(store) = procs.pixel_storei {
                unsafe {
                    store(GL_UNPACK_ROW_LENGTH, 0);
                    store(GL_UNPACK_ALIGNMENT, 4);
                }
            }
            if let Some(act) = procs.active_texture {
                unsafe { act(GL_TEXTURE0) };
            }
            if let Some(bind_t) = procs.bind_texture {
                unsafe { bind_t(GL_TEXTURE_2D, gr.texture) };
            }

            let expected = (ow as usize).saturating_mul(oh as usize).saturating_mul(4);
            if rgba.len() >= expected && expected > 0 {
                if gr.tex_w != ow || gr.tex_h != oh {
                    if let Some(ti) = procs.tex_image_2d {
                        unsafe {
                            ti(
                                GL_TEXTURE_2D,
                                0,
                                GL_RGBA8 as i32,
                                ow as i32,
                                oh as i32,
                                0,
                                GL_RGBA,
                                GL_UNSIGNED_BYTE,
                                rgba.as_ptr().cast::<c_void>(),
                            );
                        }
                    }
                    gr.tex_w = ow;
                    gr.tex_h = oh;
                    gr.uploaded_generation = ogen;
                } else if gr.uploaded_generation != ogen {
                    if let Some(tsi) = procs.tex_sub_image_2d {
                        unsafe {
                            tsi(
                                GL_TEXTURE_2D,
                                0,
                                0,
                                0,
                                ow as i32,
                                oh as i32,
                                GL_RGBA,
                                GL_UNSIGNED_BYTE,
                                rgba.as_ptr().cast::<c_void>(),
                            );
                        }
                    }
                    gr.uploaded_generation = ogen;
                }
            }

            procs.set_enabled(GL_SCISSOR_TEST, false);
            procs.set_enabled(GL_DEPTH_TEST, false);
            procs.set_enabled(GL_CULL_FACE, false);
            procs.set_enabled(GL_BLEND, true);
            procs.blend_func(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            procs.viewport([0, 0, w as i32, h as i32]);
            procs.use_program(gr.program);
            procs.bind_vertex_array(gr.vao);
            if let Some(u1i) = procs.uniform1i
                && gr.sampler_loc >= 0
            {
                unsafe { u1i(gr.sampler_loc, 0) };
            }
            if let Some(draw) = procs.draw_arrays {
                unsafe { draw(GL_TRIANGLES, 0, 3) };
            }
        }

        if let Some(store) = procs.pixel_storei {
            unsafe {
                store(GL_UNPACK_ROW_LENGTH, prev_row_len.max(0));
                store(GL_UNPACK_ALIGNMENT, prev_align.max(1));
            }
        }
        if let Some(bind_t) = procs.bind_texture {
            unsafe { bind_t(GL_TEXTURE_2D, prev_tex.max(0) as u32) };
        }
        if let Some(act) = procs.active_texture {
            unsafe { act(prev_active_tex.max(GL_TEXTURE0 as i32) as u32) };
        }
        procs.set_enabled(GL_DEPTH_TEST, prev_depth);
        procs.set_enabled(GL_CULL_FACE, prev_cull);
        backup.restore(&procs);
    }

    if let Some(true) = take_shot {
        capture_gl_screenshot(&procs, w, h);
    }
}
