/*
 * User settings, stored with the Zephyr settings subsystem.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct app_config {
	uint8_t contrast;            /* display register 0x23/0x24 */
	uint8_t backlight_level;     /* percent */
	uint8_t backlight_timeout_s; /* 0 = never */
	uint16_t screen_timeout_s;   /* 0 = never */
	uint8_t volume;              /* percent */
	bool key_click;
	bool vibrate;
	uint8_t language;            /* enum lang, menu */
	uint8_t input_lang;          /* enum lang, predictive text */
	bool learn_words;
};

extern struct app_config app_config;

/* Load the stored settings (defaults if none). */
void config_init(void);
void config_save(void);
