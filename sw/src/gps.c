#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/math_extras.h>

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

#if defined(HAVE_RECEIVER) && defined(CONFIG_GNSS_SATELLITES)
#define HAVE_SKY 1
/*
 * Satellites in view: the receiver reports each system (GPS, Galileo, ...)
 * in its own set, so the latest set of each is kept and the sets that are
 * still fresh are summed up.
 */
#define SKY_SYSTEMS 12
#define SKY_SATS    24
#define SKY_FRESH_MS 3000

static struct {
	int64_t at; /* uptime ms, 0 = none */
	uint8_t n;
	uint8_t nsnr;
	uint8_t snr[SKY_SATS];
} sky[SKY_SYSTEMS];
#endif

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
#ifdef HAVE_SKY
	memset(sky, 0, sizeof(sky));
#endif
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

	/* Over a worse source, or when noticeably off: the clock runs on its own. */
	if ((clock_source() < CLOCK_SRC_GPS || llabs(clock_unix() - unix) > 2) &&
	    clock_offer(unix, CLOCK_SRC_GPS)) {
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

#ifdef HAVE_SKY
static void on_satellites(const struct device *dev, const struct gnss_satellite *s, uint16_t n)
{
	uint32_t system = 0;
	int slot;

	ARG_UNUSED(dev);
	if (n == 0) {
		return;
	}
	/* The set's system; SBAS satellites come within the GPS set. */
	for (int i = 0; i < n && !system; i++) {
		system = s[i].system == GNSS_SYSTEM_SBAS ? 0 : s[i].system;
	}
	slot = system ? u32_count_trailing_zeros(system) : u32_count_trailing_zeros(GNSS_SYSTEM_GPS);
	if (slot >= SKY_SYSTEMS) {
		return;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (powered) {
		sky[slot].at = k_uptime_get();
		sky[slot].n = MIN(n, SKY_SATS);
		sky[slot].nsnr = 0;
		for (int i = 0; i < sky[slot].n; i++) {
			if (s[i].snr) {
				sky[slot].snr[sky[slot].nsnr++] = s[i].snr;
			}
		}
	}
	k_mutex_unlock(&lock);
}

GNSS_SATELLITES_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(gnss)), on_satellites);

/* Sum up the fresh sets; lock held. */
static void sky_status(struct gps_status *st, int64_t now)
{
	st->has_sky = true;
	for (int i = 0; i < SKY_SYSTEMS; i++) {
		if (!sky[i].at || now - sky[i].at > SKY_FRESH_MS) {
			continue;
		}
		st->in_view += sky[i].n;
		st->heard += sky[i].nsnr;
		/* Keep the strongest few, sorted. */
		for (int j = 0; j < sky[i].nsnr; j++) {
			uint8_t v = sky[i].snr[j];

			for (int k = 0; k < ARRAY_SIZE(st->cn0); k++) {
				if (v > st->cn0[k]) {
					uint8_t t = st->cn0[k];

					st->cn0[k] = v;
					v = t;
				}
			}
		}
	}
}
#endif
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
	s->has_sky = false;
	s->in_view = 0;
	s->heard = 0;
	memset(s->cn0, 0, sizeof(s->cn0));
#ifdef HAVE_SKY
	sky_status(s, now);
#endif
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
