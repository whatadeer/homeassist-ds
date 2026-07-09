#ifndef SCREEN_SETTINGS_H
#define SCREEN_SETTINGS_H

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

// Switches to the Settings screen, remembering the mask it was opened with
// so leaving it can tell whether a refresh is actually needed.
void screen_settings_enter(void);

void screen_settings_handle_input(u32 kDown, int touch_tapped, touchPosition touch);

void screen_settings_draw(C2D_Font font, C2D_TextBuf dynBuf,
                           C3D_RenderTarget *top, C3D_RenderTarget *bottom);

#endif
