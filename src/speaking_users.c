/*
 * speaking_users.c — Implementation of the thread-safe active speaker list.
 *
 * Uses a linked list protected by a mutex / SRWLOCK.
 * Mumble callbacks (main thread) write; render thread reads.
 */

#include "speaking_users.h"

#include <stdlib.h>
#include <string.h>

/* ---- Synchronisation ---- */
static SRWLOCK g_lock;
#define LOCK_INIT()   InitializeSRWLock(&g_lock)
#define LOCK_ACQ()    AcquireSRWLockExclusive(&g_lock)
#define LOCK_REL()    ReleaseSRWLockExclusive(&g_lock)

/* ---- Internal state ---- */
static speaking_user_t *g_head = NULL;

/* ---- Public API ---- */

void speaking_users_init(void) {
    InitializeSRWLock(&g_lock);
    g_head = NULL;
}

void speaking_users_destroy(void) {
    LOCK_ACQ();
    speaking_user_t *cur = g_head;
    while (cur != NULL) {
        speaking_user_t *next = cur->next;
        free(cur);
        cur = next;
    }
    g_head = NULL;
    LOCK_REL();
}

void speaking_users_upsert(uint32_t user_id, const char *name, su_talking_state_t state) {
    LOCK_ACQ();
    speaking_user_t *cur = g_head;
    while (cur != NULL) {
        if (cur->user_id == user_id) {
            cur->state = state;
            cur->last_update = time(NULL);
            if (name != NULL && name[0] != '\0' && cur->name[0] == '\0') {
                strncpy(cur->name, name, sizeof(cur->name) - 1);
                cur->name[sizeof(cur->name) - 1] = '\0';
            }
            LOCK_REL();
            return;
        }
        cur = cur->next;
    }

    /* New user — prepend */
    speaking_user_t *new_user = (speaking_user_t *)calloc(1, sizeof(speaking_user_t));
    if (new_user == NULL) {
        LOCK_REL();
        return;
    }
    new_user->user_id = user_id;
    new_user->state   = state;
    new_user->channel_id = -1;
    new_user->last_update = time(NULL);
    if (name != NULL) {
        strncpy(new_user->name, name, sizeof(new_user->name) - 1);
        new_user->name[sizeof(new_user->name) - 1] = '\0';
    } else {
        new_user->name[0] = '\0';
    }
    new_user->next = g_head;
    g_head = new_user;
    LOCK_REL();
}

void speaking_users_set_user_channel(uint32_t user_id, int32_t channel_id) {
    LOCK_ACQ();
    speaking_user_t *cur = g_head;
    while (cur != NULL) {
        if (cur->user_id == user_id) {
            cur->channel_id = channel_id;
            cur->last_update = time(NULL);
            LOCK_REL();
            return;
        }
        cur = cur->next;
    }

    /* User not yet tracked — create a placeholder with the channel set */
    speaking_user_t *new_user = (speaking_user_t *)calloc(1, sizeof(speaking_user_t));
    if (new_user == NULL) {
        LOCK_REL();
        return;
    }
    new_user->user_id    = user_id;
    new_user->state      = SU_PASSIVE;
    new_user->channel_id = channel_id;
    new_user->last_update = time(NULL);
    new_user->name[0]    = '\0';
    new_user->next = g_head;
    g_head = new_user;
    LOCK_REL();
}

void speaking_users_remove(uint32_t user_id) {
    LOCK_ACQ();
    speaking_user_t *prev = NULL;
    speaking_user_t *cur = g_head;
    while (cur != NULL) {
        if (cur->user_id == user_id) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                g_head = cur->next;
            }
            free(cur);
            LOCK_REL();
            return;
        }
        prev = cur;
        cur = cur->next;
    }
    LOCK_REL();
}

int speaking_users_get_active(speaking_user_t *out_array, int max_count, int timeout_seconds) {
    int count = 0;
    time_t now = time(NULL);

    LOCK_ACQ();
    speaking_user_t *prev = NULL;
    speaking_user_t *cur = g_head;

    while (cur != NULL) {
        speaking_user_t *next = cur->next;

        /* Prune expired entries */
        if ((cur->state == SU_PASSIVE || cur->state == SU_TALKING_MUTED)
            && (now - cur->last_update) > timeout_seconds) {
            if (prev != NULL) {
                prev->next = next;
            } else {
                g_head = next;
            }
            free(cur);
            cur = next;
            continue;
        }

        /* Collect active (non-passive) */
        if (cur->state != SU_PASSIVE && cur->state != SU_TALKING_MUTED && count < max_count) {
            out_array[count++] = *cur;  /* shallow copy */
        }
        prev = cur;
        cur = next;
    }
    LOCK_REL();

    return count;
}

void speaking_users_clear(void) {
    LOCK_ACQ();
    speaking_user_t *cur = g_head;
    while (cur != NULL) {
        speaking_user_t *next = cur->next;
        cur->state = SU_PASSIVE;
        cur->last_update = 0;  /* will be pruned soon */
        cur = next;
    }
    LOCK_REL();
}

int speaking_users_get_all(speaking_user_t *out_array, int max_count, int timeout_seconds) {
    int count = 0;
    time_t now = time(NULL);

    LOCK_ACQ();
    speaking_user_t *prev = NULL;
    speaking_user_t *cur = g_head;

    while (cur != NULL) {
        speaking_user_t *next = cur->next;

        /* Prune expired entries (same as get_active) */
        if ((cur->state == SU_PASSIVE || cur->state == SU_TALKING_MUTED)
            && (now - cur->last_update) > timeout_seconds) {
            if (prev != NULL) {
                prev->next = next;
            } else {
                g_head = next;
            }
            free(cur);
            cur = next;
            continue;
        }

        /* Collect ALL users (including passive/muted) */
        if (count < max_count) {
            out_array[count++] = *cur;
        }
        prev = cur;
        cur = next;
    }
    LOCK_REL();

    return count;
}
