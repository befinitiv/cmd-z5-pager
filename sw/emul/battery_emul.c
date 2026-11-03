/*
 * Simulated battery for the emulator, in place of the nPM1300: a cell as
 * src/gauge.h assumes it (1100 mAh to 4.2 V, 200 mOhm), charged like on the
 * board (constant current, then constant voltage until 10 %). The current
 * follows the emulated outputs with the estimates of the power budget; the
 * display is not counted, its consumption is unknown. The readings are
 * quantised like the nPM1300 ADC with a 1 A discharge limit and go through
 * the same fuel gauge (gauge.c) as on the board.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "../src/battery_backend.h"
#include "../src/gauge.h"
#include "emul.h"
#include "frontpanel.h"

#define CAPACITY_UAH  1100000 /* as gauge.h assumes */
#define CHARGE_UA     400000  /* charger current, as in the board devicetree */
#define TERM_UA       (CHARGE_UA / 10)
#define CV_PERMILLE   850     /* constant voltage from here on */
#define R_INTERNAL_MO 200
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

static int64_t charge_uams; /* true remaining charge, µA·ms */
static struct gauge gauge;

/* Open-circuit voltage over the state of charge, the curve gauge.c assumes. */
static const int16_t ocv_mv[] = {
	3300, 3600, 3670, 3710, 3750, 3790, 3830, 3890, 3970, 4060, 4170,
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

static enum gauge_charger charger_state(bool usb)
{
	if (!usb) {
		return GAUGE_DISCHARGING;
	}
	if (soc_permille() >= 1000) {
		return GAUGE_FULL;
	}
	return soc_permille() < CV_PERMILLE ? GAUGE_CHARGE_CC : GAUGE_CHARGE_CV;
}

/* Charge current while charging: constant, then tapering off to the termination. */
static int charge_ua(void)
{
	int p = soc_permille();

	if (p >= 1000) {
		return 0;
	}
	if (p < CV_PERMILLE) {
		return CHARGE_UA;
	}
	return TERM_UA + (CHARGE_UA - TERM_UA) * (1000 - p) / (1000 - CV_PERMILLE);
}

int battery_backend_init(void)
{
	static const struct gauge_params params = {
		.capacity_uah = CAPACITY_UAH,
		.r_mohm = R_INTERNAL_MO,
		.charge_ua = CHARGE_UA,
	};
	int32_t permille;

	if (settings_load_one(STATE_KEY, &permille, sizeof(permille)) != sizeof(permille)) {
		permille = 800;
	}
	charge_uams = (int64_t)CLAMP(permille, 0, 1000) * CAPACITY_UAH * 3600;
	gauge_init(&gauge, &params, ocv(soc_permille()), 0, permille);
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
		ua = -charge_ua();
	}
	noisy = ua + (int)(sys_rand32_get() % (2 * IBAT_LSB_UA)) - IBAT_LSB_UA;
	s->current_ua = ua ? noisy / IBAT_LSB_UA * IBAT_LSB_UA : 0;
	/* The charger holds the cell at 4.2 V at most (constant voltage). */
	s->voltage_mv = MIN(ocv(soc_permille()) - ua / 1000 * R_INTERNAL_MO / 1000, 4200);
	s->temp_dc = 250 + (usb ? 30 : 0);
	s->usb = usb;
	s->charge = !usb ? BATTERY_DISCHARGING
		    : soc_permille() < 1000 ? BATTERY_CHARGING : BATTERY_FULL;
	/* The charger reports complete below the termination current. */
	if (usb && charge_ua() <= TERM_UA) {
		charge_uams = (int64_t)CAPACITY_UAH * 3600 * 1000;
		s->charge = BATTERY_FULL;
	}
	return 0;
}

void battery_backend_gauge(const struct battery_sample *s, uint32_t dt_ms,
			   struct battery_gauge *out)
{
	int64_t full = (int64_t)CAPACITY_UAH * 3600 * 1000;
	enum gauge_charger ch = charger_state(s->usb);

	/* The true charge follows the true current (the measured one is quantised). */
	if (s->usb) {
		charge_uams = MIN(charge_uams + (int64_t)charge_ua() * dt_ms, full);
	} else {
		charge_uams = MAX(charge_uams - (int64_t)load_ua() * dt_ms, 0);
	}
	gauge_update(&gauge, s->voltage_mv, s->current_ua, ch, dt_ms);
	out->soc = gauge_soc(&gauge, ch);
	out->tte_min = gauge_tte_min(&gauge, ch);
	out->ttf_min = gauge_ttf_min(&gauge, ch);
}

void battery_backend_save(void)
{
	/* The true charge: the emulator keeps it over a restart. */
	int32_t permille = soc_permille();

	settings_save_one(STATE_KEY, &permille, sizeof(permille));
}
