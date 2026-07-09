// Domain icon shapes, sourced from Pixelarticons
// (https://github.com/halfmage/pixelarticons), (c) 2019 Gerrit Halfmann,
// MIT License (see CREDITS.md).
//
// Every icon in that set is a union of axis-aligned rectangles on a 24x24
// grid - each SVG subpath traces one rectangle's perimeter with h/v line
// commands rather than filling a shape directly. These arrays are that same
// rectangle geometry, unchanged, just extracted from the path data instead
// of an SVG parser: draw_pixel_icon() below reproduces the SVG exactly by
// filling each rect with C2D_DrawRectSolid at whatever size and color the
// call site wants, which is also what makes them recolorable per domain and
// crisp at any scale (list-row size or the blown-up top-screen hero) - it's
// vector geometry, not a raster texture.
#ifndef PIXEL_ICONS_H
#define PIXEL_ICONS_H

#include <3ds.h>

typedef struct {
    u8 x, y, w, h;
} icon_rect_t;

#define ICON_GRID 24

static const icon_rect_t ICON_LIGHTBULB[] = {
    {9,4,6,2}, {7,6,2,2}, {15,6,2,2}, {19,4,2,2}, {21,2,2,2}, {0,10,3,2},
    {21,10,3,2}, {3,4,2,2}, {1,2,2,2}, {7,14,2,2}, {15,14,2,2}, {5,8,2,6},
    {17,8,2,6}, {9,16,6,2}, {9,20,6,2}, {9,18,2,2}, {13,18,2,2}, {11,0,2,3}
};
static const icon_rect_t ICON_SWITCH[] = {
    {3,19,2,2}, {15,15,6,6}, {5,17,2,2}, {7,15,2,2}, {15,15,2,2}, {13,13,2,2},
    {11,11,2,2}, {9,9,2,2}, {13,9,2,2}, {7,7,2,2}, {15,7,2,2}, {15,3,6,6},
    {5,5,2,2}, {3,3,2,2}
};
static const icon_rect_t ICON_WIND[] = {
    {2,7,10,2}, {12,3,2,4}, {7,1,5,2}, {2,11,18,2}, {20,7,2,4}, {16,5,4,2},
    {2,15,12,2}, {14,17,2,2}, {9,19,5,2}
};
static const icon_rect_t ICON_LOCK[] = {
    {5,8,14,2}, {5,20,14,2}, {3,10,2,10}, {19,10,2,10}, {7,4,2,4}, {9,2,6,2},
    {15,4,2,4}
};
static const icon_rect_t ICON_WINDOW_FRAME[] = {
    {4,2,16,2}, {4,8,16,2}, {4,20,16,2}, {2,4,2,16}, {20,4,2,16}, {5,5,2,2},
    {8,5,2,2}
};
static const icon_rect_t ICON_THERMOMETER[] = {
    {9,2,6,2}, {9,20,6,2}, {11,16,2,2}, {7,4,2,16}, {15,4,2,16}
};
static const icon_rect_t ICON_PLAY[] = {
    {13,9,2,2}, {13,13,2,2}, {11,15,2,2}, {11,7,2,2}, {9,5,2,2}, {7,3,2,18},
    {15,11,2,2}, {9,17,2,2}
};
static const icon_rect_t ICON_CHECKBOX[] = {
    {4,2,16,2}, {4,20,16,2}, {2,4,2,16}, {20,4,2,16}, {7,12,2,2}, {9,14,2,2},
    {11,12,2,2}, {13,10,2,2}, {15,8,2,2}
};

typedef struct {
    const icon_rect_t *rects;
    int count;
} pixel_icon_t;

#define PIXEL_ICON(arr) { arr, (int)(sizeof(arr) / sizeof((arr)[0])) }

static const pixel_icon_t ICON_LIGHT = PIXEL_ICON(ICON_LIGHTBULB);
static const pixel_icon_t ICON_SWITCH_ICON = PIXEL_ICON(ICON_SWITCH);
static const pixel_icon_t ICON_FAN = PIXEL_ICON(ICON_WIND);
static const pixel_icon_t ICON_LOCK_ICON = PIXEL_ICON(ICON_LOCK);
static const pixel_icon_t ICON_COVER = PIXEL_ICON(ICON_WINDOW_FRAME);
static const pixel_icon_t ICON_CLIMATE = PIXEL_ICON(ICON_THERMOMETER);
static const pixel_icon_t ICON_MEDIA_PLAYER = PIXEL_ICON(ICON_PLAY);
static const pixel_icon_t ICON_INPUT_BOOLEAN = PIXEL_ICON(ICON_CHECKBOX);

#undef PIXEL_ICON

#endif
