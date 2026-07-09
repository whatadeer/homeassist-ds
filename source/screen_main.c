// --- Main entity list screen ------------------------------------------------
// The default screen: a floating filter box, an optional sticky room bar,
// and the scrollable entity list on the bottom screen; a "now selected"
// hero card (big tinted icon, name, room, live state) on the top screen.
#include "screen_main.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "dbglog.h"
#include "domain_icons.h"
#include "entity_list.h"
#include "ha_client.h"
#include "ha_worker.h"
#include "screen_color.h"
#include "screen_settings.h"
#include "screen_signin.h"
#include "ui_common.h"

// Circle Pad list scrolling (see the input-handling CPAD_* block below).
// libctru's circlePosition axes run roughly -156..156; CPAD_DEADZONE ignores
// drift near center, CPAD_MAX is where auto-repeat maxes out at
// 60/CPAD_SCROLL_INTERVAL_MIN steps/sec (~2/sec at CPAD_SCROLL_INTERVAL_MAX
// for a barely-past-deadzone nudge).
#define CPAD_DEADZONE 24.0f
#define CPAD_MAX 150.0f
#define CPAD_SCROLL_INTERVAL_MAX 30
#define CPAD_SCROLL_INTERVAL_MIN 3

// Frames left before the Circle Pad's next auto-repeat move_selection()
// step - see the CPAD_* handling below. 0 means "step on the next held
// frame", which is also its released-state reset so a fresh push always
// steps immediately instead of waiting out a stale interval.
static int cpad_scroll_timer = 0;

static C2D_Text instructionsText;

void screen_main_init(C2D_Font font, C2D_TextBuf staticBuf) {
    C2D_TextFontParse(&instructionsText, font, staticBuf,
        "Touch/A: toggle   Up/Down/Stick: move   Y: refresh   X: group   SELECT: sign in\n"
        "Left/Right: dim/temp   L/R: min/max/+-2\xC2\xB0   B: color   START: settings");
    C2D_TextOptimize(&instructionsText);
}

// Formats an entity's state text and its color the same way everywhere it's
// shown (entity list row, top-screen hero) - climate shows current->target
// (or just target if the entity never reported a live reading), dimmable
// lights that are on show their brightness percent, everything else shows
// the raw state string.
//
// GCC's -Wformat-truncation sizes %.0f's worst case off float's full range
// (a value near FLT_MAX printed as %f is ~300+ digits), not off what a
// Home Assistant target_temp actually holds, so it flags this snprintf as
// possibly truncating no matter how big out_size is. snprintf itself is
// unconditionally safe here (it always bounds and null-terminates); the
// diagnostic is suppressed only for this function, not project-wide.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void format_entity_state(const ha_entity_t *e, char *out, size_t out_size, u32 *color) {
    int is_on = (strcmp(e->state, "on") == 0);
    int is_climate_off = e->is_climate && strcmp(e->state, "off") == 0;
    *color = is_climate_off ? C2D_Color32(0x88, 0x88, 0x88, 0xFF)
        : e->is_climate ? C2D_Color32(0xFF, 0x99, 0x55, 0xFF)
        : is_on ? C2D_Color32(0x4c, 0xd9, 0x64, 0xFF)
        : C2D_Color32(0x88, 0x88, 0x88, 0xFF);

    if (e->is_climate) {
        if (e->current_temp != 0.0f) {
            snprintf(out, out_size, "%.0f\xC2\xB0->%.0f\xC2\xB0", (double)e->current_temp, (double)e->target_temp);
        } else {
            snprintf(out, out_size, "%s %.0f\xC2\xB0", e->state, (double)e->target_temp);
        }
    } else if (is_on && e->supports_brightness) {
        snprintf(out, out_size, "on %d%%", e->brightness_pct);
    } else {
        snprintf(out, out_size, "%s", e->state);
    }
}
#pragma GCC diagnostic pop

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

void screen_main_handle_input(u32 kDown, int touch_tapped, touchPosition touch) {
    if ((kDown & KEY_SELECT) && !network_busy && network_ready) {
        // Re-sign-in (change server or account). The form has its own
        // Cancel button, so opening it is always safe.
        screen_signin_enter(1);
    }
    if ((kDown & KEY_START) && !network_busy) {
        // Blocked while a worker is in flight: it reads g_enabled_domains
        // (via is_supported_domain/ha_fetch_area_map) with no lock, and
        // settings_toggle_domain() writes it straight from the main
        // thread - same race the sign-in path above already guards
        // against with this same check.
        screen_settings_enter();
    }
    if (kDown & KEY_Y) {
        start_worker(OP_REFRESH, NULL, 0, 0.0f, NULL, 0);
    }
    if (kDown & KEY_X) {
        char preserved_id[HA_MAX_ENTITY_ID];
        capture_selected_id(preserved_id, sizeof(preserved_id));
        group_mode = (group_mode + 1) % (GROUP_BY_STATUS + 1);
        sort_entities();
        rebuild_visible_list(preserved_id);
        const char *msg = group_mode == GROUP_BY_ROOM ? "Grouped by room"
            : group_mode == GROUP_BY_STATUS ? "Grouped by status"
            : "Sorted by name";
        snprintf(status_msg, sizeof(status_msg), "%s", msg);
    }
    if (kDown & KEY_DUP) {
        move_selection(-1);
    }
    if (kDown & KEY_DDOWN) {
        move_selection(1);
    }

    // Circle Pad: continuous, deflection-speed scrolling alongside the
    // D-Pad's precise one-row-per-press. The further the stick is pushed,
    // the shorter the gap between auto-repeated move_selection() steps - up
    // to CPAD_SCROLL_STEPS_PER_SEC_MAX steps/sec at full deflection, vs. one
    // per manual press.
    circlePosition cpad;
    hidCircleRead(&cpad);
    int cpad_dir = 0;
    if (cpad.dy > CPAD_DEADZONE) {
        cpad_dir = -1;  // pushed up -> same direction as KEY_DUP
    } else if (cpad.dy < -CPAD_DEADZONE) {
        cpad_dir = 1;   // pushed down -> same direction as KEY_DDOWN
    }
    if (cpad_dir != 0) {
        if (cpad_scroll_timer <= 0) {
            move_selection(cpad_dir);
            float mag = fabsf((float)cpad.dy);
            if (mag > CPAD_MAX) mag = CPAD_MAX;
            float t = (mag - CPAD_DEADZONE) / (CPAD_MAX - CPAD_DEADZONE);
            cpad_scroll_timer = (int)(CPAD_SCROLL_INTERVAL_MAX -
                t * (CPAD_SCROLL_INTERVAL_MAX - CPAD_SCROLL_INTERVAL_MIN));
        } else {
            cpad_scroll_timer--;
        }
    } else {
        cpad_scroll_timer = 0;
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
        screen_color_enter(&entities[selected_entity_idx]);
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
}

void screen_main_draw(C2D_Font font, C2D_TextBuf dynBuf,
                       C3D_RenderTarget *top, C3D_RenderTarget *bottom,
                       int anim_frame, int log_first_frame) {
    // The top screen is a "now selected" hero card for whatever row is
    // highlighted below: a big tinted icon, its name, room, and live
    // state. Rendered once per eye with each element at a different
    // stereo_shift() depth - the icon sits closest to the viewer, state
    // close behind it, name mid-ground, room/chrome recede behind the
    // screen plane - so the physical 3D slider reads as real layered
    // depth, not a flat pop-out. At slider==0 every shift is 0 and this
    // is pixel-identical across both eyes.
    const ha_entity_t *hero = current_selected_entity();
    for (int eye = 0; eye < 2; eye++) {
        C3D_RenderTarget *eye_target = eye ? g_top_right : top;
        C2D_TargetClear(eye_target, C2D_Color32(0x18, 0x1c, 0x28, 0xFF));
        C2D_SceneBegin(eye_target);
        if (log_first_frame) LOG("frame 1: top cleared (eye %d), drawing hero", eye);

        C2D_Text statusText;
        C2D_TextFontParse(&statusText, font, dynBuf, status_msg);
        C2D_TextOptimize(&statusText);
        C2D_DrawText(&statusText, C2D_WithColor, 10.0f + stereo_shift(-0.5f, eye), 6.0f, 0.5f, 0.4f, 0.4f,
            C2D_Color32(0x9f, 0xd8, 0xff, 0xFF));

        if (network_busy) {
            draw_spinner(380.0f + stereo_shift(-0.5f, eye), 16.0f, 6.0f, anim_frame);
        }

        if (hero) {
            const pixel_icon_t *icon;
            u8 iconR, iconG, iconB;
            float icon_x = 20.0f + stereo_shift(2.8f, eye);
            if (domain_icon_lookup(hero->entity_id, &icon, &iconR, &iconG, &iconB)) {
                u32 iconColor = domain_tint(iconR, iconG, iconB, entity_is_active(hero));
                draw_pixel_icon(icon, icon_x, 40.0f, HERO_ICON_SIZE, iconColor, 0.5f);
            } else {
                C2D_DrawRectSolid(icon_x + 8.0f, 48.0f, 0.5f, HERO_ICON_SIZE - 16.0f, HERO_ICON_SIZE - 16.0f,
                    C2D_Color32(0x77, 0x77, 0x77, 0xFF));
            }

            char hero_name[32];
            snprintf(hero_name, sizeof(hero_name), "%.24s", hero->friendly_name);
            draw_text(font, dynBuf, hero_name, 128.0f + stereo_shift(1.0f, eye), 54.0f, 0.5f, 0.62f,
                C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

            draw_text(font, dynBuf, entity_area_label(hero), 128.0f + stereo_shift(-0.2f, eye), 88.0f,
                0.5f, 0.4f, C2D_Color32(0x88, 0x88, 0x88, 0xFF));

            char hero_state[32];
            u32 stateColor;
            format_entity_state(hero, hero_state, sizeof(hero_state), &stateColor);
            draw_text(font, dynBuf, hero_state, 128.0f + stereo_shift(1.3f, eye), 112.0f, 0.5f, 0.6f, stateColor);
        } else {
            draw_text(font, dynBuf, "No entities", 20.0f + stereo_shift(1.0f, eye), 70.0f, 0.5f, 0.6f,
                C2D_Color32(0x88, 0x88, 0x88, 0xFF));
        }

        C2D_DrawText(&instructionsText, C2D_WithColor, 12.0f + stereo_shift(-0.8f, eye), 196.0f, 0.5f, 0.4f, 0.4f,
            C2D_Color32(0x88, 0x88, 0x88, 0xFF));
        if (log_first_frame) LOG("frame 1: hero drawn");
    }

    C2D_TargetClear(bottom, C2D_Color32(0x10, 0x12, 0x18, 0xFF));
    C2D_SceneBegin(bottom);
    if (log_first_frame) LOG("frame 1: bottom cleared, entering entity list loop (visible=%d)", visible_count);

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

    // Persistent "current group" bar: only drawn when scrolled mid-group
    // (see sticky_group_bar_active()) so a long group's name stays on
    // screen even once its own header row has scrolled out of view.
    if (sticky_group_bar_active()) {
        const ha_entity_t *top_entity = &entities[visible_indices[scroll_offset]];
        C2D_DrawRectSolid(0.0f, (float)FILTER_BOX_HEIGHT, 0.4f, 320.0f, STICKY_GROUP_BAR_HEIGHT,
            C2D_Color32(0x2a, 0x22, 0x0e, 0xFF));
        draw_text(font, dynBuf, entity_group_label(top_entity), 8.0f, (float)FILTER_BOX_HEIGHT + 2.0f,
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
            // group the row(s) below belong to (room or on/off status,
            // depending on group_mode), so grouping reads as real sections
            // rather than a same-list resort. Its text comes from the
            // entity right after it, since a header is only ever emitted
            // immediately before the first entity of a new group (see
            // rebuild_visible_list). Accent goes on the left edge, not a
            // line flush along the top - a top line on the very first
            // header sits right on the seam with the filter box above it
            // and reads as the header overlapping/peeking out from
            // underneath it.
            C2D_DrawRectSolid(0.0f, y, 0.4f, 320.0f, (float)ROW_HEIGHT, C2D_Color32(0x3a, 0x2e, 0x12, 0xFF));
            C2D_DrawRectSolid(0.0f, y, 0.41f, 4.0f, (float)ROW_HEIGHT, C2D_Color32(0xFF, 0xB3, 0x4D, 0xFF));

            const char *group = AREA_UNASSIGNED_LABEL;
            if (pos + 1 < visible_count && visible_indices[pos + 1] != ROW_IS_HEADER) {
                const ha_entity_t *next = &entities[visible_indices[pos + 1]];
                group = entity_group_label(next);
            }
            char header_display[40];
            snprintf(header_display, sizeof(header_display), "%.36s", group);

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

        draw_domain_icon(&entities[idx], 4.0f, y + 3.0f);

        char display_name[48];
        snprintf(display_name, sizeof(display_name), "%.32s", entities[idx].friendly_name);

        draw_text(font, dynBuf, display_name, 28.0f, y + 4.0f, 0.5f, 0.42f,
            C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

        char state_display[32];
        u32 stateColor;
        format_entity_state(&entities[idx], state_display, sizeof(state_display), &stateColor);

        draw_text(font, dynBuf, state_display, 235.0f, y + 4.0f, 0.5f, 0.42f, stateColor);
    }
}
