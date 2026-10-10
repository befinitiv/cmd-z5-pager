/*
 * Battery on the board: the nPM1300 charger measures voltage, current and
 * die temperature; nRF Fuel Gauge turns them into the state of charge.
 *
 * The battery has no thermistor, so the die temperature stands in for the
 * battery temperature. The battery model is Nordic's example model from the
 * nPM1300 fuel gauge sample (battery_model.inc). Replace it with a profile
 * of the real cell, made with the nPM1300 EK and nPM PowerUP, for accurate
 * readings.
 */

#include <math.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <nrf_fuel_gauge.h>

#include "battery_backend.h"

LOG_MODULE_REGISTER(battery_pmic, CONFIG_CMDZ5_APP_LOG_LEVEL);

#define STATE_KEY "batt/fg"

/* CHARGER.BCHGCHARGESTATUS */
#define CHG_STATUS_COMPLETE BIT(1)
#define CHG_STATUS_TRICKLE  BIT(2)
#define CHG_STATUS_CC       BIT(3)
#define CHG_STATUS_CV       BIT(4)
/* VBUSIN.VBUSINSTATUS */
#define VBUS_PRESENT BIT(0)

static const struct device *const charger = DEVICE_DT_GET(DT_NODELABEL(charger));

static const struct battery_model model = {
#include "battery_model.inc"
};

static uint8_t state_buf[1024];
static int32_t last_status = -1;
static bool last_usb;

static float to_float(const struct sensor_value *v)
{
	return (float)v->val1 + (float)v->val2 / 1000000.0f;
}

/* Raw readings in the units of nrf_fuel_gauge (V, A, °C). */
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
	/* Zephyr: negative = discharging; here and in nrf_fuel_gauge the opposite. */
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

static void inform_charge_status(int32_t chg)
{
	union nrf_fuel_gauge_ext_state_info_data info;

	if (chg & CHG_STATUS_COMPLETE) {
		info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
	} else if (chg & CHG_STATUS_TRICKLE) {
		info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
	} else if (chg & CHG_STATUS_CC) {
		/* The charge current may be limited by VBUS. */
		info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CC_LIMITED;
	} else if (chg & CHG_STATUS_CV) {
		info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CV;
	} else {
		info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
	}
	nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE, &info);
}

int battery_backend_init(void)
{
	struct nrf_fuel_gauge_init_parameters params =
		NRF_FUEL_GAUGE_DEFAULT_INIT_PARAMETERS_SECONDARY(0.0f, 0.0f, 0.0f, &model);
	struct sensor_value val;
	int32_t chg, err;
	float charge_current;
	bool usb;
	int ret;

	if (!device_is_ready(charger)) {
		return -ENODEV;
	}
	ret = read(&params.v0, &params.i0, &params.t0, &chg, &err, &usb);
	if (ret) {
		return ret;
	}
	ret = settings_load_one(STATE_KEY, state_buf, sizeof(state_buf));
	if (ret == (int)nrf_fuel_gauge_state_size &&
	    nrf_fuel_gauge_state_compatible_check(state_buf, ret) == 0) {
		params.state = state_buf;
		params.state_size = ret;
		LOG_INF("fuel gauge state restored");
	}
	ret = nrf_fuel_gauge_init(&params, NULL);
	if (ret) {
		LOG_ERR("fuel gauge init failed (%d)", ret);
		return ret;
	}

	/* Charge current and termination current (10 %) for the time to full. */
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &val);
	charge_current = to_float(&val);
	nrf_fuel_gauge_ext_state_update(
		NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT,
		&(union nrf_fuel_gauge_ext_state_info_data){.charge_current_limit = charge_current});
	nrf_fuel_gauge_ext_state_update(
		NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT,
		&(union nrf_fuel_gauge_ext_state_info_data){.charge_term_current =
								    charge_current / 10.0f});
	last_status = -1;
	LOG_INF("fuel gauge %s, model %s", nrf_fuel_gauge_version, model.name);
	return 0;
}

static struct {
	float v, i, t;
	int32_t chg, err;
	bool usb;
} raw;

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

static int minutes(float seconds)
{
	return isfinite(seconds) && seconds >= 0.0f ? (int)(seconds / 60.0f) : -1;
}

void battery_backend_gauge(const struct battery_sample *s, uint32_t dt_ms,
			   struct battery_gauge *out)
{
	float soc, t;

	ARG_UNUSED(s);
	if (raw.usb != last_usb || last_status < 0) {
		last_usb = raw.usb;
		nrf_fuel_gauge_ext_state_update(raw.usb ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
							: NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED,
						NULL);
	}
	if (raw.chg != last_status) {
		last_status = raw.chg;
		inform_charge_status(raw.chg);
	}

	out->soc = -1;
	out->tte_min = -1;
	out->ttf_min = -1;
	if (nrf_fuel_gauge_process(raw.v, raw.i, raw.t, dt_ms / 1000.0f, &soc, NULL) != 0) {
		return;
	}
	out->soc = CLAMP(lroundf(soc), 0, 100);
	if (raw.i >= 0.0f) {
		if (nrf_fuel_gauge_tte_get(&t) == 0) {
			out->tte_min = minutes(t);
		}
	} else if (nrf_fuel_gauge_ttf_get(&t) == 0) {
		out->ttf_min = minutes(t);
	}
}

void battery_backend_save(void)
{
	int ret = nrf_fuel_gauge_state_get(state_buf, sizeof(state_buf));

	if (ret == 0) {
		ret = settings_save_one(STATE_KEY, state_buf, nrf_fuel_gauge_state_size);
	}
	if (ret) {
		LOG_WRN("saving the fuel gauge state failed (%d)", ret);
	}
}
