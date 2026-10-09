#include <math.h>
#include <string.h>

#include "meshtastic/channel.h"

static const uint8_t default_psk[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
					0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

struct preset {
	enum mt_preset id;
	const char *name;
	uint32_t bw_hz;
	uint8_t sf, cr;
};

static const struct preset presets[] = {
	{MT_PRESET_LONG_FAST, "LongFast", 250000, 11, 5},
	{MT_PRESET_LONG_SLOW, "LongSlow", 125000, 12, 8},
	{MT_PRESET_MEDIUM_SLOW, "MediumSlow", 250000, 10, 5},
	{MT_PRESET_MEDIUM_FAST, "MediumFast", 250000, 9, 5},
	{MT_PRESET_SHORT_SLOW, "ShortSlow", 250000, 8, 5},
	{MT_PRESET_SHORT_FAST, "ShortFast", 250000, 7, 5},
	{MT_PRESET_LONG_MODERATE, "LongMod", 125000, 11, 8},
	{MT_PRESET_SHORT_TURBO, "ShortTurbo", 500000, 7, 5},
	{MT_PRESET_LONG_TURBO, "LongTurbo", 500000, 11, 8},
};

struct region {
	enum mt_region id;
	const char *name;
	uint32_t start_khz, end_khz, spacing_khz;
	int8_t power;
	uint8_t duty;
};

static const struct region regions[] = {
	{MT_REGION_US, "US", 902000, 928000, 0, 30, 100},
	{MT_REGION_EU_433, "EU_433", 433000, 434000, 0, 10, 10},
	{MT_REGION_EU_868, "EU_868", 869400, 869650, 0, 27, 10},
};

static const struct preset *find_preset(enum mt_preset p)
{
	for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
		if (presets[i].id == p) {
			return &presets[i];
		}
	}
	return NULL;
}

static const struct region *find_region(enum mt_region r)
{
	for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
		if (regions[i].id == r) {
			return &regions[i];
		}
	}
	return NULL;
}

const char *mt_preset_name(enum mt_preset p)
{
	const struct preset *ps = find_preset(p);

	return ps ? ps->name : "Custom";
}

bool mt_preset_valid(enum mt_preset p)
{
	return find_preset(p) != NULL;
}

const char *mt_region_name(enum mt_region r)
{
	const struct region *rg = find_region(r);

	return rg ? rg->name : "UNSET";
}

uint32_t mt_djb2(const char *s)
{
	uint32_t h = 5381;

	while (*s) {
		h = h * 33 + (uint8_t)*s++;
	}
	return h;
}

static uint8_t xor_hash(const uint8_t *p, size_t n)
{
	uint8_t h = 0;

	while (n--) {
		h ^= *p++;
	}
	return h;
}

const char *mt_channel_name(const struct mt_channel *c, enum mt_preset preset)
{
	return c->name[0] ? c->name : mt_preset_name(preset);
}

void mt_channel_init(struct mt_channel *c, const char *name, enum mt_preset preset,
		     const uint8_t *psk, size_t psk_len)
{
	memset(c, 0, sizeof(*c));
	strncpy(c->name, name, sizeof(c->name) - 1);
	if (psk_len == 1) {
		if (psk[0] != 0) {
			memcpy(c->key, default_psk, 16);
			c->key[15] += psk[0] - 1;
			c->key_len = 16;
		}
	} else if (psk_len > 1) {
		/* Short keys are zero-padded to AES-128, longer ones to AES-256. */
		c->key_len = psk_len <= 16 ? 16 : 32;
		memcpy(c->key, psk, psk_len > 32 ? 32 : psk_len);
	}
	const char *n = mt_channel_name(c, preset);

	c->hash = xor_hash((const uint8_t *)n, strlen(n)) ^ xor_hash(c->key, c->key_len);
}

int mt_radio_params(enum mt_region region, enum mt_preset preset, const char *channel_name,
		    struct mt_radio_params *out)
{
	const struct preset *p = find_preset(preset);
	const struct region *r = find_region(region);

	if (!p || !r) {
		return -1;
	}
	/* In Hz, so that the result is exact (869.525 MHz, not 869.525024). */
	uint32_t bw = p->bw_hz;
	uint32_t span = (r->end_khz - r->start_khz) * 1000;
	uint32_t slots = span / (r->spacing_khz * 1000 + bw);

	if (slots == 0) {
		slots = 1;
	}
	uint32_t slot = mt_djb2(channel_name) % slots;

	out->freq_hz = r->start_khz * 1000 + bw / 2 + slot * bw;
	out->bw_hz = p->bw_hz;
	out->sf = p->sf;
	out->cr = p->cr;
	out->sync_word = MT_SYNC_WORD;
	out->preamble = MT_PREAMBLE;
	out->power_dbm = r->power;
	out->duty_cycle_percent = r->duty;

	/* Slot time: CAD (SX126x: 2 symbols + 0.5) + propagation, turnaround, MAC. */
	float symbol_ms = (float)(1u << p->sf) / (p->bw_hz / 1000.0f);

	out->slot_ms = (uint32_t)(2.5f * symbol_ms + 0.2f + 0.4f + 7.0f);
	return 0;
}

uint32_t mt_airtime_ms(const struct mt_radio_params *p, size_t len)
{
	/* Semtech LoRa time-on-air formula, explicit header, CRC on. */
	double tsym = (double)(1u << p->sf) / p->bw_hz * 1000.0;
	int de = tsym > 16.0; /* low data rate optimisation */
	double num = 8.0 * len - 4.0 * p->sf + 28 + 16;
	double den = 4.0 * (p->sf - 2 * de);
	double payload_sym = 8 + fmax(ceil(num / den) * p->cr, 0);
	double t = (p->preamble + 4.25 + payload_sym) * tsym;

	return (uint32_t)(t + 0.5);
}
