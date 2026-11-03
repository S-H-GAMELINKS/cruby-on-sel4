/*
 * Stand-in for the header CAmkES generates for the Ruby component, carrying
 * only what fb_console.c reads from it. The values are defined by render.c.
 */

#pragma once

extern const int fb_width;
extern const int fb_height;
extern const int fb_pitch;
extern const int fb_scale;
extern const int fb_rotate;
extern void *fb;
