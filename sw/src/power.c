#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "config.h"
#include "hw.h"
#include "power.h"
#include "ui/ui.h"

LOG_MODULE_REGISTER(power, CONFIG_CMDZ5_APP_LOG_LEVEL);

enum power_state {
	POWER_ACTIVE,
	POWER_DIM,
	POWER_OFF,
};

static enum power_state state = POWER_OFF;
static int64_t last_activity;
static bool lid_closed;

static void enter(enum power_state s)
{
	if (s == state) {
		return;
	}
	LOG_DBG("state %d -> %d", state, s);

	if (s == POWER_OFF) {
		hw_backlight(0);
		hw_display_power(false);
	} else {
		if (state == POWER_OFF) {
			hw_display_power(true);
			hw_display_contrast(app_config.contrast);
			ui_invalidate();
		}
		hw_backlight(s == POWER_ACTIVE ? app_config.backlight_level : 0);
	}
	state = s;
}

void power_init(void)
{
	last_activity = k_uptime_get();
	enter(POWER_ACTIVE);
}

bool power_user_activity(bool keypad)
{
	bool was_off = state == POWER_OFF;

	if (keypad && lid_closed) {
		/* Pressed through the closed lid: ignore without waking. */
		return true;
	}
	last_activity = k_uptime_get();
	enter(POWER_ACTIVE);
	return was_off;
}

void power_lid(bool closed)
{
	lid_closed = closed;
	last_activity = k_uptime_get();
	if (closed) {
		ui_home();
		if (state == POWER_ACTIVE) {
			enter(POWER_DIM);
		}
	} else {
		enter(POWER_ACTIVE);
	}
}

void power_toggle_display(void)
{
	last_activity = k_uptime_get();
	enter(state == POWER_OFF ? POWER_ACTIVE : POWER_OFF);
}

void power_settings_changed(void)
{
	if (state == POWER_ACTIVE) {
		hw_backlight(app_config.backlight_level);
	}
}

void power_update(void)
{
	int64_t idle = k_uptime_get() - last_activity;

	if (state == POWER_ACTIVE && idle >= app_config.backlight_timeout_s * 1000LL) {
		enter(POWER_DIM);
	}
	if (state != POWER_OFF && app_config.screen_timeout_s &&
	    idle >= app_config.screen_timeout_s * 1000LL) {
		enter(POWER_OFF);
	}
}

bool power_display_on(void)
{
	return state != POWER_OFF;
}
