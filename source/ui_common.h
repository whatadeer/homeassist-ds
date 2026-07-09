#ifndef UI_COMMON_H
#define UI_COMMON_H

#include <citro2d.h>
#include <citro3d.h>
#include <stddef.h>

// Horizontal parallax for one top-screen element, driven by the console's
// physical stereoscopic-3D depth slider (osGet3DSliderState(), 0.0 off..1.0
// max). layer_depth is how far toward the viewer this element should sit
// (negative recedes behind the screen plane); the left/right eye render the
// same element shifted by equal and opposite amounts, which is the parallax
// cue real stereo rendering relies on. At slider==0 both eyes get shift 0,
// so the screen is pixel-identical to mono rendering.
float stereo_shift(float layer_depth, int right_eye);

// Parses, optimizes, and draws a single line of text - the same three calls
// repeated at every text-drawing site in this codebase, collapsed to one.
void draw_text(C2D_Font font, C2D_TextBuf dynBuf, const char *str,
                float x, float y, float z, float scale, u32 color);

// Renders one static frame with up to two lines of text on the top screen -
// used around blocking network calls so the display shows what's happening
// rather than a stale frame.
void draw_status_frame(C2D_Font font, C2D_TextBuf dynBuf,
                        C3D_RenderTarget *top, C3D_RenderTarget *bottom,
                        const char *line1, const char *line2);

// Generic blocking keyboard prompt. Returns 1 if the user confirmed (right
// button), 0 if they cancelled. out keeps its previous content on cancel.
int prompt_text(const char *hint, const char *initial, char *out, size_t out_size, int password);

#endif
