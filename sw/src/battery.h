/*
 * Battery: voltage, current, charging and the state of charge.
 *
 * On the board the nPM1300 measures voltage, current and temperature and
 * Nordic's fuel gauge library estimates the state of charge and the times to
 * empty and full. The emulator simulates a battery instead. A thread samples
 * once a second and keeps averages of the current.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum battery_charge {
	BATTERY_DISCHARGING, /* no USB power */
	BATTERY_CHARGING,
	BATTERY_FULL,        /* USB power, charging complete or not needed */
	BATTERY_ERROR,       /* charger reports an error */
};

struct battery_status {
	bool valid; /* false until the first sample, or without a PMIC */
	bool usb;   /* USB power present */
	enum battery_charge charge;
	int voltage_mv;
	int current_ua;  /* latest sample; positive = drawn from the battery */
	int avg_min_ua;  /* average over the last minute */
	int avg_hour_ua; /* average over the last hour (or since the start) */
	int temp_dc;     /* temperature in 0.1 °C (PMIC die, the battery has no NTC) */
	int soc;         /* state of charge in %, -1 = unknown */
	int tte_min;     /* time to empty in minutes, -1 = unknown */
	int ttf_min;     /* time to full in minutes, -1 = unknown */
	uint32_t drawn_uah; /* charge drawn from the battery since the start */
	uint32_t uptime_s;  /* time covered by the samples */
};

/* Start sampling. Call after settings are available. */
void battery_init(void);
void battery_get(struct battery_status *out);
/* Store the fuel gauge state; call before switching off. */
void battery_save(void);
