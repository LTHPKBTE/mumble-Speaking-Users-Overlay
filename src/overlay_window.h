/*
 * overlay_window.h — GLFW + cimgui overlay window
 *
 * A floating, transparent overlay window that renders a list of
 * currently speaking users using Dear ImGui.
 */

#ifndef OVERLAY_WINDOW_H
#define OVERLAY_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

/* Result codes for overlay_window_init */
#define OW_OK         0
#define OW_ERR_GLFW   1
#define OW_ERR_IMGUI  2
#define OW_ERR_HOTKEY 3

/*
 * Runtime configuration for the overlay window.
 * Can be modified at runtime (e.g. via ImGui sliders).
 */
typedef struct {
    int   window_x;
    int   window_y;
    int   window_width;
    int   window_height;
    float alpha;              /* window background transparency (0.0 – 1.0) */
    float text_alpha;         /* text / UI element alpha (0.0 – 1.0) */
    float window_scale;       /* content scale factor (0.25 – 4.0) */
    bool  mouse_passthrough;  /* whether clicks pass through the main overlay (not settings) */
    bool  always_on_top;      /* window stays on top of others */
    int   max_visible_speakers; /* max "recent" speakers shown at top before scroll */
    bool  dangerous_alpha_allowed; /* allow alpha below 0.2 */
    bool  show_all_users;       /* always show all known users (never prune) */
    bool  show_recent_speakers; /* show recently-speaking users dimmed (only when !show_all_users) */
    float idle_user_alpha;    /* opacity for non-speaking users (0.0 – 1.0) */
    int   idle_timeout_seconds; /* seconds before a passive user is pruned (only when show_recent_speakers) */
    bool  show_current_channel_only; /* only show users in the same channel as local user */
    bool  mumble_logging_enabled; /* whether to print log messages via MumbleAPI */
    bool  debug_show_fps;         /* print FPS to Mumble log every 10 seconds (debug) */
    bool  vsync_enabled;          /* use vsync for frame pacing (off by default, warns about driver busy-wait) */
    int   fps_passthrough;        /* target FPS when mouse passthrough is active (15-400) */
    int   fps_clickable;          /* target FPS when window is clickable (15-400) */
    int   fps_settings_open;      /* target FPS when settings panel is open (15-400, highest priority) */
    bool  auto_detect_refresh;    /* auto-detect monitor refresh rate for clickable/settings FPS */

    /* ---- Global hotkeys (saved as Win32 VK + modifiers) ---- */
    int   hotkey_toggle_vk;       /* Win32 virtual-key code for toggle passthrough (0 = unset) */
    int   hotkey_toggle_mods;     /* MOD_CONTROL / MOD_SHIFT / MOD_ALT / MOD_WIN */
    int   hotkey_show_vk;         /* Win32 virtual-key code for show window (0 = unset) */
    int   hotkey_show_mods;       /* MOD_CONTROL / MOD_SHIFT / MOD_ALT / MOD_WIN */
} overlay_config_t;

/* Get default configuration */
overlay_config_t overlay_config_default(void);

/*
 * Initialise GLFW, create window, set up OpenGL + cimgui.
 * Also registers global hotkeys (Ctrl+Shift+P, Ctrl+Shift+H).
 * Returns OW_OK on success.
 */
int  overlay_window_init(const overlay_config_t *cfg);

/*
 * Render one frame.
 * - poll_speakers: callback invoked to get the list of active speakers
 * - userdata:      opaque pointer passed to the callback
 * Returns: true to continue, false if window was closed
 */
typedef int (*overlay_poll_speakers_fn)(void *userdata,
                                        /* out: */ uint32_t *user_ids,
                                        /* out: */ char (*names)[128],
                                        /* out: */ int *states,
                                        int max_count);

/*
 * Log callback — called by the overlay window to report errors/warnings.
 * Set via overlay_window_set_log_callback(). If not set, errors only go to stderr.
 */
typedef void (*overlay_log_fn)(const char *msg);

/*
 * Set the log callback. Call once from the Mumble main thread before
 * starting the render thread. Thread-safe.
 */
void overlay_window_set_log_callback(overlay_log_fn fn);

bool overlay_window_frame(overlay_poll_speakers_fn poll, void *userdata);

/*
 * Request the overlay to become visible (e.g. after being hidden).
 * Thread-safe: only sets a flag, read by the render thread.
 */
void overlay_window_request_show(void);

/*
 * Request the overlay window position to be reset to defaults.
 * Thread-safe: only sets a flag, read by the render thread.
 */
void overlay_window_request_reset_position(void);

/*
 * Tear down cimgui, destroy GLFW window, terminate GLFW.
 * Also unregisters global hotkeys.
 */
void overlay_window_shutdown(void);

/*
 * Get current config (e.g. to persist on shutdown).
 * The config is updated live as the user drags the window or moves sliders.
 */
void overlay_window_get_config(overlay_config_t *cfg);

/*
 * Get a const pointer to the current config (read-only).
 * Avoids copying the entire struct every frame.
 */
const overlay_config_t* overlay_window_get_config_ptr(void);

#endif /* OVERLAY_WINDOW_H */
