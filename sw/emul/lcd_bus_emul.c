/*
 * Emulated display bus: feeds the bytes written by the real display driver
 * (cmdz5_lcd) into the controller model and shows its framebuffer on the
 * front panel.
 */

#define DT_DRV_COMPAT cmdz5_lcd_bus_emul

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cmdz5/drivers/lcd_bus.h>

#include "frontpanel.h"
#include "lcd_ctrl_model.h"

LOG_MODULE_REGISTER(lcd_bus_emul, CONFIG_CMDZ5_EMUL_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "need exactly one emulated LCD bus");

static struct lcd_model model;
static bool enabled;

static void model_warn(const char *msg, int reg, int value)
{
	LOG_WRN("display model: %s (reg 0x%02x, value 0x%02x)", msg, reg & 0xff, value & 0xff);
}

static void publish(void)
{
	frontpanel_lcd_update(model.fb, enabled && lcd_model_display_on(&model),
			      lcd_model_contrast(&model));
}

static int lcd_bus_emul_write(const struct device *dev, bool a0, const uint8_t *buf, size_t len)
{
	ARG_UNUSED(dev);

	if (!enabled) {
		return -EIO;
	}
	for (size_t i = 0; i < len; i++) {
		lcd_model_write(&model, a0, buf[i]);
	}
	publish();
	return 0;
}

static int lcd_bus_emul_reset(const struct device *dev, bool assert)
{
	ARG_UNUSED(dev);

	lcd_model_reset(&model, assert);
	publish();
	return 0;
}

static int lcd_bus_emul_enable(const struct device *dev, bool on)
{
	ARG_UNUSED(dev);

	enabled = on;
	/* Lines parked low means the controller sits in reset. */
	lcd_model_reset(&model, true);
	publish();
	return 0;
}

static const struct lcd_bus_api lcd_bus_emul_api = {
	.write = lcd_bus_emul_write,
	.reset = lcd_bus_emul_reset,
	.enable = lcd_bus_emul_enable,
};

static int lcd_bus_emul_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	lcd_model_init(&model, model_warn);
	return 0;
}

DEVICE_DT_INST_DEFINE(0, lcd_bus_emul_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_CMDZ5_LCD_BUS_INIT_PRIORITY, &lcd_bus_emul_api);
