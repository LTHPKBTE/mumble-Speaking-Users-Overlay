/*
 * speaking_users.h — Thread-safe active speaker list
 *
 * Maintains a list of users currently speaking (talking/shouting/whispering).
 * Thread-safe: callbacks from Mumble main thread write, render thread reads.
 */

#ifndef SPEAKING_USERS_H
#define SPEAKING_USERS_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* Talking state mirrors Mumble_TalkingState from the API */
typedef enum {
    SU_PASSIVE          = 0,
    SU_TALKING          = 1,
    SU_WHISPERING       = 2,
    SU_SHOUTING         = 3,
    SU_TALKING_MUTED    = 4
} su_talking_state_t;

/* A single user entry in our tracking list */
typedef struct speaking_user {
    uint32_t   user_id;
    char       name[128];        /* cached user name (set once from main thread) */
    su_talking_state_t state;
    time_t     last_update;
    struct speaking_user *next;
} speaking_user_t;

/* Public API */

/* Initialise the list. Must be called before any other function. */
void speaking_users_init(void);

/* Destroy the list and free all memory. */
void speaking_users_destroy(void);

/*
 * Update (upsert) a user's talking state.
 * Callable from Mumble callbacks (main thread).
 * If `name` is non-NULL and user not yet named, the name is stored.
 * If user goes PASSIVE they remain in list for a timeout period.
 */
void speaking_users_upsert(uint32_t user_id, const char *name, su_talking_state_t state);

/*
 * Get a snapshot of currently active speakers (non-passive, non-muted).
 * Returns number of speakers written to `out_array`, up to `max_count`.
 * Callable from render thread.
 * Users inactive longer than `timeout_seconds` are pruned.
 */
int speaking_users_get_active(speaking_user_t *out_array, int max_count, int timeout_seconds);

/*
 * Get ALL known users (including passive/muted).
 * Returns number of users written to `out_array`, up to `max_count`.
 * Callable from render thread.
 * Users inactive longer than `timeout_seconds` are pruned.
 */
int speaking_users_get_all(speaking_user_t *out_array, int max_count, int timeout_seconds);

/*
 * Mark all users as passive (e.g. on disconnect).
 */
void speaking_users_clear(void);

#endif /* SPEAKING_USERS_H */
