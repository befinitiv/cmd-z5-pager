/*
 * Fuel gauge for the cell in the phone: counts the measured current and
 * corrects the count with the open-circuit voltage when the battery is
 * nearly at rest. Plain C (host tests in tests/gauge/), used by the board
 * backend and by the emulator's simulated battery.
 *
 * The cell is a 603450 LiPo (3.7 V, charged to 4.2 V, 1250 mAh nominal,
 * 1100 mAh typical, with protection circuit), wired to J8: assumed 1100 mAh
 * usable, about 200 mOhm internal resistance with the protection circuit
 * and wires, and a typical LiPo open-circuit voltage curve. These are
 * assumptions, not a measured profile.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum gauge_charger {
	GAUGE_DISCHARGING, /* no USB power */
	GAUGE_IDLE,        /* USB power, charger not charging (not yet, or paused) */
	GAUGE_CHARGE_CC,   /* constant current (and trickle) */
	GAUGE_CHARGE_CV,   /* constant voltage, the current tapers off */
	GAUGE_FULL,        /* the charger reports charging complete */
};

struct gauge_params {
	int32_t capacity_uah;
	int32_t r_mohm;    /* internal resistance */
	int32_t charge_ua; /* charger current in the constant-current phase */
};

struct gauge {
	struct gauge_params p;
	int64_t charge_uams; /* remaining charge, µA·ms */
	int32_t avg_ua;      /* filtered current, positive = discharging */
	uint32_t avg_ms;     /* time covered by avg_ua, up to its time constant */
	bool charging;       /* avg_ua is a charge current: restarts when this changes */
	uint32_t rest_ms;    /* time at low discharge current */
};

/* Open-circuit voltage to state of charge, ‰ (exposed for the tests). */
int gauge_ocv_permille(int ocv_mv);

/*
 * Start from a stored state of charge (‰, or -1), checked against the
 * open-circuit voltage estimated from the first reading.
 */
void gauge_init(struct gauge *g, const struct gauge_params *p, int v_mv, int i_ua,
		int saved_permille);
/* One measurement: battery voltage, current (positive = discharging), dt. */
void gauge_update(struct gauge *g, int v_mv, int i_ua, enum gauge_charger ch, uint32_t dt_ms);

int gauge_permille(const struct gauge *g);
/* State of charge to show, %: at most 95 in the constant-current phase, 99 until full. */
int gauge_soc(const struct gauge *g, enum gauge_charger ch);
/* Minutes, -1 = unknown or not applicable. */
int gauge_tte_min(const struct gauge *g, enum gauge_charger ch);
int gauge_ttf_min(const struct gauge *g, enum gauge_charger ch);
