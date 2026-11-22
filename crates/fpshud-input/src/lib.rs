//! Pure-Rust input handling (`x11rb`) & swapchain surface disambiguation.

pub mod keycodes;
pub mod manager;
pub mod x11;

pub use manager::{
    InputAction, InputManager, KeybindTarget, SurfaceBackendType, UiInputEvent,
};
