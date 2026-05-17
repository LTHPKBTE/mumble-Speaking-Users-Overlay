/*
 * render_thread.c — Implementation
 *
 * A single render thread runs overlay_window_frame() until signalled to stop.
 * Uses platform atomics for the running/stop flags since Mumble callbacks
 * may arrive from different threads.
 */

#include "render_thread.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE thread_handle_t;
#define THREAD_RETURN DWORD WINAPI
#define THREAD_EXIT(code) return (code)
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t thread_handle_t;
#define THREAD_RETURN void *
#define THREAD_EXIT(code) return NULL
#endif

/* ---- Atomic helpers ---- */
#ifdef _WIN32
/* Use Interlocked* on Windows */
static LONG g_running   = 0;  /* 0=idle, 1=running, -1=stopping */
static LONG g_stop_flag = 0;
#define ATOMIC_SET(dst, val)  InterlockedExchange(&(dst), (LONG)(val))
#define ATOMIC_GET(src)       InterlockedCompareExchange(&(src), 0, 0)
#else
/* Use C++11 std::atomic (compiled as C++) */
#include <atomic>
static std::atomic<int>  g_running(0);
static std::atomic<bool> g_stop_flag(false);
#define ATOMIC_SET(dst, val)  (dst).store((int)(val), std::memory_order_relaxed)
#define ATOMIC_GET(src)       (src).load(std::memory_order_relaxed)
#endif

/* ---- Internal state ---- */
static thread_handle_t g_render_thread;

/* ---- Context passed to render thread ---- */
typedef struct {
    overlay_config_t          config;
    bool                      use_saved_config;  /* If true, pass NULL to overlay_window_init */
    overlay_poll_speakers_fn  poll_fn;
    void                     *userdata;
} render_thread_data_t;

/* ---- Thread entry point ---- */
static THREAD_RETURN render_thread_proc(void *arg) {
    render_thread_data_t *data = (render_thread_data_t *)arg;

    int rc = overlay_window_init(data->use_saved_config ? NULL : &data->config);
    if (rc != OW_OK) {
        free(data);
        ATOMIC_SET(g_running, 0);
        THREAD_EXIT(1);
    }

    ATOMIC_SET(g_running, 1);

    while (!ATOMIC_GET(g_stop_flag)) {
        bool keep_going = overlay_window_frame(data->poll_fn, data->userdata);
        if (!keep_going) {
            break;
        }
    }

    overlay_window_shutdown();
    free(data);
    ATOMIC_SET(g_running, 0);
    THREAD_EXIT(0);
}

/* ---- Public API ---- */

int render_thread_start(const overlay_config_t *cfg,
                        overlay_poll_speakers_fn poll, void *userdata) {
    if (ATOMIC_GET(g_running)) {
        return -1;  /* already running */
    }

    render_thread_data_t *data =
        (render_thread_data_t *)malloc(sizeof(render_thread_data_t));
    if (data == NULL) {
        return -2;
    }
    /* If cfg is NULL, tell overlay_window_init to use saved config from disk */
    if (cfg != NULL) {
        data->config = *cfg;
        data->use_saved_config = false;
    } else {
        memset(&data->config, 0, sizeof(data->config));
        data->use_saved_config = true;
    }
    data->poll_fn  = poll;
    data->userdata = userdata;

    ATOMIC_SET(g_stop_flag, false);

#ifdef _WIN32
    g_render_thread = CreateThread(NULL, 0, render_thread_proc, data, 0, NULL);
    if (g_render_thread == NULL) {
        free(data);
        return -3;
    }
#else
    int rc = pthread_create(&g_render_thread, NULL, render_thread_proc, data);
    if (rc != 0) {
        free(data);
        return -3;
    }
#endif

    return 0;
}

void render_thread_stop(void) {
    /* Atomically transition 1 -> -1 (stopping). Only ONE caller succeeds. */
#ifdef _WIN32
    LONG prev = InterlockedCompareExchange(&g_running, -1, 1);
    if (prev != 1) {
        return;  /* already idle or another thread is stopping */
    }
#else
    int expected = 1;
    if (!g_running.compare_exchange_strong(expected, -1)) {
        return;
    }
#endif

    ATOMIC_SET(g_stop_flag, true);

#ifdef _WIN32
    WaitForSingleObject(g_render_thread, 5000);
    CloseHandle(g_render_thread);
#else
    pthread_join(g_render_thread, NULL);
#endif

    ATOMIC_SET(g_running, 0);
}

bool render_thread_is_running(void) {
    return ATOMIC_GET(g_running) == 1;
}
