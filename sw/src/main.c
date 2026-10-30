/*
 * CMD-Z5 firmware: UI thread and event loop.
 *
 * All UI state is owned by this thread. Other threads (input, later the radio
 * services) pass events through app_post().
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#include "app.h"
#include "battery.h"
#include "clock.h"
#include "config.h"
#include "gps.h"
#include "hw.h"
#include "i18n.h"
#include "mesh/mesh.h"
#include "mesh/msgstore.h"
#include "meshtastic/packet.h"
#include "textin.h"
#include "power.h"
#include "screens/screens.h"
#include "update.h"

LOG_MODULE_REGISTER(app, CONFIG_CMDZ5_APP_LOG_LEVEL);

#define POLL_MS 200

static int poll_ms = POLL_MS;

K_MSGQ_DEFINE(ui_queue, sizeof(struct ui_event), 32, 4);

static struct gfx gfx;

/* Startup: the first 7 notes of the Super Mario Bros. theme (E E - E - C E - G - - - G). */
#define SMB_16TH 110 /* ms */
static const struct hw_note startup_tune[] = {
	{1319, SMB_16TH},     {1319, SMB_16TH}, {0, SMB_16TH},     {1319, SMB_16TH},
	{0, SMB_16TH},        {1047, SMB_16TH}, {1319, 2 * SMB_16TH}, {1568, 2 * SMB_16TH},
	{0, 2 * SMB_16TH},    {784, 2 * SMB_16TH},
};

void app_post(const struct ui_event *ev)
{
	if (k_msgq_put(&ui_queue, ev, K_NO_WAIT) != 0) {
		LOG_WRN("UI queue full, event dropped");
	}
}

void app_set_frame_ms(int ms)
{
	poll_ms = ms > 0 ? ms : POLL_MS;
}

static void tick(struct k_timer *timer)
{
	struct ui_event ev = {.type = UI_EV_TICK};

	ARG_UNUSED(timer);
	app_post(&ev);
}

K_TIMER_DEFINE(tick_timer, tick, NULL);

/* Switching off: C major arpeggio from top to bottom (C6 G5 E5 C5). */
static const struct hw_note goodbye_tune[] = {
	{1047, 110}, {784, 110}, {659, 110}, {523, 300},
};
#define GOODBYE_MS (110 + 110 + 110 + 300)

static void render(void)
{
	if (power_display_on() && ui_needs_render()) {
		ui_render(&gfx);
		hw_display_flush(&gfx);
	}
}

static FUNC_NORETURN void switch_off(void)
{
	LOG_INF("switching off");
	k_timer_stop(&tick_timer);
	poweroff_goodbye();
	render();
	hw_melody(HW_SPEAKER, goodbye_tune, ARRAY_SIZE(goodbye_tune), app_config.volume);
	battery_save();
	/* Let the arpeggio finish (and its last note fade out). */
	k_msleep(GOODBYE_MS + 200);
	hw_power_off();
}

/* Key whose press only woke the display; its long/release events are dropped too. */
static enum ui_key swallowed = UI_KEY_NONE;
/* The current power key press only woke the display. */
static bool power_woke;

/* Key click and wheel tick (Settings -> Sound -> Key clicks). */
static void click(const struct ui_event *ev)
{
	if (!app_config.key_click) {
		return;
	}
	if (ev->type == UI_EV_KEY_PRESS) {
		hw_tone(HW_SPEAKER, 2500, 6, app_config.volume / 4);
	} else if (ev->type == UI_EV_WHEEL) {
		/* Subtler than a key: shorter, higher and quieter. */
		hw_tone(HW_SPEAKER, 4000, 3, app_config.volume / 5);
	}
}

/* Keys under the lid, as opposed to the side controls (wheel, power key). */
static bool is_keypad(const struct ui_event *ev)
{
	return ev->type != UI_EV_WHEEL && ev->key != UI_KEY_SELECT && ev->key != UI_KEY_BACK &&
	       ev->key != UI_KEY_DOWN && ev->key != UI_KEY_POWER;
}

static bool is_user_input(const struct ui_event *ev)
{
	return ev->type == UI_EV_KEY_PRESS || ev->type == UI_EV_KEY_LONG ||
	       ev->type == UI_EV_KEY_RELEASE || ev->type == UI_EV_WHEEL;
}

#define TOAST_MS 8000

static int64_t toast_until;

static void open_conversation(void *arg)
{
	chat_open((uint32_t)(uintptr_t)arg);
}

/* New message: a pop-up, unless the home screen or that chat shows it anyway. */
static void show_message_toast(void)
{
	static struct msg m;
	char who[24], title[40];
	bool have = false;

	msgstore_lock();
	if (msgstore_newest_unread()) {
		m = *msgstore_newest_unread();
		have = true;
	}
	msgstore_unlock();
	if (!have || ui_top() == &home_screen || chat_showing(m.conv)) {
		return;
	}
	mesh_node_name(m.from, who, sizeof(who));
	if (conv_channel(m.conv) >= 0) {
		char chan[16];

		mesh_conv_name(m.conv, chan, sizeof(chan));
		snprintf(title, sizeof(title), "%s %s", who, chan);
	} else {
		snprintf(title, sizeof(title), "%s", who);
	}
	ui_toast(title, m.text, open_conversation, (void *)(uintptr_t)m.conv);
	toast_until = k_uptime_get() + TOAST_MS;
}

static const struct hw_note message_tune[] = {
	{1568, 80}, {2093, 120},
};

/* A sent message failed: two low, falling tones. */
static const struct hw_note failed_tune[] = {
	{392, 150}, {0, 60}, {294, 350},
};

static void handle(const struct ui_event *ev)
{
	if (ev->type == APP_EV_LID) {
		power_lid(ev->value);
		return;
	}
	if (ev->type == APP_EV_MESH) {
		if (ev->value == MESH_EV_MESSAGE) {
			power_user_activity(false);
			hw_melody(HW_SPEAKER, message_tune, ARRAY_SIZE(message_tune),
				  app_config.volume);
			if (app_config.vibrate) {
				hw_vibrate(300);
			}
			show_message_toast();
		} else if (ev->value == MESH_EV_FAILED) {
			hw_melody(HW_SPEAKER, failed_tune, ARRAY_SIZE(failed_tune),
				  app_config.volume);
			if (app_config.vibrate) {
				hw_vibrate(300);
			}
		}
		ui_invalidate();
		return;
	}
	if (ev->type == UI_EV_TICK) {
		if (power_display_on()) {
			ui_dispatch(ev);
		}
		return;
	}
	if (!is_user_input(ev)) {
		return;
	}

	if (ev->type != UI_EV_KEY_PRESS && ev->type != UI_EV_WHEEL && ev->key == swallowed) {
		if (ev->type == UI_EV_KEY_RELEASE) {
			swallowed = UI_KEY_NONE;
		}
		return;
	}

	if (ev->key == UI_KEY_POWER) {
		if (ev->type == UI_EV_KEY_PRESS) {
			/* Like any key: a press that only wakes the display does nothing else. */
			power_woke = power_user_activity(false);
			if (!power_woke) {
				click(ev);
			}
		} else if (ui_ev_short(ev, UI_KEY_POWER)) {
			if (!power_woke) {
				ui_home();
			}
		} else if (ui_ev_long(ev, UI_KEY_POWER)) {
			power_user_activity(false);
			poweroff_show();
		} else if (ev->type == UI_EV_KEY_RELEASE) {
			/* Released after a long press. */
			switch_off();
		}
		return;
	}

	if (power_user_activity(is_keypad(ev))) {
		if (ev->type == UI_EV_KEY_PRESS) {
			swallowed = ev->key;
		}
		return;
	}

	click(ev);
	ui_dispatch(ev);
}

/*
 * Switched off while on USB power: the PMIC cannot stay in ship mode, so the
 * device only looks off. Display, mesh and GPS stay off, the battery charges.
 * Holding the power key switches on (a restart); removing USB enters ship
 * mode.
 */
static FUNC_NORETURN void stay_off(void)
{
	struct ui_event ev;

	LOG_INF("off (USB power): hold the power key to switch on");
	hw_display_power(false);
	hw_backlight(0);
	for (;;) {
		if (k_msgq_get(&ui_queue, &ev, K_MSEC(500)) == 0 && ui_ev_long(&ev, UI_KEY_POWER)) {
			LOG_INF("switching on");
			log_flush();
			sys_reboot(SYS_REBOOT_COLD);
		}
		if (!hw_usb_power()) {
			hw_power_off();
		}
	}
}

int main(void)
{
	struct ui_event ev;

	LOG_INF("CMD-Z5 firmware %s", APP_VERSION);

	if (hw_init() != 0) {
		LOG_ERR("hardware init failed");
		return 0;
	}
	if (hw_boot_off()) {
		stay_off();
	}
	config_init();
	hw_display_contrast(app_config.contrast);
	i18n_set(app_config.language);
	if (textin_init() != 0) {
		LOG_ERR("predictive text not available");
	}
	mesh_init();
	gps_init();
	update_init();
	battery_init();
	clock_init();
	gfx_init(&gfx);
	ui_init(statusbar_draw);
	ui_set_translator(tr);
	ui_set_root(&home_screen);
	power_init();

	hw_melody(HW_SPEAKER, startup_tune, ARRAY_SIZE(startup_tune), app_config.volume);
	k_timer_start(&tick_timer, K_SECONDS(1), K_SECONDS(1));

	for (;;) {
		if (k_msgq_get(&ui_queue, &ev, K_MSEC(poll_ms)) == 0) {
			handle(&ev);
			/* Drain queued events before drawing, so the wheel stays responsive. */
			while (k_msgq_get(&ui_queue, &ev, K_NO_WAIT) == 0) {
				handle(&ev);
			}
		}
		power_update();
		if (toast_until && k_uptime_get() >= toast_until) {
			toast_until = 0;
			ui_toast_hide();
		}
		ui_poll();
		render();
	}
	return 0;
}
