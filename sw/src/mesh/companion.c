/* MeshCore companion protocol: see companion.h. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>

#include "../app.h"
#include "../battery.h"
#include "../clock.h"
#include "../gps.h"
#include "companion.h"
#include "mesh_internal.h"
#include "meshcore/packet.h"
#include "msgstore.h"

LOG_MODULE_REGISTER(companion, CONFIG_CMDZ5_APP_LOG_LEVEL);

/* The protocol version we speak (MeshCore's FIRMWARE_VER_CODE). */
#define VER_CODE     12
#define TX_POWER_DBM 22
#define TIME_MIN     1700000000u

enum {
	CMD_APP_START = 1,
	CMD_SEND_TXT_MSG = 2,
	CMD_SEND_CHANNEL_TXT_MSG = 3,
	CMD_GET_CONTACTS = 4,
	CMD_GET_DEVICE_TIME = 5,
	CMD_SET_DEVICE_TIME = 6,
	CMD_SEND_SELF_ADVERT = 7,
	CMD_SET_ADVERT_NAME = 8,
	CMD_ADD_UPDATE_CONTACT = 9,
	CMD_SYNC_NEXT_MESSAGE = 10,
	CMD_SET_RADIO_PARAMS = 11,
	CMD_SET_RADIO_TX_POWER = 12,
	CMD_RESET_PATH = 13,
	CMD_SET_ADVERT_LATLON = 14,
	CMD_REMOVE_CONTACT = 15,
	CMD_SHARE_CONTACT = 16,
	CMD_EXPORT_CONTACT = 17,
	CMD_IMPORT_CONTACT = 18,
	CMD_REBOOT = 19,
	CMD_GET_BATT_AND_STORAGE = 20,
	CMD_SET_TUNING_PARAMS = 21,
	CMD_DEVICE_QUERY = 22,
	CMD_EXPORT_PRIVATE_KEY = 23,
	CMD_IMPORT_PRIVATE_KEY = 24,
	CMD_SEND_LOGIN = 26,
	CMD_SEND_STATUS_REQ = 27,
	CMD_HAS_CONNECTION = 28,
	CMD_LOGOUT = 29,
	CMD_GET_CONTACT_BY_KEY = 30,
	CMD_GET_CHANNEL = 31,
	CMD_SET_CHANNEL = 32,
	CMD_SEND_TRACE_PATH = 36,
	CMD_SET_DEVICE_PIN = 37,
	CMD_SET_OTHER_PARAMS = 38,
	CMD_SEND_TELEMETRY_REQ = 39,
	CMD_GET_CUSTOM_VARS = 40,
	CMD_SET_CUSTOM_VAR = 41,
	CMD_GET_ADVERT_PATH = 42,
	CMD_GET_TUNING_PARAMS = 43,
	CMD_SEND_BINARY_REQ = 50,
	CMD_SEND_PATH_DISCOVERY_REQ = 52,
	CMD_SET_FLOOD_SCOPE_KEY = 54,
	CMD_GET_STATS = 56,
	CMD_SET_AUTOADD_CONFIG = 58,
	CMD_GET_AUTOADD_CONFIG = 59,
	CMD_GET_ALLOWED_REPEAT_FREQ = 60,
	CMD_SET_PATH_HASH_MODE = 61,
	CMD_SET_DEFAULT_FLOOD_SCOPE = 63,
	CMD_GET_DEFAULT_FLOOD_SCOPE = 64,
};

enum {
	RESP_OK = 0,
	RESP_ERR = 1,
	RESP_CONTACTS_START = 2,
	RESP_CONTACT = 3,
	RESP_END_OF_CONTACTS = 4,
	RESP_SELF_INFO = 5,
	RESP_SENT = 6,
	RESP_CONTACT_MSG_RECV = 7,
	RESP_CHANNEL_MSG_RECV = 8,
	RESP_CURR_TIME = 9,
	RESP_NO_MORE_MESSAGES = 10,
	RESP_EXPORT_CONTACT = 11,
	RESP_BATT_AND_STORAGE = 12,
	RESP_DEVICE_INFO = 13,
	RESP_DISABLED = 15,
	RESP_CONTACT_MSG_RECV_V3 = 16,
	RESP_CHANNEL_MSG_RECV_V3 = 17,
	RESP_CHANNEL_INFO = 18,
	RESP_CUSTOM_VARS = 21,
	RESP_TUNING_PARAMS = 23,
	RESP_STATS = 24,
	RESP_AUTOADD_CONFIG = 25,
	RESP_ALLOWED_REPEAT_FREQ = 26,
	RESP_DEFAULT_FLOOD_SCOPE = 28,

	PUSH_ADVERT = 0x80,
	PUSH_PATH_UPDATED = 0x81,
	PUSH_SEND_CONFIRMED = 0x82,
	PUSH_MSG_WAITING = 0x83,
	PUSH_LOGIN_SUCCESS = 0x85,
	PUSH_LOGIN_FAIL = 0x86,
	PUSH_STATUS_RESPONSE = 0x87,
	PUSH_LOG_RX_DATA = 0x88,
	PUSH_TRACE_DATA = 0x89,
	PUSH_NEW_ADVERT = 0x8a,
	PUSH_TELEMETRY_RESPONSE = 0x8b,
	PUSH_BINARY_RESPONSE = 0x8c,
	PUSH_PATH_DISCOVERY_RESPONSE = 0x8d,
	PUSH_CONTACT_DELETED = 0x8f,
};

enum {
	ERR_UNSUPPORTED = 1,
	ERR_NOT_FOUND = 2,
	ERR_TABLE_FULL = 3,
	ERR_BAD_STATE = 4,
	ERR_FILE_IO = 5,
	ERR_ILLEGAL_ARG = 6,
};

#define TXT_TYPE_PLAIN    0
#define TXT_TYPE_CLI_DATA 1
#define TXT_TYPE_CLI_CMD  3
#define REQ_STATUS        0x01
#define REQ_TELEMETRY     0x03
#define RESP_LOGIN_OK     0 /* RESP_SERVER_LOGIN_OK */
#define TELEM_PERM_BASE   0x01
#define CONTACT_FRAME  148
#define OUT_LEN        8

/* Settings of the app that the device keeps for it (stored: "mc/app"). */
static struct app_prefs {
	uint8_t manual_add;  /* the app's "manual add contacts" */
	uint8_t telemetry;   /* telemetry modes: env << 4 | loc << 2 | base */
	uint8_t multi_acks;
	uint8_t autoadd;     /* auto-add config bits */
	uint8_t autoadd_hops;
} prefs;

struct frame {
	uint8_t len;
	uint8_t buf[COMPANION_FRAME_MAX];
};

/* Everything below: under the mesh lock. */
static struct frame outq[OUT_LEN]; /* responses and pushes */
static int out_head, out_count;
static struct frame offline[COMPANION_QUEUE]; /* received messages */
static int off_count;
static bool connected;
static void (*wake)(void);
static uint8_t app_ver;

/* Contact list being sent (CMD_GET_CONTACTS), one frame at a time. */
static bool iter_on;
static int iter_pos;
static uint32_t iter_since, iter_newest;

/*
 * The app's request waiting for an answer (one at a time, as MeshCore):
 * logins and status matched by the contact's key, the rest by the tag.
 */
static uint32_t pending_login, pending_status; /* first 4 key bytes */
static uint32_t pending_telemetry, pending_req, pending_discovery; /* tags */

/* The app's direct messages: the ACK it waits for. */
static struct {
	uint32_t id, ack, sent_ms;
} sends[MC_PENDING_LEN * 2];

static void put16(uint8_t *p, uint16_t v)
{
	sys_put_le16(v, p);
}

static void put32(uint8_t *p, uint32_t v)
{
	sys_put_le32(v, p);
}

static uint32_t get32(const uint8_t *p)
{
	return sys_get_le32(p);
}

static void out(const uint8_t *buf, size_t len)
{
	struct frame *f;

	if (out_count == OUT_LEN) {
		LOG_WRN("app queue full, frame %02x dropped", buf[0]);
		return;
	}
	f = &outq[(out_head + out_count++) % OUT_LEN];
	f->len = MIN(len, COMPANION_FRAME_MAX);
	memcpy(f->buf, buf, f->len);
	if (wake) {
		wake();
	}
}

static void ok(void)
{
	uint8_t b = RESP_OK;

	out(&b, 1);
}

static void err(uint8_t code)
{
	uint8_t b[2] = {RESP_ERR, code};

	out(b, 2);
}

static void push(const uint8_t *buf, size_t len)
{
	if (connected) {
		out(buf, len);
	}
}

static int8_t snr4(int8_t snr_db)
{
	return CLAMP(snr_db * 4, INT8_MIN, INT8_MAX);
}

static void save_prefs(void)
{
	settings_save_one("mc/app", &prefs, sizeof(prefs));
}

int companion_prefs_read(size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (len != sizeof(prefs)) {
		return -EINVAL;
	}
	return read_cb(cb_arg, &prefs, len) == (ssize_t)len ? 0 : -EIO;
}

/* Contacts ----------------------------------------------------------------------- */

static int contact_frame(uint8_t *f, uint8_t code, const struct mc_app_contact *c)
{
	int i = 0;

	f[i++] = code;
	memcpy(f + i, c->pub, 32);
	i += 32;
	f[i++] = c->type;
	f[i++] = c->flags;
	f[i++] = c->path_len;
	memset(f + i, 0, 64);
	if (c->path_len != MC_PATH_UNKNOWN) {
		memcpy(f + i, c->path, MIN(mc_path_bytes(c->path_len), 64));
	}
	i += 64;
	memset(f + i, 0, 32);
	for (int k = 0; k < 31 && c->name[k]; k++) {
		f[i + k] = c->name[k];
	}
	i += 32;
	put32(f + i, c->advert_ts);
	put32(f + i + 4, c->lat_e6);
	put32(f + i + 8, c->lon_e6);
	put32(f + i + 12, c->lastmod);
	return i + 16;
}

static int count_contacts(void)
{
	struct mt_nodedb *db = mesh_nodes();
	int n = 0;

	for (int i = 0; i < db->count; i++) {
		n += meshcore_is_contact(&db->nodes[i]);
	}
	return n;
}

/* The next frame of the contact list, or its end. */
static int next_contact(uint8_t *buf)
{
	struct mt_nodedb *db = mesh_nodes();

	while (iter_pos < db->count) {
		const struct mt_node *n = &db->nodes[iter_pos++];
		struct mc_app_contact c;

		if (!meshcore_is_contact(n)) {
			continue;
		}
		meshcore_contact_info(n, &c);
		if (c.lastmod <= iter_since && iter_since) {
			continue;
		}
		iter_newest = MAX(iter_newest, c.lastmod);
		return contact_frame(buf, RESP_CONTACT, &c);
	}
	iter_on = false;
	buf[0] = RESP_END_OF_CONTACTS;
	put32(buf + 1, iter_newest);
	return 5;
}

/* A contact by its whole key (commands that name one). */
static struct mt_node *contact_arg(const uint8_t *f, size_t len, size_t at)
{
	return len >= at + 32 ? meshcore_find_key(f + at, 32) : NULL;
}

/* Messages for the app -------------------------------------------------------------- */

static bool is_channel_msg(const struct frame *f)
{
	return f->buf[0] == RESP_CHANNEL_MSG_RECV || f->buf[0] == RESP_CHANNEL_MSG_RECV_V3;
}

static void queue_message(const uint8_t *buf, size_t len)
{
	uint8_t b = PUSH_MSG_WAITING;
	int drop = 0;

	if (off_count == COMPANION_QUEUE) {
		/* Full: the oldest channel message goes, else the oldest message. */
		for (int i = 0; i < off_count; i++) {
			if (is_channel_msg(&offline[i])) {
				drop = i;
				break;
			}
		}
		memmove(&offline[drop], &offline[drop + 1], (off_count - drop - 1) * sizeof(offline[0]));
		off_count--;
	}
	offline[off_count].len = MIN(len, COMPANION_FRAME_MAX);
	memcpy(offline[off_count].buf, buf, offline[off_count].len);
	off_count++;
	push(&b, 1);
}

/* The path length as MeshCore reports it: of a flood, else 0xff. */
static uint8_t rx_path_len(const struct mc_rx_info *rx)
{
	return rx->flood ? rx->path_len : 0xff;
}

static int text_into(uint8_t *f, int i, const char *text)
{
	size_t n = MIN(strlen(text), (size_t)(COMPANION_FRAME_MAX - i));

	memcpy(f + i, text, n);
	return i + n;
}

static void contact_message(const uint8_t pub[32], uint8_t type, uint32_t timestamp,
			    const char *text, const struct mc_rx_info *rx)
{
	uint8_t f[COMPANION_FRAME_MAX];
	int i = 0;

	if (app_ver >= 3) {
		f[i++] = RESP_CONTACT_MSG_RECV_V3;
		f[i++] = snr4(rx->snr);
		f[i++] = 0;
		f[i++] = 0;
	} else {
		f[i++] = RESP_CONTACT_MSG_RECV;
	}
	memcpy(f + i, pub, 6);
	i += 6;
	f[i++] = rx_path_len(rx);
	f[i++] = type;
	put32(f + i, timestamp);
	i += 4;
	queue_message(f, text_into(f, i, text));
}

void companion_text(const uint8_t pub[32], uint32_t timestamp, const char *text,
		    const struct mc_rx_info *rx)
{
	contact_message(pub, TXT_TYPE_PLAIN, timestamp, text, rx);
}

/* A repeater's reply to a command: for the app only. */
void companion_cli_text(const uint8_t pub[32], uint32_t timestamp, const char *text,
			const struct mc_rx_info *rx)
{
	contact_message(pub, TXT_TYPE_CLI_DATA, timestamp, text, rx);
}

void companion_group(int channel, uint32_t timestamp, const char *text,
		     const struct mc_rx_info *rx)
{
	uint8_t f[COMPANION_FRAME_MAX];
	int i = 0;

	if (app_ver >= 3) {
		f[i++] = RESP_CHANNEL_MSG_RECV_V3;
		f[i++] = snr4(rx->snr);
		f[i++] = 0;
		f[i++] = 0;
	} else {
		f[i++] = RESP_CHANNEL_MSG_RECV;
	}
	f[i++] = channel;
	f[i++] = rx_path_len(rx);
	f[i++] = TXT_TYPE_PLAIN;
	put32(f + i, timestamp);
	i += 4;
	queue_message(f, text_into(f, i, text));
}

/* Pushes ---------------------------------------------------------------------------- */

static void push_key(uint8_t code, const uint8_t pub[32])
{
	uint8_t f[33];

	f[0] = code;
	memcpy(f + 1, pub, 32);
	push(f, sizeof(f));
}

void companion_advert(const struct mt_node *n, bool is_new)
{
	if (!connected) {
		return;
	}
	if (is_new) {
		uint8_t f[CONTACT_FRAME];
		struct mc_app_contact c;

		meshcore_contact_info(n, &c);
		push(f, contact_frame(f, PUSH_NEW_ADVERT, &c));
	} else {
		push_key(PUSH_ADVERT, n->user.public_key);
	}
}

void companion_path(const uint8_t pub[32])
{
	push_key(PUSH_PATH_UPDATED, pub);
}

void companion_deleted(const uint8_t pub[32])
{
	push_key(PUSH_CONTACT_DELETED, pub);
}

void companion_rx_log(const uint8_t *frame, size_t len, int16_t rssi, int8_t snr)
{
	uint8_t f[COMPANION_FRAME_MAX];

	/* Only with room to spare: the log must not crowd out answers. */
	if (!connected || len + 3 > sizeof(f) || out_count > OUT_LEN / 2) {
		return;
	}
	f[0] = PUSH_LOG_RX_DATA;
	f[1] = snr4(snr);
	f[2] = CLAMP(rssi, INT8_MIN, INT8_MAX);
	memcpy(f + 3, frame, len);
	push(f, len + 3);
}

bool companion_status(uint32_t id, enum mc_tx_status st)
{
	for (size_t i = 0; i < ARRAY_SIZE(sends); i++) {
		if (!sends[i].id || sends[i].id != id) {
			continue;
		}
		if (st == MC_TX_ACKED) {
			uint8_t f[9] = {PUSH_SEND_CONFIRMED};

			put32(f + 1, sends[i].ack);
			put32(f + 5, k_uptime_get_32() - sends[i].sent_ms);
			push(f, sizeof(f));
		}
		if (st != MC_TX_SENT) {
			sends[i].id = 0;
		}
		return true;
	}
	return false;
}

static uint32_t key4(const uint8_t *pub)
{
	return sys_get_le32(pub);
}

void companion_response(const struct mt_node *n, const uint8_t *data, size_t len,
			const uint8_t *in_path, uint8_t in_path_len)
{
	const uint8_t *pub = n->user.public_key;
	uint8_t f[COMPANION_FRAME_MAX];
	uint32_t tag = len >= 4 ? get32(data) : 0;
	size_t rest = len > 4 ? MIN(len - 4, sizeof(f) - 12) : 0;
	int i = 0;

	if (in_path && pending_discovery && tag == pending_discovery && len > 4) {
		struct mc_app_contact c;
		uint8_t out_len;

		pending_discovery = 0;
		meshcore_contact_info(n, &c);
		out_len = c.path_len == MC_PATH_UNKNOWN ? 0 : c.path_len;
		f[i++] = PUSH_PATH_DISCOVERY_RESPONSE;
		f[i++] = 0;
		memcpy(f + i, pub, 6);
		i += 6;
		f[i++] = out_len;
		memcpy(f + i, c.path, mc_path_bytes(out_len));
		i += mc_path_bytes(out_len);
		f[i++] = in_path_len;
		memcpy(f + i, in_path, mc_path_bytes(in_path_len));
		i += mc_path_bytes(in_path_len);
		push(f, i);
		return;
	}
	if (pending_login && pending_login == key4(pub)) {
		pending_login = 0;
		if (len >= 6 && memcmp(data + 4, "OK", 2) == 0) {
			/* Older repeaters */
			f[i++] = PUSH_LOGIN_SUCCESS;
			f[i++] = 0;
			memcpy(f + i, pub, 6);
			i += 6;
		} else if (len >= 13 && data[4] == RESP_LOGIN_OK) {
			f[i++] = PUSH_LOGIN_SUCCESS;
			f[i++] = data[6]; /* permissions (admin) */
			memcpy(f + i, pub, 6);
			i += 6;
			put32(f + i, tag); /* the server's time */
			i += 4;
			f[i++] = data[7];  /* ACL permissions */
			f[i++] = data[12]; /* its firmware level */
		} else {
			f[i++] = PUSH_LOGIN_FAIL;
			f[i++] = 0;
			memcpy(f + i, pub, 6);
			i += 6;
		}
		LOG_INF("login to %02x%02x%02x%02x: %s", pub[0], pub[1], pub[2], pub[3],
			f[0] == PUSH_LOGIN_SUCCESS ? "ok" : "refused");
		push(f, i);
		return;
	}
	if (len > 4 && pending_status && pending_status == key4(pub)) {
		pending_status = 0;
		f[i++] = PUSH_STATUS_RESPONSE;
		f[i++] = 0;
		memcpy(f + i, pub, 6);
		i += 6;
	} else if (len > 4 && pending_telemetry && tag == pending_telemetry) {
		pending_telemetry = 0;
		f[i++] = PUSH_TELEMETRY_RESPONSE;
		f[i++] = 0;
		memcpy(f + i, pub, 6);
		i += 6;
	} else if (len > 4 && pending_req && tag == pending_req) {
		pending_req = 0;
		f[i++] = PUSH_BINARY_RESPONSE;
		f[i++] = 0;
		put32(f + i, tag);
		i += 4;
	} else {
		/* Unasked (e.g. pushed telemetry): the whole response. */
		f[i++] = PUSH_BINARY_RESPONSE;
		f[i++] = 0;
		len = MIN(len, sizeof(f) - 2);
		memcpy(f + i, data, len);
		push(f, i + len);
		return;
	}
	memcpy(f + i, data + 4, rest);
	push(f, i + rest);
}

void companion_trace(uint32_t tag, uint32_t auth, uint8_t flags, const uint8_t *hashes,
		     uint8_t hashes_len, const int8_t *snrs, uint8_t snr_count, int8_t snr)
{
	uint8_t f[COMPANION_FRAME_MAX];
	uint8_t nsnr = MIN(snr_count, (uint8_t)(hashes_len >> (flags & 3)));
	int i = 0;

	if (12 + hashes_len + nsnr + 1 > sizeof(f)) {
		return;
	}
	f[i++] = PUSH_TRACE_DATA;
	f[i++] = 0;
	f[i++] = hashes_len;
	f[i++] = flags;
	put32(f + i, tag);
	put32(f + i + 4, auth);
	i += 8;
	memcpy(f + i, hashes, hashes_len);
	i += hashes_len;
	memcpy(f + i, snrs, nsnr);
	i += nsnr;
	f[i++] = snr4(snr); /* the last hop, to us */
	push(f, i);
}

/* Commands -------------------------------------------------------------------------- */

static void clear_pending(void)
{
	pending_login = pending_status = 0;
	pending_telemetry = pending_req = pending_discovery = 0;
}

static void sent(bool flood, uint32_t tag, uint32_t timeout)
{
	uint8_t r[10] = {RESP_SENT, flood};

	put32(r + 2, tag);
	put32(r + 6, timeout);
	out(r, sizeof(r));
}

/* Login to a repeater or room server: the password, at most 15 characters. */
static void send_login(const uint8_t *f, size_t len)
{
	struct mt_node *n = contact_arg(f, len, 1);
	uint8_t data[24];
	struct mc_contact c;
	size_t pl = MIN(len - 33, (size_t)15), dl = 4;
	uint32_t timeout;
	bool flood;

	if (!n) {
		err(ERR_NOT_FOUND);
		return;
	}
	put32(data, meshcore_next_timestamp());
	if (n->user.role == MC_ADV_ROOM) {
		put32(data + dl, 0); /* messages since: all */
		dl += 4;
	}
	memcpy(data + dl, f + 33, pl);
	meshcore_contact_of(n, &c);
	timeout = mc_mesh_send_anon(meshcore_lib(), &c, data, dl + pl, &flood);
	if (!timeout) {
		err(ERR_TABLE_FULL);
		return;
	}
	clear_pending();
	pending_login = key4(n->user.public_key);
	sent(flood, pending_login, timeout);
}

/* A request to a contact; *tag set (and returned to the app) */
static bool request(struct mt_node *n, const uint8_t *body, size_t blen, bool force_flood,
		    uint32_t *tag)
{
	uint8_t data[MC_MAX_PAYLOAD];
	struct mc_contact c;
	uint32_t timeout;
	bool flood;

	if (blen > sizeof(data) - 4 - 32) {
		err(ERR_ILLEGAL_ARG);
		return false;
	}
	*tag = meshcore_next_timestamp();
	put32(data, *tag);
	memcpy(data + 4, body, blen);
	meshcore_contact_of(n, &c);
	timeout = mc_mesh_send_request(meshcore_lib(), &c, data, 4 + blen, force_flood, &flood);
	if (!timeout) {
		err(ERR_TABLE_FULL);
		return false;
	}
	clear_pending();
	sent(flood, *tag, timeout);
	return true;
}

/* A request of a type: as MeshCore, 4 reserved bytes and 4 random ones after it. */
static bool typed_request(struct mt_node *n, uint8_t type, uint8_t mask, bool force_flood,
			  uint32_t *tag)
{
	uint8_t body[9] = {type, mask};

	sys_rand_get(body + 5, 4);
	return request(n, body, sizeof(body), force_flood, tag);
}

static void send_cli(struct mt_node *n, const uint8_t *f, size_t len)
{
	char text[MC_MAX_TEXT + 1];
	struct mc_contact c;
	uint32_t timeout;
	bool flood;

	len = MIN(len - 13, (size_t)MC_MAX_TEXT);
	memcpy(text, f + 13, len);
	text[len] = '\0';
	meshcore_contact_of(n, &c);
	/* Our own time: the repeater rejects times it had already. */
	timeout = mc_mesh_send_cli(meshcore_lib(), &c, meshcore_next_timestamp(), f[2], text,
				   &flood);
	if (!timeout) {
		err(ERR_TABLE_FULL);
		return;
	}
	sent(flood, 0, timeout); /* no ACK */
}

static void send_trace(const uint8_t *f, size_t len)
{
	uint32_t timeout;

	timeout = mc_mesh_send_trace(meshcore_lib(), get32(f + 1), get32(f + 5), f[9], f + 10,
				     len - 10);
	if (!timeout) {
		err(ERR_ILLEGAL_ARG);
		return;
	}
	sent(false, get32(f + 1), timeout);
}

static void device_info(void)
{
	uint8_t f[82] = {RESP_DEVICE_INFO, VER_CODE, MT_MAX_NODES / 2, MESH_CHANNELS};

	/* BLE PIN 0: a random one, shown on the display when pairing. */
	put32(f + 4, 0);
	snprintf((char *)f + 8, 12, "%s", __DATE__);
	snprintf((char *)f + 20, 40, "CMD-Z5");
	snprintf((char *)f + 60, 20, "CMD-Z5 %s", APP_VERSION);
	f[80] = 0; /* no repeating */
	f[81] = 0; /* 1-byte path hashes */
	out(f, sizeof(f));
}

static void self_info(void)
{
	uint8_t f[COMPANION_FRAME_MAX];
	struct mesh_mc_config mcc;
	struct mesh_config mc;
	struct gps_config gc;
	struct gps_fix fix;
	uint32_t freq_hz, bw_hz;
	uint8_t sf, cr;
	int i = 0;

	mesh_mc_get_config(&mcc);
	mesh_get_config(&mc);
	gps_get_config(&gc);
	meshcore_preset(mcc.preset, &freq_hz, &bw_hz, &sf, &cr);
	f[i++] = RESP_SELF_INFO;
	f[i++] = MC_ADV_CHAT;
	f[i++] = TX_POWER_DBM;
	f[i++] = TX_POWER_DBM;
	memcpy(f + i, meshcore_pub_key(), 32);
	i += 32;
	if (!gps_position(&fix)) {
		fix.lat_e7 = fix.lon_e7 = 0;
	}
	put32(f + i, fix.lat_e7 / 10);
	put32(f + i + 4, fix.lon_e7 / 10);
	i += 8;
	f[i++] = prefs.multi_acks;
	f[i++] = gc.share != GPS_SHARE_OFF; /* advert location policy */
	f[i++] = prefs.telemetry;
	f[i++] = prefs.manual_add;
	put32(f + i, freq_hz / 1000);
	put32(f + i + 4, bw_hz);
	i += 8;
	f[i++] = sf;
	f[i++] = cr;
	i = text_into(f, i, mc.long_name);
	out(f, i);
}

static void send_text(const uint8_t *f, size_t len)
{
	uint8_t type = f[1], attempt = f[2];
	uint32_t ts = get32(f + 3), ack, timeout;
	struct mt_node *n = meshcore_find_key(f + 7, 6);
	char text[MC_MAX_TEXT + 1];
	uint8_t r[10] = {RESP_SENT};
	bool flood;
	uint32_t id;

	if (!n) {
		err(ERR_NOT_FOUND);
		return;
	}
	if (type == TXT_TYPE_CLI_DATA || type == TXT_TYPE_CLI_CMD) {
		send_cli(n, f, len);
		return;
	}
	if (type != TXT_TYPE_PLAIN) {
		err(ERR_UNSUPPORTED);
		return;
	}
	len = MIN(len - 13, (size_t)MC_MAX_TEXT);
	memcpy(text, f + 13, len);
	text[len] = '\0';
	id = meshcore_app_send(n, ts, attempt, text, &ack, &timeout, &flood);
	if (!id) {
		err(ERR_TABLE_FULL);
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(sends); i++) {
		if (!sends[i].id || i == ARRAY_SIZE(sends) - 1) {
			sends[i].id = id;
			sends[i].ack = ack;
			sends[i].sent_ms = k_uptime_get_32();
			break;
		}
	}
	r[1] = flood;
	put32(r + 2, ack);
	put32(r + 6, timeout);
	out(r, sizeof(r));
}

static bool channel_ok(int idx)
{
	struct mesh_channel c;

	return idx == 0 || (idx > 0 && idx < MESH_CHANNELS && mesh_get_channel(idx, &c));
}

static void send_channel(const uint8_t *f, size_t len)
{
	char text[MC_MAX_TEXT + 1];
	size_t n = MIN(len - 7, (size_t)MC_MAX_TEXT);

	if (f[1] != TXT_TYPE_PLAIN) {
		err(ERR_UNSUPPORTED);
		return;
	}
	if (!channel_ok(f[2])) {
		err(ERR_NOT_FOUND);
		return;
	}
	memcpy(text, f + 7, n);
	text[n] = '\0';
	/* Stored and sent as one of ours, under our name. */
	if (mesh_send_text(conv_of_channel(f[2]), text) == 0) {
		ok();
	} else {
		err(ERR_TABLE_FULL);
	}
}

static void get_channel(int idx)
{
	uint8_t f[50] = {RESP_CHANNEL_INFO, idx};
	struct mesh_channel c;

	if (idx < 0 || idx >= MESH_CHANNELS) {
		err(ERR_NOT_FOUND);
		return;
	}
	if (idx == 0) {
		snprintf((char *)f + 2, 32, "Public");
		memcpy(f + 34, mc_public_secret, 16);
	} else if (mesh_get_channel(idx, &c)) {
		if (c.key_type == MESH_KEY_HASHTAG) {
			snprintf((char *)f + 2, 32, "#%s", c.name);
			mc_hashtag_secret(c.name, f + 34);
		} else {
			snprintf((char *)f + 2, 32, "%s", c.name);
			memcpy(f + 34, c.key, 16);
		}
	}
	/* An unused slot: no name, no key (as MeshCore). */
	out(f, sizeof(f));
}

static void set_channel(const uint8_t *f, size_t len)
{
	static const uint8_t zero[16];
	struct mesh_channel c = {0};
	const uint8_t *secret = f + 34;
	uint8_t tag[16];
	char name[33];
	const char *p;

	if (len >= 2 + 32 + 32) {
		err(ERR_UNSUPPORTED); /* 256-bit keys */
		return;
	}
	if (f[1] >= MESH_CHANNELS) {
		err(ERR_NOT_FOUND);
		return;
	}
	memcpy(name, f + 2, 32);
	name[32] = '\0';
	if (f[1] == 0) {
		/* The Public channel is fixed. */
		if (memcmp(secret, mc_public_secret, 16) == 0) {
			ok();
		} else {
			err(ERR_ILLEGAL_ARG);
		}
		return;
	}
	if (!name[0] || memcmp(secret, zero, 16) == 0) {
		mesh_set_channel(f[1], NULL);
		ok();
		return;
	}
	p = name[0] == '#' ? name + 1 : name;
	mc_hashtag_secret(p, tag);
	if (name[0] == '#' && memcmp(tag, secret, 16) == 0) {
		c.key_type = MESH_KEY_HASHTAG;
	} else {
		c.key_type = MESH_KEY_PASSPHRASE; /* any key */
		memcpy(c.key, secret, 16);
		p = name;
	}
	if (strlen(p) > MESH_CHANNEL_NAME) {
		err(ERR_ILLEGAL_ARG); /* the device keeps shorter names */
		return;
	}
	memcpy(c.name, p, strlen(p) + 1);
	mesh_set_channel(f[1], &c);
	ok();
}

static void add_contact(const uint8_t *f, size_t len)
{
	struct mc_app_contact c = {0};

	if (len < 136) {
		err(ERR_ILLEGAL_ARG);
		return;
	}
	memcpy(c.pub, f + 1, 32);
	c.type = f[33];
	c.flags = f[34];
	c.path_len = f[35];
	memcpy(c.path, f + 36, 64);
	memcpy(c.name, f + 100, 31);
	c.advert_ts = get32(f + 132);
	c.lastmod = meshcore_time();
	if (len >= 144) {
		c.lat_e6 = get32(f + 136);
		c.lon_e6 = get32(f + 140);
		if (len >= 148) {
			c.lastmod = get32(f + 144);
		}
	}
	if (meshcore_contact_set(&c)) {
		ok();
	} else {
		err(ERR_ILLEGAL_ARG);
	}
}

static void set_radio(const uint8_t *f, size_t len)
{
	uint32_t freq_khz = get32(f + 1), bw_hz = get32(f + 5);
	struct mesh_mc_config c;

	if (len < 11 || (len > 11 && f[11])) {
		err(ERR_ILLEGAL_ARG); /* no repeating */
		return;
	}
	/* The device has MeshCore's presets only. */
	for (int i = 0; i < mesh_mc_presets(); i++) {
		uint32_t pf, pb;
		uint8_t sf, cr;

		meshcore_preset(i, &pf, &pb, &sf, &cr);
		if (pf / 1000 == freq_khz && pb == bw_hz && sf == f[9] && cr == f[10]) {
			mesh_mc_get_config(&c);
			c.preset = i;
			mesh_mc_set_config(&c);
			ok();
			return;
		}
	}
	err(ERR_ILLEGAL_ARG);
}

static void set_latlon(const uint8_t *f)
{
	int32_t lat = get32(f + 1), lon = get32(f + 5);
	struct gps_config c;

	if (lat < -90000000 || lat > 90000000 || lon < -180000000 || lon > 180000000) {
		err(ERR_ILLEGAL_ARG);
		return;
	}
	/* The device's fixed position (Settings > Position). */
	gps_get_config(&c);
	c.has_fixed = true;
	c.fixed_lat_e7 = lat * 10;
	c.fixed_lon_e7 = lon * 10;
	if (c.mode == GPS_MODE_OFF) {
		c.mode = GPS_MODE_FIXED;
	}
	gps_set_config(&c);
	ok();
}

static void set_other(const uint8_t *f, size_t len)
{
	struct gps_config gc;

	prefs.manual_add = f[1];
	if (len >= 3) {
		prefs.telemetry = f[2];
	}
	if (len >= 4) {
		/* Location in adverts: the device's position sharing. */
		gps_get_config(&gc);
		if (!f[3] != (gc.share == GPS_SHARE_OFF)) {
			gc.share = f[3] ? GPS_SHARE_EXACT : GPS_SHARE_OFF;
			gps_set_config(&gc);
		}
	}
	if (len >= 5) {
		prefs.multi_acks = f[4];
	}
	save_prefs();
	ok();
}

static void batt_storage(void)
{
	uint8_t f[11] = {RESP_BATT_AND_STORAGE};
	struct battery_status b;
	struct fs_statvfs vfs;
	uint32_t used = 0, total = 0;

	battery_get(&b);
	if (fs_statvfs("/lfs", &vfs) == 0) {
		total = (uint32_t)((uint64_t)vfs.f_blocks * vfs.f_frsize / 1024);
		used = total - (uint32_t)((uint64_t)vfs.f_bfree * vfs.f_frsize / 1024);
	}
	put16(f + 1, b.valid ? b.voltage_mv : 0);
	put32(f + 3, used);
	put32(f + 7, total);
	out(f, sizeof(f));
}

/* Our own telemetry (Cayenne LPP): battery voltage and temperature. */
static void self_telemetry(void)
{
	uint8_t f[16] = {PUSH_TELEMETRY_RESPONSE, 0};
	struct battery_status b;
	int i = 8;

	memcpy(f + 2, meshcore_pub_key(), 6);
	battery_get(&b);
	f[i++] = 1;    /* channel */
	f[i++] = 116;  /* voltage, 0.01 V */
	sys_put_be16(b.valid ? b.voltage_mv / 10 : 0, f + i);
	i += 2;
	if (b.valid) {
		f[i++] = 1;
		f[i++] = 103; /* temperature, 0.1 °C */
		sys_put_be16((uint16_t)(int16_t)b.temp_dc, f + i);
		i += 2;
	}
	out(f, i);
}

static void stats(uint8_t type)
{
	const struct mc_mesh *m = meshcore_mesh();
	uint8_t f[30] = {RESP_STATS, type};
	struct mesh_stats s;
	int i = 2;

	mesh_get_stats(&s);
	if (type == 0) {
		struct battery_status b;

		battery_get(&b);
		put16(f + i, b.valid ? b.voltage_mv : 0);
		put32(f + i + 2, k_uptime_get_32() / 1000);
		put16(f + i + 6, 0); /* error flags */
		f[i + 8] = 0;        /* queue */
		i += 9;
	} else if (type == 1) {
		int16_t noise;

		if (mesh_noise_floor(&noise) != 0) {
			noise = 0;
		}
		put16(f + i, noise);
		f[i + 2] = CLAMP(s.last_rssi, INT8_MIN, INT8_MAX);
		f[i + 3] = snr4(s.last_snr);
		put32(f + i + 4, 0); /* airtime: not counted */
		put32(f + i + 8, 0);
		i += 12;
	} else if (type == 2) {
		put32(f + i, m->rx_packets);
		put32(f + i + 4, m->tx_packets);
		/* Flood and direct counts are not kept. */
		put32(f + i + 24, m->rx_undecoded);
		i += 28;
	} else {
		err(ERR_ILLEGAL_ARG);
		return;
	}
	out(f, i);
}

/* A region name from the app ("de-sh" or "#de-sh"): into the settings, lower case. */
static bool scope_name(const uint8_t *f, char *out_name, size_t size)
{
	const char *p = (const char *)f;
	size_t n = 0;

	if (*p == '#') {
		p++;
	}
	while (*p && n < size - 1 && p < (const char *)f + 31) {
		out_name[n++] = *p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p;
		p++;
	}
	out_name[n] = '\0';
	return n > 0 && !*p;
}

static void set_default_scope(const uint8_t *f, size_t len)
{
	struct mesh_mc_config c;
	uint8_t key[16];

	mesh_mc_get_config(&c);
	if (len < 1 + 31 + 16) {
		c.scope[0] = '\0';
	} else {
		char name[sizeof(c.scope)];

		/* The device keeps the name and derives the key: it must be the public one. */
		if (!scope_name(f + 1, name, sizeof(name))) {
			err(ERR_ILLEGAL_ARG);
			return;
		}
		mc_scope_key(name, key);
		if (memcmp(key, f + 32, 16) != 0) {
			err(ERR_ILLEGAL_ARG);
			return;
		}
		snprintf(c.scope, sizeof(c.scope), "%s", name);
	}
	mesh_mc_set_config(&c);
	ok();
}

static void get_default_scope(void)
{
	uint8_t f[48] = {RESP_DEFAULT_FLOOD_SCOPE};
	struct mesh_mc_config c;

	mesh_mc_get_config(&c);
	if (!c.scope[0]) {
		out(f, 1);
		return;
	}
	snprintf((char *)f + 1, 31, "%s", c.scope);
	mc_scope_key(c.scope, f + 32);
	out(f, sizeof(f));
}

void companion_command(const uint8_t *f, size_t len)
{
	struct mt_node *n;
	uint8_t r[COMPANION_FRAME_MAX];

	if (len == 0) {
		return;
	}
	LOG_DBG("app command %u (%u bytes)", f[0], (unsigned)len);
	switch (f[0]) {
	case CMD_DEVICE_QUERY:
		if (len < 2) {
			break;
		}
		app_ver = f[1];
		device_info();
		return;
	case CMD_APP_START:
		if (len < 8) {
			break;
		}
		iter_on = false;
		LOG_INF("app \"%.*s\" connected", (int)MIN(len - 8, 32), (const char *)f + 8);
		self_info();
		return;
	case CMD_SEND_TXT_MSG:
		if (len < 14) {
			break;
		}
		send_text(f, len);
		return;
	case CMD_SEND_CHANNEL_TXT_MSG:
		if (len < 8) {
			break;
		}
		send_channel(f, len);
		return;
	case CMD_GET_CONTACTS:
		if (iter_on) {
			err(ERR_BAD_STATE);
			return;
		}
		iter_since = len >= 5 ? get32(f + 1) : 0;
		iter_newest = 0;
		iter_pos = 0;
		iter_on = true;
		r[0] = RESP_CONTACTS_START;
		put32(r + 1, count_contacts());
		out(r, 5);
		return;
	case CMD_GET_DEVICE_TIME:
		r[0] = RESP_CURR_TIME;
		put32(r + 1, meshcore_time());
		out(r, 5);
		return;
	case CMD_SET_DEVICE_TIME:
		if (len < 5 || get32(f + 1) < TIME_MIN) {
			err(ERR_ILLEGAL_ARG);
			return;
		}
		if (clock_offer(get32(f + 1), CLOCK_SRC_APP)) {
			LOG_INF("clock set by the app");
		}
		ok();
		return;
	case CMD_SEND_SELF_ADVERT:
		meshcore_app_advert(len >= 2 && f[1] == 1);
		ok();
		return;
	case CMD_SET_ADVERT_NAME: {
		struct mesh_config mc;
		char name[32];
		size_t nl = MIN(len - 1, sizeof(name) - 1);

		if (len < 2) {
			break;
		}
		memcpy(name, f + 1, nl);
		name[nl] = '\0';
		name[mc_utf8_prefix(name, nl)] = '\0';
		mesh_get_config(&mc);
		snprintf(mc.long_name, sizeof(mc.long_name), "%s", name);
		mesh_set_config(&mc);
		ok();
		return;
	}
	case CMD_ADD_UPDATE_CONTACT:
		add_contact(f, len);
		return;
	case CMD_SYNC_NEXT_MESSAGE:
		if (off_count) {
			out(offline[0].buf, offline[0].len);
			memmove(&offline[0], &offline[1], (off_count - 1) * sizeof(offline[0]));
			off_count--;
		} else {
			r[0] = RESP_NO_MORE_MESSAGES;
			out(r, 1);
		}
		return;
	case CMD_SET_RADIO_PARAMS:
		set_radio(f, len);
		return;
	case CMD_SET_RADIO_TX_POWER:
		if (len >= 2 && (int8_t)f[1] == TX_POWER_DBM) {
			ok();
		} else {
			err(ERR_ILLEGAL_ARG); /* fixed */
		}
		return;
	case CMD_RESET_PATH:
		n = contact_arg(f, len, 1);
		if (!n) {
			err(ERR_NOT_FOUND);
			return;
		}
		meshcore_reset_path(n->num);
		ok();
		return;
	case CMD_SET_ADVERT_LATLON:
		if (len < 9) {
			break;
		}
		set_latlon(f);
		return;
	case CMD_REMOVE_CONTACT:
		n = contact_arg(f, len, 1);
		if (!n) {
			err(ERR_NOT_FOUND);
			return;
		}
		meshcore_contact_remove(n->num);
		ok();
		return;
	case CMD_GET_CONTACT_BY_KEY: {
		struct mc_app_contact c;

		n = contact_arg(f, len, 1);
		if (!n) {
			err(ERR_NOT_FOUND);
			return;
		}
		meshcore_contact_info(n, &c);
		out(r, contact_frame(r, RESP_CONTACT, &c));
		return;
	}
	case CMD_EXPORT_CONTACT:
		if (len >= 33) {
			/* Other contacts' signed adverts are not kept. */
			err(ERR_UNSUPPORTED);
			return;
		}
		r[0] = RESP_EXPORT_CONTACT;
		out(r, 1 + meshcore_export_self(r + 1));
		return;
	case CMD_IMPORT_CONTACT:
		if (len > 2 + 32 + 64 && meshcore_import(f + 1, len - 1)) {
			ok();
		} else {
			err(ERR_ILLEGAL_ARG);
		}
		return;
	case CMD_REBOOT:
		if (len >= 7 && memcmp(f + 1, "reboot", 6) == 0) {
			LOG_INF("restart requested by the app");
			sys_reboot(SYS_REBOOT_COLD);
		}
		break;
	case CMD_GET_BATT_AND_STORAGE:
		batt_storage();
		return;
	case CMD_SET_TUNING_PARAMS:
		ok(); /* repeater delays: the device does not repeat */
		return;
	case CMD_GET_TUNING_PARAMS:
		r[0] = RESP_TUNING_PARAMS;
		put32(r + 1, 0);
		put32(r + 5, 1000);
		out(r, 9);
		return;
	case CMD_EXPORT_PRIVATE_KEY:
	case CMD_IMPORT_PRIVATE_KEY:
		r[0] = RESP_DISABLED;
		out(r, 1);
		return;
	case CMD_SEND_LOGIN:
		if (len < 33) {
			break;
		}
		send_login(f, len);
		return;
	case CMD_SEND_STATUS_REQ: {
		uint32_t tag;

		n = contact_arg(f, len, 1);
		if (!n) {
			err(ERR_NOT_FOUND);
			return;
		}
		if (typed_request(n, REQ_STATUS, 0, false, &tag)) {
			pending_status = key4(n->user.public_key);
		}
		return;
	}
	case CMD_SEND_BINARY_REQ: {
		uint32_t tag;

		n = contact_arg(f, len, 1);
		if (!n || len < 34) {
			err(n ? ERR_ILLEGAL_ARG : ERR_NOT_FOUND);
			return;
		}
		if (request(n, f + 33, len - 33, false, &tag)) {
			pending_req = tag;
		}
		return;
	}
	case CMD_SEND_PATH_DISCOVERY_REQ: {
		uint32_t tag;

		n = len >= 2 && f[1] == 0 ? contact_arg(f, len, 2) : NULL;
		if (!n) {
			err(ERR_NOT_FOUND);
			return;
		}
		/* A flooded telemetry request (base only): the answer brings both paths. */
		if (typed_request(n, REQ_TELEMETRY, (uint8_t)~TELEM_PERM_BASE, true, &tag)) {
			pending_discovery = tag;
		}
		return;
	}
	case CMD_SEND_TRACE_PATH:
		if (len <= 10) {
			break;
		}
		send_trace(f, len);
		return;
	case CMD_HAS_CONNECTION:
		err(ERR_NOT_FOUND);
		return;
	case CMD_LOGOUT:
		ok();
		return;
	case CMD_GET_CHANNEL:
		if (len < 2) {
			break;
		}
		get_channel(f[1]);
		return;
	case CMD_SET_CHANNEL:
		if (len < 2 + 32 + 16) {
			break;
		}
		set_channel(f, len);
		return;
	case CMD_SET_DEVICE_PIN:
		/* Only 0: a new PIN on the display each time. */
		if (len >= 5 && get32(f + 1) == 0) {
			ok();
		} else {
			err(ERR_ILLEGAL_ARG);
		}
		return;
	case CMD_SET_OTHER_PARAMS:
		if (len < 2) {
			break;
		}
		set_other(f, len);
		return;
	case CMD_SEND_TELEMETRY_REQ: {
		uint32_t tag;

		if (len == 4) {
			self_telemetry();
			return;
		}
		n = contact_arg(f, len, 4);
		if (!n) {
			err(ERR_NOT_FOUND);
			return;
		}
		if (typed_request(n, REQ_TELEMETRY, 0, false, &tag)) {
			pending_telemetry = tag;
		}
		return;
	}
	case CMD_GET_CUSTOM_VARS:
		r[0] = RESP_CUSTOM_VARS;
		out(r, 1);
		return;
	case CMD_SET_CUSTOM_VAR:
		err(ERR_ILLEGAL_ARG);
		return;
	case CMD_GET_ADVERT_PATH:
		err(ERR_NOT_FOUND);
		return;
	case CMD_SET_FLOOD_SCOPE_KEY:
		if (len >= 2 && f[1] == 0) {
			meshcore_app_scope(len >= 18 ? f + 2 : NULL, false);
			ok();
			return;
		}
		if (len >= 2 && f[1] == 1) {
			meshcore_app_scope(NULL, true);
			ok();
			return;
		}
		break;
	case CMD_GET_STATS:
		if (len < 2) {
			break;
		}
		stats(f[1]);
		return;
	case CMD_SET_AUTOADD_CONFIG:
		if (len < 2) {
			break;
		}
		prefs.autoadd = f[1];
		if (len >= 3) {
			prefs.autoadd_hops = MIN(f[2], 64);
		}
		save_prefs();
		ok();
		return;
	case CMD_GET_AUTOADD_CONFIG:
		r[0] = RESP_AUTOADD_CONFIG;
		r[1] = prefs.autoadd;
		r[2] = prefs.autoadd_hops;
		out(r, 3);
		return;
	case CMD_GET_ALLOWED_REPEAT_FREQ:
		r[0] = RESP_ALLOWED_REPEAT_FREQ; /* none: the device does not repeat */
		out(r, 1);
		return;
	case CMD_SET_PATH_HASH_MODE:
		if (len >= 3 && f[1] == 0 && f[2] == 0) {
			ok();
		} else {
			err(ERR_ILLEGAL_ARG);
		}
		return;
	case CMD_SET_DEFAULT_FLOOD_SCOPE:
		set_default_scope(f, len);
		return;
	case CMD_GET_DEFAULT_FLOOD_SCOPE:
		get_default_scope();
		return;
	default:
		break;
	}
	LOG_DBG("app command %u not supported", f[0]);
	err(ERR_UNSUPPORTED);
}

/* Transport side ------------------------------------------------------------------- */

void companion_attach(void (*fn)(void))
{
	mesh_lock();
	wake = fn;
	mesh_unlock();
}

void companion_connected(bool on)
{
	mesh_lock();
	if (on != connected) {
		LOG_INF("app %s", on ? "connected" : "disconnected");
	}
	connected = on;
	out_count = 0;
	iter_on = false;
	if (!on) {
		app_ver = 0;
		meshcore_app_scope(NULL, false);
	}
	mesh_unlock();
}

bool companion_is_connected(void)
{
	return connected;
}

int companion_put(const uint8_t *frame, size_t len)
{
	if (len == 0 || len > COMPANION_FRAME_MAX) {
		return -EINVAL;
	}
	return meshcore_app_frame(frame, len);
}

int companion_get(uint8_t buf[COMPANION_FRAME_MAX])
{
	int len = 0;

	mesh_lock();
	if (out_count) {
		struct frame *f = &outq[out_head];

		len = f->len;
		memcpy(buf, f->buf, len);
		out_head = (out_head + 1) % OUT_LEN;
		out_count--;
	} else if (iter_on) {
		len = next_contact(buf);
	}
	mesh_unlock();
	return len;
}
