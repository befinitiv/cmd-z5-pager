/*
 * Zephyr side of the emulator front panel: input device for the phone keys,
 * wheel and lid, and owner of the state that the host side renders.
 */

#define DT_DRV_COMPAT cmdz5_frontpanel

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <dt-bindings/cmdz5/keys.h>

#include "frontpanel.h"
#include "frontpanel_bottom.h"

LOG_MODULE_REGISTER(frontpanel, CONFIG_CMDZ5_EMUL_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "need exactly one front panel");

#define RENDER_INTERVAL_MS 33
/* Same as ship-to-active-time-ms of the board's nPM1300. */
#define POWER_ON_HOLD_MS 1008

static const struct device *const fp_dev = DEVICE_DT_GET(DT_DRV_INST(0));

/* Same codes as the keymap of the real board. */
static const uint16_t button_codes[FP_BTN_COUNT] = {
	[FP_BTN_1] = INPUT_KEY_1,
	[FP_BTN_2] = INPUT_KEY_2,
	[FP_BTN_3] = INPUT_KEY_3,
	[FP_BTN_4] = INPUT_KEY_4,
	[FP_BTN_5] = INPUT_KEY_5,
	[FP_BTN_6] = INPUT_KEY_6,
	[FP_BTN_7] = INPUT_KEY_7,
	[FP_BTN_8] = INPUT_KEY_8,
	[FP_BTN_9] = INPUT_KEY_9,
	[FP_BTN_0] = INPUT_KEY_0,
	[FP_BTN_STAR] = CMDZ5_KEY_STAR,
	[FP_BTN_POUND] = CMDZ5_KEY_POUND,
	[FP_BTN_CALL] = CMDZ5_KEY_CALL,
	[FP_BTN_MEMO] = CMDZ5_KEY_MEMO,
	[FP_BTN_CLEAR] = INPUT_KEY_BACKSPACE,
	[FP_BTN_WHEEL_PUSH] = INPUT_KEY_ENTER,
	[FP_BTN_WHEEL_UP] = INPUT_KEY_PAGEUP,
	[FP_BTN_WHEEL_DOWN] = INPUT_KEY_PAGEDOWN,
	[FP_BTN_POWER] = INPUT_KEY_POWER,
};

/* Given once the window exists; the script runner (script.c) waits for it. */
extern struct k_sem frontpanel_ready;

static K_MUTEX_DEFINE(state_lock);
static struct fp_state state;
static int64_t speaker_until, earpiece_until;

static bool powered_off;

static void power_on(struct k_work *work)
{
	ARG_UNUSED(work);
	printk("frontpanel: power key held, switching on\n");
	/* Like waking from ship mode: a fresh boot. */
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(power_on_work, power_on);

void frontpanel_power_off(void)
{
	powered_off = true;
	printk("frontpanel: switched off, hold Esc (power key) for 1 s to switch on\n");
}

/* Called from Zephyr's SDL event thread. */
static void on_event(const struct fp_event *ev)
{
	if (powered_off) {
		/* Only a long press on the power key does anything. */
		if (ev->type == FP_EV_BUTTON && ev->button == FP_BTN_POWER) {
			if (ev->value) {
				k_work_reschedule(&power_on_work, K_MSEC(POWER_ON_HOLD_MS));
			} else {
				k_work_cancel_delayable(&power_on_work);
			}
		}
		return;
	}

	switch (ev->type) {
	case FP_EV_BUTTON:
		input_report_key(fp_dev, button_codes[ev->button], ev->value, true, K_FOREVER);
		break;
	case FP_EV_WHEEL:
		input_report_rel(fp_dev, INPUT_REL_WHEEL, ev->value, true, K_FOREVER);
		break;
	case FP_EV_LID:
		k_mutex_lock(&state_lock, K_FOREVER);
		state.lid_closed = ev->value;
		k_mutex_unlock(&state_lock);
		input_report_key(fp_dev, CMDZ5_KEY_LID, ev->value, true, K_FOREVER);
		break;
	case FP_EV_USB:
		k_mutex_lock(&state_lock, K_FOREVER);
		state.usb = ev->value;
		k_mutex_unlock(&state_lock);
		break;
	default:
		break;
	}
}

void frontpanel_get_load(struct frontpanel_load *out)
{
	int64_t now = k_uptime_get();

	k_mutex_lock(&state_lock, K_FOREVER);
	out->backlight = state.backlight;
	out->vibration = state.vibration;
	out->speaker = now < speaker_until;
	out->earpiece = now < earpiece_until;
	out->lcd_on = state.lcd_on;
	k_mutex_unlock(&state_lock);
}

bool frontpanel_usb(void)
{
	bool usb;

	k_mutex_lock(&state_lock, K_FOREVER);
	usb = state.usb;
	k_mutex_unlock(&state_lock);
	return usb;
}

void frontpanel_lcd_update(const uint8_t *fb, bool on, uint8_t contrast)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	memcpy(state.fb, fb, sizeof(state.fb));
	state.lcd_on = on;
	state.contrast = contrast;
	k_mutex_unlock(&state_lock);
}

void frontpanel_set_output(enum frontpanel_output out, uint8_t level)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	if (out == FRONTPANEL_BACKLIGHT) {
		state.backlight = MIN(level, 100);
	} else if (out == FRONTPANEL_VIBRATION) {
		state.vibration = MIN(level, 100);
	}
	k_mutex_unlock(&state_lock);
}

int frontpanel_audio_rate(void)
{
	return fp_bottom_audio_rate();
}

void frontpanel_audio_play(enum frontpanel_sink sink, const int16_t *samples, int count,
			   uint32_t duration_ms)
{
	int64_t until = k_uptime_get() + duration_ms;

	fp_bottom_audio_queue(samples, count);
	k_mutex_lock(&state_lock, K_FOREVER);
	if (sink == FRONTPANEL_SPEAKER) {
		speaker_until = MAX(speaker_until, until);
	} else {
		earpiece_until = MAX(earpiece_until, until);
	}
	k_mutex_unlock(&state_lock);
}

void frontpanel_audio_stop(void)
{
	fp_bottom_audio_clear();
	k_mutex_lock(&state_lock, K_FOREVER);
	speaker_until = 0;
	earpiece_until = 0;
	k_mutex_unlock(&state_lock);
}

int64_t frontpanel_unix_time(void)
{
	return fp_bottom_unix_time();
}

int32_t frontpanel_utc_offset(void)
{
	return fp_bottom_utc_offset();
}

static void render_thread(void *p1, void *p2, void *p3)
{
	static struct fp_state snapshot;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (fp_bottom_init(DT_INST_PROP(0, scale), on_event) != 0) {
		LOG_ERR("front panel window could not be created");
		return;
	}
	k_sem_give(&frontpanel_ready);

	for (;;) {
		int64_t now = k_uptime_get();

		k_mutex_lock(&state_lock, K_FOREVER);
		state.speaker_active = now < speaker_until;
		state.earpiece_active = now < earpiece_until;
		state.frame++;
		snapshot = state;
		k_mutex_unlock(&state_lock);

		fp_bottom_render(&snapshot);
		k_msleep(RENDER_INTERVAL_MS);
	}
}

K_THREAD_DEFINE(frontpanel_render, CONFIG_ARCH_POSIX_RECOMMENDED_STACK_SIZE, render_thread, NULL,
		NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);
