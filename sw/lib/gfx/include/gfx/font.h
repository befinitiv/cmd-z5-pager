/*
 * Bitmap font format used by lib/gfx.
 *
 * Only standard C types are used here: the emulator's host-side code includes
 * the generated font headers too.
 */

#pragma once

#include <stdint.h>

/* A run of consecutive codepoints; glyph index = base + (cp - first). */
struct gfx_font_range {
	uint16_t first;
	uint16_t count;
	uint16_t base;
};

/*
 * Every glyph has the full cell height. Its bitmap is row-major with
 * (width + 7) / 8 bytes per row, MSB = leftmost pixel. The width includes the
 * spacing to the next glyph.
 */
struct gfx_font {
	uint8_t height;
	uint8_t ascent;
	uint8_t num_ranges;
	uint16_t fallback;
	const struct gfx_font_range *ranges;
	const uint8_t *widths;
	const uint16_t *offsets;
	const uint8_t *bitmap;
};

/* Glyph index for a codepoint, or the fallback glyph if the font lacks it. */
static inline uint16_t gfx_font_glyph(const struct gfx_font *font, uint32_t cp)
{
	for (uint8_t i = 0; i < font->num_ranges; i++) {
		const struct gfx_font_range *r = &font->ranges[i];

		if (cp >= r->first && cp < (uint32_t)r->first + r->count) {
			return r->base + (cp - r->first);
		}
	}
	return font->fallback;
}
