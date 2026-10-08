/*
 * Tone output on a PWM channel: loudspeaker (low-side FET) or earpiece
 * (RC filter into the PAM8302 amplifier, which has its own supply).
 *
 * Each output needs its own PWM instance because the tone frequency sets the
 * period of the whole instance. The volume scales the duty cycle; 100 % volume
 * is a 50 % square wave.
 */

#define DT_DRV_COMPAT cmdz5_audio_pwm

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cmdz5/drivers/audio_out.h>

LOG_MODULE_REGISTER(audio_pwm, CONFIG_CMDZ5_AUDIO_LOG_LEVEL);

struct audio_pwm_config {
	struct pwm_dt_spec pwm;
	const struct device *amp_supply;
};

struct audio_pwm_data {
	const struct device *dev;
	struct k_work_delayable stop_work;
	bool busy;
	bool amp_on;
};

static void amp_set(const struct device *dev, bool on)
{
#ifdef CONFIG_REGULATOR
	const struct audio_pwm_config *cfg = dev->config;
	struct audio_pwm_data *data = dev->data;

	if (cfg->amp_supply && on != data->amp_on) {
		int ret = on ? regulator_enable(cfg->amp_supply)
			     : regulator_disable(cfg->amp_supply);

		if (ret) {
			LOG_WRN("amp supply %s failed (%d)", on ? "on" : "off", ret);
			return;
		}
		data->amp_on = on;
	}
#endif
}

static int output_off(const struct device *dev)
{
	const struct audio_pwm_config *cfg = dev->config;

	/* Pulse width 0 keeps the pin low, as required while the amp is off. */
	return pwm_set_pulse_dt(&cfg->pwm, 0);
}

static void stop_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct audio_pwm_data *data = CONTAINER_OF(dwork, struct audio_pwm_data, stop_work);

	output_off(data->dev);
	amp_set(data->dev, false);
	data->busy = false;
}

static int audio_pwm_tone(const struct device *dev, uint32_t freq_hz, uint32_t duration_ms,
			  uint8_t volume)
{
	const struct audio_pwm_config *cfg = dev->config;
	struct audio_pwm_data *data = dev->data;
	int ret;

	k_work_cancel_delayable(&data->stop_work);
	data->busy = true;

	if (freq_hz == 0 || volume == 0) {
		ret = output_off(dev);
	} else {
		uint32_t period = PWM_HZ(freq_hz);
		uint32_t pulse = (uint64_t)period * MIN(volume, 100) / 200;

		amp_set(dev, true);
		ret = pwm_set_dt(&cfg->pwm, period, pulse);
	}
	k_work_schedule(&data->stop_work, K_MSEC(duration_ms));
	return ret;
}

static int audio_pwm_stop(const struct device *dev)
{
	struct audio_pwm_data *data = dev->data;

	k_work_cancel_delayable(&data->stop_work);
	output_off(dev);
	amp_set(dev, false);
	data->busy = false;
	return 0;
}

static bool audio_pwm_busy(const struct device *dev)
{
	struct audio_pwm_data *data = dev->data;

	return data->busy;
}

static const struct audio_out_api audio_pwm_api = {
	.tone = audio_pwm_tone,
	.stop = audio_pwm_stop,
	.busy = audio_pwm_busy,
};

static int audio_pwm_init(const struct device *dev)
{
	const struct audio_pwm_config *cfg = dev->config;
	struct audio_pwm_data *data = dev->data;

	if (!pwm_is_ready_dt(&cfg->pwm)) {
		return -ENODEV;
	}
	data->dev = dev;
	k_work_init_delayable(&data->stop_work, stop_work_handler);
	return output_off(dev);
}

#define AUDIO_PWM_DEFINE(n)                                                                \
	static const struct audio_pwm_config audio_pwm_config_##n = {                       \
		.pwm = PWM_DT_SPEC_INST_GET(n),                                            \
		.amp_supply = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, amp_supply),            \
					  (DEVICE_DT_GET(DT_INST_PHANDLE(n, amp_supply))), \
					  (NULL)),                                         \
	};                                                                                 \
	static struct audio_pwm_data audio_pwm_data_##n;                                   \
	DEVICE_DT_INST_DEFINE(n, audio_pwm_init, NULL, &audio_pwm_data_##n,                 \
			      &audio_pwm_config_##n, POST_KERNEL,                          \
			      CONFIG_CMDZ5_AUDIO_INIT_PRIORITY, &audio_pwm_api);

DT_INST_FOREACH_STATUS_OKAY(AUDIO_PWM_DEFINE)
