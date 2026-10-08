#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#include <cmdz5/drivers/audio_out.h>
#include <cmdz5/drivers/lcd.h>

#include "hw.h"

#ifdef CONFIG_CMDZ5_EMUL
#include "../emul/frontpanel.h"
#endif

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(pmic)) && defined(CONFIG_MFD_NPM13XX)
#define HAVE_PMIC 1
static const struct device *const pmic = DEVICE_DT_GET(DT_NODELABEL(pmic));
/* nPM1300 SHIP block: TASKENTERSHIPMODE (datasheet register map, verify on the board). */
#define NPM1300_SHIP_BASE            0x0B
#define NPM1300_TASK_ENTER_SHIP_MODE 0x02
#endif

LOG_MODULE_REGISTER(hw, CONFIG_CMDZ5_APP_LOG_LEVEL);

static const struct device *const lcd = DEVICE_DT_GET(DT_NODELABEL(lcd));
static const struct device *const speaker = DEVICE_DT_GET(DT_NODELABEL(speaker));
static const struct device *const earpiece = DEVICE_DT_GET(DT_NODELABEL(earpiece));
static const struct led_dt_spec backlight = LED_DT_SPEC_GET(DT_NODELABEL(backlight));
static const struct led_dt_spec vibration = LED_DT_SPEC_GET(DT_NODELABEL(vibration));

BUILD_ASSERT(GFX_FB_SIZE == LCD_STRIDE * LCD_HEIGHT, "framebuffer format mismatch");

/* What the display currently shows, to skip unchanged rows. */
static uint8_t shadow[GFX_FB_SIZE];
static bool display_on;

static uint8_t backlight_level;
static struct k_work_delayable blink_work;
static int blink_steps;

static struct k_work_delayable vibrate_work;

static struct k_work_delayable melody_work;
static const struct device *melody_dev;
static const struct hw_note *melody_notes;
static size_t melody_len, melody_pos;
static uint8_t melody_volume;

static const struct device *sink_dev(enum hw_sink sink)
{
	return sink == HW_EARPIECE ? earpiece : speaker;
}

int hw_display_power(bool on)
{
	int ret = lcd_set_power(lcd, on);

	if (ret == 0) {
		display_on = on;
		/* The controller is cleared on power-up; resend everything. */
		memset(shadow, 0, sizeof(shadow));
	}
	return ret;
}

bool hw_display_is_on(void)
{
	return display_on;
}

void hw_display_contrast(uint8_t value)
{
	lcd_set_contrast(lcd, value);
}

void hw_display_flush(struct gfx *g)
{
	int first, count;

	if (!gfx_take_dirty(g, &first, &count) || !display_on) {
		return;
	}
	/* Send runs of changed rows. */
	for (int y = first; y < first + count;) {
		if (memcmp(&shadow[y * LCD_STRIDE], &g->fb[y * LCD_STRIDE], LCD_STRIDE) == 0) {
			y++;
			continue;
		}
		int run = y;

		while (run < first + count &&
		       memcmp(&shadow[run * LCD_STRIDE], &g->fb[run * LCD_STRIDE], LCD_STRIDE)) {
			run++;
		}
		lcd_write_rows(lcd, g->fb, y, run - y);
		memcpy(&shadow[y * LCD_STRIDE], &g->fb[y * LCD_STRIDE], (run - y) * LCD_STRIDE);
		y = run;
	}
}

void hw_backlight(uint8_t percent)
{
	k_work_cancel_delayable(&blink_work);
	backlight_level = MIN(percent, 100);
	led_set_brightness_dt(&backlight, backlight_level);
}

static void blink_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (blink_steps-- > 0) {
		led_set_brightness_dt(&backlight, (blink_steps & 1) ? 100 : 0);
		k_work_schedule(&blink_work, K_MSEC(250));
	} else {
		led_set_brightness_dt(&backlight, backlight_level);
	}
}

void hw_backlight_blink(int n)
{
	blink_steps = 2 * n;
	k_work_reschedule(&blink_work, K_NO_WAIT);
}

static void vibrate_off(struct k_work *work)
{
	ARG_UNUSED(work);
	led_off_dt(&vibration);
}

void hw_vibrate(uint32_t ms)
{
	led_on_dt(&vibration);
	k_work_reschedule(&vibrate_work, K_MSEC(ms));
}

void hw_tone(enum hw_sink sink, uint32_t freq_hz, uint32_t ms, uint8_t volume)
{
	audio_out_tone(sink_dev(sink), freq_hz, ms, volume);
}

static void melody_step(struct k_work *work)
{
	ARG_UNUSED(work);

	if (melody_pos >= melody_len) {
		return;
	}
	const struct hw_note *n = &melody_notes[melody_pos++];

	/* Short gap between notes so repeated notes stay distinct. */
	audio_out_tone(melody_dev, n->freq, n->ms > 20 ? n->ms - 15 : n->ms, melody_volume);
	k_work_schedule(&melody_work, K_MSEC(n->ms));
}

void hw_melody(enum hw_sink sink, const struct hw_note *notes, size_t count, uint8_t volume)
{
	k_work_cancel_delayable(&melody_work);
	melody_dev = sink_dev(sink);
	melody_notes = notes;
	melody_len = count;
	melody_pos = 0;
	melody_volume = volume;
	k_work_schedule(&melody_work, K_NO_WAIT);
}

void hw_audio_stop(void)
{
	k_work_cancel_delayable(&melody_work);
	audio_out_stop(speaker);
	audio_out_stop(earpiece);
}

FUNC_NORETURN void hw_power_off(void)
{
	hw_audio_stop();
	led_off_dt(&vibration);
	hw_backlight(0);
	hw_display_power(false);

#if defined(CONFIG_CMDZ5_EMUL)
	frontpanel_power_off();
#elif defined(HAVE_PMIC)
	LOG_INF("entering ship mode");
	log_flush();
	int ret = mfd_npm13xx_reg_write(pmic, NPM1300_SHIP_BASE, NPM1300_TASK_ENTER_SHIP_MODE, 1);

	/* Still running: the write failed, or USB power keeps the PMIC awake. */
	k_msleep(2000);
	LOG_WRN("ship mode not entered (%d), restarting", ret);
	log_flush();
	sys_reboot(SYS_REBOOT_COLD);
#endif
	for (;;) {
		k_sleep(K_FOREVER);
	}
}

int hw_init(void)
{
	const struct device *devs[] = {lcd, speaker, earpiece, backlight.dev, vibration.dev};

	for (size_t i = 0; i < ARRAY_SIZE(devs); i++) {
		if (!device_is_ready(devs[i])) {
			LOG_ERR("device %s not ready", devs[i]->name);
			return -ENODEV;
		}
	}
	k_work_init_delayable(&blink_work, blink_handler);
	k_work_init_delayable(&vibrate_work, vibrate_off);
	k_work_init_delayable(&melody_work, melody_step);
	return 0;
}
