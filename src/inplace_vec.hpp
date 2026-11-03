#pragma once

// Isolate libstdc++ internal deprecated std::is_trivial_v usage inside <inplace_vector>
// so deprecation warnings remain active across FPSHUD and FFmpeg call sites.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <inplace_vector>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
