#pragma once

#include <cstdint>
#include <string>

namespace fpshud {

// Self-contained, dependency-free lossless PNG encoder.
// format:
//   0 = BGRA (8-bit per channel)
//   1 = RGBA (8-bit per channel)
//   2 = X2BGR10LE (10-bit per channel packed in 32-bit LE word)
//   3 = X2RGB10LE (10-bit per channel packed in 32-bit LE word)
bool write_rgba_to_png(const std::string& path, const uint8_t* pixels, uint32_t width,
                       uint32_t height, uint32_t format, bool flip_y);

}  // namespace fpshud
