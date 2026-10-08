/*
 * Behavioural model of the CMD-Z5 display controller, built from what is
 * documented in ../display/DOCUMENTATION.md. Plain C, no Zephyr dependencies.
 *
 * Anything the documentation does not confirm is reported through the warn
 * callback (e.g. more pixel bytes than a row holds, unknown registers), so the
 * emulator catches driver bugs that the real display might silently accept or
 * reject.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LCD_MODEL_W      96
#define LCD_MODEL_H      72
#define LCD_MODEL_STRIDE 24
#define LCD_MODEL_NREGS  0x26

struct lcd_model {
	uint8_t regs[LCD_MODEL_NREGS];
	uint8_t fb[LCD_MODEL_STRIDE * LCD_MODEL_H];
	int cur_reg;    /* -1: no index written yet */
	int data_count; /* data bytes written since the last index */
	bool in_reset;
	uint32_t pixel_writes;
	void (*warn)(const char *msg, int reg, int value);
};

void lcd_model_init(struct lcd_model *m, void (*warn)(const char *msg, int reg, int value));
void lcd_model_reset(struct lcd_model *m, bool asserted);
void lcd_model_write(struct lcd_model *m, bool a0, uint8_t value);

static inline bool lcd_model_display_on(const struct lcd_model *m)
{
	return !m->in_reset && (m->regs[0x00] & 0x40);
}

static inline uint8_t lcd_model_contrast(const struct lcd_model *m)
{
	return m->regs[0x23];
}
