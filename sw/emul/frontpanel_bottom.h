/*
 * Interface between the Zephyr side of the emulator front panel
 * (frontpanel.c) and its host side (frontpanel_bottom.c), which is built
 * against the host C library and SDL2. Only standard C types are allowed
 * here.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum fp_button {
	FP_BTN_1,
	FP_BTN_2,
	FP_BTN_3,
	FP_BTN_4,
	FP_BTN_5,
	FP_BTN_6,
	FP_BTN_7,
	FP_BTN_8,
	FP_BTN_9,
	FP_BTN_0,
	FP_BTN_STAR,
	FP_BTN_POUND,
	FP_BTN_CALL,
	FP_BTN_MEMO,
	FP_BTN_CLEAR,
	FP_BTN_WHEEL_PUSH,
	FP_BTN_WHEEL_UP,   /* wheel bent up */
	FP_BTN_WHEEL_DOWN, /* wheel bent down */
	FP_BTN_POWER,
	FP_BTN_COUNT,
};

enum fp_event_type {
	FP_EV_BUTTON, /* button, value 1 = pressed, 0 = released */
	FP_EV_WHEEL,  /* value = detents, positive = rotated up */
	FP_EV_LID,    /* value 1 = closed */
};

struct fp_event {
	uint8_t type;
	uint8_t button;
	int32_t value;
};

typedef void (*fp_event_cb_t)(const struct fp_event *ev);

#define FP_LCD_W      96
#define FP_LCD_H      72
#define FP_LCD_STRIDE 24

struct fp_state {
	uint8_t fb[FP_LCD_STRIDE * FP_LCD_H];
	bool lcd_on;
	uint8_t contrast;
	uint8_t backlight; /* 0..100 */
	uint8_t vibration; /* 0..100 */
	bool speaker_active;
	bool earpiece_active;
	bool lid_closed;
	uint32_t frame; /* increments with every render, drives animations */
};

/* Create the window and open the audio device. SDL video is already initialised. */
int fp_bottom_init(int scale, fp_event_cb_t cb);
void fp_bottom_render(const struct fp_state *st);

/* Audio: mono signed 16-bit samples at fp_bottom_audio_rate(). */
int fp_bottom_audio_rate(void);
void fp_bottom_audio_queue(const int16_t *samples, int count);
void fp_bottom_audio_clear(void);

/* Scripted input (--script): inject an event as if the user caused it. */
void fp_bottom_inject(const struct fp_event *ev);
/* Save the whole window as BMP after the next render. */
void fp_bottom_request_shot(const char *path);
bool fp_bottom_shot_pending(void);
/* Read a host file into buf (NUL-terminated). Returns the length or -1. */
long fp_bottom_read_file(const char *path, char *buf, long size);

/* Host wall-clock time in seconds since 1970 (UTC). */
int64_t fp_bottom_unix_time(void);
/* Host time zone offset to UTC in seconds. */
int32_t fp_bottom_utc_offset(void);

#ifdef __cplusplus
}
#endif
