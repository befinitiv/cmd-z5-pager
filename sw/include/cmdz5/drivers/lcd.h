/*
 * CMD-Z5 display: 96 x 72 pixels, 2 bpp, rows of 24 bytes in the format used
 * by lib/gfx (leftmost pixel in bits 7..6, 0 = white, 3 = black).
 *
 * The controller is write-only and was reverse-engineered, see
 * ../display/DOCUMENTATION.md.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#define LCD_WIDTH  96
#define LCD_HEIGHT 72
#define LCD_STRIDE 24

struct lcd_api {
	/* Send rows [first, first + count) of a full framebuffer. */
	int (*write_rows)(const struct device *dev, const uint8_t *fb, int first, int count);
	int (*set_contrast)(const struct device *dev, uint8_t value);
	/*
	 * true: switch on the supply, reset and initialise the controller.
	 * false: display off, bus lines parked, supply off.
	 * The display content is lost while off; redraw after switching on.
	 */
	int (*set_power)(const struct device *dev, bool on);
};

static inline int lcd_write_rows(const struct device *dev, const uint8_t *fb, int first,
				 int count)
{
	return ((const struct lcd_api *)dev->api)->write_rows(dev, fb, first, count);
}

static inline int lcd_set_contrast(const struct device *dev, uint8_t value)
{
	return ((const struct lcd_api *)dev->api)->set_contrast(dev, value);
}

static inline int lcd_set_power(const struct device *dev, bool on)
{
	return ((const struct lcd_api *)dev->api)->set_power(dev, on);
}
