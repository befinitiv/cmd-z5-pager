/*
 * MeshCore packets (protocol of MeshCore 1.x, payload version 1).
 *
 * On the wire: header byte (bits 0-1 route type, 2-5 payload type, 6-7
 * version), two 16-bit transport codes (little-endian) for the transport
 * route types, the path length byte (bits 0-5 the number of hashes, bits 6-7
 * the hash size - 1), the path (one hash per repeater: the first bytes of
 * its public key) and the payload.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_MAX_FRAME   255
#define MC_MAX_PAYLOAD 184
#define MC_MAX_PATH    64
#define MC_HASH_SIZE   8   /* packet hash, for duplicate detection */
#define MC_PATH_UNKNOWN 0xff /* no route to a contact: flood */

enum mc_route {
	MC_ROUTE_TRANSPORT_FLOOD = 0,
	MC_ROUTE_FLOOD = 1,
	MC_ROUTE_DIRECT = 2,
	MC_ROUTE_TRANSPORT_DIRECT = 3,
};

enum mc_payload_type {
	MC_PT_REQ = 0,
	MC_PT_RESPONSE = 1,
	MC_PT_TXT_MSG = 2,
	MC_PT_ACK = 3,
	MC_PT_ADVERT = 4,
	MC_PT_GRP_TXT = 5,
	MC_PT_GRP_DATA = 6,
	MC_PT_ANON_REQ = 7,
	MC_PT_PATH = 8,
	MC_PT_TRACE = 9,
	MC_PT_MULTIPART = 10,
	MC_PT_CONTROL = 11,
	MC_PT_RAW_CUSTOM = 15,
};

struct mc_packet {
	uint8_t header;
	uint16_t transport[2];
	uint8_t path_len; /* as on the wire */
	uint8_t path[MC_MAX_PATH];
	uint8_t payload_len;
	uint8_t payload[MC_MAX_PAYLOAD];
};

static inline uint8_t mc_route(const struct mc_packet *p)
{
	return p->header & 0x03;
}

static inline uint8_t mc_type(const struct mc_packet *p)
{
	return (p->header >> 2) & 0x0f;
}

static inline bool mc_is_flood(const struct mc_packet *p)
{
	return mc_route(p) == MC_ROUTE_FLOOD || mc_route(p) == MC_ROUTE_TRANSPORT_FLOOD;
}

static inline bool mc_has_transport(const struct mc_packet *p)
{
	return mc_route(p) == MC_ROUTE_TRANSPORT_FLOOD || mc_route(p) == MC_ROUTE_TRANSPORT_DIRECT;
}

static inline uint8_t mc_path_count(uint8_t path_len)
{
	return path_len & 63;
}

static inline uint8_t mc_path_hash_size(uint8_t path_len)
{
	return (path_len >> 6) + 1;
}

static inline int mc_path_bytes(uint8_t path_len)
{
	return mc_path_count(path_len) * mc_path_hash_size(path_len);
}

/* False for reserved hash sizes or paths longer than MC_MAX_PATH. */
bool mc_path_valid(uint8_t path_len);

/* Returns 0, or -1 for a malformed frame. */
int mc_packet_decode(struct mc_packet *p, const uint8_t *frame, size_t len);
/* Returns the frame length (at most MC_MAX_FRAME). */
int mc_packet_encode(const struct mc_packet *p, uint8_t *frame);

/* SHA-256 of the payload type and payload, truncated: the same packet on every hop. */
void mc_packet_hash(const struct mc_packet *p, uint8_t hash[MC_HASH_SIZE]);

/*
 * Region scope ("flood scope"): repeaters only flood packets whose first
 * transport code matches a region they serve. The key of a public region
 * "de-sh" is SHA-256 of "#de-sh", truncated to 16 bytes.
 */
void mc_scope_key(const char *region, uint8_t key[16]);
uint16_t mc_transport_code(const uint8_t key[16], const struct mc_packet *p);

/* Group channels: the hash byte is SHA-256 of the 16-byte secret. */
uint8_t mc_channel_hash(const uint8_t secret[16]);
/* Secret of a public "#name" channel: SHA-256 of "#name", truncated. */
void mc_hashtag_secret(const char *name, uint8_t secret[16]);
/* The "Public" channel every MeshCore node has. */
extern const uint8_t mc_public_secret[16];

/* Bytes of s that fit in max, cut at a character boundary (UTF-8). */
size_t mc_utf8_prefix(const char *s, size_t max);

/* Adverts ------------------------------------------------------------------------- */

enum mc_adv_type {
	MC_ADV_NONE = 0,
	MC_ADV_CHAT = 1, /* a companion (person) */
	MC_ADV_REPEATER = 2,
	MC_ADV_ROOM = 3, /* room server */
	MC_ADV_SENSOR = 4,
};

#define MC_ADVERT_DATA_MAX 32
#define MC_NAME_MAX        31

struct mc_advert {
	uint8_t pub[32];
	uint32_t timestamp; /* sender's clock, rising */
	uint8_t type;       /* enum mc_adv_type */
	bool has_latlon;
	int32_t lat_e6, lon_e6;
	char name[MC_NAME_MAX + 1];
};

/*
 * Advert payload, signed with our key (a->pub must be ours). The name is
 * cut to what fits, at a character boundary.
 */
void mc_advert_encode(struct mc_packet *p, const struct mc_advert *a, const uint8_t prv[64]);
/* Returns 0, -1 if malformed, -2 if the signature is wrong. */
int mc_advert_decode(const struct mc_packet *p, struct mc_advert *a);

#ifdef __cplusplus
}
#endif
