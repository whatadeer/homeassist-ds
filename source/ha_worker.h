#ifndef HA_WORKER_H
#define HA_WORKER_H

#include <stdbool.h>

enum {
    OP_REFRESH = 0,
    OP_TOGGLE = 1,
    OP_SET_BRIGHTNESS = 2,
    OP_SET_TEMPERATURE = 3,
    OP_SET_COLOR = 4,
    OP_SET_COLOR_TEMP = 5,
};

// True while a worker thread is in flight - guards against starting a
// second one (start_worker() no-ops) and gates any main-thread code that
// isn't safe to run concurrently with one (e.g. settings/sign-in entry
// points that touch shared config state).
extern volatile bool network_busy;

// Must be called once at startup, before any start_worker() call.
void ha_worker_init(void);

// Mints a fresh access token from the stored refresh token when the
// current one is near expiry. Called from the worker thread before every
// network op (and from the main thread during boot, never concurrently).
// Returns 0 if a valid token is in place.
int ensure_access_token(void);

// Kicks off a refresh, toggle-then-refresh, set-brightness-then-refresh,
// set-temperature-then-refresh, or set-color(-temp)-then-refresh on a
// background thread. No-ops if one is already running - only one network op
// is ever in flight at a time. int_param is brightness_pct for
// OP_SET_BRIGHTNESS, kelvin for OP_SET_COLOR_TEMP, or r/g/b packed as
// (r<<16)|(g<<8)|b for OP_SET_COLOR (avoids growing this signature further
// for a 3-int param used by only one op); temp_param is the target
// temperature for OP_SET_TEMPERATURE; current_state/is_group describe the
// entity (for OP_TOGGLE - see ha_toggle_entity()). Unused for other ops.
void start_worker(int op, const char *entity_id, int int_param, float temp_param,
                   const char *current_state, int is_group);

// Called once per frame from the main thread: picks up a finished worker's
// results, if any, merges them into entities[]/status_msg (see
// entity_list.h), and joins/frees its thread handle.
void poll_worker(void);

// Don't tear down anything the worker depends on (e.g. the network stack)
// out from under it if the app is exiting while a request is still in
// flight - blocks until it finishes. Safe to call even when nothing is
// running (no-op).
void ha_worker_join_if_busy(void);

#endif
