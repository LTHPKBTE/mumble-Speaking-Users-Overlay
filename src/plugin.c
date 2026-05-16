/*
 * plugin.c — Mumble Speaking Users Overlay Plugin
 *
 * Implements the mandatory Mumble plugin API functions and
 * wires Mumble callbacks to the speaking-users list and
 * the GLFW + ImGui overlay render thread.
 *
 * Architecture:
 *   [Mumble main thread]                [Render thread]
 *      callbacks  ──► speaking_users   ◄── poll callback (reads list)
 *                         (mutex)
 *   MumbleAPI calls OK here             NO MumbleAPI calls!
 */

#include "MumblePlugin.h"
#include "speaking_users.h"
#include "overlay_window.h"
#include "render_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================
 * Global state
 * ======================================================================== */
static MumbleAPI           g_api;              /* Mumble API function table */
static mumble_plugin_id_t  g_plugin_id;        /* Our plugin ID */
static mumble_connection_t g_active_connection = -1; /* Current server connection */

/* ========================================================================
 * Forward declarations
 * ======================================================================== */
static const char *fetch_user_name(mumble_connection_t conn, mumble_userid_t user_id,
                                   char *buf, size_t buf_size);

/* ========================================================================
 * Poll callback — called from render thread to get active speakers
 *
 * This is the bridge: render thread calls this → it calls
 * speaking_users_get_active() (thread-safe) → returns data.
 *
 * IMPORTANT: This does NOT call any MumbleAPI function.
 *            User names are pre-fetched in Mumble callbacks.
 * ======================================================================== */
static int overlay_poll_speakers(void *userdata,
                                 uint32_t *user_ids,
                                 char (*names)[128],
                                 int *states,
                                 int max_count) {
    (void)userdata;

    speaking_user_t buffer[64];
    int count = speaking_users_get_all(buffer, 64, 5);  /* 5-second timeout */
    if (count > max_count) count = max_count;

    for (int i = 0; i < count; i++) {
        user_ids[i] = buffer[i].user_id;
        strncpy(names[i], buffer[i].name, 127);
        names[i][127] = '\0';
        states[i] = (int)buffer[i].state;
    }
    return count;
}

/* ========================================================================
 * MANDATORY PLUGIN FUNCTIONS
 * ======================================================================== */

mumble_error_t mumble_init(mumble_plugin_id_t id) {
    g_plugin_id = id;

    g_api.log(g_plugin_id, "Plugin initialized");

    speaking_users_init();
    g_active_connection = -1;

    return MUMBLE_STATUS_OK;
}

void mumble_shutdown(void) {
    g_api.log(g_plugin_id, "Plugin shutting down");

    /* Stop render thread first */
    if (render_thread_is_running()) {
        render_thread_stop();
    }

    speaking_users_destroy();

    g_api.log(g_plugin_id, "Shutdown complete");
}

struct MumbleStringWrapper mumble_getName(void) {
    static const char *name = "SpeakingUsersOverlay";
    struct MumbleStringWrapper wrapper;
    wrapper.data           = name;
    wrapper.size           = strlen(name);
    wrapper.needsReleasing = false;
    return wrapper;
}

mumble_version_t mumble_getAPIVersion(void) {
    return MUMBLE_PLUGIN_API_VERSION;
}

void mumble_registerAPIFunctions(void *apiStruct) {
    g_api = MUMBLE_API_CAST(apiStruct);
}

void mumble_releaseResource(const void *pointer) {
    (void)pointer;
    /* This plugin never sets needsReleasing = true */
}

/* ========================================================================
 * OPTIONAL (but recommended) PLUGIN FUNCTIONS
 * ======================================================================== */

mumble_version_t mumble_getVersion(void) {
    mumble_version_t v;
    v.major = 1;
    v.minor = 0;
    v.patch = 0;
    return v;
}

struct MumbleStringWrapper mumble_getAuthor(void) {
    static const char *author = "Mumble Community";
    struct MumbleStringWrapper wrapper;
    wrapper.data           = author;
    wrapper.size           = strlen(author);
    wrapper.needsReleasing = false;
    return wrapper;
}

struct MumbleStringWrapper mumble_getDescription(void) {
    static const char *desc =
        "Displays a list of currently speaking users in a floating overlay window. "
        "Supports transparency, mouse passthrough, and always-on-top.";
    struct MumbleStringWrapper wrapper;
    wrapper.data           = desc;
    wrapper.size           = strlen(desc);
    wrapper.needsReleasing = false;
    return wrapper;
}

uint32_t mumble_getFeatures(void) {
    return MUMBLE_FEATURE_NONE;
}

uint32_t mumble_deactivateFeatures(uint32_t features) {
    return features;  /* nothing to deactivate */
}

/* ========================================================================
 * EVENT CALLBACKS
 * ======================================================================== */

void mumble_onServerConnected(mumble_connection_t connection) {
    g_active_connection = connection;

    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf),
             "Connected to server (conn=%d)", (int)connection);
    g_api.log(g_plugin_id, log_buf);
}

void mumble_onServerDisconnected(mumble_connection_t connection) {
    if (render_thread_is_running()) {
        render_thread_stop();
    }
    speaking_users_clear();

    g_active_connection = -1;

    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf),
             "Disconnected from server (conn=%d)", (int)connection);
    g_api.log(g_plugin_id, log_buf);
}

void mumble_onServerSynchronized(mumble_connection_t connection) {
    g_active_connection = connection;

    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf),
             "Server synchronized (conn=%d)", (int)connection);
    g_api.log(g_plugin_id, log_buf);

    /* Start render thread when we first connect */
    if (!render_thread_is_running()) {
        overlay_config_t cfg = overlay_config_default();
        int rc = render_thread_start(&cfg, overlay_poll_speakers, NULL);
        if (rc != 0) {
            g_api.log(g_plugin_id, "Failed to start render thread");
        } else {
            g_api.log(g_plugin_id, "Overlay window started");
        }
    }
}

void mumble_onUserTalkingStateChanged(mumble_connection_t connection,
                                       mumble_userid_t userID,
                                       mumble_talking_state_t talkingState) {
    char name_buf[128];
    const char *name = fetch_user_name(connection, userID, name_buf, sizeof(name_buf));

    su_talking_state_t st;
    switch (talkingState) {
        case MUMBLE_TS_TALKING:      st = SU_TALKING;      break;
        case MUMBLE_TS_WHISPERING:   st = SU_WHISPERING;   break;
        case MUMBLE_TS_SHOUTING:     st = SU_SHOUTING;     break;
        case MUMBLE_TS_TALKING_MUTED:st = SU_TALKING_MUTED;break;
        default:                     st = SU_PASSIVE;      break;
    }

    speaking_users_upsert(userID, name, st);
}

void mumble_onUserAdded(mumble_connection_t connection, mumble_userid_t userID) {
    /* Pre-fetch the user name so it's ready when they start speaking. */
    char name_buf[128];
    const char *name = fetch_user_name(connection, userID, name_buf, sizeof(name_buf));
    if (name != NULL && name[0] != '\0') {
        speaking_users_upsert(userID, name, SU_PASSIVE);
    }
}

/* ========================================================================
 * Helper: fetch user name from Mumble API (ONLY call from main thread!)
 * ======================================================================== */
static const char *fetch_user_name(mumble_connection_t conn, mumble_userid_t user_id,
                                   char *buf, size_t buf_size) {
    const char *api_name = NULL;
    mumble_error_t err = g_api.getUserName(g_plugin_id, conn, user_id, &api_name);
    if (err == MUMBLE_STATUS_OK && api_name != NULL) {
        strncpy(buf, api_name, buf_size - 1);
        buf[buf_size - 1] = '\0';
        g_api.freeMemory(g_plugin_id, api_name);
        return buf;
    }
    return NULL;
}
