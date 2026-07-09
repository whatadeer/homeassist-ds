#ifndef DOMAIN_ICONS_H
#define DOMAIN_ICONS_H

#include <citro2d.h>

#include "ha_client.h"
#include "pixel_icons.h"

// Domain -> real Pixelarticons glyph (see pixel_icons.h) plus the accent
// color it's tinted with. box_x/box_y is the top-left of an ICON_SIZE x
// ICON_SIZE square to draw within.
#define ICON_SIZE 18.0f
// Same glyphs, redrawn much bigger for the top-screen "now selected" hero -
// see draw_pixel_icon(), it's vector geometry so this stays exactly as
// crisp as ICON_SIZE, just chunkier pixels.
#define HERO_ICON_SIZE 96.0f

// Looks up the icon and accent color for an entity's domain (the part of
// entity_id before the dot). Returns 0 and leaves *icon/*r/*g/*b untouched
// for an unrecognized domain, so callers can fall back to their own
// default. Colors come back as raw components rather than a packed
// C2D_Color32 so domain_tint() below can blend them for the off state.
int domain_icon_lookup(const char *entity_id, const pixel_icon_t **icon, u8 *r, u8 *g, u8 *b);

// A domain's icon color when active, blended most of the way to the app's
// established inactive gray (see the row/state-text off-color) when not -
// keeps a whisper of the domain hue so an off light still reads as "a
// light", while still reading unmistakably as off next to on rows.
u32 domain_tint(u8 r, u8 g, u8 b, int active);

// Fills every rect of a pixel_icon_t (see pixel_icons.h) into a size x size
// square at (x, y), scaled from its native ICON_GRID up to size - since this
// redraws the real vector geometry rather than blitting a texture, it's
// exactly as crisp at the tiny 18px list size as it is blown up for the
// top-screen hero.
void draw_pixel_icon(const pixel_icon_t *icon, float x, float y, float size, u32 color, float z);

// Draws an entity's domain icon (plus a small corner badge if it's a Home
// Assistant group) into an ICON_SIZE square at (box_x, box_y).
void draw_domain_icon(const ha_entity_t *e, float box_x, float box_y);

// Small rotating-dot spinner shown while a network request is in flight.
void draw_spinner(float cx, float cy, float radius, int frame_counter);

#endif
