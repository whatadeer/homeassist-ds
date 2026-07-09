#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "app_state.h"
#include "dbglog.h"
#include "ha_client.h"
#include "ha_worker.h"
#include "screen_color.h"
#include "screen_main.h"
#include "screen_settings.h"
#include "screen_signin.h"
#include "ui_common.h"

// libctru requires this to be at least 0x100000 - a smaller buffer lets
// socInit "succeed" but leaves soc:U in a bad state that can crash later
// instead of failing cleanly.
#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000

// Right-eye render target for the top screen - see app_state.h.
C3D_RenderTarget *g_top_right = NULL;

static u32 *soc_buffer = NULL;
int network_ready = 0;
char status_msg[128] = "Connecting...";

// The app is always showing exactly one of these four screens - see
// app_state.h.
app_mode_t app_mode = APP_MODE_MAIN;

// Sign-in state - see app_state.h. enabled_domains defaults to "everything
// on" so a fresh install (before any config file exists, and thus before
// app_config_load ever runs) still fetches every domain, same as before
// this setting existed.
app_config_t app_cfg = { .enabled_domains = HA_ALL_DOMAINS_MASK };
time_t token_expires_at = 0;
int signed_in = 0;

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    dbg_log_init();
    LOG("=== ha3ds starting ===");

    ha_worker_init();

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
    gfxSet3D(true);
    LOG("gfxInitDefault done, 3D enabled");
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    LOG("C3D_Init done");
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    LOG("C2D_Init done");
    C2D_Prepare();
    LOG("C2D_Prepare done");

    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_top_right = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    LOG("render targets created: top=%p bottom=%p top_right=%p", (void *)top, (void *)bottom, (void *)g_top_right);

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

    screen_main_init(font, staticBuf);
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
            screen_signin_enter(0);
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
            screen_main_handle_input(kDown, touch_tapped, touch);
        } else if (app_mode == APP_MODE_SETTINGS) {
            screen_settings_handle_input(kDown, touch_tapped, touch);
        } else if (app_mode == APP_MODE_COLOR) {
            screen_color_handle_input(kDown, touch_tapped, touch);
        } else { // APP_MODE_SIGNIN
            screen_signin_handle_input(kDown, touch_tapped, touch, font, dynBuf, top, bottom);
        }
        touch_was_down = touch_is_down;

        C2D_TextBufClear(dynBuf);

        if (first_frame) LOG("frame 1: entering C3D_FrameBegin");
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        if (first_frame) LOG("frame 1: C3D_FrameBegin returned");

        if (app_mode == APP_MODE_SIGNIN) {
            screen_signin_draw(font, dynBuf, top, bottom);
        } else if (app_mode == APP_MODE_SETTINGS) {
            screen_settings_draw(font, dynBuf, top, bottom);
        } else if (app_mode == APP_MODE_COLOR) {
            screen_color_draw(font, dynBuf, top, bottom);
        } else {
            screen_main_draw(font, dynBuf, top, bottom, anim_frame, first_frame);
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
    ha_worker_join_if_busy();

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
