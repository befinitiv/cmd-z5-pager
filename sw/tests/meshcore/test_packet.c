/* Packet format, keys, transport codes and adverts. */

#include <string.h>

#include "meshcore/crypto.h"
#include "meshcore/packet.h"
#include "test.h"
#include "vectors.h"

static void hex(const char *s, uint8_t *out)
{
	for (; s[0] && s[1]; s += 2) {
		unsigned v;

		sscanf(s, "%2x", &v);
		*out++ = v;
	}
}

void test_packet(void)
{
	struct mc_packet p = {0}, q;
	uint8_t frame[MC_MAX_FRAME], key[16], exp[32];
	int n;

	CHECK(mc_channel_hash(mc_public_secret) == PUBLIC_HASH, "Public channel hash");
	mc_scope_key("de-sh", key);
	hex(SCOPE_DE_SH, exp);
	CHECK_MEM(key, exp, 16, "scope key");
	mc_hashtag_secret("#ping", key);
	hex(PING_SECRET, exp);
	CHECK_MEM(key, exp, 16, "#ping secret");
	mc_hashtag_secret("ping", key);
	CHECK_MEM(key, exp, 16, "ping secret without #");

	/* A scoped flood with two path hashes. */
	p.header = MC_PT_GRP_TXT << 2 | MC_ROUTE_TRANSPORT_FLOOD;
	p.payload_len = 40;
	for (int i = 0; i < 40; i++) {
		p.payload[i] = i;
	}
	mc_scope_key("de-sh", key);
	p.transport[0] = mc_transport_code(key, &p);
	CHECK(p.transport[0] == CODE_DE_SH, "transport code %04x", p.transport[0]);
	p.path_len = 2;
	p.path[0] = 0x8a;
	p.path[1] = 0x45;
	n = mc_packet_encode(&p, frame);
	CHECK(n == 1 + 4 + 1 + 2 + 40, "frame length %d", n);
	CHECK(frame[1] == (CODE_DE_SH & 0xff) && frame[2] == CODE_DE_SH >> 8, "code bytes");
	CHECK(mc_packet_decode(&q, frame, n) == 0, "decode");
	CHECK(q.header == p.header && q.transport[0] == p.transport[0] && q.path_len == 2 &&
		      q.path[1] == 0x45 && q.payload_len == 40 && q.payload[39] == 39,
	      "decoded fields");
	/* Two-byte hashes: path_len 0x42 = 2 hashes of 2 bytes. */
	CHECK(mc_path_bytes(0x42) == 4 && mc_path_valid(0x42), "2-byte hashes");
	CHECK(!mc_path_valid(0xc1), "reserved hash size");
	CHECK(mc_packet_decode(&q, frame, 7) != 0, "truncated frame");

	/* Adverts: round trip with position, and a forged one. */
	{
		struct mc_advert a = {.type = MC_ADV_CHAT, .timestamp = 1780000000,
				      .has_latlon = true, .lat_e6 = 53812345, .lon_e6 = 10512345};
		struct mc_advert b;
		uint8_t seed[32], prv[64];

		hex(SEED_A, seed);
		mc_ed25519_keypair(a.pub, prv, seed);
		/* Too long for the 23 bytes left with a position: cut before the "ö". */
		strcpy(a.name, "CMD-Z5 Walksfelde Nordö");
		mc_advert_encode(&p, &a, prv);
		CHECK(mc_type(&p) == MC_PT_ADVERT, "advert type");
		CHECK(p.payload_len == 100 + 1 + 8 + 22, "advert length %d", p.payload_len);
		CHECK(mc_advert_decode(&p, &b) == 0, "advert decode");
		CHECK(b.type == MC_ADV_CHAT && b.has_latlon && b.lat_e6 == 53812345 &&
			      b.lon_e6 == 10512345 && b.timestamp == 1780000000,
		      "advert fields");
		CHECK(strcmp(b.name, "CMD-Z5 Walksfelde Nord") == 0, "advert name '%s'", b.name);
		p.payload[p.payload_len - 1] ^= 1;
		CHECK(mc_advert_decode(&p, &b) == -2, "forged advert");
	}
}
