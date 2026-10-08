/*
 * 8080-style write-only parallel bus to the CMD-Z5 display controller.
 *
 * Implemented by the GPIO bit-bang driver on the board and by the emulator,
 * which feeds the bytes into a model of the controller. The controller driver
 * (cmdz5_lcd) sits on top and is shared by both.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

struct lcd_bus_api {
	/* Write bytes with A0 = a0 (false: register index, true: data). */
	int (*write)(const struct device *dev, bool a0, const uint8_t *buf, size_t len);
	/* Drive /RESET: true = reset asserted (low). */
	int (*reset)(const struct device *dev, bool assert);
	/*
	 * Enable the bus outputs (lines idle high) or park them low. Parking is
	 * needed while the display supply is off, to avoid back-powering it.
	 */
	int (*enable)(const struct device *dev, bool on);
};

static inline int lcd_bus_write(const struct device *dev, bool a0, const uint8_t *buf,
				size_t len)
{
	return ((const struct lcd_bus_api *)dev->api)->write(dev, a0, buf, len);
}

static inline int lcd_bus_reset(const struct device *dev, bool assert)
{
	return ((const struct lcd_bus_api *)dev->api)->reset(dev, assert);
}

static inline int lcd_bus_enable(const struct device *dev, bool on)
{
	return ((const struct lcd_bus_api *)dev->api)->enable(dev, on);
}
