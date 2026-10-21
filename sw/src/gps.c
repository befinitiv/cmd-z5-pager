#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/settings/settings.h>

#ifdef CONFIG_GNSS
#include <zephyr/drivers/gnss.h>
#endif

#include "clock.h"
#include "gps.h"
#include "mesh/mesh.h"

LOG_MODULE_REGISTER(gps, CONFIG_CMDZ5_APP_LOG_LEVEL);

#if defined(CONFIG_GNSS) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gnss))
#define HAVE_RECEIVER 1
static const struct device *const gnss = DEVICE_DT_GET(DT_NODELABEL(gnss));
#endif

#define CONFIG_KEY "gps/cfg"
/* Periodic mode: give up after this long without a good fix. */
#define FIX_TIMEOUT_MS (120 * 1000)
/* A good fix: HDOP up to 5, seen a few times in a row (lets it settle). */
#define GOOD_HDOP_X10 50
#define GOOD_FIXES    3

static K_MUTEX_DEFINE(lock);
static struct gps_config cfg = {
	.mode = GPS_MODE_PERIODIC,
	.share = GPS_SHARE_APPROX,
	.interval_min = 15,
};
static bool powered, held;
static int64_t on_since, next_cycle; /* uptime ms */
static uint16_t sats;
static bool fixed_now;
static int good;
static struct gps_fix last;
static struct k_work_delayable cycle_work;

static int gps_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "cfg") != 0 || len != sizeof(cfg)) {
		return -ENOENT;
	}
	return read_cb(cb_arg, &cfg, sizeof(cfg)) == sizeof(cfg) ? 0 : -EIO;
}

SETTINGS_STATIC_HANDLER_DEFINE(gps, "gps", NULL, gps_settings_set, NULL, NULL);

/* Switch the receiver; lock held. */
static void power(bool on)
{
	if (on == powered) {
		return;
	}
#ifdef HAVE_RECEIVER
	int ret = pm_device_action_run(gnss, on ? PM_DEVICE_ACTION_RESUME
						: PM_DEVICE_ACTION_SUSPEND);

	if (ret && ret != -EALREADY) {
		LOG_WRN("receiver %s failed (%d)", on ? "on" : "off", ret);
		return;
	}
#else
	if (on) {
		return; /* no receiver on this board yet */
	}
#endif
	powered = on;
	sats = 0;
	fixed_now = false;
	good = 0;
	if (on) {
		on_since = k_uptime_get();
	}
	LOG_INF("receiver %s", on ? "on" : "off");
}

/* Decide whether the receiver should run; lock held. Returns the next check. */
static k_timeout_t update(void)
{
	int64_t now = k_uptime_get();
	int64_t interval = (int64_t)MAX(cfg.interval_min, 1) * 60 * 1000;
	bool want = false;

	switch (cfg.mode) {
	case GPS_MODE_ALWAYS:
		want = true;
		break;
	case GPS_MODE_PERIODIC:
		if (powered) {
			/* Done with a good fix, or given up. */
			bool done = good >= GOOD_FIXES || now - on_since >= FIX_TIMEOUT_MS;

			want = !done;
			if (done) {
				next_cycle = now + interval;
			}
		} else {
			want = now >= next_cycle;
		}
		break;
	default:
		break;
	}
	power(want || held);

#ifndef HAVE_RECEIVER
	return K_FOREVER; /* nothing to switch */
#endif
	if (powered) {
		return K_SECONDS(1);
	}
	if (cfg.mode == GPS_MODE_PERIODIC) {
		return K_MSEC(MAX(next_cycle - now, 0));
	}
	return K_FOREVER;
}

static void cycle_handler(struct k_work *work)
{
	k_timeout_t next;

	ARG_UNUSED(work);
	k_mutex_lock(&lock, K_FOREVER);
	next = update();
	k_mutex_unlock(&lock);
	if (!K_TIMEOUT_EQ(next, K_FOREVER)) {
		k_work_reschedule(&cycle_work, next);
	}
}

#ifdef HAVE_RECEIVER
/* Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int era = y / 400;
	int yoe = y - era * 400;
	int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return (int64_t)era * 146097 + doe - 719468;
}

static void set_clock(const struct gnss_time *t)
{
	int64_t unix = days_from_civil(2000 + t->century_year, t->month, t->month_day) * 86400 +
		       t->hour * 3600 + t->minute * 60 + t->millisecond / 1000;

	/* Only when unset or noticeably off: the clock runs on its own. */
	if (!clock_valid() || llabs(clock_unix() - unix) > 2) {
		clock_set_utc(unix);
		LOG_INF("clock set from GPS");
	}
}

static void on_gnss(const struct device *dev, const struct gnss_data *d)
{
	bool fix = d->info.fix_status != GNSS_FIX_STATUS_NO_FIX;
	bool done;

	ARG_UNUSED(dev);
	k_mutex_lock(&lock, K_FOREVER);
	if (!powered) {
		k_mutex_unlock(&lock);
		return;
	}
	sats = d->info.satellites_cnt;
	fixed_now = fix;
	if (fix) {
		last = (struct gps_fix){
			.valid = true,
			.lat_e7 = (int32_t)(d->nav_data.latitude / 100),
			.lon_e7 = (int32_t)(d->nav_data.longitude / 100),
			.has_alt = true,
			.alt_m = d->nav_data.altitude / 1000,
			.sats = d->info.satellites_cnt,
			.hdop_x10 = d->info.hdop / 100,
			.speed_kmh = d->nav_data.speed * 36 / 10000,
			.heading_deg = d->nav_data.bearing / 1000,
			.uptime_s = mesh_uptime_s(),
		};
		good = last.hdop_x10 <= GOOD_HDOP_X10 ? good + 1 : 0;
		set_clock(&d->utc);
	} else {
		good = 0;
	}
	done = cfg.mode == GPS_MODE_PERIODIC && good == GOOD_FIXES;
	k_mutex_unlock(&lock);

	if (fix) {
		mesh_position_update();
	}
	if (done) {
		/* Switch off soon, not from the driver's own callback. */
		k_work_reschedule(&cycle_work, K_NO_WAIT);
	}
}

GNSS_DATA_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(gnss)), on_gnss);
#endif

void gps_init(void)
{
	settings_load_subtree("gps");
	k_work_init_delayable(&cycle_work, cycle_handler);
#ifdef HAVE_RECEIVER
	/* The receiver starts powered; off until the mode wants it. */
	powered = true;
	k_mutex_lock(&lock, K_FOREVER);
	power(false);
	k_mutex_unlock(&lock);
#endif
	/* Periodic mode: the first fix right after the start. */
	next_cycle = k_uptime_get();
	k_work_reschedule(&cycle_work, K_NO_WAIT);
}

void gps_get_config(struct gps_config *c)
{
	k_mutex_lock(&lock, K_FOREVER);
	*c = cfg;
	k_mutex_unlock(&lock);
}

void gps_set_config(const struct gps_config *c)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (c->mode != cfg.mode || c->interval_min != cfg.interval_min) {
		next_cycle = k_uptime_get(); /* a fix right away in the new mode */
	}
	cfg = *c;
	k_mutex_unlock(&lock);
	settings_save_one(CONFIG_KEY, c, sizeof(*c));
	k_work_reschedule(&cycle_work, K_NO_WAIT);
	mesh_position_update();
}

void gps_status(struct gps_status *s)
{
	int64_t now = k_uptime_get();

	k_mutex_lock(&lock, K_FOREVER);
#ifdef HAVE_RECEIVER
	s->receiver = true;
#else
	s->receiver = false;
#endif
	s->powered = powered;
	s->on_s = powered ? (now - on_since) / 1000 : 0;
	s->sats = sats;
	s->fixed_now = fixed_now;
	s->last = last;
	s->next_s = !powered && cfg.mode == GPS_MODE_PERIODIC && next_cycle > now
			    ? (next_cycle - now + 999) / 1000
			    : 0;
	k_mutex_unlock(&lock);
}

bool gps_position(struct gps_fix *out)
{
	bool ok = false;

	k_mutex_lock(&lock, K_FOREVER);
	if (cfg.mode == GPS_MODE_FIXED) {
		if (cfg.has_fixed) {
			*out = (struct gps_fix){.valid = true,
						.manual = true,
						.lat_e7 = cfg.fixed_lat_e7,
						.lon_e7 = cfg.fixed_lon_e7};
			ok = true;
		}
	} else if (cfg.mode != GPS_MODE_OFF && last.valid) {
		*out = last;
		ok = true;
	}
	k_mutex_unlock(&lock);
	return ok;
}

void gps_hold(bool on)
{
	k_mutex_lock(&lock, K_FOREVER);
	held = on;
	k_mutex_unlock(&lock);
	k_work_reschedule(&cycle_work, K_NO_WAIT);
}
