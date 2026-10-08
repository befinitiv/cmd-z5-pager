/*
 * 2 bpp framebuffer graphics for the 96 x 72 CMD-Z5 display.
 *
 * The framebuffer uses the display's native format, so it can be sent to the
 * controller row by row without conversion: 24 bytes per row, 4 pixels per
 * byte, leftmost pixel in bits 7..6, 0 = white ... 3 = black.
 *
 * Plain C without Zephyr dependencies, so it also builds in host unit tests.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx/font.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_WIDTH  96
#define GFX_HEIGHT 72
#define GFX_STRIDE (GFX_WIDTH / 4)
#define GFX_FB_SIZE (GFX_STRIDE * GFX_HEIGHT)

enum gfx_color {
	GFX_WHITE = 0,
	GFX_LIGHT = 1,
	GFX_DARK = 2,
	GFX_BLACK = 3,
	/* Only valid as background colour: leave the pixels unchanged. */
	GFX_TRANSPARENT = 0xff,
};

struct gfx_rect {
	int16_t x, y, w, h;
};

struct gfx {
	uint8_t fb[GFX_FB_SIZE];
	struct gfx_rect clip;
	/* Changed rows since the last gfx_take_dirty(); dirty_lo > dirty_hi = none. */
	int16_t dirty_lo, dirty_hi;
};

/* Built-in fonts. */
extern const struct gfx_font *const gfx_font_small; /* 4x6 fixed, status bar */
extern const struct gfx_font *const gfx_font_text;  /* 5x7 proportional */
extern const struct gfx_font *const gfx_font_large; /* 10x20 fixed, clock */

void gfx_init(struct gfx *g);
void gfx_clear(struct gfx *g, enum gfx_color c);

/* Drawing is limited to the clip rectangle (initially the full screen). */
void gfx_set_clip(struct gfx *g, int x, int y, int w, int h);
void gfx_reset_clip(struct gfx *g);

void gfx_pixel(struct gfx *g, int x, int y, enum gfx_color c);
enum gfx_color gfx_get_pixel(const struct gfx *g, int x, int y);
void gfx_hline(struct gfx *g, int x, int y, int w, enum gfx_color c);
void gfx_vline(struct gfx *g, int x, int y, int h, enum gfx_color c);
void gfx_fill_rect(struct gfx *g, int x, int y, int w, int h, enum gfx_color c);
void gfx_rect(struct gfx *g, int x, int y, int w, int h, enum gfx_color c);
/* Swap white <-> black and light <-> dark inside the rectangle. */
void gfx_invert_rect(struct gfx *g, int x, int y, int w, int h);

/*
 * Draw a 1 bpp bitmap (row-major, `stride` bytes per row, MSB = left). Set
 * bits are drawn in fg, clear bits in bg (GFX_TRANSPARENT skips them).
 */
void gfx_bitmap(struct gfx *g, int x, int y, int w, int h, const uint8_t *bits,
		int stride, enum gfx_color fg, enum gfx_color bg);

/* Decode one UTF-8 character and advance *s. Invalid bytes yield U+FFFD. */
uint32_t gfx_utf8_next(const char **s);

/* Width in pixels of the first `len` bytes of a UTF-8 string (len < 0: all). */
int gfx_text_width(const struct gfx_font *f, const char *s, int len);
int gfx_char_width(const struct gfx_font *f, uint32_t cp);

/*
 * Draw a UTF-8 string with the top of the character cell at y. The background
 * is left unchanged. Returns the x position after the last character.
 */
int gfx_text(struct gfx *g, const struct gfx_font *f, int x, int y, const char *s,
	     enum gfx_color c);
int gfx_char(struct gfx *g, const struct gfx_font *f, int x, int y, uint32_t cp,
	     enum gfx_color c);

/* Like gfx_text, but cut to max_w pixels with a trailing ellipsis if needed. */
int gfx_text_fit(struct gfx *g, const struct gfx_font *f, int x, int y, int max_w,
		 const char *s, enum gfx_color c);

enum gfx_align { GFX_ALIGN_LEFT, GFX_ALIGN_CENTER, GFX_ALIGN_RIGHT };

/* Draw a string aligned inside [x, x + w). */
void gfx_text_aligned(struct gfx *g, const struct gfx_font *f, int x, int y, int w,
		      enum gfx_align align, const char *s, enum gfx_color c);

/*
 * Byte length of the longest prefix of s that fits into max_w pixels when
 * breaking at spaces (or anywhere if a single word is too long). Stops at '\n'.
 * Used to word-wrap text. *next is set to the start of the following line.
 */
int gfx_wrap(const struct gfx_font *f, const char *s, int max_w, const char **next);

/*
 * Return the range of rows changed since the last call and reset it.
 * Returns false if nothing changed.
 */
bool gfx_take_dirty(struct gfx *g, int *first, int *count);
void gfx_mark_dirty(struct gfx *g, int y0, int y1);

#ifdef __cplusplus
}
#endif
