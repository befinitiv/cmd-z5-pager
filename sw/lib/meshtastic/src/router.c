#include <string.h>

#include "meshtastic/router.h"

/* Contention window (Meshtastic RadioInterface): CWmin 3, CWmax 8 slots (log2). */
#define CW_MIN 3
#define CW_MAX 8
#define SNR_MIN (-20)
#define SNR_MAX 10
#define PROCESSING_MS 4500

static bool due(uint32_t now, uint32_t t)
{
	return (int32_t)(now - t) >= 0;
}

void mt_router_init(struct mt_router *r, const struct mt_router_ops *ops, void *ctx,
		    uint32_t my_num, struct mt_nodedb *db)
{
	memset(r, 0, sizeof(*r));
	r->ops = ops;
	r->ctx = ctx;
	r->my_num = my_num;
	r->db = db;
	r->rebroadcast = true;
}

void mt_router_set_keys(struct mt_router *r, const uint8_t priv[32])
{
	memcpy(r->priv, priv, 32);
	mt_x25519_public(r->pub, priv);
	r->has_keys = true;
	for (int i = 0; i < r->db->count; i++) {
		r->db->nodes[i].has_shared_key = false;
	}
}

void mt_router_set_radio(struct mt_router *r, const struct mt_radio_params *p)
{
	r->radio = *p;
}

uint32_t mt_router_retry_ms(const struct mt_router *r, size_t len)
{
	uint32_t slots = (1u << CW_MIN) + 2 * CW_MAX + (1u << ((CW_MAX + CW_MIN) / 2));

	return 2 * mt_airtime_ms(&r->radio, len) + slots * r->radio.slot_ms + PROCESSING_MS;
}

static uint32_t rnd(struct mt_router *r)
{
	return r->ops->random(r->ctx);
}

/* Duplicate history ---------------------------------------------------------- */

static bool seen(const struct mt_router *r, uint32_t from, uint32_t id)
{
	for (int i = 0; i < MT_SEEN_LEN; i++) {
		if (r->seen[i].id == id && r->seen[i].from == from && (id || from)) {
			return true;
		}
	}
	return false;
}

static void mark_seen(struct mt_router *r, uint32_t from, uint32_t id)
{
	r->seen[r->seen_pos].from = from;
	r->seen[r->seen_pos].id = id;
	r->seen_pos = (r->seen_pos + 1) % MT_SEEN_LEN;
}

/* Transmit queue -------------------------------------------------------------- */

static bool enqueue(struct mt_router *r, const uint8_t *frame, size_t len, uint32_t delay,
		    bool relay, uint32_t from, uint32_t id)
{
	for (int i = 0; i < MT_TXQ_LEN; i++) {
		struct mt_txq_entry *e = &r->txq[i];

		if (!e->used) {
			e->used = true;
			e->due = r->now + delay;
			e->relay = relay;
			e->from = from;
			e->id = id;
			e->len = len;
			memcpy(e->frame, frame, len);
			return true;
		}
	}
	return false;
}

/* Cancel a queued rebroadcast (someone else relayed it, or it was answered). */
static void cancel_relay(struct mt_router *r, uint32_t from, uint32_t id)
{
	for (int i = 0; i < MT_TXQ_LEN; i++) {
		struct mt_txq_entry *e = &r->txq[i];

		if (e->used && e->relay && e->from == from && e->id == id) {
			e->used = false;
			r->relay_cancelled++;
		}
	}
}

static struct mt_pending *find_pending(struct mt_router *r, uint32_t id)
{
	for (int i = 0; i < MT_PENDING_LEN; i++) {
		if (r->pending[i].used && r->pending[i].id == id) {
			return &r->pending[i];
		}
	}
	return NULL;
}

/* Encryption -------------------------------------------------------------------- */

static bool shared_key(struct mt_router *r, struct mt_node *n)
{
	if (!r->has_keys || !n || !n->has_user || !n->user.has_public_key) {
		return false;
	}
	if (!n->has_shared_key) {
		n->has_shared_key = mt_pki_shared_key(n->shared_key, r->priv, n->user.public_key);
	}
	return n->has_shared_key;
}

static bool decode(struct mt_router *r, const struct mt_header *h, const uint8_t *payload,
		   size_t len, struct mt_rx *rx)
{
	if (h->channel == 0 && h->to == r->my_num && len > MT_PKI_OVERHEAD) {
		struct mt_node *n = mt_nodedb_find(r->db, h->from);

		if (shared_key(r, n)) {
			int pl = mt_pki_decrypt(n->shared_key, h->from, h->id, payload, len, rx->plain);

			if (pl >= 0 && mt_data_decode(rx->plain, pl, &rx->d) == 0) {
				rx->pki = true;
				rx->channel_index = 0;
				return true;
			}
		}
	}
	for (int i = 0; i < r->nchannels; i++) {
		const struct mt_channel *c = &r->channels[i];

		if (c->hash != h->channel) {
			continue;
		}
		memcpy(rx->plain, payload, len);
		mt_channel_crypt(c->key, c->key_len, h->from, h->id, rx->plain, len);
		if (mt_data_decode(rx->plain, len, &rx->d) == 0) {
			rx->pki = false;
			rx->channel_index = i;
			return true;
		}
	}
	return false;
}

/* Ports that are never PKI-encrypted (as in the firmware). */
static bool pki_port(uint32_t port)
{
	return port != MT_PORT_ROUTING && port != MT_PORT_NODEINFO && port != MT_PORT_POSITION &&
	       port != MT_PORT_TRACEROUTE;
}

uint32_t mt_router_send(struct mt_router *r, uint32_t to, uint8_t channel_index, uint32_t portnum,
			const void *payload, size_t len, bool want_ack, bool want_response,
			uint32_t request_id)
{
	uint8_t frame[MT_MAX_FRAME];
	uint8_t plain[MT_MAX_PAYLOAD];
	struct mt_data d = {
		.portnum = portnum,
		.payload = payload,
		.payload_len = len,
		.want_response = want_response,
		.request_id = request_id,
	};
	struct mt_header h = {
		.to = to,
		.from = r->my_num,
		.hop_limit = MT_HOP_LIMIT,
		.hop_start = MT_HOP_LIMIT,
		.want_ack = want_ack,
		.relay_node = r->my_num & 0xff,
	};
	int n = mt_data_encode(&d, plain, sizeof(plain));
	size_t flen;

	if (n < 0 || channel_index >= r->nchannels) {
		return 0;
	}
	do {
		h.id = rnd(r);
	} while (h.id == 0);

	struct mt_node *dest = to != MT_BROADCAST ? mt_nodedb_find(r->db, to) : NULL;

	if (dest && pki_port(portnum) && shared_key(r, dest)) {
		if (n + MT_PKI_OVERHEAD > MT_MAX_PAYLOAD) {
			return 0;
		}
		h.channel = 0;
		mt_pki_encrypt(dest->shared_key, h.from, h.id, rnd(r), plain, n,
			       frame + MT_HEADER_LEN);
		flen = MT_HEADER_LEN + n + MT_PKI_OVERHEAD;
	} else {
		const struct mt_channel *c = &r->channels[channel_index];

		h.channel = c->hash;
		mt_channel_crypt(c->key, c->key_len, h.from, h.id, plain, n);
		memcpy(frame + MT_HEADER_LEN, plain, n);
		flen = MT_HEADER_LEN + n;
	}
	mt_header_encode(&h, frame);

	struct mt_pending *p = NULL;

	if (want_ack) {
		for (int i = 0; i < MT_PENDING_LEN && !p; i++) {
			if (!r->pending[i].used) {
				p = &r->pending[i];
			}
		}
		if (!p) {
			return 0;
		}
	}
	uint32_t delay = (rnd(r) % (1u << CW_MIN)) * r->radio.slot_ms;

	if (!enqueue(r, frame, flen, delay, false, h.from, h.id)) {
		return 0;
	}
	if (p) {
		*p = (struct mt_pending){.used = true, .id = h.id, .to = to, .retries = MT_RETRIES,
					 .len = flen};
		memcpy(p->frame, frame, flen);
	}
	mark_seen(r, h.from, h.id);
	return h.id;
}

static void send_ack(struct mt_router *r, uint32_t to, uint32_t id, uint8_t channel_index)
{
	uint8_t buf[4];
	int n = mt_routing_encode(0, buf, sizeof(buf));

	mt_router_send(r, to, channel_index, MT_PORT_ROUTING, buf, n, false, false, id);
}

void mt_router_receive(struct mt_router *r, const uint8_t *frame, size_t len, int16_t rssi,
		       int8_t snr, uint32_t now)
{
	static struct mt_rx rx; /* large; the router is used from one thread */
	struct mt_header h;

	r->now = now;
	if (len <= MT_HEADER_LEN || len > MT_MAX_FRAME) {
		return;
	}
	mt_header_decode(frame, &h);
	r->rx_packets++;

	if (h.from == r->my_num) {
		/* Our own packet relayed by another node: implicit ACK for broadcasts. */
		struct mt_pending *p = find_pending(r, h.id);

		if (p && p->to == MT_BROADCAST) {
			p->used = false;
			r->ops->status(r->ctx, h.id, MT_TX_ACKED);
		}
		return;
	}
	if (seen(r, h.from, h.id)) {
		bool retry = h.hop_start > 0 && h.hop_start == h.hop_limit;

		r->rx_dupes++;
		if (!retry) {
			/* Someone else relayed it first: our rebroadcast is not needed. */
			cancel_relay(r, h.from, h.id);
		} else if (h.to == r->my_num && h.want_ack) {
			/* The sender repeats it: our ACK got lost, send it again. */
			send_ack(r, h.from, h.id, 0);
		}
		return;
	}
	mark_seen(r, h.from, h.id);

	struct mt_node *node = mt_nodedb_get(r->db, h.from, now / 1000);

	node->last_heard = now / 1000;
	node->rssi = rssi;
	node->snr = snr;
	node->hops_away = h.hop_start >= h.hop_limit ? h.hop_start - h.hop_limit : 0;

	memset(&rx, 0, sizeof(rx));
	rx.h = h;
	rx.rssi = rssi;
	rx.snr = snr;
	if (decode(r, &h, frame + MT_HEADER_LEN, len - MT_HEADER_LEN, &rx)) {
		const struct mt_data *d = &rx.d;

		if (d->portnum == MT_PORT_ROUTING && d->request_id && h.to == r->my_num) {
			uint32_t err;
			struct mt_pending *p = find_pending(r, d->request_id);

			mt_routing_decode(d->payload, d->payload_len, &err);
			if (p) {
				p->used = false;
				r->ops->status(r->ctx, d->request_id, err ? MT_TX_FAILED : MT_TX_ACKED);
			}
		}
		if (h.to == r->my_num || h.to == MT_BROADCAST) {
			r->ops->deliver(r->ctx, &rx);
		}
		if (h.to == r->my_num && h.want_ack && d->portnum != MT_PORT_ROUTING) {
			send_ack(r, h.from, h.id, rx.channel_index);
		}
		/* An answer to a direct message ends the flooding of that message. */
		if ((d->request_id || d->reply_id) && h.to != r->my_num && h.to != MT_BROADCAST) {
			cancel_relay(r, h.to, d->request_id);
		}
	} else {
		r->rx_undecoded++;
	}

	if (r->rebroadcast && h.to != r->my_num && h.hop_limit > 0) {
		uint8_t relay[MT_MAX_FRAME];
		int span = SNR_MAX - SNR_MIN;
		int s = snr < SNR_MIN ? SNR_MIN : (snr > SNR_MAX ? SNR_MAX : snr);
		/* High SNR = close by = wait longer, so that distant nodes relay first. */
		int cw = CW_MIN + (s - SNR_MIN) * (CW_MAX - CW_MIN) / span;
		uint32_t delay = (2 * CW_MAX + rnd(r) % (1u << cw)) * r->radio.slot_ms;

		memcpy(relay, frame, len);
		h.hop_limit--;
		h.relay_node = r->my_num & 0xff;
		mt_header_encode(&h, relay);
		if (enqueue(r, relay, len, delay, true, h.from, h.id)) {
			r->tx_relayed++;
		}
	}
}

uint32_t mt_router_poll(struct mt_router *r, uint32_t now)
{
	uint32_t next = UINT32_MAX;

	r->now = now;
	for (int i = 0; i < MT_TXQ_LEN; i++) {
		struct mt_txq_entry *e = &r->txq[i];

		if (!e->used || !due(now, e->due)) {
			continue;
		}
		e->used = false;
		r->ops->transmit(r->ctx, e->frame, e->len);
		r->tx_packets++;
		if (e->relay) {
			continue;
		}
		struct mt_pending *p = find_pending(r, e->id);

		if (!p || !p->sent) {
			if (p) {
				p->sent = true;
			}
			r->ops->status(r->ctx, e->id, MT_TX_SENT);
		}
		if (p) {
			p->next = now + mt_router_retry_ms(r, p->len);
		}
	}

	for (int i = 0; i < MT_PENDING_LEN; i++) {
		struct mt_pending *p = &r->pending[i];

		if (!p->used || !p->sent || !due(now, p->next)) {
			continue;
		}
		if (p->retries) {
			p->retries--;
			p->next = now + mt_router_retry_ms(r, p->len);
			enqueue(r, p->frame, p->len, 0, false, r->my_num, p->id);
		} else {
			p->used = false;
			r->ops->status(r->ctx, p->id,
				       p->to == MT_BROADCAST ? MT_TX_NO_ACK : MT_TX_FAILED);
		}
	}

	for (int i = 0; i < MT_TXQ_LEN; i++) {
		if (r->txq[i].used) {
			uint32_t d = due(now, r->txq[i].due) ? 0 : r->txq[i].due - now;

			next = d < next ? d : next;
		}
	}
	for (int i = 0; i < MT_PENDING_LEN; i++) {
		if (r->pending[i].used && r->pending[i].sent) {
			uint32_t d = due(now, r->pending[i].next) ? 0 : r->pending[i].next - now;

			next = d < next ? d : next;
		}
	}
	return next;
}
