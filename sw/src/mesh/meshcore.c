/*
 * MeshCore mode of the mesh service: runs the MeshCore client (lib/meshcore)
 * in its own thread instead of the Meshtastic router. Contacts live in the
 * shared node database (number: the first 4 bytes of the public key, user
 * info from the advert, role: the advert type); their routes here.
 *
 * Stored under "mc/": the identity (key), MeshCore settings (cfg), private
 * channels (ch), contacts (n/<num>, with their route) and the time of our
 * last advert (advts), which keeps advert times rising without a clock.
 *
 * Adverts: flooded 10-15 s after the start (once the clock is set, or after
 * 2 minutes without), then every 12 hours, and from Settings.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>

#include <cmdz5/drivers/mesh_radio.h>

#include "../clock.h"
#include "../gps.h"
#include "../i18n.h"
#include "meshcore/crypto.h"
#include "meshcore/mesh.h"
#include "meshtastic/geo.h"
#include "mesh_internal.h"
#include "msgstore.h"

#ifdef CONFIG_CMDZ5_EMUL
#include "../../emul/emul.h"
#endif

LOG_MODULE_REGISTER(meshcore, CONFIG_CMDZ5_APP_LOG_LEVEL);

#define ADVERT_FIRST_MS     10000
#define ADVERT_JITTER_MS    5000
#define ADVERT_CLOCK_WAIT_MS (2 * 60 * 1000)
#define ADVERT_INTERVAL_MS  (12 * 3600 * 1000)
#define MAX_WAIT_MS         1000
#define POWER_DBM           22
#define SYNC_WORD           0x12 /* RadioLib's "private" LoRa sync word */
/* As in mesh.c: Meshtastic's "approximate" position, about 6 km. */
#define APPROX_PRECISION    13
/* Times from messages only if plausible and from few hops (see mesh.c). */
#define MESH_TIME_MIN       1767225600
#define MESH_TIME_MAX_HOPS  3
#define DAY_S               (24 * 3600)

/* Radio presets of the MeshCore app. */
static const struct {
	const char *name;
	uint32_t freq_hz;
	uint32_t bw_hz;
	uint8_t sf, cr;
} presets[] = {
	{"EU narrow", 869618000, 62500, 8, 5},
	{"EU long", 869525000, 250000, 11, 5},
	{"EU medium", 869525000, 250000, 10, 5},
	{"US narrow", 910525000, 62500, 7, 5},
};

enum evt_type {
	EVT_RX,
	EVT_SEND,
	EVT_RECONFIG,
	EVT_ADVERT,
};

struct evt {
	uint8_t type;
	uint8_t len;
	int16_t rssi;
	int8_t snr;
	uint16_t local_id;
	uint8_t data[MC_MAX_FRAME];
};

K_MSGQ_DEFINE(mc_evq, sizeof(struct evt), 6, 4);

static const struct device *radio;
static struct mc_mesh mesh;
static struct mesh_mc_config cfg;
static uint8_t pub_key[32], prv_key[64];
static bool have_key;
static uint32_t my_num;
static uint32_t last_ts;   /* our last timestamp (adverts, messages) */
static uint32_t advert_ts; /* stored: our last advert's */
static bool started;

/* The last frame from the radio (mesh_stats). */
static struct k_spinlock rx_lock;
static int64_t last_rx_ms;
static int16_t last_rssi;
static int8_t last_snr;

/* Frames produced while the lock is held, sent afterwards. */
static struct {
	uint8_t len;
	uint8_t data[MC_MAX_FRAME];
} txbuf[MC_TXQ_LEN];
static int txcount;

/* Routes and advert times of the contacts in the node database. */
static struct route {
	uint32_t num;
	uint8_t path_len; /* MC_PATH_UNKNOWN: flood */
	uint8_t path[MC_MAX_PATH];
	uint32_t advert_ts;
} routes[MT_MAX_NODES];

/* A contact as stored. */
struct stored_contact {
	uint8_t pub[32];
	char name[MC_NAME_MAX + 1];
	uint8_t type;
	bool has_position;
	int32_t lat_e7, lon_e7;
	uint32_t advert_ts;
	uint32_t heard_unix;
	uint8_t path_len;
	uint8_t path[MC_MAX_PATH];
};

/* A name cut to MeshCore's length, at a character boundary. */
static void copy_name(char out[MC_NAME_MAX + 1], const char *name)
{
	size_t n = mc_utf8_prefix(name, MC_NAME_MAX);

	memcpy(out, name, n);
	out[n] = '\0';
}

static uint32_t num_of(const uint8_t pub[32])
{
	return (uint32_t)pub[0] << 24 | pub[1] << 16 | pub[2] << 8 | pub[3];
}

/* Node numbers that cannot be confused with the broadcast or channel conversations. */
static bool valid_num(uint32_t n)
{
	return n >= 4 && n < CONV_CHANNEL - CONV_CHANNELS;
}

static bool node_mine(const struct mt_node *n)
{
	return n->has_user && n->user.has_public_key;
}

/* The route entry of a node (a free or stale one if new). */
static struct route *route_of(uint32_t num)
{
	struct route *free_slot = NULL;

	for (int i = 0; i < MT_MAX_NODES; i++) {
		if (routes[i].num == num) {
			return &routes[i];
		}
		if (!free_slot && (routes[i].num == 0 || !mt_nodedb_find(mesh_nodes(), routes[i].num))) {
			free_slot = &routes[i];
		}
	}
	if (free_slot) {
		*free_slot = (struct route){.num = num, .path_len = MC_PATH_UNKNOWN};
	}
	return free_slot;
}

/* Settings --------------------------------------------------------------------------- */

static void save_contact(const struct mt_node *n)
{
	struct route *r = route_of(n->num);
	struct stored_contact sc = {
		.type = n->user.role,
		.has_position = n->has_position,
		.lat_e7 = n->position.latitude_i,
		.lon_e7 = n->position.longitude_i,
		.heard_unix = n->heard_unix,
		.path_len = MC_PATH_UNKNOWN,
	};
	char key[24];

	memcpy(sc.pub, n->user.public_key, 32);
	copy_name(sc.name, n->user.long_name);
	if (r) {
		sc.advert_ts = r->advert_ts;
		sc.path_len = r->path_len;
		memcpy(sc.path, r->path, sizeof(sc.path));
	}
	snprintf(key, sizeof(key), "mc/n/%08x", n->num);
	if (settings_save_one(key, &sc, sizeof(sc)) != 0) {
		LOG_WRN("contact %08x not stored", n->num);
	}
}

static void user_of(struct mt_user *u, const uint8_t pub[32], const char *name, uint8_t type)
{
	memset(u, 0, sizeof(*u));
	snprintf(u->id, sizeof(u->id), "!%08x", num_of(pub));
	snprintf(u->long_name, sizeof(u->long_name), "%s", name);
	/* The short name: the hash that stands for the node in paths. */
	snprintf(u->short_name, sizeof(u->short_name), "%02x", pub[0]);
	u->role = type;
	u->has_public_key = true;
	memcpy(u->public_key, pub, 32);
}

static void load_contact(const struct stored_contact *sc)
{
	uint32_t num = num_of(sc->pub);
	struct mt_node *n;
	struct mt_user u;
	struct route *r;

	if (!valid_num(num)) {
		return;
	}
	n = mt_nodedb_get(mesh_nodes(), num, 0);
	user_of(&u, sc->pub, sc->name, sc->type);
	mt_nodedb_set_user(n, &u);
	n->has_position = sc->has_position;
	n->position = (struct mt_position){.has_latlon = true, .latitude_i = sc->lat_e7,
					   .longitude_i = sc->lon_e7, .precision_bits = 32};
	n->last_heard = 0;
	n->heard_unix = sc->heard_unix;
	r = route_of(num);
	if (r) {
		r->advert_ts = sc->advert_ts;
		r->path_len = mc_path_valid(sc->path_len) ? sc->path_len : MC_PATH_UNKNOWN;
		memcpy(r->path, sc->path, sizeof(r->path));
	}
}

static int mc_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "cfg") == 0 && len == sizeof(cfg)) {
		if (read_cb(cb_arg, &cfg, len) != (ssize_t)len) {
			memset(&cfg, 0, sizeof(cfg));
			return -EIO;
		}
		cfg.scope[sizeof(cfg.scope) - 1] = '\0';
		return 0;
	}
	if (strcmp(name, "key") == 0 && len == sizeof(prv_key) + sizeof(pub_key)) {
		uint8_t buf[sizeof(prv_key) + sizeof(pub_key)];

		have_key = read_cb(cb_arg, buf, len) == (ssize_t)len;
		memcpy(prv_key, buf, sizeof(prv_key));
		memcpy(pub_key, buf + sizeof(prv_key), sizeof(pub_key));
		return 0;
	}
	if (strcmp(name, "advts") == 0 && len == sizeof(advert_ts)) {
		return read_cb(cb_arg, &advert_ts, len) == (ssize_t)len ? 0 : -EIO;
	}
	if (strcmp(name, "ch") == 0) {
		return mesh_channels_read(len, read_cb, cb_arg);
	}
	if (strncmp(name, "n/", 2) == 0 && len == sizeof(struct stored_contact)) {
		struct stored_contact sc;

		if (read_cb(cb_arg, &sc, len) == (ssize_t)len) {
			sc.name[MC_NAME_MAX] = '\0';
			load_contact(&sc);
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(meshcore, "mc", NULL, mc_set, NULL, NULL);

/* Identity and time -------------------------------------------------------------------- */

static void random_bytes(void *buf, size_t len)
{
#ifdef CONFIG_CMDZ5_EMUL
	emul_random(buf, len);
#else
	sys_csrand_get(buf, len);
#endif
}

static void init_identity(void)
{
	/* As MeshCore: hashes 00 and ff are reserved. */
	while (!have_key || pub_key[0] == 0x00 || pub_key[0] == 0xff || !valid_num(num_of(pub_key))) {
		uint8_t seed[32], buf[sizeof(prv_key) + sizeof(pub_key)];

		random_bytes(seed, sizeof(seed));
		mc_ed25519_keypair(pub_key, prv_key, seed);
		memcpy(buf, prv_key, sizeof(prv_key));
		memcpy(buf + sizeof(prv_key), pub_key, sizeof(pub_key));
		settings_save_one("mc/key", buf, sizeof(buf));
		have_key = true;
	}
	my_num = num_of(pub_key);
}

/*
 * Our timestamps (adverts, messages): the clock, or without it the last
 * advert's time plus the uptime; always rising, as receivers expect.
 */
static uint32_t timestamp(void)
{
	uint32_t t = clock_valid() ? (uint32_t)clock_unix()
				   : advert_ts + (uint32_t)(k_uptime_get() / 1000);

	if (t <= last_ts) {
		t = last_ts + 1;
	}
	last_ts = t;
	return t;
}

/* A sender's time: set the clock from it if there is no better time. */
static void offer_time(uint32_t t, uint8_t hops, uint32_t from)
{
	if (t >= MESH_TIME_MIN && hops <= MESH_TIME_MAX_HOPS && clock_offer(t, CLOCK_SRC_MESH)) {
		LOG_INF("clock set from %08x (%u hops)", from, hops);
	}
}

/* Client callbacks (mesh thread, lock held) ------------------------------------------------ */

static void cb_transmit(void *ctx, const uint8_t *frame, size_t len)
{
	ARG_UNUSED(ctx);
	if (txcount < (int)ARRAY_SIZE(txbuf)) {
		txbuf[txcount].len = len;
		memcpy(txbuf[txcount].data, frame, len);
		txcount++;
	}
}

static uint32_t cb_random(void *ctx)
{
	uint32_t r;

	ARG_UNUSED(ctx);
	random_bytes(&r, sizeof(r));
	return r;
}

static void contact_of(struct mt_node *n, struct mc_contact *c)
{
	struct route *r = route_of(n->num);

	memset(c, 0, sizeof(*c));
	c->num = n->num;
	memcpy(c->pub, n->user.public_key, 32);
	/* The shared secret is cached in the node (an X25519 operation). */
	if (!n->has_shared_key) {
		n->has_shared_key = mc_shared_secret(n->shared_key, prv_key, n->user.public_key);
	}
	memcpy(c->secret, n->shared_key, 32);
	c->path_len = r ? r->path_len : MC_PATH_UNKNOWN;
	if (r) {
		memcpy(c->path, r->path, sizeof(c->path));
	}
}

static int cb_find_peers(void *ctx, uint8_t hash, struct mc_contact *peers, int max)
{
	struct mt_nodedb *db = mesh_nodes();
	int n = 0;

	ARG_UNUSED(ctx);
	for (int i = 0; i < db->count && n < max; i++) {
		struct mt_node *node = &db->nodes[i];

		if (node_mine(node) && node->user.public_key[0] == hash) {
			contact_of(node, &peers[n++]);
		}
	}
	return n;
}

/* Heard from a node: when, and stored again once a day. */
static void heard(struct mt_node *n, const struct mc_rx_info *rx)
{
	uint32_t t = mesh_now_time();

	n->last_heard = mesh_uptime_s();
	n->rssi = rx->rssi;
	n->snr = rx->snr;
	n->hops_away = rx->hops == 0xff ? 0 : rx->hops;
	if (t) {
		bool new_day = n->heard_unix / DAY_S != t / DAY_S;

		n->heard_unix = t;
		if (new_day && node_mine(n)) {
			save_contact(n);
		}
	}
}

static void cb_advert(void *ctx, const struct mc_advert *a, const struct mc_rx_info *rx)
{
	uint32_t num = num_of(a->pub);
	struct mt_node *n;
	struct route *r;
	struct mt_user u;
	bool changed;

	ARG_UNUSED(ctx);
	if (!valid_num(num) || !a->name[0]) {
		return;
	}
	n = mt_nodedb_get(mesh_nodes(), num, mesh_uptime_s());
	r = route_of(num);
	if (r && node_mine(n) && memcmp(n->user.public_key, a->pub, 32) == 0 &&
	    a->timestamp <= r->advert_ts) {
		LOG_DBG("old advert of %08x", num);
		return; /* a replay, or an advert we had already */
	}
	user_of(&u, a->pub, a->name, a->type);
	changed = !n->has_user || memcmp(&n->user, &u, sizeof(u)) != 0;
	mt_nodedb_set_user(n, &u);
	if (a->has_latlon && (a->lat_e6 || a->lon_e6)) {
		int32_t lat = a->lat_e6 * 10, lon = a->lon_e6 * 10;

		changed |= !n->has_position || n->position.latitude_i != lat ||
			   n->position.longitude_i != lon;
		n->position = (struct mt_position){.has_latlon = true, .latitude_i = lat,
						   .longitude_i = lon, .time = a->timestamp,
						   .precision_bits = 32};
		n->has_position = true;
	}
	if (r) {
		r->advert_ts = a->timestamp;
	}
	heard(n, rx);
	if (changed) {
		save_contact(n);
	}
	/* Companions take their time from a phone. */
	if (a->type == MC_ADV_CHAT) {
		offer_time(a->timestamp, rx->hops, num);
	}
	LOG_INF("advert from %08x \"%s\" (type %u, %u hops)", num, a->name, a->type, rx->hops);
	mesh_notify(MESH_EV_NODES);
}

static void store_text(uint32_t conv, uint32_t from, const char *text,
		       const struct mc_rx_info *rx)
{
	struct route *r = rx->hops == 0xff && from ? route_of(from) : NULL;
	struct msg m = {
		.conv = conv,
		.from = from,
		.time = mesh_now_time(),
		.uptime_s = mesh_uptime_mark(),
		.status = MSG_RECEIVED,
		.has_radio = true,
		.hops = rx->hops == 0xff ? MSG_HOPS_UNKNOWN : rx->hops,
		.rssi = rx->rssi,
		.snr = rx->snr,
	};

	/* Came along a route, whose length the packet does not show: our way back. */
	if (r && r->path_len != MC_PATH_UNKNOWN) {
		m.hops = mc_path_count(r->path_len);
		m.route_hops = true;
	}
	snprintf(m.text, sizeof(m.text), "%s", text);
	msgstore_lock();
	msgstore_add(&m);
	msgstore_unlock();
	mesh_notify(MESH_EV_MESSAGE);
}

static void cb_text(void *ctx, uint32_t from, uint32_t ts, const char *text,
		    const struct mc_rx_info *rx)
{
	struct mt_node *n = mt_nodedb_find(mesh_nodes(), from);

	ARG_UNUSED(ctx);
	if (n) {
		heard(n, rx);
	}
	offer_time(ts, rx->hops, from);
	store_text(from, from, text, rx);
	LOG_INF("text from %08x: %s", from, text);
}

static void cb_group_text(void *ctx, int channel, uint32_t ts, const char *text,
			  const struct mc_rx_info *rx)
{
	ARG_UNUSED(ctx);
	offer_time(ts, rx->hops, 0);
	/* The sender is only named in the text ("name: text"): from 0. */
	store_text(conv_of_channel(channel), 0, text, rx);
	LOG_INF("channel %d: %s", channel, text);
}

static void cb_path(void *ctx, uint32_t num, const uint8_t *path, uint8_t path_len)
{
	struct mt_node *n = mt_nodedb_find(mesh_nodes(), num);
	struct route *r = route_of(num);

	ARG_UNUSED(ctx);
	if (!n || !r) {
		return;
	}
	r->path_len = path_len;
	if (path) {
		memcpy(r->path, path, mc_path_bytes(path_len));
	}
	LOG_INF("route to %08x: %u hops%s", num, path ? mc_path_count(path_len) : 0,
		path ? "" : " (flood)");
	save_contact(n);
	mesh_notify(MESH_EV_NODES);
}

static void cb_status(void *ctx, uint32_t id, enum mc_tx_status st)
{
	static const uint8_t map[] = {
		[MC_TX_SENT] = MSG_SENT,
		[MC_TX_ACKED] = MSG_DELIVERED,
		[MC_TX_NO_ACK] = MSG_NO_ACK,
		[MC_TX_FAILED] = MSG_FAILED,
	};
	bool known;

	ARG_UNUSED(ctx);
	if (st != MC_TX_SENT) {
		LOG_INF("message %08x %s", id,
			st == MC_TX_ACKED ? "delivered" : st == MC_TX_NO_ACK ? "not relayed" : "failed");
	}
	msgstore_lock();
	known = msgstore_set_status(id, map[st]);
	msgstore_unlock();
	if (known) {
		mesh_notify(st == MC_TX_FAILED || st == MC_TX_NO_ACK ? MESH_EV_FAILED
								     : MESH_EV_STATUS);
	}
}

static const struct mc_mesh_ops ops = {
	.transmit = cb_transmit,
	.random = cb_random,
	.find_peers = cb_find_peers,
	.advert = cb_advert,
	.text = cb_text,
	.group_text = cb_group_text,
	.path = cb_path,
	.status = cb_status,
};

/* Radio and configuration ----------------------------------------------------------------- */

static void on_rx(const struct device *dev, const uint8_t *data, size_t len, int16_t rssi,
		  int8_t snr, void *user_data)
{
	struct evt e = {.type = EVT_RX, .len = len, .rssi = rssi, .snr = snr};

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	K_SPINLOCK(&rx_lock) {
		last_rx_ms = k_uptime_get();
		last_rssi = rssi;
		last_snr = snr;
	}
	if (len > MC_MAX_FRAME) {
		return;
	}
	memcpy(e.data, data, len);
	if (k_msgq_put(&mc_evq, &e, K_NO_WAIT) != 0) {
		LOG_WRN("rx queue full, packet dropped");
	}
}

/* Radio, scope and channels from the settings; lock held. */
static void apply(void)
{
	struct mesh_channel chans[MESH_CHANNELS];
	int p = cfg.preset < ARRAY_SIZE(presets) ? cfg.preset : 0;
	struct mesh_radio_config rc = {
		.freq_hz = presets[p].freq_hz,
		.bw_hz = presets[p].bw_hz,
		.sf = presets[p].sf,
		.cr = presets[p].cr,
		.sync_word = SYNC_WORD,
		/* As MeshCore: a longer preamble at low spreading factors. */
		.preamble = presets[p].sf <= 8 ? 32 : 16,
		.power_dbm = POWER_DBM,
	};

	mc_mesh_set_radio(&mesh, rc.sf, rc.bw_hz, rc.cr, rc.preamble);
	if (cfg.scope[0]) {
		uint8_t key[16];

		mc_scope_key(cfg.scope, key);
		mc_mesh_set_scope(&mesh, key);
	} else {
		mc_mesh_set_scope(&mesh, NULL);
	}
	mesh_channels_get(chans);
	mc_mesh_set_channel(&mesh, 0, mc_public_secret);
	for (int slot = 1; slot < MESH_CHANNELS; slot++) {
		const struct mesh_channel *c = &chans[slot];
		uint8_t secret[16];

		if (!c->name[0]) {
			mc_mesh_set_channel(&mesh, slot, NULL);
			continue;
		}
		if (c->key_type == MESH_KEY_HASHTAG) {
			mc_hashtag_secret(c->name, secret);
		} else {
			memcpy(secret, c->key, sizeof(secret));
		}
		mc_mesh_set_channel(&mesh, slot, secret);
	}
	if (radio) {
		mesh_radio_configure(radio, &rc);
	}
}

static void send_advert(bool flood)
{
	struct mesh_config mc;
	struct mc_advert a = {.type = MC_ADV_CHAT};
	struct gps_config gc;
	struct gps_fix f;

	mesh_get_config(&mc);
	memcpy(a.pub, pub_key, 32);
	a.timestamp = timestamp();
	copy_name(a.name, mc.long_name);
	gps_get_config(&gc);
	if (gc.share != GPS_SHARE_OFF && gps_position(&f)) {
		uint32_t bits = gc.share == GPS_SHARE_EXACT ? 32 : APPROX_PRECISION;

		a.has_latlon = true;
		a.lat_e6 = mt_geo_reduce(f.lat_e7, bits) / 10;
		a.lon_e6 = mt_geo_reduce(f.lon_e7, bits) / 10;
	}
	mc_mesh_send_advert(&mesh, &a, flood);
	advert_ts = a.timestamp;
	settings_save_one("mc/advts", &advert_ts, sizeof(advert_ts));
	LOG_INF("advert sent (%s%s)", flood ? "flood" : "zero hop",
		a.has_latlon ? ", with position" : "");
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

static void handle_send(uint16_t local_id)
{
	struct msg copy;
	uint32_t id = 0;
	int slot;

	if (!load_msg(local_id, &copy)) {
		return;
	}
	slot = conv_channel(copy.conv);
	if (slot >= 0) {
		struct mesh_config mc;

		mesh_get_config(&mc);
		id = mc_mesh_send_group(&mesh, slot, timestamp(), mc.long_name, copy.text);
	} else {
		struct mt_node *n = mt_nodedb_find(mesh_nodes(), copy.conv);
		struct mc_contact c;

		if (n && node_mine(n)) {
			contact_of(n, &c);
			id = mc_mesh_send_text(&mesh, &c, timestamp(), copy.text);
		}
	}
	msgstore_lock();
	msgstore_set_sent(local_id, id, id ? MSG_QUEUED : MSG_FAILED);
	msgstore_unlock();
	mesh_notify(id ? MESH_EV_STATUS : MESH_EV_FAILED);
}

/* Send what the client produced, outside the lock (sending takes the airtime). */
static void flush_tx(void)
{
	static uint8_t frame[MC_MAX_FRAME];

	for (;;) {
		uint8_t len;

		mesh_lock();
		if (txcount == 0) {
			mesh_unlock();
			return;
		}
		len = txbuf[0].len;
		memcpy(frame, txbuf[0].data, len);
		memmove(&txbuf[0], &txbuf[1], (txcount - 1) * sizeof(txbuf[0]));
		txcount--;
		mesh_unlock();
		mesh_radio_send(radio, frame, len);
	}
}

static void mc_thread(void *p1, void *p2, void *p3)
{
	struct evt e;
	uint32_t start = k_uptime_get_32();
	/* Not all at once after a power cut. */
	uint32_t next_advert = start + ADVERT_FIRST_MS + cb_random(NULL) % ADVERT_JITTER_MS;
	bool first_advert = true;
	uint32_t wait = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		int got = k_msgq_get(&mc_evq, &e, K_MSEC(MIN(wait, MAX_WAIT_MS)));
		uint32_t now = k_uptime_get_32();

		mesh_lock();
		mesh.now = now;
		if (got == 0) {
			switch (e.type) {
			case EVT_RX:
				mc_mesh_receive(&mesh, e.data, e.len, e.rssi, e.snr, now);
				break;
			case EVT_SEND:
				handle_send(e.local_id);
				break;
			case EVT_RECONFIG:
				apply();
				break;
			case EVT_ADVERT:
				send_advert(true);
				break;
			default:
				break;
			}
		}
		mesh_date_messages();
		/* The first advert waits a while for the clock (GPS). */
		if ((int32_t)(now - next_advert) >= 0 &&
		    (!first_advert || clock_valid() || now - start >= ADVERT_CLOCK_WAIT_MS)) {
			send_advert(true);
			first_advert = false;
			next_advert = now + ADVERT_INTERVAL_MS;
		}
		wait = mc_mesh_poll(&mesh, now);
		mesh_unlock();
		flush_tx();
	}
}

/* Verifying an advert needs some 2 KB of stack. */
K_THREAD_STACK_DEFINE(mc_stack, 8192);
static struct k_thread mc_tid;

/* API for mesh.c --------------------------------------------------------------------------- */

int meshcore_start(const struct device *dev)
{
	init_identity();
	mc_mesh_init(&mesh, &ops, NULL, pub_key, prv_key);
	radio = dev;
	mesh_lock();
	apply();
	mesh_unlock();
	if (!radio) {
		return -ENODEV;
	}
	if (mesh_radio_receive(radio, on_rx, NULL) != 0) {
		LOG_WRN("mesh radio not available");
		return -EIO;
	}
	started = true;
	LOG_INF("MeshCore node %08x on %s%s%s", my_num, presets[cfg.preset < ARRAY_SIZE(presets)
								    ? cfg.preset
								    : 0].name,
		cfg.scope[0] ? ", region " : "", cfg.scope);
	k_thread_create(&mc_tid, mc_stack, K_THREAD_STACK_SIZEOF(mc_stack), mc_thread, NULL, NULL,
			NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	k_thread_name_set(&mc_tid, "meshcore");
	return 0;
}

static void post(uint8_t type, uint16_t local_id)
{
	struct evt e = {.type = type, .local_id = local_id};

	if (started) {
		k_msgq_put(&mc_evq, &e, K_NO_WAIT);
	}
}

void meshcore_reconfig(void)
{
	post(EVT_RECONFIG, 0);
}

void meshcore_send_advert(void)
{
	post(EVT_ADVERT, 0);
}

int meshcore_send(uint16_t local_id)
{
	struct evt e = {.type = EVT_SEND, .local_id = local_id};

	return k_msgq_put(&mc_evq, &e, K_NO_WAIT) == 0 ? 0 : -EBUSY;
}

uint32_t meshcore_my_num(void)
{
	return my_num;
}

void meshcore_get_stats(struct mesh_stats *s)
{
	mesh_lock();
	*s = (struct mesh_stats){.rx = mesh.rx_packets, .tx = mesh.tx_packets};
	mesh_unlock();
	K_SPINLOCK(&rx_lock) {
		s->last_rx_ms = last_rx_ms;
		s->last_rssi = last_rssi;
		s->last_snr = last_snr;
	}
}

size_t meshcore_text_max(uint32_t conv)
{
	struct mesh_config mc;

	if (conv_channel(conv) < 0) {
		return MC_MAX_TEXT;
	}
	/* "name: " goes before the text. */
	mesh_get_config(&mc);
	return MC_MAX_TEXT - MIN(strlen(mc.long_name), MC_MAX_TEXT / 2) - 2;
}

int meshcore_node_lines(const struct mt_node *n, char lines[][32], int max)
{
	static const char *const types[] = {"?", "Companion", "Repeater", "Room server", "Sensor"};
	struct route r = {.path_len = MC_PATH_UNKNOWN};
	int k = 0;

	if (max < 2) {
		return 0;
	}
	mesh_lock();
	for (int i = 0; i < MT_MAX_NODES; i++) {
		if (routes[i].num == n->num) {
			r = routes[i];
		}
	}
	mesh_unlock();
	snprintf(lines[k++], 32, "%s", tr(types[n->user.role < ARRAY_SIZE(types) ? n->user.role : 0]));
	if (r.path_len == MC_PATH_UNKNOWN) {
		snprintf(lines[k++], 32, "%s: %s", tr("Route"), tr("flood"));
	} else if (mc_path_count(r.path_len) == 0) {
		snprintf(lines[k++], 32, "%s: %s", tr("Route"), tr("direct"));
	} else {
		int len = snprintf(lines[k], 32, "%s:", tr("Route"));
		int size = mc_path_hash_size(r.path_len);

		for (int h = 0; h < mc_path_count(r.path_len) && len < 28; h++) {
			len += snprintf(lines[k] + len, 32 - len, " ");
			for (int b = 0; b < size && len < 30; b++) {
				len += snprintf(lines[k] + len, 32 - len, "%02x",
						r.path[h * size + b]);
			}
		}
		k++;
	}
	return k;
}

/* Public API: MeshCore settings ------------------------------------------------------------ */

void mesh_hashtag_key(const char *name, uint8_t key[MESH_KEY_LEN])
{
	mc_hashtag_secret(name, key);
}

int mesh_mc_presets(void)
{
	return ARRAY_SIZE(presets);
}

const char *mesh_mc_preset_name(int i)
{
	return i >= 0 && i < (int)ARRAY_SIZE(presets) ? presets[i].name : "?";
}

void mesh_mc_get_config(struct mesh_mc_config *c)
{
	mesh_lock();
	*c = cfg;
	mesh_unlock();
}

void mesh_mc_set_config(const struct mesh_mc_config *c)
{
	mesh_lock();
	cfg = *c;
	cfg.scope[sizeof(cfg.scope) - 1] = '\0';
	mesh_unlock();
	settings_save_one("mc/cfg", &cfg, sizeof(cfg));
	meshcore_reconfig();
}
