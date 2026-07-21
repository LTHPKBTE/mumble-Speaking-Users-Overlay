/*
 * glfw_attrib_wrapper.h — Macro redirect for glfwSetWindowAttrib
 *
 * Include this header BEFORE imgui_impl_glfw.cpp so that every
 * call to glfwSetWindowAttrib() in the Dear ImGui GLFW backend is
 * redirected to OverlayGlfwSetWindowAttrib(), which deduplicates
 * redundant calls (e.g. GLFW_MOUSE_PASSTHROUGH at 320 fps → 3.37 % CPU).
 *
 * DO NOT include this header in glfw_attrib_wrapper.cpp itself —
 * that file provides the actual OverlayGlfwSetWindowAttrib()
 * implementation and must call the real ::glfwSetWindowAttrib().
 */

#pragma once

#include <GLFW/glfw3.h>

void OverlayGlfwSetWindowAttrib(GLFWwindow* window, int attrib, int value);

#define glfwSetWindowAttrib OverlayGlfwSetWindowAttrib
