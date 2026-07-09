// --- Settings screen (which entity-type domains to pull) -------------------
// Opened from the main list with START.
#include "screen_settings.h"

#include <stdio.h>

#include "app_config.h"
#include "app_state.h"
#include "dbglog.h"
#include "ha_client.h"
#include "ha_worker.h"
#include "ui_common.h"

#define SETTINGS_ROW_H 26.0f
#define SETTINGS_TOP_Y 22.0f

static int settings_cursor = 0;
// settings_mask_on_entry lets the exit path tell whether anything actually
// changed, so it only spends a refresh (and the network round-trip that
// comes with it) when needed.
static unsigned int settings_mask_on_entry = 0;
static char settings_status[64] = "";

void screen_settings_enter(void) {
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

void screen_settings_handle_input(u32 kDown, int touch_tapped, touchPosition touch) {
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
}

void screen_settings_draw(C2D_Font font, C2D_TextBuf dynBuf,
                           C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
    for (int eye = 0; eye < 2; eye++) {
        C3D_RenderTarget *eye_target = eye ? g_top_right : top;
        C2D_TargetClear(eye_target, C2D_Color32(0x18, 0x1c, 0x28, 0xFF));
        C2D_SceneBegin(eye_target);

        draw_text(font, dynBuf, "Settings", 12.0f + stereo_shift(1.0f, eye), 8.0f, 0.5f, 0.6f,
            C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
        draw_text(font, dynBuf, "Entity types to pull from Home Assistant", 12.0f + stereo_shift(0.5f, eye), 36.0f,
            0.5f, 0.5f, C2D_Color32(0x9f, 0xd8, 0xff, 0xFF));

        if (settings_status[0]) {
            draw_text(font, dynBuf, settings_status, 12.0f + stereo_shift(0.5f, eye), 60.0f, 0.5f, 0.42f,
                C2D_Color32(0xFF, 0x99, 0x55, 0xFF));
        }

        draw_text(font, dynBuf, "Touch/A: toggle   Up/Down: move   B/START: back",
            12.0f + stereo_shift(-0.6f, eye), 196.0f, 0.5f, 0.4f, C2D_Color32(0x88, 0x88, 0x88, 0xFF));
    }

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
