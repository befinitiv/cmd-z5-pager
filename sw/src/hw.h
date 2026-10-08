/*
 * Access to the phone hardware through the devicetree node labels that the
 * board and the emulator both define (lcd, backlight, vibration, speaker,
 * earpiece).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#include "gfx/gfx.h"

enum hw_sink {
	HW_SPEAKER,
	HW_EARPIECE,
};

struct hw_note {
	uint16_t freq; /* Hz, 0 = rest */
	uint16_t ms;
};

int hw_init(void);

int hw_display_power(bool on);
bool hw_display_is_on(void);
void hw_display_contrast(uint8_t value);
/* Send the changed rows of the framebuffer to the display. */
void hw_display_flush(struct gfx *g);

/* Backlight brightness 0..100 (keypad and display LEDs share one switch). */
void hw_backlight(uint8_t percent);
/* Blink the backlight n times, then restore the last brightness. */
void hw_backlight_blink(int n);

void hw_vibrate(uint32_t ms);

void hw_tone(enum hw_sink sink, uint32_t freq_hz, uint32_t ms, uint8_t volume);
/* Play a note sequence in the background. Replaces a running melody. */
void hw_melody(enum hw_sink sink, const struct hw_note *notes, size_t count, uint8_t volume);
void hw_audio_stop(void);

/*
 * Switch the device completely off: nPM1300 ship mode on the board (holding
 * the power key for about 1 s switches it on again), on the emulator a
 * blank panel until Esc is held. Does not return.
 */
FUNC_NORETURN void hw_power_off(void);
