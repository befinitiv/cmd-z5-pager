/*
 * Driver for the CMD-Z5 display controller (unknown chip, reverse-engineered).
 *
 * The init sequence replays what the original phone firmware writes, see
 * ../display/DOCUMENTATION.md. Register meanings are guesses; only the pixel
 * addressing (03 = column, 04 = row, 0B = 24 bytes of row data) is confirmed.
 */

#define DT_DRV_COMPAT sony_cmdz5_lcd

#include <zephyr/device.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cmdz5/drivers/lcd.h>
#include <cmdz5/drivers/lcd_bus.h>

LOG_MODULE_REGISTER(cmdz5_lcd, CONFIG_CMDZ5_LCD_LOG_LEVEL);

#define REG_CONTROL   0x00
#define REG_COLUMN    0x03
#define REG_ROW       0x04
#define REG_PIXELS    0x0B
#define REG_CONTRAST1 0x23
#define REG_CONTRAST2 0x24

#define CONTROL_ON  0x40
#define CONTROL_OFF 0x00

struct cmdz5_lcd_config {
	const struct device *bus;
	const struct device *supply;
	uint8_t contrast;
};

struct cmdz5_lcd_data {
	struct k_mutex lock;
	bool on;
	uint8_t contrast;
};

static int idx(const struct device *bus, uint8_t r)
{
	return lcd_bus_write(bus, false, &r, 1);
}

static int reg(const struct device *bus, uint8_t r, uint8_t v)
{
	int ret = idx(bus, r);

	return ret ? ret : lcd_bus_write(bus, true, &v, 1);
}

/* Register/value pairs, written in this order. 0x10 and 0x14 are 16-byte tables. */
static const uint8_t init_seq1[][2] = {
	{0x02, 0x01}, {0x00, 0x04}, {0x01, 0x00}, {0x05, 0x47}, {0x06, 0x27},
	{0x07, 0x11}, {0x08, 0x25}, {0x09, 0x00}, {0x0A, 0x00}, {0x0C, 0x00},
	{0x0D, 0x00}, {0x0E, 0x00}, {0x0F, 0x00},
};
static const uint8_t init_seq2[][2] = {
	{0x11, 0x00}, {0x12, 0x00}, {0x13, 0x00},
};
static const uint8_t init_seq3[][2] = {
	{0x15, 0x00}, {0x20, 0xDF}, {0x21, 0x52}, {0x22, 0x65},
	{0x23, 0x80}, {0x24, 0x80}, {0x25, 0x00}, {0x00, 0x00},
};
/* Gray-level drive tables (guessed), written before switching on. */
static const uint8_t on_seq[][2] = {
	{0x23, 0x7B}, {0x17, 0x00}, {0x18, 0x10}, {0x19, 0x18}, {0x1A, 0x20},
	{0x24, 0x7B}, {0x1B, 0x00}, {0x1C, 0x14}, {0x1D, 0x1A}, {0x1E, 0x20},
	{0x20, 0x1F}, {0x00, CONTROL_ON},
};

static const uint8_t zero_row[LCD_STRIDE];

static int supply_set(const struct device *supply, bool on)
{
#ifdef CONFIG_REGULATOR
	if (supply) {
		return on ? regulator_enable(supply) : regulator_disable(supply);
	}
#endif
	return 0;
}

static int write_seq(const struct device *bus, const uint8_t (*seq)[2], size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int ret = reg(bus, seq[i][0], seq[i][1]);

		if (ret) {
			return ret;
		}
	}
	return 0;
}

static int write_table(const struct device *bus, uint8_t r)
{
	int ret = idx(bus, r);

	return ret ? ret : lcd_bus_write(bus, true, zero_row, 16);
}

static int write_row(const struct device *bus, int y, const uint8_t *row)
{
	int ret = reg(bus, REG_COLUMN, 0);

	ret = ret ? ret : reg(bus, REG_ROW, y);
	ret = ret ? ret : idx(bus, REG_PIXELS);
	return ret ? ret : lcd_bus_write(bus, true, row, LCD_STRIDE);
}

static int controller_init(const struct device *dev)
{
	const struct cmdz5_lcd_config *cfg = dev->config;
	struct cmdz5_lcd_data *data = dev->data;
	const struct device *bus = cfg->bus;
	int ret;

	lcd_bus_reset(bus, true);
	k_msleep(1);
	lcd_bus_reset(bus, false);
	k_msleep(5);

	ret = write_seq(bus, init_seq1, ARRAY_SIZE(init_seq1));
	ret = ret ? ret : write_table(bus, 0x10);
	ret = ret ? ret : write_seq(bus, init_seq2, ARRAY_SIZE(init_seq2));
	ret = ret ? ret : write_table(bus, 0x14);
	ret = ret ? ret : write_seq(bus, init_seq3, ARRAY_SIZE(init_seq3));

	/* Clear the panel before switching it on. */
	for (int y = 0; y < LCD_HEIGHT && !ret; y++) {
		ret = write_row(bus, y, zero_row);
	}

	ret = ret ? ret : write_seq(bus, on_seq, ARRAY_SIZE(on_seq));
	ret = ret ? ret : reg(bus, REG_CONTRAST1, data->contrast);
	ret = ret ? ret : reg(bus, REG_CONTRAST2, data->contrast);
	return ret;
}

static int cmdz5_lcd_set_power(const struct device *dev, bool on)
{
	const struct cmdz5_lcd_config *cfg = dev->config;
	struct cmdz5_lcd_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (on == data->on) {
		goto out;
	}

	if (on) {
		ret = supply_set(cfg->supply, true);
		if (ret) {
			LOG_ERR("supply enable failed (%d)", ret);
			goto out;
		}
		lcd_bus_enable(cfg->bus, true);
		ret = controller_init(dev);
		if (ret) {
			LOG_ERR("init failed (%d)", ret);
		}
	} else {
		reg(cfg->bus, REG_CONTROL, CONTROL_OFF);
		lcd_bus_enable(cfg->bus, false);
		supply_set(cfg->supply, false);
	}
	data->on = on;
out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int cmdz5_lcd_write_rows(const struct device *dev, const uint8_t *fb, int first,
				int count)
{
	const struct cmdz5_lcd_config *cfg = dev->config;
	struct cmdz5_lcd_data *data = dev->data;
	int ret = 0;

	if (first < 0 || count < 0 || first + count > LCD_HEIGHT) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (!data->on) {
		ret = -EIO;
		goto out;
	}
	for (int y = first; y < first + count && !ret; y++) {
		ret = write_row(cfg->bus, y, &fb[y * LCD_STRIDE]);
	}
out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int cmdz5_lcd_set_contrast(const struct device *dev, uint8_t value)
{
	const struct cmdz5_lcd_config *cfg = dev->config;
	struct cmdz5_lcd_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->contrast = value;
	if (data->on) {
		ret = reg(cfg->bus, REG_CONTRAST1, value);
		ret = ret ? ret : reg(cfg->bus, REG_CONTRAST2, value);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static const struct lcd_api cmdz5_lcd_api = {
	.write_rows = cmdz5_lcd_write_rows,
	.set_contrast = cmdz5_lcd_set_contrast,
	.set_power = cmdz5_lcd_set_power,
};

static int cmdz5_lcd_init(const struct device *dev)
{
	const struct cmdz5_lcd_config *cfg = dev->config;
	struct cmdz5_lcd_data *data = dev->data;

	if (!device_is_ready(cfg->bus)) {
		return -ENODEV;
	}
	k_mutex_init(&data->lock);
	data->contrast = cfg->contrast;
	/* The display stays off until the application switches it on. */
	return 0;
}

#define CMDZ5_LCD_DEFINE(n)                                                                \
	static const struct cmdz5_lcd_config cmdz5_lcd_config_##n = {                       \
		.bus = DEVICE_DT_GET(DT_INST_PARENT(n)),                                   \
		.supply = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, power_supply),              \
				      (DEVICE_DT_GET(DT_INST_PHANDLE(n, power_supply))),   \
				      (NULL)),                                             \
		.contrast = DT_INST_PROP(n, contrast),                                     \
	};                                                                                 \
	static struct cmdz5_lcd_data cmdz5_lcd_data_##n;                                   \
	DEVICE_DT_INST_DEFINE(n, cmdz5_lcd_init, NULL, &cmdz5_lcd_data_##n,                 \
			      &cmdz5_lcd_config_##n, POST_KERNEL,                          \
			      CONFIG_CMDZ5_LCD_INIT_PRIORITY, &cmdz5_lcd_api);

DT_INST_FOREACH_STATUS_OKAY(CMDZ5_LCD_DEFINE)
