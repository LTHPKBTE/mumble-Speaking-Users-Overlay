# Technical Notes

This document covers implementation details and design decisions. For usage, build, and installation, see [README.md](./README.md).

!! Notice: This document was generated autonomously by AI; I make no guarantees regarding its accuracy or reliability, and I sincerely apologize for any errors it may contain. !!

## Table of Contents

- [Framerate Control](#framerate-control)
- [Global Hotkeys (RegisterHotKey)](#global-hotkeys-registerhotkey)
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
| 3 (lowest) | Passthrough mode | `fps_passthrough` | 15 |

When VSync is enabled (`vsync_enabled = true`), all FPS profiles are ignored and `glfwSwapInterval(1)` is used instead.

### Legacy Config Upgrade (VSync)

Old versions of the plugin stored framerate settings under a different key (`custom_fps_enabled`). When a legacy config file is detected on load:

- VSync is **forced off** regardless of the old setting — the GPU driver busy-wait behaviour (see [GPU Driver VSync Busy-Wait](#gpu-driver-vsync-busy-wait)) makes it undesirable for an overlay at typical refresh rates.
- The settings panel auto-opens on the first frame with a notice.
- The user can re-enable VSync in the settings panel if desired; the new choice is saved normally and not overridden on subsequent launches.
- Detection is based on the presence of the `custom_fps_enabled` key in the config file — once the file is saved in the new format that key is no longer written.

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

### Framerate Limiter Implementation

The limiter runs after `glfwSwapBuffers`:

1. Calculate effective target FPS from priority rules.
2. Compute `remaining = last_frame_time + (1.0 / target_fps) - now`.
3. Sleep the entire remaining time via `SetWaitableTimer` + `WaitForSingleObject`.

```c
if (remaining > 0.0 && g_frame_timer != NULL) {
    LARGE_INTEGER due;
    due.QuadPart = (LONGLONG)(remaining * -10000000.0);  // 100ns units
    SetWaitableTimer(g_frame_timer, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(g_frame_timer, INFINITE);
}
```

`g_last_frame_time` is reset to the actual wall-clock time after the wait, so any late-wake jitter (typically < 1 ms) does not accumulate into subsequent frames. For an overlay, the sub-millisecond precision of a high-resolution waitable timer is generally sufficient without additional spin-waiting; the marginal jitter is unlikely to be perceptible at typical overlay FPS targets.

**Preemption behaviour**: If a high-priority process preempts the CPU during the wait, the thread may wake past the deadline. `g_last_frame_time` resets to the actual time, so the next frame's deadline is based on the current wall clock — late-wake error does not propagate.

### Hidden Window Low-Power Mode

When the overlay is hidden (user clicked X), the render loop calls `glfwWaitEventsTimeout(0.05)` — processes OS messages and sleeps up to 50 ms. CPU usage drops to near zero while still responding promptly to the Show Window hotkey (WM_HOTKEY delivered by the kernel even during `glfwWaitEvents`).

---

## Global Hotkeys (RegisterHotKey)

### Overview

Two global hotkeys (Toggle Passthrough and Show Window) are configurable in the Settings panel. The primary mechanism is `RegisterHotKey` with `MOD_NOREPEAT`, delivering `WM_HOTKEY` directly to the window procedure. This replaces the previous `WH_KEYBOARD_LL` hook on the render thread, which could cause system-wide input lag at low FPS on some configurations.

### WH_KEYBOARD_LL Limitations

The old implementation installed a `WH_KEYBOARD_LL` hook on the render thread. When the frame limiter put the render thread to sleep, the hook chain could be blocked — `CallNextHookEx` might not execute until the render thread woke up. On affected systems, this could cause noticeable input lag for other applications.

`RegisterHotKey` mitigates this: the kernel delivers `WM_HOTKEY` asynchronously via the message queue, with effectively no measurable latency on typical systems and no dependency on the render thread being awake.

### Implementation

```c
static bool register_one_hotkey(HWND hwnd, int id, int vk, int mods) {
    if (vk == 0 || hwnd == NULL) return false;
    UINT fsModifiers = (UINT)mods | MOD_NOREPEAT;
    return RegisterHotKey(hwnd, (int)id, fsModifiers, (UINT)vk) != 0;
}
```

- **`MOD_NOREPEAT`**: The hotkey fires once per press, no auto-repeat spam.
- Two hotkey slots: `HOTKEY_ID_TOGGLE` (100) and `HOTKEY_ID_SHOW` (101).
- Two staging slots (102, 103) for atomic re-registration.

### WM_HOTKEY Handler

In `overlay_window_proc`:

```c
if (msg == WM_HOTKEY) {
    int id = (int)wParam;
    if (id == HOTKEY_ID_TOGGLE) {
        InterlockedExchange(&g_hotkey_toggle_passthrough, 1);
        return 0;
    } else if (id == HOTKEY_ID_SHOW) {
        InterlockedExchange(&g_hotkey_show_window, 1);
        return 0;
    }
}
```

Volatile flags (`g_hotkey_toggle_passthrough`, `g_hotkey_show_window`) are checked on the render thread each frame. `InterlockedExchange` provides cross-thread visibility without a mutex on x86/x64 architectures.

### Conflict Detection

On initialization, `register_all_hotkeys()` attempts to register both hotkeys. If either `RegisterHotKey` call fails (returns FALSE — the key is already owned by another process), `g_hotkey_conflict_on_init` is set to `true`. On the **first frame** after `g_first_frame` transitions to false, the settings panel auto-opens with a warning. This only happens on the very first launch — not during gameplay when a hotkey might be temporarily occupied.

### Hotkey Editor

The Settings panel includes an interactive hotkey capture widget:

1. User clicks the hotkey button → capture mode activates.
2. ImGui's `IsKeyPressed()` iterates over `ImGuiKey_NamedKey_BEGIN..END` to find the pressed key.
3. Modifier state (`Ctrl`/`Shift`/`Alt`/`Win`) is read from `ImGuiIO::KeyCtrl/KeyShift/KeyAlt/KeySuper`.
4. `ImGuiKey` is mapped to Win32 VK via bidirectional lookup tables.
5. `ImGui::GetKeyName()` formats the display string.
6. On capture, `unregister_all_hotkeys()` + `register_all_hotkeys()` re-registers with the new key.

**ImGuiKey ↔ VK mapping**:
- Letters: `ImGuiKey_A + (vk - 'A')`, digits: `ImGuiKey_0 + (vk - '0')`, F-keys: `ImGuiKey_F1 + (vk - VK_F1)`
- Punctuation/special keys: individual switch cases (Space, Enter, Tab, arrows, brackets, etc.)
- Reverse mapping uses the same tables in the opposite direction.

### Atomic Re-registration

When the user changes a hotkey, the code first registers the new key on a **staging ID**, then unregisters the old active ID:

```c
static bool replace_registered_hotkey(HWND hwnd, int active_id, int staging_id,
                                      int vk, int mods) {
    if (vk == 0) return false;
    if (!register_one_hotkey(hwnd, staging_id, vk, mods)) return false;
    UnregisterHotKey(hwnd, active_id);
    return true;
}
```

This avoids a gap where the old hotkey is unregistered before the new one is registered, though neither registration is atomic with respect to system-wide hotkey state.

### Config Persistence

```
hotkey_toggle_vk=80   (0x50 = 'P')
hotkey_toggle_mods=3  (MOD_CONTROL | MOD_SHIFT)
hotkey_show_vk=72     (0x48 = 'H')
hotkey_show_mods=3
```

Saved/loaded alongside all other settings. Reset All Settings restores defaults.

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

### Last-Resort Sleep() Fallback

If **both** `CreateWaitableTimerExW` and `CreateWaitableTimer` fail (extremely unlikely):

```c
DWORD sleep_ms = (DWORD)(remaining * 1000.0);
if (sleep_ms > 0) { Sleep(sleep_ms); }
```

On systems with `timeBeginPeriod(1)` active this gives ~1 ms granularity; without it, ~15.6 ms. This path exists as a defensive fallback — in practice, `CreateWaitableTimer` is expected to succeed on nearly all supported Windows versions.

### Why Not Sleep()

`Sleep(n)` has ~15.6 ms granularity by default, which is too coarse for framerate limiting (e.g., 60 FPS = 16.67 ms per frame). `timeBeginPeriod(1)` improves `Sleep()` to ~1 ms, but it's a system-wide setting that affects all applications and can increase power consumption. The waitable timer approach helps avoid this trade-off on modern Windows (10 1803+).

---

## GPU Driver VSync Busy-Wait

When VSync is enabled (`glfwSwapInterval(1)`), the CPU waits in `glfwSwapBuffers` for the GPU's vertical blank interval. Some GPU drivers (notably NVIDIA and AMD) may implement this wait as a **busy-loop** rather than an interrupt-based sleep, which can cause:

- CPU usage near 100% of one core at high refresh rates (e.g., 144 Hz).
- The effect tends to be more noticeable for applications like overlays that have minimal GPU work per frame.

Setting `glfwSwapInterval(0)` (VSync off) and using the frame limiter instead typically avoids this. The limiter uses an OS waitable timer that puts the thread to sleep, generally consuming low CPU between frames.

## glfwSetWindowAttrib Wrapper

The GLFW upstream function `_glfwSetWindowMousePassthroughWin32` calls expensive Win32 API functions (`SetWindowLongPtr`, `SetWindowPos`, `SetLayeredWindowAttributes`) every frame, even when the attribute hasn't changed. This was costing ~70 µs per frame at 144 Hz (redundant work on these specific systems, though not the dominant CPU consumer — the GPU driver vsync busy-wait was the larger factor).

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
| `src/overlay_window.c` | GLFW window, ImGui rendering, settings panel, frame limiter, hotkey system (RegisterHotKey) |
| `src/overlay_window.h` | Config struct, public API |
| `src/speaking_users.c` | Thread-safe speaking user list (mutex-protected) |
| `src/speaking_users.h` | Speaking user API |
| `src/render_thread.c` | Render thread loop |
| `src/render_thread.h` | Render thread API |
| `src/glfw_attrib_wrapper.h` | GLFW attrib dedup macro |
| `src/glfw_attrib_wrapper.cpp` | GLFW attrib dedup cache |
| `src/imgui_impl_glfw_overlay.cpp` | ImGui GLFW backend (includes wrapper) |
