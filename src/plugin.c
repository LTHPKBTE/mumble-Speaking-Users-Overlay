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

/* Log helper — respects config once overlay is initialized */
#define PLUGIN_LOG(msg) do { \
    if (g_mumble_logging_enabled) { \
        g_api.log(g_plugin_id, msg); \
    } \
} while(0)

/* Log with format string */
#define PLUGIN_LOGF(fmt, ...) do { \
    if (g_mumble_logging_enabled) { \
        char _buf[256]; \
        snprintf(_buf, sizeof(_buf), fmt, __VA_ARGS__); \
        g_api.log(g_plugin_id, _buf); \
    } \
} while(0)

/* Error log — ALWAYS outputs to Mumble regardless of logging config */
#define PLUGIN_LOG_ERROR(msg) do { \
    g_api.log(g_plugin_id, msg); \
} while(0)

#define PLUGIN_LOG_ERRORF(fmt, ...) do { \
    char _buf[256]; \
    snprintf(_buf, sizeof(_buf), fmt, __VA_ARGS__); \
    g_api.log(g_plugin_id, _buf); \
} while(0)

/* ========================================================================
 * Global state
 * ======================================================================== */
static MumbleAPI           g_api;              /* Mumble API function table */
static mumble_plugin_id_t  g_plugin_id;        /* Our plugin ID */
static mumble_connection_t g_active_connection = -1; /* Current server connection */
static mumble_channelid_t  g_local_channel_id  = -1; /* Our current channel (-1 = unknown) */

/* Default logging to true before overlay_window_init loads config */
static bool g_mumble_logging_enabled = true;

/* ========================================================================
 * Forward declarations
 * ======================================================================== */
static const char *fetch_user_name(mumble_connection_t conn, mumble_userid_t user_id,
                                   char *buf, size_t buf_size);
static void overlay_log_to_mumble(const char *msg);
static void sync_user_channel(mumble_connection_t conn, mumble_userid_t user_id);
static void sync_local_channel(mumble_connection_t conn);
static void sync_all_user_channels(mumble_connection_t conn, mumble_userid_t *users, size_t count);

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
    /* Use const pointer to avoid copying the entire config struct every frame */
    const overlay_config_t *cfg = overlay_window_get_config_ptr();
    int count;

    if (cfg->show_all_users) {
        /* Always show all known users — never prune.
         * Use a very large timeout so passive entries aren't freed. */
        int huge_timeout = 86400 * 365; /* 1 year */
        count = speaking_users_get_all(buffer, 64, huge_timeout);
    } else if (cfg->show_recent_speakers) {
        /* Show recently speaking users with configured timeout. */
        int timeout = cfg->idle_timeout_seconds;
        if (timeout < 1) timeout = 30;
        count = speaking_users_get_all(buffer, 64, timeout);
    } else {
        /* Only show actively speaking users. Use a reasonable
         * cleanup timeout to purge stale passive entries. */
        count = speaking_users_get_active(buffer, 64, 60);
    }

    if (count > max_count) count = max_count;

    /* Channel filter: when show_current_channel_only is on, skip users not in our channel.
     * g_local_channel_id is only written from the main thread; reading from render
     * thread is safe (int32_t is atomic on all supported platforms). */
    if (cfg->show_current_channel_only && g_local_channel_id >= 0) {
        int filtered = 0;
        for (int i = 0; i < count; i++) {
            if (buffer[i].channel_id == g_local_channel_id) {
                if (filtered != i) {
                    buffer[filtered] = buffer[i];
                }
                filtered++;
            }
        }
        count = filtered;
    }

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

    PLUGIN_LOG_ERROR("SpeakingUsersOverlay: Plugin initialized");

    /* Set up log forwarding from overlay window to Mumble */
    overlay_window_set_log_callback(overlay_log_to_mumble);

    speaking_users_init();
    g_active_connection = -1;
    g_local_channel_id  = -1;
    g_mumble_logging_enabled = true;

    /* Check if we are already connected to a server (plugin loaded after connection).
     * This allows the overlay to appear immediately without waiting for a new connection. */
    mumble_connection_t existing_conn;
    mumble_error_t err = g_api.getActiveServerConnection(g_plugin_id, &existing_conn);
    if (err == MUMBLE_STATUS_OK) {
        bool synced = false;
        err = g_api.isConnectionSynchronized(g_plugin_id, existing_conn, &synced);
        if (err == MUMBLE_STATUS_OK && synced) {
            g_active_connection = existing_conn;

            PLUGIN_LOGF("Already connected to server (conn=%d) – starting overlay",
                     (int)existing_conn);

            /* Enumerate all users already on the server */
            {
                char name_buf[128];
                mumble_userid_t *users = NULL;
                size_t user_count = 0;
                mumble_error_t uerr = g_api.getAllUsers(g_plugin_id, existing_conn, &users, &user_count);
                if (uerr == MUMBLE_STATUS_OK && users != NULL) {
                    for (size_t i = 0; i < user_count; i++) {
                        const char *name = fetch_user_name(existing_conn, users[i], name_buf, sizeof(name_buf));
                        speaking_users_upsert(users[i], name, SU_PASSIVE);
                    }
                    sync_all_user_channels(existing_conn, users, user_count);
                    g_api.freeMemory(g_plugin_id, users);
                }
                sync_local_channel(existing_conn);
            }

            if (!render_thread_is_running()) {
                /* Pass NULL — overlay will load saved config from disk */
                int rc = render_thread_start(NULL, overlay_poll_speakers, NULL);
                if (rc != 0) {
                    PLUGIN_LOG_ERRORF("Failed to start render thread for existing connection (rc=%d)", rc);
                } else {
                    /* Sync logging flag from overlay config (now loaded from disk) */
                    overlay_config_t loaded_cfg;
                    overlay_window_get_config(&loaded_cfg);
                    g_mumble_logging_enabled = loaded_cfg.mumble_logging_enabled;
                    PLUGIN_LOG("Overlay window started for existing connection");
                }
            }
        }
    } else {
        /* No active connection – normal cold-start, render thread will be
         * started in mumble_onServerSynchronized. */
        PLUGIN_LOG("No active connection on init – waiting for server connect");
    }

    return MUMBLE_STATUS_OK;
}

void mumble_shutdown(void) {
    PLUGIN_LOG_ERROR("SpeakingUsersOverlay: Plugin shutting down");

    /* Stop render thread first (this triggers overlay_window_shutdown which saves config) */
    if (render_thread_is_running()) {
        render_thread_stop();
    }

    speaking_users_destroy();

    PLUGIN_LOG_ERROR("SpeakingUsersOverlay: Shutdown complete");
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

    PLUGIN_LOGF("Connected to server (conn=%d)", (int)connection);
}

void mumble_onServerDisconnected(mumble_connection_t connection) {
    PLUGIN_LOGF("Disconnected from server (conn=%d)", (int)connection);

    /* Stop render thread (this triggers overlay_window_shutdown which saves config) */
    if (render_thread_is_running()) {
        render_thread_stop();
    }
    speaking_users_clear();

    g_active_connection = -1;
    g_local_channel_id  = -1;
}

void mumble_onServerSynchronized(mumble_connection_t connection) {
    g_active_connection = connection;

    PLUGIN_LOGF("Server synchronized (conn=%d)", (int)connection);

    /* Enumerate all users already on the server and add them as passive */
    {
        char name_buf[128];
        mumble_userid_t *users = NULL;
        size_t user_count = 0;
        mumble_error_t err = g_api.getAllUsers(g_plugin_id, connection, &users, &user_count);
        if (err == MUMBLE_STATUS_OK && users != NULL) {
            for (size_t i = 0; i < user_count; i++) {
                const char *name = fetch_user_name(connection, users[i], name_buf, sizeof(name_buf));
                speaking_users_upsert(users[i], name, SU_PASSIVE);
            }
            sync_all_user_channels(connection, users, user_count);
            g_api.freeMemory(g_plugin_id, users);
        }
        sync_local_channel(connection);
    }

    /* Start render thread when we first connect */
    if (!render_thread_is_running()) {
        /* Pass NULL — overlay will load saved config from disk */
        int rc = render_thread_start(NULL, overlay_poll_speakers, NULL);
        if (rc != 0) {
            PLUGIN_LOG_ERRORF("Failed to start render thread on server sync (rc=%d)", rc);
        } else {
            /* Sync logging flag from overlay config (now loaded from disk) */
            overlay_config_t loaded_cfg;
            overlay_window_get_config(&loaded_cfg);
            g_mumble_logging_enabled = loaded_cfg.mumble_logging_enabled;
            PLUGIN_LOG("Overlay window started");
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
    /* Also track which channel they're in */
    sync_user_channel(connection, userID);
}

void mumble_onChannelEntered(mumble_connection_t connection,
                              mumble_userid_t userID,
                              mumble_channelid_t previousChannelID,
                              mumble_channelid_t newChannelID) {
    (void)previousChannelID;

    /* Update the user's channel in our tracking list */
    speaking_users_set_user_channel((uint32_t)userID, (int32_t)newChannelID);

    /* Check if this is our own user – if so, update local channel */
    mumble_userid_t local_id;
    if (g_api.getLocalUserID(g_plugin_id, connection, &local_id) == MUMBLE_STATUS_OK
        && (uint32_t)local_id == (uint32_t)userID) {
        g_local_channel_id = (int32_t)newChannelID;
        PLUGIN_LOGF("Local user moved to channel %d", (int)newChannelID);
    }
}

void mumble_onChannelExited(mumble_connection_t connection,
                             mumble_userid_t userID,
                             mumble_channelid_t channelID) {
    (void)connection;
    /* A negative channelID means the user fully disconnected from the server.
     * Remove them so they don't linger as a "dead" duplicate when they reconnect
     * with a new user_id. */
    if (channelID < 0) {
        speaking_users_remove((uint32_t)userID);
        PLUGIN_LOGF("User %d left the server (removed from list)", (int)userID);
    }
}

void mumble_onUserRemoved(mumble_connection_t connection,
                           mumble_userid_t userID) {
    (void)connection;
    /* User disconnected from the server or was removed from the user model.
     * Delete them from our tracking list to prevent stale duplicates. */
    speaking_users_remove((uint32_t)userID);
    PLUGIN_LOGF("User %d removed from server model (removed from list)", (int)userID);
}

/* ========================================================================
 * Helpers: sync channel info into speaking_users list
 * (ONLY call from main thread!)
 * ======================================================================== */
static void sync_user_channel(mumble_connection_t conn, mumble_userid_t user_id) {
    mumble_channelid_t channel;
    mumble_error_t err = g_api.getChannelOfUser(g_plugin_id, conn, user_id, &channel);
    if (err == MUMBLE_STATUS_OK) {
        speaking_users_set_user_channel((uint32_t)user_id, (int32_t)channel);
    }
}

static void sync_local_channel(mumble_connection_t conn) {
    mumble_userid_t local_id;
    mumble_error_t err = g_api.getLocalUserID(g_plugin_id, conn, &local_id);
    if (err == MUMBLE_STATUS_OK) {
        mumble_channelid_t channel;
        err = g_api.getChannelOfUser(g_plugin_id, conn, local_id, &channel);
        if (err == MUMBLE_STATUS_OK) {
            g_local_channel_id = (int32_t)channel;
            PLUGIN_LOGF("Local user is in channel %d", (int)channel);
        }
    }
}

static void sync_all_user_channels(mumble_connection_t conn, mumble_userid_t *users, size_t count) {
    for (size_t i = 0; i < count; i++) {
        sync_user_channel(conn, users[i]);
    }
}

/* ========================================================================
 * Log bridge: forwards overlay window errors to Mumble's log
 *
 * Called from render thread via overlay_window log callback.
 * Safe because g_api.log is expected to be thread-safe by Mumble.
 * ======================================================================== */
static void overlay_log_to_mumble(const char *msg) {
    if (msg != NULL) {
        g_api.log(g_plugin_id, msg);
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
