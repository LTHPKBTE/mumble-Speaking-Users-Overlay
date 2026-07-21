# Technical Notes

This document covers implementation details and design decisions. For usage, build, and installation, see [README.md](./README.md).

!! Notice: This document was generated autonomously by AI; I make no guarantees regarding its accuracy or reliability, and I sincerely apologize for any errors it may contain. !!

## Table of Contents

- [Framerate Control](#framerate-control)
- [Architecture](#architecture)
- [GPU Driver VSync Busy-Wait](#gpu-driver-vsync-busy-wait)
- [High-Resolution Frame Timer](#high-resolution-frame-timer)
- [glfwSetWindowAttrib Wrapper](#glfwsetwindowattrib-wrapper)

---

## Framerate Control

### Priority System

The effective target FPS is recalculated every frame based on the overlay's current state:

| Priority | State | Config Key | Default |
|---|---|---|---|
| 1 (highest) | Settings panel is open | `fps_settings_open` | 60 |
| 2 | Window is clickable (not in passthrough mode) | `fps_clickable` | 60 |
| 3 | Passthrough mode + no mouse activity for `idle_fps_timeout` seconds | `fps_idle` | 4 |
| 4 (lowest) | Passthrough mode + recent mouse activity | `fps_passthrough` | 15 |

When VSync is enabled (`vsync_enabled = true`), all FPS profiles are ignored and `glfwSwapInterval(1)` is used instead.

### Auto-Detect Monitor Refresh Rate

When `auto_detect_refresh` is enabled (default):

1. The plugin queries the monitor that contains the window's centre point via GLFW:
   ```
   glfwGetWindowPos/Size → window centre → glfwGetMonitors workarea intersection → glfwGetVideoMode.refreshRate
   ```
2. The detected rate is validated to **30–350 Hz** (rounded to nearest integer). Values outside this range are clamped.
3. The validated rate is written to both `fps_clickable` and `fps_settings_open`.
4. Detection fires on:
   - Plugin initialization (after GLFW window creation)
   - User toggling `auto_detect_refresh` from off → on
   - `WM_DISPLAYCHANGE` Windows message (resolution change, refresh rate change, monitor hot-plug)
   - **Reset All Settings** button
5. `WM_DISPLAYCHANGE` is a low-frequency system event. It does **not** fire on window moves, DPI changes, or regular painting — no polling overhead.

### Idle Detection

Mouse activity is tracked per-frame via ImGui IO:

```c
ImGuiIO& io = ImGui::GetIO();
if (io.MouseMoved || io.MouseDown[0..2] || io.WantCaptureMouse) {
    g_last_user_input_time = ImGui::GetTime();
}
```

The idle timeout only matters in **passthrough mode with the settings panel closed**. When the window is clickable, the idle timer is continuously reset (user may interact).

### Framerate Limiter Implementation

The limiter runs after `glfwSwapBuffers`:

1. Calculate effective target FPS from priority rules.
2. Compute `remaining = (1.0 / target_fps) - elapsed_since_last_frame`.
3. If `remaining > 2 ms`: sleep via `SetWaitableTimer` + `WaitForSingleObject`.

```c
LARGE_INTEGER due;
due.QuadPart = (LONGLONG)(remaining * -10000000.0);  // 100 ns units, negative = relative
SetWaitableTimer(g_frame_timer, &due, 0, NULL, NULL, FALSE);
WaitForSingleObject(g_frame_timer, INFINITE);
```

### Hidden Window Low-Power Mode

When the overlay is hidden (user clicked X), the render loop calls `glfwWaitEventsTimeout(0.05)` — processes OS messages and sleeps up to 50 ms. CPU usage drops to near zero while still responding promptly to the `Ctrl+Shift+H` hotkey.

---

## High-Resolution Frame Timer

### Primary Path (Windows 10 1803+)

```c
g_frame_timer = CreateWaitableTimerExW(NULL, NULL,
    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
```

- **No global side effects** — does not change the system timer resolution.
- Sub-millisecond precision.
- This works on the vast majority of Windows installations.

### Fallback Path (Older Windows)

If `CreateWaitableTimerExW` returns NULL:

```c
g_frame_timer = CreateWaitableTimer(NULL, TRUE, NULL);
timeBeginPeriod(1);
g_using_time_period = true;
atexit(cleanup_time_period);
```

- `timeBeginPeriod(1)` raises the system-wide timer resolution to ~1 ms.
- `atexit(cleanup_time_period)` ensures `timeEndPeriod(1)` is called even on abnormal program exit (crash, `abort()`, etc.).
- Normal shutdown also calls `cleanup_time_period()` via `overlay_window_shutdown()`.

### Why Not Sleep()

`Sleep(n)` has ~15.6 ms granularity by default, which is too coarse for framerate limiting (e.g., 60 FPS = 16.67 ms per frame). `timeBeginPeriod(1)` improves `Sleep()` to ~1 ms, but it's a system-wide setting that affects all applications and increases power consumption. The waitable timer approach avoids this entirely on modern Windows.

---

## GPU Driver VSync Busy-Wait

When VSync is enabled (`glfwSwapInterval(1)`), the CPU waits in `glfwSwapBuffers` for the GPU's vertical blank interval. Some GPU drivers (notably NVIDIA and AMD) implement this wait as a **busy-loop** rather than an interrupt-based sleep, causing:

- CPU usage pinned to 100% of one core at high refresh rates (e.g., 144 Hz).
- The effect is especially pronounced for applications like overlays that have minimal GPU work per frame.

Set `glfwSwapInterval(0)` (VSync off) and use the frame limiter instead. The limiter uses a proper OS waitable timer that puts the thread to sleep, consuming near-zero CPU between frames.

## glfwSetWindowAttrib Wrapper

The GLFW upstream function `_glfwSetWindowMousePassthroughWin32` calls expensive Win32 API functions (`SetWindowLongPtr`, `SetWindowPos`, `SetLayeredWindowAttributes`) every frame, even when the attribute hasn't changed. This was costing ~70 µs per frame at 144 Hz (wasted work, though not the dominant CPU consumer — the GPU driver vsync busy-wait was).

### Solution

A header-only wrapper macro in `src/glfw_attrib_wrapper.h`:

```c
#define glfwSetWindowAttrib OverlayGlfwSetWindowAttrib
```

`OverlayGlfwSetWindowAttrib` uses an `unordered_map<AttribKey, int>` cache to track the last-set value for each `(window, attrib)` pair. If the value hasn't changed, the call is skipped.

### Coverage

Only two attributes are cached: `GLFW_MOUSE_PASSTHROUGH` and `GLFW_FLOATING`. Other attributes pass through unchanged.

Statistics (skipped/passed/ratio) are logged every 10 seconds when `debug_show_fps` is enabled.

### Files

- `src/glfw_attrib_wrapper.h` — macro + function declaration
- `src/glfw_attrib_wrapper.cpp` — cache implementation
- `src/imgui_impl_glfw_overlay.cpp` — includes the wrapper, then `#include`s the original backend

No changes to the GLFW submodule required.

---

## Architecture

```
[Mumble main thread]                [Render thread]
  callbacks --> speaking_users (mutex) <-- poll callback (read only)
  MumbleAPI allowed                  MumbleAPI forbidden
```

- **Main thread**: Runs Mumble callbacks (`onUserTalkingStateChanged`, etc.), writes to `speaking_users` under a mutex.
- **Render thread**: Owns the GLFW window and ImGui context. Polls `speaking_users` (read-only, mutex held briefly). Must not call Mumble API (thread-safety restriction).
- **Config persistence**: Saved to `%APPDATA%\Mumble\SpeakingOverlay.cfg` on every settings change and at shutdown. Loaded on startup with backward compatibility for old config keys.

### Source Files

| File | Purpose |
|---|---|
| `src/plugin.c` | Mumble plugin entry point, callbacks, main/render thread management |
| `src/overlay_window.c` | GLFW window, ImGui rendering, settings panel, frame limiter |
| `src/overlay_window.h` | Config struct, public API |
| `src/speaking_users.c` | Thread-safe speaking user list (mutex-protected) |
| `src/speaking_users.h` | Speaking user API |
| `src/render_thread.c` | Render thread loop |
| `src/render_thread.h` | Render thread API |
| `src/glfw_attrib_wrapper.h` | GLFW attrib dedup macro |
| `src/glfw_attrib_wrapper.cpp` | GLFW attrib dedup cache |
| `src/imgui_impl_glfw_overlay.cpp` | ImGui GLFW backend (includes wrapper) |
