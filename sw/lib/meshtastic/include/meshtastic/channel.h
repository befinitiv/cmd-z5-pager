/*
 * Channels, modem presets and regions as defined by Meshtastic: channel
 * key expansion and hash, frequency slot, LoRa parameters, timing.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Values of Config.LoRaConfig.ModemPreset */
enum mt_preset {
	MT_PRESET_LONG_FAST = 0,
	MT_PRESET_LONG_SLOW = 1,
	MT_PRESET_MEDIUM_SLOW = 3,
	MT_PRESET_MEDIUM_FAST = 4,
	MT_PRESET_SHORT_SLOW = 5,
	MT_PRESET_SHORT_FAST = 6,
	MT_PRESET_LONG_MODERATE = 7,
	MT_PRESET_SHORT_TURBO = 8,
	MT_PRESET_LONG_TURBO = 9,
};

/* Values of Config.LoRaConfig.RegionCode (the supported subset) */
enum mt_region {
	MT_REGION_US = 1,
	MT_REGION_EU_433 = 2,
	MT_REGION_EU_868 = 3,
};

#define MT_SYNC_WORD 0x2b
#define MT_PREAMBLE  16

struct mt_radio_params {
	uint32_t freq_hz;
	uint32_t bw_hz;
	uint8_t sf;
	uint8_t cr;       /* 5..8 for 4/5..4/8 */
	uint8_t sync_word;
	uint16_t preamble;
	int8_t power_dbm; /* regional limit */
	uint8_t duty_cycle_percent;
	uint32_t slot_ms; /* contention window slot */
};

struct mt_channel {
	char name[12];  /* "" = preset name */
	uint8_t key[32];
	uint8_t key_len;  /* 0 = no encryption */
	uint8_t hash;
};

/* Display name of a preset ("LongFast"), used as the default channel name. */
const char *mt_preset_name(enum mt_preset p);
bool mt_preset_valid(enum mt_preset p);
const char *mt_region_name(enum mt_region r);

/*
 * Set up a channel. psk of 1 byte is a default-key index (0 = no
 * encryption, 1 = default key, n = default key with the last byte + n - 1).
 * An empty name means the preset name.
 */
void mt_channel_init(struct mt_channel *c, const char *name, enum mt_preset preset,
		     const uint8_t *psk, size_t psk_len);
const char *mt_channel_name(const struct mt_channel *c, enum mt_preset preset);

/* LoRa parameters for the primary channel name. Returns -1 for unknown values. */
int mt_radio_params(enum mt_region region, enum mt_preset preset, const char *channel_name,
		    struct mt_radio_params *out);

/* Time on air of a frame of len bytes, in ms. */
uint32_t mt_airtime_ms(const struct mt_radio_params *p, size_t len);

/* djb2 hash of a string (frequency slot selection). */
uint32_t mt_djb2(const char *s);

#ifdef __cplusplus
}
#endif
