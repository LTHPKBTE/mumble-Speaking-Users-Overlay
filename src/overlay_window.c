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

#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#define GL_SILENCE_DEPRECATION
#endif

#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
/* Windows SDK provides GL/gl.h via glfw3.h auto-include */
#else
/* Minimal GL 1.1 declarations (link with -lGL / OpenGL.framework) */
#define GL_COLOR_BUFFER_BIT 0x00004000
extern "C" {
void glClear(unsigned int mask);
void glClearColor(float r, float g, float b, float a);
void glViewport(int x, int y, int w, int h);
}
#endif

/* ---- Overlay internal header ---- */
#include "overlay_window.h"

/* ========================================================================
 * Localisation: detect system language
 * ======================================================================== */
static int g_lang_is_chinese = 0;

static void detect_system_language(void) {
#ifdef _WIN32
    LANGID langID = GetUserDefaultUILanguage();
    WORD primary  = PRIMARYLANGID(langID);
    g_lang_is_chinese = (primary == LANG_CHINESE
                         || primary == LANG_CHINESE_SIMPLIFIED
                         || primary == LANG_CHINESE_TRADITIONAL);
#else
    const char *lang = getenv("LANG");
    if (lang != NULL) {
        g_lang_is_chinese = (strncmp(lang, "zh", 2) == 0);
    }
#endif
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
static bool             g_user_saw_speaking_after_hide = false;

/* ---- Request flags (set from Mumble main thread, consumed by render thread) ---- */
static volatile bool    g_request_show          = false;
static volatile bool    g_request_reset_position = false;

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

/* ---- Forward declarations ---- */
static void apply_config_to_window(void);
static void on_window_close(GLFWwindow *win);

/* ========================================================================
 * Font: load CJK glyphs for Chinese / Japanese / Korean
 * ======================================================================== */
static void load_cjk_font(void) {
    ImGuiIO& io = ImGui::GetIO();

    /* List of candidate CJK font paths (tried in order) */
    static const char *candidates[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/msyh.ttc",       /* Microsoft YaHei */
        "C:/Windows/Fonts/msyhbd.ttc",     /* Microsoft YaHei Bold */
        "C:/Windows/Fonts/simhei.ttf",     /* SimHei */
        "C:/Windows/Fonts/simsun.ttc",     /* SimSun */
        "C:/Windows/Fonts/yahei.ttf",      /* alternative */
#elif defined(__APPLE__)
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        "/System/Library/Fonts/NotoSansCJK.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc",
#endif
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
            fprintf(stderr, "Loaded CJK font: %s\n", candidates[i]);
            return; /* success */
        }
    }

    /* No CJK font found – not a fatal error; non-ASCII will show as fallback */
    fprintf(stderr, "No CJK font found, non-ASCII glyphs unavailable\n");
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
    cfg.show_idle_users   = true;
    cfg.idle_user_alpha   = 0.3f;
    cfg.idle_timeout_seconds = 5;
    cfg.mumble_logging_enabled = true;
    return cfg;
}

/* ========================================================================
 * Config persistence — simple key=value file on disk
 * ======================================================================== */
static const char *overlay_config_path(void) {
    static char path[1024];
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata) {
        snprintf(path, sizeof(path), "%s\\Mumble\\SpeakingOverlay.cfg", appdata);
        return path;
    }
#else
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path), "%s/.config/mumble-overlay-plugin.cfg", home);
        return path;
    }
#endif
    return NULL;
}

static void overlay_config_save(void) {
    const char *cfg_path = overlay_config_path();
    if (!cfg_path) return;

    FILE *f = fopen(cfg_path, "w");
    if (!f) return;
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
    fprintf(f, "show_idle_users=%d\n",  g_config.show_idle_users ? 1 : 0);
    fprintf(f, "idle_user_alpha=%.3f\n",(double)g_config.idle_user_alpha);
    fprintf(f, "idle_timeout_seconds=%d\n", g_config.idle_timeout_seconds);
    fprintf(f, "mumble_logging_enabled=%d\n", g_config.mumble_logging_enabled ? 1 : 0);
    fclose(f);
}

static void overlay_config_load(overlay_config_t *cfg) {
    /* Start from defaults, then override from file if it exists */
    *cfg = overlay_config_default();

    const char *cfg_path = overlay_config_path();
    if (!cfg_path) return;

    FILE *f = fopen(cfg_path, "r");
    if (!f) return;

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
        else if (sscanf(line, "show_idle_users=%d", &ival) == 1)   cfg->show_idle_users = (ival != 0);
        else if (sscanf(line, "idle_user_alpha=%f", &fval) == 1)   cfg->idle_user_alpha = fval;
        else if (sscanf(line, "mouse_passthrough=%d", &ival) == 1) cfg->mouse_passthrough = (ival != 0);
        else if (sscanf(line, "always_on_top=%d", &ival) == 1)   cfg->always_on_top = (ival != 0);
        else if (sscanf(line, "max_visible_speakers=%d", &ival) == 1) cfg->max_visible_speakers = ival;
        else if (sscanf(line, "dangerous_alpha_allowed=%d", &ival) == 1) cfg->dangerous_alpha_allowed = (ival != 0);
        else if (sscanf(line, "show_idle_users=%d", &ival) == 1)   cfg->show_idle_users = (ival != 0);
        else if (sscanf(line, "idle_user_alpha=%f", &fval) == 1)   cfg->idle_user_alpha = fval;
        else if (sscanf(line, "idle_timeout_seconds=%d", &ival) == 1) cfg->idle_timeout_seconds = ival;
        else if (sscanf(line, "mumble_logging_enabled=%d", &ival) == 1) cfg->mumble_logging_enabled = (ival != 0);
    }
    fclose(f);
}

/* ========================================================================
 * GLFW error callback
 * ======================================================================== */
static void glfw_error_callback(int error, const char *description) {
    (void)error;
    fprintf(stderr, "GLFW error: %s\n", description);
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
        g_config.show_idle_users   = cfg->show_idle_users;
        g_config.idle_user_alpha   = cfg->idle_user_alpha;
        g_config.window_scale      = cfg->window_scale;
        g_config.mouse_passthrough = cfg->mouse_passthrough;
        g_config.always_on_top     = cfg->always_on_top;
        if (cfg->max_visible_speakers > 0)
            g_config.max_visible_speakers = cfg->max_visible_speakers;
        g_config.dangerous_alpha_allowed = cfg->dangerous_alpha_allowed;
        g_config.show_idle_users   = cfg->show_idle_users;
        g_config.idle_user_alpha   = cfg->idle_user_alpha;
        g_config.idle_timeout_seconds = cfg->idle_timeout_seconds;
        g_config.mumble_logging_enabled = cfg->mumble_logging_enabled;
    }
    detect_system_language();

    /* --- GLFW --- */
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return OW_ERR_GLFW;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING, g_config.always_on_top ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);    /* no native title bar */
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);

    g_window = glfwCreateWindow(g_config.window_width, g_config.window_height,
                                "Mumble Speaking Overlay", NULL, NULL);
    if (g_window == NULL) {
        glfwTerminate();
        return OW_ERR_GLFW;
    }

    glfwSetWindowPos(g_window, g_config.window_x, g_config.window_y);
    glfwSetWindowCloseCallback(g_window, on_window_close);

#ifdef _WIN32
    /* Remove taskbar entry by switching to WS_EX_TOOLWINDOW */
    {
        HWND hwnd = glfwGetWin32Window(g_window);
        if (hwnd != NULL) {
            LONG_PTR exstyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            exstyle |= WS_EX_TOOLWINDOW;
            exstyle &= ~WS_EX_APPWINDOW;
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, exstyle);
            SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
    }
#endif

    glfwMakeContextCurrent(g_window);
    glfwSwapInterval(1);

    /* --- Dear ImGui --- */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;

    /* Enable viewports so the settings panel can be a true separate OS window,
     * not constrained by the main overlay's GLFW window boundaries. */
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    /* Load CJK font for Chinese / Japanese characters */
    load_cjk_font();

    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.Alpha = g_config.text_alpha;
    style.Colors[ImGuiCol_WindowBg].w = g_config.alpha;

    /* Apply window scale to font */
    io.FontGlobalScale = g_config.window_scale;

    /* --- Backend init --- */
    if (!ImGui_ImplGlfw_InitForOpenGL(g_window, true)) {
        ImGui::DestroyContext();
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return OW_ERR_IMGUI;
    }

#ifdef __APPLE__
    const char *glsl_version = "#version 150";
#else
    const char *glsl_version = "#version 130";
#endif

    if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return OW_ERR_IMGUI;
    }

    g_first_frame = true;
    return OW_OK;
}

/* ========================================================================
 * Window close callback – hide instead of destroying
 * ======================================================================== */
static void on_window_close(GLFWwindow *win) {
    (void)win;
    g_window_hidden = true;
    glfwHideWindow(g_window);
    glfwSetWindowShouldClose(g_window, GLFW_FALSE);
}

/* ========================================================================
 * Apply current config to the GLFW window
 * ======================================================================== */
static void apply_config_to_window(void) {
    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = g_config.text_alpha;
    style.Colors[ImGuiCol_WindowBg].w = g_config.alpha;

    glfwSetWindowAttrib(g_window, GLFW_FLOATING,
                        g_config.always_on_top ? GLFW_TRUE : GLFW_FALSE);
    /* Note: mouse_passthrough is implemented at the ImGui level (NoInputs flag)
     *       instead of GLFW_MOUSE_PASSTHROUGH so the settings window remains usable. */
}

/* ========================================================================
 * Render one frame
 * ======================================================================== */
bool overlay_window_frame(overlay_poll_speakers_fn poll, void *userdata) {
    /* ---- Window hidden by user?  Sleep to save CPU, don't exit ---- */
    if (g_window_hidden) {
        glfwWaitEventsTimeout(0.25);
        return true;
    }

    if (glfwWindowShouldClose(g_window)) {
        return false;
    }

    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    /* ---- Keyboard shortcuts (checked every frame via glfwGetKey + ImGui IO) ---- */
    {
        ImGuiIO& io = ImGui::GetIO();
        /* Ctrl+Shift+P -> disable mouse passthrough */
        if (glfwGetKey(g_window, GLFW_KEY_P) == GLFW_PRESS && io.KeyCtrl && io.KeyShift) {
            if (g_config.mouse_passthrough) {
                g_config.mouse_passthrough = false;
                apply_config_to_window();
            }
        }
        /* Ctrl+Shift+H → show hidden window */
        if (glfwGetKey(g_window, GLFW_KEY_H) == GLFW_PRESS && io.KeyCtrl && io.KeyShift) {
            if (g_window_hidden) {
                g_window_hidden = false;
                g_user_saw_speaking_after_hide = false;
                glfwShowWindow(g_window);
            }
        }
    }
    /* ---- Handle request flags (set from Mumble main thread) ---- */
    if (g_request_show && g_window_hidden) {
        g_request_show = false;
        g_window_hidden = false;
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
            glfwShowWindow(g_window);
        }
    }

    /* ================================================================
     * Main overlay panel — auto-sized to fit content
     * ================================================================ */
    ImGuiWindowFlags main_flags = ImGuiWindowFlags_NoTitleBar
                                | ImGuiWindowFlags_NoResize
                                | ImGuiWindowFlags_NoCollapse
                                | ImGuiWindowFlags_NoBringToFrontOnFocus
                                | ImGuiWindowFlags_NoSavedSettings
                                | ImGuiWindowFlags_AlwaysAutoResize;

    /* When mouse passthrough is active, make main panel non-interactive
     * so clicks fall through to windows behind. The settings window is
     * separate and remains interactive. */
    if (g_config.mouse_passthrough) {
        main_flags |= ImGuiWindowFlags_NoInputs;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
    ImGui::Begin("SpeakingOverlayMain", NULL, main_flags);
    ImGui::PopStyleVar();

    /* ================================================================
     * Custom title bar — full-width draggable area (hidden in passthrough)
     * ================================================================ */
    if (!g_config.mouse_passthrough) {
        /* Full-width drag handle + buttons (hidden when passthrough is active) */
        float title_h = ImGui::GetFrameHeight() + 4.0f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);

        /* Draw the title text */
        ImVec2 title_pos = ImGui::GetCursorScreenPos();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), LOC("说话列表", "Speaking Users"));

        /* Place buttons at the right side */
        float avail_w = ImGui::GetContentRegionAvail().x;
        float btn_w = ImGui::CalcTextSize(LOC("设置", "Settings")).x + 12.0f;
        float close_w = ImGui::CalcTextSize("X").x + 12.0f;
        float btn_area = btn_w + 4.0f + close_w;

        ImGui::SameLine(0.0f, -1.0f);
        float cx = ImGui::GetCursorPosX();
        ImVec2 av = ImGui::GetContentRegionAvail();
        float btn_x = cx + av.x - btn_area;
        if (btn_x < cx) btn_x = cx;
        ImGui::SetCursorPosX(btn_x);

        if (ImGui::SmallButton(LOC("设置", "Settings"))) {
            g_settings_open = !g_settings_open;
        }
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetCursorPosX(btn_x + btn_w + 4.0f);
        if (ImGui::SmallButton("X")) {
            g_window_hidden = true;
            glfwHideWindow(g_window);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                LOC("隐藏窗口（插件继续运行）", "Hide window (plugin keeps running)"));
        }

        /* ---- Full-width drag handling ---- */
        /* Track drag relative to initial mouse position for smooth movement */
        {
            ImVec2 drag_min = ImVec2(title_pos.x, title_pos.y - 2.0f);
            ImGui::SetCursorScreenPos(drag_min);
            ImGui::InvisibleButton("##title_drag", ImVec2(avail_w, title_h));
            bool hovered = ImGui::IsItemHovered();
            bool active  = ImGui::IsItemActive();

            if (hovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            if (ImGui::IsItemActivated()) {
                g_drag_active = true;
                glfwGetWindowPos(g_window, &g_drag_win_x, &g_drag_win_y);
                ImVec2 mouse = ImGui::GetIO().MousePos;
                g_drag_mouse_x = mouse.x;
                g_drag_mouse_y = mouse.y;
            }

            if (g_drag_active && active) {
                ImVec2 mouse = ImGui::GetIO().MousePos;
                int dx = (int)(mouse.x - g_drag_mouse_x);
                int dy = (int)(mouse.y - g_drag_mouse_y);
                int new_x = g_drag_win_x + dx;
                int new_y = g_drag_win_y + dy;

                /* Clamp to screen edges */
                int fb_w, fb_h;
                glfwGetWindowSize(g_window, &fb_w, &fb_h);
                int mon_count;
                GLFWmonitor **mons = glfwGetMonitors(&mon_count);
                GLFWmonitor *monitor = NULL;
                int best_area = -1;
                for (int mi = 0; mi < mon_count; mi++) {
                    int mx, my, mw, mh;
                    glfwGetMonitorWorkarea(mons[mi], &mx, &my, &mw, &mh);
                    int ix = (new_x > mx + mw) ? 0 : ((new_x + fb_w < mx) ? 0 :
                             (new_x < mx ? mx : new_x));
                    int iy = (new_y > my + mh) ? 0 : ((new_y + fb_h < my) ? 0 :
                             (new_y < my ? my : new_y));
                    int iw = (new_x + fb_w > mx + mw ? mx + mw : new_x + fb_w) - ix;
                    int ih = (new_y + fb_h > my + mh ? my + mh : new_y + fb_h) - iy;
                    int area = iw * ih;
                    if (area > best_area) {
                        best_area = area;
                        monitor = mons[mi];
                    }
                }
                if (monitor) {
                    int mx, my, mw, mh;
                    glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
                    int min_visible = fb_w / 5;
                    if (new_x + min_visible < mx) new_x = mx - min_visible;
                    if (new_x + fb_w - min_visible > mx + mw) new_x = mx + mw - fb_w + min_visible;
                    if (new_y < my) new_y = my;
                    if (new_y + 20 > my + mh) new_y = my + mh - 20;
                }
                g_config.window_x = new_x;
                g_config.window_y = new_y;
                glfwSetWindowPos(g_window, new_x, new_y);
            }

            if (!active) {
                g_drag_active = false;
            }
        }
    }

    if (!g_config.mouse_passthrough) {
        ImGui::Separator();
    }

    /* ---- Speaking users list ---- */
    uint32_t user_ids[64];
    char     names[64][128];
    int      states[64];
    int user_count = poll ? poll(userdata, user_ids, names, states, 64) : 0;

    /* ---- Track speaking recency — update timestamp order ---- */
    {
        double now = ImGui::GetTime();
        for (int i = 0; i < user_count; i++) {
            /* Find this user in existing timestamps; if not found, append */
            int found = -1;
            for (int j = 0; j < g_user_timestamp_count; j++) {
                if (g_user_timestamps[j] == user_ids[i]) { found = j; break; }
            }
            if (found < 0) {
                /* New speaker — add at end, will bubble up */
                if (g_user_timestamp_count < 64) {
                    g_user_timestamps[g_user_timestamp_count++] = user_ids[i];
                    found = g_user_timestamp_count - 1;
                }
            }
            /* Move this user to the front (most recent) */
            if (found > 0) {
                uint32_t tmp = g_user_timestamps[found];
                memmove(&g_user_timestamps[1], &g_user_timestamps[0],
                        (size_t)found * sizeof(uint32_t));
                g_user_timestamps[0] = tmp;
            }
        }
    }

    /* ---- Build sorted display order: recently-active first ---- */
    int  display_idx[64];
    int  display_count = 0;
    {
        bool used[64] = {false};
        /* First, walk timestamp order to pick up active speakers */
        for (int t = 0; t < g_user_timestamp_count && display_count < user_count; t++) {
            for (int i = 0; i < user_count; i++) {
                if (!used[i] && user_ids[i] == g_user_timestamps[t]) {
                    used[i] = true;
                    display_idx[display_count++] = i;
                    break;
                }
            }
        }
        /* Any remaining active speakers not in timestamp list (shouldn't happen) */
        for (int i = 0; i < user_count && display_count < user_count; i++) {
            if (!used[i]) {
                used[i] = true;
                display_idx[display_count++] = i;
            }
        }
    }

    /* ---- Auto-show window when users start speaking ---- */
    if (g_window_hidden && user_count > 0) {
        if (!g_user_saw_speaking_after_hide) {
            g_user_saw_speaking_after_hide = true;
            g_window_hidden = false;
            glfwShowWindow(g_window);
        }
    } else if (user_count == 0) {
        g_user_saw_speaking_after_hide = false;
    }

    /* ---- Determine whether to snap scroll to top ---- */
    bool should_snap_to_top = false;
    /* Track mouse hover/activity */
    bool mouse_hovering = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
    if (mouse_hovering || ImGui::IsAnyItemActive() || ImGui::IsAnyItemFocused()) {
        g_last_mouse_activity_time = ImGui::GetTime();
    }
    /* Auto-snap: passthrough mode active, OR mouse left window >10s without manual scroll */
    double idle_seconds = ImGui::GetTime() - g_last_mouse_activity_time;
    if (g_config.mouse_passthrough || idle_seconds > 10.0) {
        if (!g_scrolled_by_user) {
            should_snap_to_top = true;
        }
    } else {
        /* User is active — don't snap; reset scroll-override flag */
        g_scrolled_by_user = false;
    }

    if (user_count == 0) {
        ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 1.0f),
                      LOC("  当前没人说话...", "  Nobody is speaking..."));
    } else {
        /* ---- Scrollable speaker list ---- */
        float avail_h = ImGui::GetContentRegionAvail().y;
        if (avail_h < 30.0f) avail_h = 30.0f;
        ImGui::BeginChild("SpeakerList", ImVec2(0, avail_h), false,
                          ImGuiWindowFlags_NoSavedSettings);

        /* Handle scroll snapping */
        if (should_snap_to_top) {
            ImGui::SetScrollHereY(0.0f);
        } else {
            /* Detect user-initiated scroll */
            float cur_scroll = ImGui::GetScrollY();
            if (cur_scroll != g_last_scroll_y && cur_scroll > 0.0f) {
                if (!g_scrolled_by_user) {
                    g_scrolled_by_user = true;
                }
                g_last_mouse_activity_time = ImGui::GetTime();
            }
            g_last_scroll_y = cur_scroll;
        }

        int max_vis = g_config.max_visible_speakers;
        int pinned_count = (display_count < max_vis) ? display_count : max_vis;

        for (int di = 0; di < display_count; di++) {
            int i = display_idx[di];

            /* Skip passive/muted users if show_idle_users is off */
            bool is_idle = (states[i] == 0 || states[i] == 4);
            if (is_idle && !g_config.show_idle_users) {
                continue;
            }

            ImVec4 col;
            const char *status_text;
            switch (states[i]) {
                case 1: /* SU_TALKING */
                    col = ImVec4(0.2f, 1.0f, 0.3f, 1.0f);
                    status_text = LOC("说话", "Talking");
                    break;
                case 2: /* SU_WHISPERING */
                    col = ImVec4(1.0f, 0.9f, 0.2f, 1.0f);
                    status_text = LOC("密语", "Whisper");
                    break;
                case 3: /* SU_SHOUTING */
                    col = ImVec4(1.0f, 0.25f, 0.25f, 1.0f);
                    status_text = LOC("喊话", "Shout");
                    break;
                default:
                    col = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                    status_text = LOC("空闲", "Idle");
                    break;
            }

            /* Recent speakers section header (first pinned_count) */
            if (di == pinned_count) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f),
                    "%s", LOC("---------- 更多 ----------", "--- more ---"));
            }

            /* Dimmed style for overflow speakers */
            bool is_pinned = (di < pinned_count);
            ImVec4 name_col = col;
            if (!is_pinned) {
                name_col.w *= 0.7f;
            }

            /* Apply idle user dimming */
            if (is_idle) {
                name_col.w *= g_config.idle_user_alpha;
            }

            /* User row: colored bullet + name, status aligned right */
            ImGui::TextColored(name_col, "  \xe2\x97\x8f  %s", names[i]);

            ImGui::SameLine(0.0f, -1.0f);
            float px = ImGui::GetCursorPosX();
            ImVec2 av = ImGui::GetContentRegionAvail();
            ImGui::SetCursorPosX(px + av.x - 60.0f);
            ImVec4 st_col = ImVec4(0.5f, 0.5f, 0.5f, is_pinned ? 1.0f : 0.5f);
            if (is_idle) {
                st_col.w *= g_config.idle_user_alpha;
            }
            ImGui::TextColored(st_col, "%s", status_text);
        }

        ImGui::EndChild();
    }

    /* ---- Auto-size the GLFW window to fit content (every frame, skip during drag) ---- */
    if (!g_drag_active) {
        float content_h = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y * 2.0f
                          + ImGui::GetStyle().FramePadding.y * 2.0f;
        float scale = g_config.window_scale;
        if (scale < 0.01f) scale = 1.0f;
        int target_h = (int)(content_h / scale + 0.5f);
        if (target_h < 40) target_h = 40;
        int cur_w, cur_h;
        glfwGetWindowSize(g_window, &cur_w, &cur_h);
        /* Let ImGui's AlwaysAutoResize handle width naturally */
        glfwSetWindowSize(g_window, cur_w, target_h);
        g_config.window_width = cur_w;
        g_config.window_height = target_h;
        g_first_frame = false;
    }

    ImGui::End();

    /* ================================================================
     * Settings panel — rendered AFTER main, so it floats on top
     * (separate ImGui window, not affected by NoInputs)
     * ================================================================ */
    if (g_settings_open) {
        ImGui::Begin(LOC("设置", "Settings"), &g_settings_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
            /* ---- Window + Text transparency ---- */
            float a = g_config.alpha;
            ImGui::SliderFloat(LOC("窗口透明度", "Window opacity"), &a,
                          0.0f, 1.0f, "%.2f", ImGuiSliderFlags_None);
            float ta = g_config.text_alpha;
            ImGui::SliderFloat(LOC("文字透明度", "Text opacity"), &ta,
                          0.0f, 1.0f, "%.2f", ImGuiSliderFlags_None);

            /* Safety: cap both if dangerous not allowed */
            bool any_low = (a < 0.2f || ta < 0.2f);
            if (any_low && !g_config.dangerous_alpha_allowed) {
                if (a < 0.2f) a = 0.2f;
                if (ta < 0.2f) ta = 0.2f;
            }
            g_config.alpha = a;
            g_config.text_alpha = ta;

            ImGui::Checkbox(LOC("允许危险透明度", "Allow risky opacity"),
                           &g_config.dangerous_alpha_allowed);
            if (any_low && !g_config.dangerous_alpha_allowed) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f),
                    LOC("窗口和文字透明度不低于 0.2。\n"
                        "勾选后可调低。",
                        "Window & text opacity capped at 0.2.\n"
                        "Check to allow lower values."));
            }

            ImGui::Separator();

            /* ---- Scale ---- */
            ImGui::SliderFloat(LOC("缩放", "Scale"), &g_config.window_scale,
                          1.0f, 4.0f, "%.1f", ImGuiSliderFlags_None);
            {
                ImGuiIO& io = ImGui::GetIO();
                io.FontGlobalScale = g_config.window_scale;
            }

            ImGui::Separator();

            ImGui::Checkbox(LOC("窗口置顶", "Always on top"), &g_config.always_on_top);

            /* ---- Mouse passthrough ---- */
            ImGui::Checkbox(LOC("鼠标穿透", "Mouse passthrough"),
                                                   &g_config.mouse_passthrough);
            if (g_config.mouse_passthrough) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f),
                    LOC("启用后将无法用鼠标点击窗口。\n"
                        "按 Ctrl+Shift+P 可关闭穿透。",
                        "Cannot click the window once enabled.\n"
                        "Press Ctrl+Shift+P to disable."));
            }

            ImGui::Separator();

            /* ---- Visible speakers count ---- */
            ImGui::SliderInt(LOC("可见发言人数", "Visible speakers"),
                           &g_config.max_visible_speakers, 1, 64,
                           "%d", ImGuiSliderFlags_None);

            ImGui::Separator();

            /* ---- Show idle users ---- */
            ImGui::Checkbox(LOC("显示未发言用户", "Show idle users"),
                           &g_config.show_idle_users);
            if (g_config.show_idle_users) {
                ImGui::SliderFloat(LOC("未发言用户透明度", "Idle user opacity"),
                               &g_config.idle_user_alpha, 0.0f, 1.0f, "%.2f",
                               ImGuiSliderFlags_None);
                if (g_config.idle_user_alpha < 0.2f && !g_config.dangerous_alpha_allowed) {
                    g_config.idle_user_alpha = 0.2f;
                }
            }

            /* ---- Idle timeout ---- */
            ImGui::SliderInt(LOC("空闲超时(秒)", "Idle timeout(s)"),
                           &g_config.idle_timeout_seconds, 1, 30,
                           "%d", ImGuiSliderFlags_None);

            ImGui::Separator();

            /* ---- Mumble logging ---- */
            ImGui::Checkbox(LOC("在 Mumble 输出日志", "Log to Mumble console"),
                           &g_config.mumble_logging_enabled);

            apply_config_to_window();

            ImGui::Separator();

            /* ---- Action buttons ---- */
            if (g_window_hidden) {
                if (ImGui::Button(LOC("显示窗口", "Show Window"), ImVec2(-1.0f, 0.0f))) {
                    g_window_hidden = false;
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
                if (g_window_hidden) {
                    g_window_hidden = false;
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
                g_config.show_idle_users   = def.show_idle_users;
                g_config.idle_user_alpha   = def.idle_user_alpha;
                g_config.idle_timeout_seconds = def.idle_timeout_seconds;
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
                if (g_window_hidden) {
                    g_window_hidden = false;
                    glfwShowWindow(g_window);
                }
            }

            ImGui::TextWrapped(
                LOC("\"重置所有设置\" | 恢复默认值\n"
                    "拖拽窗口顶部空白区域移动位置。",
                    "\"Reset All Settings\" | Restore defaults\n"
                    "Drag the top empty area of the window to move.") );
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
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    ImDrawData *draw_data = ImGui::GetDrawData();
    if (draw_data != NULL) {
        ImGui_ImplOpenGL3_RenderDrawData(draw_data);
    }

    /* Update and render multi-viewport (separate OS windows for settings, etc.) */
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow *backup_current = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current);
        }
    }

#ifdef _WIN32
    /* Ensure all ImGui viewport windows (e.g. settings panel) are tool windows
     * without taskbar entries. The main window gets this in overlay_window_init. */
    {
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        for (int i = 0; i < pio.Viewports.Size; i++) {
            ImGuiViewport* vp = pio.Viewports[i];
            if (vp->PlatformHandle != NULL && vp->PlatformHandle != glfwGetWin32Window(g_window)) {
                HWND hwnd = (HWND)vp->PlatformHandle;
                LONG_PTR exstyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
                if (!(exstyle & WS_EX_TOOLWINDOW)) {
                    exstyle |= WS_EX_TOOLWINDOW;
                    exstyle &= ~WS_EX_APPWINDOW;
                    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exstyle);
                    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
                }
            }
        }
    }
#endif

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
    if (g_window != NULL) {
        /* Save current config before destroying */
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

#ifdef __APPLE__
#pragma clang diagnostic pop
#endif
