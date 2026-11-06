/* MeshCore adverts: a node's public key, time, type, position and name, signed. */

#include <string.h>

#include "meshcore/crypto.h"
#include "meshcore/packet.h"

#define FLAG_LATLON 0x10
#define FLAG_FEAT1  0x20
#define FLAG_FEAT2  0x40
#define FLAG_NAME   0x80

size_t mc_utf8_prefix(const char *s, size_t max)
{
	size_t n = strlen(s);

	if (n <= max) {
		return n;
	}
	n = max;
	/* Back over continuation bytes to the start of the cut character. */
	while (n > 0 && ((uint8_t)s[n] & 0xc0) == 0x80) {
		n--;
	}
	return n;
}

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

void mc_advert_encode(struct mc_packet *p, const struct mc_advert *a, const uint8_t prv[64])
{
	uint8_t app[MC_ADVERT_DATA_MAX];
	uint8_t msg[32 + 4 + MC_ADVERT_DATA_MAX];
	size_t n = 1, name_len;

	app[0] = a->type;
	if (a->has_latlon) {
		app[0] |= FLAG_LATLON;
		put32(app + 1, a->lat_e6);
		put32(app + 5, a->lon_e6);
		n += 8;
	}
	name_len = mc_utf8_prefix(a->name, sizeof(app) - n);
	if (name_len) {
		app[0] |= FLAG_NAME;
		memcpy(app + n, a->name, name_len);
		n += name_len;
	}
	memcpy(msg, a->pub, 32);
	put32(msg + 32, a->timestamp);
	memcpy(msg + 36, app, n);

	p->header = MC_PT_ADVERT << 2;
	memcpy(p->payload, a->pub, 32);
	put32(p->payload + 32, a->timestamp);
	mc_ed25519_sign(p->payload + 36, msg, 36 + n, a->pub, prv);
	memcpy(p->payload + 100, app, n);
	p->payload_len = 100 + n;
}

int mc_advert_decode(const struct mc_packet *p, struct mc_advert *a)
{
	uint8_t msg[32 + 4 + MC_ADVERT_DATA_MAX];
	const uint8_t *app = p->payload + 100;
	size_t app_len, i = 1;

	if (p->payload_len < 101) {
		return -1; /* app data has at least the flags */
	}
	app_len = p->payload_len - 100;
	if (app_len > MC_ADVERT_DATA_MAX) {
		app_len = MC_ADVERT_DATA_MAX;
	}
	memset(a, 0, sizeof(*a));
	memcpy(a->pub, p->payload, 32);
	a->timestamp = get32(p->payload + 32);
	a->type = app[0] & 0x0f;
	if (app[0] & FLAG_LATLON) {
		if (app_len < i + 8) {
			return -1;
		}
		a->has_latlon = true;
		a->lat_e6 = (int32_t)get32(app + i);
		a->lon_e6 = (int32_t)get32(app + i + 4);
		i += 8;
	}
	i += (app[0] & FLAG_FEAT1) ? 2 : 0;
	i += (app[0] & FLAG_FEAT2) ? 2 : 0;
	if (i > app_len) {
		return -1;
	}
	if (app[0] & FLAG_NAME) {
		size_t n = app_len - i;

		memcpy(a->name, app + i, n);
		a->name[n] = '\0';
	}
	memcpy(msg, a->pub, 32);
	memcpy(msg + 32, p->payload + 32, 4);
	memcpy(msg + 36, app, app_len);
	if (!mc_ed25519_verify(p->payload + 36, msg, 36 + app_len, a->pub)) {
		return -2;
	}
	return 0;
}
