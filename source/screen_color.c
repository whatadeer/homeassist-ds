// --- Color screen (rgb_color / color_temp_kelvin presets) ------------------
// Opened from the main list with B on a light that supports_color or
// supports_color_temp. A fixed preset palette rather than a full color
// wheel/slider: precise hue/saturation dragging on a 320x240 touch screen is
// fiddly, and presets cover the common case (set a mood color, or warm/
// neutral/cool white) in one tap.
#include "screen_color.h"

#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "ha_worker.h"
#include "ui_common.h"

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
// Rows the color grid actually occupies - shared by screen_color_draw()
// (where the temp row starts) and the touch handler (which row a tap in the
// color grid landed in), so the two agree on layout.
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

static char color_target_entity_id[HA_MAX_ENTITY_ID] = "";
static char color_target_name[HA_MAX_NAME] = "";
static int color_target_supports_color = 0;
static int color_target_supports_color_temp = 0;

void screen_color_enter(const ha_entity_t *e) {
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
static void screen_color_exit(void) {
    app_mode = APP_MODE_MAIN;
}

// Top-left of grid cell `index` in a `cols`-wide grid whose rows start at
// base_y, each `cell_h` tall - shared by screen_color_draw() (drawing) and
// the touch handler (hit-testing) so the two can't drift apart. Full-cell
// width/height (COLOR_CELL_W, cell_h), not the inset swatch drawn inside it,
// is the tappable area - a bigger target than the swatch itself is easier to
// hit on a 3DS touch screen.
static void color_cell_origin(int index, int cols, float base_y, float cell_h, float *out_x, float *out_y) {
    *out_x = (float)(index % cols) * COLOR_CELL_W;
    *out_y = base_y + (float)(index / cols) * cell_h;
}

void screen_color_handle_input(u32 kDown, int touch_tapped, touchPosition touch) {
    if (kDown & (KEY_B | KEY_START)) {
        screen_color_exit();
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
}

void screen_color_draw(C2D_Font font, C2D_TextBuf dynBuf,
                        C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
    char title_str[64];
    snprintf(title_str, sizeof(title_str), "Color: %.40s", color_target_name);

    for (int eye = 0; eye < 2; eye++) {
        C3D_RenderTarget *eye_target = eye ? g_top_right : top;
        C2D_TargetClear(eye_target, C2D_Color32(0x18, 0x1c, 0x28, 0xFF));
        C2D_SceneBegin(eye_target);

        draw_text(font, dynBuf, title_str, 12.0f + stereo_shift(1.0f, eye), 8.0f, 0.5f, 0.55f,
            C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
        draw_text(font, dynBuf, "Touch a swatch to apply   B: back", 12.0f + stereo_shift(0.5f, eye), 36.0f,
            0.5f, 0.42f, C2D_Color32(0x9f, 0xd8, 0xff, 0xFF));
    }

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
