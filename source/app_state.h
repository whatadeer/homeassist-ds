#ifndef APP_STATE_H
#define APP_STATE_H

#include <3ds.h>
#include <citro3d.h>
#include <time.h>

#include "app_config.h"

// The app is always showing exactly one of these four screens - they share
// the same render loop/frame cadence; only the drawing and input-handling
// branch on this (see the screen_*_handle_input/screen_*_draw functions and
// main()'s dispatch in main.c).
typedef enum {
    APP_MODE_MAIN,
    APP_MODE_SIGNIN,
    APP_MODE_SETTINGS,
    APP_MODE_COLOR,
} app_mode_t;
extern app_mode_t app_mode;

// Right-eye render target for the top screen, used to render real
// stereoscopic 3D via the console's physical depth slider - see
// stereo_shift() in ui_common.c. Only the top screen is stereoscopic
// hardware; the bottom (touch) screen stays single-target/2D. Global rather
// than threaded through every draw function's parameter list, matching this
// codebase's existing use of statics for cross-cutting render state.
extern C3D_RenderTarget *g_top_right;

// Set once at boot: whether socInit()/ha_client_init() succeeded. False
// means every network call is skipped and the UI runs offline.
extern int network_ready;

// Sign-in state. app_cfg holds the base URL + long-lived refresh token
// (persisted to SD); short-lived access tokens are minted from it and
// handed to ha_client, refreshed just before expiry. Written only by the
// main thread (boot bootstrap / sign-in screen, both of which run while no
// worker is in flight); token_expires_at is also updated by the worker
// thread via ensure_access_token - safe for the same reason: the sign-in
// screen never runs concurrently with a worker.
extern app_config_t app_cfg;
extern time_t token_expires_at;
extern int signed_in;

// One-line status shown at the top of the main screen - also doubles as the
// network worker's in-flight progress/result message.
extern char status_msg[128];

#endif
