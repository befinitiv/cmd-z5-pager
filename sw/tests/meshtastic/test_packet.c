/* Packet format, protobuf and channel tests. */

#include <string.h>

#include "meshtastic/channel.h"
#include "meshtastic/packet.h"
#include "test.h"

void test_packet(void)
{
	/* Header round trip and flag layout. */
	struct mt_header h = {.to = MT_BROADCAST, .from = 0x11223344, .id = 0xaabbccdd,
			      .hop_limit = 2, .hop_start = 3, .want_ack = true, .channel = 8,
			      .next_hop = 0, .relay_node = 0x44};
	struct mt_header h2;
	uint8_t raw[16];

	mt_header_encode(&h, raw);
	CHECK(raw[0] == 0xff && raw[4] == 0x44 && raw[7] == 0x11, "header byte order");
	CHECK(raw[12] == (2 | 0x08 | (3 << 5)), "flags 0x%02x", raw[12]);
	mt_header_decode(raw, &h2);
	CHECK(memcmp(&h, &h2, sizeof(h)) == 0, "header round trip");

	/* Data: text message as the Meshtastic firmware encodes it. */
	struct mt_data d = {.portnum = MT_PORT_TEXT, .payload = (const uint8_t *)"Hi",
			    .payload_len = 2};
	uint8_t buf[128];
	int n = mt_data_encode(&d, buf, sizeof(buf));
	static const uint8_t expect[] = {0x08, 0x01, 0x12, 0x02, 'H', 'i', 0x48, 0x00};

	CHECK(n == sizeof(expect) && memcmp(buf, expect, n) == 0, "Data encoding");
	struct mt_data d2;

	CHECK(mt_data_decode(buf, n, &d2) == 0 && d2.portnum == 1 && d2.payload_len == 2 &&
		      memcmp(d2.payload, "Hi", 2) == 0,
	      "Data decoding");
	/* Random bytes are rejected (no port). */
	static const uint8_t junk[] = {0x00, 0x13, 0x77};

	CHECK(mt_data_decode(junk, sizeof(junk), &d2) != 0, "junk accepted");

	/* User with public key. */
	struct mt_user u = {.id = "!11223344", .long_name = "CMD-Z5 3344", .short_name = "3344",
			    .hw_model = MT_HW_PRIVATE, .has_public_key = true};
	struct mt_user u2;

	memset(u.public_key, 0x5a, 32);
	n = mt_user_encode(&u, buf, sizeof(buf));
	CHECK(n > 0 && mt_user_decode(buf, n, &u2) == 0, "User codec");
	CHECK(strcmp(u2.long_name, u.long_name) == 0 && u2.has_public_key &&
		      u2.public_key[31] == 0x5a && u2.hw_model == MT_HW_PRIVATE,
	      "User round trip");

	/* Position, negative coordinates and altitude. */
	struct mt_position p = {.has_latlon = true, .latitude_i = -337000000,
				.longitude_i = 1512000000, .has_altitude = true, .altitude = -12,
				.time = 1760000000};
	struct mt_position p2;

	n = mt_position_encode(&p, buf, sizeof(buf));
	CHECK(mt_position_decode(buf, n, &p2) == 0 && p2.latitude_i == p.latitude_i &&
		      p2.longitude_i == p.longitude_i && p2.altitude == -12 && p2.time == p.time,
	      "Position round trip");

	/* Routing ACK is "error_reason = 0", written explicitly. */
	n = mt_routing_encode(0, buf, sizeof(buf));
	CHECK(n == 2 && buf[0] == 0x18 && buf[1] == 0x00, "Routing ACK encoding");

	/* Channels: default LongFast hash is 8 (seen on real meshes). */
	struct mt_channel c;
	static const uint8_t idx1 = 1, idx0 = 0;

	mt_channel_init(&c, "", MT_PRESET_LONG_FAST, &idx1, 1);
	CHECK(c.hash == 8 && c.key_len == 16 && c.key[0] == 0xd4, "LongFast hash %d", c.hash);
	mt_channel_init(&c, "", MT_PRESET_LONG_FAST, &idx0, 1);
	CHECK(c.key_len == 0, "psk 0 must disable encryption");

	/* Frequencies: EU_868 has one LongFast slot, US LongFast is 906.875 MHz. */
	struct mt_radio_params rp;

	CHECK(mt_radio_params(MT_REGION_EU_868, MT_PRESET_LONG_FAST, "LongFast", &rp) == 0, "EU");
	CHECK(rp.freq_hz == 869525000 && rp.sf == 11 && rp.bw_hz == 250000 && rp.cr == 5 &&
		      rp.sync_word == 0x2b,
	      "EU_868 LongFast %u", rp.freq_hz);
	mt_radio_params(MT_REGION_US, MT_PRESET_LONG_FAST, "LongFast", &rp);
	CHECK(rp.freq_hz == 906875000, "US LongFast %u", rp.freq_hz);

	/* Airtime of a 50-byte LongFast frame: about 0.5 s. */
	mt_radio_params(MT_REGION_EU_868, MT_PRESET_LONG_FAST, "LongFast", &rp);
	uint32_t t = mt_airtime_ms(&rp, 50);

	CHECK(t > 400 && t < 700, "airtime %u ms", t);
}
