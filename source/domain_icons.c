#include "domain_icons.h"

#include <math.h>
#include <string.h>

#include "entity_list.h" // entity_is_active

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

int domain_icon_lookup(const char *entity_id, const pixel_icon_t **icon, u8 *r, u8 *g, u8 *b) {
    const char *dot = strchr(entity_id, '.');
    size_t domain_len = dot ? (size_t)(dot - entity_id) : strlen(entity_id);

#define DOMAIN_IS(name) (domain_len == strlen(name) && strncmp(entity_id, name, domain_len) == 0)
#define SET(i, rr, gg, bb) do { *icon = (i); *r = (rr); *g = (gg); *b = (bb); } while (0)

    if (DOMAIN_IS("light")) {
        SET(&ICON_LIGHT, 0xFF, 0xC1, 0x07);
    } else if (DOMAIN_IS("switch")) {
        SET(&ICON_SWITCH_ICON, 0x26, 0xA6, 0x9A);
    } else if (DOMAIN_IS("fan")) {
        SET(&ICON_FAN, 0x4F, 0xC3, 0xF7);
    } else if (DOMAIN_IS("lock")) {
        SET(&ICON_LOCK_ICON, 0x9E, 0x9E, 0x9E);
    } else if (DOMAIN_IS("cover")) {
        SET(&ICON_COVER, 0xA1, 0x88, 0x7F);
    } else if (DOMAIN_IS("climate")) {
        SET(&ICON_CLIMATE, 0xFF, 0x70, 0x43);
    } else if (DOMAIN_IS("media_player")) {
        SET(&ICON_MEDIA_PLAYER, 0x66, 0xBB, 0x6A);
    } else if (DOMAIN_IS("input_boolean")) {
        SET(&ICON_INPUT_BOOLEAN, 0xAB, 0x47, 0xBC);
    } else {
        return 0;
    }

#undef SET
#undef DOMAIN_IS
    return 1;
}

u32 domain_tint(u8 r, u8 g, u8 b, int active) {
    if (!active) {
        const float t = 0.6f;
        const float gray = 0x70;
        r = (u8)((float)r + (gray - (float)r) * t);
        g = (u8)((float)g + (gray - (float)g) * t);
        b = (u8)((float)b + (gray - (float)b) * t);
    }
    return C2D_Color32(r, g, b, 0xFF);
}

void draw_pixel_icon(const pixel_icon_t *icon, float x, float y, float size, u32 color, float z) {
    float scale = size / (float)ICON_GRID;
    for (int i = 0; i < icon->count; i++) {
        const icon_rect_t *r = &icon->rects[i];
        C2D_DrawRectSolid(x + r->x * scale, y + r->y * scale, z, r->w * scale, r->h * scale, color);
    }
}

void draw_domain_icon(const ha_entity_t *e, float box_x, float box_y) {
    const pixel_icon_t *icon;
    u8 r, g, b;

    // citro2d's depth test is GPU_GEQUAL (a draw only shows if its z is >=
    // whatever's already at that pixel - see citro2d/source/base.c). Row
    // backgrounds are drawn at z=0.4 right before this function runs, so
    // the icon needs z > 0.4 here.
    if (domain_icon_lookup(e->entity_id, &icon, &r, &g, &b)) {
        draw_pixel_icon(icon, box_x, box_y, ICON_SIZE, domain_tint(r, g, b, entity_is_active(e)), 0.42f);
    } else {
        C2D_DrawRectSolid(box_x + 3, box_y + 3, 0.42f, ICON_SIZE - 6, ICON_SIZE - 6,
            C2D_Color32(0x77, 0x77, 0x77, 0xFF));
    }

    if (e->is_group) {
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

void draw_spinner(float cx, float cy, float radius, int frame_counter) {
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
