/* MeshCore packet format, hashes, transport codes and channel keys. */

#include <string.h>

#include "meshcore/crypto.h"
#include "meshcore/packet.h"
#include "meshtastic/crypto.h"

const uint8_t mc_public_secret[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
				       0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};

bool mc_path_valid(uint8_t path_len)
{
	return mc_path_hash_size(path_len) != 4 && mc_path_bytes(path_len) <= MC_MAX_PATH;
}

int mc_packet_decode(struct mc_packet *p, const uint8_t *frame, size_t len)
{
	size_t i = 0;
	int n;

	if (len < 2 || len > MC_MAX_FRAME) {
		return -1;
	}
	p->header = frame[i++];
	p->transport[0] = p->transport[1] = 0;
	if (mc_has_transport(p)) {
		if (len < 6) {
			return -1;
		}
		p->transport[0] = frame[1] | frame[2] << 8;
		p->transport[1] = frame[3] | frame[4] << 8;
		i += 4;
	}
	p->path_len = frame[i++];
	if (!mc_path_valid(p->path_len)) {
		return -1;
	}
	n = mc_path_bytes(p->path_len);
	if (i + n >= len) {
		return -1; /* no payload */
	}
	memcpy(p->path, frame + i, n);
	i += n;
	if (len - i > MC_MAX_PAYLOAD) {
		return -1;
	}
	p->payload_len = len - i;
	memcpy(p->payload, frame + i, p->payload_len);
	return 0;
}

int mc_packet_encode(const struct mc_packet *p, uint8_t *frame)
{
	int i = 0, n = mc_path_bytes(p->path_len);

	frame[i++] = p->header;
	if (mc_has_transport(p)) {
		frame[i++] = p->transport[0] & 0xff;
		frame[i++] = p->transport[0] >> 8;
		frame[i++] = p->transport[1] & 0xff;
		frame[i++] = p->transport[1] >> 8;
	}
	frame[i++] = p->path_len;
	memcpy(frame + i, p->path, n);
	i += n;
	memcpy(frame + i, p->payload, p->payload_len);
	return i + p->payload_len;
}

void mc_packet_hash(const struct mc_packet *p, uint8_t hash[MC_HASH_SIZE])
{
	struct mt_sha256 s;
	uint8_t t = mc_type(p), out[32];

	mt_sha256_init(&s);
	mt_sha256_update(&s, &t, 1);
	if (t == MC_PT_TRACE) {
		/* As MeshCore: a uint16 path length (little-endian). */
		uint8_t pl[2] = {p->path_len, 0};

		mt_sha256_update(&s, pl, 2);
	}
	mt_sha256_update(&s, p->payload, p->payload_len);
	mt_sha256_final(&s, out);
	memcpy(hash, out, MC_HASH_SIZE);
}

void mc_scope_key(const char *region, uint8_t key[16])
{
	struct mt_sha256 s;
	uint8_t out[32];

	mt_sha256_init(&s);
	mt_sha256_update(&s, "#", 1);
	mt_sha256_update(&s, region, strlen(region));
	mt_sha256_final(&s, out);
	memcpy(key, out, 16);
}

uint16_t mc_transport_code(const uint8_t key[16], const struct mc_packet *p)
{
	uint8_t data[1 + MC_MAX_PAYLOAD], mac[32];
	uint16_t code;

	data[0] = mc_type(p);
	memcpy(data + 1, p->payload, p->payload_len);
	mc_hmac_sha256(key, 16, data, 1 + p->payload_len, mac);
	code = mac[0] | mac[1] << 8;
	/* 0000 and FFFF are reserved. */
	if (code == 0) {
		code = 1;
	} else if (code == 0xffff) {
		code = 0xfffe;
	}
	return code;
}

uint8_t mc_channel_hash(const uint8_t secret[16])
{
	uint8_t out[32];

	mt_sha256(secret, 16, out);
	return out[0];
}

void mc_hashtag_secret(const char *name, uint8_t secret[16])
{
	/* The same derivation as a region's key. */
	mc_scope_key(name[0] == '#' ? name + 1 : name, secret);
}
