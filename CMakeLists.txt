cmake_minimum_required(VERSION 3.20)
project(FPSHUD VERSION 1.0.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_compile_options(
    -O3
    -DNDEBUG
    -ffunction-sections
    -fdata-sections
    -fno-math-errno
    -fno-trapping-math
)

add_link_options(
    -Wl,--gc-sections
    -Wl,-O2
    -static-libstdc++
    -static-libgcc
)

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
)

target_include_directories(fpshud_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(fpshud_test PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

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
