/*
 * Battery on the board: the nPM1300 charger measures voltage, current and
 * die temperature; gauge.c turns them into the state of charge, with the
 * assumptions about the cell given there. (nRF Fuel Gauge needs a battery
 * model made by profiling the cell with the nPM1300 EK and nPM PowerUP.)
 *
 * The battery has no thermistor, so the die temperature stands in for the
 * battery temperature.
 */

#include <math.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "battery_backend.h"
#include "gauge.h"

LOG_MODULE_REGISTER(battery_pmic, CONFIG_CMDZ5_APP_LOG_LEVEL);

#define STATE_KEY "batt/g" /* state of charge, ‰ */

/* CHARGER.BCHGCHARGESTATUS */
#define CHG_STATUS_COMPLETE BIT(1)
#define CHG_STATUS_TRICKLE  BIT(2)
#define CHG_STATUS_CC       BIT(3)
#define CHG_STATUS_CV       BIT(4)
/* VBUSIN.VBUSINSTATUS */
#define VBUS_PRESENT BIT(0)

static const struct device *const charger = DEVICE_DT_GET(DT_NODELABEL(charger));

static struct gauge gauge;

static float to_float(const struct sensor_value *v)
{
	return (float)v->val1 + (float)v->val2 / 1000000.0f;
}

/* Raw readings (V, A, °C). */
static int read(float *v, float *i, float *t, int32_t *chg, int32_t *err, bool *usb)
{
	struct sensor_value val;
	int ret = sensor_sample_fetch(charger);

	if (ret) {
		return ret;
	}
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &val);
	*v = to_float(&val);
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &val);
	/* Zephyr: negative = discharging; here the opposite. */
	*i = -to_float(&val);
	sensor_channel_get(charger, SENSOR_CHAN_DIE_TEMP, &val);
	*t = to_float(&val);
	sensor_channel_get(charger, (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &val);
	*chg = val.val1;
	sensor_channel_get(charger, (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_ERROR, &val);
	*err = val.val1;
	sensor_channel_get(charger, (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
			   &val);
	*usb = val.val1 & VBUS_PRESENT;
	return 0;
}

static struct {
	float v, i, t;
	int32_t chg, err;
	bool usb;
} raw;

static enum gauge_charger charger_state(void)
{
	if (!raw.usb) {
		return GAUGE_DISCHARGING;
	}
	if (raw.chg & CHG_STATUS_COMPLETE) {
		return GAUGE_FULL;
	}
	if (raw.chg & CHG_STATUS_CV) {
		return GAUGE_CHARGE_CV;
	}
	if (raw.chg & (CHG_STATUS_TRICKLE | CHG_STATUS_CC)) {
		return GAUGE_CHARGE_CC;
	}
	return GAUGE_IDLE;
}

int battery_backend_init(void)
{
	struct sensor_value val;
	struct gauge_params params = {
		.capacity_uah = 1100000,
		.r_mohm = 200,
	};
	int32_t saved = -1;
	int ret;

	if (!device_is_ready(charger)) {
		return -ENODEV;
	}
	ret = read(&raw.v, &raw.i, &raw.t, &raw.chg, &raw.err, &raw.usb);
	if (ret) {
		return ret;
	}
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &val);
	params.charge_ua = (int32_t)(to_float(&val) * 1000000.0f);
	if (settings_load_one(STATE_KEY, &saved, sizeof(saved)) != sizeof(saved)) {
		saved = -1;
	}
	gauge_init(&gauge, &params, lroundf(raw.v * 1000.0f), lroundf(raw.i * 1000000.0f), saved);
	LOG_INF("fuel gauge: %d %% (%s; %d mV, %d mA)", gauge_soc(&gauge, charger_state()),
		saved >= 0 ? "stored state" : "from the voltage", (int)lroundf(raw.v * 1000.0f),
		(int)lroundf(raw.i * 1000.0f));
	return 0;
}

int battery_backend_read(struct battery_sample *s)
{
	int ret = read(&raw.v, &raw.i, &raw.t, &raw.chg, &raw.err, &raw.usb);

	if (ret) {
		return ret;
	}
	s->voltage_mv = lroundf(raw.v * 1000.0f);
	s->current_ua = lroundf(raw.i * 1000000.0f);
	s->temp_dc = lroundf(raw.t * 10.0f);
	s->usb = raw.usb;
	if (raw.err) {
		s->charge = BATTERY_ERROR;
	} else if (!raw.usb) {
		s->charge = BATTERY_DISCHARGING;
	} else if (raw.chg & (CHG_STATUS_TRICKLE | CHG_STATUS_CC | CHG_STATUS_CV)) {
		s->charge = BATTERY_CHARGING;
	} else {
		s->charge = BATTERY_FULL;
	}
	return 0;
}

void battery_backend_gauge(const struct battery_sample *s, uint32_t dt_ms,
			   struct battery_gauge *out)
{
	enum gauge_charger ch = charger_state();

	gauge_update(&gauge, s->voltage_mv, s->current_ua, ch, dt_ms);
	out->soc = gauge_soc(&gauge, ch);
	out->tte_min = gauge_tte_min(&gauge, ch);
	out->ttf_min = gauge_ttf_min(&gauge, ch);
}

void battery_backend_save(void)
{
	int32_t permille = gauge_permille(&gauge);
	int ret = settings_save_one(STATE_KEY, &permille, sizeof(permille));

	if (ret) {
		LOG_WRN("saving the fuel gauge state failed (%d)", ret);
	}
}
