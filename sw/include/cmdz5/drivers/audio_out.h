/*
 * Simple audio output for the loudspeaker and the earpiece. Each output is a
 * separate device (node labels `speaker` and `earpiece`).
 *
 * The first version only plays tones; PCM playback (PWM EasyDMA on the board)
 * is added later through the optional `play_pcm` call.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

struct audio_out_api {
	/*
	 * Start a square-wave tone and return immediately. A new tone replaces a
	 * running one. freq_hz = 0 plays silence for the duration (a pause).
	 * volume: 0..100.
	 */
	int (*tone)(const struct device *dev, uint32_t freq_hz, uint32_t duration_ms,
		    uint8_t volume);
	int (*stop)(const struct device *dev);
	bool (*busy)(const struct device *dev);
};

static inline int audio_out_tone(const struct device *dev, uint32_t freq_hz,
				 uint32_t duration_ms, uint8_t volume)
{
	return ((const struct audio_out_api *)dev->api)->tone(dev, freq_hz, duration_ms, volume);
}

static inline int audio_out_stop(const struct device *dev)
{
	return ((const struct audio_out_api *)dev->api)->stop(dev);
}

static inline bool audio_out_busy(const struct device *dev)
{
	return ((const struct audio_out_api *)dev->api)->busy(dev);
}
