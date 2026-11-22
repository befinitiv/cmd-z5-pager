/*
 * Client simulation: A - R - B, where R is a minimal MeshCore repeater
 * (floods append its hash, direct packets follow the path) and A and B do
 * not hear each other. Frames are delivered at once; a shared clock.
 */

#include <string.h>

#include "meshcore/crypto.h"
#include "meshcore/mesh.h"
#include "test.h"

#define R_HASH 0x7e

struct sim_node {
	struct mc_mesh m;
	int index;
	uint32_t rng;
	uint8_t pub[32], prv[64];
	bool knows_peer; /* the other node's advert arrived */
	struct mc_advert advert;
	char text[200];
	uint32_t text_from;
	int texts;
	char group[200];
	int group_channel;
	int groups;
	uint32_t status_id;
	enum mc_tx_status status;
	int statuses;
	uint8_t path_len;
	uint8_t path[MC_MAX_PATH];
	int transmissions;
	uint8_t hops;
	/* The last frame sent; responses, CLI texts and traces received. */
	uint8_t last[MC_MAX_FRAME];
	size_t last_len;
	uint8_t resp[MC_MAX_PAYLOAD];
	size_t resp_len;
	uint32_t resp_from;
	bool resp_in_path;
	int responses;
	char cli[200];
	int clis;
	uint32_t trace_tag;
	uint8_t trace_hashes[16], trace_hlen;
	int8_t trace_snrs[16];
	uint8_t trace_nsnr;
	int traces;
};

static struct sim_node nodes[2]; /* A, B */
static uint32_t now;
static bool drop_to_b;    /* R loses everything for B */
static bool check_scope;  /* R checks the transport code */
static uint8_t scope[16];
static int scope_ok, scope_bad;

static struct {
	uint8_t hash[MC_HASH_SIZE];
} r_seen[64];
static int r_seen_pos;

static void deliver(int to, const uint8_t *frame, size_t len)
{
	if (to == 1 && drop_to_b) {
		return;
	}
	mc_mesh_receive(&nodes[to].m, frame, len, -90, 6, now);
}

/* The repeater: what it hears it repeats to both sides (A hears its own repeated). */
static void repeater(const uint8_t *frame, size_t len)
{
	struct mc_packet p;
	uint8_t h[MC_HASH_SIZE], out[MC_MAX_FRAME];
	int n;

	if (mc_packet_decode(&p, frame, len) != 0) {
		return;
	}
	mc_packet_hash(&p, h);
	for (int i = 0; i < 64; i++) {
		if (memcmp(r_seen[i].hash, h, MC_HASH_SIZE) == 0) {
			return;
		}
	}
	memcpy(r_seen[r_seen_pos].hash, h, MC_HASH_SIZE);
	r_seen_pos = (r_seen_pos + 1) % 64;
	if (mc_is_flood(&p)) {
		if (check_scope) {
			if (mc_route(&p) == MC_ROUTE_TRANSPORT_FLOOD &&
			    p.transport[0] == mc_transport_code(scope, &p)) {
				scope_ok++;
			} else {
				scope_bad++;
				return;
			}
		}
		p.path[mc_path_count(p.path_len)] = R_HASH;
		p.path_len++;
	} else if (mc_path_count(p.path_len) > 0 && p.path[0] == R_HASH) {
		memmove(p.path, p.path + 1, mc_path_count(p.path_len) - 1);
		p.path_len--;
	} else {
		return;
	}
	n = mc_packet_encode(&p, out);
	deliver(0, out, n);
	deliver(1, out, n);
}

static void sim_transmit(void *ctx, const uint8_t *frame, size_t len)
{
	struct sim_node *src = ctx;

	src->transmissions++;
	memcpy(src->last, frame, len);
	src->last_len = len;
	repeater(frame, len);
}

static uint32_t sim_random(void *ctx)
{
	struct sim_node *n = ctx;

	n->rng ^= n->rng << 13;
	n->rng ^= n->rng >> 17;
	n->rng ^= n->rng << 5;
	return n->rng;
}

static int sim_find_peers(void *ctx, uint8_t hash, struct mc_contact *peers, int max)
{
	struct sim_node *n = ctx;
	struct sim_node *other = &nodes[1 - n->index];

	if (!n->knows_peer || other->pub[0] != hash || max < 1) {
		return 0;
	}
	memset(&peers[0], 0, sizeof(peers[0]));
	peers[0].num = other->index + 1;
	memcpy(peers[0].pub, other->pub, 32);
	mc_shared_secret(peers[0].secret, n->prv, other->pub);
	peers[0].path_len = n->path_len;
	memcpy(peers[0].path, n->path, MC_MAX_PATH);
	return 1;
}

static void sim_advert(void *ctx, const struct mc_advert *a, const struct mc_rx_info *rx)
{
	struct sim_node *n = ctx;

	n->advert = *a;
	n->knows_peer = true;
	n->hops = rx->hops;
}

static void sim_text(void *ctx, uint32_t from, uint32_t timestamp, const char *text,
		     const struct mc_rx_info *rx)
{
	struct sim_node *n = ctx;

	strcpy(n->text, text);
	n->text_from = from;
	n->texts++;
	n->hops = rx->hops;
}

static void sim_group(void *ctx, int channel, uint32_t timestamp, const char *text,
		      const struct mc_rx_info *rx)
{
	struct sim_node *n = ctx;

	strcpy(n->group, text);
	n->group_channel = channel;
	n->groups++;
}

static void sim_path(void *ctx, uint32_t num, const uint8_t *path, uint8_t path_len)
{
	struct sim_node *n = ctx;

	n->path_len = path_len;
	if (path) {
		memcpy(n->path, path, mc_path_bytes(path_len));
	}
}

static void sim_status(void *ctx, uint32_t id, enum mc_tx_status st)
{
	struct sim_node *n = ctx;

	n->status_id = id;
	n->status = st;
	n->statuses++;
}

static void sim_response(void *ctx, uint32_t from, const uint8_t *data, size_t len,
			 const uint8_t *in_path, uint8_t in_path_len)
{
	struct sim_node *n = ctx;

	memcpy(n->resp, data, len);
	n->resp_len = len;
	n->resp_from = from;
	n->resp_in_path = in_path != NULL;
	n->responses++;
}

static void sim_cli(void *ctx, uint32_t from, uint32_t timestamp, const char *text,
		    const struct mc_rx_info *rx)
{
	struct sim_node *n = ctx;

	strcpy(n->cli, text);
	n->clis++;
}

static void sim_trace(void *ctx, uint32_t tag, uint32_t auth, uint8_t flags, const uint8_t *hashes,
		      uint8_t hlen, const int8_t *snrs, uint8_t nsnr, const struct mc_rx_info *rx)
{
	struct sim_node *n = ctx;

	n->trace_tag = tag;
	memcpy(n->trace_hashes, hashes, hlen);
	n->trace_hlen = hlen;
	memcpy(n->trace_snrs, snrs, nsnr);
	n->trace_nsnr = nsnr;
	n->traces++;
}

static const struct mc_mesh_ops ops = {
	.transmit = sim_transmit,
	.random = sim_random,
	.find_peers = sim_find_peers,
	.advert = sim_advert,
	.text = sim_text,
	.group_text = sim_group,
	.path = sim_path,
	.status = sim_status,
	.response = sim_response,
	.cli_text = sim_cli,
	.trace = sim_trace,
};

/* Run both nodes until nothing is due, or for ms. */
static void run(uint32_t ms)
{
	uint32_t end = now + ms;

	while ((int32_t)(end - now) > 0) {
		uint32_t w0 = mc_mesh_poll(&nodes[0].m, now);
		uint32_t w1 = mc_mesh_poll(&nodes[1].m, now);
		uint32_t w = w0 < w1 ? w0 : w1;

		if (w == 0) {
			continue;
		}
		now += w < end - now ? w : end - now;
	}
	mc_mesh_poll(&nodes[0].m, now);
	mc_mesh_poll(&nodes[1].m, now);
}

static void setup(void)
{
	memset(nodes, 0, sizeof(nodes));
	memset(r_seen, 0, sizeof(r_seen));
	now = 1000;
	for (int i = 0; i < 2; i++) {
		struct sim_node *n = &nodes[i];
		uint8_t seed[32];

		n->index = i;
		n->rng = 0x1234567 + i;
		n->path_len = MC_PATH_UNKNOWN;
		for (int k = 0; k < 32; k++) {
			seed[k] = 17 * i + k;
		}
		mc_ed25519_keypair(n->pub, n->prv, seed);
		mc_mesh_init(&n->m, &ops, n, n->pub, n->prv);
		mc_mesh_set_channel(&n->m, 0, mc_public_secret);
	}
}

static void advert(int i, const char *name)
{
	struct mc_advert a = {.type = MC_ADV_CHAT, .timestamp = 1780000000 + now / 1000};

	memcpy(a.pub, nodes[i].pub, 32);
	strcpy(a.name, name);
	mc_mesh_send_advert(&nodes[i].m, &a, true);
	run(10);
}

/* A datagram from one node to the other, as a repeater or room server sends it. */
static size_t datagram_frame(uint8_t type, int from, int to, const uint8_t *data, size_t len,
			     uint8_t route, const uint8_t *path, uint8_t path_len, uint8_t *frame)
{
	struct mc_packet p = {.header = type << 2 | route, .path_len = path_len};
	uint8_t secret[32];

	mc_shared_secret(secret, nodes[from].prv, nodes[to].pub);
	if (path_len) {
		memcpy(p.path, path, path_len);
	}
	p.payload[0] = nodes[to].pub[0];
	p.payload[1] = nodes[from].pub[0];
	p.payload_len = 2 + mc_encrypt_then_mac(secret, p.payload + 2, data, len);
	return mc_packet_encode(&p, frame);
}

/* What a companion app needs: logins, requests, CLI texts, traces (B plays the server). */
static void test_requests(void)
{
	struct sim_node *a = &nodes[0], *b = &nodes[1];
	uint8_t data[64], plain[MC_MAX_PAYLOAD], secret[32], frame[MC_MAX_FRAME];
	const uint8_t r_path[1] = {R_HASH};
	struct mc_packet p;
	struct mc_contact to;
	uint32_t timeout;
	bool flood;
	size_t n;
	int len, sent;

	sim_find_peers(a, b->pub[0], &to, 1);
	mc_shared_secret(secret, b->prv, a->pub);

	/* Login: ANON_REQ with our key in the clear, the data encrypted. */
	memcpy(data, "\x10\x20\x30\x40secret", 10);
	timeout = mc_mesh_send_anon(&a->m, &to, data, 10, &flood);
	run(10);
	CHECK(timeout > 0, "login queued");
	CHECK(mc_packet_decode(&p, a->last, a->last_len) == 0 && mc_type(&p) == MC_PT_ANON_REQ,
	      "login is an ANON_REQ");
	CHECK(p.payload[0] == b->pub[0] && memcmp(p.payload + 1, a->pub, 32) == 0,
	      "login: B's hash and A's key");
	len = mc_mac_then_decrypt(secret, plain, p.payload + 33, p.payload_len - 33);
	CHECK(len >= 10 && memcmp(plain, data, 10) == 0, "login: B decrypts it");

	/* The answer as a datagram. */
	memcpy(data, "\x99\x88\x77\x66\x00\x01\x03", 7);
	n = datagram_frame(MC_PT_RESPONSE, 1, 0, data, 7, MC_ROUTE_DIRECT, NULL, 0, frame);
	mc_mesh_receive(&a->m, frame, n, -90, 6, now);
	CHECK(a->responses == 1 && a->resp_from == 2 && !a->resp_in_path, "response datagram");
	CHECK(a->resp_len >= 7 && memcmp(a->resp, data, 7) == 0, "response data");

	/* The answer to a flooded request: in a PATH, with the path back. */
	plain[0] = 1;
	plain[1] = R_HASH;
	plain[2] = MC_PT_RESPONSE;
	memcpy(plain + 3, data, 7);
	n = datagram_frame(MC_PT_PATH, 1, 0, plain, 10, MC_ROUTE_FLOOD, r_path, 1, frame);
	mc_mesh_receive(&a->m, frame, n, -90, 6, now);
	CHECK(a->responses == 2 && a->resp_in_path && memcmp(a->resp, data, 7) == 0,
	      "response in a PATH");
	CHECK(a->path_len == 1 && a->path[0] == R_HASH, "PATH gave the route");
	run(2000);

	/* A request (status): REQ, B decrypts it. */
	sim_find_peers(a, b->pub[0], &to, 1);
	memcpy(data, "\x01\x02\x03\x04\x01\x00\x00\x00\x00\xaa\xbb\xcc\xdd", 13);
	timeout = mc_mesh_send_request(&a->m, &to, data, 13, false, &flood);
	run(10);
	CHECK(timeout > 0 && !flood, "request along the route");
	CHECK(mc_packet_decode(&p, a->last, a->last_len) == 0 && mc_type(&p) == MC_PT_REQ &&
	      mc_route(&p) == MC_ROUTE_DIRECT, "REQ, direct");
	len = mc_mac_then_decrypt(secret, plain, p.payload + 2, p.payload_len - 2);
	CHECK(len >= 13 && memcmp(plain, data, 13) == 0, "request decrypted");
	mc_mesh_send_request(&a->m, &to, data, 13, true, &flood);
	run(10);
	CHECK(flood, "request forced to flood");

	/* A command reply (CLI_DATA): delivered, not acknowledged. */
	sim_find_peers(b, a->pub[0], &to, 1);
	sent = a->transmissions;
	timeout = mc_mesh_send_cli(&b->m, &to, 1780001000, 0, "> neighbors: 8a", &flood);
	run(2000);
	CHECK(timeout > 0 && a->clis == 1 && strcmp(a->cli, "> neighbors: 8a") == 0, "CLI text");
	CHECK(a->transmissions == sent, "CLI text not acknowledged");

	/* Trace: hashes in the payload, the path empty; ours back with one SNR. */
	{
		const uint8_t hashes[2] = {R_HASH, 0x45};

		timeout = mc_mesh_send_trace(&a->m, 0xabcd0001, 7, 0, hashes, 2);
		run(10);
		CHECK(timeout > 0, "trace queued");
		CHECK(mc_packet_decode(&p, a->last, a->last_len) == 0 && mc_type(&p) == MC_PT_TRACE &&
		      mc_route(&p) == MC_ROUTE_DIRECT && p.path_len == 0 && p.payload_len == 11 &&
		      memcmp(p.payload + 9, hashes, 2) == 0, "trace packet");
		/* After the first hop: not at the end yet. */
		p.path[0] = 24;
		p.path_len = 1;
		n = mc_packet_encode(&p, frame);
		mc_mesh_receive(&a->m, frame, n, -90, 6, now);
		CHECK(a->traces == 0, "trace not yet at its end");
		p.path[1] = (uint8_t)-8;
		p.path_len = 2;
		n = mc_packet_encode(&p, frame);
		mc_mesh_receive(&a->m, frame, n, -90, 6, now);
		CHECK(a->traces == 1 && a->trace_tag == 0xabcd0001 && a->trace_hlen == 2 &&
		      a->trace_nsnr == 2 && a->trace_snrs[0] == 24 && a->trace_snrs[1] == -8,
		      "trace result");
	}
}

void test_mesh(void)
{
	struct sim_node *a = &nodes[0], *b = &nodes[1];
	struct mc_contact to;
	uint32_t id;

	setup();
	advert(0, "Alice");
	advert(1, "Bob");
	CHECK(b->knows_peer && strcmp(b->advert.name, "Alice") == 0, "B got A's advert");
	CHECK(b->hops == 1, "advert over one hop: %u", b->hops);
	CHECK(a->knows_peer && strcmp(a->advert.name, "Bob") == 0, "A got B's advert");

	/* First DM: flooded; B answers with a PATH carrying the ACK. */
	sim_find_peers(a, b->pub[0], &to, 1);
	id = mc_mesh_send_text(&a->m, &to, 1780000100, "Hallo Bob");
	CHECK(id != 0, "send");
	run(5000);
	CHECK(b->texts == 1 && strcmp(b->text, "Hallo Bob") == 0 && b->text_from == 1,
	      "B got the DM");
	CHECK(b->hops == 1, "DM over one hop");
	CHECK(a->status_id == id && a->status == MC_TX_ACKED, "A got the ACK (%d)", a->status);
	CHECK(a->path_len == 1 && a->path[0] == R_HASH, "A learned the path to B");
	/* A's reciprocal PATH (directly via R) gives B its way back. */
	CHECK(b->path_len == 1 && b->path[0] == R_HASH, "B learned the path to A");

	/* Second DM goes direct along the path; B's ACK comes back direct too. */
	a->transmissions = 0;
	sim_find_peers(a, b->pub[0], &to, 1);
	CHECK(to.path_len == 1, "contact has the path");
	id = mc_mesh_send_text(&a->m, &to, 1780000200, "Zweite");
	run(5000);
	CHECK(b->texts == 2 && strcmp(b->text, "Zweite") == 0, "B got the direct DM");
	CHECK(a->status_id == id && a->status == MC_TX_ACKED, "direct DM acknowledged");
	CHECK(a->transmissions == 1, "one transmission (%d)", a->transmissions);

	/* B replies; the long text is cut to 160 bytes. */
	{
		char longtext[200];

		memset(longtext, 'x', 199);
		longtext[199] = '\0';
		sim_find_peers(b, a->pub[0], &to, 1);
		id = mc_mesh_send_text(&b->m, &to, 1780000300, longtext);
		run(5000);
		CHECK(a->texts == 1 && strlen(a->text) == MC_MAX_TEXT, "A got 160 bytes");
		CHECK(b->status == MC_TX_ACKED, "B's DM acknowledged");
	}

	/* Channel message: B decodes it; A hears R repeat it: delivered. */
	id = mc_mesh_send_group(&a->m, 0, 1780000400, "Alice", "Moin");
	run(100);
	CHECK(b->groups == 1 && strcmp(b->group, "Alice: Moin") == 0 && b->group_channel == 0,
	      "B got the channel message '%s'", b->group);
	CHECK(a->status_id == id && a->status == MC_TX_ACKED, "A heard it repeated");

	/* A lost path: two direct attempts, then a flood (which works) resets it. */
	a->path[0] = 0x55; /* no such repeater */
	sim_find_peers(a, b->pub[0], &to, 1);
	a->transmissions = 0;
	id = mc_mesh_send_text(&a->m, &to, 1780000500, "Pfad?");
	run(60000);
	CHECK(a->transmissions >= 3, "retried (%d transmissions)", a->transmissions);
	CHECK(a->status_id == id && a->status == MC_TX_ACKED, "acknowledged after the flood");
	CHECK(a->path_len == 1 && a->path[0] == R_HASH, "path learned again");
	CHECK(b->texts == 3, "B shows it once (%d)", b->texts);

	/* Nobody receives: failed after the attempts. */
	drop_to_b = true;
	sim_find_peers(a, b->pub[0], &to, 1);
	a->statuses = 0;
	id = mc_mesh_send_text(&a->m, &to, 1780000600, "Weg");
	run(120000);
	CHECK(a->status_id == id && a->status == MC_TX_FAILED, "failed (%d)", a->status);
	drop_to_b = false;

	/* Scoped floods: the repeater gets the region's code. */
	mc_scope_key("de-sh", scope);
	mc_mesh_set_scope(&a->m, scope);
	check_scope = true;
	id = mc_mesh_send_group(&a->m, 0, 1780000700, "Alice", "scoped");
	run(100);
	CHECK(scope_ok == 1 && scope_bad == 0, "scoped flood (%d ok, %d bad)", scope_ok,
	      scope_bad);
	CHECK(strcmp(b->group, "Alice: scoped") == 0, "B got the scoped message");
	/* Unscoped: a regional repeater drops it, nobody repeats: no ACK. */
	mc_mesh_set_scope(&a->m, NULL);
	id = mc_mesh_send_group(&a->m, 0, 1780000800, "Alice", "unscoped");
	run(20000);
	CHECK(scope_bad == 1 && a->status_id == id && a->status == MC_TX_NO_ACK,
	      "unscoped not repeated");
	check_scope = false;

	/* Hashtag channel on one side only: not delivered. */
	{
		uint8_t secret[16];

		mc_hashtag_secret("#ping", secret);
		mc_mesh_set_channel(&a->m, 1, secret);
		b->groups = 0;
		mc_mesh_send_group(&a->m, 1, 1780000900, "Alice", "ping");
		run(100);
		CHECK(b->groups == 0, "unknown channel ignored");
		mc_mesh_set_channel(&b->m, 2, secret);
		mc_mesh_send_group(&a->m, 1, 1780000901, "Alice", "ping");
		run(100);
		CHECK(b->groups == 1 && b->group_channel == 2, "#ping on B's slot 2");
	}

	test_requests();
}
