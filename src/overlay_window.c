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

#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

/* ---- Overlay internal header ---- */
#include "overlay_window.h"

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

/* ---- Global keyboard hook ---- */
static HHOOK  g_keyboard_hook     = NULL;
static WNDPROC g_prev_wndproc       = NULL;
/* Thread-safe flags set by the keyboard hook, read by render thread */
static volatile LONG g_hotkey_toggle_passthrough = 0;
static volatile LONG g_hotkey_show_window        = 0;

/* ---- Forward declarations ---- */
static LRESULT CALLBACK low_level_keyboard_proc(int nCode, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK overlay_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void apply_config_to_window(void);
static void on_window_close(GLFWwindow *win);

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
 * Global hotkey — low-level keyboard hook
 * ======================================================================== */
static LRESULT CALLBACK low_level_keyboard_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
            if (ctrl && shift) {
                if (kb->vkCode == 'P') {
                    InterlockedExchange(&g_hotkey_toggle_passthrough, 1);
                } else if (kb->vkCode == 'H') {
                    InterlockedExchange(&g_hotkey_show_window, 1);
                }
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
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
    glfwSwapInterval(1);

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

    /* Install keyboard hook for global hotkeys */
    g_keyboard_hook = SetWindowsHookEx(WH_KEYBOARD_LL, low_level_keyboard_proc,
                                       GetModuleHandle(NULL), 0);

    g_first_frame = true;

    /* Initialise optimisation cache to match the freshly loaded config. */
    g_last_topmost   = g_config.always_on_top;

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
    glfwPollEvents();

    ImGuiIO& io = ImGui::GetIO();
    if (g_config.mouse_passthrough && !io.WantCaptureMouse) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    /* ---- Debug: periodic FPS logging ---- */
    if (g_config.debug_show_fps) {
        double now = ImGui::GetTime();
        if (now - g_last_fps_log_time >= 10.0) {
            g_last_fps_log_time = now;
            OW_LOGF("FPS: %.1f", (double)ImGui::GetIO().Framerate);
        }
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
                    LOC("启用后将无法用鼠标点击窗口。\n按 Ctrl+Shift+P 可关闭穿透。", "Cannot click the window once enabled.\nPress Ctrl+Shift+P to disable."));
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
            GLFWwindow *backup_current = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current);
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

    /* Track window position / size for config persistence */
    glfwGetWindowPos(g_window, &g_config.window_x, &g_config.window_y);
    glfwGetWindowSize(g_window, &g_config.window_width, &g_config.window_height);

    return true;
}

/* ========================================================================
 * Shutdown
 * ======================================================================== */
void overlay_window_shutdown(void) {
    if (g_keyboard_hook != NULL) {
        UnhookWindowsHookEx(g_keyboard_hook);
        g_keyboard_hook = NULL;
    }

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