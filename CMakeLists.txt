cmake_minimum_required(VERSION 3.20)
project(FPSHUD VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_ASAN "Enable AddressSanitizer and LeakSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN)
    set(USE_SANITIZERS ON)
else()
    set(USE_SANITIZERS OFF)
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
            -Wno-deprecated-declarations
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
            -Wno-deprecated-declarations
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
        -fno-math-errno
        -fno-trapping-math
        -Wno-deprecated-declarations
    )
    add_link_options(
        -Wl,--gc-sections
        -Wl,-O2
        -static-libstdc++
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
find_package(OpenGL REQUIRED COMPONENTS OpenGL GLX EGL)
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
)

target_include_directories(fpshud PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(fpshud PRIVATE
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES
)

target_link_libraries(fpshud PRIVATE
    Vulkan::Headers
    OpenGL::GL
    OpenGL::GLX
    OpenGL::EGL
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

target_link_options(fpshud PRIVATE
    -Wl,--exclude-libs,ALL
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/manifest/libfpshud.version"
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
)

target_include_directories(fpshud_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(fpshud_test PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# Screen Recorder Helper Daemon (On-Demand)
find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(AVFORMAT IMPORTED_TARGET libavformat)
    pkg_check_modules(AVCODEC IMPORTED_TARGET libavcodec)
    pkg_check_modules(AVUTIL IMPORTED_TARGET libavutil)
    pkg_check_modules(SWSCALE IMPORTED_TARGET libswscale)
    pkg_check_modules(SWRESAMPLE IMPORTED_TARGET libswresample)
    pkg_check_modules(PULSE_SIMPLE IMPORTED_TARGET libpulse-simple)
endif()

if (TARGET PkgConfig::AVFORMAT AND TARGET PkgConfig::AVCODEC AND TARGET PkgConfig::AVUTIL AND
    TARGET PkgConfig::SWSCALE AND TARGET PkgConfig::SWRESAMPLE AND TARGET PkgConfig::PULSE_SIMPLE)
    add_executable(fpshud_recorder_bin
        src/recorder_main.cpp
        src/config.cpp
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
        PkgConfig::PULSE_SIMPLE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )

    set_target_properties(fpshud_recorder_bin PROPERTIES
        BUILD_RPATH "\$ORIGIN:\$ORIGIN/../lib:/run/host/usr/lib:/run/host/usr/lib/pulseaudio:/usr/lib"
        INSTALL_RPATH "\$ORIGIN:\$ORIGIN/../lib:/run/host/usr/lib:/run/host/usr/lib/pulseaudio:/usr/lib"
    )
    target_link_options(fpshud_recorder_bin PRIVATE
        "-Wl,--disable-new-dtags"
    )

    # Universal lightweight container-aware launcher
    add_executable(fpshud_recorder
        src/recorder_launcher.c
    )
    target_link_libraries(fpshud_recorder PRIVATE
        ${CMAKE_DL_LIBS}
    )

    message(STATUS "Screen Recorder: ENABLED (fpshud_recorder & fpshud_recorder_bin will be built)")
else()
    message(WARNING "Screen Recorder: DISABLED (libavformat, libavcodec, libswscale, libswresample, or libpulse-simple not found)")
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
