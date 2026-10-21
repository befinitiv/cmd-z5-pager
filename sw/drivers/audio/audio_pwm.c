/*
 * PCM output on an nRF52840 PWM instance used as an 8-bit DAC: loudspeaker
 * (low-side FET) or earpiece (RC filter into the PAM8302 amplifier).
 *
 * The PWM runs from 16 MHz with a top value of 256, a 62.5 kHz carrier above
 * hearing. Each sample is held for 4 carrier periods (15625 samples/s). Two
 * EasyDMA sequences play in a loop; when one ends, its buffer is refilled by
 * the audio thread while the other plays (double buffering).
 *
 * The loudspeaker is driven between VSYS and ground, so it only sees a duty
 * cycle >= 0. A constant mid-level would push a large DC current through it,
 * so its offset follows the signal peak instead ("dc-coupled"): silence is a
 * duty cycle of 0. The earpiece amplifier is AC-coupled: it gets a mid-level
 * offset, ramped up and down to avoid pops.
 */

#define DT_DRV_COMPAT cmdz5_audio_pwm

#include <stdlib.h>
#include <string.h>

#include <nrfx_pwm.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <cmdz5/drivers/audio_out.h>

LOG_MODULE_REGISTER(audio_pwm, CONFIG_CMDZ5_AUDIO_LOG_LEVEL);

#define PWM_CLOCK_HZ 16000000
#define TOP          256
#define REPEATS      3 /* each sample plays REPEATS + 1 carrier periods */
#define RATE         (PWM_CLOCK_HZ / TOP / (REPEATS + 1))
#define BLOCK        256 /* samples per sequence, 16.4 ms */
/* Blocks of silence before the output is switched off. */
#define QUIET_BLOCKS 3
/* Speaker offset: falls back to 0 within about 30 ms of silence (Q8). */
#define BIAS_DECAY   ((TOP / 2 * 256) / (RATE * 30 / 1000))
/* Earpiece offset: ramps to mid-level in about 8 ms (Q8). */
#define RAMP_STEP    ((TOP / 2 * 256) / (RATE * 8 / 1000))

/* Bit 15 set: the pin is high for the compare value (normal polarity). */
#define POLARITY BIT(15)

enum {
	REQ_HALF0,
	REQ_HALF1,
	REQ_START,
	REQ_STOP,
};

struct audio_pwm_config {
	const struct pinctrl_dev_config *pcfg;
	const struct device *amp_supply;
	bool dc_coupled;
	uint8_t irq_priority;
	void (*irq_connect)(void);
	k_thread_stack_t *stack;
	size_t stack_size;
};

struct audio_pwm_data {
	nrfx_pwm_t pwm;
	uint16_t seq[2][BLOCK]; /* EasyDMA reads these */
	int16_t pcm[BLOCK];
	audio_out_fill_t fill;
	void *user;
	atomic_t req;
	struct k_sem wake;
	struct k_thread thread;
	bool running;
	int quiet;
	int32_t offset; /* Q8 duty units: speaker peak follower or earpiece ramp */
};

static uint32_t audio_pwm_rate(const struct device *dev)
{
	ARG_UNUSED(dev);
	return RATE;
}

static void amp_set(const struct device *dev, bool on)
{
#ifdef CONFIG_REGULATOR
	const struct audio_pwm_config *cfg = dev->config;

	if (cfg->amp_supply) {
		int ret = on ? regulator_enable(cfg->amp_supply)
			     : regulator_disable(cfg->amp_supply);

		if (ret) {
			LOG_WRN("amp supply %s failed (%d)", on ? "on" : "off", ret);
		}
	}
#endif
}

/* Samples to duty cycle values; ramp: the stream is starting or ending. */
static void convert(const struct device *dev, uint16_t *seq, const int16_t *pcm, bool ending)
{
	const struct audio_pwm_config *cfg = dev->config;
	struct audio_pwm_data *data = dev->data;
	int32_t offset = data->offset;

	for (int i = 0; i < BLOCK; i++) {
		int32_t s = pcm[i] * TOP / 65536; /* -128..127 */
		int32_t duty;

		if (cfg->dc_coupled) {
			/* Offset = recent peak: the duty cycle swings 0..2 x peak. */
			int32_t peak = abs(s) << 8;

			offset = peak > offset ? peak : MAX(offset - BIAS_DECAY, peak);
			duty = (offset >> 8) + s;
		} else {
			int32_t target = ending ? 0 : TOP / 2 << 8;

			offset = offset < target ? MIN(offset + RAMP_STEP, target)
						 : MAX(offset - RAMP_STEP, target);
			/* The signal scales with the offset while it ramps. */
			duty = (offset >> 8) + s * (offset >> 8) / (TOP / 2);
		}
		seq[i] = CLAMP(duty, 0, TOP) | POLARITY;
	}
	data->offset = offset;
}

static void refill(const struct device *dev, int half)
{
	struct audio_pwm_data *data = dev->data;
	bool more = data->fill && data->fill(data->pcm, BLOCK, data->user);

	if (!data->fill) {
		memset(data->pcm, 0, sizeof(data->pcm));
	}
	/*
	 * fill() still filled the block when it returns false: the last block
	 * holds the end of the sound (all of a sound shorter than a block).
	 * The earpiece offset ramps down from the first silent block on.
	 */
	data->quiet = more ? 0 : data->quiet + 1;
	convert(dev, data->seq[half], data->pcm, data->quiet > 1);
}

static void begin(const struct device *dev)
{
	const struct audio_pwm_config *cfg = dev->config;
	struct audio_pwm_data *data = dev->data;
	nrf_pwm_sequence_t seq[2];

	amp_set(dev, true);
	(void)pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	data->offset = 0;
	data->quiet = 0;
	for (int i = 0; i < 2; i++) {
		refill(dev, i);
		seq[i] = (nrf_pwm_sequence_t){
			.values.p_common = data->seq[i],
			.length = BLOCK,
			.repeats = REPEATS,
		};
	}
	nrfx_pwm_complex_playback(&data->pwm, &seq[0], &seq[1], 1,
				  NRFX_PWM_FLAG_LOOP | NRFX_PWM_FLAG_SIGNAL_END_SEQ0 |
					  NRFX_PWM_FLAG_SIGNAL_END_SEQ1);
	data->running = true;
}

static void end(const struct device *dev)
{
	const struct audio_pwm_config *cfg = dev->config;
	struct audio_pwm_data *data = dev->data;

	nrfx_pwm_stop(&data->pwm, true);
	/* Sleep state: pin disconnected, the FET gate pull-down keeps it off. */
	(void)pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_SLEEP);
	amp_set(dev, false);
	data->running = false;
}

static void audio_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct audio_pwm_data *data = dev->data;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sem_take(&data->wake, K_FOREVER);

		if (atomic_test_and_clear_bit(&data->req, REQ_STOP)) {
			if (data->running) {
				end(dev);
			}
			/* Drop refills of the stopped stream; a new start stays. */
			atomic_and(&data->req, ~(BIT(REQ_HALF0) | BIT(REQ_HALF1)));
		}
		if (atomic_test_and_clear_bit(&data->req, REQ_START)) {
			if (!data->running) {
				begin(dev);
			}
			data->quiet = 0;
		}
		for (int half = 0; half < 2; half++) {
			if (atomic_test_and_clear_bit(&data->req, REQ_HALF0 + half) &&
			    data->running) {
				refill(dev, half);
			}
		}
		if (data->running && data->quiet >= QUIET_BLOCKS) {
			end(dev);
		}
	}
}

/* nrfx event handler (interrupt context): a sequence ended, refill it. */
static void pwm_handler(nrfx_pwm_event_type_t event, void *ctx)
{
	const struct device *dev = ctx;
	struct audio_pwm_data *data = dev->data;

	if (event == NRFX_PWM_EVENT_END_SEQ0 || event == NRFX_PWM_EVENT_END_SEQ1) {
		atomic_set_bit(&data->req, event == NRFX_PWM_EVENT_END_SEQ0 ? REQ_HALF0 : REQ_HALF1);
		k_sem_give(&data->wake);
	}
}

static int audio_pwm_start(const struct device *dev, audio_out_fill_t fill, void *user)
{
	struct audio_pwm_data *data = dev->data;

	data->user = user;
	data->fill = fill;
	atomic_set_bit(&data->req, REQ_START);
	k_sem_give(&data->wake);
	return 0;
}

static int audio_pwm_stop(const struct device *dev)
{
	struct audio_pwm_data *data = dev->data;

	atomic_set_bit(&data->req, REQ_STOP);
	k_sem_give(&data->wake);
	return 0;
}

static const struct audio_out_api audio_pwm_api = {
	.rate = audio_pwm_rate,
	.start = audio_pwm_start,
	.stop = audio_pwm_stop,
};

static int audio_pwm_init(const struct device *dev)
{
	const struct audio_pwm_config *cfg = dev->config;
	struct audio_pwm_data *data = dev->data;
	nrfx_pwm_config_t pwm_cfg = {
		.skip_gpio_cfg = true,
		.skip_psel_cfg = true,
		.irq_priority = cfg->irq_priority,
		.base_clock = NRF_PWM_CLK_16MHz,
		.count_mode = NRF_PWM_MODE_UP,
		.top_value = TOP,
		.load_mode = NRF_PWM_LOAD_COMMON,
		.step_mode = NRF_PWM_STEP_AUTO,
	};
	int ret;

	(void)pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_SLEEP);
	cfg->irq_connect();
	ret = nrfx_pwm_init(&data->pwm, &pwm_cfg, pwm_handler, (void *)dev);
	if (ret) {
		LOG_ERR("PWM init failed (%d)", ret);
		return -EIO;
	}
	k_sem_init(&data->wake, 0, K_SEM_MAX_LIMIT);
	k_thread_create(&data->thread, cfg->stack, cfg->stack_size, audio_thread, (void *)dev,
			NULL, NULL, K_PRIO_COOP(2), 0, K_NO_WAIT);
	k_thread_name_set(&data->thread, dev->name);
	return 0;
}

/* The PWM peripheral comes from the pwm-peripheral phandle (status disabled). */
#define PWM_NODE(n) DT_INST_PHANDLE(n, pwm_peripheral)

#define AUDIO_PWM_DEFINE(n)                                                                \
	PINCTRL_DT_DEFINE(PWM_NODE(n));                                                    \
	K_THREAD_STACK_DEFINE(audio_pwm_stack_##n, 1536);                                  \
	static struct audio_pwm_data audio_pwm_data_##n = {                                \
		.pwm = NRFX_PWM_INSTANCE(DT_REG_ADDR(PWM_NODE(n))),                        \
	};                                                                                 \
	static void audio_pwm_irq_connect_##n(void)                                        \
	{                                                                                  \
		IRQ_CONNECT(DT_IRQN(PWM_NODE(n)), DT_IRQ(PWM_NODE(n), priority),           \
			    nrfx_pwm_irq_handler, &audio_pwm_data_##n.pwm, 0);             \
	}                                                                                  \
	static const struct audio_pwm_config audio_pwm_config_##n = {                       \
		.pcfg = PINCTRL_DT_DEV_CONFIG_GET(PWM_NODE(n)),                            \
		.amp_supply = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, amp_supply),            \
					  (DEVICE_DT_GET(DT_INST_PHANDLE(n, amp_supply))), \
					  (NULL)),                                         \
		.dc_coupled = DT_INST_PROP(n, dc_coupled),                                 \
		.irq_priority = DT_IRQ(PWM_NODE(n), priority),                             \
		.irq_connect = audio_pwm_irq_connect_##n,                                  \
		.stack = audio_pwm_stack_##n,                                              \
		.stack_size = K_THREAD_STACK_SIZEOF(audio_pwm_stack_##n),                  \
	};                                                                                 \
	DEVICE_DT_INST_DEFINE(n, audio_pwm_init, NULL, &audio_pwm_data_##n,                 \
			      &audio_pwm_config_##n, POST_KERNEL,                          \
			      CONFIG_CMDZ5_AUDIO_INIT_PRIORITY, &audio_pwm_api);

DT_INST_FOREACH_STATUS_OKAY(AUDIO_PWM_DEFINE)
