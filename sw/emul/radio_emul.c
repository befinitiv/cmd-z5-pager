/*
 * Emulated LoRa radio: packets travel as UDP multicast datagrams between
 * emulator instances. Each datagram carries the radio settings, so only
 * instances on the same frequency, bandwidth, spreading factor and sync word
 * receive it.
 *
 * The channel is shared like a real one: a datagram goes out when the
 * transmission starts, and receivers deliver the packet at the end of its
 * time on air. Until then the channel is busy. A sender listens before it
 * talks (waits for a free channel plus a random backoff, as channel
 * activity detection does on the board). Packets overlapping at a receiver
 * are lost, and the radio cannot receive while it transmits (half-duplex).
 *
 * Options: --radio-port=<n> (default 4403), --radio-off, --node-num=<hex>,
 * --meshcore (the MeshCore protocol instead of the stored choice).
 */

#define DT_DRV_COMPAT cmdz5_radio_emul

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>

#include <cmdline.h>
#include <posix_native_task.h>

#include <cmdz5/drivers/mesh_radio.h>

#include "emul.h"
#include "radio_bottom.h"

LOG_MODULE_REGISTER(radio_emul, CONFIG_CMDZ5_EMUL_LOG_LEVEL);

#define ENVELOPE_LEN 20
#define POLL_MS      5
#define MAX_PENDING  4
/* Backoff before sending: 0..BACKOFF_SLOTS-1 slots of SLOT_SYMBOLS symbols. */
#define BACKOFF_SLOTS 8
#define SLOT_SYMBOLS  4
#define EMUL_RSSI    (-60)
#define EMUL_SNR     9

static uint32_t opt_port = 4403;
static bool opt_off;
static char *opt_node_num;
static bool opt_meshcore;

static struct {
	bool open;
	uint32_t instance;
	struct mesh_radio_config cfg;
	bool configured;
	volatile bool sending;
	mesh_radio_rx_cb cb;
	void *user;
} radio;

/* Packets on the air towards us, delivered when their airtime has passed. */
static struct pending {
	bool used;
	bool lost; /* overlapped with another packet or our own transmission */
	int64_t done; /* uptime ms */
	uint8_t len;
	uint8_t data[256];
} pending[MAX_PENDING];

static K_MUTEX_DEFINE(air_lock);
static int64_t busy_until; /* the channel is busy (others or us) until then */

void emul_random(void *buf, size_t len)
{
	radio_bottom_random(buf, len);
}

bool emul_meshcore(void)
{
	return opt_meshcore;
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

static uint32_t slot_ms(void)
{
	uint32_t tsym = (1000u << radio.cfg.sf) / radio.cfg.bw_hz;

	return MAX(tsym * SLOT_SYMBOLS, 1);
}

static int radio_emul_send(const struct device *dev, const uint8_t *data, size_t len)
{
	uint8_t buf[ENVELOPE_LEN + 256];
	uint32_t air;

	ARG_UNUSED(dev);
	if (!radio.open || !radio.configured || len > 255) {
		return -EIO;
	}
	air = airtime_ms(&radio.cfg, len);

	/* Listen before talk: a free channel, then a random backoff, still free. */
	for (;;) {
		int64_t wait;
		uint32_t r;

		k_mutex_lock(&air_lock, K_FOREVER);
		wait = busy_until - k_uptime_get();
		k_mutex_unlock(&air_lock);
		if (wait > 0) {
			k_msleep(wait);
			continue;
		}
		emul_random(&r, sizeof(r));
		k_msleep(r % BACKOFF_SLOTS * slot_ms());
		k_mutex_lock(&air_lock, K_FOREVER);
		if (busy_until <= k_uptime_get()) {
			break; /* lock held */
		}
		k_mutex_unlock(&air_lock);
	}
	/* Transmitting: whatever is arriving now is lost to us. */
	radio.sending = true;
	busy_until = k_uptime_get() + air;
	for (int i = 0; i < MAX_PENDING; i++) {
		pending[i].lost |= pending[i].used;
	}
	k_mutex_unlock(&air_lock);

	put_cfg(buf, &radio.cfg, len);
	memcpy(buf + ENVELOPE_LEN, data, len);
	radio_bottom_send(buf, ENVELOPE_LEN + len);
	k_msleep(air);
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
		static struct pending done;
		int n;

		k_msleep(POLL_MS);
		while ((n = radio_bottom_recv(buf, sizeof(buf))) > 0) {
			if (n < ENVELOPE_LEN || memcmp(buf, "CZRF", 4) != 0 ||
			    sys_get_le32(buf + 4) == radio.instance || buf[19] != n - ENVELOPE_LEN) {
				continue;
			}
			/* Different settings: a real radio would not hear it either. */
			if (!radio.configured || !radio.cb ||
			    sys_get_le32(buf + 8) != radio.cfg.freq_hz ||
			    sys_get_le32(buf + 12) != radio.cfg.bw_hz || buf[16] != radio.cfg.sf ||
			    buf[18] != radio.cfg.sync_word) {
				continue;
			}
			/* The packet is on the air from now until the end of its airtime. */
			int64_t end = k_uptime_get() + airtime_ms(&radio.cfg, buf[19]);
			bool overlap = radio.sending;
			struct pending *slot = NULL;

			k_mutex_lock(&air_lock, K_FOREVER);
			for (int i = 0; i < MAX_PENDING; i++) {
				if (pending[i].used) {
					pending[i].lost = true; /* two packets at once */
					overlap = true;
				} else if (!slot) {
					slot = &pending[i];
				}
			}
			busy_until = MAX(busy_until, end);
			if (slot) {
				slot->used = true;
				slot->lost = overlap;
				slot->done = end;
				slot->len = buf[19];
				memcpy(slot->data, buf + ENVELOPE_LEN, buf[19]);
			}
			k_mutex_unlock(&air_lock);
		}

		/* Deliver what has been received completely (copied out of the lock). */
		for (int i = 0; i < MAX_PENDING; i++) {
			bool finished = false;

			k_mutex_lock(&air_lock, K_FOREVER);
			if (pending[i].used && pending[i].done <= k_uptime_get()) {
				done = pending[i];
				pending[i].used = false;
				finished = true;
			}
			k_mutex_unlock(&air_lock);
			if (finished && done.lost) {
				LOG_DBG("packet lost in a collision");
			} else if (finished) {
				radio.cb(NULL, done.data, done.len, EMUL_RSSI, EMUL_SNR, radio.user);
			}
		}
	}
}

K_THREAD_DEFINE(radio_emul_rx, CONFIG_ARCH_POSIX_RECOMMENDED_STACK_SIZE, rx_thread, NULL, NULL,
		NULL, K_PRIO_PREEMPT(5), 0, 0);

/* A plausible noise floor for LongFast that wanders a little. */
static int radio_emul_noise(const struct device *dev, int16_t *dbm)
{
	ARG_UNUSED(dev);
	if (!radio.open || !radio.configured || radio.sending) {
		return -EBUSY;
	}
	*dbm = -118 + (int16_t)(sys_rand32_get() % 4);
	return 0;
}

static int radio_emul_regulator_dcdc(const struct device *dev, bool dcdc)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(dcdc);
	return 0; /* nothing to switch */
}

static int radio_emul_reset_released(const struct device *dev, bool released)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(released);
	return 0; /* no line */
}

static int radio_emul_rx_path_blocked(const struct device *dev, bool blocked)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(blocked);
	return 0; /* no switch */
}

static const struct mesh_radio_api radio_emul_api = {
	.configure = radio_emul_configure,
	.send = radio_emul_send,
	.receive = radio_emul_receive,
	.noise = radio_emul_noise,
	.regulator_dcdc = radio_emul_regulator_dcdc,
	.reset_released = radio_emul_reset_released,
	.rx_path_blocked = radio_emul_rx_path_blocked,
};

static int radio_emul_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	/* native_sim sets options that were not given to UINT32_MAX. */
	if (opt_port == UINT32_MAX) {
		opt_port = 4403;
	}
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
		{.option = "meshcore", .is_switch = true, .type = 'b', .dest = &opt_meshcore,
		 .descript = "Use the MeshCore protocol"},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(add_options, PRE_BOOT_1, 11);

DEVICE_DT_INST_DEFINE(0, radio_emul_init, NULL, NULL, NULL, POST_KERNEL, 90, &radio_emul_api);
