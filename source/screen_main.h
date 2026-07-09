#ifndef SCREEN_MAIN_H
#define SCREEN_MAIN_H

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

// Parses the persistent instructions line once (outside the frame loop) -
// call once after the font/static text buffer are ready, before the first
// screen_main_draw().
void screen_main_init(C2D_Font font, C2D_TextBuf staticBuf);

void screen_main_handle_input(u32 kDown, int touch_tapped, touchPosition touch);

// log_first_frame gates a few one-shot diagnostic LOG() calls used to
// narrow down where a crash-on-first-frame happened - see main.c's
// first_frame variable.
void screen_main_draw(C2D_Font font, C2D_TextBuf dynBuf,
                       C3D_RenderTarget *top, C3D_RenderTarget *bottom,
                       int anim_frame, int log_first_frame);

#endif
