#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <malloc.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#include "ha_client.h"
#include "dbglog.h"

// libctru requires this to be at least 0x100000 - a smaller buffer lets
// socInit "succeed" but leaves soc:U in a bad state that can crash later
// instead of failing cleanly.
#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000
#define MAX_ENTITIES 64
#define VISIBLE_ROWS 8
#define ROW_HEIGHT 24
#define FILTER_BOX_HEIGHT 22
#define LIST_TOP_Y FILTER_BOX_HEIGHT
#define FILTER_MAX_LEN 32

static u32 *soc_buffer = NULL;
static int network_ready = 0;
static ha_entity_t entities[MAX_ENTITIES];
static int entity_count = 0;
static int scroll_offset = 0;
static int selected_index = 0;
static char status_msg[128] = "Connecting...";

// Entities matching filter_text (case-insensitive substring of name or
// entity_id), in original order. selected_index/scroll_offset are positions
// into THIS list, not directly into `entities` - always go through
// visible_indices[] to get the real entities[] index.
static char filter_text[FILTER_MAX_LEN] = "";
static int visible_indices[MAX_ENTITIES];
static int visible_count = 0;

#define LOG(...) dbg_log(__VA_ARGS__)

// --- Background network worker -------------------------------------------
// ha_fetch_states()/ha_toggle_entity() are blocking HTTPS calls that can
// take several seconds over the internet; running them on the main thread
// froze rendering for the whole call. All network work now happens on a
// dedicated worker thread; `entities`/`entity_count`/`status_msg`/
// `selected_index` are only ever touched by the main thread, and the
// pending_* staging fields (written by the worker, read by the main thread)
// are guarded by state_lock.
#define WORKER_STACK_SIZE (32 * 1024)

static LightLock state_lock;
static Thread worker_thread = NULL;
static volatile bool network_busy = false;
static volatile bool worker_result_ready = false;

enum {
    OP_REFRESH = 0,
    OP_TOGGLE = 1,
    OP_SET_BRIGHTNESS = 2,
};

static ha_entity_t pending_entities[MAX_ENTITIES];
static int pending_count = 0;
static char pending_status[128];
static char pending_entity_id[HA_MAX_ENTITY_ID];
static int pending_op = OP_REFRESH;
static int pending_brightness_pct = 0;

// After a toggle/brightness change, only that one entity actually changed -
// the worker fetches just it (ha_fetch_single_state) instead of the full
// (~100KB) state list, and poll_worker() patches it in place below.
static ha_entity_t pending_single_entity;
static int pending_single_valid = 0;

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
    if (selected_index >= 0 && selected_index < visible_count) {
        strncpy(buf, entities[visible_indices[selected_index]].entity_id, bufsize - 1);
        buf[bufsize - 1] = '\0';
    }
}

// Recomputes visible_indices[] from entities[]/filter_text, re-selecting
// preserved_id (captured by the caller via capture_selected_id) if it's
// still in the filtered list, else resetting to row 0. The caller captures
// the id rather than this function doing it because entities[] may have
// been replaced (fresh fetch, possibly reordered) between capture and
// rebuild - indexing the new array through the old visible_indices[] would
// preserve the wrong entity.
static void rebuild_visible_list(const char *preserved_id) {
    visible_count = 0;
    for (int i = 0; i < entity_count; i++) {
        if (str_contains_ci(entities[i].friendly_name, filter_text) ||
            str_contains_ci(entities[i].entity_id, filter_text)) {
            visible_indices[visible_count++] = i;
        }
    }

    selected_index = 0;
    if (preserved_id[0]) {
        for (int i = 0; i < visible_count; i++) {
            if (strcmp(entities[visible_indices[i]].entity_id, preserved_id) == 0) {
                selected_index = i;
                break;
            }
        }
    }

    clamp_selection();
}

static void worker_thread_func(void *arg) {
    (void)arg;

    if (!network_ready) {
        LightLock_Lock(&state_lock);
        pending_single_valid = 0;
        pending_count = 0;
        snprintf(pending_status, sizeof(pending_status), "No network (soc init failed)");
        worker_result_ready = true;
        LightLock_Unlock(&state_lock);
        return;
    }

    if (pending_op == OP_TOGGLE) {
        LOG("worker: toggling %s", pending_entity_id);
        ha_toggle_entity(pending_entity_id);
    } else if (pending_op == OP_SET_BRIGHTNESS) {
        LOG("worker: setting brightness %s -> %d%%", pending_entity_id, pending_brightness_pct);
        ha_set_brightness(pending_entity_id, pending_brightness_pct);
    }

    if (pending_op == OP_TOGGLE || pending_op == OP_SET_BRIGHTNESS) {
        ha_entity_t updated;
        int ok = ha_fetch_single_state(pending_entity_id, &updated) == 0;
        LOG("worker: single-entity refresh ok=%d", ok);

        LightLock_Lock(&state_lock);
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

    LightLock_Lock(&state_lock);
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

// Kicks off a refresh, toggle-then-refresh, or set-brightness-then-refresh
// on the worker thread. No-ops if one is already running - only one network
// op is ever in flight at a time.
static void start_worker(int op, const char *entity_id, int brightness_pct) {
    if (network_busy) {
        return;
    }

    pending_op = op;
    if (op == OP_TOGGLE || op == OP_SET_BRIGHTNESS) {
        strncpy(pending_entity_id, entity_id, sizeof(pending_entity_id) - 1);
        pending_entity_id[sizeof(pending_entity_id) - 1] = '\0';
    }
    if (op == OP_SET_BRIGHTNESS) {
        pending_brightness_pct = brightness_pct;
    }

    network_busy = true;
    worker_result_ready = false;
    const char *msg = (op == OP_TOGGLE) ? "Toggling..." : (op == OP_SET_BRIGHTNESS) ? "Adjusting brightness..." : "Refreshing...";
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
    if (pending_op == OP_REFRESH) {
        entity_count = pending_count;
        memcpy(entities, pending_entities, sizeof(ha_entity_t) * (size_t)pending_count);
    } else if (pending_single_valid) {
        // Patch just the one entity that changed, in place, wherever it
        // currently sits in the list - a toggle/dim never adds or removes
        // rows, so entity_count/ordering are untouched.
        for (int i = 0; i < entity_count; i++) {
            if (strcmp(entities[i].entity_id, pending_single_entity.entity_id) == 0) {
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

static void draw_domain_icon(const char *entity_id, float box_x, float box_y) {
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
        "Touch/A: toggle   Up/Down: move   Y: refresh\n"
        "Left/Right: dim +-10%   L/R: dim min/max");
    C2D_TextOptimize(&instructionsText);
    LOG("instructions text parsed+optimized");

    LOG("kicking off initial background refresh...");
    start_worker(OP_REFRESH, NULL, 0);

    int first_frame = 1;
    int touch_was_down = 0;
    int anim_frame = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        anim_frame++;

        poll_worker();

        // HOME menu handles exit itself (aptMainLoop() returns false when
        // the user closes from there) - no in-app exit key needed.
        if (kDown & KEY_Y) {
            start_worker(OP_REFRESH, NULL, 0);
        }
        if (kDown & KEY_DUP) {
            selected_index--;
            clamp_selection();
        }
        if (kDown & KEY_DDOWN) {
            selected_index++;
            clamp_selection();
        }
        int has_selection = (selected_index >= 0 && selected_index < visible_count);
        int selected_entity_idx = has_selection ? visible_indices[selected_index] : -1;

        if ((kDown & KEY_A) && has_selection) {
            start_worker(OP_TOGGLE, entities[selected_entity_idx].entity_id, 0);
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
            start_worker(OP_SET_BRIGHTNESS, entities[selected_entity_idx].entity_id, new_pct);
        }
        if ((kDown & (KEY_L | KEY_R)) && has_selection && entities[selected_entity_idx].supports_brightness) {
            // 1% rather than 0 for "min" - HA treats brightness_pct 0 as
            // ambiguous with turning the light off on some integrations.
            int target_pct = (kDown & KEY_R) ? 100 : 1;
            start_worker(OP_SET_BRIGHTNESS, entities[selected_entity_idx].entity_id, target_pct);
        }

        // KEY_TOUCH is documented as "Not actually provided by HID" - it's
        // never set by hidKeysDown()/hidKeysHeld(). Detect taps by
        // edge-triggering on touch position instead (untouched always
        // reports 0,0, which is effectively never a real touch point).
        touchPosition touch;
        hidTouchRead(&touch);
        int touch_is_down = (touch.px != 0 || touch.py != 0);
        if (touch_is_down && !touch_was_down) {
            if (touch.py < LIST_TOP_Y) {
                open_filter_keyboard();
            } else {
                int row = (touch.py - LIST_TOP_Y) / ROW_HEIGHT;
                int pos = scroll_offset + row;
                if (row < VISIBLE_ROWS && pos >= 0 && pos < visible_count) {
                    selected_index = pos;
                    start_worker(OP_TOGGLE, entities[visible_indices[pos]].entity_id, 0);
                }
            }
        }
        touch_was_down = touch_is_down;

        C2D_TextBufClear(dynBuf);

        if (first_frame) LOG("frame 1: entering C3D_FrameBegin");
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        if (first_frame) LOG("frame 1: C3D_FrameBegin returned");

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
            snprintf(filter_display, sizeof(filter_display), "Filter: %s  (%d/%d)", filter_text, visible_count, entity_count);
        } else {
            snprintf(filter_display, sizeof(filter_display), "Tap to filter list...");
        }
        C2D_Text filterTextObj;
        C2D_TextFontParse(&filterTextObj, font, dynBuf, filter_display);
        C2D_TextOptimize(&filterTextObj);
        C2D_DrawText(&filterTextObj, C2D_WithColor, 6.0f, 4.0f, 0.5f, 0.4f, 0.4f,
            C2D_Color32(0xCC, 0xCC, 0xCC, 0xFF));

        for (int i = 0; i < VISIBLE_ROWS; i++) {
            int pos = scroll_offset + i;
            if (pos >= visible_count) {
                break;
            }
            int idx = visible_indices[pos];

            float y = LIST_TOP_Y + i * ROW_HEIGHT;
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

            draw_domain_icon(entities[idx].entity_id, 4.0f, y + 3.0f);

            char display_name[36];
            snprintf(display_name, sizeof(display_name), "%.32s", entities[idx].friendly_name);

            C2D_Text nameText, stateText;
            C2D_TextFontParse(&nameText, font, dynBuf, display_name);
            C2D_TextOptimize(&nameText);
            C2D_DrawText(&nameText, C2D_WithColor, 28.0f, y + 4.0f, 0.5f, 0.42f, 0.42f,
                C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

            int is_on = (strcmp(entities[idx].state, "on") == 0);
            u32 stateColor = is_on ? C2D_Color32(0x4c, 0xd9, 0x64, 0xFF) : C2D_Color32(0x88, 0x88, 0x88, 0xFF);

            char state_display[24];
            if (is_on && entities[idx].supports_brightness) {
                snprintf(state_display, sizeof(state_display), "on %d%%", entities[idx].brightness_pct);
            } else {
                snprintf(state_display, sizeof(state_display), "%s", entities[idx].state);
            }

            C2D_TextFontParse(&stateText, font, dynBuf, state_display);
            C2D_TextOptimize(&stateText);
            C2D_DrawText(&stateText, C2D_WithColor, 235.0f, y + 4.0f, 0.5f, 0.42f, 0.42f, stateColor);
        }

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
