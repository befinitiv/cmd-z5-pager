/*
 * Zephyr-side interface of the emulator front panel, used by the emulated
 * drivers (display bus, LEDs, audio) and by the application for host time.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum frontpanel_output {
	FRONTPANEL_BACKLIGHT,
	FRONTPANEL_VIBRATION,
};

enum frontpanel_sink {
	FRONTPANEL_SPEAKER,
	FRONTPANEL_EARPIECE,
};

void frontpanel_lcd_update(const uint8_t *fb, bool on, uint8_t contrast);
void frontpanel_set_output(enum frontpanel_output out, uint8_t level);

int frontpanel_audio_rate(void);
/* Queue mixed samples for the sound card. */
void frontpanel_audio_queue(const int16_t *samples, int count);
/* Drop the queued samples. */
void frontpanel_audio_clear(void);
/* Light the SPK or EAR indicator for ms. */
void frontpanel_audio_active(enum frontpanel_sink sink, uint32_t ms);

/*
 * Emulate "completely off": the panel stays blank and ignores all input
 * until the power key is held for 1 s, which restarts the firmware.
 */
void frontpanel_power_off(void);

/* State for the simulated battery. */
struct frontpanel_load {
	uint8_t backlight; /* 0..100 */
	uint8_t vibration; /* 0..100 */
	bool speaker;
	bool earpiece;
	bool lcd_on;
};

void frontpanel_get_load(struct frontpanel_load *out);
/* USB power plugged in (U key, the USB indicator or "usb on" in a script). */
bool frontpanel_usb(void);

int64_t frontpanel_unix_time(void);
int32_t frontpanel_utc_offset(void);
