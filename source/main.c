#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <malloc.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#include "ha_client.h"
#include "app_config.h"
#include "dbglog.h"

// libctru requires this to be at least 0x100000 - a smaller buffer lets
// socInit "succeed" but leaves soc:U in a bad state that can crash later
// instead of failing cleanly.
#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000
#define MAX_ENTITIES 64
// Grouped view interleaves a non-selectable header row before each area's
// first entity, so the visible list can hold up to 2x the raw entity count
// (worst case: every entity in its own area).
#define MAX_VISIBLE_ROWS (MAX_ENTITIES * 2)
// Sentinel stored in visible_indices[] for a header row - never a valid
// entities[] index.
#define ROW_IS_HEADER (-1)
// Fallback header/group label for entities with no assigned area. Deliberately
// not "Other" - a real Home Assistant area can legitimately be named "Other",
// which would otherwise show two disconnected header rows with the same text.
#define AREA_UNASSIGNED_LABEL "Ungrouped"
#define VISIBLE_ROWS 8
#define ROW_HEIGHT 24
#define FILTER_BOX_HEIGHT 22
// Height of the persistent "current room" bar shown between the filter box
// and the list - see sticky_room_bar_active(). Doesn't consume one of the
// VISIBLE_ROWS row slots, just pushes the list's start Y down, so scrolling
// math (list positions, not pixels) is unaffected by whether it's shown.
#define STICKY_ROOM_BAR_HEIGHT 16.0f
#define FILTER_MAX_LEN 32

static u32 *soc_buffer = NULL;
static int network_ready = 0;
static ha_entity_t entities[MAX_ENTITIES];
static int entity_count = 0;
static int scroll_offset = 0;
static int selected_index = 0;
static char status_msg[128] = "Connecting...";

// Entities matching filter_text (case-insensitive substring of name or
// entity_id), in original order, interleaved with ROW_IS_HEADER sentinels
// when group_by_room is on (one before each area's first entity). Header
// rows are never selectable. selected_index/scroll_offset are positions
// into THIS list, not directly into `entities` - always go through
// visible_indices[] to get the real entities[] index.
static char filter_text[FILTER_MAX_LEN] = "";
static int visible_indices[MAX_VISIBLE_ROWS];
static int visible_count = 0;
// Count of real entity rows in visible_indices[] (visible_count minus header
// rows) - what the filter box's "(shown/total)" text should report, since
// visible_count itself includes non-entity header rows when grouped.
static int visible_entity_count = 0;

// Toggled by X: sort entities[] by (area, name) instead of just name, and
// insert a header row before each area's entities so grouping reads as
// real sections instead of a same-list resort. Room data comes from a
// separate template-based fetch (ha_fetch_area_map) merged in after every
// full refresh - see worker_thread_func's OP_REFRESH branch.
static int group_by_room = 0;

// Sign-in state. app_cfg holds the base URL + long-lived refresh token
// (persisted to SD); short-lived access tokens are minted from it and
// handed to ha_client, refreshed just before expiry. Written only by the
// main thread (boot bootstrap / wizard, both of which run while no worker
// is in flight); token_expires_at is also updated by the worker thread via
// ensure_access_token - safe for the same reason: the wizard never runs
// concurrently with a worker.
// enabled_domains defaults to "everything on" so a fresh install (before
// any config file exists, and thus before app_config_load ever runs) still
// fetches every domain, same as before this setting existed.
static app_config_t app_cfg = { .enabled_domains = HA_ALL_DOMAINS_MASK };
static time_t token_expires_at = 0;
static int signed_in = 0;

// The app is always showing exactly one of these three screens - they share
// the same render loop/frame cadence; only the drawing and input-handling
// branch on this.
typedef enum {
    APP_MODE_MAIN,
    APP_MODE_SIGNIN,
    APP_MODE_SETTINGS,
    APP_MODE_COLOR,
} app_mode_t;
static app_mode_t app_mode = APP_MODE_MAIN;

// --- Settings screen state (which entity-type domains to pull) ------------
// Opened from the main list with START. settings_mask_on_entry lets
// settings_exit() tell whether anything actually changed, so it only
// spends a refresh (and the network round-trip that comes with it) when
// needed.
static int settings_cursor = 0;
static unsigned int settings_mask_on_entry = 0;
static char settings_status[64] = "";

// --- Color screen state (rgb_color / color_temp_kelvin presets) -----------
// Opened from the main list with B on a light that supports_color or
// supports_color_temp. Captured at entry (rather than re-deriving from
// selected_index each frame) since entities[] can in principle be replaced
// by a refresh while this screen is open.
static char color_target_entity_id[HA_MAX_ENTITY_ID] = "";
static char color_target_name[HA_MAX_NAME] = "";
static int color_target_supports_color = 0;
static int color_target_supports_color_temp = 0;

// --- Sign-in form state ----------------------------------------------------
// A persistent on-screen form (URL/username/password/[2FA]) instead of a
// blind sequence of keyboard popups: tapping a field opens the system
// keyboard for just that field, and the form redraws with the updated
// value so all fields stay visible throughout. "Sign In" drives the actual
// (blocking) login-flow network calls.
enum { SF_URL, SF_USERNAME, SF_PASSWORD, SF_MFA, SF_SIGNIN, SF_CANCEL, SF_KIND_COUNT };

#define SIGNIN_LABEL_H 12.0f
#define SIGNIN_BOX_H 22.0f
#define SIGNIN_FIELD_H (SIGNIN_LABEL_H + SIGNIN_BOX_H + 4.0f)
#define SIGNIN_BTN_H 26.0f

static char signin_url[HA_MAX_URL];
static char signin_username[64];
static char signin_password[128];
static char signin_mfa[16];
static char signin_flow_id[HA_MAX_FLOW_ID];
static int signin_mfa_required = 0;
static int signin_allow_cancel = 0;
static char signin_status[96] = "Enter your Home Assistant details";

// Which fields are actually on screen this frame, and which one the D-Pad
// cursor is on - rebuilt whenever mfa_required/allow_cancel change so
// indices never go stale (MFA/Cancel appearing shifts everything after it).
static int signin_active_fields[SF_KIND_COUNT];
static int signin_active_count = 0;
static int signin_cursor = 0;

static void signin_build_active_fields(void) {
    signin_active_count = 0;
    signin_active_fields[signin_active_count++] = SF_URL;
    signin_active_fields[signin_active_count++] = SF_USERNAME;
    signin_active_fields[signin_active_count++] = SF_PASSWORD;
    if (signin_mfa_required) {
        signin_active_fields[signin_active_count++] = SF_MFA;
    }
    signin_active_fields[signin_active_count++] = SF_SIGNIN;
    if (signin_allow_cancel) {
        signin_active_fields[signin_active_count++] = SF_CANCEL;
    }
    if (signin_cursor >= signin_active_count) {
        signin_cursor = signin_active_count - 1;
    }
    if (signin_cursor < 0) {
        signin_cursor = 0;
    }
}

// Single source of truth for form layout, used by both drawing and touch
// hit-testing so they can never disagree about where a field actually is.
static float signin_field_y(int kind) {
    float y = 16.0f;
    if (kind == SF_URL) {
        return y;
    }
    y += SIGNIN_FIELD_H;
    if (kind == SF_USERNAME) {
        return y;
    }
    y += SIGNIN_FIELD_H;
    if (kind == SF_PASSWORD) {
        return y;
    }
    y += SIGNIN_FIELD_H;
    if (signin_mfa_required) {
        if (kind == SF_MFA) {
            return y;
        }
        y += SIGNIN_FIELD_H;
    }
    y += 6.0f;
    if (kind == SF_SIGNIN) {
        return y;
    }
    y += SIGNIN_BTN_H + 8.0f;
    return y; // SF_CANCEL
}

#define LOG(...) dbg_log(__VA_ARGS__)

// --- Background network worker -------------------------------------------
// ha_fetch_states()/ha_toggle_entity() are blocking HTTPS calls that can
// take several seconds over the internet; running them on the main thread
// froze rendering for the whole call. All network work now happens on a
// dedicated worker thread; `entities`/`entity_count`/`status_msg`/
// `selected_index` are only ever touched by the main thread, and the
// pending_* staging fields (written by the worker, read by the main thread)
// are guarded by state_lock.
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
static volatile bool network_busy = false;
static volatile bool worker_result_ready = false;

enum {
    OP_REFRESH = 0,
    OP_TOGGLE = 1,
    OP_SET_BRIGHTNESS = 2,
    OP_SET_TEMPERATURE = 3,
    OP_SET_COLOR = 4,
    OP_SET_COLOR_TEMP = 5,
};

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

static void clamp_selection(void) {
    if (selected_index >= visible_count) {
        selected_index = visible_count - 1;
    }
    if (selected_index < 0) {
        selected_index = 0;
    }

    int max_offset = visible_count - VISIBLE_ROWS;
    if (max_offset < 0) {
        max_offset = 0;
    }
    if (scroll_offset > max_offset) {
        scroll_offset = max_offset;
    }
    if (scroll_offset < 0) {
        scroll_offset = 0;
    }

    // Keep the selected row inside the visible window.
    if (selected_index < scroll_offset) {
        scroll_offset = selected_index;
    }
    if (selected_index >= scroll_offset + VISIBLE_ROWS) {
        scroll_offset = selected_index - VISIBLE_ROWS + 1;
    }
}

// Walks from pos in the given direction until landing on a non-header slot
// (or running off the list). Shared by move_selection() and
// rebuild_visible_list() so "never select a header row" has one
// implementation instead of two that can drift apart.
static int first_selectable_from(int pos, int dir) {
    while (pos >= 0 && pos < visible_count && visible_indices[pos] == ROW_IS_HEADER) {
        pos += dir;
    }
    return pos;
}

// Moves selected_index by dir (+-1), stepping over any header row so
// Up/Down always lands on a real entity. If dir runs off the end of the
// list without finding one, the selection is left where it was.
static void move_selection(int dir) {
    int pos = first_selectable_from(selected_index + dir, dir);
    if (pos >= 0 && pos < visible_count) {
        selected_index = pos;
    }
    clamp_selection();
}

// True when the top of the current scroll window is mid-group - the
// area's own header has scrolled above the visible list - so grouping
// would otherwise show zero room context on screen. Not true when
// scroll_offset lands exactly on a header row: that header already shows
// the same text inline, so the bar would just repeat it.
static int sticky_room_bar_active(void) {
    return group_by_room && scroll_offset >= 0 && scroll_offset < visible_count &&
        visible_indices[scroll_offset] != ROW_IS_HEADER;
}

// Y where the entity list starts. Pushed down by STICKY_ROOM_BAR_HEIGHT
// when sticky_room_bar_active() so the bar has its own space without
// stealing one of the VISIBLE_ROWS row slots - list positions (what
// clamp_selection/scroll_offset track) are pixel-agnostic, so this can
// change from one frame to the next as scroll_offset moves without any
// scrolling math needing to know about it.
static float list_top_y(void) {
    return FILTER_BOX_HEIGHT + (sticky_room_bar_active() ? STICKY_ROOM_BAR_HEIGHT : 0.0f);
}

// Case-insensitive string comparison - not relying on newlib providing
// strcasecmp, same reasoning as str_contains_ci below.
static int ci_strcmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca += 32;
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 32;
        }
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        if (ca == '\0') {
            return 0;
        }
        a++;
        b++;
    }
}

static int compare_entity_name_ci(const void *a, const void *b) {
    return ci_strcmp(((const ha_entity_t *)a)->friendly_name, ((const ha_entity_t *)b)->friendly_name);
}

// Groups by area/room first (entities with no assigned area sort last,
// after every named area), then by name within each area.
static int compare_entity_area_then_name_ci(const void *a, const void *b) {
    const ha_entity_t *ea = (const ha_entity_t *)a;
    const ha_entity_t *eb = (const ha_entity_t *)b;

    int a_empty = (ea->area_name[0] == '\0');
    int b_empty = (eb->area_name[0] == '\0');
    if (a_empty != b_empty) {
        return a_empty ? 1 : -1;
    }

    int area_cmp = ci_strcmp(ea->area_name, eb->area_name);
    if (area_cmp != 0) {
        return area_cmp;
    }
    return ci_strcmp(ea->friendly_name, eb->friendly_name);
}

// Display label for an entity's area/room: its own area_name, or
// AREA_UNASSIGNED_LABEL if it has none. Only for display - grouping
// decisions in rebuild_visible_list compare the raw area_name instead, so a
// real area actually named AREA_UNASSIGNED_LABEL can't merge with the
// no-area bucket.
static const char *entity_area_label(const ha_entity_t *e) {
    return e->area_name[0] ? e->area_name : AREA_UNASSIGNED_LABEL;
}

// Sorts entities[] in place using whichever order is currently selected.
// Callers must capture_selected_id() before calling this (sorting changes
// which index the previously-selected entity sits at) and
// rebuild_visible_list() after.
static void sort_entities(void) {
    qsort(entities, (size_t)entity_count, sizeof(ha_entity_t),
        group_by_room ? compare_entity_area_then_name_ci : compare_entity_name_ci);
}

// Case-insensitive substring search, since newlib's strcasestr availability
// isn't guaranteed here and this is cheap to just write directly.
static int str_contains_ci(const char *haystack, const char *needle) {
    if (needle[0] == '\0') {
        return 1;
    }

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) {
        return 0;
    }

    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a += 32;
            }
            if (b >= 'A' && b <= 'Z') {
                b += 32;
            }
            if (a != b) {
                break;
            }
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

// Copies the currently-selected entity's id into buf (empty string if no
// valid selection). Must be called while visible_indices[] still matches
// the entities[] array it was built from - i.e. BEFORE overwriting
// entities[] with a fresh fetch.
static void capture_selected_id(char *buf, size_t bufsize) {
    buf[0] = '\0';
    if (selected_index >= 0 && selected_index < visible_count &&
        visible_indices[selected_index] != ROW_IS_HEADER) {
        strncpy(buf, entities[visible_indices[selected_index]].entity_id, bufsize - 1);
        buf[bufsize - 1] = '\0';
    }
}

// Recomputes visible_indices[] from entities[]/filter_text, re-selecting
// preserved_id (captured by the caller via capture_selected_id) if it's
// still in the filtered list, else resetting to the first real row. The
// caller captures the id rather than this function doing it because
// entities[] may have been replaced (fresh fetch, possibly reordered)
// between capture and rebuild - indexing the new array through the old
// visible_indices[] would preserve the wrong entity.
//
// When group_by_room is on, a ROW_IS_HEADER sentinel is inserted right
// before the first entity of each area (entities[] is assumed already
// sorted by (area, name) - see sort_entities()), so a run of same-area
// entities in the filtered list stays contiguous and gets exactly one
// header.
static void rebuild_visible_list(const char *preserved_id) {
    visible_count = 0;
    visible_entity_count = 0;
    // Raw area_name of the last entity added (may be "" for unassigned) -
    // compared case-insensitively, matching sort_entities()'s comparator, so
    // a header boundary is never split or merged by casing alone. Compared
    // on the raw name rather than entity_area_label()'s resolved text, so a
    // real area actually named AREA_UNASSIGNED_LABEL can't be confused with
    // the no-area bucket.
    char last_area[HA_MAX_NAME];
    int have_last_area = 0;
    for (int i = 0; i < entity_count; i++) {
        if (!(str_contains_ci(entities[i].friendly_name, filter_text) ||
              str_contains_ci(entities[i].entity_id, filter_text))) {
            continue;
        }
        if (group_by_room) {
            const char *raw_area = entities[i].area_name;
            if (!have_last_area || ci_strcmp(raw_area, last_area) != 0) {
                visible_indices[visible_count++] = ROW_IS_HEADER;
                strncpy(last_area, raw_area, sizeof(last_area) - 1);
                last_area[sizeof(last_area) - 1] = '\0';
                have_last_area = 1;
            }
        }
        visible_indices[visible_count++] = i;
        visible_entity_count++;
    }

    selected_index = 0;
    if (preserved_id[0]) {
        for (int i = 0; i < visible_count; i++) {
            if (visible_indices[i] != ROW_IS_HEADER &&
                strcmp(entities[visible_indices[i]].entity_id, preserved_id) == 0) {
                selected_index = i;
                break;
            }
        }
    }
    selected_index = first_selectable_from(selected_index, 1);

    clamp_selection();
}

// Mints a fresh access token from the stored refresh token when the
// current one is near expiry. Called from the worker thread before every
// network op (and from the main thread during boot, never concurrently).
// Returns 0 if a valid token is in place.
static int ensure_access_token(void) {
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

// Kicks off a refresh, toggle-then-refresh, set-brightness-then-refresh,
// set-temperature-then-refresh, or set-color(-temp)-then-refresh on the
// worker thread. No-ops if one is already running - only one network op is
// ever in flight at a time. int_param is brightness_pct for
// OP_SET_BRIGHTNESS, kelvin for OP_SET_COLOR_TEMP, or r/g/b packed as
// (r<<16)|(g<<8)|b for OP_SET_COLOR (avoids growing this signature further
// for a 3-int param used by only one op); temp_param is the target
// temperature for OP_SET_TEMPERATURE; current_state/is_group describe the
// entity (for OP_TOGGLE - see ha_toggle_entity()). Unused for other ops.
static void start_worker(int op, const char *entity_id, int int_param, float temp_param,
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

// Called once per frame from the main thread: picks up a finished worker's
// results, if any, and joins/frees its thread handle.
static void poll_worker(void) {
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

// Domain -> simple vector icon, drawn with citro2d primitives only (no
// bitmap/texture pipeline needed). box_x/box_y is the top-left of an
// ICON_SIZE x ICON_SIZE square to draw within.
#define ICON_SIZE 18.0f

static void draw_domain_icon(const char *entity_id, float box_x, float box_y, int is_group) {
    const char *dot = strchr(entity_id, '.');
    size_t domain_len = dot ? (size_t)(dot - entity_id) : strlen(entity_id);

    float cx = box_x + ICON_SIZE / 2.0f;
    float cy = box_y + ICON_SIZE / 2.0f;

#define DOMAIN_IS(name) (domain_len == strlen(name) && strncmp(entity_id, name, domain_len) == 0)

    // citro2d's depth test is GPU_GEQUAL (a draw only shows if its z is >=
    // whatever's already at that pixel - see citro2d/source/base.c). Row
    // backgrounds are drawn at z=0.4 right before this function runs, so
    // every icon shape here needs z > 0.4, in non-decreasing order among
    // shapes that overlap each other, or it silently fails to appear.
    if (DOMAIN_IS("light")) {
        C2D_DrawEllipseSolid(box_x + 2, box_y + 1, 0.42f, ICON_SIZE - 4, ICON_SIZE - 4,
            C2D_Color32(0xFF, 0xC1, 0x07, 0xFF));
    } else if (DOMAIN_IS("switch")) {
        C2D_DrawRectSolid(box_x + 1, box_y + 4, 0.42f, ICON_SIZE - 2, ICON_SIZE - 8,
            C2D_Color32(0x26, 0xA6, 0x9A, 0xFF));
        C2D_DrawEllipseSolid(box_x + ICON_SIZE - 8, box_y + 2, 0.43f, 7, 7,
            C2D_Color32(0xE0, 0xE0, 0xE0, 0xFF));
    } else if (DOMAIN_IS("fan")) {
        C2D_DrawEllipseSolid(box_x + 3, box_y + 3, 0.42f, ICON_SIZE - 6, ICON_SIZE - 6,
            C2D_Color32(0x4F, 0xC3, 0xF7, 0xFF));
        C2D_DrawTriangle(
            cx, cy, C2D_Color32(0x01, 0x57, 0x9B, 0xFF),
            box_x + ICON_SIZE, box_y + 2, C2D_Color32(0x01, 0x57, 0x9B, 0xFF),
            box_x + ICON_SIZE, box_y + 8, C2D_Color32(0x01, 0x57, 0x9B, 0xFF),
            0.43f);
    } else if (DOMAIN_IS("lock")) {
        C2D_DrawEllipseSolid(box_x + 3, box_y, 0.42f, ICON_SIZE - 6, 10,
            C2D_Color32(0x9E, 0x9E, 0x9E, 0xFF));
        C2D_DrawRectSolid(box_x + 1, box_y + 7, 0.43f, ICON_SIZE - 2, ICON_SIZE - 8,
            C2D_Color32(0xBD, 0xBD, 0xBD, 0xFF));
    } else if (DOMAIN_IS("cover")) {
        C2D_DrawRectSolid(box_x + 1, box_y + 1, 0.42f, ICON_SIZE - 2, ICON_SIZE - 2,
            C2D_Color32(0xA1, 0x88, 0x7F, 0xFF));
        C2D_DrawLine(box_x + 1, cy - 3, C2D_Color32(0x5D, 0x40, 0x37, 0xFF),
            box_x + ICON_SIZE - 1, cy - 3, C2D_Color32(0x5D, 0x40, 0x37, 0xFF), 1.5f, 0.43f);
        C2D_DrawLine(box_x + 1, cy + 3, C2D_Color32(0x5D, 0x40, 0x37, 0xFF),
            box_x + ICON_SIZE - 1, cy + 3, C2D_Color32(0x5D, 0x40, 0x37, 0xFF), 1.5f, 0.43f);
    } else if (DOMAIN_IS("climate")) {
        C2D_DrawEllipseSolid(box_x + 1, box_y + 1, 0.42f, ICON_SIZE - 2, ICON_SIZE - 2,
            C2D_Color32(0xFF, 0x70, 0x43, 0xFF));
        C2D_DrawEllipseSolid(cx - 4, cy - 4, 0.43f, 8, 8, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
    } else if (DOMAIN_IS("media_player")) {
        C2D_DrawTriangle(
            box_x + 3, box_y + 2, C2D_Color32(0x66, 0xBB, 0x6A, 0xFF),
            box_x + 3, box_y + ICON_SIZE - 2, C2D_Color32(0x66, 0xBB, 0x6A, 0xFF),
            box_x + ICON_SIZE - 2, cy, C2D_Color32(0x66, 0xBB, 0x6A, 0xFF),
            0.42f);
    } else if (DOMAIN_IS("input_boolean")) {
        C2D_DrawEllipseSolid(box_x + 4, box_y + 4, 0.42f, ICON_SIZE - 8, ICON_SIZE - 8,
            C2D_Color32(0xAB, 0x47, 0xBC, 0xFF));
    } else {
        C2D_DrawRectSolid(box_x + 3, box_y + 3, 0.42f, ICON_SIZE - 6, ICON_SIZE - 6,
            C2D_Color32(0x77, 0x77, 0x77, 0xFF));
    }

#undef DOMAIN_IS

    if (is_group) {
        // Small corner badge: this row is a Home Assistant group (multiple
        // member entities acting as one, e.g. "Den Lamps"), not an
        // individual entity of the domain the base icon shows. Dark ring
        // first so the badge stays legible over any icon color, then the
        // accent-colored dot on top - both above the icon shapes' z range
        // (0.42-0.43) so they aren't hidden by the depth test (see the
        // z-ordering note above).
        C2D_DrawEllipseSolid(box_x + ICON_SIZE - 9, box_y + ICON_SIZE - 9, 0.44f, 9, 9,
            C2D_Color32(0x1a, 0x1a, 0x1a, 0xFF));
        C2D_DrawEllipseSolid(box_x + ICON_SIZE - 8, box_y + ICON_SIZE - 8, 0.45f, 7, 7,
            C2D_Color32(0xFF, 0xB3, 0x4D, 0xFF));
    }
}

// Small rotating-dot spinner shown while a network request is in flight.
static void draw_spinner(float cx, float cy, float radius, int frame_counter) {
    const int NUM_DOTS = 8;
    int active = (frame_counter / 4) % NUM_DOTS;

    for (int i = 0; i < NUM_DOTS; i++) {
        float angle = (2.0f * (float)M_PI * i) / NUM_DOTS;
        float dot_size = (i == active) ? 5.0f : 3.0f;
        float dx = cx + radius * cosf(angle) - dot_size / 2.0f;
        float dy = cy + radius * sinf(angle) - dot_size / 2.0f;
        u32 color = (i == active) ? C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF) : C2D_Color32(0x60, 0x60, 0x70, 0xFF);
        C2D_DrawEllipseSolid(dx, dy, 0.2f, dot_size, dot_size, color);
    }
}

// Generic blocking keyboard prompt. Returns 1 if the user confirmed (right
// button), 0 if they cancelled. out keeps its previous content on cancel.
static int prompt_text(const char *hint, const char *initial, char *out, size_t out_size, int password) {
    SwkbdState kb;
    char buf[HA_MAX_TOKEN];
    if (out_size > sizeof(buf)) {
        out_size = sizeof(buf);
    }
    snprintf(buf, sizeof(buf), "%s", initial ? initial : "");

    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)out_size - 1);
    swkbdSetInitialText(&kb, buf);
    swkbdSetHintText(&kb, hint);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (password) {
        swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
    }

    SwkbdButton button = swkbdInputText(&kb, buf, out_size);
    if (button != SWKBD_BUTTON_RIGHT) {
        return 0;
    }

    // swkbdInputText already enforced out_size-1 via swkbdInit's
    // maxTextLength, so this never actually truncates - but GCC can't see
    // that across the applet call, and flags a copy from the 512-byte buf
    // into a possibly-smaller out_size as a truncation risk regardless of
    // which copy function is used. Compute the bound explicitly so it can
    // verify it.
    size_t copy_len = strlen(buf);
    if (copy_len >= out_size) {
        copy_len = out_size - 1;
    }
    memcpy(out, buf, copy_len);
    out[copy_len] = '\0';
    return 1;
}

// Parses, optimizes, and draws a single line of text - the same three calls
// repeated at every text-drawing site in this file, collapsed to one for the
// entity list row/header text (the code this diff touches).
static void draw_text(C2D_Font font, C2D_TextBuf dynBuf, const char *str,
                       float x, float y, float z, float scale, u32 color) {
    C2D_Text text;
    C2D_TextFontParse(&text, font, dynBuf, str);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, z, scale, scale, color);
}

// Renders one static frame with up to two lines of text on the top screen -
// used around the blocking sign-in network calls so the display shows
// what's happening rather than a stale frame.
static void draw_status_frame(C2D_Font font, C2D_TextBuf dynBuf,
                              C3D_RenderTarget *top, C3D_RenderTarget *bottom,
                              const char *line1, const char *line2) {
    C2D_TextBufClear(dynBuf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    C2D_TargetClear(top, C2D_Color32(0x18, 0x1c, 0x28, 0xFF));
    C2D_SceneBegin(top);

    C2D_Text t1;
    C2D_TextFontParse(&t1, font, dynBuf, line1);
    C2D_TextOptimize(&t1);
    C2D_DrawText(&t1, C2D_WithColor, 12.0f, 90.0f, 0.5f, 0.55f, 0.55f,
        C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

    if (line2 && line2[0]) {
        C2D_Text t2;
        C2D_TextFontParse(&t2, font, dynBuf, line2);
        C2D_TextOptimize(&t2);
        C2D_DrawText(&t2, C2D_WithColor, 12.0f, 120.0f, 0.5f, 0.45f, 0.45f,
            C2D_Color32(0x9f, 0xd8, 0xff, 0xFF));
    }

    C2D_TargetClear(bottom, C2D_Color32(0x10, 0x12, 0x18, 0xFF));
    C2D_SceneBegin(bottom);

    C3D_FrameEnd(0);
}

// Switches to the Settings screen, remembering the mask it was opened with
// so settings_exit() can tell whether a refresh is actually needed.
static void settings_enter(void) {
    settings_mask_on_entry = app_cfg.enabled_domains;
    settings_cursor = 0;
    settings_status[0] = '\0';
    app_mode = APP_MODE_SETTINGS;
}

// Back to the main list. Only kicks off a refresh if the enabled-domain set
// actually changed while in here - toggling nothing back and forth
// shouldn't cost a network round-trip. Skipped while a worker is already
// in flight; the existing list just stays as-is until the next manual/auto
// refresh, same as any other change made mid-request.
static void settings_exit(void) {
    app_mode = APP_MODE_MAIN;
    if (app_cfg.enabled_domains != settings_mask_on_entry && !network_busy) {
        start_worker(OP_REFRESH, NULL, 0, 0.0f, NULL, 0);
    }
}

// Flips one domain's bit, applies it to ha_client immediately, and persists
// it - each toggle is a rare, deliberate action, so saving on every flip
// (rather than batching until settings_exit()) keeps the choice safe even
// if the app is closed from the HOME menu while still on this screen.
// Refuses to turn off the last enabled domain: an empty entity list with no
// on-screen explanation is more confusing than a blocked toggle.
static void settings_toggle_domain(int idx) {
    unsigned int candidate = app_cfg.enabled_domains ^ (1u << idx);
    if (candidate == 0) {
        snprintf(settings_status, sizeof(settings_status), "At least one type must stay on");
        return;
    }

    app_cfg.enabled_domains = candidate;
    ha_client_set_enabled_domains(candidate);
    settings_status[0] = '\0';
    if (app_config_save(&app_cfg) != 0) {
        LOG("WARNING: settings save failed");
    }
}

// Switches to the Color screen for one entity (B on a light that
// supports_color or supports_color_temp - see the KEY_B handler in main()).
static void color_enter(const ha_entity_t *e) {
    strncpy(color_target_entity_id, e->entity_id, sizeof(color_target_entity_id) - 1);
    color_target_entity_id[sizeof(color_target_entity_id) - 1] = '\0';
    strncpy(color_target_name, e->friendly_name, sizeof(color_target_name) - 1);
    color_target_name[sizeof(color_target_name) - 1] = '\0';
    color_target_supports_color = e->supports_color;
    color_target_supports_color_temp = e->supports_color_temp;
    app_mode = APP_MODE_COLOR;
}

// Back to the main list. No refresh needed here - tapping a preset already
// starts a set-color-then-single-entity-refresh worker (see OP_SET_COLOR/
// OP_SET_COLOR_TEMP), same as brightness.
static void color_exit(void) {
    app_mode = APP_MODE_MAIN;
}

// Resets the form and switches to it. allow_cancel is 0 at boot when there's
// no working session to fall back to, 1 when opened via SELECT from an
// already-signed-in state.
static void signin_enter(int allow_cancel) {
    snprintf(signin_url, sizeof(signin_url), "%s", app_cfg.base_url[0] ? app_cfg.base_url : "https://");
    signin_username[0] = '\0';
    signin_password[0] = '\0';
    signin_mfa[0] = '\0';
    signin_mfa_required = 0;
    signin_allow_cancel = allow_cancel;
    signin_cursor = 0;
    snprintf(signin_status, sizeof(signin_status), "Enter your Home Assistant details");
    signin_build_active_fields();
    app_mode = APP_MODE_SIGNIN;
}

// Exchanges auth_code for tokens, persists them, and drops back to the main
// list on success. On failure, stays on the form with an error and (for the
// MFA path) clears the code so it's obviously not still "pending".
static void signin_finish_exchange(const char *auth_code, C2D_Font font, C2D_TextBuf dynBuf,
                                   C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
    (void)font;
    (void)dynBuf;
    (void)top;
    (void)bottom;

    char access[HA_MAX_TOKEN];
    char refresh[HA_MAX_TOKEN];
    int expires = 0;
    if (ha_auth_exchange_code(auth_code, refresh, sizeof(refresh), access, sizeof(access), &expires) != 0) {
        snprintf(signin_status, sizeof(signin_status), "Sign-in failed - token exchange was rejected");
        signin_mfa_required = 0;
        signin_build_active_fields();
        return;
    }

    strncpy(app_cfg.base_url, signin_url, sizeof(app_cfg.base_url) - 1);
    app_cfg.base_url[sizeof(app_cfg.base_url) - 1] = '\0';
    strncpy(app_cfg.refresh_token, refresh, sizeof(app_cfg.refresh_token) - 1);
    app_cfg.refresh_token[sizeof(app_cfg.refresh_token) - 1] = '\0';
    if (app_config_save(&app_cfg) != 0) {
        LOG("WARNING: config save failed - sign-in won't survive relaunch");
    }

    ha_client_set_access_token(access);
    token_expires_at = time(NULL) + expires - 60;
    signed_in = 1;
    app_mode = APP_MODE_MAIN;
    snprintf(status_msg, sizeof(status_msg), "Signed in");
    LOG("sign-in complete for %s", signin_url);
    start_worker(OP_REFRESH, NULL, 0, 0.0f, NULL, 0);
}

// Runs the (blocking) login-flow network calls for the current form state:
// first press submits URL+username+password; if that comes back needing
// MFA, the form now shows a code field and this same function's second
// press submits the code instead. draw_status_frame keeps the screen
// informative during the actual network calls, same as the rest of the app
// does for other blocking moments.
static void signin_attempt(C2D_Font font, C2D_TextBuf dynBuf, C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
    if (!strstr(signin_url, "://")) {
        char with_scheme[HA_MAX_URL + 8];
        snprintf(with_scheme, sizeof(with_scheme), "https://%s", signin_url);
        strncpy(signin_url, with_scheme, sizeof(signin_url) - 1);
        signin_url[sizeof(signin_url) - 1] = '\0';
    }
    ha_client_set_base_url(signin_url);

    char auth_code[HA_MAX_AUTH_CODE];

    if (!signin_mfa_required) {
        draw_status_frame(font, dynBuf, top, bottom, "Signing in...", signin_url);

        if (ha_auth_login_begin(signin_flow_id, sizeof(signin_flow_id)) != 0) {
            snprintf(signin_status, sizeof(signin_status), "Could not reach that server");
            return;
        }

        int step = ha_auth_login_submit(signin_flow_id, signin_username, signin_password,
                                        auth_code, sizeof(auth_code));
        if (step == 1) {
            signin_mfa_required = 1;
            signin_build_active_fields();
            snprintf(signin_status, sizeof(signin_status), "Enter your two-factor code");
            return;
        }
        if (step != 0) {
            snprintf(signin_status, sizeof(signin_status), "Sign-in failed - check username/password");
            return;
        }
    } else {
        draw_status_frame(font, dynBuf, top, bottom, "Checking code...", NULL);

        int step = ha_auth_login_mfa(signin_flow_id, signin_mfa, auth_code, sizeof(auth_code));
        if (step != 0) {
            signin_mfa[0] = '\0';
            snprintf(signin_status, sizeof(signin_status), "Incorrect code - try again");
            return;
        }
    }

    signin_finish_exchange(auth_code, font, dynBuf, top, bottom);
}

// Handles activating (touch tap or A-button) whichever form element is
// current. Fields open the keyboard pre-filled with their current value;
// Sign In/Cancel do the obvious thing.
static void signin_activate(int kind, C2D_Font font, C2D_TextBuf dynBuf,
                            C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
    switch (kind) {
        case SF_URL:
            prompt_text("Home Assistant URL (e.g. https://ha.example.com)",
                signin_url, signin_url, sizeof(signin_url), 0);
            break;
        case SF_USERNAME:
            prompt_text("Username", signin_username, signin_username, sizeof(signin_username), 0);
            break;
        case SF_PASSWORD:
            prompt_text("Password", signin_password, signin_password, sizeof(signin_password), 1);
            break;
        case SF_MFA:
            prompt_text("Two-factor code", signin_mfa, signin_mfa, sizeof(signin_mfa), 0);
            break;
        case SF_SIGNIN:
            signin_attempt(font, dynBuf, top, bottom);
            break;
        case SF_CANCEL:
            app_mode = APP_MODE_MAIN;
            snprintf(status_msg, sizeof(status_msg), "Sign-in cancelled");
            break;
        default:
            break;
    }
}

static void draw_signin_field(C2D_Font font, C2D_TextBuf dynBuf, const char *label,
                              const char *value, float y, int selected, int mask) {
    C2D_Text labelText;
    C2D_TextFontParse(&labelText, font, dynBuf, label);
    C2D_TextOptimize(&labelText);
    C2D_DrawText(&labelText, C2D_WithColor, 8.0f, y, 0.4f, 0.32f, 0.32f,
        C2D_Color32(0x99, 0x99, 0x99, 0xFF));

    float box_y = y + SIGNIN_LABEL_H;
    u32 box_color = selected ? C2D_Color32(0x2f, 0x6f, 0xdf, 0xFF) : C2D_Color32(0x24, 0x28, 0x34, 0xFF);
    C2D_DrawRectSolid(4.0f, box_y, 0.4f, 312.0f, SIGNIN_BOX_H, box_color);

    char display[48];
    if (value[0] == '\0') {
        snprintf(display, sizeof(display), "(tap to enter)");
    } else if (mask) {
        size_t len = strlen(value);
        if (len > 20) {
            len = 20;
        }
        memset(display, '*', len);
        display[len] = '\0';
    } else {
        snprintf(display, sizeof(display), "%.44s", value);
    }

    C2D_Text valueText;
    C2D_TextFontParse(&valueText, font, dynBuf, display);
    C2D_TextOptimize(&valueText);
    u32 text_color = value[0] ? C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF) : C2D_Color32(0x77, 0x77, 0x77, 0xFF);
    C2D_DrawText(&valueText, C2D_WithColor, 10.0f, box_y + 4.0f, 0.42f, 0.38f, 0.38f, text_color);
}

static void draw_signin_button(C2D_Font font, C2D_TextBuf dynBuf, const char *label,
                               float y, u32 color, int selected) {
    C2D_DrawRectSolid(4.0f, y, 0.4f, 312.0f, SIGNIN_BTN_H, color);
    if (selected) {
        C2D_DrawRectSolid(4.0f, y, 0.41f, 4.0f, SIGNIN_BTN_H, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
    }

    C2D_Text labelText;
    C2D_TextFontParse(&labelText, font, dynBuf, label);
    C2D_TextOptimize(&labelText);
    C2D_DrawText(&labelText, C2D_WithColor | C2D_AlignCenter, 4.0f + 312.0f / 2.0f, y + 5.0f,
        0.42f, 0.42f, 0.42f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
}

static void draw_signin_screen(C2D_Font font, C2D_TextBuf dynBuf,
                               C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
    C2D_TargetClear(top, C2D_Color32(0x18, 0x1c, 0x28, 0xFF));
    C2D_SceneBegin(top);

    C2D_Text title;
    C2D_TextFontParse(&title, font, dynBuf, "Sign in to Home Assistant");
    C2D_TextOptimize(&title);
    C2D_DrawText(&title, C2D_WithColor, 12.0f, 8.0f, 0.5f, 0.6f, 0.6f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

    C2D_Text status;
    C2D_TextFontParse(&status, font, dynBuf, signin_status);
    C2D_TextOptimize(&status);
    C2D_DrawText(&status, C2D_WithColor, 12.0f, 36.0f, 0.5f, 0.5f, 0.5f, C2D_Color32(0x9f, 0xd8, 0xff, 0xFF));

    C2D_TargetClear(bottom, C2D_Color32(0x10, 0x12, 0x18, 0xFF));
    C2D_SceneBegin(bottom);

    draw_signin_field(font, dynBuf, "Server URL", signin_url,
        signin_field_y(SF_URL), signin_active_fields[signin_cursor] == SF_URL, 0);
    draw_signin_field(font, dynBuf, "Username", signin_username,
        signin_field_y(SF_USERNAME), signin_active_fields[signin_cursor] == SF_USERNAME, 0);
    draw_signin_field(font, dynBuf, "Password", signin_password,
        signin_field_y(SF_PASSWORD), signin_active_fields[signin_cursor] == SF_PASSWORD, 1);
    if (signin_mfa_required) {
        draw_signin_field(font, dynBuf, "Two-Factor Code", signin_mfa,
            signin_field_y(SF_MFA), signin_active_fields[signin_cursor] == SF_MFA, 0);
    }

    draw_signin_button(font, dynBuf, signin_mfa_required ? "Verify Code" : "Sign In",
        signin_field_y(SF_SIGNIN), C2D_Color32(0x2f, 0x6f, 0xdf, 0xFF),
        signin_active_fields[signin_cursor] == SF_SIGNIN);

    if (signin_allow_cancel) {
        draw_signin_button(font, dynBuf, "Cancel",
            signin_field_y(SF_CANCEL), C2D_Color32(0x50, 0x50, 0x58, 0xFF),
            signin_active_fields[signin_cursor] == SF_CANCEL);
    }
}

#define SETTINGS_ROW_H 26.0f
#define SETTINGS_TOP_Y 22.0f

static void draw_settings_screen(C2D_Font font, C2D_TextBuf dynBuf,
                                 C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
    C2D_TargetClear(top, C2D_Color32(0x18, 0x1c, 0x28, 0xFF));
    C2D_SceneBegin(top);

    draw_text(font, dynBuf, "Settings", 12.0f, 8.0f, 0.5f, 0.6f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
    draw_text(font, dynBuf, "Entity types to pull from Home Assistant", 12.0f, 36.0f, 0.5f, 0.5f,
        C2D_Color32(0x9f, 0xd8, 0xff, 0xFF));

    if (settings_status[0]) {
        draw_text(font, dynBuf, settings_status, 12.0f, 60.0f, 0.5f, 0.42f, C2D_Color32(0xFF, 0x99, 0x55, 0xFF));
    }

    draw_text(font, dynBuf, "Touch/A: toggle   Up/Down: move   B/START: back", 12.0f, 196.0f, 0.5f, 0.4f,
        C2D_Color32(0x88, 0x88, 0x88, 0xFF));

    C2D_TargetClear(bottom, C2D_Color32(0x10, 0x12, 0x18, 0xFF));
    C2D_SceneBegin(bottom);

    C2D_DrawRectSolid(0.0f, 0.0f, 0.4f, 320.0f, SETTINGS_TOP_Y, C2D_Color32(0x28, 0x2c, 0x38, 0xFF));
    draw_text(font, dynBuf, "Entity Types", 6.0f, 4.0f, 0.5f, 0.4f, C2D_Color32(0xCC, 0xCC, 0xCC, 0xFF));

    for (int i = 0; i < HA_NUM_DOMAINS; i++) {
        float y = SETTINGS_TOP_Y + (float)i * SETTINGS_ROW_H;
        int selected = (i == settings_cursor);
        int on = (app_cfg.enabled_domains & (1u << i)) != 0;

        u32 rowColor = selected ? C2D_Color32(0x2f, 0x6f, 0xdf, 0xFF)
            : (i % 2 == 0) ? C2D_Color32(0x20, 0x24, 0x30, 0xFF) : C2D_Color32(0x18, 0x1a, 0x24, 0xFF);
        C2D_DrawRectSolid(0.0f, y, 0.4f, 320.0f, SETTINGS_ROW_H, rowColor);
        if (selected) {
            C2D_DrawRectSolid(0.0f, y, 0.41f, 4.0f, SETTINGS_ROW_H, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
        }

        draw_text(font, dynBuf, HA_DOMAIN_LABELS[i], 14.0f, y + 6.0f, 0.5f, 0.44f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

        const char *state = on ? "ON" : "OFF";
        u32 stateColor = on ? C2D_Color32(0x4c, 0xd9, 0x64, 0xFF) : C2D_Color32(0x88, 0x88, 0x88, 0xFF);
        draw_text(font, dynBuf, state, 278.0f, y + 6.0f, 0.5f, 0.44f, stateColor);
    }
}

// --- Color screen (rgb_color / color_temp_kelvin presets) ------------------
// A fixed preset palette rather than a full color wheel/slider: precise
// hue/saturation dragging on a 320x240 touch screen is fiddly, and presets
// cover the common case (set a mood color, or warm/neutral/cool white) in
// one tap.
#define COLOR_TOP_Y 22.0f
#define COLOR_GRID_COLS 3
#define COLOR_CELL_W (320.0f / COLOR_GRID_COLS)
#define COLOR_CELL_H 50.0f
#define COLOR_TEMP_CELL_H 44.0f

typedef struct {
    int r, g, b;
    const char *label;
} color_preset_t;

static const color_preset_t COLOR_PRESETS[] = {
    {255, 0, 0, "Red"}, {255, 140, 0, "Orange"}, {255, 215, 0, "Yellow"},
    {0, 200, 0, "Green"}, {0, 200, 200, "Cyan"}, {30, 80, 255, "Blue"},
    {160, 60, 220, "Purple"}, {255, 20, 147, "Pink"}, {255, 255, 255, "White"},
};
#define NUM_COLOR_PRESETS (sizeof(COLOR_PRESETS) / sizeof(COLOR_PRESETS[0]))
// Rows the color grid actually occupies - shared by draw_color_screen()
// (where the temp row starts) and the APP_MODE_COLOR touch handler (which
// row a tap in the color grid landed in), so the two agree on layout.
#define COLOR_GRID_ROWS ((int)((NUM_COLOR_PRESETS + COLOR_GRID_COLS - 1) / COLOR_GRID_COLS))

typedef struct {
    int kelvin;
    int r, g, b; // approximate swatch color for the temperature, display only
    const char *label;
} temp_preset_t;

static const temp_preset_t TEMP_PRESETS[] = {
    {2700, 255, 179, 102, "Warm"},
    {4000, 255, 244, 229, "Neutral"},
    {6500, 204, 229, 255, "Cool"},
};
#define NUM_TEMP_PRESETS (sizeof(TEMP_PRESETS) / sizeof(TEMP_PRESETS[0]))

// Top-left of grid cell `index` in a `cols`-wide grid whose rows start at
// base_y, each `cell_h` tall - shared by draw_color_screen() (drawing) and
// the APP_MODE_COLOR touch handler (hit-testing) so the two can't drift
// apart. Full-cell width/height (COLOR_CELL_W, cell_h), not the inset
// swatch drawn inside it, is the tappable area - a bigger target than the
// swatch itself is easier to hit on a 3DS touch screen.
static void color_cell_origin(int index, int cols, float base_y, float cell_h, float *out_x, float *out_y) {
    *out_x = (float)(index % cols) * COLOR_CELL_W;
    *out_y = base_y + (float)(index / cols) * cell_h;
}

static void draw_color_screen(C2D_Font font, C2D_TextBuf dynBuf,
                               C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
    C2D_TargetClear(top, C2D_Color32(0x18, 0x1c, 0x28, 0xFF));
    C2D_SceneBegin(top);

    char title_str[64];
    snprintf(title_str, sizeof(title_str), "Color: %.40s", color_target_name);
    draw_text(font, dynBuf, title_str, 12.0f, 8.0f, 0.5f, 0.55f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
    draw_text(font, dynBuf, "Touch a swatch to apply   B: back", 12.0f, 36.0f, 0.5f, 0.42f,
        C2D_Color32(0x9f, 0xd8, 0xff, 0xFF));

    C2D_TargetClear(bottom, C2D_Color32(0x10, 0x12, 0x18, 0xFF));
    C2D_SceneBegin(bottom);

    C2D_DrawRectSolid(0.0f, 0.0f, 0.4f, 320.0f, COLOR_TOP_Y, C2D_Color32(0x28, 0x2c, 0x38, 0xFF));
    draw_text(font, dynBuf, "Colors", 6.0f, 4.0f, 0.5f, 0.4f, C2D_Color32(0xCC, 0xCC, 0xCC, 0xFF));

    float y = COLOR_TOP_Y;
    if (color_target_supports_color) {
        for (size_t i = 0; i < NUM_COLOR_PRESETS; i++) {
            float cx, cy;
            color_cell_origin((int)i, COLOR_GRID_COLS, COLOR_TOP_Y, COLOR_CELL_H, &cx, &cy);
            C2D_DrawRectSolid(cx + 4.0f, cy + 4.0f, 0.42f, COLOR_CELL_W - 8.0f, COLOR_CELL_H - 8.0f,
                C2D_Color32((u8)COLOR_PRESETS[i].r, (u8)COLOR_PRESETS[i].g, (u8)COLOR_PRESETS[i].b, 0xFF));
        }
        y = COLOR_TOP_Y + (float)COLOR_GRID_ROWS * COLOR_CELL_H;
    }
    if (color_target_supports_color_temp) {
        for (size_t i = 0; i < NUM_TEMP_PRESETS; i++) {
            float cx, cy;
            color_cell_origin((int)i, (int)NUM_TEMP_PRESETS, y, COLOR_TEMP_CELL_H, &cx, &cy);
            C2D_DrawRectSolid(cx + 4.0f, cy + 4.0f, 0.42f, COLOR_CELL_W - 8.0f, COLOR_TEMP_CELL_H - 12.0f,
                C2D_Color32((u8)TEMP_PRESETS[i].r, (u8)TEMP_PRESETS[i].g, (u8)TEMP_PRESETS[i].b, 0xFF));
            draw_text(font, dynBuf, TEMP_PRESETS[i].label, cx + 6.0f, cy + COLOR_TEMP_CELL_H - 14.0f,
                0.43f, 0.34f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
        }
    }
}

// Opens the system software keyboard to edit filter_text. Blocking (takes
// over the screen until the user confirms/cancels) - fine to call directly
// from the main thread's input handling, same as HOME menu suspension.
static void open_filter_keyboard(void) {
    SwkbdState swkbd;
    char buf[FILTER_MAX_LEN];
    strncpy(buf, filter_text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetInitialText(&swkbd, buf);
    swkbdSetHintText(&swkbd, "Filter by name (blank = show all)");
    SwkbdButton button = swkbdInputText(&swkbd, buf, sizeof(buf));
    LOG("filter keyboard closed: button=%d text=%s", button, buf);

    if (button == SWKBD_BUTTON_RIGHT) {
        // entities[] is unchanged here, so capturing right before the
        // rebuild is safe - only the filter is changing.
        char preserved_id[HA_MAX_ENTITY_ID];
        capture_selected_id(preserved_id, sizeof(preserved_id));

        strncpy(filter_text, buf, sizeof(filter_text) - 1);
        filter_text[sizeof(filter_text) - 1] = '\0';
        rebuild_visible_list(preserved_id);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    dbg_log_init();
    LOG("=== ha3ds starting ===");

    LightLock_Init(&state_lock);

    soc_buffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    LOG("memalign soc_buffer: %s", soc_buffer ? "ok" : "FAILED");
    if (soc_buffer) {
        int soc_result = socInit(soc_buffer, SOC_BUFFERSIZE);
        LOG("socInit result: 0x%08lX", (unsigned long)soc_result);
        if (soc_result == 0) {
            network_ready = 1;
            ha_client_init();
            LOG("ha_client_init done, network_ready=1");
        }
    }

    gfxInitDefault();
    LOG("gfxInitDefault done");
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    LOG("C3D_Init done");
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    LOG("C2D_Init done");
    C2D_Prepare();
    LOG("C2D_Prepare done");

    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    LOG("render targets created: top=%p bottom=%p", (void *)top, (void *)bottom);

    C2D_TextBuf staticBuf = C2D_TextBufNew(1024);
    C2D_TextBuf dynBuf = C2D_TextBufNew(4096);
    LOG("text bufs created: static=%p dyn=%p", (void *)staticBuf, (void *)dynBuf);

    // Query the console's actual region instead of hardcoding CFG_REGION_USA -
    // loading a region's font blob that doesn't match/exist on this console
    // can silently fail (C2D_FontLoadSystem returns NULL), and every
    // C2D_TextFontParse/DrawText call after that "succeeds" while rendering
    // zero glyphs - a blank screen with no error anywhere.
    Result cfgu_res = cfguInit();
    LOG("cfguInit: 0x%08lX", (unsigned long)cfgu_res);
    u8 region = CFG_REGION_USA;
    Result region_res = CFGU_SecureInfoGetRegion(&region);
    LOG("CFGU_SecureInfoGetRegion: 0x%08lX region=%d", (unsigned long)region_res, region);
    if (R_FAILED(region_res)) {
        region = CFG_REGION_USA;
    }

    LOG("loading system font for region %d...", region);
    C2D_Font font = C2D_FontLoadSystem((CFG_Region)region);
    LOG("font load done: font=%p", (void *)font);
    if (!font) {
        LOG("region-specific font load FAILED, retrying with CFG_REGION_USA");
        font = C2D_FontLoadSystem(CFG_REGION_USA);
        LOG("retry font load done: font=%p", (void *)font);
    }

    C2D_Text titleText, instructionsText;
    C2D_TextFontParse(&titleText, font, staticBuf, "Home Assistant Remote");
    LOG("title text parsed");
    C2D_TextOptimize(&titleText);
    LOG("title text optimized");
    C2D_TextFontParse(&instructionsText, font, staticBuf,
        "Touch/A: toggle   Up/Down: move   Y: refresh   X: room   SELECT: sign in\n"
        "Left/Right: dim/temp   L/R: min/max/+-2\xC2\xB0   B: color   START: settings");
    C2D_TextOptimize(&instructionsText);
    LOG("instructions text parsed+optimized");

    // --- Sign-in bootstrap ------------------------------------------------
    // Saved config -> silently refresh the access token. Missing config or
    // rejected refresh token -> the sign-in form takes over the render loop
    // from the very first frame. All of this happens before any worker
    // thread exists, so it can touch auth state freely.
    if (network_ready) {
        if (app_config_load(&app_cfg) == 0) {
            LOG("config loaded for %s, refreshing token...", app_cfg.base_url);
            ha_client_set_base_url(app_cfg.base_url);
            draw_status_frame(font, dynBuf, top, bottom, "Signing in...", app_cfg.base_url);
            if (ensure_access_token() == 0) {
                signed_in = 1;
            } else {
                LOG("stored refresh token rejected, opening sign-in form");
            }
        }
        if (!signed_in) {
            // No prior working session to fall back to yet - no Cancel button.
            signin_enter(0);
        }
    } else {
        snprintf(status_msg, sizeof(status_msg), "No network (soc init failed)");
    }
    ha_client_set_enabled_domains(app_cfg.enabled_domains);

    if (signed_in) {
        LOG("kicking off initial background refresh...");
        start_worker(OP_REFRESH, NULL, 0, 0.0f, NULL, 0);
    }

    int first_frame = 1;
    int touch_was_down = 0;
    int anim_frame = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        anim_frame++;

        poll_worker();

        // KEY_TOUCH is documented as "Not actually provided by HID" - it's
        // never set by hidKeysDown()/hidKeysHeld(). Detect taps by
        // edge-triggering on touch position instead (untouched always
        // reports 0,0, which is effectively never a real touch point).
        touchPosition touch;
        hidTouchRead(&touch);
        int touch_is_down = (touch.px != 0 || touch.py != 0);
        int touch_tapped = touch_is_down && !touch_was_down;

        // HOME menu handles exit itself (aptMainLoop() returns false when
        // the user closes from there) - no in-app exit key needed.
        if (app_mode == APP_MODE_MAIN) {
            if ((kDown & KEY_SELECT) && !network_busy && network_ready) {
                // Re-sign-in (change server or account). The form has its
                // own Cancel button, so opening it is always safe.
                signin_enter(1);
            }
            if ((kDown & KEY_START) && !network_busy) {
                // Blocked while a worker is in flight: it reads g_enabled_domains
                // (via is_supported_domain/ha_fetch_area_map) with no lock, and
                // settings_toggle_domain() writes it straight from the main
                // thread - same race the sign-in path above already guards
                // against with this same check.
                settings_enter();
            }
            if (kDown & KEY_Y) {
                start_worker(OP_REFRESH, NULL, 0, 0.0f, NULL, 0);
            }
            if (kDown & KEY_X) {
                char preserved_id[HA_MAX_ENTITY_ID];
                capture_selected_id(preserved_id, sizeof(preserved_id));
                group_by_room = !group_by_room;
                sort_entities();
                rebuild_visible_list(preserved_id);
                snprintf(status_msg, sizeof(status_msg), group_by_room ? "Grouped by room" : "Sorted by name");
            }
            if (kDown & KEY_DUP) {
                move_selection(-1);
            }
            if (kDown & KEY_DDOWN) {
                move_selection(1);
            }
            int has_selection = (selected_index >= 0 && selected_index < visible_count &&
                visible_indices[selected_index] != ROW_IS_HEADER);
            int selected_entity_idx = has_selection ? visible_indices[selected_index] : -1;

            if ((kDown & KEY_A) && has_selection) {
                start_worker(OP_TOGGLE, entities[selected_entity_idx].entity_id, 0, 0.0f,
                    entities[selected_entity_idx].state, entities[selected_entity_idx].is_group);
            }
            if ((kDown & KEY_B) && has_selection &&
                (entities[selected_entity_idx].supports_color || entities[selected_entity_idx].supports_color_temp)) {
                color_enter(&entities[selected_entity_idx]);
            }
            if ((kDown & (KEY_DLEFT | KEY_DRIGHT)) && has_selection && entities[selected_entity_idx].supports_brightness) {
                int delta = (kDown & KEY_DRIGHT) ? 10 : -10;
                int new_pct = entities[selected_entity_idx].brightness_pct + delta;
                if (new_pct < 0) {
                    new_pct = 0;
                }
                if (new_pct > 100) {
                    new_pct = 100;
                }
                start_worker(OP_SET_BRIGHTNESS, entities[selected_entity_idx].entity_id, new_pct, 0.0f, NULL, 0);
            }
            if ((kDown & (KEY_L | KEY_R)) && has_selection && entities[selected_entity_idx].supports_brightness) {
                // 1% rather than 0 for "min" - HA treats brightness_pct 0 as
                // ambiguous with turning the light off on some integrations.
                int target_pct = (kDown & KEY_R) ? 100 : 1;
                start_worker(OP_SET_BRIGHTNESS, entities[selected_entity_idx].entity_id, target_pct, 0.0f, NULL, 0);
            }
            if ((kDown & (KEY_DLEFT | KEY_DRIGHT)) && has_selection && entities[selected_entity_idx].is_climate) {
                float delta = (kDown & KEY_DRIGHT) ? 0.5f : -0.5f;
                start_worker(OP_SET_TEMPERATURE, entities[selected_entity_idx].entity_id, 0,
                    entities[selected_entity_idx].target_temp + delta, NULL, 0);
            }
            if ((kDown & (KEY_L | KEY_R)) && has_selection && entities[selected_entity_idx].is_climate) {
                float delta = (kDown & KEY_R) ? 2.0f : -2.0f;
                start_worker(OP_SET_TEMPERATURE, entities[selected_entity_idx].entity_id, 0,
                    entities[selected_entity_idx].target_temp + delta, NULL, 0);
            }

            if (touch_tapped) {
                float top_y = list_top_y();
                if (touch.py < FILTER_BOX_HEIGHT) {
                    open_filter_keyboard();
                } else if (touch.py >= top_y) {
                    int row = (int)((touch.py - top_y) / ROW_HEIGHT);
                    int pos = scroll_offset + row;
                    if (row < VISIBLE_ROWS && pos >= 0 && pos < visible_count &&
                        visible_indices[pos] != ROW_IS_HEADER) {
                        selected_index = pos;
                        start_worker(OP_TOGGLE, entities[visible_indices[pos]].entity_id, 0, 0.0f,
                            entities[visible_indices[pos]].state, entities[visible_indices[pos]].is_group);
                    }
                }
                // else: tap landed on the sticky room bar - no-op, same as
                // tapping a header row.
            }
        } else if (app_mode == APP_MODE_SETTINGS) {
            if (kDown & KEY_DUP) {
                settings_cursor = (settings_cursor - 1 + HA_NUM_DOMAINS) % HA_NUM_DOMAINS;
            }
            if (kDown & KEY_DDOWN) {
                settings_cursor = (settings_cursor + 1) % HA_NUM_DOMAINS;
            }
            if (kDown & KEY_A) {
                settings_toggle_domain(settings_cursor);
            }
            if (kDown & (KEY_B | KEY_START)) {
                settings_exit();
            }
            if (touch_tapped && touch.py >= SETTINGS_TOP_Y) {
                int row = (int)((touch.py - SETTINGS_TOP_Y) / SETTINGS_ROW_H);
                if (row >= 0 && row < HA_NUM_DOMAINS) {
                    settings_cursor = row;
                    settings_toggle_domain(row);
                }
            }
        } else if (app_mode == APP_MODE_COLOR) {
            if (kDown & (KEY_B | KEY_START)) {
                color_exit();
            }
            if (touch_tapped && touch.py >= COLOR_TOP_Y) {
                float temp_top_y = COLOR_TOP_Y + (color_target_supports_color ? (float)COLOR_GRID_ROWS * COLOR_CELL_H : 0.0f);
                if (color_target_supports_color && touch.py < temp_top_y) {
                    // Clamp col: a tap right at the panel's physical edge can
                    // report touch.px == 320, which divides out to
                    // COLOR_GRID_COLS (one past the last column) - without
                    // this it would silently fall into the row below instead
                    // of being rejected or landing on the rightmost swatch.
                    int col = (int)(touch.px / COLOR_CELL_W);
                    int row = (int)((touch.py - COLOR_TOP_Y) / COLOR_CELL_H);
                    if (col >= 0 && col < COLOR_GRID_COLS) {
                        size_t idx = (size_t)(row * COLOR_GRID_COLS + col);
                        if (idx < NUM_COLOR_PRESETS) {
                            int packed = (COLOR_PRESETS[idx].r << 16) | (COLOR_PRESETS[idx].g << 8) | COLOR_PRESETS[idx].b;
                            start_worker(OP_SET_COLOR, color_target_entity_id, packed, 0.0f, NULL, 0);
                        }
                    }
                } else if (color_target_supports_color_temp && touch.py >= temp_top_y &&
                           touch.py < temp_top_y + COLOR_TEMP_CELL_H) {
                    size_t idx = (size_t)(touch.px / COLOR_CELL_W);
                    if (idx < NUM_TEMP_PRESETS) {
                        start_worker(OP_SET_COLOR_TEMP, color_target_entity_id, TEMP_PRESETS[idx].kelvin, 0.0f, NULL, 0);
                    }
                }
            }
        } else { // APP_MODE_SIGNIN
            signin_build_active_fields();

            if (kDown & KEY_DUP) {
                signin_cursor = (signin_cursor - 1 + signin_active_count) % signin_active_count;
            }
            if (kDown & KEY_DDOWN) {
                signin_cursor = (signin_cursor + 1) % signin_active_count;
            }
            if (kDown & KEY_A) {
                signin_activate(signin_active_fields[signin_cursor], font, dynBuf, top, bottom);
            }

            if (touch_tapped) {
                for (int i = 0; i < signin_active_count; i++) {
                    int kind = signin_active_fields[i];
                    float y = signin_field_y(kind);
                    float h = (kind == SF_SIGNIN || kind == SF_CANCEL) ? SIGNIN_BTN_H : SIGNIN_FIELD_H;
                    if (touch.px >= 4 && touch.px <= 316 && touch.py >= y && touch.py < y + h) {
                        signin_cursor = i;
                        signin_activate(kind, font, dynBuf, top, bottom);
                        break;
                    }
                }
            }
        }
        touch_was_down = touch_is_down;

        C2D_TextBufClear(dynBuf);

        if (first_frame) LOG("frame 1: entering C3D_FrameBegin");
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        if (first_frame) LOG("frame 1: C3D_FrameBegin returned");

        if (app_mode == APP_MODE_SIGNIN) {
            draw_signin_screen(font, dynBuf, top, bottom);
        } else if (app_mode == APP_MODE_SETTINGS) {
            draw_settings_screen(font, dynBuf, top, bottom);
        } else if (app_mode == APP_MODE_COLOR) {
            draw_color_screen(font, dynBuf, top, bottom);
        } else {
        C2D_TargetClear(top, C2D_Color32(0x18, 0x1c, 0x28, 0xFF));
        C2D_SceneBegin(top);
        if (first_frame) LOG("frame 1: top cleared, drawing title text");

        C2D_DrawText(&titleText, C2D_WithColor, 12.0f, 8.0f, 0.5f, 0.6f, 0.6f,
            C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
        if (first_frame) LOG("frame 1: title text drawn");

        C2D_Text statusText;
        C2D_TextFontParse(&statusText, font, dynBuf, status_msg);
        C2D_TextOptimize(&statusText);
        C2D_DrawText(&statusText, C2D_WithColor, 12.0f, 36.0f, 0.5f, 0.5f, 0.5f,
            C2D_Color32(0x9f, 0xd8, 0xff, 0xFF));
        if (first_frame) LOG("frame 1: status text drawn");

        if (network_busy) {
            draw_spinner(380.0f, 20.0f, 8.0f, anim_frame);
        }

        C2D_DrawText(&instructionsText, C2D_WithColor, 12.0f, 196.0f, 0.5f, 0.4f, 0.4f,
            C2D_Color32(0x88, 0x88, 0x88, 0xFF));
        if (first_frame) LOG("frame 1: instructions text drawn");

        C2D_TargetClear(bottom, C2D_Color32(0x10, 0x12, 0x18, 0xFF));
        C2D_SceneBegin(bottom);
        if (first_frame) LOG("frame 1: bottom cleared, entering entity list loop (visible=%d)", visible_count);

        // Floating filter box - always visible, tap to open the keyboard.
        C2D_DrawRectSolid(0.0f, 0.0f, 0.4f, 320.0f, (float)FILTER_BOX_HEIGHT, C2D_Color32(0x28, 0x2c, 0x38, 0xFF));
        char filter_display[48];
        if (filter_text[0]) {
            snprintf(filter_display, sizeof(filter_display), "Filter: %s  (%d/%d)", filter_text, visible_entity_count, entity_count);
        } else {
            snprintf(filter_display, sizeof(filter_display), "Tap to filter list...");
        }
        draw_text(font, dynBuf, filter_display, 6.0f, 4.0f, 0.5f, 0.4f,
            C2D_Color32(0xCC, 0xCC, 0xCC, 0xFF));

        // Persistent "current room" bar: only drawn when scrolled mid-group
        // (see sticky_room_bar_active()) so a long room's name stays on
        // screen even once its own header row has scrolled out of view.
        if (sticky_room_bar_active()) {
            const ha_entity_t *top_entity = &entities[visible_indices[scroll_offset]];
            C2D_DrawRectSolid(0.0f, (float)FILTER_BOX_HEIGHT, 0.4f, 320.0f, STICKY_ROOM_BAR_HEIGHT,
                C2D_Color32(0x2a, 0x22, 0x0e, 0xFF));
            draw_text(font, dynBuf, entity_area_label(top_entity), 8.0f, (float)FILTER_BOX_HEIGHT + 2.0f,
                0.5f, 0.36f, C2D_Color32(0xFF, 0xB3, 0x4D, 0xFF));
        }

        float list_y = list_top_y();
        for (int i = 0; i < VISIBLE_ROWS; i++) {
            int pos = scroll_offset + i;
            if (pos >= visible_count) {
                break;
            }
            int idx = visible_indices[pos];
            float y = list_y + i * ROW_HEIGHT;

            if (idx == ROW_IS_HEADER) {
                // Section header: a distinct full-width band naming the
                // room the row(s) below belong to, so grouping reads as
                // real sections rather than a same-list resort. Its text
                // comes from the entity right after it, since a header is
                // only ever emitted immediately before the first entity of
                // a new area (see rebuild_visible_list). Accent goes on the
                // left edge, not a line flush along the top - a top line on
                // the very first header sits right on the seam with the
                // filter box above it and reads as the header overlapping/
                // peeking out from underneath it.
                C2D_DrawRectSolid(0.0f, y, 0.4f, 320.0f, (float)ROW_HEIGHT, C2D_Color32(0x3a, 0x2e, 0x12, 0xFF));
                C2D_DrawRectSolid(0.0f, y, 0.41f, 4.0f, (float)ROW_HEIGHT, C2D_Color32(0xFF, 0xB3, 0x4D, 0xFF));

                const char *area = AREA_UNASSIGNED_LABEL;
                if (pos + 1 < visible_count && visible_indices[pos + 1] != ROW_IS_HEADER) {
                    const ha_entity_t *next = &entities[visible_indices[pos + 1]];
                    area = entity_area_label(next);
                }
                char header_display[40];
                snprintf(header_display, sizeof(header_display), "%.36s", area);

                draw_text(font, dynBuf, header_display, 8.0f, y + 6.0f, 0.5f, 0.44f,
                    C2D_Color32(0xFF, 0xB3, 0x4D, 0xFF));
                continue;
            }

            int is_selected = (pos == selected_index);
            u32 rowColor;
            if (is_selected) {
                rowColor = C2D_Color32(0x2f, 0x6f, 0xdf, 0xFF);
            } else {
                rowColor = (i % 2 == 0) ? C2D_Color32(0x20, 0x24, 0x30, 0xFF) : C2D_Color32(0x18, 0x1a, 0x24, 0xFF);
            }
            C2D_DrawRectSolid(0.0f, y, 0.4f, 320.0f, (float)ROW_HEIGHT, rowColor);
            if (is_selected) {
                // Left accent bar makes the selection readable even for
                // colorblind users who might not distinguish the fill tint.
                C2D_DrawRectSolid(0.0f, y, 0.41f, 4.0f, (float)ROW_HEIGHT, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
            }

            draw_domain_icon(entities[idx].entity_id, 4.0f, y + 3.0f, entities[idx].is_group);

            char display_name[48];
            snprintf(display_name, sizeof(display_name), "%.32s", entities[idx].friendly_name);

            draw_text(font, dynBuf, display_name, 28.0f, y + 4.0f, 0.5f, 0.42f,
                C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

            int is_on = (strcmp(entities[idx].state, "on") == 0);
            int is_climate_off = entities[idx].is_climate && strcmp(entities[idx].state, "off") == 0;
            u32 stateColor = is_climate_off ? C2D_Color32(0x88, 0x88, 0x88, 0xFF)
                : entities[idx].is_climate ? C2D_Color32(0xFF, 0x99, 0x55, 0xFF)
                : is_on ? C2D_Color32(0x4c, 0xd9, 0x64, 0xFF)
                : C2D_Color32(0x88, 0x88, 0x88, 0xFF);

            char state_display[24];
            if (entities[idx].is_climate) {
                // Not every climate entity reports a live sensor reading -
                // fall back to just the target when current is unknown.
                if (entities[idx].current_temp != 0.0f) {
                    snprintf(state_display, sizeof(state_display), "%.0f\xC2\xB0->%.0f\xC2\xB0",
                        (double)entities[idx].current_temp, (double)entities[idx].target_temp);
                } else {
                    snprintf(state_display, sizeof(state_display), "%s %.0f\xC2\xB0",
                        entities[idx].state, (double)entities[idx].target_temp);
                }
            } else if (is_on && entities[idx].supports_brightness) {
                snprintf(state_display, sizeof(state_display), "on %d%%", entities[idx].brightness_pct);
            } else {
                snprintf(state_display, sizeof(state_display), "%s", entities[idx].state);
            }

            draw_text(font, dynBuf, state_display, 235.0f, y + 4.0f, 0.5f, 0.42f, stateColor);
        }
        } // app_mode == APP_MODE_MAIN

        if (first_frame) LOG("frame 1: entering C3D_FrameEnd");
        C3D_FrameEnd(0);
        if (first_frame) {
            LOG("frame 1: C3D_FrameEnd returned - first frame complete");
            first_frame = 0;
        }
    }

    LOG("exited main loop cleanly");

    // Don't tear down the network stack out from under a live worker thread
    // if the user backed out via HOME while a request was still in flight.
    if (network_busy && worker_thread) {
        LOG("waiting for in-flight worker thread before shutdown...");
        threadJoin(worker_thread, U64_MAX);
        threadFree(worker_thread);
        worker_thread = NULL;
        LOG("worker thread joined");
    }

    C2D_FontFree(font);
    C2D_TextBufDelete(dynBuf);
    C2D_TextBufDelete(staticBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    cfguExit();

    if (network_ready) {
        ha_client_exit();
        socExit();
    }
    free(soc_buffer);

    LOG("clean exit");
    dbg_log_close();

    return 0;
}
