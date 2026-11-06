/* MeshCore client: see mesh.h. */

#include <string.h>

#include "meshcore/crypto.h"
#include "meshcore/mesh.h"
#include "meshtastic/crypto.h"

/* Delays as MeshCore's BaseChatMesh (ms). */
#define ACK_DELAY_MS   200 /* TXT_ACK_DELAY */
#define RPATH_DELAY_MS 500 /* reciprocal path after a flooded PATH */
/* Send timeouts as the companion firmware. */
#define TIMEOUT_BASE_MS        500
#define FLOOD_TIMEOUT_FACTOR   16
#define DIRECT_PERHOP_FACTOR   6
#define DIRECT_PERHOP_EXTRA_MS 250
/* A channel message counts as relayed if a repeater sends it on within this. */
#define REPEAT_WAIT_MS 15000

#define TXT_TYPE_PLAIN  0
#define TXT_TYPE_SIGNED 2

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool before(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

void mc_mesh_init(struct mc_mesh *m, const struct mc_mesh_ops *ops, void *ctx,
		  const uint8_t pub[32], const uint8_t prv[64])
{
	memset(m, 0, sizeof(*m));
	m->ops = ops;
	m->ctx = ctx;
	memcpy(m->pub, pub, 32);
	memcpy(m->prv, prv, 64);
	m->sf = 8;
	m->bw_hz = 62500;
	m->cr = 5;
	m->preamble = 32;
}

void mc_mesh_set_radio(struct mc_mesh *m, uint8_t sf, uint32_t bw_hz, uint8_t cr,
		       uint16_t preamble)
{
	m->sf = sf;
	m->bw_hz = bw_hz;
	m->cr = cr;
	m->preamble = preamble;
}

void mc_mesh_set_scope(struct mc_mesh *m, const uint8_t key[16])
{
	m->scoped = key != NULL;
	if (key) {
		memcpy(m->scope, key, 16);
	}
}

void mc_mesh_set_channel(struct mc_mesh *m, int i, const uint8_t secret[16])
{
	struct mc_channel *c = &m->channels[i];

	memset(c, 0, sizeof(*c));
	if (secret) {
		c->used = true;
		memcpy(c->secret, secret, 16);
		c->hash = mc_channel_hash(secret);
	}
}

uint32_t mc_mesh_airtime_ms(const struct mc_mesh *m, size_t len)
{
	/* Semtech's formula: explicit header, CRC on. */
	uint32_t tsym_us = (uint32_t)(((uint64_t)1000000 << m->sf) / m->bw_hz);
	int de = tsym_us >= 16000;
	int num = 8 * (int)len - 4 * m->sf + 28 + 16;
	int den = 4 * (m->sf - 2 * de);
	int symbols = 8 + (num > 0 ? (num + den - 1) / den * m->cr : 0);
	uint64_t us = (uint64_t)(m->preamble * 4 + 17) * tsym_us / 4 + (uint64_t)symbols * tsym_us;

	return (uint32_t)((us + 999) / 1000);
}

/* Duplicates ------------------------------------------------------------------------ */

static bool was_seen(const struct mc_mesh *m, const uint8_t hash[MC_HASH_SIZE])
{
	for (int i = 0; i < MC_SEEN_LEN; i++) {
		if (memcmp(m->seen[i], hash, MC_HASH_SIZE) == 0) {
			return true;
		}
	}
	return false;
}

static void mark_seen(struct mc_mesh *m, const uint8_t hash[MC_HASH_SIZE])
{
	memcpy(m->seen[m->seen_pos], hash, MC_HASH_SIZE);
	m->seen_pos = (m->seen_pos + 1) % MC_SEEN_LEN;
}

/* Sending ---------------------------------------------------------------------------- */

static bool queue(struct mc_mesh *m, const struct mc_packet *p, uint32_t delay, uint32_t id)
{
	for (int i = 0; i < MC_TXQ_LEN; i++) {
		struct mc_txq_entry *e = &m->txq[i];

		if (!e->used) {
			e->used = true;
			e->due = m->now + delay;
			e->id = id;
			e->len = mc_packet_encode(p, e->frame);
			return true;
		}
	}
	return false;
}

/* Flood, with our region scope if there is one. The packet's hash is returned. */
static bool send_flood(struct mc_mesh *m, struct mc_packet *p, uint32_t delay, uint32_t id,
		       uint8_t hash[MC_HASH_SIZE])
{
	uint8_t h[MC_HASH_SIZE];

	p->header &= ~0x03;
	if (m->scoped) {
		p->header |= MC_ROUTE_TRANSPORT_FLOOD;
		p->transport[0] = mc_transport_code(m->scope, p);
		p->transport[1] = 0;
	} else {
		p->header |= MC_ROUTE_FLOOD;
	}
	p->path_len = 0;
	mc_packet_hash(p, hash ? hash : h);
	mark_seen(m, hash ? hash : h); /* not taken for new when repeated back */
	return queue(m, p, delay, id);
}

/* Along a path (path_len 0: to direct neighbours only). */
static bool send_direct(struct mc_mesh *m, struct mc_packet *p, const uint8_t *path,
			uint8_t path_len, uint32_t delay, uint32_t id)
{
	uint8_t h[MC_HASH_SIZE];

	p->header = (p->header & ~0x03) | MC_ROUTE_DIRECT;
	p->path_len = path_len;
	memcpy(p->path, path, mc_path_bytes(path_len));
	mc_packet_hash(p, h);
	mark_seen(m, h);
	return queue(m, p, delay, id);
}

/* To a contact: along its path if known, else by flood. */
static bool send_to(struct mc_mesh *m, struct mc_packet *p, const struct mc_contact *c,
		    uint32_t delay, uint32_t id)
{
	if (c->path_len == MC_PATH_UNKNOWN) {
		return send_flood(m, p, delay, id, NULL);
	}
	return send_direct(m, p, c->path, c->path_len, delay, id);
}

/* dest hash, src hash, MAC and encrypted data. */
static void datagram(struct mc_mesh *m, struct mc_packet *p, uint8_t type,
		     const struct mc_contact *to, const uint8_t *data, size_t len)
{
	p->header = type << 2;
	p->payload[0] = to->pub[0];
	p->payload[1] = m->pub[0];
	p->payload_len = 2 + mc_encrypt_then_mac(to->secret, p->payload + 2, data, len);
}

/* Our path to the contact (as it reached us), with an ACK or response in it. */
static bool path_return(struct mc_mesh *m, struct mc_packet *p, const struct mc_contact *to,
			const uint8_t *path, uint8_t path_len, uint8_t extra_type,
			const uint8_t *extra, size_t extra_len)
{
	uint8_t data[MC_MAX_PAYLOAD];
	int n = mc_path_bytes(path_len);
	size_t len = 0;

	if (n + extra_len + 5 > MC_MAX_PAYLOAD - 2 - 16) {
		return false;
	}
	data[len++] = path_len;
	memcpy(data + len, path, n);
	len += n;
	if (extra_len) {
		data[len++] = extra_type;
		memcpy(data + len, extra, extra_len);
		len += extra_len;
	} else {
		/* A dummy type and random bytes keep the packet hash unique. */
		data[len++] = 0xff;
		put32(data + len, m->ops->random(m->ctx));
		len += 4;
	}
	datagram(m, p, MC_PT_PATH, to, data, len);
	return true;
}

/* The ACK value of a message: SHA-256 of time, flags and text and the sender's key. */
static uint32_t ack_of(const uint8_t *data, size_t len, const uint8_t pub[32])
{
	struct mt_sha256 s;
	uint8_t h[32];

	mt_sha256_init(&s);
	mt_sha256_update(&s, data, len);
	mt_sha256_update(&s, pub, 32);
	mt_sha256_final(&s, h);
	return get32(h);
}

static uint32_t timeout_ms(const struct mc_mesh *m, size_t frame_len, uint8_t path_len)
{
	uint32_t t = mc_mesh_airtime_ms(m, frame_len);

	if (path_len == MC_PATH_UNKNOWN) {
		return TIMEOUT_BASE_MS + FLOOD_TIMEOUT_FACTOR * t;
	}
	return TIMEOUT_BASE_MS +
	       (t * DIRECT_PERHOP_FACTOR + DIRECT_PERHOP_EXTRA_MS) * (mc_path_count(path_len) + 1);
}

static bool send_attempt(struct mc_mesh *m, struct mc_pending *pd)
{
	uint8_t data[5 + MC_MAX_TEXT];
	size_t tlen = strlen(pd->text), frame_len;
	struct mc_packet p;

	put32(data, pd->timestamp);
	data[4] = pd->attempt & 3;
	memcpy(data + 5, pd->text, tlen);
	pd->acks[pd->attempt] = ack_of(data, 5 + tlen, m->pub);
	datagram(m, &p, MC_PT_TXT_MSG, &pd->to, data, 5 + tlen);
	/* The last attempt floods: the path may be broken. */
	if (pd->attempt == MC_ATTEMPTS - 1 && pd->to.path_len != MC_PATH_UNKNOWN) {
		pd->to.path_len = MC_PATH_UNKNOWN;
		m->ops->path(m->ctx, pd->to.num, NULL, MC_PATH_UNKNOWN);
	}
	frame_len = 2 + p.payload_len;
	if (pd->to.path_len == MC_PATH_UNKNOWN) {
		frame_len += m->scoped ? 4 : 0;
	} else {
		frame_len += mc_path_bytes(pd->to.path_len);
	}
	pd->deadline = m->now + timeout_ms(m, frame_len, pd->to.path_len);
	return send_to(m, &p, &pd->to, 0, pd->attempt == 0 ? pd->id : 0);
}

static struct mc_pending *new_pending(struct mc_mesh *m)
{
	for (int i = 0; i < MC_PENDING_LEN; i++) {
		if (!m->pending[i].used) {
			memset(&m->pending[i], 0, sizeof(m->pending[i]));
			return &m->pending[i];
		}
	}
	return NULL;
}

static uint32_t new_id(struct mc_mesh *m)
{
	uint32_t id;

	do {
		id = m->ops->random(m->ctx);
	} while (id == 0);
	return id;
}

void mc_mesh_send_advert(struct mc_mesh *m, const struct mc_advert *a, bool flood)
{
	struct mc_packet p;

	mc_advert_encode(&p, a, m->prv);
	if (flood) {
		send_flood(m, &p, 0, 0, NULL);
	} else {
		send_direct(m, &p, NULL, 0, 0, 0);
	}
}

uint32_t mc_mesh_send_text(struct mc_mesh *m, const struct mc_contact *to, uint32_t timestamp,
			   const char *text)
{
	struct mc_pending *pd = new_pending(m);
	size_t n = mc_utf8_prefix(text, MC_MAX_TEXT);

	if (!pd) {
		return 0;
	}
	pd->used = true;
	pd->id = new_id(m);
	pd->to = *to;
	pd->timestamp = timestamp;
	memcpy(pd->text, text, n);
	pd->text[n] = '\0';
	if (!send_attempt(m, pd)) {
		pd->used = false;
		return 0;
	}
	return pd->id;
}

uint32_t mc_mesh_send_group(struct mc_mesh *m, int channel, uint32_t timestamp,
			    const char *sender, const char *text)
{
	uint8_t data[5 + MC_MAX_TEXT];
	struct mc_packet p;
	struct mc_pending *pd;
	size_t sn, tn, len;

	if (channel < 0 || channel >= MC_CHANNELS || !m->channels[channel].used ||
	    !(pd = new_pending(m))) {
		return 0;
	}
	put32(data, timestamp);
	data[4] = TXT_TYPE_PLAIN;
	sn = mc_utf8_prefix(sender, MC_MAX_TEXT / 2);
	memcpy(data + 5, sender, sn);
	memcpy(data + 5 + sn, ": ", 2);
	tn = mc_utf8_prefix(text, MC_MAX_TEXT - sn - 2);
	memcpy(data + 7 + sn, text, tn);
	len = 7 + sn + tn;

	p.header = MC_PT_GRP_TXT << 2;
	p.payload[0] = m->channels[channel].hash;
	p.payload_len = 1 + mc_encrypt_then_mac(m->channels[channel].secret, p.payload + 1, data,
						len);
	pd->used = true;
	pd->group = true;
	pd->id = new_id(m);
	pd->deadline = m->now + REPEAT_WAIT_MS;
	if (!send_flood(m, &p, 0, pd->id, pd->hash)) {
		pd->used = false;
		return 0;
	}
	return pd->id;
}

/* Receiving ------------------------------------------------------------------------- */

static void process_ack(struct mc_mesh *m, uint32_t ack)
{
	for (int i = 0; i < MC_PENDING_LEN; i++) {
		struct mc_pending *pd = &m->pending[i];

		if (!pd->used || pd->group) {
			continue;
		}
		for (int a = 0; a <= pd->attempt; a++) {
			if (pd->acks[a] == ack) {
				pd->used = false;
				m->ops->status(m->ctx, pd->id, MC_TX_ACKED);
				return;
			}
		}
	}
}

/* One of our channel messages came back from a repeater. */
static void check_repeat(struct mc_mesh *m, const uint8_t hash[MC_HASH_SIZE])
{
	for (int i = 0; i < MC_PENDING_LEN; i++) {
		struct mc_pending *pd = &m->pending[i];

		if (pd->used && pd->group && memcmp(pd->hash, hash, MC_HASH_SIZE) == 0) {
			pd->used = false;
			m->ops->status(m->ctx, pd->id, MC_TX_ACKED);
		}
	}
}

/* A retried message (same sender and time) was delivered already. */
static bool recent(struct mc_mesh *m, uint32_t from, uint32_t timestamp)
{
	for (size_t i = 0; i < sizeof(m->recent) / sizeof(m->recent[0]); i++) {
		if (m->recent[i].from == from && m->recent[i].timestamp == timestamp) {
			return true;
		}
	}
	m->recent[m->recent_pos].from = from;
	m->recent[m->recent_pos].timestamp = timestamp;
	m->recent_pos = (m->recent_pos + 1) % (sizeof(m->recent) / sizeof(m->recent[0]));
	return false;
}

/* Acknowledge a message: with our path back if it came by flood. */
static void reply_ack(struct mc_mesh *m, const struct mc_packet *in, const struct mc_contact *c,
		      const uint8_t *ack, size_t ack_len)
{
	struct mc_packet p;

	if (mc_is_flood(in)) {
		if (path_return(m, &p, c, in->path, in->path_len, MC_PT_ACK, ack, ack_len)) {
			send_flood(m, &p, ACK_DELAY_MS, 0, NULL);
		}
		return;
	}
	p.header = MC_PT_ACK << 2;
	memcpy(p.payload, ack, ack_len);
	p.payload_len = ack_len;
	send_to(m, &p, c, ACK_DELAY_MS, 0);
}

static void on_text(struct mc_mesh *m, const struct mc_packet *in, const struct mc_rx_info *rx,
		    const struct mc_contact *c, uint8_t *data, size_t len)
{
	uint32_t timestamp = get32(data);
	uint8_t type = data[4] >> 2;
	uint8_t ack[6];
	const char *text;
	size_t tlen;

	data[len] = '\0'; /* the text is zero-padded */
	if (type == TXT_TYPE_PLAIN) {
		text = (const char *)data + 5;
		tlen = strlen(text);
		put32(ack, ack_of(data, 5 + tlen, c->pub));
		/* The extended attempt number (if any), then a random byte. */
		ack[4] = 5 + tlen + 1 < len ? data[5 + tlen + 1] : 0;
		ack[5] = m->ops->random(m->ctx);
		reply_ack(m, in, c, ack, 6);
	} else if (type == TXT_TYPE_SIGNED && len > 9) {
		/* A room server's post: 4 bytes of the author's key before the text. */
		text = (const char *)data + 9;
		tlen = strlen(text);
		put32(ack, ack_of(data, 9 + tlen, m->pub));
		reply_ack(m, in, c, ack, 4);
	} else {
		return; /* CLI data (replies of a repeater) */
	}
	if (!recent(m, c->num, timestamp)) {
		m->ops->text(m->ctx, c->num, timestamp, text, rx);
	}
}

static void on_path(struct mc_mesh *m, const struct mc_packet *in, const struct mc_contact *c,
		    const uint8_t *data, size_t len)
{
	uint8_t path_len = data[0];
	int n = mc_path_bytes(path_len);
	const uint8_t *extra;
	uint8_t extra_type;
	struct mc_packet p;

	if (!mc_path_valid(path_len) || (size_t)(2 + n) > len) {
		return;
	}
	extra_type = data[1 + n] & 0x0f;
	extra = data + 2 + n;
	m->ops->path(m->ctx, c->num, data + 1, path_len);
	for (int i = 0; i < MC_PENDING_LEN; i++) {
		struct mc_pending *pd = &m->pending[i];

		if (pd->used && !pd->group && pd->to.num == c->num) {
			pd->to.path_len = path_len;
			memcpy(pd->to.path, data + 1, n);
		}
	}
	if (extra_type == MC_PT_ACK && len - 2 - n >= 4) {
		process_ack(m, get32(extra));
	}
	/* Flooded: tell the sender our way back, directly along its path. */
	if (mc_is_flood(in) && path_return(m, &p, c, in->path, in->path_len, 0, NULL, 0)) {
		send_direct(m, &p, data + 1, path_len, RPATH_DELAY_MS, 0);
	}
}

/* TXT_MSG, PATH and the like: from a contact, encrypted with our shared secret. */
static void on_datagram(struct mc_mesh *m, const struct mc_packet *p, const struct mc_rx_info *rx)
{
	struct mc_contact peers[MC_MAX_PEERS];
	uint8_t data[MC_MAX_PAYLOAD + 1];
	int n;

	if (p->payload_len <= 2 + MC_MAC_SIZE || p->payload[0] != m->pub[0]) {
		return;
	}
	n = m->ops->find_peers(m->ctx, p->payload[1], peers, MC_MAX_PEERS);
	for (int i = 0; i < n; i++) {
		int len = mc_mac_then_decrypt(peers[i].secret, data, p->payload + 2,
					      p->payload_len - 2);

		if (len <= 0) {
			continue;
		}
		if (mc_type(p) == MC_PT_TXT_MSG && len > 5) {
			on_text(m, p, rx, &peers[i], data, len);
		} else if (mc_type(p) == MC_PT_PATH) {
			on_path(m, p, &peers[i], data, len);
		}
		return;
	}
}

static void on_group(struct mc_mesh *m, const struct mc_packet *p, const struct mc_rx_info *rx)
{
	uint8_t data[MC_MAX_PAYLOAD + 1];

	if (p->payload_len <= 1 + MC_MAC_SIZE) {
		return;
	}
	for (int i = 0; i < MC_CHANNELS; i++) {
		const struct mc_channel *c = &m->channels[i];
		int len;

		if (!c->used || c->hash != p->payload[0]) {
			continue;
		}
		len = mc_mac_then_decrypt(c->secret, data, p->payload + 1, p->payload_len - 1);
		if (len <= 0) {
			continue;
		}
		if (len >= 5 && (data[4] >> 2) == TXT_TYPE_PLAIN) {
			data[len] = '\0';
			m->ops->group_text(m->ctx, i, get32(data), (const char *)data + 5, rx);
		}
		return;
	}
}

void mc_mesh_receive(struct mc_mesh *m, const uint8_t *frame, size_t len, int16_t rssi,
		     int8_t snr, uint32_t now)
{
	struct mc_packet p;
	struct mc_rx_info rx = {.rssi = rssi, .snr = snr};
	uint8_t hash[MC_HASH_SIZE];

	m->now = now;
	m->rx_packets++;
	if (mc_packet_decode(&p, frame, len) != 0) {
		m->rx_undecoded++;
		return;
	}
	rx.flood = mc_is_flood(&p);
	/* Direct: at the end of a route, or zero-hop (adverts are only sent so). */
	rx.hops = rx.flood ? mc_path_count(p.path_len) : mc_type(&p) == MC_PT_ADVERT ? 0 : 0xff;
	rx.path_len = p.path_len;
	rx.path = p.path;
	/* Routed to someone else (we do not repeat); an ACK may be ours, heard early. */
	if (!rx.flood && mc_path_count(p.path_len) > 0) {
		if (mc_type(&p) == MC_PT_ACK && p.payload_len >= 4) {
			process_ack(m, get32(p.payload));
		}
		return;
	}
	mc_packet_hash(&p, hash);
	if (rx.flood && mc_path_count(p.path_len) > 0) {
		check_repeat(m, hash);
	}
	if (was_seen(m, hash)) {
		m->rx_dupes++;
		return;
	}
	mark_seen(m, hash);
	switch (mc_type(&p)) {
	case MC_PT_ACK:
		if (p.payload_len >= 4) {
			process_ack(m, get32(p.payload));
		}
		break;
	case MC_PT_TXT_MSG:
	case MC_PT_PATH:
		on_datagram(m, &p, &rx);
		break;
	case MC_PT_GRP_TXT:
		on_group(m, &p, &rx);
		break;
	case MC_PT_ADVERT: {
		struct mc_advert a;

		if (p.payload_len >= 32 && memcmp(p.payload, m->pub, 32) != 0 &&
		    mc_advert_decode(&p, &a) == 0) {
			m->ops->advert(m->ctx, &a, &rx);
		}
		break;
	}
	default:
		break;
	}
}

/* Timeouts and transmission ------------------------------------------------------------ */

uint32_t mc_mesh_poll(struct mc_mesh *m, uint32_t now)
{
	uint32_t next = UINT32_MAX;

	m->now = now;
	for (int i = 0; i < MC_PENDING_LEN; i++) {
		struct mc_pending *pd = &m->pending[i];

		if (!pd->used || before(now, pd->deadline)) {
			continue;
		}
		if (pd->group) {
			pd->used = false;
			m->ops->status(m->ctx, pd->id, MC_TX_NO_ACK);
		} else if (pd->attempt + 1 < MC_ATTEMPTS) {
			pd->attempt++;
			if (!send_attempt(m, pd)) {
				pd->deadline = now + 1000; /* queue full: try again */
			}
		} else {
			pd->used = false;
			m->ops->status(m->ctx, pd->id, MC_TX_FAILED);
		}
	}
	for (;;) {
		struct mc_txq_entry *due = NULL;

		for (int i = 0; i < MC_TXQ_LEN; i++) {
			struct mc_txq_entry *e = &m->txq[i];

			if (e->used && !before(now, e->due) && (!due || before(e->due, due->due))) {
				due = e;
			}
		}
		if (!due) {
			break;
		}
		due->used = false;
		m->tx_packets++;
		/* Before transmitting: an answer may come before transmit() returns. */
		if (due->id) {
			m->ops->status(m->ctx, due->id, MC_TX_SENT);
		}
		m->ops->transmit(m->ctx, due->frame, due->len);
	}
	for (int i = 0; i < MC_TXQ_LEN; i++) {
		if (m->txq[i].used && m->txq[i].due - now < next) {
			next = m->txq[i].due - now;
		}
	}
	for (int i = 0; i < MC_PENDING_LEN; i++) {
		if (m->pending[i].used && m->pending[i].deadline - now < next) {
			next = m->pending[i].deadline - now;
		}
	}
	return next;
}
