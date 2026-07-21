/*
 * imgui_impl_glfw_overlay.cpp — Thin wrapper around Dear ImGui GLFW backend
 *
 * Compiles the stock imgui_impl_glfw.cpp with glfw_attrib_wrapper.h
 * included first, so every glfwSetWindowAttrib() call inside the backend
 * is redirected through OverlayGlfwSetWindowAttrib().
 *
 * This file REPLACES imgui_impl_glfw.cpp in the build — do not compile
 * the original alongside it.
 */

#include "glfw_attrib_wrapper.h"

/* Include the original backend source directly. */
#include "imgui_impl_glfw.cpp"
