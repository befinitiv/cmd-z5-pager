/*
 * Translates Zephyr input events (keypad matrix, wheel, lid, power key, or the
 * emulator front panel) into UI events, with long-press detection.
 */

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include <dt-bindings/cmdz5/keys.h>

#include "app.h"

#define LONG_PRESS_MS 600
/* Longer for the power key, which switches the device off. */
#define POWER_LONG_PRESS_MS 1500

static enum ui_key held_key = UI_KEY_NONE;
static bool long_sent;
static struct k_work_delayable long_work;

static enum ui_key map_key(uint16_t code)
{
	switch (code) {
	case INPUT_KEY_0: return UI_KEY_0;
	case INPUT_KEY_1: return UI_KEY_1;
	case INPUT_KEY_2: return UI_KEY_2;
	case INPUT_KEY_3: return UI_KEY_3;
	case INPUT_KEY_4: return UI_KEY_4;
	case INPUT_KEY_5: return UI_KEY_5;
	case INPUT_KEY_6: return UI_KEY_6;
	case INPUT_KEY_7: return UI_KEY_7;
	case INPUT_KEY_8: return UI_KEY_8;
	case INPUT_KEY_9: return UI_KEY_9;
	case CMDZ5_KEY_STAR: return UI_KEY_STAR;
	case CMDZ5_KEY_POUND: return UI_KEY_POUND;
	case CMDZ5_KEY_CALL: return UI_KEY_CALL;
	case CMDZ5_KEY_MEMO: return UI_KEY_MEMO;
	case INPUT_KEY_BACKSPACE: return UI_KEY_CLEAR;
	case INPUT_KEY_ENTER: return UI_KEY_SELECT;
	case INPUT_KEY_PAGEUP: return UI_KEY_BACK;
	case INPUT_KEY_PAGEDOWN: return UI_KEY_DOWN;
	case INPUT_KEY_POWER: return UI_KEY_POWER;
	default: return UI_KEY_NONE;
	}
}

static void post(uint8_t type, uint8_t key, int16_t value)
{
	struct ui_event ev = {.type = type, .key = key, .value = value};

	app_post(&ev);
}

static void long_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (held_key != UI_KEY_NONE && !long_sent) {
		long_sent = true;
		post(UI_EV_KEY_LONG, held_key, 0);
	}
}

static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type == INPUT_EV_REL && evt->code == INPUT_REL_WHEEL) {
		post(UI_EV_WHEEL, UI_KEY_NONE, evt->value);
		return;
	}
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	if (evt->code == CMDZ5_KEY_LID) {
		post(APP_EV_LID, UI_KEY_NONE, evt->value);
		return;
	}

	enum ui_key key = map_key(evt->code);

	if (key == UI_KEY_NONE) {
		return;
	}
	if (evt->value) {
		/* Only one key is tracked for long presses: the last one pressed. */
		held_key = key;
		long_sent = false;
		k_work_reschedule(&long_work, K_MSEC(key == UI_KEY_POWER ? POWER_LONG_PRESS_MS
									  : LONG_PRESS_MS));
		post(UI_EV_KEY_PRESS, key, 0);
	} else {
		bool was_long = false;

		if (key == held_key) {
			k_work_cancel_delayable(&long_work);
			was_long = long_sent;
			held_key = UI_KEY_NONE;
		}
		post(UI_EV_KEY_RELEASE, key, was_long);
	}
}
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

static int input_init(void)
{
	k_work_init_delayable(&long_work, long_handler);
	return 0;
}
SYS_INIT(input_init, APPLICATION, 0);
