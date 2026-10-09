/*
 * CMD-Z5 firmware: UI thread and event loop.
 *
 * All UI state is owned by this thread. Other threads (input, later the radio
 * services) pass events through app_post().
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "clock.h"
#include "config.h"
#include "hw.h"
#include "i18n.h"
#include "mesh/mesh.h"
#include "mesh/msgstore.h"
#include "meshtastic/packet.h"
#include "textin.h"
#include "power.h"
#include "screens/screens.h"

LOG_MODULE_REGISTER(app, CONFIG_CMDZ5_APP_LOG_LEVEL);

#define POLL_MS 200

K_MSGQ_DEFINE(ui_queue, sizeof(struct ui_event), 32, 4);

static struct gfx gfx;

static const struct hw_note startup_tune[] = {
	{1047, 90}, {1319, 90}, {1568, 90}, {2093, 160},
};

void app_post(const struct ui_event *ev)
{
	if (k_msgq_put(&ui_queue, ev, K_NO_WAIT) != 0) {
		LOG_WRN("UI queue full, event dropped");
	}
}

static void tick(struct k_timer *timer)
{
	struct ui_event ev = {.type = UI_EV_TICK};

	ARG_UNUSED(timer);
	app_post(&ev);
}

K_TIMER_DEFINE(tick_timer, tick, NULL);

static const struct hw_note goodbye_tune[] = {
	{2093, 90}, {1568, 90}, {1319, 90}, {1047, 200},
};

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
	k_msleep(1000);
	hw_power_off();
}

/* Key whose press only woke the display; its long/release events are dropped too. */
static enum ui_key swallowed = UI_KEY_NONE;

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
	if (m.conv == CONV_CHANNEL) {
		snprintf(title, sizeof(title), "%s #%s", who, mesh_channel_name());
	} else {
		snprintf(title, sizeof(title), "%s", who);
	}
	ui_toast(title, m.text, open_conversation, (void *)(uintptr_t)m.conv);
	toast_until = k_uptime_get() + TOAST_MS;
}

static const struct hw_note message_tune[] = {
	{1568, 80}, {2093, 120},
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
		if (ui_ev_short(ev, UI_KEY_POWER)) {
			power_toggle_display();
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

	if (ev->type == UI_EV_KEY_PRESS && app_config.key_click) {
		hw_tone(HW_SPEAKER, 2500, 6, app_config.volume / 2);
	}
	ui_dispatch(ev);
}

int main(void)
{
	struct ui_event ev;

	LOG_INF("CMD-Z5 firmware %s", APP_VERSION);

	if (hw_init() != 0) {
		LOG_ERR("hardware init failed");
		return 0;
	}
	config_init();
	i18n_set(app_config.language);
	if (textin_init() != 0) {
		LOG_ERR("predictive text not available");
	}
	mesh_init();
	clock_init();
	gfx_init(&gfx);
	ui_init(statusbar_draw);
	ui_set_translator(tr);
	ui_set_root(&home_screen);
	power_init();

	hw_melody(HW_SPEAKER, startup_tune, ARRAY_SIZE(startup_tune), app_config.volume);
	k_timer_start(&tick_timer, K_SECONDS(1), K_SECONDS(1));

	for (;;) {
		if (k_msgq_get(&ui_queue, &ev, K_MSEC(POLL_MS)) == 0) {
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
