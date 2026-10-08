/*
 * Small 1 bpp icons for the status bar and lists. One byte per row, MSB = left.
 */

#pragma once

#include <stdint.h>

struct gfx_icon {
	uint8_t w, h;
	const uint8_t *bits;
};

extern const struct gfx_icon gfx_icon_envelope; /* 7x5 */
extern const struct gfx_icon gfx_icon_gps;      /* 5x6 */
extern const struct gfx_icon gfx_icon_antenna;  /* 5x6 */
extern const struct gfx_icon gfx_icon_speaker;  /* 5x5 */
extern const struct gfx_icon gfx_icon_bolt;     /* 3x5, charging */
extern const struct gfx_icon gfx_icon_arrow_r;  /* 3x5, submenu marker */

#define gfx_draw_icon(g, x, y, icon, fg)                                                   \
	gfx_bitmap((g), (x), (y), (icon)->w, (icon)->h, (icon)->bits, 1, (fg), GFX_TRANSPARENT)
