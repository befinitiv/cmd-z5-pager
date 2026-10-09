/*
 * Meshtastic over-the-air packet format: the 16-byte header and the few
 * protobuf messages the device uses (Data, User, Position, Routing).
 * Field numbers follow meshtastic/protobufs; the encoder is written by hand
 * to keep the firmware small and free of generated code.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT_BROADCAST    0xffffffffu
#define MT_HEADER_LEN   16
#define MT_MAX_FRAME    255
#define MT_MAX_PAYLOAD  (MT_MAX_FRAME - MT_HEADER_LEN)
#define MT_HOP_LIMIT    3
#define MT_HW_PRIVATE   255 /* HardwareModel PRIVATE_HW */

enum mt_port {
	MT_PORT_TEXT = 1,
	MT_PORT_POSITION = 3,
	MT_PORT_NODEINFO = 4,
	MT_PORT_ROUTING = 5,
	MT_PORT_TELEMETRY = 67,
	MT_PORT_TRACEROUTE = 70,
};

struct mt_header {
	uint32_t to, from, id;
	uint8_t hop_limit;
	uint8_t hop_start;
	bool want_ack;
	bool via_mqtt;
	uint8_t channel; /* channel hash, 0 for direct messages with PKI */
	uint8_t next_hop;
	uint8_t relay_node;
};

void mt_header_encode(const struct mt_header *h, uint8_t out[MT_HEADER_LEN]);
void mt_header_decode(const uint8_t in[MT_HEADER_LEN], struct mt_header *h);

/* Decoded Data message; payload points into the decoded buffer. */
struct mt_data {
	uint32_t portnum;
	const uint8_t *payload;
	size_t payload_len;
	bool want_response;
	uint32_t dest, source, request_id, reply_id, emoji;
};

/* Returns the encoded length, or -1 if it does not fit. */
int mt_data_encode(const struct mt_data *d, uint8_t *buf, size_t size);
/* Returns 0, or -1 for malformed data (also: random bytes from a wrong key). */
int mt_data_decode(const uint8_t *buf, size_t len, struct mt_data *d);

struct mt_user {
	char id[16];         /* "!12345678" */
	char long_name[40];
	char short_name[5];
	uint32_t hw_model;
	uint32_t role;
	bool has_public_key;
	uint8_t public_key[32];
};

int mt_user_encode(const struct mt_user *u, uint8_t *buf, size_t size);
int mt_user_decode(const uint8_t *buf, size_t len, struct mt_user *u);

struct mt_position {
	bool has_latlon;
	int32_t latitude_i;  /* degrees * 1e7 */
	int32_t longitude_i;
	bool has_altitude;
	int32_t altitude;    /* m */
	uint32_t time;       /* unix time */
	uint32_t precision_bits;
};

int mt_position_encode(const struct mt_position *p, uint8_t *buf, size_t size);
int mt_position_decode(const uint8_t *buf, size_t len, struct mt_position *p);

/* Routing message: error_reason (0 = none, i.e. an ACK). */
int mt_routing_encode(uint32_t error_reason, uint8_t *buf, size_t size);
int mt_routing_decode(const uint8_t *buf, size_t len, uint32_t *error_reason);

#ifdef __cplusplus
}
#endif
