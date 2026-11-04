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
    message(FATAL_ERROR
        "FPSHUD requires an ISO C++26 compiler and standard library providing <inplace_vector> "
        "(GCC 16+ or Clang 20+ with libstdc++ 16+).\n"
        "On Ubuntu/Debian: install g++-16 (Ubuntu 26.04 universe, or ppa:ubuntu-toolchain-r/test) and pass -DCMAKE_CXX_COMPILER=g++-16.")
endif()

option(FPSHUD_NATIVE_ARCH "Optimize for host CPU microarchitecture (-march=native)" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer and LeakSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

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
    set_source_files_properties(src/glibc_compat.c PROPERTIES COMPILE_OPTIONS "-fno-lto")
    set(FPSHUD_GLIBC_COMPAT_SOURCES src/glibc_compat.c)
    set(FPSHUD_GLIBC_COMPAT_LINK_OPTS
        -Wl,--wrap=acosf
        -Wl,--wrap=atan2f
        -Wl,--wrap=fmodf
        -Wl,--wrap=__isoc23_sscanf
        -Wl,--wrap=__isoc23_strtol
        -Wl,--wrap=__isoc23_strtoul
        -Wl,--wrap=__isoc23_strtoull
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

    if(ENABLE_ASAN)
        message(STATUS "  -> AddressSanitizer + LeakSanitizer: ENABLED")
        add_compile_options(-fsanitize=address,leak)
        add_link_options(-fsanitize=address,leak)
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
else()
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

# Automatically generate src/embedded_fonts.hpp if not already present
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_embedded_fonts.py" "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_fonts.hpp"
    )
endif()

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

add_library(fpshud SHARED
    src/config.cpp
    src/telemetry.cpp
    src/recorder_ipc.cpp
    src/input_manager.cpp
    src/gui_menu.cpp
    src/overlay_ui.cpp
    src/vulkan_layer.cpp
    src/opengl_hook.cpp
    ${IMGUI_SOURCES}
    ${FPSHUD_GLIBC_COMPAT_SOURCES}
)

target_include_directories(fpshud PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
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
    src/test_cli.cpp
    src/config.cpp
    src/telemetry.cpp
    src/recorder_ipc.cpp
    ${FPSHUD_GLIBC_COMPAT_SOURCES}
)

target_include_directories(fpshud_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(fpshud_test PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

target_link_options(fpshud_test PRIVATE
    ${FPSHUD_GLIBC_COMPAT_LINK_OPTS}
)

# Screen Recorder Helper Daemon (On-Demand)
find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(AVFORMAT IMPORTED_TARGET libavformat)
    pkg_check_modules(AVCODEC IMPORTED_TARGET libavcodec)
    pkg_check_modules(AVUTIL IMPORTED_TARGET libavutil)
    pkg_check_modules(SWSCALE IMPORTED_TARGET libswscale)
    pkg_check_modules(SWRESAMPLE IMPORTED_TARGET libswresample)
    pkg_check_modules(PULSE IMPORTED_TARGET libpulse)
endif()

if (TARGET PkgConfig::AVFORMAT AND TARGET PkgConfig::AVCODEC AND TARGET PkgConfig::AVUTIL AND
    TARGET PkgConfig::SWSCALE AND TARGET PkgConfig::SWRESAMPLE AND TARGET PkgConfig::PULSE)
    add_executable(fpshud_recorder_bin
        src/recorder_main.cpp
        ${FPSHUD_GLIBC_COMPAT_SOURCES}
    )

    target_include_directories(fpshud_recorder_bin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
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
        src/recorder_launcher.c
        ${FPSHUD_GLIBC_COMPAT_SOURCES}
    )
    target_link_libraries(fpshud_recorder PRIVATE
        ${CMAKE_DL_LIBS}
    )
    target_link_options(fpshud_recorder PRIVATE
        ${FPSHUD_GLIBC_COMPAT_LINK_OPTS}
    )

    message(STATUS "Screen Recorder: ENABLED (fpshud_recorder & fpshud_recorder_bin will be built)")
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder_ipc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/input_manager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui_menu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/overlay_ui.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_layer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/opengl_hook.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recorder_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/test_cli.cpp
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
