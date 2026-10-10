/*
 * Emulated LoRa radio: packets travel as UDP multicast datagrams between
 * emulator instances. Each datagram carries the radio settings, so only
 * instances on the same frequency, bandwidth, spreading factor and sync word
 * receive it. Sending takes the packet's time on air; the radio is
 * half-duplex, like the real one.
 *
 * Options: --radio-port=<n> (default 4403), --radio-off, --node-num=<hex>.
 */

#define DT_DRV_COMPAT cmdz5_radio_emul

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <cmdline.h>
#include <posix_native_task.h>

#include <cmdz5/drivers/mesh_radio.h>

#include "emul.h"
#include "radio_bottom.h"

LOG_MODULE_REGISTER(radio_emul, CONFIG_CMDZ5_EMUL_LOG_LEVEL);

#define ENVELOPE_LEN 20
#define POLL_MS      5
#define EMUL_RSSI    (-60)
#define EMUL_SNR     9

static uint32_t opt_port = 4403;
static bool opt_off;
static char *opt_node_num;

static struct {
	bool open;
	uint32_t instance;
	struct mesh_radio_config cfg;
	bool configured;
	volatile bool sending;
	mesh_radio_rx_cb cb;
	void *user;
} radio;

void emul_random(void *buf, size_t len)
{
	radio_bottom_random(buf, len);
}

uint32_t emul_node_num(void)
{
	return opt_node_num ? strtoul(opt_node_num, NULL, 16) : 0;
}

static uint32_t airtime_ms(const struct mesh_radio_config *c, size_t len)
{
	double tsym = (double)(1u << c->sf) / c->bw_hz * 1000.0;
	int de = tsym > 16.0;
	double n = ceil((8.0 * len - 4.0 * c->sf + 44) / (4.0 * (c->sf - 2 * de))) * c->cr;

	return (uint32_t)((c->preamble + 4.25 + 8 + (n > 0 ? n : 0)) * tsym);
}

static void put_cfg(uint8_t *env, const struct mesh_radio_config *c, size_t len)
{
	memcpy(env, "CZRF", 4);
	sys_put_le32(radio.instance, env + 4);
	sys_put_le32(c->freq_hz, env + 8);
	sys_put_le32(c->bw_hz, env + 12);
	env[16] = c->sf;
	env[17] = c->cr;
	env[18] = c->sync_word;
	env[19] = len;
}

bool emul_radio_rx(void)
{
	return radio.open && radio.configured;
}

bool emul_radio_tx(void)
{
	return radio.sending;
}

static int radio_emul_configure(const struct device *dev, const struct mesh_radio_config *cfg)
{
	ARG_UNUSED(dev);
	radio.cfg = *cfg;
	radio.configured = true;
	LOG_INF("%u.%03u MHz, SF%u, %u kHz, CR4/%u, sync 0x%02x", cfg->freq_hz / 1000000,
		(cfg->freq_hz / 1000) % 1000, cfg->sf, cfg->bw_hz / 1000, cfg->cr, cfg->sync_word);
	return 0;
}

static int radio_emul_send(const struct device *dev, const uint8_t *data, size_t len)
{
	uint8_t buf[ENVELOPE_LEN + 256];

	ARG_UNUSED(dev);
	if (!radio.open || !radio.configured || len > 255) {
		return -EIO;
	}
	radio.sending = true;
	k_msleep(airtime_ms(&radio.cfg, len));
	put_cfg(buf, &radio.cfg, len);
	memcpy(buf + ENVELOPE_LEN, data, len);
	radio_bottom_send(buf, ENVELOPE_LEN + len);
	radio.sending = false;
	return 0;
}

static int radio_emul_receive(const struct device *dev, mesh_radio_rx_cb cb, void *user_data)
{
	ARG_UNUSED(dev);
	radio.user = user_data;
	radio.cb = cb;
	return radio.open ? 0 : -EIO;
}

static void rx_thread(void *p1, void *p2, void *p3)
{
	static uint8_t buf[ENVELOPE_LEN + 256];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		int n;

		k_msleep(POLL_MS);
		while ((n = radio_bottom_recv(buf, sizeof(buf))) > 0) {
			if (n < ENVELOPE_LEN || memcmp(buf, "CZRF", 4) != 0 ||
			    sys_get_le32(buf + 4) == radio.instance || buf[19] != n - ENVELOPE_LEN) {
				continue;
			}
			/* Different settings: a real radio would not hear it either. */
			if (!radio.configured || radio.sending || !radio.cb ||
			    sys_get_le32(buf + 8) != radio.cfg.freq_hz ||
			    sys_get_le32(buf + 12) != radio.cfg.bw_hz || buf[16] != radio.cfg.sf ||
			    buf[18] != radio.cfg.sync_word) {
				continue;
			}
			radio.cb(NULL, buf + ENVELOPE_LEN, buf[19], EMUL_RSSI, EMUL_SNR, radio.user);
		}
	}
}

K_THREAD_DEFINE(radio_emul_rx, CONFIG_ARCH_POSIX_RECOMMENDED_STACK_SIZE, rx_thread, NULL, NULL,
		NULL, K_PRIO_PREEMPT(5), 0, 0);

static const struct mesh_radio_api radio_emul_api = {
	.configure = radio_emul_configure,
	.send = radio_emul_send,
	.receive = radio_emul_receive,
};

static int radio_emul_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	radio_bottom_random(&radio.instance, sizeof(radio.instance));
	if (opt_off) {
		LOG_INF("radio switched off (--radio-off)");
		return 0;
	}
	radio.open = radio_bottom_open(opt_port) == 0;
	if (!radio.open) {
		LOG_WRN("no UDP multicast: the emulated radio is offline");
	}
	return 0;
}

static void add_options(void)
{
	static struct args_struct_t options[] = {
		{.option = "radio-port", .name = "port", .type = 'u', .dest = &opt_port,
		 .descript = "UDP port of the emulated LoRa radio (default 4403)"},
		{.option = "radio-off", .is_switch = true, .type = 'b', .dest = &opt_off,
		 .descript = "Do not connect the emulated LoRa radio"},
		{.option = "node-num", .name = "hex", .type = 's', .dest = &opt_node_num,
		 .descript = "Mesh node number (hex), instead of a random one"},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(add_options, PRE_BOOT_1, 11);

DEVICE_DT_INST_DEFINE(0, radio_emul_init, NULL, NULL, NULL, POST_KERNEL, 90, &radio_emul_api);
