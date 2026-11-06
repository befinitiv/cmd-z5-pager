/*
 * Settings menus. Values are applied immediately and saved when changed.
 */

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#include "../clock.h"
#include "../config.h"
#include "../factory.h"
#include "../gps.h"
#include "../hw.h"
#include "../i18n.h"
#include "../mesh/mesh.h"
#include "../textin.h"
#include "../update.h"
#include "meshtastic/channel.h"
#include "../power.h"
#include "screens.h"

/* 0 = never */
static const uint8_t backlight_timeouts[] = {5, 10, 20, 30, 60, 0};
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
		if (app_config.backlight_timeout_s == 0) {
			return tr("never");
		}
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

/* The other protocol, after a restart. */
static void switch_protocol(void)
{
	mesh_set_protocol(mesh_protocol() == MESH_PROTO_MESHCORE ? MESH_PROTO_MESHTASTIC
								 : MESH_PROTO_MESHCORE);
	log_flush();
	sys_reboot(SYS_REBOOT_COLD);
}

static void protocol_action(void)
{
	ui_confirm("Protocol",
		   mesh_protocol() == MESH_PROTO_MESHCORE
			   ? "Switch to Meshtastic? The device restarts. CALL = switch, C = cancel."
			   : "Switch to MeshCore? The device restarts. CALL = switch, C = cancel.",
		   switch_protocol);
}

static const char *protocol_name(void)
{
	return mesh_protocol() == MESH_PROTO_MESHCORE ? "MeshCore" : "Meshtastic";
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
	case 5:
		mesh_set_relay(!mesh_relay());
		ui_invalidate();
		return;
	case 7:
		protocol_action();
		return;
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
	case 5:
		return tr(mesh_relay() ? "on" : "off");
	case 7:
		return protocol_name();
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
	{"Relay packets", NULL, mesh_action},
	{"Channels", &channels_menu.screen, NULL},
	{"Protocol", NULL, mesh_action},
};

/* MeshCore: its presets, a region (scope) for floods, adverts. */
static void scope_done(const char *text)
{
	struct mesh_mc_config c;
	size_t n = 0;

	mesh_mc_get_config(&c);
	/* "#de-sh" or "DE-SH" as "de-sh": regions are lower case. */
	for (const char *p = text[0] == '#' ? text + 1 : text; *p && n < sizeof(c.scope) - 1;
	     p++) {
		if (*p != ' ') {
			c.scope[n++] = *p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p;
		}
	}
	c.scope[n] = '\0';
	mesh_mc_set_config(&c);
}

static void mc_action(struct ui_menu *m, int idx)
{
	struct mesh_mc_config c;
	struct mesh_config mc;

	ARG_UNUSED(m);
	mesh_mc_get_config(&c);
	mesh_get_config(&mc);
	switch (idx) {
	case 0:
		compose_open_edit("Name", mc.long_name, sizeof(mc.long_name) - 1, name_done);
		return;
	case 1:
		c.preset = (c.preset + 1) % mesh_mc_presets();
		mesh_mc_set_config(&c);
		break;
	case 2:
		compose_open_edit("Region", c.scope, sizeof(c.scope) - 1, scope_done);
		return;
	case 4:
		mesh_send_advert();
		ui_toast("Advert", "Sent to the mesh.", NULL, NULL);
		return;
	case 6:
		protocol_action();
		return;
	default:
		return;
	}
	ui_invalidate();
}

static const char *mc_value(struct ui_menu *m, int idx, char *buf, size_t len)
{
	struct mesh_mc_config c;
	struct mesh_config mc;

	ARG_UNUSED(m);
	mesh_mc_get_config(&c);
	switch (idx) {
	case 0:
		mesh_get_config(&mc);
		snprintf(buf, len, "%s", mc.long_name);
		return buf;
	case 1:
		return mesh_mc_preset_name(c.preset);
	case 2:
		snprintf(buf, len, "%s", c.scope[0] ? c.scope : "-");
		return buf;
	case 3:
		snprintf(buf, len, "%08x", mesh_my_num());
		return buf;
	case 6:
		return protocol_name();
	default:
		return NULL;
	}
}

static const struct ui_menu_item mc_items[] = {
	{"Name", NULL, mc_action},
	{"Preset", NULL, mc_action},
	{"Region", NULL, mc_action},
	{"Node", NULL, mc_action},
	{"Send advert", NULL, mc_action},
	{"Channels", &channels_menu.screen, NULL},
	{"Protocol", NULL, mc_action},
};

/* The items of the protocol in use. */
static void mesh_enter(struct ui_screen *s)
{
	struct ui_menu *m = CONTAINER_OF(s, struct ui_menu, screen);
	bool mc = mesh_protocol() == MESH_PROTO_MESHCORE;

	m->items = mc ? mc_items : mesh_items;
	m->count = mc ? ARRAY_SIZE(mc_items) : ARRAY_SIZE(mesh_items);
	m->value = mc ? mc_value : mesh_value;
}

struct ui_menu mesh_menu = {
	.screen = {.title = "Mesh", .enter = mesh_enter, .event = ui_menu_event,
		   .draw = ui_menu_draw},
	.items = mesh_items,
	.count = ARRAY_SIZE(mesh_items),
	.value = mesh_value,
};

/* Position ------------------------------------------------------------------ */

/* GPS mode choices: off, fixed, periodic with these intervals, always. */
static const uint16_t gps_intervals[] = {5, 15, 30, 60};
#define GPS_CHOICES (2 + ARRAY_SIZE(gps_intervals) + 1)

static int gps_choice(const struct gps_config *c)
{
	switch (c->mode) {
	case GPS_MODE_FIXED:
		return 1;
	case GPS_MODE_PERIODIC:
		for (size_t i = 0; i < ARRAY_SIZE(gps_intervals); i++) {
			if (gps_intervals[i] == c->interval_min) {
				return 2 + i;
			}
		}
		return 3;
	case GPS_MODE_ALWAYS:
		return GPS_CHOICES - 1;
	default:
		return 0;
	}
}

static void gps_apply_choice(struct gps_config *c, int choice)
{
	if (choice == 0) {
		c->mode = GPS_MODE_OFF;
	} else if (choice == 1) {
		c->mode = GPS_MODE_FIXED;
	} else if (choice == GPS_CHOICES - 1) {
		c->mode = GPS_MODE_ALWAYS;
	} else {
		c->mode = GPS_MODE_PERIODIC;
		c->interval_min = gps_intervals[choice - 2];
	}
}

/* Degrees with up to 7 decimals ("-33.8568") to degrees * 1e7. */
static const char *parse_coord(const char *p, int32_t *out)
{
	int64_t v = 0, scale = 10000000;
	bool neg = false, digits = false;

	while (*p == ' ' || *p == ',' || *p == ';') {
		p++;
	}
	if (*p == '-' || *p == '+') {
		neg = *p++ == '-';
	}
	for (; *p >= '0' && *p <= '9'; p++) {
		v = v * 10 + (*p - '0');
		digits = true;
	}
	v *= scale;
	if (*p == '.') {
		for (p++; *p >= '0' && *p <= '9'; p++) {
			scale /= 10;
			v += (*p - '0') * scale;
			digits = true;
		}
	}
	if (!digits || v > 1800000000) {
		return NULL;
	}
	*out = neg ? -v : v;
	return p;
}

static void fixed_done(const char *text)
{
	struct gps_config c;
	int32_t lat, lon;
	const char *p = parse_coord(text, &lat);

	if (!p || !(p = parse_coord(p, &lon)) || lat > 900000000 || lat < -900000000) {
		ui_message("Fixed position", "Enter latitude and longitude in degrees, "
					     "e.g. 52.52 13.405 (south and west negative).");
		return;
	}
	gps_get_config(&c);
	c.has_fixed = true;
	c.fixed_lat_e7 = lat;
	c.fixed_lon_e7 = lon;
	gps_set_config(&c);
}

static void fmt_fixed(char *buf, size_t len, const struct gps_config *c)
{
	snprintf(buf, len, "%s%d.%05d %s%d.%05d", c->fixed_lat_e7 < 0 ? "-" : "",
		 abs(c->fixed_lat_e7) / 10000000, abs(c->fixed_lat_e7) % 10000000 / 100,
		 c->fixed_lon_e7 < 0 ? "-" : "", abs(c->fixed_lon_e7) / 10000000,
		 abs(c->fixed_lon_e7) % 10000000 / 100);
}

static void position_action(struct ui_menu *m, int idx)
{
	static char text[40];
	struct gps_config c;
	struct gps_status st;

	ARG_UNUSED(m);
	gps_get_config(&c);
	switch (idx) {
	case 0:
		gps_apply_choice(&c, (gps_choice(&c) + 1) % GPS_CHOICES);
		break;
	case 1:
		c.share = (c.share + 1) % 3;
		break;
	case 2:
		if (c.has_fixed) {
			fmt_fixed(text, sizeof(text), &c);
		} else {
			text[0] = '\0';
		}
		compose_open_edit("Fixed position", text, sizeof(text) - 1, fixed_done);
		return;
	case 3:
		/* Take the last GPS fix as the fixed position. */
		gps_status(&st);
		if (!st.last.valid) {
			ui_message("Fixed position", "No GPS fix yet.");
			return;
		}
		c.has_fixed = true;
		c.fixed_lat_e7 = st.last.lat_e7;
		c.fixed_lon_e7 = st.last.lon_e7;
		break;
	default:
		return;
	}
	gps_set_config(&c);
	ui_invalidate();
}

static const char *position_value(struct ui_menu *m, int idx, char *buf, size_t len)
{
	static const char *const share_names[] = {"off", "approx.", "exact"};
	struct gps_config c;

	ARG_UNUSED(m);
	gps_get_config(&c);
	switch (idx) {
	case 0:
		switch (c.mode) {
		case GPS_MODE_OFF:
			return tr("off");
		case GPS_MODE_FIXED:
			return tr("fixed");
		case GPS_MODE_ALWAYS:
			return tr("always");
		default:
			snprintf(buf, len, "%u min", c.interval_min);
			return buf;
		}
	case 1:
		return tr(share_names[c.share % 3]);
	case 2:
		return c.has_fixed ? tr("set") : "-";
	default:
		return NULL;
	}
}

static const struct ui_menu_item position_items[] = {
	{"GPS", NULL, position_action},
	{"Share position", NULL, position_action},
	{"Fixed position", NULL, position_action},
	{"Use GPS fix", NULL, position_action},
};

struct ui_menu position_menu = UI_MENU("Position", position_items, position_value);

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

static void zone_next(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);
	ARG_UNUSED(idx);
	clock_set_zone((clock_zone() + 1) % clock_zone_count());
}

static const char *settings_value(struct ui_menu *m, int idx, char *buf, size_t len)
{
	ARG_UNUSED(m);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	switch (idx) {
	case 5:
		return i18n_name(app_config.language);
	case 6:
		return tr(clock_zone_name(clock_zone()));
	default:
		return NULL;
	}
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

static void update_action(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);
	ARG_UNUSED(idx);
	if (!update_available()) {
		ui_toast("Firmware update", "No bootloader in this build.", NULL, NULL);
		return;
	}
	ui_confirm("Firmware update",
		   "Restarts into the bootloader, which waits for new firmware over USB. "
		   "CALL = restart, C = cancel.",
		   update_enter_bootloader);
}

static const struct ui_menu_item settings_items[] = {
	{"Mesh", &mesh_menu.screen, NULL},
	{"Position", &position_menu.screen, NULL},
	{"Display", &display_menu.screen, NULL},
	{"Sound", &sound_menu.screen, NULL},
	{"Text input", &input_menu.screen, NULL},
	{"Language", NULL, language_next},
	{"Time zone", NULL, zone_next},
	{"Factory reset", NULL, reset_action},
	{"Firmware update", NULL, update_action},
};

struct ui_menu settings_menu = UI_MENU("Settings", settings_items, settings_value);
