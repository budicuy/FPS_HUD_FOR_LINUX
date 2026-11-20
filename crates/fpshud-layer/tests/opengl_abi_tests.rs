use fpshud::opengl::blit::{
    GlStateBackup, GlStateOps, build_gl_readpixels_capture, parse_gl_version_is_es,
    select_gl_quad_shaders,
};
use fpshud_screenshot::PixelFormat;
use std::cell::RefCell;

#[derive(Default)]
struct MockGlOps {
    log: RefCell<Vec<(&'static str, i32)>>,
}

impl GlStateOps for MockGlOps {
    fn get_integerv(&self, pname: u32) -> i32 {
        match pname {
            0x8B8D => 7,      // GL_CURRENT_PROGRAM
            0x85B5 => 11,     // GL_VERTEX_ARRAY_BINDING
            0x8CA6 => 3,      // GL_DRAW_FRAMEBUFFER_BINDING
            0x88EF => 5,      // GL_PIXEL_UNPACK_BUFFER_BINDING
            0x80C9 => 0x0302, // GL_BLEND_SRC_RGB (Khronos GL spec)
            0x80C8 => 0x0303, // GL_BLEND_DST_RGB (Khronos GL spec)
            _ => 0,
        }
    }

    fn get_viewport(&self) -> [i32; 4] {
        [10, 20, 1280, 720]
    }

    fn is_enabled(&self, cap: u32) -> bool {
        match cap {
            0x0BE2 => true,  // GL_BLEND
            0x0C11 => true,  // GL_SCISSOR_TEST
            _ => false,
        }
    }

    fn use_program(&self, prog: u32) {
        self.log.borrow_mut().push(("use_program", prog as i32));
    }

    fn bind_vertex_array(&self, vao: u32) {
        self.log.borrow_mut().push(("bind_vao", vao as i32));
    }

    fn bind_framebuffer(&self, fbo: u32) {
        self.log.borrow_mut().push(("bind_fbo", fbo as i32));
    }

    fn bind_unpack_buffer(&self, buf: u32) {
        self.log.borrow_mut().push(("bind_unpack_pbo", buf as i32));
    }

    fn set_enabled(&self, cap: u32, enable: bool) {
        self.log
            .borrow_mut()
            .push(("set_enabled", ((cap as i32) << 1) | i32::from(enable)));
    }

    fn blend_func(&self, src: u32, dst: u32) {
        self.log.borrow_mut().push(("blend_src", src as i32));
        self.log.borrow_mut().push(("blend_dst", dst as i32));
    }

    fn viewport(&self, vp: [i32; 4]) {
        self.log.borrow_mut().push(("viewport_w", vp[2]));
        self.log.borrow_mut().push(("viewport_h", vp[3]));
    }

    fn active_texture(&self, unit: u32) {
        self.log.borrow_mut().push(("active_texture", unit as i32));
    }

    fn bind_texture_2d(&self, tex: u32) {
        self.log.borrow_mut().push(("bind_texture_2d", tex as i32));
    }
}

#[test]
fn glx_and_egl_get_proc_address_dispatch() {
    unsafe {
        let p_swap_glx = fpshud::glXGetProcAddress(c"glXSwapBuffers".as_ptr().cast::<u8>());
        assert_eq!(
            p_swap_glx as usize,
            fpshud::glXSwapBuffers as *const () as usize
        );

        let p_arb = fpshud::glXGetProcAddressARB(c"glXGetProcAddress".as_ptr().cast::<u8>());
        assert_eq!(
            p_arb as usize,
            fpshud::glXGetProcAddress as *const () as usize
        );

        let p_egl = fpshud::eglGetProcAddress(c"eglSwapBuffers".as_ptr());
        assert_eq!(p_egl as usize, fpshud::eglSwapBuffers as *const () as usize);

        // Simbol non-hook tidak boleh dipetakan ke hook kita
        let p_other = fpshud::glXGetProcAddress(c"glClearColor".as_ptr().cast::<u8>());
        assert_ne!(
            p_other as usize,
            fpshud::glXSwapBuffers as *const () as usize
        );
    }
}

#[test]
fn gl_version_es_detection_and_shader_selection() {
    assert!(!parse_gl_version_is_es("4.6 (Core Profile) Mesa 24.0"));
    assert!(parse_gl_version_is_es("OpenGL ES 3.2 Mesa 24.0"));
    assert!(!parse_gl_version_is_es(""));
    assert!(!parse_gl_version_is_es("corrupt_version_header"));

    let (desk_v, desk_f) = select_gl_quad_shaders(false);
    assert!(desk_v.contains("#version 130"));
    assert!(desk_f.contains("#version 130"));

    let (es_v, es_f) = select_gl_quad_shaders(true);
    assert!(es_v.contains("#version 300 es"));
    assert!(es_f.contains("#version 300 es"));
}

#[test]
fn gl_state_backup_and_readpixels_capture_wiring() {
    let ops = MockGlOps::default();
    let backup = GlStateBackup::capture(&ops);
    assert_eq!(backup.prev_program, 7);
    assert_eq!(backup.prev_vao, 11);
    assert_eq!(backup.prev_fbo, 3);
    assert_eq!(backup.unpack_pbo, 5);
    assert!(backup.blend_enabled);
    assert!(backup.scissor_enabled);
    assert_eq!(backup.blend_src_rgb, 0x0302);
    assert_eq!(backup.blend_dst_rgb, 0x0303);
    assert_eq!(backup.viewport, [10, 20, 1280, 720]);

    backup.restore(&ops);
    let log = ops.log.borrow();
    assert!(log.contains(&("use_program", 7)));
    assert!(log.contains(&("bind_vao", 11)));
    assert!(log.contains(&("bind_fbo", 3)));
    assert!(log.contains(&("bind_unpack_pbo", 5)));
    assert!(log.contains(&("set_enabled", (0x0BE2 << 1) | 1)));
    assert!(log.contains(&("set_enabled", (0x0C11 << 1) | 1)));
    assert!(log.contains(&("blend_src", 0x0302)));
    assert!(log.contains(&("blend_dst", 0x0303)));
    assert!(log.contains(&("viewport_w", 1280)));
    assert!(log.contains(&("viewport_h", 720)));

    let raw = vec![0xFF_u8; 4 * 2 * 4];
    let shot = build_gl_readpixels_capture(4, 2, raw).unwrap();
    assert_eq!(shot.width, 4);
    assert_eq!(shot.height, 2);
    assert_eq!(shot.format, PixelFormat::Rgba8);
    assert!(shot.flip_y);
}

#[test]
fn gl_context_probe_rejects_missing_or_corrupt_version_without_panic() {
    use fpshud::opengl::blit::try_with_thread_gl_context_for_test;
    use std::ffi::{c_char, c_void};

    unsafe extern "C" fn null_lookup(_name: *const c_char) -> *const c_void {
        std::ptr::null()
    }

    unsafe extern "C" fn null_get_string(_pname: u32) -> *const c_char {
        std::ptr::null()
    }
    unsafe extern "C" fn lookup_null_ver(name: *const c_char) -> *const c_void {
        let s = unsafe { std::ffi::CStr::from_ptr(name) }.to_bytes();
        if s == b"glGetString" {
            return null_get_string as *const c_void;
        }
        std::ptr::null()
    }

    unsafe extern "C" fn corrupt_get_string(_pname: u32) -> *const c_char {
        c"corrupt_no_gl_context".as_ptr()
    }
    unsafe extern "C" fn lookup_corrupt_ver(name: *const c_char) -> *const c_void {
        let s = unsafe { std::ffi::CStr::from_ptr(name) }.to_bytes();
        if s == b"glGetString" {
            return corrupt_get_string as *const c_void;
        }
        std::ptr::null()
    }

    assert!(try_with_thread_gl_context_for_test("OPENGL_GLX", null_lookup).is_none());
    assert!(try_with_thread_gl_context_for_test("OPENGL_GLX", lookup_null_ver).is_none());
    assert!(try_with_thread_gl_context_for_test("OPENGL_GLX", lookup_corrupt_ver).is_none());
}

#[test]
fn gl_overlay_texture_unit_preserves_non_zero_active_unit() {
    use fpshud::opengl::blit::with_overlay_texture_unit;

    struct UnitMockOps {
        active_unit: RefCell<u32>,
        bound_tex: RefCell<[u32; 4]>,
    }

    impl GlStateOps for UnitMockOps {
        fn get_integerv(&self, pname: u32) -> i32 {
            match pname {
                0x84E0 => *self.active_unit.borrow() as i32, // GL_ACTIVE_TEXTURE
                0x8069 => {
                    let idx = (*self.active_unit.borrow() - 0x84C0) as usize;
                    self.bound_tex.borrow()[idx] as i32
                } // GL_TEXTURE_BINDING_2D
                _ => 0,
            }
        }
        fn get_viewport(&self) -> [i32; 4] {
            [0; 4]
        }
        fn is_enabled(&self, _cap: u32) -> bool {
            false
        }
        fn use_program(&self, _prog: u32) {}
        fn bind_vertex_array(&self, _vao: u32) {}
        fn bind_framebuffer(&self, _fbo: u32) {}
        fn bind_unpack_buffer(&self, _buf: u32) {}
        fn set_enabled(&self, _cap: u32, _enable: bool) {}
        fn blend_func(&self, _src: u32, _dst: u32) {}
        fn viewport(&self, _vp: [i32; 4]) {}
        fn active_texture(&self, unit: u32) {
            *self.active_unit.borrow_mut() = unit;
        }
        fn bind_texture_2d(&self, tex: u32) {
            let idx = (*self.active_unit.borrow() - 0x84C0) as usize;
            self.bound_tex.borrow_mut()[idx] = tex;
        }
    }

    // Game has GL_TEXTURE3 active with tex=99 while TEXTURE0 holds tex=42
    let ops = UnitMockOps {
        active_unit: RefCell::new(0x84C3),
        bound_tex: RefCell::new([42, 0, 0, 99]),
    };

    let mut unit_during_draw = 0;
    with_overlay_texture_unit(&ops, || {
        unit_during_draw = *ops.active_unit.borrow();
        ops.bind_texture_2d(77);
    });

    assert_eq!(unit_during_draw, 0x84C0); // Switched to TEXTURE0 before draw/bind
    assert_eq!(*ops.active_unit.borrow(), 0x84C3); // Restored GL_TEXTURE3
    assert_eq!(*ops.bound_tex.borrow(), [42, 0, 0, 99]); // Neither unit clobbered!
}
