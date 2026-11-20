use crate::fonts::create_embedded_text_system;
use crate::window::{OffscreenWindow, OffscreenWindowState};
use futures::channel::oneshot;
use gpui::{
    Action, ActivityGuard, AnyWindowHandle, BackgroundExecutor, Bounds, ClipboardItem, CursorStyle,
    DisplayId, DummyKeyboardMapper, ForegroundExecutor, Keymap, Menu, MenuItem, PathPromptOptions,
    Pixels, Platform, PlatformDisplay, PlatformDispatcher, PlatformKeyboardLayout,
    PlatformKeyboardMapper, PlatformTextSystem, PlatformWindow, Point, Priority, RunnableVariant,
    Size, Task, ThermalState, WindowAppearance, WindowParams, px,
};
use parking_lot::Mutex;
use std::cell::RefCell;
use std::collections::VecDeque;
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::{Arc, mpsc};
use std::thread::{self, ThreadId};
use std::time::{Duration, Instant};
use uuid::Uuid;

struct DelayedTask {
    due: Instant,
    runnable: RunnableVariant,
}

struct OffscreenDispatcher {
    main_thread_id: ThreadId,
    main_queue: Mutex<VecDeque<RunnableVariant>>,
    delayed_queue: Mutex<Vec<DelayedTask>>,
    bg_sender: mpsc::Sender<RunnableVariant>,
}

impl OffscreenDispatcher {
    fn new() -> Arc<Self> {
        let (bg_sender, bg_rx) = mpsc::channel::<RunnableVariant>();
        let shared_rx = Arc::new(Mutex::new(bg_rx));
        for idx in 0..2 {
            let rx_clone = Arc::clone(&shared_rx);
            let _ = thread::Builder::new()
                .name(format!("fpshud_gpui_bg_{idx}"))
                .spawn(move || {
                    loop {
                        let next = rx_clone.lock().recv();
                        match next {
                            Ok(runnable) => {
                                let _ = runnable.run();
                            }
                            Err(_) => break,
                        }
                    }
                });
        }

        Arc::new(Self {
            main_thread_id: thread::current().id(),
            main_queue: Mutex::new(VecDeque::new()),
            delayed_queue: Mutex::new(Vec::new()),
            bg_sender,
        })
    }

    fn drain_main_tasks(&self) {
        loop {
            let now = Instant::now();
            let mut ready_delayed = Vec::new();
            {
                let mut delayed = self.delayed_queue.lock();
                let mut i = 0;
                while i < delayed.len() {
                    if delayed[i].due <= now {
                        ready_delayed.push(delayed.swap_remove(i).runnable);
                    } else {
                        i += 1;
                    }
                }
            }
            if !ready_delayed.is_empty() {
                self.main_queue.lock().extend(ready_delayed);
            }
            let next = self.main_queue.lock().pop_front();
            match next {
                Some(task) => {
                    let _ = task.run();
                }
                None => break,
            }
        }
    }
}

impl PlatformDispatcher for OffscreenDispatcher {
    fn is_main_thread(&self) -> bool {
        thread::current().id() == self.main_thread_id
    }

    fn dispatch(&self, runnable: RunnableVariant, _priority: Priority) {
        let _ = self.bg_sender.send(runnable);
    }

    fn dispatch_on_main_thread(&self, runnable: RunnableVariant, _priority: Priority) {
        self.main_queue.lock().push_back(runnable);
    }

    fn dispatch_after(&self, duration: Duration, runnable: RunnableVariant) {
        self.delayed_queue.lock().push(DelayedTask {
            due: Instant::now() + duration,
            runnable,
        });
    }

    fn now(&self) -> Instant {
        Instant::now()
    }

    fn spawn_realtime(&self, f: Box<dyn FnOnce() + Send + 'static>) {
        let _ = thread::Builder::new()
            .name("fpshud_gpui_rt".to_string())
            .spawn(f);
    }
}

#[derive(Debug)]
pub struct OffscreenDisplay {
    id: DisplayId,
    uuid: Uuid,
    size: Mutex<Size<Pixels>>,
}

impl OffscreenDisplay {
    #[must_use]
    pub fn new(width: u32, height: u32) -> Self {
        Self {
            id: DisplayId::new(1),
            uuid: Uuid::new_v4(),
            size: Mutex::new(Size::new(px(width.max(1) as f32), px(height.max(1) as f32))),
        }
    }

    pub fn set_size(&self, width: u32, height: u32) {
        *self.size.lock() = Size::new(px(width.max(1) as f32), px(height.max(1) as f32));
    }
}

impl PlatformDisplay for OffscreenDisplay {
    fn id(&self) -> DisplayId {
        self.id
    }

    fn uuid(&self) -> anyhow::Result<Uuid> {
        Ok(self.uuid)
    }

    fn bounds(&self) -> Bounds<Pixels> {
        Bounds::new(Point::default(), *self.size.lock())
    }

    fn refresh_interval(&self) -> Option<Duration> {
        Some(Duration::from_secs_f64(1.0 / 60.0))
    }
}

struct OffscreenKeyboardLayout;

impl PlatformKeyboardLayout for OffscreenKeyboardLayout {
    fn id(&self) -> &str {
        "fpshud.offscreen"
    }

    fn name(&self) -> &str {
        "FPSHUD Offscreen Keyboard"
    }
}

/// Custom zero-C-desktop `gpui::Platform` executing offscreen against `WgpuOffscreenRenderer`.
pub struct OffscreenPlatform {
    dispatcher: Arc<OffscreenDispatcher>,
    background: BackgroundExecutor,
    foreground: ForegroundExecutor,
    text_system: Arc<dyn PlatformTextSystem>,
    display: Rc<OffscreenDisplay>,
    scale_factor: Mutex<f32>,
    clipboard: Mutex<Option<ClipboardItem>>,
    primary: Mutex<Option<ClipboardItem>>,
    last_window: RefCell<Option<Rc<Mutex<OffscreenWindowState>>>>,
    last_window_handle: RefCell<Option<AnyWindowHandle>>,
}

impl OffscreenPlatform {
    #[must_use]
    pub fn new(width: u32, height: u32, scale_factor: f32) -> Rc<Self> {
        let dispatcher = OffscreenDispatcher::new();
        let background = BackgroundExecutor::new(dispatcher.clone());
        let foreground = ForegroundExecutor::new(dispatcher.clone());
        let text_system = create_embedded_text_system();

        Rc::new(Self {
            dispatcher,
            background,
            foreground,
            text_system,
            display: Rc::new(OffscreenDisplay::new(width, height)),
            scale_factor: Mutex::new(scale_factor.clamp(0.5, 3.0)),
            clipboard: Mutex::new(None),
            primary: Mutex::new(None),
            last_window: RefCell::new(None),
            last_window_handle: RefCell::new(None),
        })
    }

    pub fn drain_tasks(&self) {
        self.dispatcher.drain_main_tasks();
    }

    pub fn set_display_size_and_scale(&self, width: u32, height: u32, scale_factor: f32) {
        self.display.set_size(width, height);
        *self.scale_factor.lock() = scale_factor.clamp(0.5, 3.0);
    }

    pub(crate) fn last_window_state(&self) -> Option<Rc<Mutex<OffscreenWindowState>>> {
        self.last_window.borrow().clone()
    }
}

impl Platform for OffscreenPlatform {
    fn background_executor(&self) -> BackgroundExecutor {
        self.background.clone()
    }

    fn foreground_executor(&self) -> ForegroundExecutor {
        self.foreground.clone()
    }

    fn text_system(&self) -> Arc<dyn PlatformTextSystem> {
        self.text_system.clone()
    }

    fn run(&self, on_finish_launching: Box<dyn 'static + FnOnce()>) {
        on_finish_launching();
    }

    fn quit(&self) {}
    fn restart(&self, _binary_path: Option<PathBuf>, _arguments: Vec<OsString>) {}
    fn activate(&self, _ignoring_other_apps: bool) {}
    fn hide(&self) {}
    fn hide_other_apps(&self) {}
    fn unhide_other_apps(&self) {}

    fn displays(&self) -> Vec<Rc<dyn PlatformDisplay>> {
        vec![self.display.clone()]
    }

    fn primary_display(&self) -> Option<Rc<dyn PlatformDisplay>> {
        Some(self.display.clone())
    }

    fn on_displays_changed(&self, _callback: Box<dyn FnMut()>) {}

    fn active_window(&self) -> Option<AnyWindowHandle> {
        *self.last_window_handle.borrow()
    }

    fn open_window(
        &self,
        handle: AnyWindowHandle,
        options: WindowParams,
    ) -> anyhow::Result<Box<dyn PlatformWindow>> {
        let scale = *self.scale_factor.lock();
        let win = OffscreenWindow::new(handle, options, self.display.clone(), scale)?;
        *self.last_window.borrow_mut() = Some(Rc::clone(&win.inner));
        *self.last_window_handle.borrow_mut() = Some(handle);
        Ok(Box::new(win))
    }

    fn window_appearance(&self) -> WindowAppearance {
        WindowAppearance::Dark
    }

    fn compositor_name(&self) -> &'static str {
        "FPSHUD-Offscreen"
    }

    fn open_url(&self, _url: &str) {}
    fn on_open_urls(&self, _callback: Box<dyn FnMut(Vec<String>)>) {}

    fn register_url_scheme(&self, _url: &str) -> Task<anyhow::Result<()>> {
        Task::ready(Ok(()))
    }

    fn prompt_for_paths(
        &self,
        _options: PathPromptOptions,
    ) -> oneshot::Receiver<anyhow::Result<Option<Vec<PathBuf>>>> {
        let (tx, rx) = oneshot::channel();
        let _ = tx.send(Ok(None));
        rx
    }

    fn prompt_for_new_path(
        &self,
        _directory: &Path,
        _suggested_name: Option<&str>,
    ) -> oneshot::Receiver<anyhow::Result<Option<PathBuf>>> {
        let (tx, rx) = oneshot::channel();
        let _ = tx.send(Ok(None));
        rx
    }

    fn can_select_mixed_files_and_dirs(&self) -> bool {
        false
    }

    fn reveal_path(&self, _path: &Path) {}
    fn open_with_system(&self, _path: &Path) {}
    fn on_quit(&self, _callback: Box<dyn FnMut() -> bool>) {}
    fn on_reopen(&self, _callback: Box<dyn FnMut()>) {}
    fn on_system_sleep(&self, _callback: Box<dyn FnMut()>) {}
    fn on_system_wake(&self, _callback: Box<dyn FnMut()>) {}
    fn set_menus(&self, _menus: Vec<Menu>, _keymap: &Keymap) {}
    fn set_dock_menu(&self, _menu: Vec<MenuItem>, _keymap: &Keymap) {}
    fn on_app_menu_action(&self, _callback: Box<dyn FnMut(&dyn Action)>) {}
    fn on_will_open_app_menu(&self, _callback: Box<dyn FnMut()>) {}
    fn on_validate_app_menu_command(&self, _callback: Box<dyn FnMut(&dyn Action) -> bool>) {}

    fn thermal_state(&self) -> ThermalState {
        ThermalState::Nominal
    }

    fn on_thermal_state_change(&self, _callback: Box<dyn FnMut()>) {}

    fn prevent_idle_sleep(&self, _reason: &str) -> Task<anyhow::Result<ActivityGuard>> {
        Task::ready(Ok(ActivityGuard::new(|| {})))
    }

    fn app_path(&self) -> anyhow::Result<PathBuf> {
        std::env::current_exe().map_err(Into::into)
    }

    fn path_for_auxiliary_executable(&self, name: &str) -> anyhow::Result<PathBuf> {
        Ok(std::env::current_exe()?
            .parent()
            .unwrap_or_else(|| Path::new("."))
            .join(name))
    }

    fn set_cursor_style(&self, _style: CursorStyle) {}
    fn hide_cursor_until_mouse_moves(&self) {}

    fn is_cursor_visible(&self) -> bool {
        true
    }

    fn should_auto_hide_scrollbars(&self) -> bool {
        false
    }

    fn read_from_clipboard(&self) -> Option<ClipboardItem> {
        self.clipboard.lock().clone()
    }

    fn write_to_clipboard(&self, item: ClipboardItem) {
        *self.clipboard.lock() = Some(item);
    }

    #[cfg(any(target_os = "linux", target_os = "freebsd"))]
    fn read_from_primary(&self) -> Option<ClipboardItem> {
        self.primary.lock().clone()
    }

    #[cfg(any(target_os = "linux", target_os = "freebsd"))]
    fn write_to_primary(&self, item: ClipboardItem) {
        *self.primary.lock() = Some(item);
    }

    fn write_credentials(&self, _url: &str, _username: &str, _password: &[u8]) -> Task<anyhow::Result<()>> {
        Task::ready(Ok(()))
    }

    fn read_credentials(&self, _url: &str) -> Task<anyhow::Result<Option<(String, Vec<u8>)>>> {
        Task::ready(Ok(None))
    }

    fn delete_credentials(&self, _url: &str) -> Task<anyhow::Result<()>> {
        Task::ready(Ok(()))
    }

    fn keyboard_layout(&self) -> Box<dyn PlatformKeyboardLayout> {
        Box::new(OffscreenKeyboardLayout)
    }

    fn keyboard_mapper(&self) -> Rc<dyn PlatformKeyboardMapper> {
        Rc::new(DummyKeyboardMapper)
    }

    fn on_keyboard_layout_change(&self, _callback: Box<dyn FnMut()>) {}
}
