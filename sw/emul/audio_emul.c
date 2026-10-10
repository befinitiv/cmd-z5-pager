/*
 * Emulated loudspeaker and earpiece: the same PCM streams as on the board,
 * mixed by one thread and played through the PC's sound card. Samples are
 * reduced to the 8 bits of the board's PWM DAC; the earpiece is quieter.
 */

#define DT_DRV_COMPAT cmdz5_audio_emul

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <cmdz5/drivers/audio_out.h>

#include "frontpanel.h"

#define BLOCK        256 /* samples, 16 ms at 16 kHz */
#define LEAD_BLOCKS  3   /* how far the queue runs ahead of the sound card */
#define QUIET_BLOCKS 3   /* silent blocks before an output stops */

/* Output level relative to full scale: loudspeaker and earpiece. */
static const float sink_gain[] = {0.7f, 0.25f};

struct audio_emul_config {
	enum frontpanel_sink sink;
};

struct audio_emul_data {
	audio_out_fill_t fill;
	void *user;
	atomic_t active;
	int quiet;
};

static uint32_t audio_emul_rate(const struct device *dev)
{
	ARG_UNUSED(dev);
	return frontpanel_audio_rate();
}

static int audio_emul_start(const struct device *dev, audio_out_fill_t fill, void *user)
{
	struct audio_emul_data *data = dev->data;

	data->user = user;
	data->fill = fill;
	data->quiet = 0;
	atomic_set(&data->active, 1);
	return 0;
}

static int audio_emul_stop(const struct device *dev)
{
	struct audio_emul_data *data = dev->data;

	atomic_set(&data->active, 0);
	frontpanel_audio_clear();
	return 0;
}

static const struct audio_out_api audio_emul_api = {
	.rate = audio_emul_rate,
	.start = audio_emul_start,
	.stop = audio_emul_stop,
};

#define AUDIO_EMUL_DEFINE(n)                                                               \
	static const struct audio_emul_config audio_emul_config_##n = {                     \
		.sink = DT_INST_ENUM_IDX(n, sink),                                         \
	};                                                                                 \
	static struct audio_emul_data audio_emul_data_##n;                                 \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, &audio_emul_data_##n, &audio_emul_config_##n,  \
			      POST_KERNEL, CONFIG_CMDZ5_AUDIO_INIT_PRIORITY,               \
			      &audio_emul_api);

/* The enum order in the binding matches enum frontpanel_sink. */
DT_INST_FOREACH_STATUS_OKAY(AUDIO_EMUL_DEFINE)

#define OUTPUT_DEV(n) DEVICE_DT_INST_GET(n),
static const struct device *const outputs[] = {DT_INST_FOREACH_STATUS_OKAY(OUTPUT_DEV)};

/* Pull a block from every active output and queue the mix. */
static void mix_block(int ms)
{
	static int16_t part[BLOCK];
	static int16_t out[BLOCK];
	float mix[BLOCK] = {0};
	bool sound = false;

	for (size_t k = 0; k < ARRAY_SIZE(outputs); k++) {
		const struct audio_emul_config *cfg = outputs[k]->config;
		struct audio_emul_data *data = outputs[k]->data;
		bool audible = false;

		if (!atomic_get(&data->active) || !data->fill) {
			continue;
		}
		if (data->fill(part, BLOCK, data->user)) {
			data->quiet = 0;
		} else if (++data->quiet >= QUIET_BLOCKS) {
			atomic_set(&data->active, 0);
		}
		for (int i = 0; i < BLOCK; i++) {
			/* The board's PWM DAC has 8 bits. */
			int16_t s = (int16_t)(part[i] & ~0xff);

			mix[i] += s * sink_gain[cfg->sink];
			audible |= s != 0;
		}
		if (audible) {
			frontpanel_audio_active(cfg->sink, 2 * ms);
			sound = true;
		}
	}
	if (sound) {
		for (int i = 0; i < BLOCK; i++) {
			out[i] = (int16_t)CLAMP(mix[i], -32768.0f, 32767.0f);
		}
		frontpanel_audio_queue(out, BLOCK);
	}
}

/*
 * Paced by the uptime (native_sim runs in real time): keeps the sound
 * card's queue LEAD_BLOCKS ahead, and drops time that nothing was playing.
 */
static void mixer_thread(void *p1, void *p2, void *p3)
{
	int rate = frontpanel_audio_rate();
	int ms = BLOCK * 1000 / rate;
	int64_t start = k_uptime_get();
	int64_t produced = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		int64_t due = (k_uptime_get() - start) * rate / 1000;

		if (produced < due) {
			produced = due;
		}
		while (produced < due + LEAD_BLOCKS * BLOCK) {
			mix_block(ms);
			produced += BLOCK;
		}
		k_msleep(ms / 2);
	}
}

K_THREAD_DEFINE(audio_mixer, CONFIG_ARCH_POSIX_RECOMMENDED_STACK_SIZE, mixer_thread, NULL, NULL,
		NULL, K_PRIO_COOP(2), 0, 100);
