#ifndef SCREEN_SIGNIN_H
#define SCREEN_SIGNIN_H

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

// Resets the form and switches to it. allow_cancel is 0 at boot when there's
// no working session to fall back to, 1 when opened via SELECT from an
// already-signed-in state.
void screen_signin_enter(int allow_cancel);

void screen_signin_handle_input(u32 kDown, int touch_tapped, touchPosition touch,
                                 C2D_Font font, C2D_TextBuf dynBuf,
                                 C3D_RenderTarget *top, C3D_RenderTarget *bottom);

void screen_signin_draw(C2D_Font font, C2D_TextBuf dynBuf,
                         C3D_RenderTarget *top, C3D_RenderTarget *bottom);

#endif
