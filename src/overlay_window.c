/*
 * overlay_window.c — GLFW + Dear ImGui overlay window implementation
 *
 * Creates a floating overlay with:
 *   - Transparency control (alpha slider)
 *   - Optional mouse passthrough
 *   - Always-on-top
 *   - Live list of currently speaking users
 *   - System language detection for UI strings
 *
 * Uses Dear ImGui C++ API directly.
 */

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include <windows.h>
#include <mmsystem.h>    /* timeBeginPeriod / timeEndPeriod for fallback timer */
#include <intrin.h>      /* _mm_pause for spin-wait */
#pragma comment(lib, "winmm.lib")

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

/* ---- Overlay internal header ---- */
#include "overlay_window.h"

/* Forward-declare wrapper stats (don't include glfw_attrib_wrapper.h — its macro
 * would redirect glfwSetWindowAttrib in apply_config_to_window()). */
bool OverlayGlfwAttribGetStats(long long *out_skipped, long long *out_passed);

/* ========================================================================
 * Log callback — set by plugin.c to forward messages to Mumble
 * ======================================================================== */
static overlay_log_fn g_log_fn = NULL;

void overlay_window_set_log_callback(overlay_log_fn fn) {
    g_log_fn = fn;
}

#define OW_LOG(msg) do { \
    fprintf(stderr, "[overlay] %s\n", msg); \
    if (g_log_fn) g_log_fn(msg); \
} while(0)

#define OW_LOGF(fmt, ...) do { \
    char _ow_buf[512]; \
    snprintf(_ow_buf, sizeof(_ow_buf), fmt, __VA_ARGS__); \
    fprintf(stderr, "[overlay] %s\n", _ow_buf); \
    if (g_log_fn) g_log_fn(_ow_buf); \
} while(0)

/* ========================================================================
 * Localisation: detect system language
 * ======================================================================== */
static int g_lang_is_chinese = 0;

static void detect_system_language(void) {
    LANGID langID = GetUserDefaultUILanguage();
    WORD primary  = PRIMARYLANGID(langID);
    g_lang_is_chinese = (primary == LANG_CHINESE
                         || primary == LANG_CHINESE_SIMPLIFIED
                         || primary == LANG_CHINESE_TRADITIONAL);
}

#define LOC(chinese, english)  (g_lang_is_chinese ? (chinese) : (english))

/* ========================================================================
 * Internal state
 * ======================================================================== */
static GLFWwindow      *g_window         = NULL;
static overlay_config_t g_config;
static bool             g_settings_open  = false;
static bool             g_first_frame    = true;
static bool             g_window_hidden  = false;   /* user clicked close */
static bool             g_user_hid_window = false;  /* true when user clicked X */
static bool             g_user_saw_speaking_after_hide = false;

/* ---- Request flags (set from Mumble main thread, consumed by render thread) ---- */
static volatile bool    g_request_show          = false;
static volatile bool    g_request_reset_position = false;

/* ---- Display change flag (set by WM_DISPLAYCHANGE in window proc) ---- */
static volatile bool    g_display_changed       = false;

/* ---- RegisterHotKey conflict flag — triggers settings auto-popup on first frame ---- */
static volatile bool    g_hotkey_conflict_on_init = false;

/* ---- Optimization: cached values to avoid redundant work ---- */
static bool   g_last_topmost     = true;    /* last always_on_top for viewport mgmt */

/* ---- User list scrolling / activity state ---- */
static double  g_last_mouse_activity_time = 0.0; /* ImGui::GetTime() of last hover/scroll */
static bool    g_scrolled_by_user         = false; /* user manually scrolled */
static float   g_last_scroll_y            = 0.0f;  /* last known scroll position */
static uint32_t g_user_timestamps[64];      /* ordered user IDs by speaking recency */
static int     g_user_timestamp_count     = 0;

/* ---- Drag state (smooth window dragging) ---- */
static bool    g_drag_active  = false;
static int     g_drag_win_x   = 0;
static int     g_drag_win_y   = 0;
static float   g_drag_mouse_x = 0.0f;
static float   g_drag_mouse_y = 0.0f;

/* ---- Height auto-sizing ---- */
static int     g_last_content_h = 0;

/* ---- Debug FPS periodic logging ---- */
static double  g_last_fps_log_time = 0.0;

/* ---- Per-frame profiling (always active, only logged when debug_show_fps is on) ---- */
#define PROF_SAMPLES 5
static LARGE_INTEGER g_prof_freq;
static LARGE_INTEGER g_prof_t0;
static LARGE_INTEGER g_prof_t1[PROF_SAMPLES];
static int           g_prof_idx = 0;
static void prof_tick(void) {
    if (g_prof_idx < PROF_SAMPLES) {
        QueryPerformanceCounter(&g_prof_t1[g_prof_idx]);
        g_prof_idx++;
    }
}
/* Always reset at frame start — QPC is ~20 ns, negligible overhead. */
#define PROF_BEGIN() do { g_prof_idx = 0; QueryPerformanceCounter(&g_prof_t0); } while(0)
#define PROF_TICK() prof_tick()

/* ---- High-resolution waitable timer (replaces Sleep + timeBeginPeriod) ---- */
static HANDLE  g_frame_timer = NULL;
static bool    g_using_time_period = false;  /* true if fallback timer needed timeBeginPeriod */
static void cleanup_time_period(void);  /* forward decl — defined near shutdown */

/* ---- Dynamic spin-wait margin for two-phase frame pacing (plan.txt research) ----
 * Initial margin: 300 us. After each WaitForSingleObject, actual late-wake time
 * is measured and fed into an EMA. The margin auto-adjusts within [100 us, 2 ms]
 * so it tracks the system's real scheduler jitter over time.
 *
 * A 50 us safety buffer is added to the EMA to avoid consistently cutting it close.
 * On heavily loaded systems the margin will drift up; on quiet systems it decays
 * toward the 100 us floor — minimising CPU waste in both cases. */
static double   g_spin_margin       = 0.0003;   /* current effective margin (s) */
static double   g_spin_ema          = 0.0;      /* EMA of observed late-wake (s) */
static uint64_t g_spin_samples      = 0;
#define SPIN_MARGIN_MIN    0.0001   /* 100 us — don't drop below */
#define SPIN_MARGIN_MAX    0.0020   /*   2 ms — don't absorb extreme preemption */
#define SPIN_MARGIN_ALPHA  0.02     /* EMA smoothing (plan.txt recommendation) */
#define SPIN_MARGIN_BUFFER 0.00005  /*  50 us safety on top of EMA */

/* ---- Current effective target interval (determined each frame from priority rules) ---- */
static double  g_frame_target_interval = 0.0;
static double  g_last_frame_time       = 0.0;

/* ---- Global hotkeys: RegisterHotKey ---- */
#define HOTKEY_ID_TOGGLE        100
#define HOTKEY_ID_SHOW          101
#define HOTKEY_ID_TOGGLE_STAGING 102  /* staging slot for atomic swap */
#define HOTKEY_ID_SHOW_STAGING  103

static volatile LONG g_hotkey_toggle_passthrough = 0;
static volatile LONG g_hotkey_show_window        = 0;

/* WNDPROC for subclassing */
static WNDPROC g_prev_wndproc       = NULL;

/* ---- Forward declarations ---- */
static LRESULT CALLBACK overlay_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void apply_config_to_window(void);
static void on_window_close(GLFWwindow *win);
static void register_all_hotkeys(void);
static void unregister_all_hotkeys(void);

static float clamp01f(float v);
static ImVec4 with_text_alpha(ImVec4 color);

/* ========================================================================
 * Font: load CJK glyphs for Chinese / Japanese / Korean
 * ======================================================================== */
static void load_cjk_font(void) {
    ImGuiIO& io = ImGui::GetIO();

    /* List of candidate CJK font paths (tried in order) */
    static const char *candidates[] = {
        "C:/Windows/Fonts/msyh.ttc",       /* Microsoft YaHei */
        "C:/Windows/Fonts/msyhbd.ttc",     /* Microsoft YaHei Bold */
        "C:/Windows/Fonts/simhei.ttf",     /* SimHei */
        "C:/Windows/Fonts/simsun.ttc",     /* SimSun */
        "C:/Windows/Fonts/yahei.ttf",      /* alternative */
        NULL
    };

    /* First, ensure default font is loaded so we have ASCII glyphs */
    io.Fonts->AddFontDefault();

    ImFontConfig config;
    config.MergeMode   = true;
    config.FontDataOwnedByAtlas = false;
    /* Slightly reduced glyph ranges to keep atlas size manageable */
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,   /* Basic Latin + Latin-1 Supplement */
        0x0100, 0x024F,   /* Latin Extended-A/B */
        0x2000, 0x206F,   /* General Punctuation */
        0x3000, 0x30FF,   /* CJK Symbols, Hiragana, Katakana */
        0x4E00, 0x9FFF,   /* CJK Unified Ideographs */
        0xFF00, 0xFFEF,   /* Fullwidth forms */
        0
    };

    for (int i = 0; candidates[i] != NULL; i++) {
        ImFont *font = io.Fonts->AddFontFromFileTTF(candidates[i], 16.0f, &config, ranges);
        if (font != NULL) {
            OW_LOGF("Loaded CJK font: %s", candidates[i]);
            return; /* success */
        }
    }

    /* No CJK font found – not a fatal error; non-ASCII will show as fallback */
    OW_LOG("No CJK font found, non-ASCII glyphs unavailable");
}

/* ========================================================================
 * Global hotkeys — RegisterHotKey
 * ======================================================================== */

/* Convert a Win32 VK + modifiers to a RegisterHotKey call. MOD_NOREPEAT
 * prevents the hotkey from auto-repeating while held. */
static bool register_one_hotkey(HWND hwnd, int id, int vk, int mods) {
    if (vk == 0 || hwnd == NULL) return false;
    UINT fsModifiers = (UINT)mods | MOD_NOREPEAT;
    return RegisterHotKey(hwnd, (int)id, fsModifiers, (UINT)vk) != 0;
}

static void register_all_hotkeys(void) {
    if (g_window == NULL) return;
    HWND hwnd = glfwGetWin32Window(g_window);
    if (hwnd == NULL) return;

    register_one_hotkey(hwnd, HOTKEY_ID_TOGGLE,
        g_config.hotkey_toggle_vk, g_config.hotkey_toggle_mods);
    register_one_hotkey(hwnd, HOTKEY_ID_SHOW,
        g_config.hotkey_show_vk, g_config.hotkey_show_mods);
}

static void unregister_all_hotkeys(void) {
    if (g_window == NULL) return;
    HWND hwnd = glfwGetWin32Window(g_window);
    if (hwnd == NULL) return;

    UnregisterHotKey(hwnd, HOTKEY_ID_TOGGLE);
    UnregisterHotKey(hwnd, HOTKEY_ID_SHOW);
    UnregisterHotKey(hwnd, HOTKEY_ID_TOGGLE_STAGING);
    UnregisterHotKey(hwnd, HOTKEY_ID_SHOW_STAGING);
}

/* Try an atomic swap: register candidate on staging ID, then swap. */
static bool replace_registered_hotkey(HWND hwnd, int active_id, int staging_id,
                                      int vk, int mods) {
    if (vk == 0) return false;
    if (!register_one_hotkey(hwnd, staging_id, vk, mods)) return false;
    UnregisterHotKey(hwnd, active_id);
    /* Active/staging IDs are now swapped logically — caller tracks which is which. */
    return true;
}

/* ========================================================================
 * Auto-detect monitor refresh rate (30-350 Hz, clamped)
 * Returns the detected rate, or 0 on failure.
 * ======================================================================== */
static int detect_monitor_refresh_rate(void) {
    if (g_window == NULL) return 0;

    /* Find which monitor the window centre falls on */
    int wx, wy, ww, wh;
    glfwGetWindowPos(g_window, &wx, &wy);
    glfwGetWindowSize(g_window, &ww, &wh);
    int cx = wx + ww / 2;
    int cy = wy + wh / 2;

    int count;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; i++) {
        int mx, my, mw, mh;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh) {
            const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
            if (mode != NULL && mode->refreshRate > 0) {
                int rate = (int)(mode->refreshRate + 0.5f);
                if (rate < 30)  return 30;
                if (rate > 350) return 350;
                return rate;
            }
            break;
        }
    }
    /* Fallback: try the primary monitor */
    if (count > 0) {
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[0]);
        if (mode != NULL && mode->refreshRate > 0) {
            int rate = (int)(mode->refreshRate + 0.5f);
            if (rate < 30)  return 30;
            if (rate > 350) return 350;
            return rate;
        }
    }
    return 0;
}

/* Apply auto-detected refresh to clickable + settings FPS (if enabled). */
static void overlay_apply_auto_refresh(void) {
    if (!g_config.auto_detect_refresh) return;
    int rate = detect_monitor_refresh_rate();
    if (rate > 0) {
        g_config.fps_clickable    = rate;
        g_config.fps_settings_open = rate;
        OW_LOGF("Auto-detected monitor refresh rate: %d Hz", rate);
    }
}

/* ========================================================================
 * Configuration defaults
 * ======================================================================== */
overlay_config_t overlay_config_default(void) {
    overlay_config_t cfg;
    cfg.window_x          = 100;
    cfg.window_y          = 100;
    cfg.window_width      = 320;
    cfg.window_height     = 400;
    cfg.alpha             = 0.85f;
    cfg.text_alpha        = 1.0f;
    cfg.window_scale      = 1.0f;
    cfg.mouse_passthrough = false;
    cfg.always_on_top     = true;
    cfg.max_visible_speakers = 8;
    cfg.dangerous_alpha_allowed = false;
    cfg.show_all_users       = true;   /* always show all known users */
    cfg.show_recent_speakers = false;  /* off by default */
    cfg.idle_user_alpha   = 0.3f;
    cfg.idle_timeout_seconds = 5;
    cfg.show_current_channel_only = false;
    cfg.mumble_logging_enabled = true;
    cfg.debug_show_fps = false;
    cfg.vsync_enabled    = false;
    cfg.fps_passthrough  = 15;
    cfg.fps_clickable    = 60;
    cfg.fps_settings_open = 60;
    cfg.auto_detect_refresh = true;
    cfg.hotkey_toggle_vk   = 'P';
    cfg.hotkey_toggle_mods = MOD_CONTROL | MOD_SHIFT;
    cfg.hotkey_show_vk     = 'H';
    cfg.hotkey_show_mods   = MOD_CONTROL | MOD_SHIFT;
    return cfg;
}

/* ========================================================================
 * Config persistence — simple key=value file on disk
 * ======================================================================== */
static const char *overlay_config_path(void) {
    static char path[1024];
    const char *appdata = getenv("APPDATA");
    if (appdata) {
        snprintf(path, sizeof(path), "%s\\Mumble\\SpeakingOverlay.cfg", appdata);
        return path;
    }
    return NULL;
}

static void overlay_config_save(void) {
    const char *cfg_path = overlay_config_path();
    if (!cfg_path) {
        OW_LOG("Config save failed: could not determine config path");
        return;
    }

    /* Ensure parent directory exists */
    {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", cfg_path);
        char *last_sep = strrchr(dir, '\\');
        if (last_sep != NULL) {
            *last_sep = '\0';
            if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                /* Log but continue — fopen will also fail if dir is unusable */
                OW_LOGF("Config save: could not create directory %s (error %lu)", dir, GetLastError());
            }
        }
    }

    FILE *f = fopen(cfg_path, "w");
    if (!f) {
        OW_LOGF("Config save failed: could not open %s for writing", cfg_path);
        return;
    }
    fprintf(f, "window_x=%d\n",        g_config.window_x);
    fprintf(f, "window_y=%d\n",        g_config.window_y);
    fprintf(f, "window_width=%d\n",    g_config.window_width);
    fprintf(f, "window_height=%d\n",   g_config.window_height);
    fprintf(f, "alpha=%.3f\n",         (double)g_config.alpha);
    fprintf(f, "text_alpha=%.3f\n",    (double)g_config.text_alpha);
    fprintf(f, "window_scale=%.3f\n",  (double)g_config.window_scale);
    fprintf(f, "mouse_passthrough=%d\n", g_config.mouse_passthrough ? 1 : 0);
    fprintf(f, "always_on_top=%d\n",   g_config.always_on_top ? 1 : 0);
    fprintf(f, "max_visible_speakers=%d\n", g_config.max_visible_speakers);
    fprintf(f, "show_all_users=%d\n",  g_config.show_all_users ? 1 : 0);
    fprintf(f, "show_recent_speakers=%d\n", g_config.show_recent_speakers ? 1 : 0);
    fprintf(f, "idle_user_alpha=%.3f\n",(double)g_config.idle_user_alpha);
    fprintf(f, "idle_timeout_seconds=%d\n", g_config.idle_timeout_seconds);
    fprintf(f, "show_current_channel_only=%d\n", g_config.show_current_channel_only ? 1 : 0);
    fprintf(f, "mumble_logging_enabled=%d\n", g_config.mumble_logging_enabled ? 1 : 0);
    fprintf(f, "debug_show_fps=%d\n", g_config.debug_show_fps ? 1 : 0);
    fprintf(f, "vsync_enabled=%d\n", g_config.vsync_enabled ? 1 : 0);
    fprintf(f, "fps_passthrough=%d\n", g_config.fps_passthrough);
    fprintf(f, "fps_clickable=%d\n", g_config.fps_clickable);
    fprintf(f, "fps_settings_open=%d\n", g_config.fps_settings_open);
    fprintf(f, "auto_detect_refresh=%d\n", g_config.auto_detect_refresh ? 1 : 0);
    fprintf(f, "hotkey_toggle_vk=%d\n",   g_config.hotkey_toggle_vk);
    fprintf(f, "hotkey_toggle_mods=%d\n",  g_config.hotkey_toggle_mods);
    fprintf(f, "hotkey_show_vk=%d\n",     g_config.hotkey_show_vk);
    fprintf(f, "hotkey_show_mods=%d\n",    g_config.hotkey_show_mods);
    fclose(f);
}

static void overlay_config_load(overlay_config_t *cfg) {
    /* Start from defaults, then override from file if it exists */
    *cfg = overlay_config_default();

    const char *cfg_path = overlay_config_path();
    if (!cfg_path) {
        OW_LOG("Config load: could not determine config path, using defaults");
        return;
    }

    FILE *f = fopen(cfg_path, "r");
    if (!f) {
        /* File doesn't exist — normal for first run, don't log as error */
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        int ival;
        float fval;
        if (sscanf(line, "window_x=%d", &ival) == 1)             cfg->window_x = ival;
        else if (sscanf(line, "window_y=%d", &ival) == 1)        cfg->window_y = ival;
        else if (sscanf(line, "window_width=%d", &ival) == 1)    cfg->window_width = ival;
        else if (sscanf(line, "window_height=%d", &ival) == 1)   cfg->window_height = ival;
        else if (sscanf(line, "alpha=%f", &fval) == 1)           cfg->alpha = fval;
        else if (sscanf(line, "text_alpha=%f", &fval) == 1)      cfg->text_alpha = fval;
        else if (sscanf(line, "window_scale=%f", &fval) == 1)    cfg->window_scale = fval;
        else if (sscanf(line, "mouse_passthrough=%d", &ival) == 1) cfg->mouse_passthrough = (ival != 0);
        else if (sscanf(line, "always_on_top=%d", &ival) == 1)   cfg->always_on_top = (ival != 0);
        else if (sscanf(line, "max_visible_speakers=%d", &ival) == 1) cfg->max_visible_speakers = ival;
        else if (sscanf(line, "show_all_users=%d", &ival) == 1)      cfg->show_all_users = (ival != 0);
        else if (sscanf(line, "show_idle_users=%d", &ival) == 1) {
            /* backward compat: old config key maps to show_all_users */
            cfg->show_all_users = (ival != 0);
        }
        else if (sscanf(line, "show_recent_speakers=%d", &ival) == 1) cfg->show_recent_speakers = (ival != 0);
        else if (sscanf(line, "idle_user_alpha=%f", &fval) == 1)   cfg->idle_user_alpha = fval;
        else if (sscanf(line, "idle_timeout_seconds=%d", &ival) == 1) cfg->idle_timeout_seconds = ival;
        else if (sscanf(line, "show_current_channel_only=%d", &ival) == 1) cfg->show_current_channel_only = (ival != 0);
        else if (sscanf(line, "dangerous_alpha_allowed=%d", &ival) == 1) cfg->dangerous_alpha_allowed = (ival != 0);
        else if (sscanf(line, "mumble_logging_enabled=%d", &ival) == 1) cfg->mumble_logging_enabled = (ival != 0);
        else if (sscanf(line, "debug_show_fps=%d", &ival) == 1) cfg->debug_show_fps = (ival != 0);
        else if (sscanf(line, "custom_fps_enabled=%d", &ival) == 1) {
            /* backward compat: old flag → modern split config */
            if (ival != 0) {
                /* Old: custom framerate ON (= vsync off + limit FPS) */
                cfg->vsync_enabled = false;
                cfg->fps_passthrough  = 15;
                cfg->fps_clickable    = 60;
                cfg->fps_settings_open = 60;
            } else {
                /* Old: custom framerate OFF (= vsync on) */
                cfg->vsync_enabled = true;
            }
        }
        else if (sscanf(line, "custom_fps_value=%d", &ival) == 1) {
            /* backward compat: apply old single-value setting to modern split config */
            cfg->fps_clickable    = ival;
            cfg->fps_settings_open = ival;
        }
        else if (sscanf(line, "vsync_enabled=%d", &ival) == 1) cfg->vsync_enabled = (ival != 0);
        else if (sscanf(line, "fps_passthrough=%d", &ival) == 1) cfg->fps_passthrough = ival;
        else if (sscanf(line, "fps_clickable=%d", &ival) == 1) cfg->fps_clickable = ival;
        else if (sscanf(line, "fps_settings_open=%d", &ival) == 1) cfg->fps_settings_open = ival;
        else if (sscanf(line, "auto_detect_refresh=%d", &ival) == 1) cfg->auto_detect_refresh = (ival != 0);
        else if (sscanf(line, "hotkey_toggle_vk=%d", &ival) == 1)   cfg->hotkey_toggle_vk = ival;
        else if (sscanf(line, "hotkey_toggle_mods=%d", &ival) == 1)  cfg->hotkey_toggle_mods = ival;
        else if (sscanf(line, "hotkey_show_vk=%d", &ival) == 1)     cfg->hotkey_show_vk = ival;
        else if (sscanf(line, "hotkey_show_mods=%d", &ival) == 1)    cfg->hotkey_show_mods = ival;
        /* hotkey_compat_mode, fps_idle, idle_fps_timeout are no longer
         * supported — silently ignore if present in old configs */
    }
    fclose(f);
}

/* ========================================================================
 * GLFW error callback
 * ======================================================================== */
static void glfw_error_callback(int error, const char *description) {
    (void)error;
    OW_LOGF("GLFW error %d: %s", error, description);
}

/* ========================================================================
 * Initialize
 * ======================================================================== */
int overlay_window_init(const overlay_config_t *cfg) {
    /* Load persisted config (if any), then override with the provided one */
    overlay_config_load(&g_config);
    if (cfg != NULL) {
        /* Only override fields that aren't zero/default */
        if (cfg->window_x != 0 || cfg->window_y != 0 ||
            cfg->window_width != 0 || cfg->window_height != 0) {
            if (cfg->window_width  > 0) g_config.window_width  = cfg->window_width;
            if (cfg->window_height > 0) g_config.window_height = cfg->window_height;
            g_config.window_x = cfg->window_x;
            g_config.window_y = cfg->window_y;
        }
        /* Always honor explicitly-set boolean/float overrides */
        g_config.alpha             = cfg->alpha;
        g_config.text_alpha        = cfg->text_alpha;
        g_config.show_all_users    = cfg->show_all_users;
        g_config.show_recent_speakers = cfg->show_recent_speakers;
        g_config.idle_user_alpha   = cfg->idle_user_alpha;
        g_config.show_current_channel_only = cfg->show_current_channel_only;
        g_config.window_scale      = cfg->window_scale;
        g_config.mouse_passthrough = cfg->mouse_passthrough;
        g_config.always_on_top     = cfg->always_on_top;
        if (cfg->max_visible_speakers > 0)
            g_config.max_visible_speakers = cfg->max_visible_speakers;
        g_config.dangerous_alpha_allowed = cfg->dangerous_alpha_allowed;
        g_config.idle_timeout_seconds = cfg->idle_timeout_seconds;
        g_config.mumble_logging_enabled = cfg->mumble_logging_enabled;
        g_config.vsync_enabled      = cfg->vsync_enabled;
        g_config.fps_passthrough    = cfg->fps_passthrough;
        g_config.fps_clickable      = cfg->fps_clickable;
        g_config.fps_settings_open  = cfg->fps_settings_open;
        g_config.auto_detect_refresh = cfg->auto_detect_refresh;
        if (cfg->hotkey_toggle_vk != 0 || cfg->hotkey_toggle_mods != 0) {
            g_config.hotkey_toggle_vk   = cfg->hotkey_toggle_vk;
            g_config.hotkey_toggle_mods = cfg->hotkey_toggle_mods;
        }
        if (cfg->hotkey_show_vk != 0 || cfg->hotkey_show_mods != 0) {
            g_config.hotkey_show_vk     = cfg->hotkey_show_vk;
            g_config.hotkey_show_mods   = cfg->hotkey_show_mods;
        }
    }
    detect_system_language();

    /* --- GLFW --- */
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        OW_LOG("Failed to initialize GLFW");
        return OW_ERR_GLFW;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); /* Keep hidden until the native window attributes are applied. */
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);

    g_window = glfwCreateWindow(g_config.window_width, g_config.window_height,
                                "Mumble Speaking Overlay", NULL, NULL);
    if (g_window == NULL) {
        OW_LOG("Failed to create GLFW window");
        glfwTerminate();
        return OW_ERR_GLFW;
    }

    glfwSetWindowPos(g_window, g_config.window_x, g_config.window_y);

    {
        HWND hwnd = glfwGetWin32Window(g_window);
        if (hwnd != NULL && g_prev_wndproc == NULL) {
            g_prev_wndproc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)overlay_window_proc);
        }
    }

    glfwMakeContextCurrent(g_window);
    if (g_config.vsync_enabled) {
        glfwSwapInterval(1);
        g_frame_target_interval = 0.0;
    } else {
        glfwSwapInterval(0);
        g_frame_target_interval = 1.0 / (double)g_config.fps_passthrough;
    }

    /* Auto-detect monitor refresh rate on first start (before applying config). */
    overlay_apply_auto_refresh();

    /* Create high-resolution waitable timer (Win10 1803+, no global side effects). */
    g_frame_timer = CreateWaitableTimerExW(NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    /* Fallback to regular waitable timer on older Windows (< 1803).
     * Regular waitable timers use the system default resolution (~15.6 ms),
     * so we need timeBeginPeriod(1) for acceptable frame pacing. */
    if (g_frame_timer == NULL) {
        g_frame_timer = CreateWaitableTimer(NULL, TRUE, NULL);
        if (g_frame_timer != NULL) {
            timeBeginPeriod(1);
            g_using_time_period = true;
            atexit(cleanup_time_period);  /* ensure timeEndPeriod even on abnormal exit */
        }
    }

    /* --- Dear ImGui --- */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;

    /* Enable viewports so the settings panel can be a true separate OS window */
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    /* Load CJK font */
    load_cjk_font();

    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.Alpha = 1.0f;
    style.Colors[ImGuiCol_WindowBg].w = clamp01f(g_config.alpha);
    style.Colors[ImGuiCol_ChildBg].w = 0.0f;
    io.FontGlobalScale = g_config.window_scale;

    /* --- Backend init --- */
    if (!ImGui_ImplGlfw_InitForOpenGL(g_window, true)) {
        OW_LOG("Failed to init ImGui GLFW backend");
        ImGui::DestroyContext();
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return OW_ERR_IMGUI;
    }

    if (!ImGui_ImplOpenGL3_Init("#version 130")) {
        OW_LOG("Failed to init ImGui OpenGL3 backend");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return OW_ERR_IMGUI;
    }

    /* Install close callback */
    glfwSetWindowCloseCallback(g_window, on_window_close);

    /* Apply config (passthrough, toolwindow, topmost) via native/glfw APIs safely */
    apply_config_to_window();

    /* Register global hotkeys via RegisterHotKey (zero-latency, no hook overhead).
     * Check for conflicts — if any hotkey fails, auto-popup settings on first frame. */
    {
        HWND hwnd = glfwGetWin32Window(g_window);
        bool ok_toggle = register_one_hotkey(hwnd, HOTKEY_ID_TOGGLE,
            g_config.hotkey_toggle_vk, g_config.hotkey_toggle_mods);
        bool ok_show   = register_one_hotkey(hwnd, HOTKEY_ID_SHOW,
            g_config.hotkey_show_vk, g_config.hotkey_show_mods);
        if (!ok_toggle || !ok_show) {
            g_hotkey_conflict_on_init = true;
        }
    }

    g_first_frame = true;

    /* Initialise optimisation cache to match the freshly loaded config. */
    g_last_topmost   = g_config.always_on_top;

    /* Init profiling timer frequency */
    QueryPerformanceFrequency(&g_prof_freq);

    /* Show the window only after the initial native state has been applied. */
    if (!g_window_hidden) {
        glfwShowWindow(g_window);
    }
    
    return OW_OK;
}

/* ========================================================================
 * Window close callback
 * ======================================================================== */
static void on_window_close(GLFWwindow *win) {
    (void)win;
    g_window_hidden = true;
    g_user_hid_window = true;
    glfwHideWindow(g_window);
    glfwSetWindowShouldClose(g_window, GLFW_FALSE);
}

/* ========================================================================
 * Apply current config to the GLFW window natively
 * ======================================================================== */
static float clamp01f(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static ImVec4 with_text_alpha(ImVec4 color) {
    color.w *= clamp01f(g_config.text_alpha);
    return color;
}

/* ========================================================================
 * Subclass window proc to enforce passthrough and toolwindow styles
 * ======================================================================== */
static LRESULT CALLBACK overlay_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    /* Intercept style changes at the OS kernel level to enforce passthrough state */
    if (msg == WM_STYLECHANGING && wParam == GWL_EXSTYLE) {
        STYLESTRUCT* style = (STYLESTRUCT*)lParam;
        if (g_config.mouse_passthrough) {
            style->styleNew |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
        } else {
            style->styleNew &= ~WS_EX_TRANSPARENT;
        }
        /* Always hide from taskbar */
        style->styleNew |= WS_EX_TOOLWINDOW;
        style->styleNew &= ~WS_EX_APPWINDOW;
    }

    /* Global hotkey (RegisterHotKey) */
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

    /* Display mode changed (resolution / refresh rate / monitor plugged).
     * Set flag to re-detect on next frame — cheap, low-frequency event. */
    if (msg == WM_DISPLAYCHANGE) {
        g_display_changed = true;
    }

    if (msg == WM_NCHITTEST && g_config.mouse_passthrough) {
        return HTTRANSPARENT;
    }
    if (g_prev_wndproc != NULL) {
        return CallWindowProc(g_prev_wndproc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void apply_config_to_window(void) {
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha = 1.0f;
    style.Colors[ImGuiCol_WindowBg].w = clamp01f(g_config.alpha);

    if (!g_window) return;

    glfwSetWindowAttrib(g_window, GLFW_FLOATING,
        g_config.always_on_top ? GLFW_TRUE : GLFW_FALSE);

    /* Apply vsync / framerate limiter.
     * Vsync is off by default (NVIDIA/AMD drivers busy-wait on vsync, burning CPU).
     * Framerate is controlled via a high-resolution waitable timer per-frame. */
    if (g_config.vsync_enabled) {
        glfwSwapInterval(1);
        g_frame_target_interval = 0.0;
    } else {
        glfwSwapInterval(0);
        /* Start with passthrough FPS — real FPS is recalculated each frame by priority rules. */
        int fps = g_config.fps_passthrough;
        if (fps < 1) fps = 15;
        if (fps > 400) fps = 400;
        g_frame_target_interval = 1.0 / (double)fps;
    }

    /* Let GLFW track the native mouse-passthrough state (GLFW 3.3.2+) */
#if defined(GLFW_MOUSE_PASSTHROUGH)
    glfwSetWindowAttrib(g_window, GLFW_MOUSE_PASSTHROUGH,
        g_config.mouse_passthrough ? GLFW_TRUE : GLFW_FALSE);
#endif

    HWND hwnd = glfwGetWin32Window(g_window);
    if (hwnd) {
        LONG_PTR exstyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

        /* 
         * Add WS_EX_TOOLWINDOW and remove WS_EX_APPWINDOW to hide from taskbar.
         * Preserve WS_EX_LAYERED if GLFW set it for transparent framebuffer.
         */
        exstyle |= WS_EX_TOOLWINDOW;
        exstyle &= ~WS_EX_APPWINDOW;

        /* Enable click-through by adding WS_EX_TRANSPARENT */
        if (g_config.mouse_passthrough) {
            exstyle |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
        } else {
            exstyle &= ~WS_EX_TRANSPARENT;
        }

        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exstyle);

        /* 
         * DO NOT call SetLayeredWindowAttributes here! 
         * Calling it forces constant alpha and breaks the per-pixel alpha of 
         * GLFW's transparent framebuffer, causing a pure black background.
         */

        SetWindowPos(hwnd,
            g_config.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

/* ========================================================================
 * Return a const pointer to the current runtime configuration.
 * Avoids copying the entire struct (30+ fields) every frame.
 * ======================================================================== */
const overlay_config_t* overlay_window_get_config_ptr(void) {
    return &g_config;
}

/* ========================================================================
 * Render one frame
 * ======================================================================== */
bool overlay_window_frame(overlay_poll_speakers_fn poll, void *userdata) {
    if (glfwWindowShouldClose(g_window)) {
        return false;
    }

    /* Do not sleep and return early while the window is hidden.
     * Skipping frames would stop ImGui input and detached settings viewports from being pumped. */
    PROF_BEGIN();
    glfwPollEvents();
    PROF_TICK();

    ImGuiIO& io = ImGui::GetIO();
    if (g_config.mouse_passthrough && !io.WantCaptureMouse) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    PROF_TICK();

    /* ---- Process display change (monitor resolution / refresh rate) ---- */
    if (g_display_changed) {
        g_display_changed = false;
        overlay_apply_auto_refresh();
        apply_config_to_window();
    }

    /* ---- Process global keyboard shortcuts ---- */
    if (InterlockedCompareExchange(&g_hotkey_toggle_passthrough, 0, 1) == 1) {
        g_config.mouse_passthrough = !g_config.mouse_passthrough;
        apply_config_to_window();
    }
    if (InterlockedCompareExchange(&g_hotkey_show_window, 0, 1) == 1) {
        if (g_window_hidden) {
            g_window_hidden = false;
            g_user_hid_window = false;
            g_user_saw_speaking_after_hide = false;
            glfwShowWindow(g_window);
        }
    }

    /* ---- Handle request flags ---- */
    if (g_request_show && g_window_hidden) {
        g_request_show = false;
        g_window_hidden = false;
        g_user_hid_window = false;
        g_user_saw_speaking_after_hide = false;
        glfwShowWindow(g_window);
    }
    if (g_request_reset_position) {
        g_request_reset_position = false;
        overlay_config_t def = overlay_config_default();
        g_config.window_x = def.window_x;
        g_config.window_y = def.window_y;
        g_config.window_width = def.window_width;
        g_config.window_height = def.window_height;
        glfwSetWindowPos(g_window, g_config.window_x, g_config.window_y);
        glfwSetWindowSize(g_window, g_config.window_width, g_config.window_height);
        if (g_window_hidden) {
            g_window_hidden = false;
            g_user_hid_window = false;
            glfwShowWindow(g_window);
        }
    }

    /* ---- Poll the speaker list even when the main window is hidden. ---- */
    uint32_t user_ids[64];
    char     names[64][128];
    int      states[64];
    int user_count = poll ? poll(userdata, user_ids, names, states, 64) : 0;
    PROF_TICK();

    {
        double now = ImGui::GetTime();
        for (int i = 0; i < user_count; i++) {
            int found = -1;
            for (int j = 0; j < g_user_timestamp_count; j++) {
                if (g_user_timestamps[j] == user_ids[i]) { found = j; break; }
            }
            if (found < 0) {
                if (g_user_timestamp_count < 64) {
                    g_user_timestamps[g_user_timestamp_count++] = user_ids[i];
                    found = g_user_timestamp_count - 1;
                }
            }
            if (found > 0) {
                uint32_t tmp = g_user_timestamps[found];
                memmove(&g_user_timestamps[1], &g_user_timestamps[0],
                        (size_t)found * sizeof(uint32_t));
                g_user_timestamps[0] = tmp;
            }
        }
    }

    int  display_idx[64];
    int  display_count = 0;
    {
        bool used[64] = {false};
        for (int t = 0; t < g_user_timestamp_count && display_count < user_count; t++) {
            for (int i = 0; i < user_count; i++) {
                if (!used[i] && user_ids[i] == g_user_timestamps[t]) {
                    used[i] = true;
                    display_idx[display_count++] = i;
                    break;
                }
            }
        }
        for (int i = 0; i < user_count && display_count < user_count; i++) {
            if (!used[i]) {
                used[i] = true;
                display_idx[display_count++] = i;
            }
        }
    }

    /* ---- Auto-show logic ---- */
    if (g_window_hidden && !g_user_hid_window && user_count > 0) {
        if (!g_user_saw_speaking_after_hide) {
            g_user_saw_speaking_after_hide = true;
            g_window_hidden = false;
            glfwShowWindow(g_window);
        }
    } else if (user_count == 0) {
        g_user_saw_speaking_after_hide = false;
    }

    /* ---- Low-power sleep when window is hidden ---- */
    if (g_window_hidden) {
        /* Properly end the ImGui frame (ImGui::NewFrame was called above) */
        ImGui::EndFrame();
        /*
         * glfwWaitEventsTimeout processes OS messages (needed for the
         * low-level keyboard hook on Windows) and sleeps for up to 50 ms.
         * This drops CPU usage from 100% of vsync to near 0% while hidden,
         * while still waking promptly for hotkeys (Ctrl+Shift+P/H).
         */
        glfwWaitEventsTimeout(0.05);
        return true;
    }

    /* ================================================================
     * Main panel rendering area
     * ================================================================ */
    if (!g_window_hidden) {
        ImGuiWindowFlags main_flags = ImGuiWindowFlags_NoTitleBar
                                    | ImGuiWindowFlags_NoResize
                                    | ImGuiWindowFlags_NoCollapse
                                    | ImGuiWindowFlags_NoBringToFrontOnFocus
                                    | ImGuiWindowFlags_NoSavedSettings
                                    | ImGuiWindowFlags_AlwaysAutoResize;

        if (g_config.mouse_passthrough) {
            main_flags |= ImGuiWindowFlags_NoInputs;
        }

        ImGuiViewport* main_viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(main_viewport->Pos);
        ImGui::SetNextWindowViewport(main_viewport->ID);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));

        float win_alpha = clamp01f(g_config.alpha);
        ImGuiStyle& style = ImGui::GetStyle();

        /*
         * Push style colors with applied alpha every frame unconditionally.
         * ImGui's PushStyleColor/PopStyleColor work as a stack: we push N colors
         * at the start of the frame and pop them at the end.  Skipping pushes
         * based on a cache would let the base (opaque) style leak through.
         */
        const int PUSHED_STYLE_COUNT = 10;
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(style.Colors[ImGuiCol_Border].x, style.Colors[ImGuiCol_Border].y, style.Colors[ImGuiCol_Border].z, style.Colors[ImGuiCol_Border].w * win_alpha));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(style.Colors[ImGuiCol_Button].x, style.Colors[ImGuiCol_Button].y, style.Colors[ImGuiCol_Button].z, style.Colors[ImGuiCol_Button].w * win_alpha));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(style.Colors[ImGuiCol_ButtonHovered].x, style.Colors[ImGuiCol_ButtonHovered].y, style.Colors[ImGuiCol_ButtonHovered].z, style.Colors[ImGuiCol_ButtonHovered].w * win_alpha));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(style.Colors[ImGuiCol_ButtonActive].x, style.Colors[ImGuiCol_ButtonActive].y, style.Colors[ImGuiCol_ButtonActive].z, style.Colors[ImGuiCol_ButtonActive].w * win_alpha));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(style.Colors[ImGuiCol_Separator].x, style.Colors[ImGuiCol_Separator].y, style.Colors[ImGuiCol_Separator].z, style.Colors[ImGuiCol_Separator].w * win_alpha));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(style.Colors[ImGuiCol_ScrollbarBg].x, style.Colors[ImGuiCol_ScrollbarBg].y, style.Colors[ImGuiCol_ScrollbarBg].z, style.Colors[ImGuiCol_ScrollbarBg].w * win_alpha));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(style.Colors[ImGuiCol_ScrollbarGrab].x, style.Colors[ImGuiCol_ScrollbarGrab].y, style.Colors[ImGuiCol_ScrollbarGrab].z, style.Colors[ImGuiCol_ScrollbarGrab].w * win_alpha));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(style.Colors[ImGuiCol_ScrollbarGrabHovered].x, style.Colors[ImGuiCol_ScrollbarGrabHovered].y, style.Colors[ImGuiCol_ScrollbarGrabHovered].z, style.Colors[ImGuiCol_ScrollbarGrabHovered].w * win_alpha));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(style.Colors[ImGuiCol_ScrollbarGrabActive].x, style.Colors[ImGuiCol_ScrollbarGrabActive].y, style.Colors[ImGuiCol_ScrollbarGrabActive].z, style.Colors[ImGuiCol_ScrollbarGrabActive].w * win_alpha));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(style.Colors[ImGuiCol_FrameBg].x, style.Colors[ImGuiCol_FrameBg].y, style.Colors[ImGuiCol_FrameBg].z, style.Colors[ImGuiCol_FrameBg].w * win_alpha));

        int pushed_colors = PUSHED_STYLE_COUNT;

        ImGui::Begin("SpeakingOverlayMain", NULL, main_flags);
        ImGui::PopStyleVar();

        bool is_interactive_hovered = false;

        ImVec4 base_text_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        base_text_col.w = clamp01f(g_config.text_alpha);
        ImGui::PushStyleColor(ImGuiCol_Text, base_text_col); pushed_colors++;

        float max_w = 0.0f;
        for (int di = 0; di < display_count; di++) {
            int i = display_idx[di];
            char text_buf[256];
            snprintf(text_buf, sizeof(text_buf), "  \xe2\x97\x8f  %s  ", names[i]);
            float w = ImGui::CalcTextSize(text_buf).x;

            const char *status_text;
            switch (states[i]) {
                case 1: status_text = LOC("说话", "Talking"); break;
                case 2: status_text = LOC("密语", "Whisper"); break;
                case 3: status_text = LOC("喊话", "Shout"); break;
                default: status_text = LOC("空闲", "Idle"); break;
            }
            float st_w = ImGui::CalcTextSize(status_text).x;
            float row_w = w + st_w + 30.0f; 
            if (row_w > max_w) max_w = row_w;
        }

        float top_bar_w = 0.0f;
        if (!g_config.mouse_passthrough) {
            top_bar_w = ImGui::CalcTextSize(LOC("说话列表", "Speaking Users")).x 
                      + ImGui::CalcTextSize(LOC("设置", "Settings")).x 
                      + ImGui::CalcTextSize("X").x + 40.0f;
        }
        if (max_w < top_bar_w) max_w = top_bar_w;
        
        int max_vis = g_config.max_visible_speakers;
        if (display_count > max_vis) {
            max_w += ImGui::GetStyle().ScrollbarSize + 10.0f; 
        }

        if (user_count == 0) {
            float empty_w = ImGui::CalcTextSize(LOC("  当前没人说话...", "  Nobody is speaking...")).x + 20.0f;
            if (max_w < empty_w) max_w = empty_w;
        }

        if (!g_config.mouse_passthrough) {
            float title_h = ImGui::GetFrameHeight() + 4.0f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);

            ImVec2 title_pos = ImGui::GetCursorScreenPos();
            ImGui::TextColored(with_text_alpha(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)), LOC("说话列表", "Speaking Users"));

            float btn_w = ImGui::CalcTextSize(LOC("设置", "Settings")).x + 12.0f;
            float close_w = ImGui::CalcTextSize("X").x + 12.0f;
            float btn_area = btn_w + 4.0f + close_w;

            ImGui::SameLine(0.0f, -1.0f);
            float cx = ImGui::GetCursorPosX();
            
            float right_edge = ImGui::GetCursorStartPos().x + max_w;
            float btn_target_x = right_edge - btn_area;
            if (btn_target_x < cx) btn_target_x = cx;
            ImGui::SetCursorPosX(btn_target_x);

            if (ImGui::SmallButton(LOC("设置", "Settings"))) {
                g_settings_open = !g_settings_open;
            }
            if (ImGui::IsItemHovered()) is_interactive_hovered = true;

            ImGui::SameLine(0.0f, 2.0f);
            ImGui::SetCursorPosX(btn_target_x + btn_w + 4.0f);
            if (ImGui::SmallButton("X")) {
                g_user_hid_window = true;
                g_window_hidden = true;
                glfwHideWindow(g_window);
                g_drag_active = false;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", LOC("隐藏窗口（插件继续运行）", "Hide window (plugin keeps running)"));
                is_interactive_hovered = true;
            }
        }

        if (!g_config.mouse_passthrough) ImGui::Separator();

        bool should_snap_to_top = false;
        bool mouse_hovering = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
        if (mouse_hovering || ImGui::IsAnyItemActive() || ImGui::IsAnyItemFocused()) {
            g_last_mouse_activity_time = ImGui::GetTime();
        }
        double idle_seconds = ImGui::GetTime() - g_last_mouse_activity_time;
        if (g_config.mouse_passthrough || idle_seconds > 10.0) {
            if (!g_scrolled_by_user) should_snap_to_top = true;
        } else {
            g_scrolled_by_user = false;
        }

        if (user_count == 0) {
            ImGui::TextColored(with_text_alpha(ImVec4(0.45f, 0.45f, 0.45f, 1.0f)), LOC("  当前没人说话...", "  Nobody is speaking..."));
        } else {
            int pinned_count = (display_count < max_vis) ? display_count : max_vis;
            // Add 4px margin to prevent floating-point rounding from forcing a scrollbar unnecessarily
            float child_h = pinned_count * ImGui::GetTextLineHeightWithSpacing() + 4.0f;
            if (display_count > max_vis) {
                child_h += 6.0f;
                child_h += ImGui::GetTextLineHeightWithSpacing();
            }

            // Dynamically configure child flags: suppress scrollbar when count fits without scrolling
            ImGuiWindowFlags child_flags = ImGuiWindowFlags_NoSavedSettings;
            if (display_count <= max_vis) {
                child_flags |= ImGuiWindowFlags_NoScrollbar;
            }

            ImGui::BeginChild("SpeakerList", ImVec2(max_w, child_h), ImGuiChildFlags_None, child_flags);

            if (should_snap_to_top) {
                ImGui::SetScrollHereY(0.0f);
            } else {
                float cur_scroll = ImGui::GetScrollY();
                if (cur_scroll != g_last_scroll_y && cur_scroll > 0.0f) {
                    g_scrolled_by_user = true;
                    g_last_mouse_activity_time = ImGui::GetTime();
                }
                g_last_scroll_y = cur_scroll;
            }

            for (int di = 0; di < display_count; di++) {
                int i = display_idx[di];
                bool is_idle = (states[i] == 0 || states[i] == 4);

                /* Filter: skip idle users unless show_all_users or show_recent_speakers */
                if (is_idle && !g_config.show_all_users && !g_config.show_recent_speakers) continue;

                ImVec4 col;
                const char *status_text;
                switch (states[i]) {
                    case 1: col = ImVec4(0.2f, 1.0f, 0.3f, 1.0f); status_text = LOC("说话", "Talking"); break;
                    case 2: col = ImVec4(1.0f, 0.9f, 0.2f, 1.0f); status_text = LOC("密语", "Whisper"); break;
                    case 3: col = ImVec4(1.0f, 0.25f, 0.25f, 1.0f); status_text = LOC("喊话", "Shout"); break;
                    default: col = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); status_text = LOC("空闲", "Idle"); break;
                }

                if (di == pinned_count) {
                    ImGui::Separator();
                    ImGui::TextColored(with_text_alpha(ImVec4(0.4f, 0.4f, 0.4f, 1.0f)), "%s", LOC("---------- 更多 ----------", "--- more ---"));
                }

                bool is_pinned = (di < pinned_count);
                ImVec4 name_col = col;
                if (!is_pinned) name_col.w *= 0.7f;
                if (is_idle) name_col.w *= g_config.idle_user_alpha;

                ImGui::TextColored(with_text_alpha(name_col), "  \xe2\x97\x8f  %s  ", names[i]);

                ImGui::SameLine(0.0f, -1.0f);
                float text_w = ImGui::CalcTextSize(status_text).x;
                float avail_w = ImGui::GetContentRegionAvail().x;
                float cur_x = ImGui::GetCursorPosX();
                float target_x = cur_x + avail_w - text_w;
                if (target_x > cur_x) {
                    ImGui::SetCursorPosX(target_x);
                }
                ImVec4 st_col = ImVec4(0.5f, 0.5f, 0.5f, is_pinned ? 1.0f : 0.5f);
                if (is_idle) st_col.w *= g_config.idle_user_alpha;
                ImGui::TextColored(with_text_alpha(st_col), "%s", status_text);
            }
            ImGui::EndChild();

            // Exclude scrollbar area from drag detection to avoid conflict with scrolling
            if (display_count > max_vis) {
                ImVec2 child_min = ImGui::GetItemRectMin();
                ImVec2 child_max = ImGui::GetItemRectMax();
                float scrollbar_width = ImGui::GetStyle().ScrollbarSize;
                ImVec2 scrollbar_min = ImVec2(child_max.x - scrollbar_width, child_min.y);
                if (ImGui::IsMouseHoveringRect(scrollbar_min, child_max, false)) {
                    is_interactive_hovered = true;
                }
            }
        }

        ImGui::Dummy(ImVec2(max_w, 0.0f));

        // New drag logic: drag anywhere on the main window except over interactive elements
        if (!g_config.mouse_passthrough) {
            bool can_drag = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && !is_interactive_hovered;
            
            if (can_drag || g_drag_active) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            
            if (can_drag && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_drag_active = true;
                glfwGetWindowPos(g_window, &g_drag_win_x, &g_drag_win_y);
                ImVec2 mouse = ImGui::GetIO().MousePos;
                g_drag_mouse_x = mouse.x;
                g_drag_mouse_y = mouse.y;
            }
            if (g_drag_active) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    ImVec2 mouse = ImGui::GetIO().MousePos;
                    int new_x = g_drag_win_x + (int)(mouse.x - g_drag_mouse_x);
                    int new_y = g_drag_win_y + (int)(mouse.y - g_drag_mouse_y);
                    g_config.window_x = new_x;
                    g_config.window_y = new_y;
                    glfwSetWindowPos(g_window, new_x, new_y);
                } else {
                    g_drag_active = false;
                }
            }
        }

        /* --- Core fix: use GetCursorPosY() for accurate content height, add 2-px tolerance --- */
        if (!g_drag_active) {
            float target_w_f = max_w + ImGui::GetStyle().WindowPadding.x * 2.0f;
            // Use GetCursorPosY() instead of GetWindowSize() to get the true layout height
            float target_h_f = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
            
            int target_w = (int)(target_w_f + 0.5f);
            int target_h = (int)(target_h_f + 0.5f);
            
            // Enforce a minimum native window size so it never shrinks into a state from which it cannot expand
            if (target_w < 150) target_w = 150;
            if (target_h < 40)  target_h = 40;
            
            int cur_w, cur_h;
            glfwGetWindowSize(g_window, &cur_w, &cur_h);
            
            // 2-pixel tolerance to prevent infinite resize loops at the OS level.
            // This fixes TopMost and OBS capture breaking due to 60 FPS continuous resizing.
            if (abs(cur_w - target_w) > 2 || abs(cur_h - target_h) > 2) {
                glfwSetWindowSize(g_window, target_w, target_h);
                g_config.window_width = target_w;
                g_config.window_height = target_h;
            }
            g_first_frame = false;

            /* Auto-popup settings on first launch if RegisterHotKey failed (conflict).
             * Only on the very first frame, never during gameplay. */
            if (g_hotkey_conflict_on_init) {
                g_hotkey_conflict_on_init = false;
                g_settings_open = true;
            }
        }

        ImGui::PopStyleColor(pushed_colors);
        ImGui::End();
    }

    /* ================================================================
     * Settings panel (rendered independently from main-window visibility)
     * ================================================================ */
    if (g_settings_open) {
        ImGui::Begin(LOC("设置", "Settings"), &g_settings_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
            // Track whether any relevant setting has changed
            bool settings_changed = false;

            float a = g_config.alpha;
            if (ImGui::SliderFloat(LOC("窗口透明度", "Window opacity"), &a, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_None)) settings_changed = true;
            float ta = g_config.text_alpha;
            if (ImGui::SliderFloat(LOC("文字透明度", "Text opacity"), &ta, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_None)) settings_changed = true;

            // Dangerous alpha cap only applies to text alpha; window alpha is always free
            bool any_low = (ta < 0.2f);
            if (any_low && !g_config.dangerous_alpha_allowed) {
                if (ta < 0.2f) ta = 0.2f;
            }
            g_config.alpha = a;
            g_config.text_alpha = ta;

            if (ImGui::Checkbox(LOC("允许危险透明度", "Allow risky opacity"), &g_config.dangerous_alpha_allowed)) settings_changed = true;
            if (any_low && !g_config.dangerous_alpha_allowed) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f),
                    LOC("文字透明度不低于 0.2。\n勾选后可调低。", "Text opacity capped at 0.2.\nCheck to allow lower values."));
            }

            ImGui::Separator();

            if (ImGui::SliderFloat(LOC("缩放", "Scale"), &g_config.window_scale, 1.0f, 4.0f, "%.1f", ImGuiSliderFlags_None)) settings_changed = true;
            {
                ImGuiIO& io = ImGui::GetIO();
                io.FontGlobalScale = g_config.window_scale;
            }

            ImGui::Separator();

            if (ImGui::Checkbox(LOC("窗口置顶", "Always on top"), &g_config.always_on_top)) settings_changed = true;

            if (ImGui::Checkbox(LOC("鼠标穿透", "Mouse passthrough"), &g_config.mouse_passthrough)) settings_changed = true;
            if (g_config.mouse_passthrough) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f),
                    LOC("启用后将无法用鼠标点击窗口。\n使用快捷键可关闭穿透。", "Cannot click the window once enabled.\nUse the hotkey to disable passthrough."));
            }

            ImGui::Separator();

            if (ImGui::SliderInt(LOC("可见发言人数", "Visible speakers"), &g_config.max_visible_speakers, 1, 64, "%d", ImGuiSliderFlags_None)) settings_changed = true;

            ImGui::Separator();

            /* ---- User display options ---- */
            if (ImGui::Checkbox(LOC("一直显示所有用户", "Always show all users"), &g_config.show_all_users)) {
                settings_changed = true;
                if (g_config.show_all_users) {
                    g_config.show_recent_speakers = false;
                }
            }
            if (g_config.show_all_users) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                    LOC("所有已知用户将始终显示在列表中。", "All known users will always be shown in the list."));
            } else {
                ImGui::Spacing();
                if (ImGui::Checkbox(LOC("显示刚刚发言的用户", "Show recently speaking users"), &g_config.show_recent_speakers)) settings_changed = true;
                if (g_config.show_recent_speakers) {
                    if (ImGui::SliderFloat(LOC("未发言用户透明度", "Idle user opacity"), &g_config.idle_user_alpha, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_None)) settings_changed = true;
                    if (g_config.idle_user_alpha < 0.2f && !g_config.dangerous_alpha_allowed) {
                        g_config.idle_user_alpha = 0.2f;
                    }
                    if (ImGui::SliderInt(LOC("空闲超时(秒)", "Idle timeout(s)"), &g_config.idle_timeout_seconds, 1, 120, "%d", ImGuiSliderFlags_None)) settings_changed = true;
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                        LOC("超过超时时间的未发言用户将从列表中移除。", "Idle users beyond timeout will be removed from the list."));
                } else {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                        LOC("仅显示正在发言的用户。", "Only show users who are currently speaking."));
                }
            }

            ImGui::Spacing();
            if (ImGui::Checkbox(LOC("仅显示当前频道用户", "Only show current channel"), &g_config.show_current_channel_only)) settings_changed = true;
            if (g_config.show_current_channel_only) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                    LOC("仅显示与你在同一频道的用户。", "Only show users in the same channel as you."));
            }

            ImGui::Separator();

            if (ImGui::Checkbox(LOC("在 Mumble 输出日志", "Log to Mumble console"), &g_config.mumble_logging_enabled)) settings_changed = true;

            ImGui::Separator();

            /* ---- Debug options ---- */
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                LOC("调试选项", "Debug options"));
            if (ImGui::Checkbox(LOC("每10秒输出帧率到日志", "Log FPS every 10s"), &g_config.debug_show_fps)) settings_changed = true;

            ImGui::Separator();

            /* ---- Global hotkeys ---- */
            {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                    LOC("全局快捷键", "Global hotkeys"));

                /* Format a Win32 VK + modifiers as a human-readable string using ImGui::GetKeyName */
                auto format_hotkey = [](int vk, int mods) -> std::string {
                    if (vk == 0) return LOC("未设置", "Unset");
                    /* Map common VK codes to ImGuiKey for GetKeyName */
                    ImGuiKey ikey = ImGuiKey_None;
                    if (vk >= 'A' && vk <= 'Z') ikey = (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
                    else if (vk >= '0' && vk <= '9') ikey = (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
                    else if (vk >= VK_F1 && vk <= VK_F24) ikey = (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
                    else switch (vk) {
                        case VK_SPACE:   ikey = ImGuiKey_Space; break;
                        case VK_RETURN:  ikey = ImGuiKey_Enter; break;
                        case VK_TAB:     ikey = ImGuiKey_Tab; break;
                        case VK_ESCAPE:  ikey = ImGuiKey_Escape; break;
                        case VK_BACK:    ikey = ImGuiKey_Backspace; break;
                        case VK_DELETE:  ikey = ImGuiKey_Delete; break;
                        case VK_INSERT:  ikey = ImGuiKey_Insert; break;
                        case VK_HOME:    ikey = ImGuiKey_Home; break;
                        case VK_END:     ikey = ImGuiKey_End; break;
                        case VK_PRIOR:   ikey = ImGuiKey_PageUp; break;
                        case VK_NEXT:    ikey = ImGuiKey_PageDown; break;
                        case VK_LEFT:    ikey = ImGuiKey_LeftArrow; break;
                        case VK_RIGHT:   ikey = ImGuiKey_RightArrow; break;
                        case VK_UP:      ikey = ImGuiKey_UpArrow; break;
                        case VK_DOWN:    ikey = ImGuiKey_DownArrow; break;
                        case VK_OEM_MINUS:    ikey = ImGuiKey_Minus; break;
                        case VK_OEM_PERIOD:   ikey = ImGuiKey_Period; break;
                        case VK_OEM_COMMA:    ikey = ImGuiKey_Comma; break;
                        case VK_OEM_1:        ikey = ImGuiKey_Semicolon; break;
                        case VK_OEM_2:        ikey = ImGuiKey_Slash; break;
                        case VK_OEM_3:        ikey = ImGuiKey_GraveAccent; break;
                        case VK_OEM_4:        ikey = ImGuiKey_LeftBracket; break;
                        case VK_OEM_5:        ikey = ImGuiKey_Backslash; break;
                        case VK_OEM_6:        ikey = ImGuiKey_RightBracket; break;
                        case VK_OEM_7:        ikey = ImGuiKey_Apostrophe; break;
                        default: break;
                    }
                    std::string s;
                    if (mods & MOD_CONTROL) s += "Ctrl+";
                    if (mods & MOD_SHIFT)   s += "Shift+";
                    if (mods & MOD_ALT)     s += "Alt+";
                    if (mods & MOD_WIN)     s += "Win+";
                    const char *kn = ikey != ImGuiKey_None ? ImGui::GetKeyName(ikey) : nullptr;
                    s += kn ? kn : std::to_string(vk);
                    return s;
                };

                /* Static state for hotkey capture */
                static int  g_hotkey_capture_target = 0; /* 0=none, 1=toggle, 2=show */
                static bool g_hotkey_capture_active = false;

                auto hotkey_button = [&](const char* label, const char* label_en,
                                         int *pvk, int *pmods, int target_id) {
                    ImGui::Text("%s", LOC(label, label_en));
                    ImGui::SameLine(180.0f);
                    std::string cap_text;
                    if (g_hotkey_capture_active && g_hotkey_capture_target == target_id) {
                        cap_text = LOC("请按键… (Esc取消)", "Press key… (Esc cancel)");
                    } else {
                        cap_text = format_hotkey(*pvk, *pmods);
                    }
                    if (ImGui::Button(cap_text.c_str(), ImVec2(200.0f, 0.0f))) {
                        g_hotkey_capture_active = true;
                        g_hotkey_capture_target = target_id;
                    }

                    if (g_hotkey_capture_active && g_hotkey_capture_target == target_id) {
                        ImGuiIO& io_cap = ImGui::GetIO();
                        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                            g_hotkey_capture_active = false;
                        } else {
                            /* Poll all named keys for capture */
                            for (int ki = ImGuiKey_NamedKey_BEGIN; ki < ImGuiKey_NamedKey_END; ki++) {
                                ImGuiKey k = (ImGuiKey)ki;
                                /* Skip modifiers */
                                if (k == ImGuiKey_LeftCtrl || k == ImGuiKey_RightCtrl
                                 || k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift
                                 || k == ImGuiKey_LeftAlt || k == ImGuiKey_RightAlt
                                 || k == ImGuiKey_LeftSuper || k == ImGuiKey_RightSuper)
                                    continue;
                                if (ImGui::IsKeyPressed(k, false)) {
                                    /* Map ImGuiKey back to VK */
                                    int nvk = 0;
                                    if (k >= ImGuiKey_A && k <= ImGuiKey_Z)
                                        nvk = 'A' + (ki - ImGuiKey_A);
                                    else if (k >= ImGuiKey_0 && k <= ImGuiKey_9)
                                        nvk = '0' + (ki - ImGuiKey_0);
                                    else if (k >= ImGuiKey_F1 && k <= ImGuiKey_F24)
                                        nvk = VK_F1 + (ki - ImGuiKey_F1);
                                    else switch (k) {
                                        case ImGuiKey_Space:       nvk = VK_SPACE; break;
                                        case ImGuiKey_Enter:       nvk = VK_RETURN; break;
                                        case ImGuiKey_Tab:         nvk = VK_TAB; break;
                                        case ImGuiKey_Backspace:   nvk = VK_BACK; break;
                                        case ImGuiKey_Delete:      nvk = VK_DELETE; break;
                                        case ImGuiKey_Insert:      nvk = VK_INSERT; break;
                                        case ImGuiKey_Home:        nvk = VK_HOME; break;
                                        case ImGuiKey_End:         nvk = VK_END; break;
                                        case ImGuiKey_PageUp:      nvk = VK_PRIOR; break;
                                        case ImGuiKey_PageDown:    nvk = VK_NEXT; break;
                                        case ImGuiKey_LeftArrow:   nvk = VK_LEFT; break;
                                        case ImGuiKey_RightArrow:  nvk = VK_RIGHT; break;
                                        case ImGuiKey_UpArrow:     nvk = VK_UP; break;
                                        case ImGuiKey_DownArrow:   nvk = VK_DOWN; break;
                                        case ImGuiKey_Minus:       nvk = VK_OEM_MINUS; break;
                                        case ImGuiKey_Period:      nvk = VK_OEM_PERIOD; break;
                                        case ImGuiKey_Comma:       nvk = VK_OEM_COMMA; break;
                                        case ImGuiKey_Semicolon:   nvk = VK_OEM_1; break;
                                        case ImGuiKey_Slash:       nvk = VK_OEM_2; break;
                                        case ImGuiKey_GraveAccent: nvk = VK_OEM_3; break;
                                        case ImGuiKey_LeftBracket: nvk = VK_OEM_4; break;
                                        case ImGuiKey_Backslash:   nvk = VK_OEM_5; break;
                                        case ImGuiKey_RightBracket:nvk = VK_OEM_6; break;
                                        case ImGuiKey_Apostrophe:  nvk = VK_OEM_7; break;
                                        default: break;
                                    }
                                    if (nvk != 0) {
                                        int nmods = 0;
                                        if (io_cap.KeyCtrl)  nmods |= MOD_CONTROL;
                                        if (io_cap.KeyShift) nmods |= MOD_SHIFT;
                                        if (io_cap.KeyAlt)   nmods |= MOD_ALT;
                                        if (io_cap.KeySuper) nmods |= MOD_WIN;
                                        *pvk = nvk;
                                        *pmods = nmods;
                                        settings_changed = true;
                                        g_hotkey_capture_active = false;

                                        /* Re-register hotkeys on change */
                                        unregister_all_hotkeys();
                                        register_all_hotkeys();
                                        /* Clear conflict flag — new key may not conflict */
                                        g_hotkey_conflict_on_init = false;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                };

                hotkey_button("切换穿透", "Toggle Passthrough",
                    &g_config.hotkey_toggle_vk, &g_config.hotkey_toggle_mods, 1);
                hotkey_button("显示窗口", "Show Window",
                    &g_config.hotkey_show_vk, &g_config.hotkey_show_mods, 2);

                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                    LOC("快捷键被占用时，请更换组合键。",
                        "If the hotkey is occupied, change the key combination."));
            }

            ImGui::Separator();

            /* ---- Framerate control (multi-profile, no vsync by default) ---- */
            {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                    LOC("帧率控制", "Framerate control"));

                /* Vsync toggle */
                if (ImGui::Checkbox(LOC("垂直同步", "VSync"), &g_config.vsync_enabled)) settings_changed = true;
                if (g_config.vsync_enabled) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.15f, 1.0f),
                        LOC("⚠ 部分系统上VSync可能导致高CPU占用",
                            "⚠ VSync may cause high CPU usage on some systems"));
                }

                /* Auto-detect monitor refresh rate */
                {
                    bool old_auto = g_config.auto_detect_refresh;
                    if (ImGui::Checkbox(LOC("自动检测显示器刷新率", "Auto-detect monitor refresh rate"),
                                        &g_config.auto_detect_refresh)) {
                        settings_changed = true;
                        if (g_config.auto_detect_refresh && !old_auto) {
                            overlay_apply_auto_refresh();
                        }
                    }
                }

                ImGui::Separator();

                /* Helper: FPS input with +/-1 buttons and 15/30/60 presets */
                auto fps_input = [&](const char* label, const char* label_en, int* fps, bool disabled) {
                    ImGui::Text("%s", LOC(label, label_en));
                    ImGui::SameLine(180.0f);
                    ImGui::PushItemWidth(60.0f);
                    ImGuiInputTextFlags flags = ImGuiInputTextFlags_None;
                    if (disabled) flags |= ImGuiInputTextFlags_ReadOnly;
                    if (disabled) ImGui::BeginDisabled();
                    if (ImGui::InputInt((std::string("##fps_") + label_en).c_str(), fps, 0, 0, flags)) {
                        if (*fps < 15) *fps = 15;
                        if (*fps > 400) *fps = 400;
                        settings_changed = true;
                    }
                    if (disabled) ImGui::EndDisabled();
                    ImGui::PopItemWidth();
                    ImGui::SameLine();
                    if (disabled) ImGui::BeginDisabled();
                    if (ImGui::SmallButton((std::string("15##") + label_en).c_str())) { *fps = 15;  settings_changed = true; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton((std::string("30##") + label_en).c_str())) { *fps = 30;  settings_changed = true; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton((std::string("60##") + label_en).c_str())) { *fps = 60;  settings_changed = true; }
                    if (disabled) ImGui::EndDisabled();
                };

                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                    LOC("优先级: 设置面板 > 可点击 > 穿透。范围 15-400。",
                        "Priority: Settings panel > Clickable > Passthrough. Range 15-400."));

                if (!g_config.vsync_enabled) {
                    bool auto_locked = g_config.auto_detect_refresh;

                    fps_input("点击穿透时 FPS", "Passthrough FPS", &g_config.fps_passthrough, false);
                    fps_input("可点击时 FPS", "Clickable FPS", &g_config.fps_clickable, auto_locked);
                    fps_input("打开设置时 FPS", "Settings FPS", &g_config.fps_settings_open, auto_locked);

                    if (auto_locked) {
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                            LOC("可点击/设置 FPS 由显示器刷新率自动决定。",
                                "Clickable/Settings FPS auto-set from monitor refresh rate."));
                    }
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                        LOC("垂直同步启用时，上述帧率设置无效。",
                            "When VSync is enabled, the FPS settings above have no effect."));
                }
            }

            // Only refresh window properties when the user actually changed a relevant setting
            if (settings_changed) {
                apply_config_to_window();
            }

            ImGui::Separator();

            if (g_window_hidden) {
                if (ImGui::Button(LOC("显示窗口", "Show Window"), ImVec2(-1.0f, 0.0f))) {
                    g_window_hidden = false;
                    g_user_hid_window = false;
                    glfwShowWindow(g_window);
                }
            }

            if (ImGui::Button(LOC("重置窗口位置", "Reset Position"), ImVec2(-1.0f, 0.0f))) {
                overlay_config_t def = overlay_config_default();
                g_config.window_x = def.window_x;
                g_config.window_y = def.window_y;
                g_config.window_width = def.window_width;
                g_config.window_height = def.window_height;
                glfwSetWindowPos(g_window, g_config.window_x, g_config.window_y);
                glfwSetWindowSize(g_window, g_config.window_width, g_config.window_height);
                settings_changed = true;
                if (g_window_hidden) {
                    g_window_hidden = false;
                    g_user_hid_window = false;
                    glfwShowWindow(g_window);
                }
            }

            if (ImGui::Button(LOC("重置所有设置", "Reset All Settings"), ImVec2(-1.0f, 0.0f))) {
                overlay_config_t def = overlay_config_default();
                g_config.alpha             = def.alpha;
                g_config.text_alpha        = def.text_alpha;
                g_config.window_scale      = def.window_scale;
                g_config.mouse_passthrough = def.mouse_passthrough;
                g_config.always_on_top     = def.always_on_top;
                g_config.max_visible_speakers = def.max_visible_speakers;
                g_config.dangerous_alpha_allowed = def.dangerous_alpha_allowed;
                g_config.show_all_users       = def.show_all_users;
                g_config.show_recent_speakers = def.show_recent_speakers;
                g_config.idle_user_alpha   = def.idle_user_alpha;
                g_config.idle_timeout_seconds = def.idle_timeout_seconds;
                g_config.show_current_channel_only = def.show_current_channel_only;
                g_config.mumble_logging_enabled = def.mumble_logging_enabled;
                g_config.vsync_enabled      = def.vsync_enabled;
                g_config.fps_passthrough    = def.fps_passthrough;
                g_config.fps_clickable      = def.fps_clickable;
                g_config.fps_settings_open  = def.fps_settings_open;
                g_config.auto_detect_refresh = def.auto_detect_refresh;
                g_config.hotkey_toggle_vk   = def.hotkey_toggle_vk;
                g_config.hotkey_toggle_mods = def.hotkey_toggle_mods;
                g_config.hotkey_show_vk     = def.hotkey_show_vk;
                g_config.hotkey_show_mods   = def.hotkey_show_mods;
                /* Re-register all hotkeys with defaults */
                unregister_all_hotkeys();
                register_all_hotkeys();
                /* Re-detect when resetting all settings */
                overlay_apply_auto_refresh();
                g_config.window_x = def.window_x;
                g_config.window_y = def.window_y;
                g_config.window_width = def.window_width;
                g_config.window_height = def.window_height;
                glfwSetWindowPos(g_window, g_config.window_x, g_config.window_y);
                glfwSetWindowSize(g_window, g_config.window_width, g_config.window_height);
                {
                    ImGuiIO& io_reset = ImGui::GetIO();
                    io_reset.FontGlobalScale = g_config.window_scale;
                }
                settings_changed = true;
                if (g_window_hidden) {
                    g_window_hidden = false;
                    g_user_hid_window = false;
                    glfwShowWindow(g_window);
                }
            }

            /* Save config to disk when settings change or panel is about to close */
            if (settings_changed || !g_settings_open) {
                overlay_config_save();
            }
        ImGui::End();
    }

    /* ================================================================
     * Render
     * ================================================================ */
    ImGui::Render();

    {
        int fb_w, fb_h;
        glfwGetFramebufferSize(g_window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        /* Preserve alpha 0 when clearing so DWM can compose the transparent framebuffer correctly. */
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    ImDrawData *draw_data = ImGui::GetDrawData();
    if (draw_data != NULL) {
        ImGui_ImplOpenGL3_RenderDrawData(draw_data);
    }

    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
            /* Only perform per-viewport context switches when there are
             * actually detached platform windows (e.g. Settings panel).
             * When only the main viewport exists (Size == 1), the
             * makeContextCurrent + UpdatePlatformWindows + RenderPlatformWindowsDefault
             * dance is a no-op that still calls into wglMakeCurrent every frame,
             * wasting ~9% CPU according to profiling. */
            if (pio.Viewports.Size > 1) {
                GLFWwindow *backup_current = glfwGetCurrentContext();
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
                glfwMakeContextCurrent(backup_current);
            }
        }
    }

    /*
     * Manage all detached ImGui viewports (e.g., Settings window):
     * hide them from the taskbar and synchronize their TopMost status.
     *
     * Cache g_last_topmost so the expensive GetPlatformIO + enumeration
     * is only done when the setting actually changes.  The inner loop
     * already short-circuits per-viewport if no update is needed.
     */
    if (g_config.always_on_top != g_last_topmost) {
        g_last_topmost = g_config.always_on_top;
        HWND main_hwnd = glfwGetWin32Window(g_window);
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        for (int i = 0; i < pio.Viewports.Size; i++) {
            ImGuiViewport* vp = pio.Viewports[i];
            if (vp->PlatformHandle != NULL) {
                HWND hwnd = glfwGetWin32Window((GLFWwindow*)vp->PlatformHandle);

                /* Exclude the main overlay window, which is already managed by apply_config_to_window() */
                if (hwnd != NULL && hwnd != main_hwnd) {
                    LONG_PTR exstyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

                    /* Ensure the viewport remains a tool window (hidden from taskbar) */
                    LONG_PTR needed = exstyle | WS_EX_TOOLWINDOW;
                    needed &= ~WS_EX_APPWINDOW;

                    /* Check if the current Z-order state matches the global setting */
                    bool is_topmost = (exstyle & WS_EX_TOPMOST) != 0;
                    bool needs_z_update = (is_topmost != g_config.always_on_top);

                    if (exstyle != needed || needs_z_update) {
                        SetWindowLongPtr(hwnd, GWL_EXSTYLE, needed);

                        /* Apply HWND_TOPMOST or HWND_NOTOPMOST dynamically */
                        HWND insert_after = g_config.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST;
                        SetWindowPos(hwnd, insert_after, 0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
                                     | SWP_FRAMECHANGED | SWP_NOREDRAW);
                    }
                }
            }
        }
    }

    glfwSwapBuffers(g_window);
    PROF_TICK();

    /* ---- Debug: periodic FPS + per-stage profiling (after full frame) ---- */
    if (g_config.debug_show_fps) {
        double now = glfwGetTime();
        if (now - g_last_fps_log_time >= 10.0) {
            g_last_fps_log_time = now;
            if (g_prof_idx >= 4 && g_prof_freq.QuadPart > 0) {
                double us[4];
                const LARGE_INTEGER *prev = &g_prof_t0;
                for (int pi = 0; pi < 4; pi++) {
                    us[pi] = (double)(g_prof_t1[pi].QuadPart - prev->QuadPart)
                           * 1000000.0 / (double)g_prof_freq.QuadPart;
                    prev = &g_prof_t1[pi];
                }
                long long wrp_skip = 0, wrp_pass = 0;
                OverlayGlfwAttribGetStats(&wrp_skip, &wrp_pass);
                long long wrp_total = wrp_skip + wrp_pass;
                int wrp_pct = wrp_total > 0 ? (int)(100LL * wrp_skip / wrp_total) : 0;
                OW_LOGF("FPS: %.1f (tgt:%d)  [poll:%.0fus newfrm:%.0fus keys+speak:%.0fus ui+rendr+swap:%.0fus]  wrapper: %lld/%lld saved (%d%%)",
                        (double)ImGui::GetIO().Framerate,
                        g_frame_target_interval > 0.0 ? (int)(1.0 / g_frame_target_interval) : 0,
                        us[0], us[1], us[2], us[3],
                        wrp_skip, wrp_total, wrp_pct);
            }
        }
    }

    /* ---- Recalculate target FPS based on priority rules ---- */
    if (!g_config.vsync_enabled && !g_window_hidden) {
        int target_fps;
        if (g_settings_open) {
            /* Highest priority: settings panel is open */
            target_fps = g_config.fps_settings_open;
        } else if (!g_config.mouse_passthrough) {
            /* Window is clickable (not in passthrough mode) */
            target_fps = g_config.fps_clickable;
        } else {
            /* Passthrough mode */
            target_fps = g_config.fps_passthrough;
        }
        if (target_fps < 1) target_fps = 1;
        if (target_fps > 400) target_fps = 400;
        g_frame_target_interval = 1.0 / (double)target_fps;
    }

    /* ---- Two-phase frame pacing (plan.txt research) ----
     * Phase 1: Sleep via waitable timer until g_spin_margin before deadline.
     * Phase 2: Spin _mm_pause() for the final margin to absorb scheduler jitter.
     * The spin margin auto-adjusts via EMA of measured late-wake times.
     * Fallback: Sleep() when no waitable timer is available (extremely rare). */
    if (g_frame_target_interval > 0.0) {
        double now = glfwGetTime();
        if (g_last_frame_time > 0.0) {
            double deadline  = g_last_frame_time + g_frame_target_interval;
            double remaining = deadline - now;

            if (remaining > g_spin_margin) {
                double sleep_duration = remaining - g_spin_margin;

                if (g_frame_timer != NULL) {
                    /* High-precision path: SetWaitableTimer with 100ns units.
                     * Negative QuadPart = relative time. */
                    LARGE_INTEGER due;
                    due.QuadPart = (LONGLONG)(sleep_duration * -10000000.0);
                    if (due.QuadPart == 0) {
                        due.QuadPart = -1;  /* minimum non-zero relative time */
                    }
                    SetWaitableTimer(g_frame_timer, &due, 0, NULL, NULL, FALSE);
                    WaitForSingleObject(g_frame_timer, INFINITE);

                    /* Measure how late the timer actually woke us up
                     * and feed it into an EMA to auto-tune g_spin_margin. */
                    double post_sleep  = glfwGetTime();
                    double actual_sleep = post_sleep - now;
                    double late = actual_sleep - sleep_duration;
                    double clamped_late = (late > 0.0) ? late : 0.0;

                    if (g_spin_samples == 0) {
                        g_spin_ema = clamped_late;
                    } else {
                        g_spin_ema = g_spin_ema * (1.0 - SPIN_MARGIN_ALPHA)
                                   + clamped_late * SPIN_MARGIN_ALPHA;
                    }
                    g_spin_samples++;

                    g_spin_margin = g_spin_ema + SPIN_MARGIN_BUFFER;
                    if (g_spin_margin < SPIN_MARGIN_MIN) g_spin_margin = SPIN_MARGIN_MIN;
                    if (g_spin_margin > SPIN_MARGIN_MAX) g_spin_margin = SPIN_MARGIN_MAX;
                } else {
                    /* Last-resort fallback: Sleep().
                     * On modern Windows with timeBeginPeriod(1) this gives ~1ms
                     * granularity. Without it, ~15.6ms — but this path is only
                     * reached if BOTH CreateWaitableTimerExW and CreateWaitableTimer
                     * failed, which is extremely unlikely. */
                    DWORD sleep_ms = (DWORD)(sleep_duration * 1000.0);
                    if (sleep_ms > 0) {
                        Sleep(sleep_ms);
                    }
                }
            }

            /* Phase 2: Spin-wait the final margin to hit the deadline precisely.
             * _mm_pause() hints the CPU to avoid memory-order mis-speculation
             * and reduces power consumption during the spin loop.
             *
             * If we were preempted (e.g. high-priority process stole the CPU),
             * glfwGetTime() will already be past the deadline — the loop exits
             * immediately with zero wasted cycles. */
            while (glfwGetTime() < deadline) {
                _mm_pause();
            }
        }
        g_last_frame_time = glfwGetTime();
    }

    /* Track window position / size for config persistence */
    glfwGetWindowPos(g_window, &g_config.window_x, &g_config.window_y);
    glfwGetWindowSize(g_window, &g_config.window_width, &g_config.window_height);

    return true;
}

/* ========================================================================
 * Shutdown
 * ======================================================================== */
/* ---- atexit handler: ensure timeEndPeriod even on abnormal exit ---- */
static void cleanup_time_period(void) {
    if (g_using_time_period) {
        timeEndPeriod(1);
        g_using_time_period = false;
    }
}

void overlay_window_shutdown(void) {
    if (g_frame_timer != NULL) {
        CloseHandle(g_frame_timer);
        g_frame_timer = NULL;
    }
    cleanup_time_period();

    unregister_all_hotkeys();

    if (g_window != NULL) {
        if (g_prev_wndproc != NULL) {
            HWND hwnd = glfwGetWin32Window(g_window);
            if (hwnd != NULL) {
                SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)g_prev_wndproc);
            }
            g_prev_wndproc = NULL;
        }
        overlay_config_save();

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(g_window);
        g_window = NULL;
    }
    glfwTerminate();
}

/* ========================================================================
 * Get current config
 * ======================================================================== */
void overlay_window_get_config(overlay_config_t *cfg) {
    *cfg = g_config;
}

/* ========================================================================
 * Request API — called from Mumble main thread
 * ======================================================================== */
void overlay_window_request_show(void) {
    g_request_show = true;
}

void overlay_window_request_reset_position(void) {
    g_request_reset_position = true;
}