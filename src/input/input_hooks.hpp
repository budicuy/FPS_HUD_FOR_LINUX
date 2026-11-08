#pragma once

#include <cstdint>

namespace fpshud {

// Initialize dynamic function pointers for SDL and X11
void init_input_hooks() noexcept;

// Notifies input hooks when the in-game GUI menu opens or closes
void on_gui_state_changed(bool gui_open) noexcept;

// Checks whether mouse isolation / absorption is currently active
bool is_mouse_isolation_active() noexcept;

}  // namespace fpshud
