// --- Background network worker -------------------------------------------
// ha_fetch_states()/ha_toggle_entity() are blocking HTTPS calls that can
// take several seconds over the internet; running them on the main thread
// froze rendering for the whole call. All network work happens on a
// dedicated worker thread; `entities`/`entity_count`/`status_msg`/
// `selected_index` are only ever touched by the main thread, and the
// pending_* staging fields (written by the worker, read by the main thread)
// are guarded by state_lock.
#include "ha_worker.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <3ds.h>

#include "app_state.h"
#include "dbglog.h"
#include "entity_list.h"
#include "ha_client.h"

// Was 32KB, which was already too small for its own good: the OP_REFRESH
// path stack-allocates ha_entity_t local_entities[MAX_ENTITIES] (23.8KB)
// AND ha_area_entry_t areas[MAX_ENTITIES] (14.3KB) simultaneously - 37.2KB
// on their own, before curl/mbedTLS/jansson's own stack usage for the
// nested HTTPS calls. That overflowed straight into the guard page (a
// real-hardware data abort; the emulator run that "passed" never reached
// this path since it had no saved session to trigger a refresh with).
#define WORKER_STACK_SIZE (128 * 1024)

static LightLock state_lock;
static Thread worker_thread = NULL;
volatile bool network_busy = false;
static volatile bool worker_result_ready = false;

static ha_entity_t pending_entities[MAX_ENTITIES];
static int pending_count = 0;
static char pending_status[128];
static char pending_entity_id[HA_MAX_ENTITY_ID];
static int pending_op = OP_REFRESH;
static int pending_brightness_pct = 0;
static float pending_target_temp = 0.0f;
// r/g/b (0-255 each) for OP_SET_COLOR, kelvin for OP_SET_COLOR_TEMP - see
// start_worker(), which packs r/g/b into its int_param to avoid growing its
// signature further.
static int pending_color_r = 0, pending_color_g = 0, pending_color_b = 0;
static int pending_color_kelvin = 0;
// entity_id's state/is_group as of the moment OP_TOGGLE was kicked off, so
// ha_toggle_entity() can decide on/off itself instead of asking HA to -
// see its doc comment in ha_client.h.
static char pending_toggle_state[HA_MAX_STATE];
static int pending_toggle_is_group = 0;

// After a toggle/brightness change, only that one entity actually changed -
// the worker fetches just it (ha_fetch_single_state) instead of the full
// (~100KB) state list, and poll_worker() patches it in place below.
static ha_entity_t pending_single_entity;
static int pending_single_valid = 0;

// Set when the worker bailed before doing its op (no network / sign-in
// expired): poll_worker only updates the status line and leaves the
// current entity list untouched.
static int pending_noop = 0;

void ha_worker_init(void) {
    LightLock_Init(&state_lock);
}

int ensure_access_token(void) {
    if (time(NULL) < token_expires_at) {
        return 0;
    }

    char access[HA_MAX_TOKEN];
    int expires = 0;
    LOG("access token expired/missing, refreshing...");
    if (ha_auth_refresh(app_cfg.refresh_token, access, sizeof(access), &expires) != 0) {
        LOG("token refresh FAILED");
        return -1;
    }

    ha_client_set_access_token(access);
    token_expires_at = time(NULL) + expires - 60; // renew a minute early
    LOG("token refreshed, expires_in=%d", expires);
    return 0;
}

static void worker_thread_func(void *arg) {
    (void)arg;

    if (!network_ready) {
        LightLock_Lock(&state_lock);
        pending_noop = 1;
        snprintf(pending_status, sizeof(pending_status), "No network (soc init failed)");
        worker_result_ready = true;
        LightLock_Unlock(&state_lock);
        return;
    }

    if (ensure_access_token() != 0) {
        LightLock_Lock(&state_lock);
        pending_noop = 1;
        snprintf(pending_status, sizeof(pending_status), "Sign-in expired - press SELECT to sign in");
        worker_result_ready = true;
        LightLock_Unlock(&state_lock);
        return;
    }

    if (pending_op == OP_TOGGLE) {
        LOG("worker: toggling %s (was %s, group=%d)", pending_entity_id, pending_toggle_state,
            pending_toggle_is_group);
        ha_toggle_entity(pending_entity_id, pending_toggle_state, pending_toggle_is_group);
    } else if (pending_op == OP_SET_BRIGHTNESS) {
        LOG("worker: setting brightness %s -> %d%%", pending_entity_id, pending_brightness_pct);
        ha_set_brightness(pending_entity_id, pending_brightness_pct);
    } else if (pending_op == OP_SET_TEMPERATURE) {
        LOG("worker: setting temperature %s -> %.1f", pending_entity_id, (double)pending_target_temp);
        ha_set_temperature(pending_entity_id, pending_target_temp);
    } else if (pending_op == OP_SET_COLOR) {
        LOG("worker: setting color %s -> rgb(%d,%d,%d)", pending_entity_id,
            pending_color_r, pending_color_g, pending_color_b);
        ha_set_color(pending_entity_id, pending_color_r, pending_color_g, pending_color_b);
    } else if (pending_op == OP_SET_COLOR_TEMP) {
        LOG("worker: setting color temp %s -> %dK", pending_entity_id, pending_color_kelvin);
        ha_set_color_temp(pending_entity_id, pending_color_kelvin);
    }

    if (pending_op == OP_TOGGLE || pending_op == OP_SET_BRIGHTNESS || pending_op == OP_SET_TEMPERATURE ||
        pending_op == OP_SET_COLOR || pending_op == OP_SET_COLOR_TEMP) {
        ha_entity_t updated;
        int ok = ha_fetch_single_state(pending_entity_id, &updated) == 0;
        LOG("worker: single-entity refresh ok=%d", ok);

        LightLock_Lock(&state_lock);
        pending_noop = 0;
        pending_single_valid = ok;
        if (ok) {
            pending_single_entity = updated;
            snprintf(pending_status, sizeof(pending_status), "Updated %s", updated.friendly_name);
        } else {
            snprintf(pending_status, sizeof(pending_status), "Action sent - refresh (Y) to confirm");
        }
        worker_result_ready = true;
        LightLock_Unlock(&state_lock);
        return;
    }

    // OP_REFRESH: only case that needs the full list.
    ha_entity_t local_entities[MAX_ENTITIES];
    int result = ha_fetch_states(local_entities, MAX_ENTITIES);
    LOG("worker: fetch result=%d", result);

    if (result > 0) {
        // Room/area data needs a separate call (no area info in
        // /api/states) - failure here just means no area labels this
        // round, not a failed refresh, so it's not folded into `result`.
        ha_area_entry_t areas[MAX_ENTITIES];
        int area_count = ha_fetch_area_map(areas, MAX_ENTITIES);
        LOG("worker: area map result=%d", area_count);
        for (int i = 0; i < area_count; i++) {
            for (int j = 0; j < result; j++) {
                if (strcmp(areas[i].entity_id, local_entities[j].entity_id) == 0) {
                    strncpy(local_entities[j].area_name, areas[i].area_name, HA_MAX_NAME - 1);
                    local_entities[j].area_name[HA_MAX_NAME - 1] = '\0';
                    break;
                }
            }
        }
    }

    LightLock_Lock(&state_lock);
    pending_noop = 0;
    pending_single_valid = 0;
    if (result < 0) {
        pending_count = 0;
        snprintf(pending_status, sizeof(pending_status), "Failed to reach Home Assistant");
    } else {
        pending_count = result;
        memcpy(pending_entities, local_entities, sizeof(ha_entity_t) * (size_t)pending_count);
        snprintf(pending_status, sizeof(pending_status), "%d controllable entities", pending_count);
    }
    worker_result_ready = true;
    LightLock_Unlock(&state_lock);
}

void start_worker(int op, const char *entity_id, int int_param, float temp_param,
                   const char *current_state, int is_group) {
    if (network_busy) {
        return;
    }
    if (!signed_in) {
        snprintf(status_msg, sizeof(status_msg), "Not signed in - press SELECT to sign in");
        return;
    }

    pending_op = op;
    if (op == OP_TOGGLE || op == OP_SET_BRIGHTNESS || op == OP_SET_TEMPERATURE ||
        op == OP_SET_COLOR || op == OP_SET_COLOR_TEMP) {
        strncpy(pending_entity_id, entity_id, sizeof(pending_entity_id) - 1);
        pending_entity_id[sizeof(pending_entity_id) - 1] = '\0';
    }
    if (op == OP_TOGGLE) {
        strncpy(pending_toggle_state, current_state ? current_state : "", sizeof(pending_toggle_state) - 1);
        pending_toggle_state[sizeof(pending_toggle_state) - 1] = '\0';
        pending_toggle_is_group = is_group;
    }
    if (op == OP_SET_BRIGHTNESS) {
        pending_brightness_pct = int_param;
    }
    if (op == OP_SET_TEMPERATURE) {
        pending_target_temp = temp_param;
    }
    if (op == OP_SET_COLOR) {
        pending_color_r = (int_param >> 16) & 0xFF;
        pending_color_g = (int_param >> 8) & 0xFF;
        pending_color_b = int_param & 0xFF;
    }
    if (op == OP_SET_COLOR_TEMP) {
        pending_color_kelvin = int_param;
    }

    network_busy = true;
    worker_result_ready = false;
    const char *msg = (op == OP_TOGGLE) ? "Toggling..." :
        (op == OP_SET_BRIGHTNESS) ? "Adjusting brightness..." :
        (op == OP_SET_TEMPERATURE) ? "Setting temperature..." :
        (op == OP_SET_COLOR || op == OP_SET_COLOR_TEMP) ? "Setting color..." : "Refreshing...";
    snprintf(status_msg, sizeof(status_msg), "%s", msg);

    s32 main_prio = 0;
    svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);
    worker_thread = threadCreate(worker_thread_func, NULL, WORKER_STACK_SIZE, main_prio - 1, -2, false);
    if (!worker_thread) {
        network_busy = false;
        snprintf(status_msg, sizeof(status_msg), "Failed to start background thread");
        LOG("start_worker: threadCreate FAILED");
    }
}

void poll_worker(void) {
    if (!network_busy) {
        return;
    }

    LightLock_Lock(&state_lock);
    bool ready = worker_result_ready;
    LightLock_Unlock(&state_lock);

    if (!ready) {
        return;
    }

    threadJoin(worker_thread, U64_MAX);
    threadFree(worker_thread);
    worker_thread = NULL;

    // Capture the selection id now, while visible_indices[] still matches
    // the current entities[] - the OP_REFRESH branch below replaces
    // entities[] with a fresh fetch whose ordering isn't guaranteed to
    // match, which would make index-based capture land on the wrong entity.
    char preserved_id[HA_MAX_ENTITY_ID];
    capture_selected_id(preserved_id, sizeof(preserved_id));

    LightLock_Lock(&state_lock);
    if (pending_noop) {
        // Worker bailed before doing anything (no network / sign-in
        // expired) - only the status line changes.
    } else if (pending_op == OP_REFRESH) {
        entity_count = pending_count;
        memcpy(entities, pending_entities, sizeof(ha_entity_t) * (size_t)pending_count);
        sort_entities();
    } else if (pending_single_valid) {
        // Patch just the one entity that changed, in place, wherever it
        // currently sits in the list - a toggle/dim never adds or removes
        // rows, so entity_count/ordering are untouched. pending_single_entity
        // comes from ha_fetch_single_state(), which (unlike the OP_REFRESH
        // path) never populates area_name - area data only comes from the
        // separate ha_fetch_area_map() call merged in after a full refresh.
        // Carry the entity's existing area_name over instead of overwriting
        // it with the always-empty one, or a toggle/dim silently bumps the
        // entity into the "Ungrouped" bucket until the next full refresh.
        for (int i = 0; i < entity_count; i++) {
            if (strcmp(entities[i].entity_id, pending_single_entity.entity_id) == 0) {
                strncpy(pending_single_entity.area_name, entities[i].area_name,
                    sizeof(pending_single_entity.area_name) - 1);
                pending_single_entity.area_name[sizeof(pending_single_entity.area_name) - 1] = '\0';
                entities[i] = pending_single_entity;
                break;
            }
        }
    }
    strncpy(status_msg, pending_status, sizeof(status_msg) - 1);
    status_msg[sizeof(status_msg) - 1] = '\0';
    worker_result_ready = false;
    LightLock_Unlock(&state_lock);

    network_busy = false;
    rebuild_visible_list(preserved_id);
}

void ha_worker_join_if_busy(void) {
    if (network_busy && worker_thread) {
        LOG("waiting for in-flight worker thread before shutdown...");
        threadJoin(worker_thread, U64_MAX);
        threadFree(worker_thread);
        worker_thread = NULL;
        LOG("worker thread joined");
    }
}
