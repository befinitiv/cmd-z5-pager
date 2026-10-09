/*
 * Router simulation: nodes with a range graph, a shared clock, frames
 * delivered instantly to the neighbours of the sender.
 */

#include <string.h>

#include "meshtastic/router.h"
#include "test.h"

#define NODES 4

struct sim_node {
	struct mt_router r;
	struct mt_nodedb db;
	int index;
	uint32_t rng;
	/* Last text delivered to this node and statuses seen. */
	char text[64];
	uint32_t text_from;
	bool text_pki;
	int texts;
	uint32_t status_id;
	enum mt_tx_status status;
	int statuses;
	int transmissions;
};

static struct sim_node nodes[NODES];
static bool in_range[NODES][NODES];
static uint32_t now;

static void sim_transmit(void *ctx, const uint8_t *frame, size_t len)
{
	struct sim_node *src = ctx;

	src->transmissions++;
	for (int i = 0; i < NODES; i++) {
		if (i != src->index && in_range[src->index][i]) {
			mt_router_receive(&nodes[i].r, frame, len, -80, 5, now);
		}
	}
}

static void sim_deliver(void *ctx, const struct mt_rx *rx)
{
	struct sim_node *n = ctx;

	if (rx->d.portnum == MT_PORT_TEXT) {
		size_t len = rx->d.payload_len < 63 ? rx->d.payload_len : 63;

		memcpy(n->text, rx->d.payload, len);
		n->text[len] = '\0';
		n->text_from = rx->h.from;
		n->text_pki = rx->pki;
		n->texts++;
	}
}

static void sim_status(void *ctx, uint32_t id, enum mt_tx_status st)
{
	struct sim_node *n = ctx;

	n->status_id = id;
	n->status = st;
	n->statuses++;
}

static uint32_t sim_random(void *ctx)
{
	struct sim_node *n = ctx;

	n->rng = n->rng * 1103515245 + 12345;
	return n->rng >> 8;
}

static const struct mt_router_ops ops = {
	.transmit = sim_transmit,
	.deliver = sim_deliver,
	.status = sim_status,
	.random = sim_random,
};

static void setup(void)
{
	static const uint8_t psk = 1;
	struct mt_radio_params rp;

	mt_radio_params(MT_REGION_EU_868, MT_PRESET_SHORT_FAST, "ShortFast", &rp);
	memset(in_range, 0, sizeof(in_range));
	for (int i = 0; i < NODES; i++) {
		struct sim_node *n = &nodes[i];
		uint8_t priv[32];

		memset(n, 0, sizeof(*n));
		n->index = i;
		n->rng = 1000 + i;
		mt_nodedb_init(&n->db);
		mt_router_init(&n->r, &ops, n, 0x1000 + i, &n->db);
		mt_router_set_radio(&n->r, &rp);
		mt_channel_init(&n->r.channels[0], "", MT_PRESET_SHORT_FAST, &psk, 1);
		n->r.nchannels = 1;
		for (int k = 0; k < 32; k++) {
			priv[k] = i * 37 + k * 11;
		}
		mt_router_set_keys(&n->r, priv);
	}
	now = 0;
}

static void link(int a, int b)
{
	in_range[a][b] = in_range[b][a] = true;
}

/* Every node knows every other node's public key (as after NodeInfo exchange). */
static void exchange_keys(void)
{
	for (int i = 0; i < NODES; i++) {
		for (int j = 0; j < NODES; j++) {
			if (i == j) {
				continue;
			}
			struct mt_node *n = mt_nodedb_get(&nodes[i].db, nodes[j].r.my_num, 0);
			struct mt_user u = {.has_public_key = true};

			memcpy(u.public_key, nodes[j].r.pub, 32);
			mt_nodedb_set_user(n, &u);
		}
	}
}

static void run(uint32_t ms)
{
	for (uint32_t end = now + ms; now < end; now += 10) {
		for (int i = 0; i < NODES; i++) {
			mt_router_poll(&nodes[i].r, now);
		}
	}
}

void test_router(void)
{
	/* Line A - B - C: a broadcast reaches C through B; A gets the implicit ACK. */
	setup();
	link(0, 1);
	link(1, 2);
	uint32_t id = mt_router_send(&nodes[0].r, MT_BROADCAST, 0, MT_PORT_TEXT, "hello", 5, true,
				     false, 0);

	CHECK(id != 0, "send failed");
	run(20000);
	CHECK(nodes[1].texts == 1 && strcmp(nodes[1].text, "hello") == 0, "B did not get it");
	CHECK(nodes[2].texts == 1 && nodes[2].text_from == 0x1000, "C did not get it (relay)");
	CHECK(nodes[0].status_id == id && nodes[0].status == MT_TX_ACKED,
	      "A: implicit ACK missing (status %d)", nodes[0].status);
	CHECK(nodes[0].transmissions == 1, "A retransmitted %d times", nodes[0].transmissions - 1);
	CHECK(nodes[1].r.rx_dupes >= 1, "B should hear C's relay as a duplicate");

	/* Direct message A -> C with PKI and ACK over two hops. */
	setup();
	link(0, 1);
	link(1, 2);
	exchange_keys();
	id = mt_router_send(&nodes[0].r, 0x1002, 0, MT_PORT_TEXT, "secret", 6, true, false, 0);
	run(30000);
	CHECK(nodes[2].texts == 1 && strcmp(nodes[2].text, "secret") == 0 && nodes[2].text_pki,
	      "C: PKI direct message not received");
	CHECK(nodes[1].texts == 0, "B must not deliver a DM for C");
	CHECK(nodes[0].status_id == id && nodes[0].status == MT_TX_ACKED,
	      "A: ACK for the DM missing (status %d)", nodes[0].status);

	/* Without known keys the DM falls back to channel encryption. */
	setup();
	link(0, 1);
	id = mt_router_send(&nodes[0].r, 0x1001, 0, MT_PORT_TEXT, "legacy", 6, true, false, 0);
	run(20000);
	CHECK(nodes[1].texts == 1 && !nodes[1].text_pki, "legacy DM");
	CHECK(nodes[0].status == MT_TX_ACKED, "legacy DM ACK");

	/* Unreachable destination: 3 retries, then FAILED. */
	setup();
	link(0, 1);
	exchange_keys();
	id = mt_router_send(&nodes[0].r, 0x1003, 0, MT_PORT_TEXT, "anyone?", 7, true, false, 0);
	run(120000);
	CHECK(nodes[0].status_id == id && nodes[0].status == MT_TX_FAILED,
	      "unreachable DM status %d", nodes[0].status);
	CHECK(nodes[0].transmissions == 1 + MT_RETRIES, "transmissions %d",
	      nodes[0].transmissions);

	/* A lost ACK: the receiver answers the sender's retry with another ACK. */
	setup();
	link(0, 1);
	id = mt_router_send(&nodes[0].r, 0x1001, 0, MT_PORT_TEXT, "again", 5, true, false, 0);
	in_range[1][0] = false; /* B -> A broken: B's first ACK is lost */
	run(4000);
	CHECK(nodes[1].texts == 1, "B did not get the DM");
	in_range[1][0] = true;
	run(60000);
	CHECK(nodes[1].texts == 1, "B delivered the retry twice");
	CHECK(nodes[0].status_id == id && nodes[0].status == MT_TX_ACKED,
	      "A: no ACK after the retry (status %d)", nodes[0].status);

	/* Triangle A, B, C all in range: B and C both hear A; only one needs to relay. */
	setup();
	link(0, 1);
	link(0, 2);
	link(1, 2);
	mt_router_send(&nodes[0].r, MT_BROADCAST, 0, MT_PORT_TEXT, "all", 3, false, false, 0);
	run(20000);
	CHECK(nodes[1].texts == 1 && nodes[2].texts == 1, "triangle delivery");
	CHECK(nodes[1].r.relay_cancelled + nodes[2].r.relay_cancelled == 1,
	      "exactly one relay should be cancelled (%u + %u)", nodes[1].r.relay_cancelled,
	      nodes[2].r.relay_cancelled);

	/* Other channel key: the packet is relayed but not delivered. */
	setup();
	link(0, 1);
	link(1, 2);
	static const uint8_t other = 2;

	mt_channel_init(&nodes[1].r.channels[0], "", MT_PRESET_SHORT_FAST, &other, 1);
	mt_router_send(&nodes[0].r, MT_BROADCAST, 0, MT_PORT_TEXT, "x", 1, false, false, 0);
	run(20000);
	CHECK(nodes[1].texts == 0 && nodes[2].texts == 1, "foreign channel relay");
}
