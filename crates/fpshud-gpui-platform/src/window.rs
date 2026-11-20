use crate::offscreen::OffscreenSurfaceTarget;
use crate::platform::OffscreenDisplay;
use gpui::{
    AnyWindowHandle, Bounds, Capslock, DevicePixels, DispatchEventResult, GpuSpecs, Modifiers,
    Pixels, PlatformAtlas, PlatformDisplay, PlatformInput, PlatformInputHandler, PlatformWindow,
    Point, PromptButton, RequestFrameOptions, Scene, Size, WindowAppearance,
    WindowBackgroundAppearance, WindowBounds, WindowControlArea, WindowParams, WindowVisibility,
};
use parking_lot::Mutex;
use raw_window_handle::{DisplayHandle, HandleError, HasDisplayHandle, HasWindowHandle, WindowHandle};
use std::rc::Rc;
use std::sync::Arc;

pub(crate) type ResizeCallback = Box<dyn FnMut(Size<Pixels>, f32)>;

#[must_use]
pub fn logical_to_device_size(size: Size<Pixels>, scale_factor: f32) -> Size<DevicePixels> {
    let s = scale_factor.max(0.25);
    Size {
        width: DevicePixels((f32::from(size.width) * s).round().max(1.0) as i32),
        height: DevicePixels((f32::from(size.height) * s).round().max(1.0) as i32),
    }
}

pub(crate) struct OffscreenWindowState {
    pub bounds: Bounds<Pixels>,
    pub scale_factor: f32,
    pub mouse_pos: Point<Pixels>,
    pub modifiers: Modifiers,
    pub dirty: bool,
    pub visible: bool,
    pub title: String,
    pub input_handler: Option<PlatformInputHandler>,
    pub display: Rc<OffscreenDisplay>,
    pub target: OffscreenSurfaceTarget,
    pub request_frame_cb: Option<Box<dyn FnMut(RequestFrameOptions)>>,
    pub input_cb: Option<Box<dyn FnMut(PlatformInput) -> DispatchEventResult>>,
    pub resize_cb: Option<ResizeCallback>,
}

#[derive(Clone)]
pub struct OffscreenWindow {
    pub(crate) inner: Rc<Mutex<OffscreenWindowState>>,
    pub(crate) _handle: AnyWindowHandle,
    atlas: Arc<dyn PlatformAtlas>,
}

impl OffscreenWindow {
    pub(crate) fn new(
        handle: AnyWindowHandle,
        params: WindowParams,
        display: Rc<OffscreenDisplay>,
        scale_factor: f32,
    ) -> anyhow::Result<Self> {
        let dev = logical_to_device_size(params.bounds.size, scale_factor);
        let target = OffscreenSurfaceTarget::new(dev.width.0 as u32, dev.height.0 as u32)?;
        let atlas = target.sprite_atlas();
        let inner = Rc::new(Mutex::new(OffscreenWindowState {
            bounds: params.bounds,
            scale_factor: scale_factor.max(0.25),
            mouse_pos: Point::default(),
            modifiers: Modifiers::default(),
            dirty: true,
            visible: true,
            title: "FPSHUD".to_string(),
            input_handler: None,
            display,
            target,
            request_frame_cb: None,
            input_cb: None,
            resize_cb: None,
        }));
        Ok(Self {
            inner,
            _handle: handle,
            atlas,
        })
    }

    #[must_use]
    pub fn is_dirty(&self) -> bool {
        self.inner.lock().dirty
    }

    pub fn mark_dirty(&self) {
        self.inner.lock().dirty = true;
    }
}

impl HasWindowHandle for OffscreenWindow {
    fn window_handle(&self) -> Result<WindowHandle<'_>, HandleError> {
        Err(HandleError::NotSupported)
    }
}

impl HasDisplayHandle for OffscreenWindow {
    fn display_handle(&self) -> Result<DisplayHandle<'_>, HandleError> {
        Err(HandleError::NotSupported)
    }
}

impl PlatformWindow for OffscreenWindow {
    fn bounds(&self) -> Bounds<Pixels> {
        self.inner.lock().bounds
    }

    fn is_maximized(&self) -> bool {
        false
    }

    fn window_bounds(&self) -> WindowBounds {
        WindowBounds::Windowed(self.inner.lock().bounds)
    }

    fn content_size(&self) -> Size<Pixels> {
        self.inner.lock().bounds.size
    }

    fn resize(&mut self, size: Size<Pixels>) {
        let scale = {
            let mut st = self.inner.lock();
            st.bounds.size = size;
            st.dirty = true;
            let dev = logical_to_device_size(size, st.scale_factor);
            let _ = st.target.resize(dev.width.0 as u32, dev.height.0 as u32);
            st.scale_factor
        };
        let cb = self.inner.lock().resize_cb.take();
        if let Some(mut cb) = cb {
            cb(size, scale);
            self.inner.lock().resize_cb = Some(cb);
        }
    }

    fn scale_factor(&self) -> f32 {
        self.inner.lock().scale_factor
    }

    fn appearance(&self) -> WindowAppearance {
        WindowAppearance::Dark
    }

    fn display(&self) -> Option<Rc<dyn PlatformDisplay>> {
        Some(self.inner.lock().display.clone())
    }

    fn mouse_position(&self) -> Point<Pixels> {
        self.inner.lock().mouse_pos
    }

    fn modifiers(&self) -> Modifiers {
        self.inner.lock().modifiers
    }

    fn capslock(&self) -> Capslock {
        Capslock::default()
    }

    fn set_input_handler(&mut self, input_handler: PlatformInputHandler) {
        self.inner.lock().input_handler = Some(input_handler);
    }

    fn take_input_handler(&mut self) -> Option<PlatformInputHandler> {
        self.inner.lock().input_handler.take()
    }

    fn prompt(
        &self,
        _level: gpui::PromptLevel,
        _msg: &str,
        _detail: Option<&str>,
        _answers: &[PromptButton],
    ) -> Option<futures::channel::oneshot::Receiver<usize>> {
        None
    }

    fn activate(&self, _token: Option<&str>) -> bool {
        true
    }

    fn is_active(&self) -> bool {
        true
    }

    fn visibility(&self) -> WindowVisibility {
        WindowVisibility::Visible
    }

    fn is_hovered(&self) -> bool {
        true
    }

    fn background_appearance(&self) -> WindowBackgroundAppearance {
        WindowBackgroundAppearance::Transparent
    }

    fn set_title(&mut self, title: &str) {
        self.inner.lock().title = title.to_string();
    }

    fn set_background_appearance(&self, _background_appearance: WindowBackgroundAppearance) {}

    fn set_visible(&self, visible: bool) {
        self.inner.lock().visible = visible;
    }

    fn minimize(&self) {}
    fn zoom(&self) {}
    fn toggle_fullscreen(&self) {}
    fn is_fullscreen(&self) -> bool {
        false
    }

    fn on_request_frame(&self, callback: Box<dyn FnMut(RequestFrameOptions)>) {
        self.inner.lock().request_frame_cb = Some(callback);
    }

    fn schedule_frame(&self) {
        self.inner.lock().dirty = true;
    }

    fn on_input(&self, callback: Box<dyn FnMut(PlatformInput) -> DispatchEventResult>) {
        self.inner.lock().input_cb = Some(callback);
    }

    fn on_active_status_change(&self, _callback: Box<dyn FnMut(bool)>) {}
    fn on_visibility_change(&self, _callback: Box<dyn FnMut(WindowVisibility)>) {}
    fn on_hover_status_change(&self, _callback: Box<dyn FnMut(bool)>) {}

    fn on_resize(&self, callback: Box<dyn FnMut(Size<Pixels>, f32)>) {
        self.inner.lock().resize_cb = Some(callback);
    }

    fn on_moved(&self, _callback: Box<dyn FnMut()>) {}
    fn on_display_changed(&self, _callback: Box<dyn FnMut()>) {}
    fn on_should_close(&self, _callback: Box<dyn FnMut() -> bool>) {}
    fn on_hit_test_window_control(
        &self,
        _callback: Box<dyn FnMut() -> Option<WindowControlArea>>,
    ) {
    }
    fn on_close(&self, _callback: Box<dyn FnOnce()>) {}
    fn on_appearance_changed(&self, _callback: Box<dyn FnMut()>) {}

    fn draw(&self, scene: &Scene) {
        let mut st = self.inner.lock();
        let _ = st.target.render_scene(scene);
        st.dirty = false;
    }

    fn sprite_atlas(&self) -> Arc<dyn PlatformAtlas> {
        self.atlas.clone()
    }

    fn is_subpixel_rendering_supported(&self) -> bool {
        false
    }

    fn gpu_specs(&self) -> Option<GpuSpecs> {
        None
    }

    fn update_ime_position(&self, _bounds: Bounds<Pixels>) {}
}
