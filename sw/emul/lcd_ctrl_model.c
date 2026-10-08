#include <string.h>

#include "lcd_ctrl_model.h"

#define REG_COLUMN 0x03
#define REG_ROW    0x04
#define REG_PIXELS 0x0B

static void warn(struct lcd_model *m, const char *msg, int reg, int value)
{
	if (m->warn) {
		m->warn(msg, reg, value);
	}
}

void lcd_model_init(struct lcd_model *m, void (*warn_cb)(const char *msg, int reg, int value))
{
	memset(m, 0, sizeof(*m));
	m->warn = warn_cb;
	m->cur_reg = -1;
	m->in_reset = true;
}

void lcd_model_reset(struct lcd_model *m, bool asserted)
{
	if (asserted) {
		memset(m->regs, 0, sizeof(m->regs));
		m->cur_reg = -1;
		m->data_count = 0;
	}
	m->in_reset = asserted;
}

/* Registers whose data is a 16-byte table; all others take one byte. */
static bool is_table(int reg)
{
	return reg == 0x10 || reg == 0x14;
}

void lcd_model_write(struct lcd_model *m, bool a0, uint8_t value)
{
	if (m->in_reset) {
		warn(m, "write during reset", m->cur_reg, value);
		return;
	}

	if (!a0) {
		if (value >= LCD_MODEL_NREGS || value == 0x16 || value == 0x1F) {
			warn(m, "index of a register the phone never writes", value, value);
		}
		m->cur_reg = value;
		m->data_count = 0;
		return;
	}

	if (m->cur_reg < 0) {
		warn(m, "data without register index", -1, value);
		return;
	}

	int count = m->data_count++;

	if (m->cur_reg == REG_PIXELS) {
		int row = m->regs[REG_ROW];
		int col = m->regs[REG_COLUMN] + count;

		if (row >= LCD_MODEL_H) {
			warn(m, "row address out of range", REG_ROW, row);
			return;
		}
		if (col >= LCD_MODEL_STRIDE) {
			/* Behaviour unknown (auto-increment to the next row?). */
			warn(m, "pixel data beyond end of row", REG_PIXELS, count);
			return;
		}
		m->fb[row * LCD_MODEL_STRIDE + col] = value;
		m->pixel_writes++;
	} else if (is_table(m->cur_reg)) {
		if (count >= 16) {
			warn(m, "table longer than 16 bytes", m->cur_reg, count);
		}
	} else if (m->cur_reg < LCD_MODEL_NREGS) {
		if (count > 0) {
			warn(m, "more than one data byte for a register", m->cur_reg, value);
		}
		m->regs[m->cur_reg] = value;
	}
}
