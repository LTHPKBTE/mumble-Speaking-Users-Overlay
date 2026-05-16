# Mumble Speaking Users Overlay Plugin
# ============================================
# 在悬浮窗中显示当前正在说话的 Mumble 用户列表
#
# 依赖:
#   3rdparty/cimgui  — Dear ImGui C binding (git submodule)
#   3rdparty/glfw    — GLFW library (git submodule)

cmake_minimum_required(VERSION 3.16)

project(SpeakingUsersOverlay
    VERSION "1.0.0"
    DESCRIPTION "Mumble 插件: 悬浮窗显示当前说话玩家列表"
    LANGUAGES C CXX
)

# ============================================================
# 选项
# ============================================================
option(OVERLAY_BUILD_STANDALONE "独立测试模式 (无需 Mumble)" OFF)

# ============================================================
# 路径常量
# ============================================================
set(CIMGUI_ROOT   "${CMAKE_SOURCE_DIR}/3rdparty/cimgui")
set(IMGUI_ROOT    "${CIMGUI_ROOT}/imgui")
set(IMGUI_BACKEND "${IMGUI_ROOT}/backends")
set(GLFW_ROOT     "${CMAKE_SOURCE_DIR}/3rdparty/glfw")

# ============================================================
# GLFW
# ============================================================
if(EXISTS "${GLFW_ROOT}/CMakeLists.txt")
    message(STATUS "使用本地 GLFW 子目录: ${GLFW_ROOT}")
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # Linux CI 通常缺 wayland-scanner，只构建 X11
    if(UNIX AND NOT APPLE)
        set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_X11     ON  CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(${GLFW_ROOT} EXCLUDE_FROM_ALL)
    set(GLFW_LIBRARIES glfw)
    set(GLFW_INCLUDE_DIR  "${GLFW_ROOT}/include")
else()
    find_package(glfw3 REQUIRED)
    set(GLFW_LIBRARIES    glfw)
    set(GLFW_INCLUDE_DIR  "${glfw3_INCLUDE_DIRS}")
endif()

# ============================================================
# cimgui 静态库 (C++ 编译)
# ============================================================
if(NOT EXISTS "${CIMGUI_ROOT}/cimgui.h")
    message(FATAL_ERROR "找不到 cimgui。请确保 3rdparty/cimgui 子模块已初始化。")
endif()

# ImGui 核心源文件
set(IMGUI_CORE
    ${IMGUI_ROOT}/imgui.cpp
    ${IMGUI_ROOT}/imgui_draw.cpp
    ${IMGUI_ROOT}/imgui_widgets.cpp
    ${IMGUI_ROOT}/imgui_tables.cpp
    ${IMGUI_ROOT}/imgui_demo.cpp
)

# cimgui C 绑定 (不含 cimgui_impl.cpp — 它只做转发，会和后端重复声明)
set(CIMGUI_BINDING
    ${CIMGUI_ROOT}/cimgui.cpp
)

# 后端实现 (C++)
set(CIMGUI_BACKENDS
    ${IMGUI_BACKEND}/imgui_impl_glfw.cpp
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

target_link_libraries(cimgui PUBLIC
    ${GLFW_LIBRARIES}
)

target_compile_definitions(cimgui PUBLIC
    CIMGUI_USE_GLFW
    CIMGUI_USE_OPENGL3
    IMGUI_DISABLE_DEMO_WINDOWS
)

if(WIN32)
    target_link_libraries(cimgui PUBLIC opengl32)
    target_compile_definitions(cimgui PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
elseif(APPLE)
    find_library(OpenGL_LIB OpenGL REQUIRED)
    target_link_libraries(cimgui PUBLIC ${OpenGL_LIB})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(cimgui PUBLIC OpenGL::GL)
endif()

target_compile_features(cimgui PUBLIC cxx_std_11)

# ============================================================
# 我们的插件共享库 (C 编译)
# ============================================================
add_library(plugin SHARED
    src/plugin.c
    src/speaking_users.c
    src/overlay_window.c
    src/render_thread.c
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
)

# 编译选项
if(MSVC)
    target_compile_definitions(plugin PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(plugin PRIVATE /W3)
else()
    target_compile_options(plugin PRIVATE -Wall -Wextra)
endif()

# 输出名称
set_target_properties(plugin PROPERTIES PREFIX "")

# ============================================================
# 安装规则
# ============================================================
install(TARGETS plugin
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
)

# ============================================================
# 独立测试可执行文件
# ============================================================
if(OVERLAY_BUILD_STANDALONE)
    add_executable(overlay_test
        test/test_standalone.c
        src/overlay_window.c
        src/render_thread.c
        src/speaking_users.c
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
    )
    target_compile_definitions(overlay_test PRIVATE OVERLAY_STANDALONE_TEST)
    if(MSVC)
        target_compile_definitions(overlay_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()
