/*
 * Emulated loudspeaker and earpiece. Tones are synthesised and played through
 * the PC's sound card:
 * - speaker: pulse wave with the same duty cycle the board's PWM uses, so the
 *   volume behaves like on the hardware;
 * - earpiece: quieter sine, roughly what the RC filter and amplifier deliver.
 */

#define DT_DRV_COMPAT cmdz5_audio_emul

#include <math.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <cmdz5/drivers/audio_out.h>

#include "frontpanel.h"

#define CHUNK 512
#define SPEAKER_AMPLITUDE  12000.0f
#define EARPIECE_AMPLITUDE 3000.0f

struct audio_emul_config {
	enum frontpanel_sink sink;
};

struct audio_emul_data {
	int64_t busy_until;
};

static int audio_emul_tone(const struct device *dev, uint32_t freq_hz, uint32_t duration_ms,
			   uint8_t volume)
{
	const struct audio_emul_config *cfg = dev->config;
	struct audio_emul_data *data = dev->data;
	static int16_t buf[CHUNK];
	int rate = frontpanel_audio_rate();
	int total = (int)((int64_t)rate * duration_ms / 1000);
	float vol = MIN(volume, 100) / 100.0f;
	float duty = vol / 2.0f;
	float phase = 0.0f;
	float step = (float)freq_hz / rate;

	frontpanel_audio_stop();

	for (int done = 0; done < total;) {
		int n = MIN(CHUNK, total - done);

		for (int i = 0; i < n; i++) {
			float s = 0.0f;

			if (freq_hz > 0) {
				if (cfg->sink == FRONTPANEL_SPEAKER) {
					/* DC-free pulse wave. */
					s = SPEAKER_AMPLITUDE * ((phase < duty ? 1.0f : 0.0f) - duty);
				} else {
					s = EARPIECE_AMPLITUDE * vol * sinf(2.0f * 3.14159265f * phase);
				}
			}
			buf[i] = (int16_t)s;
			phase += step;
			if (phase >= 1.0f) {
				phase -= 1.0f;
			}
		}
		/* The indicator duration is passed with the first chunk only. */
		frontpanel_audio_play(cfg->sink, buf, n, done == 0 ? duration_ms : 0);
		done += n;
	}

	data->busy_until = k_uptime_get() + duration_ms;
	return 0;
}

static int audio_emul_stop(const struct device *dev)
{
	struct audio_emul_data *data = dev->data;

	frontpanel_audio_stop();
	data->busy_until = 0;
	return 0;
}

static bool audio_emul_busy(const struct device *dev)
{
	struct audio_emul_data *data = dev->data;

	return k_uptime_get() < data->busy_until;
}

static const struct audio_out_api audio_emul_api = {
	.tone = audio_emul_tone,
	.stop = audio_emul_stop,
	.busy = audio_emul_busy,
};

/* The enum order in the binding matches enum frontpanel_sink. */
#define AUDIO_EMUL_DEFINE(n)                                                               \
	static const struct audio_emul_config audio_emul_config_##n = {                     \
		.sink = DT_INST_ENUM_IDX(n, sink),                                         \
	};                                                                                 \
	static struct audio_emul_data audio_emul_data_##n;                                 \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, &audio_emul_data_##n, &audio_emul_config_##n,  \
			      POST_KERNEL, CONFIG_CMDZ5_AUDIO_INIT_PRIORITY,               \
			      &audio_emul_api);

DT_INST_FOREACH_STATUS_OKAY(AUDIO_EMUL_DEFINE)
