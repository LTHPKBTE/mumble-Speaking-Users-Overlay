/*
 * test_standalone.c — Standalone test for the overlay window
 *
 * Compile with -DOVERLAY_STANDALONE_TEST via CMake option OVERLAY_BUILD_STANDALONE.
 * This creates the overlay window with fake speaking users, no Mumble needed.
 */

#ifdef OVERLAY_STANDALONE_TEST

#include "overlay_window.h"
#include "render_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <windows.h>

/* ---- Fake speaker data for testing ---- */
static const char *fake_names[] = {
    "Alice", "Bob", "Charlie", "Diana", "Eve",
    "Frank", "Grace", "Henry"
};
#define FAKE_COUNT (sizeof(fake_names) / sizeof(fake_names[0]))

/*
 * Poll callback — returns a rotating set of fake speakers
 */
static int fake_poll_speakers(void *userdata,
                               uint32_t *user_ids,
                               char (*names)[128],
                               int *states,
                               int max_count) {
    (void)userdata;
    static int tick = 0;
    tick++;

    /* Simulate 0-3 speakers at a time */
    int count = (tick / 60) % 4;  /* changes every ~1s at 60fps */
    if (count > max_count) count = max_count;

    for (int i = 0; i < count; i++) {
        int idx = ((tick / 30) + i) % FAKE_COUNT;
        user_ids[i] = (uint32_t)(idx + 1);
        snprintf(names[i], 128, "%s", fake_names[idx]);
        /* Cycle through talking states */
        states[i] = ((tick / 20) + i) % 3 + 1;  /* 1,2,3 = talking,whisper,shout */
    }
    return count;
}

/* ---- Entry point ---- */
int main(void) {
    printf("=== Mumble Speaking Users Overlay — Standalone Test ===\n");
    printf("Starting overlay window with fake data...\n");
    printf("Close the overlay window or press Ctrl+C to exit.\n\n");

    overlay_config_t cfg = overlay_config_default();
    /* Standalone: start at center-ish position */
    cfg.window_x      = 200;
    cfg.window_y      = 200;
    cfg.window_width  = 350;
    cfg.window_height = 400;

    int rc = render_thread_start(&cfg, fake_poll_speakers, NULL);
    if (rc != 0) {
        fprintf(stderr, "Failed to start render thread (code %d)\n", rc);
        return 1;
    }

    /* Wait until render thread exits (user closes window) */
    while (render_thread_is_running()) {
        Sleep(100);
    }

    printf("Overlay window closed. Goodbye!\n");
    return 0;
}

#endif /* OVERLAY_STANDALONE_TEST */
