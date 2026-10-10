/*
 * Simulated battery for the emulator, in place of the nPM1300 and nRF Fuel
 * Gauge (which exists only for ARM). The current follows the emulated
 * outputs with the estimates of the power budget; the display is not
 * counted, its consumption is unknown. The readings are quantised like the
 * nPM1300 ADC with a 1 A discharge limit, so the averages behave as on the
 * board.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "../src/battery_backend.h"
#include "emul.h"
#include "frontpanel.h"

#define CAPACITY_UAH  1000000 /* assumed 1000 mAh cell */
#define CHARGE_UA     150000  /* charger current, as in the board devicetree */
#define R_INTERNAL_MO 150     /* internal resistance, mOhm */
/* nPM1300 IBAT: full scale 1.12 x discharge limit, 10 bits. */
#define IBAT_LSB_UA (1120000 / 1023)
#define STATE_KEY   "batt/emul"

/* Currents from the battery, µA (see the README power estimate). */
#define I_BASE      50     /* nRF52840 mostly asleep, PMIC, accelerometer */
#define I_RADIO_RX  4500
#define I_RADIO_TX  110000
#define I_BACKLIGHT 15000  /* at 100 % */
#define I_VIBRATION 80000
#define I_SPEAKER   60000
#define I_EARPIECE  15000

static int64_t charge_uams; /* remaining charge, µA·ms */
static int load_avg_ua;     /* smoothed for the time to empty */

/* Open-circuit voltage of a Li-ion cell over the state of charge. */
static const int16_t ocv_mv[] = {
	3300, 3550, 3650, 3690, 3720, 3750, 3770, 3790, 3820, 3870, 3920, 3980, 4040, 4110, 4180,
};

static int soc_permille(void)
{
	return CLAMP(charge_uams / (CAPACITY_UAH * 3600LL), 0, 1000);
}

static int ocv(int permille)
{
	int n = ARRAY_SIZE(ocv_mv) - 1;
	int pos = permille * n;
	int i = MIN(pos / 1000, n - 1);

	return ocv_mv[i] + (ocv_mv[i + 1] - ocv_mv[i]) * (pos - i * 1000) / 1000;
}

static int load_ua(void)
{
	struct frontpanel_load l;
	int ua = I_BASE;

	frontpanel_get_load(&l);
	if (emul_radio_tx()) {
		ua += I_RADIO_TX;
	} else if (emul_radio_rx()) {
		ua += I_RADIO_RX;
	}
	ua += I_BACKLIGHT * l.backlight / 100 + I_VIBRATION * l.vibration / 100;
	ua += (l.speaker ? I_SPEAKER : 0) + (l.earpiece ? I_EARPIECE : 0);
	return ua;
}

int battery_backend_init(void)
{
	int32_t permille;

	if (settings_load_one(STATE_KEY, &permille, sizeof(permille)) != sizeof(permille)) {
		permille = 800;
	}
	charge_uams = (int64_t)CLAMP(permille, 0, 1000) * CAPACITY_UAH * 3600;
	load_avg_ua = load_ua();
	return 0;
}

int battery_backend_read(struct battery_sample *s)
{
	bool usb = frontpanel_usb();
	int load = load_ua();
	int ua = load;
	/* Measurement noise of about one LSB, then the ADC step. */
	int noisy;

	if (usb) {
		/* USB supplies the load; the battery charges until full. */
		ua = soc_permille() < 1000 ? -CHARGE_UA : 0;
	}
	noisy = ua + (int)(sys_rand32_get() % (2 * IBAT_LSB_UA)) - IBAT_LSB_UA;
	s->current_ua = ua ? noisy / IBAT_LSB_UA * IBAT_LSB_UA : 0;
	s->voltage_mv = ocv(soc_permille()) - ua / 1000 * R_INTERNAL_MO / 1000;
	s->temp_dc = 250 + (usb ? 30 : 0);
	s->usb = usb;
	s->charge = !usb ? BATTERY_DISCHARGING
		    : soc_permille() < 1000 ? BATTERY_CHARGING : BATTERY_FULL;
	return 0;
}

void battery_backend_gauge(const struct battery_sample *s, uint32_t dt_ms,
			   struct battery_gauge *out)
{
	int64_t full = (int64_t)CAPACITY_UAH * 3600 * 1000;

	/* Integrate the true current (the measured one is quantised). */
	if (s->usb) {
		charge_uams = MIN(charge_uams + (int64_t)CHARGE_UA * dt_ms, full);
	} else {
		charge_uams = MAX(charge_uams - (int64_t)load_ua() * dt_ms, 0);
	}
	/* About a 5 min time constant; restarts when USB is unplugged. */
	if (s->usb) {
		load_avg_ua = load_ua();
	} else {
		load_avg_ua += (s->current_ua - load_avg_ua) / 300;
	}

	out->soc = (soc_permille() + 5) / 10;
	out->tte_min = -1;
	out->ttf_min = -1;
	if (!s->usb && load_avg_ua > 0) {
		out->tte_min = charge_uams / 1000 / load_avg_ua / 60;
	} else if (s->charge == BATTERY_CHARGING) {
		out->ttf_min = (full - charge_uams) / 1000 / CHARGE_UA / 60;
	}
}

void battery_backend_save(void)
{
	int32_t permille = soc_permille();

	settings_save_one(STATE_KEY, &permille, sizeof(permille));
}
