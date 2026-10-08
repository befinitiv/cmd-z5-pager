/*
 * 8080-style display bus bit-banged on GPIOs.
 *
 * On the CMD-Z5 board D0..D7 are on consecutive pins P0.19..P0.26, so a byte
 * is written with one masked port write. Other pin layouts work too, with a
 * slower per-pin fallback. All levels are raw: /WR and /RESET are active low
 * on the wire, the devicetree flags are ignored.
 *
 * The controller latches on the rising edge of /WR. The original phone keeps
 * /WR low for 300-350 ns; CONFIG_CMDZ5_LCD_BUS_WR_LOW_NOPS stretches the low
 * phase on top of the GPIO call overhead.
 */

#define DT_DRV_COMPAT cmdz5_lcd_bus_gpio

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cmdz5/drivers/lcd_bus.h>

LOG_MODULE_REGISTER(lcd_bus_gpio, CONFIG_CMDZ5_LCD_LOG_LEVEL);

struct lcd_bus_gpio_config {
	struct gpio_dt_spec data[8];
	struct gpio_dt_spec wr;
	struct gpio_dt_spec a0;
	struct gpio_dt_spec reset;
};

struct lcd_bus_gpio_data {
	/* Fast path: all data pins on one port, D0..D7 consecutive. */
	bool fast;
	uint8_t shift;
	gpio_port_pins_t mask;
	bool enabled;
};

static inline void wr_delay(void)
{
	for (int i = 0; i < CONFIG_CMDZ5_LCD_BUS_WR_LOW_NOPS; i++) {
		arch_nop();
	}
}

static inline void put_byte(const struct lcd_bus_gpio_config *cfg,
			    const struct lcd_bus_gpio_data *data, uint8_t b)
{
	if (data->fast) {
		gpio_port_set_masked_raw(cfg->data[0].port, data->mask,
					 (gpio_port_value_t)b << data->shift);
	} else {
		for (int i = 0; i < 8; i++) {
			gpio_pin_set_raw(cfg->data[i].port, cfg->data[i].pin, (b >> i) & 1);
		}
	}
}

static int lcd_bus_gpio_write(const struct device *dev, bool a0, const uint8_t *buf, size_t len)
{
	const struct lcd_bus_gpio_config *cfg = dev->config;
	struct lcd_bus_gpio_data *data = dev->data;

	if (!data->enabled) {
		return -EIO;
	}

	gpio_pin_set_raw(cfg->a0.port, cfg->a0.pin, a0);
	for (size_t i = 0; i < len; i++) {
		put_byte(cfg, data, buf[i]);
		gpio_pin_set_raw(cfg->wr.port, cfg->wr.pin, 0);
		wr_delay();
		gpio_pin_set_raw(cfg->wr.port, cfg->wr.pin, 1);
	}
	return 0;
}

static int lcd_bus_gpio_reset(const struct device *dev, bool assert)
{
	const struct lcd_bus_gpio_config *cfg = dev->config;

	return gpio_pin_set_raw(cfg->reset.port, cfg->reset.pin, assert ? 0 : 1);
}

static int lcd_bus_gpio_enable(const struct device *dev, bool on)
{
	const struct lcd_bus_gpio_config *cfg = dev->config;
	struct lcd_bus_gpio_data *data = dev->data;

	/* Idle: /WR high, /RESET asserted until the driver releases it. Parked: all low. */
	put_byte(cfg, data, 0);
	gpio_pin_set_raw(cfg->a0.port, cfg->a0.pin, 0);
	gpio_pin_set_raw(cfg->reset.port, cfg->reset.pin, 0);
	gpio_pin_set_raw(cfg->wr.port, cfg->wr.pin, on ? 1 : 0);
	data->enabled = on;
	return 0;
}

static const struct lcd_bus_api lcd_bus_gpio_api = {
	.write = lcd_bus_gpio_write,
	.reset = lcd_bus_gpio_reset,
	.enable = lcd_bus_gpio_enable,
};

static int config_low(const struct gpio_dt_spec *spec)
{
	if (!gpio_is_ready_dt(spec)) {
		return -ENODEV;
	}
	/* GPIO_OUTPUT_LOW sets the raw level regardless of the active-low flag. */
	return gpio_pin_configure(spec->port, spec->pin, GPIO_OUTPUT_LOW);
}

static int lcd_bus_gpio_init(const struct device *dev)
{
	const struct lcd_bus_gpio_config *cfg = dev->config;
	struct lcd_bus_gpio_data *data = dev->data;
	int ret = 0;

	data->fast = true;
	for (int i = 0; i < 8; i++) {
		ret = ret ? ret : config_low(&cfg->data[i]);
		if (cfg->data[i].port != cfg->data[0].port ||
		    cfg->data[i].pin != cfg->data[0].pin + i) {
			data->fast = false;
		}
	}
	ret = ret ? ret : config_low(&cfg->wr);
	ret = ret ? ret : config_low(&cfg->a0);
	ret = ret ? ret : config_low(&cfg->reset);
	if (ret) {
		LOG_ERR("GPIO setup failed (%d)", ret);
		return ret;
	}

	data->shift = cfg->data[0].pin;
	data->mask = (gpio_port_pins_t)0xff << data->shift;
	if (!data->fast) {
		LOG_WRN("data pins not consecutive, using slow path");
	}
	return 0;
}

#define DATA_SPEC(node_id, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),

#define LCD_BUS_GPIO_DEFINE(n)                                                             \
	BUILD_ASSERT(DT_INST_PROP_LEN(n, data_gpios) == 8, "need 8 data GPIOs");           \
	static const struct lcd_bus_gpio_config lcd_bus_gpio_config_##n = {                 \
		.data = {DT_INST_FOREACH_PROP_ELEM(n, data_gpios, DATA_SPEC)},             \
		.wr = GPIO_DT_SPEC_INST_GET(n, wr_gpios),                                  \
		.a0 = GPIO_DT_SPEC_INST_GET(n, a0_gpios),                                  \
		.reset = GPIO_DT_SPEC_INST_GET(n, reset_gpios),                            \
	};                                                                                 \
	static struct lcd_bus_gpio_data lcd_bus_gpio_data_##n;                             \
	DEVICE_DT_INST_DEFINE(n, lcd_bus_gpio_init, NULL, &lcd_bus_gpio_data_##n,           \
			      &lcd_bus_gpio_config_##n, POST_KERNEL,                       \
			      CONFIG_CMDZ5_LCD_BUS_INIT_PRIORITY, &lcd_bus_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(LCD_BUS_GPIO_DEFINE)
