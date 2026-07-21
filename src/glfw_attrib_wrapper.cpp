/*
 * glfw_attrib_wrapper.cpp — Deduplication wrapper for glfwSetWindowAttrib
 *
 * ImGui's GLFW backend calls glfwSetWindowAttrib(GLFW_MOUSE_PASSTHROUGH, ...)
 * every frame (~320 fps).  GLFW unconditionally forwards every call to the
 * Win32 platform layer (_glfwSetWindowMousePassthroughWin32 → SetWindowLongW),
 * consuming ~3.37 % CPU for a no-op state re-application.
 *
 * This wrapper intercepts those calls (via the macro in glfw_attrib_wrapper.h)
 * and skips the platform call when the value hasn't changed.
 *
 * NOTE: this file must NOT include glfw_attrib_wrapper.h (which would
 * recursively macro-expand ::glfwSetWindowAttrib).  It uses the real GLFW API
 * directly.
 */

#include <GLFW/glfw3.h>
#include <cstdio>
#include <unordered_map>

namespace {

struct AttribKey {
    GLFWwindow *window;
    int         attrib;

    bool operator==(const AttribKey &o) const {
        return window == o.window && attrib == o.attrib;
    }
};

struct AttribKeyHash {
    std::size_t operator()(const AttribKey &k) const {
        return reinterpret_cast<std::size_t>(k.window) ^
               (static_cast<std::size_t>(k.attrib) << 16);
    }
};

/*
 * Cache of last-known value per (window, attrib).
 * All callers run on the render thread → no mutex needed.
 */
std::unordered_map<AttribKey, int, AttribKeyHash> g_cache;

/* Verification: count deduplicated calls, log once after warm-up. */
long long g_skipped_count   = 0;
long long g_passed_count    = 0;
double    g_last_verify_time = 0.0;
bool      g_verify_done      = false;

} // anonymous namespace

void OverlayGlfwSetWindowAttrib(GLFWwindow *window, int attrib, int value)
{
    /* Only deduplicate attributes that are known to be called redundantly
     * every frame.  Everything else passes through unchanged. */
    if (attrib == GLFW_MOUSE_PASSTHROUGH || attrib == GLFW_FLOATING) {
        const AttribKey key{window, attrib};
        const auto it = g_cache.find(key);
        if (it != g_cache.end() && it->second == value) {
            g_skipped_count++;
            /* Log verification once after a few seconds of runtime */
            if (!g_verify_done && g_skipped_count > 500) {
                g_verify_done = true;
                fprintf(stderr, "[wrapper] dedup active: %lld calls skipped (%.1f%% saved)\n",
                        g_skipped_count,
                        100.0 * (double)g_skipped_count / (double)(g_skipped_count + g_passed_count));
            }
            return; // unchanged — skip the expensive platform syscall
        }
        g_cache[key] = value;
    }

    g_passed_count++;
    ::glfwSetWindowAttrib(window, attrib, value);
}
