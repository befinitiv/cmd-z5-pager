/*
 * u-blox M10 GPS receiver on UART0 with a switched supply (TPS610997 boost,
 * enable-gpios). The receiver runs with its factory settings: NMEA at
 * 38400 baud, one fix per second. Zephyr's NMEA 0183 parser turns GGA and
 * RMC (and GSV with CONFIG_GNSS_SATELLITES) into GNSS API callbacks.
 *
 * Zephyr's gnss-nmea-generic driver does the same but cannot be suspended
 * and has no supply, so the GPS service could not switch the receiver.
 * Here device PM switches both: resume = supply on, UART on, pipe open;
 * suspend = pipe closed, UART pins to their sleep state, supply off. Every
 * switch-on is a cold start (no backup supply).
 */

#define DT_DRV_COMPAT cmdz5_gps_m10

#include <zephyr/device.h>
#include <zephyr/drivers/gnss.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/chat.h>
#include <zephyr/pm/device.h>

/* Zephyr's private NMEA helpers (drivers/gnss). */
#include "gnss_nmea0183.h"
#include "gnss_nmea0183_match.h"
#include "gnss_parse.h"

LOG_MODULE_REGISTER(gps_m10, CONFIG_GNSS_LOG_LEVEL);

#define UART_RX_BUF_SZ   (256 + IS_ENABLED(CONFIG_GNSS_SATELLITES) * 512)
#define UART_TX_BUF_SZ   64
#define CHAT_RECV_BUF_SZ 256
#define CHAT_ARGV_SZ     32

struct gps_m10_config {
	const struct device *uart;
	struct gpio_dt_spec enable;
	uint32_t startup_ms;
};

struct gps_m10_data {
	/* First: the chat callbacks get this struct as their user data. */
	struct gnss_nmea0183_match_data match_data;
#if CONFIG_GNSS_SATELLITES
	struct gnss_satellite satellites[CONFIG_CMDZ5_GPS_M10_SATELLITES_COUNT];
#endif
	struct modem_pipe *uart_pipe;
	struct modem_backend_uart uart_backend;
	uint8_t uart_backend_receive_buf[UART_RX_BUF_SZ];
	uint8_t uart_backend_transmit_buf[UART_TX_BUF_SZ];
	struct modem_chat chat;
	uint8_t chat_receive_buf[CHAT_RECV_BUF_SZ];
	uint8_t *chat_argv[CHAT_ARGV_SZ];
};

MODEM_CHAT_MATCHES_DEFINE(unsol_matches,
	MODEM_CHAT_MATCH_WILDCARD("$??GGA,", ",*", gnss_nmea0183_match_gga_callback),
	MODEM_CHAT_MATCH_WILDCARD("$??RMC,", ",*", gnss_nmea0183_match_rmc_callback),
#if CONFIG_GNSS_SATELLITES
	MODEM_CHAT_MATCH_WILDCARD("$??GSV,", ",*", gnss_nmea0183_match_gsv_callback),
#endif
);

static uint8_t delimiter[] = {'\r', '\n'};

static int uart_pm(const struct device *dev, enum pm_device_action action)
{
	const struct gps_m10_config *cfg = dev->config;
	int ret = pm_device_action_run(cfg->uart, action);

	return ret == -EALREADY ? 0 : ret;
}

static int gps_m10_suspend(const struct device *dev)
{
	const struct gps_m10_config *cfg = dev->config;
	struct gps_m10_data *data = dev->data;

	modem_chat_release(&data->chat);
	modem_pipe_close(data->uart_pipe, K_SECONDS(1));
	(void)uart_pm(dev, PM_DEVICE_ACTION_SUSPEND);
	return gpio_pin_set_dt(&cfg->enable, 0);
}

static int gps_m10_resume(const struct device *dev)
{
	const struct gps_m10_config *cfg = dev->config;
	struct gps_m10_data *data = dev->data;
	int ret;

	ret = gpio_pin_set_dt(&cfg->enable, 1);
	if (ret) {
		return ret;
	}
	k_msleep(cfg->startup_ms);

	ret = uart_pm(dev, PM_DEVICE_ACTION_RESUME);
	if (ret == 0) {
		ret = modem_pipe_open(data->uart_pipe, K_SECONDS(1));
	}
	if (ret == 0) {
		ret = modem_chat_attach(&data->chat, data->uart_pipe);
	}
	if (ret) {
		LOG_ERR("resume failed (%d)", ret);
		gps_m10_suspend(dev);
	}
	return ret;
}

static int gps_m10_pm_action(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		return gps_m10_resume(dev);
	case PM_DEVICE_ACTION_SUSPEND:
		return gps_m10_suspend(dev);
	default:
		return -ENOTSUP;
	}
}

static int gps_m10_init(const struct device *dev)
{
	const struct gps_m10_config *cfg = dev->config;
	struct gps_m10_data *data = dev->data;
	int ret;

	if (!gpio_is_ready_dt(&cfg->enable) || !device_is_ready(cfg->uart)) {
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&cfg->enable, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		return ret;
	}

	const struct gnss_nmea0183_match_config match_config = {
		.gnss = dev,
#if CONFIG_GNSS_SATELLITES
		.satellites = data->satellites,
		.satellites_size = ARRAY_SIZE(data->satellites),
#endif
	};
	ret = gnss_nmea0183_match_init(&data->match_data, &match_config);
	if (ret) {
		return ret;
	}

	const struct modem_backend_uart_config uart_config = {
		.uart = cfg->uart,
		.receive_buf = data->uart_backend_receive_buf,
		.receive_buf_size = sizeof(data->uart_backend_receive_buf),
		.transmit_buf = data->uart_backend_transmit_buf,
		.transmit_buf_size = sizeof(data->uart_backend_transmit_buf),
	};
	data->uart_pipe = modem_backend_uart_init(&data->uart_backend, &uart_config);

	const struct modem_chat_config chat_config = {
		.user_data = data,
		.receive_buf = data->chat_receive_buf,
		.receive_buf_size = sizeof(data->chat_receive_buf),
		.delimiter = delimiter,
		.delimiter_size = ARRAY_SIZE(delimiter),
		.argv = data->chat_argv,
		.argv_size = ARRAY_SIZE(data->chat_argv),
		.unsol_matches = unsol_matches,
		.unsol_matches_size = ARRAY_SIZE(unsol_matches),
	};
	ret = modem_chat_init(&data->chat, &chat_config);
	if (ret) {
		return ret;
	}

	/* The receiver starts unpowered: UART pins to their sleep state. */
	ret = uart_pm(dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret) {
		LOG_WRN("UART not suspended (%d)", ret);
	}
	pm_device_init_suspended(dev);
	return 0;
}

static DEVICE_API(gnss, gps_m10_api) = {};

#define GPS_M10(n)                                                                                 \
	static const struct gps_m10_config gps_m10_config_##n = {                                  \
		.uart = DEVICE_DT_GET(DT_INST_BUS(n)),                                             \
		.enable = GPIO_DT_SPEC_INST_GET(n, enable_gpios),                                  \
		.startup_ms = DT_INST_PROP(n, startup_delay_ms),                                   \
	};                                                                                         \
	static struct gps_m10_data gps_m10_data_##n;                                               \
	PM_DEVICE_DT_INST_DEFINE(n, gps_m10_pm_action);                                            \
	DEVICE_DT_INST_DEFINE(n, gps_m10_init, PM_DEVICE_DT_INST_GET(n), &gps_m10_data_##n,        \
			      &gps_m10_config_##n, POST_KERNEL, CONFIG_GNSS_INIT_PRIORITY,         \
			      &gps_m10_api);

DT_INST_FOREACH_STATUS_OKAY(GPS_M10)
