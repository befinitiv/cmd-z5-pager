/*
 * PCM audio output for the loudspeaker and the earpiece. Each output is a
 * separate device (node labels `speaker` and `earpiece`).
 *
 * The output pulls the samples: once started, the driver calls fill() from
 * its own thread for each block of signed 16-bit mono samples at rate().
 * When fill() returns false (nothing more to play), the driver lets the
 * output settle and switches it off (PWM, amplifier) until the next start.
 *
 * Board: PWM with EasyDMA as an 8-bit DAC (audio_pwm.c). Emulator: mixed
 * into the PC's sound output (emul/audio_emul.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

/* Fill buf with n samples. Return false when there is nothing more to play. */
typedef bool (*audio_out_fill_t)(int16_t *buf, size_t n, void *user);

struct audio_out_api {
	uint32_t (*rate)(const struct device *dev);
	/* Start pulling samples; a running output just keeps going. */
	int (*start)(const struct device *dev, audio_out_fill_t fill, void *user);
	/* Stop at once, without waiting for fill() to finish. */
	int (*stop)(const struct device *dev);
};

static inline uint32_t audio_out_rate(const struct device *dev)
{
	return ((const struct audio_out_api *)dev->api)->rate(dev);
}

static inline int audio_out_start(const struct device *dev, audio_out_fill_t fill, void *user)
{
	return ((const struct audio_out_api *)dev->api)->start(dev, fill, user);
}

static inline int audio_out_stop(const struct device *dev)
{
	return ((const struct audio_out_api *)dev->api)->stop(dev);
}
