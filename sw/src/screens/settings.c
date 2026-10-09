/*
 * Settings menus. Values are applied immediately and saved when changed.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "../config.h"
#include "../factory.h"
#include "../hw.h"
#include "../i18n.h"
#include "../mesh/mesh.h"
#include "../textin.h"
#include "meshtastic/channel.h"
#include "../power.h"
#include "screens.h"

static const uint8_t backlight_timeouts[] = {5, 10, 20, 30, 60};
static const uint16_t screen_timeouts[] = {30, 60, 120, 300, 0};

static void saved(void)
{
	config_save();
	power_settings_changed();
	ui_invalidate();
}

/* Display ------------------------------------------------------------------ */

static void contrast_changed(struct ui_slider *s, int v)
{
	ARG_UNUSED(s);
	hw_display_contrast(v);
}

static void contrast_done(struct ui_slider *s, int v, bool accepted)
{
	ARG_UNUSED(s);
	if (accepted) {
		app_config.contrast = v;
		saved();
	}
}

static void backlight_changed(struct ui_slider *s, int v)
{
	ARG_UNUSED(s);
	hw_backlight(v);
}

static void backlight_done(struct ui_slider *s, int v, bool accepted)
{
	ARG_UNUSED(s);
	if (accepted) {
		app_config.backlight_level = v;
	}
	saved();
}

/* Contrast range is a guess around the phone's default of 0x85. */
static struct ui_slider contrast_slider =
	UI_SLIDER("Contrast", 0x60, 0xB0, 1, NULL, contrast_changed, contrast_done);
static struct ui_slider backlight_slider =
	UI_SLIDER("Backlight", 10, 100, 10, "%", backlight_changed, backlight_done);

static int index_of_u8(const uint8_t *arr, int n, int v)
{
	for (int i = 0; i < n; i++) {
		if (arr[i] == v) {
			return i;
		}
	}
	return 0;
}

static int index_of_u16(const uint16_t *arr, int n, int v)
{
	for (int i = 0; i < n; i++) {
		if (arr[i] == v) {
			return i;
		}
	}
	return 0;
}

static void display_action(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);

	switch (idx) {
	case 0:
		ui_slider_open(&contrast_slider, app_config.contrast);
		break;
	case 1:
		ui_slider_open(&backlight_slider, app_config.backlight_level);
		break;
	case 2: {
		int i = index_of_u8(backlight_timeouts, ARRAY_SIZE(backlight_timeouts),
				    app_config.backlight_timeout_s);

		app_config.backlight_timeout_s =
			backlight_timeouts[(i + 1) % ARRAY_SIZE(backlight_timeouts)];
		saved();
		break;
	}
	case 3: {
		int i = index_of_u16(screen_timeouts, ARRAY_SIZE(screen_timeouts),
				     app_config.screen_timeout_s);

		app_config.screen_timeout_s = screen_timeouts[(i + 1) % ARRAY_SIZE(screen_timeouts)];
		saved();
		break;
	}
	default:
		break;
	}
}

static const char *display_value(struct ui_menu *m, int idx, char *buf, size_t len)
{
	ARG_UNUSED(m);

	switch (idx) {
	case 0:
		snprintf(buf, len, "%d", app_config.contrast);
		return buf;
	case 1:
		snprintf(buf, len, "%d%%", app_config.backlight_level);
		return buf;
	case 2:
		snprintf(buf, len, "%ds", app_config.backlight_timeout_s);
		return buf;
	case 3:
		if (app_config.screen_timeout_s == 0) {
			return tr("never");
		}
		snprintf(buf, len, "%ds", app_config.screen_timeout_s);
		return buf;
	default:
		return NULL;
	}
}

static const struct ui_menu_item display_items[] = {
	{"Contrast", NULL, display_action},
	{"Backlight", NULL, display_action},
	{"Light off", NULL, display_action},
	{"Screen off", NULL, display_action},
};

static struct ui_menu display_menu = UI_MENU("Display", display_items, display_value);

/* Sound -------------------------------------------------------------------- */

static void volume_changed(struct ui_slider *s, int v)
{
	ARG_UNUSED(s);
	hw_tone(HW_SPEAKER, 1000, 80, v);
}

static void volume_done(struct ui_slider *s, int v, bool accepted)
{
	ARG_UNUSED(s);
	if (accepted) {
		app_config.volume = v;
		saved();
	}
}

static struct ui_slider volume_slider =
	UI_SLIDER("Volume", 0, 100, 10, "%", volume_changed, volume_done);

static void sound_action(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);

	switch (idx) {
	case 0:
		ui_slider_open(&volume_slider, app_config.volume);
		break;
	case 1:
		app_config.key_click = !app_config.key_click;
		saved();
		break;
	case 2:
		app_config.vibrate = !app_config.vibrate;
		if (app_config.vibrate) {
			hw_vibrate(200);
		}
		saved();
		break;
	default:
		break;
	}
}

static const char *sound_value(struct ui_menu *m, int idx, char *buf, size_t len)
{
	ARG_UNUSED(m);

	switch (idx) {
	case 0:
		snprintf(buf, len, "%d%%", app_config.volume);
		return buf;
	case 1:
		return tr(app_config.key_click ? "on" : "off");
	case 2:
		return tr(app_config.vibrate ? "on" : "off");
	default:
		return NULL;
	}
}

static const struct ui_menu_item sound_items[] = {
	{"Volume", NULL, sound_action},
	{"Key clicks", NULL, sound_action},
	{"Vibration", NULL, sound_action},
};

static struct ui_menu sound_menu = UI_MENU("Sound", sound_items, sound_value);

/* Mesh ---------------------------------------------------------------------- */

static const uint8_t regions[] = {MT_REGION_EU_868, MT_REGION_US, MT_REGION_EU_433};
static const uint8_t presets[] = {
	MT_PRESET_LONG_FAST,  MT_PRESET_LONG_MODERATE, MT_PRESET_LONG_SLOW,
	MT_PRESET_MEDIUM_FAST, MT_PRESET_MEDIUM_SLOW,  MT_PRESET_SHORT_FAST,
	MT_PRESET_SHORT_SLOW,  MT_PRESET_SHORT_TURBO,  MT_PRESET_LONG_TURBO,
};

static int next_in(const uint8_t *arr, int n, uint8_t v)
{
	for (int i = 0; i < n; i++) {
		if (arr[i] == v) {
			return arr[(i + 1) % n];
		}
	}
	return arr[0];
}

static void name_done(const char *text)
{
	struct mesh_config c;

	if (!text[0]) {
		return;
	}
	mesh_get_config(&c);
	snprintf(c.long_name, sizeof(c.long_name), "%s", text);
	mesh_set_config(&c);
}

static void short_done(const char *text)
{
	struct mesh_config c;

	if (!text[0]) {
		return;
	}
	mesh_get_config(&c);
	snprintf(c.short_name, sizeof(c.short_name), "%s", text);
	mesh_set_config(&c);
}

static void mesh_action(struct ui_menu *m, int idx)
{
	struct mesh_config c;

	ARG_UNUSED(m);
	mesh_get_config(&c);
	switch (idx) {
	case 0:
		compose_open_edit("Name", c.long_name, sizeof(c.long_name) - 1, name_done);
		return;
	case 1:
		compose_open_edit("Short name", c.short_name, sizeof(c.short_name) - 1, short_done);
		return;
	case 2:
		c.region = next_in(regions, ARRAY_SIZE(regions), c.region);
		break;
	case 3:
		c.preset = next_in(presets, ARRAY_SIZE(presets), c.preset);
		break;
	default:
		return;
	}
	mesh_set_config(&c);
	ui_invalidate();
}

static const char *mesh_value(struct ui_menu *m, int idx, char *buf, size_t len)
{
	struct mesh_config c;

	ARG_UNUSED(m);
	mesh_get_config(&c);
	switch (idx) {
	case 0:
		snprintf(buf, len, "%s", c.long_name);
		return buf;
	case 1:
		snprintf(buf, len, "%s", c.short_name);
		return buf;
	case 2:
		return mt_region_name(c.region);
	case 3:
		return mt_preset_name(c.preset);
	case 4:
		snprintf(buf, len, "!%08x", mesh_my_num());
		return buf;
	default:
		return NULL;
	}
}

static const struct ui_menu_item mesh_items[] = {
	{"Name", NULL, mesh_action},
	{"Short name", NULL, mesh_action},
	{"Region", NULL, mesh_action},
	{"Preset", NULL, mesh_action},
	{"Node", NULL, mesh_action},
};

struct ui_menu mesh_menu = UI_MENU("Mesh", mesh_items, mesh_value);

/* Top level ---------------------------------------------------------------- */

/* Text input --------------------------------------------------------------- */

static void input_action(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);

	switch (idx) {
	case 0:
		textin_next_lang();
		break;
	case 1:
		app_config.learn_words = !app_config.learn_words;
		saved();
		break;
	case 2:
		textin_reset_learned();
		ui_message("Text input", "Learned words deleted.");
		break;
	default:
		break;
	}
	ui_invalidate();
}

static const char *input_value(struct ui_menu *m, int idx, char *buf, size_t len)
{
	ARG_UNUSED(m);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);

	switch (idx) {
	case 0:
		return i18n_name(textin_lang());
	case 1:
		return tr(app_config.learn_words ? "on" : "off");
	default:
		return NULL;
	}
}

static const struct ui_menu_item input_items[] = {
	{"Language", NULL, input_action},
	{"Learn words", NULL, input_action},
	{"Delete learned", NULL, input_action},
};

static struct ui_menu input_menu = UI_MENU("Text input", input_items, input_value);

static void language_next(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);
	ARG_UNUSED(idx);
	app_config.language = (app_config.language + 1) % LANG_COUNT;
	i18n_set(app_config.language);
	saved();
}

static const char *settings_value(struct ui_menu *m, int idx, char *buf, size_t len)
{
	ARG_UNUSED(m);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return idx == 4 ? i18n_name(app_config.language) : NULL;
}

static void reset_action(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);
	ARG_UNUSED(idx);
	ui_confirm("Factory reset",
		   "All settings, messages, known nodes and learned words are deleted, "
		   "and the device gets a new key. CALL = reset, C = cancel.",
		   factory_reset);
}

static const struct ui_menu_item settings_items[] = {
	{"Mesh", &mesh_menu.screen, NULL},
	{"Display", &display_menu.screen, NULL},
	{"Sound", &sound_menu.screen, NULL},
	{"Text input", &input_menu.screen, NULL},
	{"Language", NULL, language_next},
	{"Factory reset", NULL, reset_action},
};

struct ui_menu settings_menu = UI_MENU("Settings", settings_items, settings_value);
