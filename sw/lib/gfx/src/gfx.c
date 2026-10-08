/*
 * 2 bpp framebuffer primitives.
 */

#include <string.h>

#include "gfx/gfx.h"

static inline int imax(int a, int b)
{
	return a > b ? a : b;
}

static inline int imin(int a, int b)
{
	return a < b ? a : b;
}

void gfx_mark_dirty(struct gfx *g, int y0, int y1)
{
	y0 = imax(y0, 0);
	y1 = imin(y1, GFX_HEIGHT - 1);
	if (y0 > y1) {
		return;
	}
	if (g->dirty_lo > g->dirty_hi) {
		g->dirty_lo = y0;
		g->dirty_hi = y1;
	} else {
		g->dirty_lo = imin(g->dirty_lo, y0);
		g->dirty_hi = imax(g->dirty_hi, y1);
	}
}

bool gfx_take_dirty(struct gfx *g, int *first, int *count)
{
	if (g->dirty_lo > g->dirty_hi) {
		return false;
	}
	*first = g->dirty_lo;
	*count = g->dirty_hi - g->dirty_lo + 1;
	g->dirty_lo = 1;
	g->dirty_hi = 0;
	return true;
}

void gfx_reset_clip(struct gfx *g)
{
	g->clip = (struct gfx_rect){0, 0, GFX_WIDTH, GFX_HEIGHT};
}

void gfx_set_clip(struct gfx *g, int x, int y, int w, int h)
{
	int x1 = imin(x + w, GFX_WIDTH);
	int y1 = imin(y + h, GFX_HEIGHT);

	x = imax(x, 0);
	y = imax(y, 0);
	g->clip = (struct gfx_rect){x, y, imax(x1 - x, 0), imax(y1 - y, 0)};
}

void gfx_init(struct gfx *g)
{
	memset(g->fb, 0, sizeof(g->fb));
	gfx_reset_clip(g);
	gfx_mark_dirty(g, 0, GFX_HEIGHT - 1);
}

void gfx_clear(struct gfx *g, enum gfx_color c)
{
	memset(g->fb, (c & 3) * 0x55, sizeof(g->fb));
	gfx_mark_dirty(g, 0, GFX_HEIGHT - 1);
}

static inline bool in_clip(const struct gfx *g, int x, int y)
{
	return x >= g->clip.x && x < g->clip.x + g->clip.w && y >= g->clip.y &&
	       y < g->clip.y + g->clip.h;
}

static inline void put(struct gfx *g, int x, int y, unsigned int c)
{
	uint8_t *p = &g->fb[y * GFX_STRIDE + (x >> 2)];
	unsigned int shift = 6 - 2 * (x & 3);

	*p = (*p & ~(3u << shift)) | ((c & 3u) << shift);
}

void gfx_pixel(struct gfx *g, int x, int y, enum gfx_color c)
{
	if (c == GFX_TRANSPARENT || !in_clip(g, x, y)) {
		return;
	}
	put(g, x, y, c);
	gfx_mark_dirty(g, y, y);
}

enum gfx_color gfx_get_pixel(const struct gfx *g, int x, int y)
{
	if (x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) {
		return GFX_WHITE;
	}
	return (g->fb[y * GFX_STRIDE + (x >> 2)] >> (6 - 2 * (x & 3))) & 3;
}

/* Clip a rectangle; returns false if nothing is left. */
static bool clip_rect(const struct gfx *g, int *x, int *y, int *w, int *h)
{
	int x0 = imax(*x, g->clip.x);
	int y0 = imax(*y, g->clip.y);
	int x1 = imin(*x + *w, g->clip.x + g->clip.w);
	int y1 = imin(*y + *h, g->clip.y + g->clip.h);

	if (x0 >= x1 || y0 >= y1) {
		return false;
	}
	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
	return true;
}

void gfx_fill_rect(struct gfx *g, int x, int y, int w, int h, enum gfx_color c)
{
	if (c == GFX_TRANSPARENT || !clip_rect(g, &x, &y, &w, &h)) {
		return;
	}
	uint8_t pattern = (c & 3) * 0x55;

	for (int row = y; row < y + h; row++) {
		int px = x;
		int end = x + w;

		/* Leading pixels up to a byte boundary, whole bytes, trailing pixels. */
		while (px < end && (px & 3)) {
			put(g, px++, row, c);
		}
		if (end - px >= 4) {
			int nbytes = (end - px) >> 2;

			memset(&g->fb[row * GFX_STRIDE + (px >> 2)], pattern, nbytes);
			px += nbytes * 4;
		}
		while (px < end) {
			put(g, px++, row, c);
		}
	}
	gfx_mark_dirty(g, y, y + h - 1);
}

void gfx_hline(struct gfx *g, int x, int y, int w, enum gfx_color c)
{
	gfx_fill_rect(g, x, y, w, 1, c);
}

void gfx_vline(struct gfx *g, int x, int y, int h, enum gfx_color c)
{
	gfx_fill_rect(g, x, y, 1, h, c);
}

void gfx_rect(struct gfx *g, int x, int y, int w, int h, enum gfx_color c)
{
	if (w <= 0 || h <= 0) {
		return;
	}
	gfx_hline(g, x, y, w, c);
	gfx_hline(g, x, y + h - 1, w, c);
	gfx_vline(g, x, y + 1, h - 2, c);
	gfx_vline(g, x + w - 1, y + 1, h - 2, c);
}

void gfx_invert_rect(struct gfx *g, int x, int y, int w, int h)
{
	if (!clip_rect(g, &x, &y, &w, &h)) {
		return;
	}
	for (int row = y; row < y + h; row++) {
		for (int px = x; px < x + w; px++) {
			put(g, px, row, 3 - gfx_get_pixel(g, px, row));
		}
	}
	gfx_mark_dirty(g, y, y + h - 1);
}

void gfx_bitmap(struct gfx *g, int x, int y, int w, int h, const uint8_t *bits,
		int stride, enum gfx_color fg, enum gfx_color bg)
{
	for (int row = 0; row < h; row++) {
		const uint8_t *line = bits + row * stride;

		for (int col = 0; col < w; col++) {
			bool set = line[col >> 3] & (0x80 >> (col & 7));
			enum gfx_color c = set ? fg : bg;

			if (c != GFX_TRANSPARENT && in_clip(g, x + col, y + row)) {
				put(g, x + col, y + row, c);
			}
		}
	}
	gfx_mark_dirty(g, y, y + h - 1);
}

uint32_t gfx_utf8_next(const char **s)
{
	const uint8_t *p = (const uint8_t *)*s;
	uint32_t cp;
	int extra;

	if (p[0] < 0x80) {
		cp = p[0];
		extra = 0;
	} else if ((p[0] & 0xe0) == 0xc0) {
		cp = p[0] & 0x1f;
		extra = 1;
	} else if ((p[0] & 0xf0) == 0xe0) {
		cp = p[0] & 0x0f;
		extra = 2;
	} else if ((p[0] & 0xf8) == 0xf0) {
		cp = p[0] & 0x07;
		extra = 3;
	} else {
		*s += 1;
		return 0xfffd;
	}
	for (int i = 1; i <= extra; i++) {
		if ((p[i] & 0xc0) != 0x80) {
			*s += i;
			return 0xfffd;
		}
		cp = (cp << 6) | (p[i] & 0x3f);
	}
	*s += extra + 1;
	return cp;
}

int gfx_char_width(const struct gfx_font *f, uint32_t cp)
{
	return f->widths[gfx_font_glyph(f, cp)];
}

int gfx_text_width(const struct gfx_font *f, const char *s, int len)
{
	const char *end = len < 0 ? NULL : s + len;
	int w = 0;

	while (*s && (end == NULL || s < end)) {
		w += gfx_char_width(f, gfx_utf8_next(&s));
	}
	return w;
}

int gfx_char(struct gfx *g, const struct gfx_font *f, int x, int y, uint32_t cp,
	     enum gfx_color c)
{
	uint16_t idx = gfx_font_glyph(f, cp);
	int w = f->widths[idx];

	gfx_bitmap(g, x, y, w, f->height, &f->bitmap[f->offsets[idx]], (w + 7) / 8, c,
		   GFX_TRANSPARENT);
	return x + w;
}

int gfx_text(struct gfx *g, const struct gfx_font *f, int x, int y, const char *s,
	     enum gfx_color c)
{
	while (*s) {
		x = gfx_char(g, f, x, y, gfx_utf8_next(&s), c);
	}
	return x;
}

int gfx_text_fit(struct gfx *g, const struct gfx_font *f, int x, int y, int max_w,
		 const char *s, enum gfx_color c)
{
	if (gfx_text_width(f, s, -1) <= max_w) {
		return gfx_text(g, f, x, y, s, c);
	}

	/* Prefer the ellipsis character, fall back to "..". */
	const char *ell = "\xe2\x80\xa6";
	int ell_w = gfx_text_width(f, ell, -1);

	if (gfx_font_glyph(f, 0x2026) == f->fallback) {
		ell = "..";
		ell_w = gfx_text_width(f, ell, -1);
	}

	int end = x + max_w - ell_w;

	while (*s) {
		const char *p = s;
		uint32_t cp = gfx_utf8_next(&p);

		if (x + gfx_char_width(f, cp) > end) {
			break;
		}
		x = gfx_char(g, f, x, y, cp, c);
		s = p;
	}
	return gfx_text(g, f, x, y, ell, c);
}

void gfx_text_aligned(struct gfx *g, const struct gfx_font *f, int x, int y, int w,
		      enum gfx_align align, const char *s, enum gfx_color c)
{
	int tw = gfx_text_width(f, s, -1);

	if (tw > w) {
		gfx_text_fit(g, f, x, y, w, s, c);
		return;
	}
	if (align == GFX_ALIGN_CENTER) {
		x += (w - tw) / 2;
	} else if (align == GFX_ALIGN_RIGHT) {
		x += w - tw;
	}
	gfx_text(g, f, x, y, s, c);
}

int gfx_wrap(const struct gfx_font *f, const char *s, int max_w, const char **next)
{
	const char *start = s;
	const char *last_space = NULL;
	int w = 0;

	while (*s) {
		const char *p = s;
		uint32_t cp = gfx_utf8_next(&p);

		if (cp == '\n') {
			*next = p;
			return s - start;
		}
		w += gfx_char_width(f, cp);
		if (w > max_w) {
			if (last_space) {
				*next = last_space + 1;
				return last_space - start;
			}
			/* A single word longer than the line: break inside it. */
			if (s == start) {
				s = p;
			}
			*next = s;
			return s - start;
		}
		if (cp == ' ') {
			last_space = s;
		}
		s = p;
	}
	*next = s;
	return s - start;
}
