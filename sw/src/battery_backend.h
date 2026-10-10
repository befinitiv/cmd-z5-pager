/*
 * Measurement and fuel gauge behind battery.c: the nPM1300 on the board
 * (battery_npm13xx.c), a simulated battery in the emulator (emul/).
 */

#pragma once

#include "battery.h"

struct battery_sample {
	int voltage_mv;
	int current_ua; /* positive = drawn from the battery */
	int temp_dc;
	bool usb;
	enum battery_charge charge;
};

struct battery_gauge {
	int soc;     /* %, -1 = unknown */
	int tte_min; /* -1 = unknown */
	int ttf_min; /* -1 = unknown */
};

int battery_backend_init(void);
int battery_backend_read(struct battery_sample *s);
/* Feed a sample taken dt_ms after the previous one into the fuel gauge. */
void battery_backend_gauge(const struct battery_sample *s, uint32_t dt_ms,
			   struct battery_gauge *out);
void battery_backend_save(void);
