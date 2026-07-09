#ifndef SCREEN_COLOR_H
#define SCREEN_COLOR_H

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include "ha_client.h"

// Switches to the Color screen for one entity (B on a light that
// supports_color or supports_color_temp).
void screen_color_enter(const ha_entity_t *e);

void screen_color_handle_input(u32 kDown, int touch_tapped, touchPosition touch);

void screen_color_draw(C2D_Font font, C2D_TextBuf dynBuf,
                        C3D_RenderTarget *top, C3D_RenderTarget *bottom);

#endif
