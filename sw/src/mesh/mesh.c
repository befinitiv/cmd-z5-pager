#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>

#include <cmdz5/drivers/mesh_radio.h>

#include "../app.h"
#include "../clock.h"
#include "meshtastic/router.h"
#include "mesh.h"
#include "msgstore.h"

#ifdef CONFIG_CMDZ5_EMUL
#include "../../emul/emul.h"
#endif

LOG_MODULE_REGISTER(mesh, CONFIG_CMDZ5_APP_LOG_LEVEL);

#define NODEINFO_INTERVAL_MS (3 * 3600 * 1000) /* as the Meshtastic default */
#define NODEINFO_FIRST_MS    5000
#define NODEINFO_REPLY_MS    (10 * 60 * 1000)  /* answer one node at most every 10 min */
#define MAX_WAIT_MS          1000

#define RADIO_NODE DT_NODELABEL(mesh_radio)
#if DT_NODE_HAS_STATUS_OKAY(RADIO_NODE)
static const struct device *const radio = DEVICE_DT_GET(RADIO_NODE);
#else
static const struct device *const radio;
#endif

enum evt_type {
	EVT_RX,
	EVT_SEND,
	EVT_RECONFIG,
};

struct evt {
	uint8_t type;
	uint8_t len;
	int16_t rssi;
	int8_t snr;
	uint16_t local_id; /* EVT_SEND */
	uint8_t data[MT_MAX_FRAME];
};

K_MSGQ_DEFINE(evq, sizeof(struct evt), 8, 4);
static K_MUTEX_DEFINE(lock);

static struct mt_router router;
static struct mt_nodedb nodedb;
static struct mesh_config cfg = {.region = MT_REGION_EU_868, .preset = MT_PRESET_LONG_FAST};
static uint32_t my_num;
static uint8_t priv_key[32];
static bool have_priv;
static bool available;
static uint32_t rng;

/* Frames produced while the lock is held, sent afterwards. */
static struct {
	uint8_t len;
	uint8_t data[MT_MAX_FRAME];
} txbuf[MT_TXQ_LEN];
static int txcount;

/* Who we answered with our NodeInfo recently. */
static struct {
	uint32_t num;
	uint32_t time;
} replied[8];

/* Settings ------------------------------------------------------------------ */

/* Node as stored: what we learned from NodeInfo and Position packets. */
struct stored_node {
	uint32_t num;
	struct mt_user user;
	bool has_position;
	struct mt_position position;
};

static int mesh_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "cfg") == 0 && len == sizeof(cfg)) {
		return read_cb(cb_arg, &cfg, len) == (ssize_t)len ? 0 : -EIO;
	}
	if (strcmp(name, "num") == 0 && len == sizeof(my_num)) {
		return read_cb(cb_arg, &my_num, len) == (ssize_t)len ? 0 : -EIO;
	}
	if (strcmp(name, "priv") == 0 && len == sizeof(priv_key)) {
		have_priv = read_cb(cb_arg, priv_key, len) == (ssize_t)len;
		return 0;
	}
	if (strncmp(name, "n/", 2) == 0 && len == sizeof(struct stored_node)) {
		struct stored_node sn;

		if (read_cb(cb_arg, &sn, len) == (ssize_t)len) {
			struct mt_node *n = mt_nodedb_get(&nodedb, sn.num, 0);

			mt_nodedb_set_user(n, &sn.user);
			n->has_position = sn.has_position;
			n->position = sn.position;
			n->last_heard = 0; /* unknown after a restart */
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(mesh, "mesh", NULL, mesh_set, NULL, NULL);

static void save_node(const struct mt_node *n)
{
	struct stored_node sn = {.num = n->num, .user = n->user, .has_position = n->has_position,
				 .position = n->position};
	char key[24];

	snprintf(key, sizeof(key), "mesh/n/%08x", n->num);
	settings_save_one(key, &sn, sizeof(sn));
}

/* Identity ------------------------------------------------------------------- */

static void random_bytes(void *buf, size_t len)
{
#ifdef CONFIG_CMDZ5_EMUL
	emul_random(buf, len);
#else
	sys_csrand_get(buf, len);
#endif
}

static uint32_t next_random(void *ctx)
{
	ARG_UNUSED(ctx);
	/* xorshift32, seeded from the identity and the entropy source. */
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static bool valid_num(uint32_t n)
{
	return n >= 4 && n != MT_BROADCAST;
}

static void init_identity(void)
{
	bool save = false;

#ifdef CONFIG_CMDZ5_EMUL
	if (emul_node_num()) {
		save = my_num != emul_node_num();
		my_num = emul_node_num();
	}
#endif
	if (!valid_num(my_num)) {
		uint8_t id[8];
		ssize_t n = hwinfo_get_device_id(id, sizeof(id));

		my_num = 0;
#ifndef CONFIG_CMDZ5_EMUL
		/* The board: the last 4 bytes of the chip's device id. */
		if (n >= 4) {
			my_num = id[n - 4] << 24 | id[n - 3] << 16 | id[n - 2] << 8 | id[n - 1];
		}
#else
		ARG_UNUSED(n);
#endif
		while (!valid_num(my_num)) {
			random_bytes(&my_num, sizeof(my_num));
		}
		save = true;
	}
	if (save) {
		settings_save_one("mesh/num", &my_num, sizeof(my_num));
	}
	if (!have_priv) {
		random_bytes(priv_key, sizeof(priv_key));
		settings_save_one("mesh/priv", priv_key, sizeof(priv_key));
		have_priv = true;
	}
	if (!cfg.long_name[0]) {
		snprintf(cfg.long_name, sizeof(cfg.long_name), "CMD-Z5 %04x", my_num & 0xffff);
		snprintf(cfg.short_name, sizeof(cfg.short_name), "%04x", my_num & 0xffff);
	}
	random_bytes(&rng, sizeof(rng));
	rng ^= my_num;
	if (!rng) {
		rng = 1;
	}
}

/* Router callbacks (mesh thread, lock held) ----------------------------------- */

static void cb_transmit(void *ctx, const uint8_t *frame, size_t len)
{
	ARG_UNUSED(ctx);
	if (txcount < (int)ARRAY_SIZE(txbuf)) {
		txbuf[txcount].len = len;
		memcpy(txbuf[txcount].data, frame, len);
		txcount++;
	}
}

static void notify(enum mesh_event e)
{
	struct ui_event ev = {.type = APP_EV_MESH, .value = e};

	app_post(&ev);
}

static void send_nodeinfo(uint32_t to, bool want_response)
{
	struct mt_user u = {.hw_model = MT_HW_PRIVATE, .has_public_key = true};
	uint8_t buf[128];
	int n;

	snprintf(u.id, sizeof(u.id), "!%08x", my_num);
	strncpy(u.long_name, cfg.long_name, sizeof(u.long_name) - 1);
	strncpy(u.short_name, cfg.short_name, sizeof(u.short_name) - 1);
	memcpy(u.public_key, router.pub, 32);
	n = mt_user_encode(&u, buf, sizeof(buf));
	if (n > 0) {
		mt_router_send(&router, to, 0, MT_PORT_NODEINFO, buf, n, false, want_response, 0);
	}
}

/* Answer NodeInfo requests and introduce ourselves to new nodes, rate-limited. */
static void maybe_reply_nodeinfo(uint32_t to, bool want_response)
{
	uint32_t now = k_uptime_get_32();
	int slot = 0;

	for (int i = 0; i < (int)ARRAY_SIZE(replied); i++) {
		if (replied[i].num == to) {
			if (now - replied[i].time < NODEINFO_REPLY_MS) {
				return;
			}
			slot = i;
			break;
		}
		if (now - replied[i].time > now - replied[slot].time) {
			slot = i;
		}
	}
	replied[slot].num = to;
	replied[slot].time = now;
	send_nodeinfo(to, want_response);
}

static uint32_t now_time(void)
{
	return clock_valid() ? (uint32_t)clock_unix() : 0;
}

static void cb_deliver(void *ctx, const struct mt_rx *rx)
{
	const struct mt_data *d = &rx->d;
	struct mt_node *node = mt_nodedb_find(&nodedb, rx->h.from);

	ARG_UNUSED(ctx);
	switch (d->portnum) {
	case MT_PORT_TEXT: {
		struct msg m = {
			.conv = rx->h.to == MT_BROADCAST ? CONV_CHANNEL : rx->h.from,
			.from = rx->h.from,
			.packet_id = rx->h.id,
			.time = now_time(),
			.status = MSG_RECEIVED,
		};
		size_t n = MIN(d->payload_len, MSG_TEXT_MAX);

		memcpy(m.text, d->payload, n);
		m.text[n] = '\0';
		if (rx->h.to == MT_BROADCAST && rx->channel_index != 0) {
			break; /* only the primary channel is shown for now */
		}
		msgstore_lock();
		msgstore_add(&m);
		msgstore_unlock();
		notify(MESH_EV_MESSAGE);
		LOG_INF("text from %08x: %s", rx->h.from, m.text);
		break;
	}
	case MT_PORT_NODEINFO: {
		struct mt_user u;

		if (node && mt_user_decode(d->payload, d->payload_len, &u) == 0) {
			bool changed = !node->has_user || memcmp(&node->user, &u, sizeof(u)) != 0;

			mt_nodedb_set_user(node, &u);
			if (changed) {
				save_node(node);
			}
			notify(MESH_EV_NODES);
		}
		if (d->want_response) {
			maybe_reply_nodeinfo(rx->h.from, false);
		}
		break;
	}
	case MT_PORT_POSITION: {
		struct mt_position p;

		if (node && mt_position_decode(d->payload, d->payload_len, &p) == 0 &&
		    p.has_latlon) {
			node->position = p;
			node->has_position = true;
			notify(MESH_EV_NODES);
		}
		break;
	}
	default:
		break;
	}
	/* A node we know nothing about: introduce ourselves and ask for its info. */
	if (node && !node->has_user && d->portnum != MT_PORT_NODEINFO) {
		maybe_reply_nodeinfo(rx->h.from, true);
	}
}

static void cb_status(void *ctx, uint32_t id, enum mt_tx_status st)
{
	static const uint8_t map[] = {
		[MT_TX_SENT] = MSG_SENT,
		[MT_TX_ACKED] = MSG_DELIVERED,
		[MT_TX_NO_ACK] = MSG_NO_ACK,
		[MT_TX_FAILED] = MSG_FAILED,
	};
	bool known;

	ARG_UNUSED(ctx);
	msgstore_lock();
	known = msgstore_set_status(id, map[st]);
	msgstore_unlock();
	if (known) {
		notify(MESH_EV_STATUS);
	}
}

static const struct mt_router_ops ops = {
	.transmit = cb_transmit,
	.deliver = cb_deliver,
	.status = cb_status,
	.random = next_random,
};

/* Radio -------------------------------------------------------------------------- */

static void on_rx(const struct device *dev, const uint8_t *data, size_t len, int16_t rssi,
		  int8_t snr, void *user_data)
{
	struct evt e = {.type = EVT_RX, .len = len, .rssi = rssi, .snr = snr};

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	if (len > MT_MAX_FRAME) {
		return;
	}
	memcpy(e.data, data, len);
	if (k_msgq_put(&evq, &e, K_NO_WAIT) != 0) {
		LOG_WRN("rx queue full, packet dropped");
	}
}

/* Apply the radio settings; lock held. */
static void apply_radio(void)
{
	static const uint8_t default_psk = 1;
	struct mt_radio_params p;

	mt_channel_init(&router.channels[0], "", cfg.preset, &default_psk, 1);
	router.nchannels = 1;
	if (mt_radio_params(cfg.region, cfg.preset, mt_channel_name(&router.channels[0], cfg.preset),
			    &p) != 0) {
		cfg.region = MT_REGION_EU_868;
		cfg.preset = MT_PRESET_LONG_FAST;
		mt_radio_params(cfg.region, cfg.preset, "LongFast", &p);
	}
	mt_router_set_radio(&router, &p);

	struct mesh_radio_config rc = {
		.freq_hz = p.freq_hz,
		.bw_hz = p.bw_hz,
		.sf = p.sf,
		.cr = p.cr,
		.sync_word = p.sync_word,
		.preamble = p.preamble,
		.power_dbm = p.power_dbm,
	};

	mesh_radio_configure(radio, &rc);
}

/* Send what the router produced, outside the lock (sending takes the airtime). */
static void flush_tx(void)
{
	static uint8_t frame[MT_MAX_FRAME];

	for (;;) {
		uint8_t len;

		k_mutex_lock(&lock, K_FOREVER);
		if (txcount == 0) {
			k_mutex_unlock(&lock);
			return;
		}
		len = txbuf[0].len;
		memcpy(frame, txbuf[0].data, len);
		memmove(&txbuf[0], &txbuf[1], (txcount - 1) * sizeof(txbuf[0]));
		txcount--;
		k_mutex_unlock(&lock);
		mesh_radio_send(radio, frame, len);
	}
}

static void handle_send(uint16_t local_id)
{
	struct msg copy;
	struct msg *m;
	uint32_t id;

	msgstore_lock();
	m = msgstore_find_local(local_id);
	if (m) {
		copy = *m;
	}
	msgstore_unlock();
	if (!m) {
		return;
	}
	uint32_t to = copy.conv == CONV_CHANNEL ? MT_BROADCAST : copy.conv;

	id = mt_router_send(&router, to, 0, MT_PORT_TEXT, copy.text, strlen(copy.text), true, false,
			    0);
	msgstore_lock();
	msgstore_set_sent(local_id, id, id ? MSG_QUEUED : MSG_FAILED);
	msgstore_unlock();
	notify(MESH_EV_STATUS);
}

static void mesh_thread(void *p1, void *p2, void *p3)
{
	struct evt e;
	uint32_t next_nodeinfo = k_uptime_get_32() + NODEINFO_FIRST_MS + next_random(NULL) % 10000;
	uint32_t wait = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		int got = k_msgq_get(&evq, &e, K_MSEC(MIN(wait, MAX_WAIT_MS)));
		uint32_t now = k_uptime_get_32();

		k_mutex_lock(&lock, K_FOREVER);
		if (got == 0) {
			switch (e.type) {
			case EVT_RX:
				mt_router_receive(&router, e.data, e.len, e.rssi, e.snr, now);
				break;
			case EVT_SEND:
				router.now = now;
				handle_send(e.local_id);
				break;
			case EVT_RECONFIG:
				apply_radio();
				next_nodeinfo = now;
				break;
			default:
				break;
			}
		}
		if ((int32_t)(now - next_nodeinfo) >= 0) {
			router.now = now;
			send_nodeinfo(MT_BROADCAST, false);
			next_nodeinfo = now + NODEINFO_INTERVAL_MS;
		}
		wait = mt_router_poll(&router, now);
		k_mutex_unlock(&lock);
		flush_tx();
	}
}

K_THREAD_STACK_DEFINE(mesh_stack, 6144);
static struct k_thread mesh_tid;

/* Public API ---------------------------------------------------------------------- */

int mesh_init(void)
{
	msgstore_init();
	mt_nodedb_init(&nodedb);
	settings_load_subtree("mesh");
	init_identity();

	mt_router_init(&router, &ops, NULL, my_num, &nodedb);
	mt_router_set_keys(&router, priv_key);

	if (!radio || !device_is_ready(radio)) {
		LOG_WRN("no mesh radio on this board yet");
		return -ENODEV;
	}
	apply_radio();
	if (mesh_radio_receive(radio, on_rx, NULL) != 0) {
		LOG_WRN("mesh radio not available");
		return -EIO;
	}
	available = true;
	LOG_INF("node !%08x \"%s\" on %s, %s", my_num, cfg.long_name, mt_region_name(cfg.region),
		mt_preset_name(cfg.preset));
	k_thread_create(&mesh_tid, mesh_stack, K_THREAD_STACK_SIZEOF(mesh_stack), mesh_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	k_thread_name_set(&mesh_tid, "mesh");
	return 0;
}

bool mesh_available(void)
{
	return available;
}

uint32_t mesh_my_num(void)
{
	return my_num;
}

const char *mesh_channel_name(void)
{
	return mt_preset_name(cfg.preset);
}

void mesh_get_config(struct mesh_config *c)
{
	k_mutex_lock(&lock, K_FOREVER);
	*c = cfg;
	k_mutex_unlock(&lock);
}

void mesh_set_config(const struct mesh_config *c)
{
	struct evt e = {.type = EVT_RECONFIG};

	k_mutex_lock(&lock, K_FOREVER);
	cfg = *c;
	k_mutex_unlock(&lock);
	settings_save_one("mesh/cfg", &cfg, sizeof(cfg));
	if (available) {
		k_msgq_put(&evq, &e, K_NO_WAIT);
	}
}

int mesh_send_text(uint32_t to, const char *text)
{
	struct msg m = {
		.conv = to == MT_BROADCAST ? CONV_CHANNEL : to,
		.from = my_num,
		.time = now_time(),
		.outgoing = true,
		.read = true,
		.status = available ? MSG_QUEUED : MSG_FAILED,
	};
	struct evt e = {.type = EVT_SEND};

	strncpy(m.text, text, MSG_TEXT_MAX);
	msgstore_lock();
	e.local_id = msgstore_add(&m);
	msgstore_unlock();
	if (!available) {
		return -ENODEV;
	}
	return k_msgq_put(&evq, &e, K_NO_WAIT) == 0 ? 0 : -EBUSY;
}

void mesh_lock(void)
{
	k_mutex_lock(&lock, K_FOREVER);
}

void mesh_unlock(void)
{
	k_mutex_unlock(&lock);
}

struct mt_nodedb *mesh_nodes(void)
{
	return &nodedb;
}

const char *mesh_node_name(uint32_t num, char *buf, int size)
{
	char tmp[16];

	if (num == my_num) {
		snprintf(buf, size, "%s", cfg.long_name);
		return buf;
	}
	k_mutex_lock(&lock, K_FOREVER);
	struct mt_node *n = mt_nodedb_find(&nodedb, num);

	if (n) {
		snprintf(buf, size, "%s", mt_node_name(n, tmp, sizeof(tmp)));
	} else {
		snprintf(buf, size, "!%08x", num);
	}
	k_mutex_unlock(&lock);
	return buf;
}

uint32_t mesh_uptime_s(void)
{
	return k_uptime_get_32() / 1000;
}
