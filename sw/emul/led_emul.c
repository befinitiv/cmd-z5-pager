/*
 * Emulated backlight LEDs and vibration motor, shown on the front panel.
 */

#define DT_DRV_COMPAT cmdz5_led_emul

#include <zephyr/device.h>
#include <zephyr/drivers/led.h>

#include "frontpanel.h"

struct led_emul_config {
	const uint8_t *roles;
	uint8_t num;
};

static int led_emul_set_brightness(const struct device *dev, uint32_t led, uint8_t value)
{
	const struct led_emul_config *cfg = dev->config;

	if (led >= cfg->num) {
		return -EINVAL;
	}
	frontpanel_set_output(cfg->roles[led], value);
	return 0;
}

static int led_emul_on(const struct device *dev, uint32_t led)
{
	return led_emul_set_brightness(dev, led, LED_BRIGHTNESS_MAX);
}

static int led_emul_off(const struct device *dev, uint32_t led)
{
	return led_emul_set_brightness(dev, led, 0);
}

static DEVICE_API(led, led_emul_api) = {
	.on = led_emul_on,
	.off = led_emul_off,
	.set_brightness = led_emul_set_brightness,
};

/* The enum order in the binding matches enum frontpanel_output. */
#define ROLE(child) DT_ENUM_IDX(child, role),

#define LED_EMUL_DEFINE(n)                                                                 \
	static const uint8_t led_emul_roles_##n[] = {DT_INST_FOREACH_CHILD(n, ROLE)};       \
	static const struct led_emul_config led_emul_config_##n = {                         \
		.roles = led_emul_roles_##n,                                               \
		.num = ARRAY_SIZE(led_emul_roles_##n),                                     \
	};                                                                                 \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, &led_emul_config_##n, POST_KERNEL,       \
			      CONFIG_LED_INIT_PRIORITY, &led_emul_api);

DT_INST_FOREACH_STATUS_OKAY(LED_EMUL_DEFINE)
