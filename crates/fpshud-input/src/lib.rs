#![forbid(unsafe_code)]
//! Pure-Rust input handling (`x11rb`), optional non-privileged `evdev`, & swapchain disambiguation.

pub mod evdev;
pub mod keycodes;
pub mod manager;
pub mod x11;

pub use manager::{
    InputAction, InputManager, KeybindTarget, SurfaceBackendType, UiInputEvent,
};
