// --- Sign-in form -----------------------------------------------------------
// A persistent on-screen form (URL/username/password/[2FA]) instead of a
// blind sequence of keyboard popups: tapping a field opens the system
// keyboard for just that field, and the form redraws with the updated
// value so all fields stay visible throughout. "Sign In" drives the actual
// (blocking) login-flow network calls.
#include "screen_signin.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "app_state.h"
#include "dbglog.h"
#include "ha_client.h"
#include "ha_worker.h"
#include "ui_common.h"

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

void screen_signin_enter(int allow_cancel) {
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
static void signin_finish_exchange(const char *auth_code) {
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

    signin_finish_exchange(auth_code);
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

void screen_signin_handle_input(u32 kDown, int touch_tapped, touchPosition touch,
                                 C2D_Font font, C2D_TextBuf dynBuf,
                                 C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
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

void screen_signin_draw(C2D_Font font, C2D_TextBuf dynBuf,
                         C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
    for (int eye = 0; eye < 2; eye++) {
        C3D_RenderTarget *eye_target = eye ? g_top_right : top;
        C2D_TargetClear(eye_target, C2D_Color32(0x18, 0x1c, 0x28, 0xFF));
        C2D_SceneBegin(eye_target);

        C2D_Text title;
        C2D_TextFontParse(&title, font, dynBuf, "Sign in to Home Assistant");
        C2D_TextOptimize(&title);
        C2D_DrawText(&title, C2D_WithColor, 12.0f + stereo_shift(1.0f, eye), 8.0f, 0.5f, 0.6f, 0.6f,
            C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

        C2D_Text status;
        C2D_TextFontParse(&status, font, dynBuf, signin_status);
        C2D_TextOptimize(&status);
        C2D_DrawText(&status, C2D_WithColor, 12.0f + stereo_shift(0.5f, eye), 36.0f, 0.5f, 0.5f, 0.5f,
            C2D_Color32(0x9f, 0xd8, 0xff, 0xFF));
    }

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
