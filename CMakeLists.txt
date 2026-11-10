cmake_minimum_required(VERSION 3.25)
project(FPSHUD VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++26")
check_cxx_source_compiles("
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored \"-Wdeprecated-declarations\"
#endif
#include <inplace_vector>
int main() {
    std::inplace_vector<int, 4> v;
    v.try_push_back(42);
    return v.size() == 1 ? 0 : 1;
}
" FPSHUD_HAS_CXX26_INPLACE_VECTOR)
if(NOT FPSHUD_HAS_CXX26_INPLACE_VECTOR)
    message(FATAL_ERROR "FPSHUD requires pure native ISO C++26 <inplace_vector> support (Clang 19+ or GCC 15+). Polyfill has been deprecated.")
endif()

check_cxx_source_compiles("
template <typename... T>
constexpr auto first(T... args) { return args...[0]; }
int main() { return first(0, 1, 2); }
" FPSHUD_HAS_CXX26_PACK_INDEXING)
if(NOT FPSHUD_HAS_CXX26_PACK_INDEXING)
    message(FATAL_ERROR "FPSHUD requires native ISO C++26 pack indexing (P2662R3). Toolchain does not support args...[N].")
endif()
unset(CMAKE_REQUIRED_FLAGS)

add_compile_options(-Wno-deprecated-declarations)

option(FPSHUD_NATIVE_ARCH "Optimize for host CPU microarchitecture (-march=native)" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer and LeakSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(FPSHUD_ENABLE_EXPERIMENTAL "Enable isolated experimental features" ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(FPSHUD_ENABLE_DEBUG_LOGGING_DEFAULT ON)
else()
    set(FPSHUD_ENABLE_DEBUG_LOGGING_DEFAULT OFF)
endif()
option(FPSHUD_ENABLE_DEBUG_LOGGING "Enable development and trace/debug logging" ${FPSHUD_ENABLE_DEBUG_LOGGING_DEFAULT})

if(FPSHUD_ENABLE_DEBUG_LOGGING)
    add_compile_definitions(FPSHUD_ENABLE_DEBUG_LOGGING=1)
endif()

if(FPSHUD_ENABLE_EXPERIMENTAL)
    add_compile_definitions(FPSHUD_ENABLE_EXPERIMENTAL=1)
    set(FPSHUD_EXPERIMENTAL_SOURCES
        src/experimental-feature/adaptive_vfr.cpp
    )
    set(FPSHUD_EXPERIMENTAL_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/experimental-feature
    )
else()
    set(FPSHUD_EXPERIMENTAL_SOURCES "")
    set(FPSHUD_EXPERIMENTAL_INCLUDES "")
endif()

if(FPSHUD_NATIVE_ARCH)
    add_compile_options(-march=native)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_compile_options(-march=x86-64 -mtune=generic)
endif()

if(ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN)
    set(USE_SANITIZERS ON)
else()
    set(USE_SANITIZERS OFF)
endif()

include(CheckSymbolExists)
check_symbol_exists(__GLIBC__ "features.h" FPSHUD_IS_GLIBC)

set(FPSHUD_GLIBC_COMPAT_SOURCES "")
set(FPSHUD_GLIBC_COMPAT_LINK_OPTS "")
if(FPSHUD_IS_GLIBC AND UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT USE_SANITIZERS)
    set_source_files_properties(src/layer/glibc_compat.c PROPERTIES COMPILE_OPTIONS "-fno-lto")
    set(FPSHUD_GLIBC_COMPAT_SOURCES src/layer/glibc_compat.c)
    set(FPSHUD_GLIBC_COMPAT_LINK_OPTS
        -Wl,--wrap=acosf
        -Wl,--wrap=atan2f
        -Wl,--wrap=fmodf
        -Wl,--wrap=sinh
        -Wl,--wrap=cosh
        -Wl,--wrap=__isoc23_sscanf
        -Wl,--wrap=__isoc23_strtol
        -Wl,--wrap=__isoc23_strtoul
        -Wl,--wrap=__isoc23_strtoull
        -Wl,--wrap=__isoc23_strtoll
        -Wl,--wrap=arc4random
    )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(NOT USE_SANITIZERS)
        message(STATUS "Toolchain: LLVM/Clang (${CMAKE_CXX_COMPILER_VERSION}) with LLD & ThinLTO (Extreme Release)")
        add_compile_options(
            -O3
            -DNDEBUG
            -flto=thin
            -fno-plt
            -fno-semantic-interposition
            -ffunction-sections
            -fdata-sections
            -fstack-protector-strong
            -fno-math-errno
            -fno-trapping-math
            -Wall
            -Wextra
        )
        add_link_options(
            -fuse-ld=lld
            -flto=thin
            -Wl,--gc-sections
            -Wl,-O2
            -Wl,--as-needed
            $<$<LINK_LANGUAGE:CXX>:-static-libstdc++>
            -static-libgcc
        )
    else()
        message(STATUS "Toolchain: LLVM/Clang (${CMAKE_CXX_COMPILER_VERSION}) [Diagnostic Sanitizer Build]")
        add_compile_options(
            -O1
            -g
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
            -Wall
            -Wextra
        )
    endif()
else()
    if(NOT USE_SANITIZERS)
        message(STATUS "Toolchain: GNU/GCC (${CMAKE_CXX_COMPILER_VERSION})")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        add_compile_options(
            -O3
            -DNDEBUG
            -ffunction-sections
            -fdata-sections
            -fstack-protector-strong
            -fno-math-errno
            -fno-trapping-math
            -Wall
            -Wextra
        )
        add_link_options(
            -Wl,--gc-sections
            -Wl,-O2
            -Wl,--as-needed
            $<$<LINK_LANGUAGE:CXX>:-static-libstdc++>
            -static-libgcc
        )
    else()
        message(STATUS "Toolchain: GNU/GCC (${CMAKE_CXX_COMPILER_VERSION}) [Diagnostic Sanitizer Build]")
        add_compile_options(
            -O1
            -g
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
            -Wall
            -Wextra
        )
    endif()
endif()

if(ENABLE_ASAN)
    message(STATUS "  -> AddressSanitizer + LeakSanitizer: ENABLED")
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

if(ENABLE_UBSAN)
    message(STATUS "  -> UndefinedBehaviorSanitizer: ENABLED")
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=undefined)
endif()

if(ENABLE_TSAN)
    message(STATUS "  -> ThreadSanitizer: ENABLED")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

include(FetchContent)

# Fetch Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

find_package(Vulkan REQUIRED)
find_path(FPSHUD_GL_INCLUDE_DIR NAMES GL/gl.h REQUIRED)
find_path(FPSHUD_EGL_INCLUDE_DIR NAMES EGL/egl.h REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter)

# Automatically generate src/ui/embedded_fonts.hpp if not already present
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_embedded_fonts.py" "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/embedded_fonts.hpp"
    )
endif()

set(IMGUI_CORE_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
set(IMGUI_SOURCES
    ${IMGUI_CORE_SOURCES}
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

add_library(fpshud SHARED
    src/core/config.cpp
    src/core/logger.cpp
    src/telemetry/telemetry.cpp
    src/telemetry/analytics/frametime_analyzer.cpp
    src/telemetry/analytics/benchmark_recorder.cpp
    src/telemetry/providers/cpu_provider.cpp
    src/telemetry/providers/ram_provider.cpp
    src/telemetry/providers/system_provider.cpp
    src/telemetry/providers/gpu_nvidia.cpp
    src/telemetry/providers/gpu_drm.cpp
    src/telemetry/providers/gpu_levelzero.cpp
    src/recorder/png_writer.cpp
    src/recorder/recorder_ipc.cpp
    src/input/backends/evdev_backend.cpp
    src/input/backends/wayland_backend.cpp
    src/input/backends/x11_backend.cpp
    src/input/input_manager.cpp
    src/ui/gui_menu.cpp
    src/ui/overlay_ui.cpp
    src/layer/vulkan_hooks.cpp
    src/layer/vulkan_swapchain.cpp
    src/layer/vulkan_capture.cpp
    src/layer/vulkan_player_stream.cpp
    src/layer/vulkan_layer.cpp
    src/layer/opengl_hook.cpp
    ${IMGUI_SOURCES}
    ${FPSHUD_GLIBC_COMPAT_SOURCES}
    ${FPSHUD_EXPERIMENTAL_SOURCES}
)

target_include_directories(fpshud PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/analytics
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder
    ${CMAKE_CURRENT_SOURCE_DIR}/src/layer
    ${FPSHUD_EXPERIMENTAL_INCLUDES}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${Vulkan_INCLUDE_DIRS}
    ${FPSHUD_GL_INCLUDE_DIR}
    ${FPSHUD_EGL_INCLUDE_DIR}
)

target_compile_definitions(fpshud PRIVATE
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES
)

target_link_libraries(fpshud PRIVATE
    Vulkan::Headers
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

target_link_options(fpshud PRIVATE
    -Wl,--exclude-libs,ALL
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/manifest/libfpshud.version"
    ${FPSHUD_GLIBC_COMPAT_LINK_OPTS}
)

# Hide internal symbols except exported Vulkan Layer & OpenGL hook entrypoints
set_target_properties(fpshud PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX "lib"
    OUTPUT_NAME "fpshud"
)

# CLI Test Utility for Config & Hardware Telemetry
add_executable(fpshud_test
    tests/test_cli.cpp
    src/core/config.cpp
    src/core/logger.cpp
    src/telemetry/telemetry.cpp
    src/telemetry/analytics/frametime_analyzer.cpp
    src/telemetry/analytics/benchmark_recorder.cpp
    src/telemetry/providers/cpu_provider.cpp
    src/telemetry/providers/ram_provider.cpp
    src/telemetry/providers/system_provider.cpp
    src/telemetry/providers/gpu_nvidia.cpp
    src/telemetry/providers/gpu_drm.cpp
    src/telemetry/providers/gpu_levelzero.cpp
    src/input/backends/evdev_backend.cpp
    src/input/backends/wayland_backend.cpp
    src/input/backends/x11_backend.cpp
    src/input/input_manager.cpp
    src/recorder/png_writer.cpp
    src/recorder/recorder_ipc.cpp
    src/recorder/recorder_player.cpp
    src/ui/gui_menu.cpp
    src/ui/overlay_ui.cpp
    ${IMGUI_CORE_SOURCES}
    ${FPSHUD_GLIBC_COMPAT_SOURCES}
    ${FPSHUD_EXPERIMENTAL_SOURCES}
)

target_include_directories(fpshud_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/analytics
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder
    ${CMAKE_CURRENT_SOURCE_DIR}/src/layer
    ${FPSHUD_EXPERIMENTAL_INCLUDES}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(fpshud_test PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

target_link_options(fpshud_test PRIVATE
    ${FPSHUD_GLIBC_COMPAT_LINK_OPTS}
)

# Doctest Modular Test Scaffolding Smoke Target
add_executable(fpshud_test_smoke
    tests/test_main.cpp
    src/core/config.cpp
    src/core/logger.cpp
    src/telemetry/telemetry.cpp
    src/telemetry/analytics/frametime_analyzer.cpp
    src/telemetry/analytics/benchmark_recorder.cpp
    src/telemetry/providers/cpu_provider.cpp
    src/telemetry/providers/ram_provider.cpp
    src/telemetry/providers/system_provider.cpp
    src/telemetry/providers/gpu_nvidia.cpp
    src/telemetry/providers/gpu_drm.cpp
    src/telemetry/providers/gpu_levelzero.cpp
    src/input/backends/evdev_backend.cpp
    src/input/backends/wayland_backend.cpp
    src/input/backends/x11_backend.cpp
    src/input/input_manager.cpp
    src/recorder/png_writer.cpp
    src/recorder/recorder_ipc.cpp
    src/recorder/recorder_player.cpp
    src/ui/gui_menu.cpp
    src/ui/overlay_ui.cpp
    ${IMGUI_CORE_SOURCES}
    ${FPSHUD_GLIBC_COMPAT_SOURCES}
    ${FPSHUD_EXPERIMENTAL_SOURCES}
)

target_include_directories(fpshud_test_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/analytics
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder
    ${CMAKE_CURRENT_SOURCE_DIR}/src/layer
    ${FPSHUD_EXPERIMENTAL_INCLUDES}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(fpshud_test_smoke PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

target_link_options(fpshud_test_smoke PRIVATE
    ${FPSHUD_GLIBC_COMPAT_LINK_OPTS}
)

# Screen Recorder Helper Daemon (On-Demand)
set(FFMPEG_MINIMAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg_minimal" CACHE PATH "Path to minimal static FFmpeg build")
set(FPSHUD_STATIC_RECORDER "AUTO" CACHE STRING "Statically link recorder binary with minimal FFmpeg (AUTO/ON/OFF)")

set(BUILD_STATIC_RECORDER FALSE)
if(FPSHUD_STATIC_RECORDER STREQUAL "ON" OR
   (FPSHUD_STATIC_RECORDER STREQUAL "AUTO" AND
    EXISTS "${FFMPEG_MINIMAL_DIR}/lib/libavformat.a" AND
    EXISTS "${FFMPEG_MINIMAL_DIR}/lib/libavcodec.a" AND
    EXISTS "${FFMPEG_MINIMAL_DIR}/lib/libswscale.a" AND
    EXISTS "${FFMPEG_MINIMAL_DIR}/lib/libswresample.a" AND
    EXISTS "${FFMPEG_MINIMAL_DIR}/lib/libavutil.a"))
    set(BUILD_STATIC_RECORDER TRUE)
endif()

find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(AVFORMAT IMPORTED_TARGET libavformat)
    pkg_check_modules(AVCODEC IMPORTED_TARGET libavcodec)
    pkg_check_modules(AVUTIL IMPORTED_TARGET libavutil)
    pkg_check_modules(SWSCALE IMPORTED_TARGET libswscale)
    pkg_check_modules(SWRESAMPLE IMPORTED_TARGET libswresample)
    pkg_check_modules(PULSE IMPORTED_TARGET libpulse)
    pkg_check_modules(LIBVA IMPORTED_TARGET libva)
    pkg_check_modules(LIBVA_DRM IMPORTED_TARGET libva-drm)
    pkg_check_modules(LIBDRM IMPORTED_TARGET libdrm)
endif()

if (BUILD_STATIC_RECORDER)
    add_executable(fpshud_recorder_bin
        src/recorder/recorder_main.cpp
        src/recorder/recorder_player.cpp
        ${FPSHUD_GLIBC_COMPAT_SOURCES}
    )

    target_compile_definitions(fpshud_recorder_bin PRIVATE
        FPSHUD_RECORDER_DAEMON=1
    )

    target_include_directories(fpshud_recorder_bin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${FFMPEG_MINIMAL_DIR}/include
    )

    target_compile_options(fpshud_recorder_bin PRIVATE
        -ffunction-sections
        -fdata-sections
    )

    target_link_libraries(fpshud_recorder_bin PRIVATE
        ${FFMPEG_MINIMAL_DIR}/lib/libavformat.a
        ${FFMPEG_MINIMAL_DIR}/lib/libavcodec.a
        ${FFMPEG_MINIMAL_DIR}/lib/libswscale.a
        ${FFMPEG_MINIMAL_DIR}/lib/libswresample.a
        ${FFMPEG_MINIMAL_DIR}/lib/libavutil.a
        $<$<TARGET_EXISTS:PkgConfig::PULSE>:PkgConfig::PULSE>
        $<$<NOT:$<TARGET_EXISTS:PkgConfig::PULSE>>:pulse>
        $<$<TARGET_EXISTS:PkgConfig::LIBVA>:PkgConfig::LIBVA>
        $<$<NOT:$<TARGET_EXISTS:PkgConfig::LIBVA>>:va>
        $<$<TARGET_EXISTS:PkgConfig::LIBVA_DRM>:PkgConfig::LIBVA_DRM>
        $<$<NOT:$<TARGET_EXISTS:PkgConfig::LIBVA_DRM>>:va-drm>
        $<$<TARGET_EXISTS:PkgConfig::LIBDRM>:PkgConfig::LIBDRM>
        $<$<NOT:$<TARGET_EXISTS:PkgConfig::LIBDRM>>:drm>
        z
        m
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )

    target_link_options(fpshud_recorder_bin PRIVATE
        -Wl,--gc-sections
        $<$<LINK_LANGUAGE:CXX>:-static-libstdc++>
        -static-libgcc
        ${FPSHUD_GLIBC_COMPAT_LINK_OPTS}
    )

    # Universal lightweight container-aware launcher
    add_executable(fpshud_recorder
        src/recorder/recorder_launcher.c
        ${FPSHUD_GLIBC_COMPAT_SOURCES}
    )
    target_link_libraries(fpshud_recorder PRIVATE
        ${CMAKE_DL_LIBS}
    )
    target_link_options(fpshud_recorder PRIVATE
        ${FPSHUD_GLIBC_COMPAT_LINK_OPTS}
    )

    message(STATUS "Screen Recorder: ENABLED [Minimal Hardware Static FFmpeg < 5 MB] (fpshud_recorder & fpshud_recorder_bin)")
elseif (TARGET PkgConfig::AVFORMAT AND TARGET PkgConfig::AVCODEC AND TARGET PkgConfig::AVUTIL AND
        TARGET PkgConfig::SWSCALE AND TARGET PkgConfig::SWRESAMPLE AND TARGET PkgConfig::PULSE)
    add_executable(fpshud_recorder_bin
        src/recorder/recorder_main.cpp
        src/recorder/recorder_player.cpp
        ${FPSHUD_GLIBC_COMPAT_SOURCES}
    )

    target_compile_definitions(fpshud_recorder_bin PRIVATE
        FPSHUD_RECORDER_DAEMON=1
    )

    target_include_directories(fpshud_recorder_bin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    )

    target_link_libraries(fpshud_recorder_bin PRIVATE
        PkgConfig::AVFORMAT
        PkgConfig::AVCODEC
        PkgConfig::AVUTIL
        PkgConfig::SWSCALE
        PkgConfig::SWRESAMPLE
        PkgConfig::PULSE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )

    set_target_properties(fpshud_recorder_bin PROPERTIES
        BUILD_RPATH "\$ORIGIN:\$ORIGIN/../lib:/run/host/usr/lib/x86_64-linux-gnu:/run/host/usr/lib/x86_64-linux-gnu/pulseaudio:/run/host/usr/lib64:/run/host/usr/lib:/run/host/usr/lib/pulseaudio:/usr/lib"
        INSTALL_RPATH "\$ORIGIN:\$ORIGIN/../lib:/run/host/usr/lib/x86_64-linux-gnu:/run/host/usr/lib/x86_64-linux-gnu/pulseaudio:/run/host/usr/lib64:/run/host/usr/lib:/run/host/usr/lib/pulseaudio:/usr/lib"
    )
    target_link_options(fpshud_recorder_bin PRIVATE
        "-Wl,--disable-new-dtags"
        ${FPSHUD_GLIBC_COMPAT_LINK_OPTS}
    )

    # Universal lightweight container-aware launcher
    add_executable(fpshud_recorder
        src/recorder/recorder_launcher.c
        ${FPSHUD_GLIBC_COMPAT_SOURCES}
    )
    target_link_libraries(fpshud_recorder PRIVATE
        ${CMAKE_DL_LIBS}
    )
    target_link_options(fpshud_recorder PRIVATE
        ${FPSHUD_GLIBC_COMPAT_LINK_OPTS}
    )

    message(STATUS "Screen Recorder: ENABLED [Dynamic System FFmpeg] (fpshud_recorder & fpshud_recorder_bin will be built)")
else()
    message(WARNING "Screen Recorder: DISABLED (libavformat, libavcodec, libswscale, libswresample, or libpulse not found)")
endif()

# Generate Vulkan Implicit Layer JSON manifest with absolute path to libfpshud.so
set(FPSHUD_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/libfpshud.so")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest/VkLayer_FPSHUD_overlay.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_FPSHUD_overlay.json"
    @ONLY
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fpshud.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fpshud"
    @ONLY
)
file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/fpshud"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

# Automated Static Analysis with clang-tidy
find_program(CLANG_TIDY_EXE NAMES clang-tidy)
if(CLANG_TIDY_EXE)
    set(SRC_FILES_FOR_TIDY
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/config.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/logger.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/telemetry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/analytics/frametime_analyzer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/analytics/benchmark_recorder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/cpu_provider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/ram_provider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/system_provider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/gpu_nvidia.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/gpu_drm.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/gpu_levelzero.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/png_writer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/recorder_ipc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends/evdev_backend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends/wayland_backend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends/x11_backend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/input_manager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/gui_menu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/overlay_ui.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_hooks.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_swapchain.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_capture.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_player_stream.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_layer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/opengl_hook.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/recorder_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.cpp
    )
    add_custom_target(clang-tidy
        COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_CURRENT_BINARY_DIR}
            ${SRC_FILES_FOR_TIDY}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy static analysis on src/..."
        VERBATIM
    )
    message(STATUS "Static Analysis: clang-tidy target AVAILABLE ('ninja clang-tidy')")
endif()

# Automated Code Formatting with clang-format (Google Style)
find_program(CLANG_FORMAT_EXE NAMES clang-format clang-format-23 clang-format-20 clang-format-19 clang-format-18)
if(CLANG_FORMAT_EXE)
    set(FORMAT_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/config.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/config.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/logger.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/logger.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/pack_indexing.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/sysfs_util.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/telemetry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/telemetry.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/analytics/frametime_analyzer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/analytics/frametime_analyzer.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/analytics/benchmark_recorder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/analytics/benchmark_recorder.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/system_provider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/system_provider.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/cpu_provider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/cpu_provider.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/ram_provider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/ram_provider.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/gpu_nvidia.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/gpu_nvidia.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/gpu_drm.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/gpu_drm.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/gpu_levelzero.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry/providers/gpu_levelzero.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends/evdev_backend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends/evdev_backend.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends/wayland_backend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends/wayland_backend.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends/x11_backend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/backends/x11_backend.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/input_manager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input/input_manager.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/gui_menu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/gui_menu.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/overlay_ui.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/overlay_ui.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/png_writer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/png_writer.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/recorder_ipc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/recorder_ipc.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/recorder_playback_protocol.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/recorder_player.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/recorder_player.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/recorder_launcher.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder/recorder_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_hooks.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_hooks.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_swapchain.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_swapchain.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_capture.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_capture.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_player_stream.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_player_stream.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/vulkan_layer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/opengl_hook.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/layer/glibc_compat.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/experimental-feature/adaptive_vfr.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/experimental-feature/adaptive_vfr.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_common.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp
    )

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i ${FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Auto-formatting FPSHUD C/C++ source code with clang-format (Google Style)..."
        VERBATIM
    )

    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking FPSHUD code format compliance with clang-format (Google Style)..."
        VERBATIM
    )
    message(STATUS "Code Formatter: clang-format targets AVAILABLE ('ninja format', 'ninja format-check')")
endif()
