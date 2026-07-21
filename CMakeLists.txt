# Mumble Speaking Users Overlay Plugin
# ============================================
# Overlay window showing currently speaking Mumble users.
#
# Dependencies (git submodules):
#   3rdparty/cimgui  -- Dear ImGui C binding
#   3rdparty/glfw    -- GLFW library

cmake_minimum_required(VERSION 3.16)

project(SpeakingUsersOverlay
    VERSION "1.0.0"
    DESCRIPTION "Mumble plugin: overlay window showing currently speaking users"
    LANGUAGES C CXX
)

# ============================================================
# Options
# ============================================================
option(OVERLAY_BUILD_STANDALONE "Standalone test mode (no Mumble required)" OFF)

# ============================================================
# Path constants
# ============================================================
set(CIMGUI_ROOT   "${CMAKE_SOURCE_DIR}/3rdparty/cimgui")
set(IMGUI_ROOT    "${CIMGUI_ROOT}/imgui")
set(IMGUI_BACKEND "${IMGUI_ROOT}/backends")
set(GLFW_ROOT     "${CMAKE_SOURCE_DIR}/3rdparty/glfw")

# ============================================================
# GLFW
# ============================================================
if(EXISTS "${GLFW_ROOT}/CMakeLists.txt")
    message(STATUS "Using local GLFW subdirectory: ${GLFW_ROOT}")
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${GLFW_ROOT} EXCLUDE_FROM_ALL)
    set(GLFW_LIBRARIES glfw)
    set(GLFW_INCLUDE_DIR  "${GLFW_ROOT}/include")
else()
    find_package(glfw3 REQUIRED)
    set(GLFW_LIBRARIES    glfw)
    set(GLFW_INCLUDE_DIR  "${glfw3_INCLUDE_DIRS}")
endif()

# ============================================================
# cimgui static library (compiled as C++)
# ============================================================
if(NOT EXISTS "${CIMGUI_ROOT}/cimgui.h")
    message(FATAL_ERROR "cimgui not found. Ensure 3rdparty/cimgui submodule is initialised.")
endif()

# ImGui core sources
set(IMGUI_CORE
    ${IMGUI_ROOT}/imgui.cpp
    ${IMGUI_ROOT}/imgui_draw.cpp
    ${IMGUI_ROOT}/imgui_widgets.cpp
    ${IMGUI_ROOT}/imgui_tables.cpp
    ${IMGUI_ROOT}/imgui_demo.cpp
)

# cimgui C binding (without cimgui_impl.cpp -- redeclares backend functions)
set(CIMGUI_BINDING
    ${CIMGUI_ROOT}/cimgui.cpp
)

# Backend implementations (C++)
# imgui_impl_glfw.cpp is wrapped via imgui_impl_glfw_overlay.cpp to deduplicate
# redundant glfwSetWindowAttrib() calls (e.g. MOUSE_PASSTHROUGH at ~320 fps).
set(CIMGUI_BACKENDS
    ${CMAKE_SOURCE_DIR}/src/imgui_impl_glfw_overlay.cpp
    ${CMAKE_SOURCE_DIR}/src/glfw_attrib_wrapper.cpp
    ${IMGUI_BACKEND}/imgui_impl_opengl3.cpp
)

add_library(cimgui STATIC
    ${IMGUI_CORE}
    ${CIMGUI_BINDING}
    ${CIMGUI_BACKENDS}
)

target_include_directories(cimgui PUBLIC
    "${CIMGUI_ROOT}"
    "${IMGUI_ROOT}"
    "${IMGUI_BACKEND}"
    "${GLFW_INCLUDE_DIR}"
)
# Wrapper files in src/ need glfw_attrib_wrapper.h
target_include_directories(cimgui PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(cimgui PUBLIC
    ${GLFW_LIBRARIES}
)

target_compile_definitions(cimgui PUBLIC
    CIMGUI_USE_GLFW
    CIMGUI_USE_OPENGL3
    IMGUI_DISABLE_DEMO_WINDOWS
)

# Required for linking cimgui into a shared library (Linux)
set_target_properties(cimgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(cimgui PUBLIC opengl32)
target_compile_definitions(cimgui PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

target_compile_features(cimgui PUBLIC cxx_std_11)

# ============================================================
# Plugin shared library (compiled as C++ for MumblePlugin.h compatibility)
# ============================================================
add_library(plugin SHARED
    src/plugin.c
    src/speaking_users.c
    src/overlay_window.c
    src/render_thread.c
)

# Force C++ compilation (MumblePlugin.h requires C++)
set_source_files_properties(
    src/plugin.c
    src/speaking_users.c
    src/overlay_window.c
    src/render_thread.c
    PROPERTIES LANGUAGE CXX
)

target_include_directories(plugin PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src"
    "${GLFW_INCLUDE_DIR}"
)
target_include_directories(plugin SYSTEM PRIVATE
    "${CIMGUI_ROOT}"
    "${IMGUI_ROOT}"
)

target_link_libraries(plugin PRIVATE
    cimgui
    ${GLFW_LIBRARIES}
    winmm
)

# Compiler flags (MSVC)
target_compile_definitions(plugin PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(plugin PRIVATE /W3)

# Output basename (no lib prefix)
set_target_properties(plugin PROPERTIES PREFIX "")

# ============================================================
# Install rules
# ============================================================
install(TARGETS plugin
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
)

# ============================================================
# Standalone test executable
# ============================================================
if(OVERLAY_BUILD_STANDALONE)
    add_executable(overlay_test
        test/test_standalone.c
        src/overlay_window.c
        src/render_thread.c
        src/speaking_users.c
    )
    set_source_files_properties(
        test/test_standalone.c
        src/overlay_window.c
        src/render_thread.c
        src/speaking_users.c
        PROPERTIES LANGUAGE CXX
    )
    target_include_directories(overlay_test PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src"
        "${GLFW_INCLUDE_DIR}"
    )
    target_include_directories(overlay_test SYSTEM PRIVATE
        "${CIMGUI_ROOT}"
        "${IMGUI_ROOT}"
    )
    target_link_libraries(overlay_test PRIVATE
        cimgui
        ${GLFW_LIBRARIES}
        winmm
    )
    target_compile_definitions(overlay_test PRIVATE OVERLAY_STANDALONE_TEST)
    target_compile_definitions(overlay_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
