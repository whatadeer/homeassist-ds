#include "ui_common.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "ha_client.h" // HA_MAX_TOKEN, reused as prompt_text's scratch buffer size

// See stereo_shift()'s doc comment in ui_common.h.
#define STEREO_MAX_PARALLAX_PX 6.0f

float stereo_shift(float layer_depth, int right_eye) {
    float slider = osGet3DSliderState();
    float px = slider * STEREO_MAX_PARALLAX_PX * layer_depth;
    return right_eye ? px : -px;
}

void draw_text(C2D_Font font, C2D_TextBuf dynBuf, const char *str,
                float x, float y, float z, float scale, u32 color) {
    C2D_Text text;
    C2D_TextFontParse(&text, font, dynBuf, str);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, z, scale, scale, color);
}

void draw_status_frame(C2D_Font font, C2D_TextBuf dynBuf,
                        C3D_RenderTarget *top, C3D_RenderTarget *bottom,
                        const char *line1, const char *line2) {
    C2D_TextBufClear(dynBuf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    for (int eye = 0; eye < 2; eye++) {
        C3D_RenderTarget *eye_target = eye ? g_top_right : top;
        C2D_TargetClear(eye_target, C2D_Color32(0x18, 0x1c, 0x28, 0xFF));
        C2D_SceneBegin(eye_target);

        C2D_Text t1;
        C2D_TextFontParse(&t1, font, dynBuf, line1);
        C2D_TextOptimize(&t1);
        C2D_DrawText(&t1, C2D_WithColor, 12.0f + stereo_shift(1.0f, eye), 90.0f, 0.5f, 0.55f, 0.55f,
            C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

        if (line2 && line2[0]) {
            C2D_Text t2;
            C2D_TextFontParse(&t2, font, dynBuf, line2);
            C2D_TextOptimize(&t2);
            C2D_DrawText(&t2, C2D_WithColor, 12.0f + stereo_shift(0.5f, eye), 120.0f, 0.5f, 0.45f, 0.45f,
                C2D_Color32(0x9f, 0xd8, 0xff, 0xFF));
        }
    }

    C2D_TargetClear(bottom, C2D_Color32(0x10, 0x12, 0x18, 0xFF));
    C2D_SceneBegin(bottom);

    C3D_FrameEnd(0);
}

int prompt_text(const char *hint, const char *initial, char *out, size_t out_size, int password) {
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
