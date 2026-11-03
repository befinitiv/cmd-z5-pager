#include <stdio.h>
#include <stdlib.h>
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
#include "../gps.h"
#include "meshtastic/crypto.h"
#include "meshtastic/geo.h"
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
/*
 * Current Meshtastic firmware refuses direct messages that are not
 * PKI-encrypted ("legacy DM", NAK NO_CHANNEL). A DM to a node whose public
 * key we lack waits for the key: the node is asked for its NodeInfo, the
 * message goes out when the key arrives, after KEY_WAIT_MS without PKI
 * (older firmware still takes that). A NAK because the node lacks our key
 * sends it our NodeInfo and the message again after KEY_RETRY_MS, once per
 * node every NODEINFO_REPLY_MS.
 */
#define KEY_WAIT_MS          30000
#define KEY_RETRY_MS         3000
#define KEY_WAITS            4
/* Position broadcasts, as the Meshtastic defaults (position_broadcast_secs and
 * smart broadcast): every 15 min, and after moving 100 m, at most every 30 s. */
#define POSITION_INTERVAL_MS (15 * 60 * 1000)
#define POSITION_MIN_MOVE_M  100
#define POSITION_MIN_GAP_MS  (30 * 1000)
#define POSITION_REPLY_MS    (10 * 60 * 1000)
/* "Approximate": Meshtastic precision 13, within about 2.9 km. */
#define APPROX_PRECISION     13
/* Received positions are stored again after moving this far. */
#define POSITION_SAVE_MOVE_M 100
/*
 * Time from received packets (as stock firmware, "time from net"): only
 * plausible times, from packets that came over a few hops by radio.
 */
#define MESH_TIME_MIN        1767225600 /* 2026-01-01: before this firmware */
#define MESH_TIME_MAX_HOPS   3

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
	EVT_POSITION, /* our position or the sharing setting changed */
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
static bool relay_on = true;
static struct mt_nodedb nodedb;
static struct mesh_config cfg = {.region = MT_REGION_EU_868, .preset = MT_PRESET_LONG_FAST};
/* Private channels by slot (slot 0 unused: the primary channel). */
static struct mesh_channel chans[MESH_CHANNELS];
/* Router channel index -> slot. */
static uint8_t router_slot[MT_MAX_CHANNELS];
static uint32_t my_num;
static uint8_t priv_key[32];
static bool have_priv;
static bool available;
/* The last frame from the radio (mesh_stats); the lock is the radio thread's. */
static struct k_spinlock last_rx_lock;
static int64_t last_rx_ms;
static int16_t last_rssi;
static int8_t last_snr;
static uint32_t rng;

/* Direct messages waiting for a public key (see KEY_WAIT_MS). */
static struct key_wait {
	bool used;
	bool ready; /* the key arrived: send now */
	uint16_t local_id;
	uint32_t to;
	uint32_t until;
} key_waits[KEY_WAITS];

/* A node's public key arrived: its held messages can go. */
static void key_arrived(uint32_t num)
{
	for (int i = 0; i < KEY_WAITS; i++) {
		if (key_waits[i].used && key_waits[i].to == num) {
			key_waits[i].ready = true;
		}
	}
}

/* Frames produced while the lock is held, sent afterwards. */
static struct {
	uint8_t len;
	uint8_t data[MT_MAX_FRAME];
} txbuf[MT_TXQ_LEN];
static int txcount;

/* Who we answered with our NodeInfo (and position) recently. */
struct reply_slot {
	uint32_t num;
	uint32_t time;
};

static struct reply_slot replied[8];
/* Nodes asked for their key, and nodes sent ours after a NAK. */
static struct reply_slot key_asked[4], key_sent[4];
static struct reply_slot replied_pos[8];

/* The position we sent last (as sent, i.e. rounded), for smart broadcasts. */
static struct {
	bool sent;
	int32_t lat_e7, lon_e7;
	uint8_t share;
	uint32_t time; /* uptime ms */
} pos_sent;

/* Settings ------------------------------------------------------------------ */

/*
 * Node as stored: what we learned from NodeInfo and Position packets, and
 * when it was last heard (wall clock, stored again at most once a day).
 * At most MT_MAX_NODES are stored: a node replaced in the table is deleted
 * (see nodedb.h for which; nodes with a direct-message conversation stay).
 */
struct stored_node {
	uint32_t num;
	struct mt_user user;
	bool has_position;
	struct mt_position position;
	uint32_t heard_unix; /* not in the first version */
};

#define STORED_NODE_V1 offsetof(struct stored_node, heard_unix)
BUILD_ASSERT(STORED_NODE_V1 == 152, "first version of the stored node");

#define DAY_S (24 * 3600)

/* While loading: stored nodes that did not fit, deleted afterwards. */
static bool loading_nodes;
static uint32_t drop_nodes[64];
static int ndrop;

static int mesh_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "cfg") == 0 && len == sizeof(cfg)) {
		return read_cb(cb_arg, &cfg, len) == (ssize_t)len ? 0 : -EIO;
	}
	if (strcmp(name, "ch") == 0 && len == sizeof(chans)) {
		if (read_cb(cb_arg, chans, len) != (ssize_t)len) {
			memset(chans, 0, sizeof(chans));
			return -EIO;
		}
		for (int i = 0; i < MESH_CHANNELS; i++) {
			chans[i].name[MESH_CHANNEL_NAME] = '\0';
		}
		memset(&chans[0], 0, sizeof(chans[0]));
		return 0;
	}
	if (strcmp(name, "relay") == 0 && len == sizeof(relay_on)) {
		return read_cb(cb_arg, &relay_on, len) == (ssize_t)len ? 0 : -EIO;
	}
	if (strcmp(name, "num") == 0 && len == sizeof(my_num)) {
		return read_cb(cb_arg, &my_num, len) == (ssize_t)len ? 0 : -EIO;
	}
	if (strcmp(name, "priv") == 0 && len == sizeof(priv_key)) {
		have_priv = read_cb(cb_arg, priv_key, len) == (ssize_t)len;
		return 0;
	}
	if (strncmp(name, "n/", 2) == 0 &&
	    (len == sizeof(struct stored_node) || len == STORED_NODE_V1)) {
		struct stored_node sn = {0};

		if (read_cb(cb_arg, &sn, len) == (ssize_t)len) {
			struct mt_node *n = mt_nodedb_get(&nodedb, sn.num, 0);

			mt_nodedb_set_user(n, &sn.user);
			n->has_position = sn.has_position;
			n->position = sn.position;
			n->last_heard = 0; /* unknown after a restart */
			n->heard_unix = sn.heard_unix;
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(mesh, "mesh", NULL, mesh_set, NULL, NULL);

static void save_node(const struct mt_node *n)
{
	struct stored_node sn = {.num = n->num, .user = n->user, .has_position = n->has_position,
				 .position = n->position, .heard_unix = n->heard_unix};
	char key[24];

	snprintf(key, sizeof(key), "mesh/n/%08x", n->num);
	int err = settings_save_one(key, &sn, sizeof(sn));

	if (err) {
		LOG_WRN("node %08x not stored: %d", n->num, err);
	}
}

static void delete_node(uint32_t num)
{
	char key[24];

	snprintf(key, sizeof(key), "mesh/n/%08x", num);
	settings_delete(key);
}

/* Node database hooks: keep the nodes we have a direct conversation with. */
static bool keep_node(void *ctx, const struct mt_node *n)
{
	bool keep;

	ARG_UNUSED(ctx);
	msgstore_lock();
	keep = msgstore_has_conv(n->num);
	msgstore_unlock();
	return keep;
}

static void node_evicted(void *ctx, const struct mt_node *n)
{
	ARG_UNUSED(ctx);
	if (!n->has_user) {
		return; /* never stored */
	}
	if (!loading_nodes) {
		delete_node(n->num);
	} else if (ndrop < (int)ARRAY_SIZE(drop_nodes)) {
		drop_nodes[ndrop++] = n->num; /* not while the settings are read */
	}
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

/* True if `to` may get another reply (at most one per `ms`); records it. */
static bool reply_allowed(struct reply_slot *slots, int n, uint32_t to, uint32_t ms)
{
	uint32_t now = k_uptime_get_32();
	int slot = 0;

	for (int i = 0; i < n; i++) {
		if (slots[i].num == to) {
			if (now - slots[i].time < ms) {
				return false;
			}
			slot = i;
			break;
		}
		if (now - slots[i].time > now - slots[slot].time) {
			slot = i;
		}
	}
	slots[slot].num = to;
	slots[slot].time = now;
	return true;
}

/* Answer NodeInfo requests and introduce ourselves to new nodes, rate-limited. */
static void maybe_reply_nodeinfo(uint32_t to, bool want_response)
{
	if (reply_allowed(replied, ARRAY_SIZE(replied), to, NODEINFO_REPLY_MS)) {
		send_nodeinfo(to, want_response);
	}
}

static uint32_t now_time(void)
{
	return clock_valid() ? (uint32_t)clock_unix() : 0;
}

/* For messages without time: when they came, to date them once the clock is set. */
static uint32_t uptime_mark(void)
{
	return clock_valid() ? 0 : (uint32_t)(k_uptime_get() / 1000) + 1;
}

/* Messages of this boot may still lack their time. */
static bool untimed = true;

static void date_messages(void)
{
	bool changed;

	if (!untimed || !clock_valid()) {
		return;
	}
	untimed = false;
	msgstore_lock();
	changed = msgstore_set_times((uint32_t)clock_unix(), (uint32_t)(k_uptime_get() / 1000));
	msgstore_unlock();
	if (changed) {
		notify(MESH_EV_STATUS);
	}
}

/* Our position as we may share it; false if there is none or sharing is off. */
static bool make_position(struct mt_position *p, struct gps_fix *f, uint8_t *share)
{
	struct gps_config gc;
	uint32_t bits;

	gps_get_config(&gc);
	*share = gc.share;
	if (gc.share == GPS_SHARE_OFF || !gps_position(f)) {
		return false;
	}
	bits = gc.share == GPS_SHARE_EXACT ? 32 : APPROX_PRECISION;
	*p = (struct mt_position){
		.has_latlon = true,
		.latitude_i = mt_geo_reduce(f->lat_e7, bits),
		.longitude_i = mt_geo_reduce(f->lon_e7, bits),
		.time = now_time(),
		.location_source = f->manual ? MT_LOC_MANUAL : MT_LOC_INTERNAL,
		.precision_bits = bits,
	};
	/* Altitude and satellites only with the exact position. */
	if (bits == 32 && f->has_alt) {
		p->has_altitude = true;
		p->altitude = f->alt_m;
	}
	if (bits == 32 && !f->manual) {
		p->sats_in_view = f->sats;
	}
	return true;
}

/* Send our position (lock held); false if there is nothing to share. */
static bool send_position(uint32_t to, bool want_response)
{
	struct mt_position p;
	struct gps_fix f;
	uint8_t share, buf[64];
	int n;

	if (!make_position(&p, &f, &share)) {
		return false;
	}
	n = mt_position_encode(&p, buf, sizeof(buf));
	if (n <= 0) {
		return false;
	}
	mt_router_send(&router, to, 0, MT_PORT_POSITION, buf, n, false, want_response, 0);
	if (to == MT_BROADCAST) {
		pos_sent.sent = true;
		pos_sent.lat_e7 = p.latitude_i;
		pos_sent.lon_e7 = p.longitude_i;
		pos_sent.share = share;
		pos_sent.time = k_uptime_get_32();
	}
	LOG_INF("position sent to %08x (precision %u)", to, p.precision_bits);
	return true;
}

/*
 * Smart broadcast: first position, sharing changed, or moved far enough.
 * Compared as shared, so an approximate position is only sent again when it
 * falls into another cell.
 */
static void position_changed(uint32_t now)
{
	struct mt_position p;
	struct gps_fix f;
	uint8_t share;

	if (!make_position(&p, &f, &share)) {
		pos_sent.share = share;
		return;
	}
	if (!pos_sent.sent || share != pos_sent.share ||
	    (now - pos_sent.time >= POSITION_MIN_GAP_MS &&
	     mt_geo_distance_m(pos_sent.lat_e7, pos_sent.lon_e7, p.latitude_i, p.longitude_i) >=
		     POSITION_MIN_MOVE_M)) {
		router.now = now;
		send_position(MT_BROADCAST, false);
	}
}

/* Device and environment metrics come in separate packets: keep both. */
static void merge_telemetry(struct mt_telemetry *dst, const struct mt_telemetry *src)
{
#define TAKE(has, field)                                                                   \
	do {                                                                               \
		if (src->has) {                                                            \
			dst->has = true;                                                   \
			dst->field = src->field;                                           \
		}                                                                          \
	} while (0)
	TAKE(has_battery, battery_level);
	TAKE(has_voltage, voltage);
	TAKE(has_channel_util, channel_util);
	TAKE(has_air_util_tx, air_util_tx);
	TAKE(has_uptime, uptime_s);
	TAKE(has_temperature, temperature);
	TAKE(has_humidity, humidity);
	TAKE(has_pressure, pressure);
#undef TAKE
	if (src->time) {
		dst->time = src->time;
	}
}

/* The time field of a received packet: set the clock if it has no better time. */
static void mesh_time(const struct mt_rx *rx, uint32_t t)
{
	int hops = rx->h.hop_start >= rx->h.hop_limit ? rx->h.hop_start - rx->h.hop_limit : 0;

	if (t < MESH_TIME_MIN || rx->h.via_mqtt || hops > MESH_TIME_MAX_HOPS) {
		return;
	}
	if (clock_offer(t, CLOCK_SRC_MESH)) {
		LOG_INF("clock set from %08x (%d hops)", rx->h.from, hops);
	}
}

static void cb_deliver(void *ctx, const struct mt_rx *rx)
{
	const struct mt_data *d = &rx->d;
	struct mt_node *node = mt_nodedb_find(&nodedb, rx->h.from);
	uint32_t t = now_time();

	ARG_UNUSED(ctx);
	if (node && t) {
		bool new_day = node->heard_unix / DAY_S != t / DAY_S;

		node->heard_unix = t;
		if (node->has_user && new_day) {
			save_node(node);
		}
	}
	switch (d->portnum) {
	case MT_PORT_ROUTING: {
		uint32_t err = 0;

		mt_routing_decode(d->payload, d->payload_len, &err);
		if (err) {
			LOG_INF("NAK from %08x for %08x: error %u", rx->h.from, d->request_id, err);
		}
		break;
	}
	case MT_PORT_TEXT: {
		struct msg m = {
			.conv = rx->h.to != MT_BROADCAST		 ? rx->h.from
				: rx->channel_index < router.nchannels ? conv_of_channel(router_slot[rx->channel_index])
								       : CONV_CHANNEL,
			.from = rx->h.from,
			.packet_id = rx->h.id,
			.time = now_time(),
			.uptime_s = uptime_mark(),
			.status = MSG_RECEIVED,
			.has_radio = true,
			/* hop_start 0: firmware before 2.3 does not tell. */
			.hops = rx->h.hop_start && rx->h.hop_start >= rx->h.hop_limit
					? rx->h.hop_start - rx->h.hop_limit
					: MSG_HOPS_UNKNOWN,
			.rssi = rx->rssi,
			.snr = rx->snr,
		};
		size_t n = MIN(d->payload_len, MSG_TEXT_MAX);

		memcpy(m.text, d->payload, n);
		m.text[n] = '\0';
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
			if (u.has_public_key) {
				key_arrived(rx->h.from);
			}
			notify(MESH_EV_NODES);
		}
		if (d->want_response) {
			maybe_reply_nodeinfo(rx->h.from, false);
		}
		break;
	}
	case MT_PORT_TELEMETRY: {
		struct mt_telemetry tm;

		if (mt_telemetry_decode(d->payload, d->payload_len, &tm) != 0) {
			break;
		}
		if (tm.time) {
			mesh_time(rx, tm.time);
		}
		if (node) {
			merge_telemetry(&node->telemetry, &tm);
			notify(MESH_EV_NODES);
		}
		LOG_DBG("telemetry from %08x", rx->h.from);
		break;
	}
	case MT_PORT_POSITION: {
		struct mt_position p;
		bool ok = mt_position_decode(d->payload, d->payload_len, &p) == 0;

		if (ok && p.time) {
			mesh_time(rx, p.time);
		}
		if (node && ok && p.has_latlon) {
			/* Store it when new or moved, not with every update. */
			bool save = !node->has_position ||
				    mt_geo_distance_m(node->position.latitude_i,
						      node->position.longitude_i, p.latitude_i,
						      p.longitude_i) >= POSITION_SAVE_MOVE_M;

			node->position = p;
			node->has_position = true;
			if (save) {
				save_node(node);
			}
			notify(MESH_EV_NODES);
			LOG_INF("position from %08x: %d.%07d %d.%07d", rx->h.from,
				p.latitude_i / 10000000, abs(p.latitude_i % 10000000),
				p.longitude_i / 10000000, abs(p.longitude_i % 10000000));
		}
		/* A request for our position (e.g. from the phone app). */
		if (d->want_response && rx->h.to == my_num &&
		    reply_allowed(replied_pos, ARRAY_SIZE(replied_pos), rx->h.from,
				  POSITION_REPLY_MS)) {
			send_position(rx->h.from, false);
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

static bool resend_with_our_key(uint32_t packet_id);

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
	if (st == MT_TX_NO_PUBKEY) {
		if (resend_with_our_key(id)) {
			notify(MESH_EV_STATUS);
			return;
		}
		st = MT_TX_FAILED;
	}
	msgstore_lock();
	known = msgstore_set_status(id, map[st]);
	msgstore_unlock();
	if (known) {
		notify(st == MT_TX_FAILED || st == MT_TX_NO_ACK ? MESH_EV_FAILED : MESH_EV_STATUS);
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
	K_SPINLOCK(&last_rx_lock) {
		last_rx_ms = k_uptime_get();
		last_rssi = rssi;
		last_snr = snr;
	}
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
	router_slot[0] = 0;
	router.nchannels = 1;
	for (int slot = 1; slot < MESH_CHANNELS && router.nchannels < MT_MAX_CHANNELS; slot++) {
		const struct mesh_channel *c = &chans[slot];

		if (!c->name[0]) {
			continue;
		}
		if (c->key_type == MESH_KEY_PASSPHRASE) {
			mt_channel_init(&router.channels[router.nchannels], c->name, cfg.preset, c->key,
					sizeof(c->key));
		} else {
			mt_channel_init(&router.channels[router.nchannels], c->name, cfg.preset,
					&c->number, 1);
		}
		router_slot[router.nchannels++] = slot;
	}
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

static bool load_msg(uint16_t local_id, struct msg *copy)
{
	struct msg *m;

	msgstore_lock();
	m = msgstore_find_local(local_id);
	if (m) {
		*copy = *m;
	}
	msgstore_unlock();
	return m != NULL;
}

static void send_now(uint16_t local_id)
{
	struct msg copy;
	uint32_t id;

	if (!load_msg(local_id, &copy)) {
		return;
	}
	int slot = conv_channel(copy.conv);
	uint32_t to = slot < 0 ? copy.conv : MT_BROADCAST;
	int ch = slot < 0 ? 0 : -1;

	/* A channel message goes out on that channel's router index. */
	for (int i = 0; i < router.nchannels && ch < 0; i++) {
		if (router_slot[i] == slot) {
			ch = i;
		}
	}
	id = ch < 0 ? 0
		    : mt_router_send(&router, to, ch, MT_PORT_TEXT, copy.text, strlen(copy.text),
				     true, false, 0);
	msgstore_lock();
	msgstore_set_sent(local_id, id, id ? MSG_QUEUED : MSG_FAILED);
	msgstore_unlock();
	notify(id ? MESH_EV_STATUS : MESH_EV_FAILED);
}

/* Hold a message until `until`; false if all places are taken. */
static bool hold(uint16_t local_id, uint32_t to, uint32_t until)
{
	for (int i = 0; i < KEY_WAITS; i++) {
		struct key_wait *w = &key_waits[i];

		if (!w->used) {
			*w = (struct key_wait){.used = true, .local_id = local_id, .to = to,
					       .until = until};
			return true;
		}
	}
	return false;
}

static void handle_send(uint16_t local_id)
{
	struct msg copy;
	uint32_t now = k_uptime_get_32();

	if (!load_msg(local_id, &copy)) {
		return;
	}
	/* A DM without the destination's key: ask for it first. */
	if (conv_channel(copy.conv) < 0 && router.has_keys &&
	    !mt_router_has_key(&router, copy.conv) && hold(local_id, copy.conv, now + KEY_WAIT_MS)) {
		LOG_INF("DM to %08x waits for its public key", copy.conv);
		if (reply_allowed(key_asked, ARRAY_SIZE(key_asked), copy.conv, KEY_WAIT_MS)) {
			send_nodeinfo(copy.conv, true);
		}
		return;
	}
	send_now(local_id);
}

/* Send the held messages that got their key or waited long enough. */
static void send_held(uint32_t now)
{
	for (int i = 0; i < KEY_WAITS; i++) {
		struct key_wait *w = &key_waits[i];

		if (w->used && (w->ready || (int32_t)(now - w->until) >= 0)) {
			w->used = false;
			send_now(w->local_id);
		}
	}
}

/*
 * The destination of a DM lacks our public key: send it our NodeInfo and
 * the message again shortly after. False if that is not possible (then the
 * message failed).
 */
static bool resend_with_our_key(uint32_t packet_id)
{
	uint16_t local_id = 0;
	uint32_t to = 0;

	msgstore_lock();
	for (int i = 0; i < msgstore_count(); i++) {
		struct msg *m = msgstore_get(i);

		if (m->outgoing && m->packet_id == packet_id && conv_channel(m->conv) < 0) {
			local_id = m->local_id;
			to = m->conv;
		}
	}
	msgstore_unlock();
	if (!to || !reply_allowed(key_sent, ARRAY_SIZE(key_sent), to, NODEINFO_REPLY_MS) ||
	    !hold(local_id, to, k_uptime_get_32() + KEY_RETRY_MS)) {
		return false;
	}
	LOG_INF("%08x lacks our public key: NodeInfo, then the DM again", to);
	send_nodeinfo(to, false);
	msgstore_lock();
	msgstore_set_sent(local_id, packet_id, MSG_QUEUED);
	msgstore_unlock();
	return true;
}

static void mesh_thread(void *p1, void *p2, void *p3)
{
	struct evt e;
	uint32_t next_nodeinfo = k_uptime_get_32() + NODEINFO_FIRST_MS + next_random(NULL) % 10000;
	uint32_t next_position = k_uptime_get_32() + POSITION_INTERVAL_MS;
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
			case EVT_POSITION:
				position_changed(now);
				break;
			default:
				break;
			}
		}
		router.now = now;
		send_held(now);
		date_messages();
		if ((int32_t)(now - next_nodeinfo) >= 0) {
			router.now = now;
			send_nodeinfo(MT_BROADCAST, false);
			next_nodeinfo = now + NODEINFO_INTERVAL_MS;
		}
		if ((int32_t)(now - next_position) >= 0) {
			router.now = now;
			send_position(MT_BROADCAST, false);
			next_position = now + POSITION_INTERVAL_MS;
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
	nodedb.keep = keep_node;
	nodedb.evicted = node_evicted;
	loading_nodes = true;
	settings_load_subtree("mesh");
	loading_nodes = false;
	for (int i = 0; i < ndrop; i++) {
		delete_node(drop_nodes[i]);
	}
	if (ndrop) {
		LOG_INF("%d stored nodes beyond %d deleted", ndrop, MT_MAX_NODES);
	}
	init_identity();

	mt_router_init(&router, &ops, NULL, my_num, &nodedb);
	mt_router_set_keys(&router, priv_key);
	router.rebroadcast = relay_on;

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

void mesh_position_update(void)
{
	struct evt e = {.type = EVT_POSITION};

	if (available) {
		k_msgq_put(&evq, &e, K_NO_WAIT);
	}
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

/* Channels ------------------------------------------------------------------ */

bool mesh_get_channel(int slot, struct mesh_channel *c)
{
	bool used;

	if (slot < 1 || slot >= MESH_CHANNELS) {
		return false;
	}
	k_mutex_lock(&lock, K_FOREVER);
	*c = chans[slot];
	used = c->name[0] != '\0';
	k_mutex_unlock(&lock);
	return used;
}

void mesh_set_channel(int slot, const struct mesh_channel *c)
{
	struct evt e = {.type = EVT_RECONFIG};

	if (slot < 1 || slot >= MESH_CHANNELS) {
		return;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (c) {
		chans[slot] = *c;
		chans[slot].name[MESH_CHANNEL_NAME] = '\0';
	} else {
		memset(&chans[slot], 0, sizeof(chans[slot]));
	}
	k_mutex_unlock(&lock);
	settings_save_one("mesh/ch", chans, sizeof(chans));
	if (!c) {
		/* A new channel in this slot must not inherit the old conversation. */
		msgstore_lock();
		msgstore_delete_conv(conv_of_channel(slot));
		msgstore_unlock();
		notify(MESH_EV_STATUS);
	}
	if (available) {
		k_msgq_put(&evq, &e, K_NO_WAIT);
	}
}

int mesh_free_channel(void)
{
	int slot = -1;

	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 1; i < MESH_CHANNELS && slot < 0; i++) {
		if (!chans[i].name[0]) {
			slot = i;
		}
	}
	k_mutex_unlock(&lock);
	return slot;
}

void mesh_channel_label(int slot, char *buf, size_t size)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (slot == 0) {
		snprintf(buf, size, "%s", mt_preset_name(cfg.preset));
	} else if (slot > 0 && slot < MESH_CHANNELS) {
		snprintf(buf, size, "%s", chans[slot].name);
	} else if (size) {
		buf[0] = '\0';
	}
	k_mutex_unlock(&lock);
}

void mesh_conv_name(uint32_t conv, char *buf, size_t size)
{
	int slot = conv_channel(conv);

	if (slot < 0) {
		mesh_node_name(conv, buf, size);
		return;
	}
	char label[MESH_CHANNEL_NAME + 1];

	mesh_channel_label(slot, label, sizeof(label));
	snprintf(buf, size, "#%s", label);
}

void mesh_key_from_passphrase(const char *phrase, uint8_t key[MESH_KEY_LEN])
{
	/*
	 * Typed with predictive text, the same phrase may come out with other
	 * capitals or spaces: hash it in lower case, single-spaced and trimmed.
	 */
	char norm[MSG_TEXT_MAX + 1];
	size_t n = 0;
	uint8_t digest[32];

	for (const char *p = phrase; *p && n < sizeof(norm) - 1; p++) {
		char ch = *p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p;

		if (ch == ' ' && (n == 0 || norm[n - 1] == ' ')) {
			continue;
		}
		norm[n++] = ch;
	}
	while (n && norm[n - 1] == ' ') {
		n--;
	}
	mt_sha256(norm, n, digest);
	memcpy(key, digest, MESH_KEY_LEN);
}

/* Base64url without padding, as in Meshtastic links. */
static int base64url(const uint8_t *in, size_t len, char *out, size_t size)
{
	static const char abc[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	size_t n = 0;

	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = in[i] << 16 | (i + 1 < len ? in[i + 1] << 8 : 0) |
			     (i + 2 < len ? in[i + 2] : 0);
		int chars = len - i >= 3 ? 4 : (int)(len - i) + 1;

		for (int k = 0; k < chars; k++) {
			if (n + 1 >= size) {
				return -1;
			}
			out[n++] = abc[(v >> (18 - 6 * k)) & 0x3f];
		}
	}
	out[n] = '\0';
	return n;
}

int mesh_channel_url(int slot, char *buf, size_t size)
{
	static const char prefix[] = "https://meshtastic.org/e/?add=true#";
	struct mesh_channel c;
	/* ChannelSet { settings (1) = ChannelSettings { psk (2), name (3) } } */
	uint8_t pb[4 + 2 + MESH_KEY_LEN + 2 + MESH_CHANNEL_NAME];
	size_t name_len, n = 0;
	int ret;

	if (!mesh_get_channel(slot, &c) || size < sizeof(prefix)) {
		return -1;
	}
	name_len = strlen(c.name);
	pb[n++] = 0x0a; /* field 1, length-delimited */
	pb[n++] = 0;    /* length, set below */
	pb[n++] = 0x12; /* psk */
	if (c.key_type == MESH_KEY_PASSPHRASE) {
		pb[n++] = MESH_KEY_LEN;
		memcpy(pb + n, c.key, MESH_KEY_LEN);
		n += MESH_KEY_LEN;
	} else {
		pb[n++] = 1;
		pb[n++] = c.number;
	}
	pb[n++] = 0x1a; /* name */
	pb[n++] = name_len;
	memcpy(pb + n, c.name, name_len);
	n += name_len;
	pb[1] = n - 2;

	strcpy(buf, prefix);
	ret = base64url(pb, n, buf + sizeof(prefix) - 1, size - (sizeof(prefix) - 1));
	return ret < 0 ? -1 : (int)(sizeof(prefix) - 1) + ret;
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

int mesh_send_text(uint32_t conv, const char *text)
{
	struct msg m = {
		.conv = conv,
		.from = my_num,
		.time = now_time(),
		.uptime_s = uptime_mark(),
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

bool mesh_relay(void)
{
	return relay_on;
}

void mesh_set_relay(bool on)
{
	k_mutex_lock(&lock, K_FOREVER);
	relay_on = on;
	router.rebroadcast = on;
	k_mutex_unlock(&lock);
	settings_save_one("mesh/relay", &relay_on, sizeof(relay_on));
	LOG_INF("relaying %s", on ? "on" : "off");
}

void mesh_get_stats(struct mesh_stats *s)
{
	k_mutex_lock(&lock, K_FOREVER);
	s->rx = router.rx_packets;
	s->tx = router.tx_packets;
	/* tx_relayed counts the queued rebroadcasts, some are cancelled later. */
	s->cancelled = router.relay_cancelled;
	s->relayed = router.tx_relayed - router.relay_cancelled;
	k_mutex_unlock(&lock);
	K_SPINLOCK(&last_rx_lock) {
		s->last_rx_ms = last_rx_ms;
		s->last_rssi = last_rssi;
		s->last_snr = last_snr;
	}
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

int mesh_noise_floor(int16_t *dbm)
{
	if (!available) {
		return -ENODEV;
	}
	return mesh_radio_noise(radio, dbm);
}

static bool radio_ldo;

int mesh_radio_dcdc(bool dcdc)
{
	int ret;

	if (!available) {
		return -ENODEV;
	}
	ret = mesh_radio_regulator_dcdc(radio, dcdc);
	if (ret == 0) {
		radio_ldo = !dcdc;
	}
	return ret;
}

bool mesh_radio_dcdc_on(void)
{
	return !radio_ldo;
}

static bool reset_released;

int mesh_radio_reset_line(bool driven)
{
	int ret;

	if (!available) {
		return -ENODEV;
	}
	ret = mesh_radio_reset_released(radio, !driven);
	if (ret == 0) {
		reset_released = !driven;
	}
	return ret;
}

bool mesh_radio_reset_driven(void)
{
	return !reset_released;
}

static bool rx_path_off;

int mesh_radio_rx_path(bool on)
{
	int ret;

	if (!available) {
		return -ENODEV;
	}
	ret = mesh_radio_rx_path_blocked(radio, !on);
	if (ret == 0) {
		rx_path_off = !on;
	}
	return ret;
}

bool mesh_radio_rx_path_on(void)
{
	return !rx_path_off;
}
