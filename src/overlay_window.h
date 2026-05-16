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
    bool  show_idle_users;    /* show non-speaking users dimmed */
    float idle_user_alpha;    /* opacity for non-speaking users (0.0 – 1.0) */
    int   idle_timeout_seconds; /* seconds before a passive user is pruned from list */
    bool  mumble_logging_enabled; /* whether to print log messages via MumbleAPI */
} overlay_config_t;

/* Get default configuration */
overlay_config_t overlay_config_default(void);

/*
 * Initialise GLFW, create window, set up OpenGL + cimgui.
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
 */
void overlay_window_shutdown(void);

/*
 * Get current config (e.g. to persist on shutdown).
 * The config is updated live as the user drags the window or moves sliders.
 */
void overlay_window_get_config(overlay_config_t *cfg);

#endif /* OVERLAY_WINDOW_H */
