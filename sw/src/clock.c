#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "clock.h"

#ifdef CONFIG_CMDZ5_EMUL
#include "../emul/frontpanel.h"
#endif

LOG_MODULE_REGISTER(clock, CONFIG_CMDZ5_APP_LOG_LEVEL);

/* The mesh may correct its own earlier time after this long. */
#define MESH_REFRESH_MS (60 * 60 * 1000)

enum dst_rule {
	DST_NONE,
	DST_EU, /* last Sunday of March 01:00 UTC .. last Sunday of October 01:00 UTC */
	DST_US, /* second Sunday of March 02:00 .. first Sunday of November 02:00 local */
};

static const struct zone {
	const char *name;
	int32_t std_offset; /* s */
	enum dst_rule dst;
} zones[] = {
	{"UTC", 0, DST_NONE},
	{"WET", 0, DST_EU},
	{"CET", 3600, DST_EU},
	{"EET", 2 * 3600, DST_EU},
	{"US East", -5 * 3600, DST_US},
	{"US Central", -6 * 3600, DST_US},
	{"US Mountain", -7 * 3600, DST_US},
	{"US Pacific", -8 * 3600, DST_US},
};

#define ZONE_DEFAULT 2 /* CET */

static int64_t base_unix;   /* UTC seconds at base_uptime */
static int64_t base_uptime; /* ms */
static enum clock_source source;
static uint8_t zone = ZONE_DEFAULT;

static int clock_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t z;

	if (strcmp(name, "zone") != 0 || len != sizeof(z)) {
		return -ENOENT;
	}
	if (read_cb(cb_arg, &z, sizeof(z)) != sizeof(z)) {
		return -EIO;
	}
	if (z < ARRAY_SIZE(zones)) {
		zone = z;
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(clock, "clock", NULL, clock_settings_set, NULL, NULL);

static void set(int64_t unix_utc, enum clock_source src)
{
	base_unix = unix_utc;
	base_uptime = k_uptime_get();
	source = src;
}

bool clock_offer(int64_t unix_utc, enum clock_source src)
{
	bool take;

	if (src == CLOCK_SRC_MESH) {
		take = source == CLOCK_SRC_NONE ||
		       (source == CLOCK_SRC_MESH &&
			k_uptime_get() - base_uptime >= MESH_REFRESH_MS);
	} else {
		take = src >= source;
	}
	if (take) {
		set(unix_utc, src);
	}
	return take;
}

enum clock_source clock_source(void)
{
	return source;
}

bool clock_valid(void)
{
	return source != CLOCK_SRC_NONE;
}

int64_t clock_unix(void)
{
	return base_unix + (k_uptime_get() - base_uptime) / 1000;
}

/* Civil date to days since 1970-01-01 (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + doe - 719468;
}

/* Days since 1970-01-01 to civil date (Howard Hinnant's algorithm). */
void clock_from_unix(int64_t t, struct clock_tm *tm)
{
	int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
	int64_t secs = t - days * 86400;

	tm->hour = secs / 3600;
	tm->min = secs / 60 % 60;
	tm->sec = secs % 60;
	tm->wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */

	int64_t z = days + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;

	tm->day = (int)(doy - (153 * mp + 2) / 5 + 1);
	tm->month = (int)(mp < 10 ? mp + 3 : mp - 9);
	tm->year = (int)(yoe + era * 400 + (tm->month <= 2));
}

/* Day (days since 1970) of the n-th Sunday of a month; n = -1: the last. */
static int64_t sunday(int year, int month, int n)
{
	if (n > 0) {
		int64_t first = days_from_civil(year, month, 1);
		int wday = (int)((first % 7 + 11) % 7);

		return first + (7 - wday) % 7 + 7 * (n - 1);
	}
	int64_t last = days_from_civil(month == 12 ? year + 1 : year, month % 12 + 1, 1) - 1;
	int wday = (int)((last % 7 + 11) % 7);

	return last - wday;
}

/* Offset from UTC at t (UTC), with daylight saving time. */
static int32_t offset_at(int64_t t)
{
	const struct zone *z = &zones[zone];
	struct clock_tm tm;
	int64_t start, end;

	if (z->dst == DST_NONE) {
		return z->std_offset;
	}
	clock_from_unix(t, &tm);
	if (z->dst == DST_EU) {
		start = sunday(tm.year, 3, -1) * 86400 + 3600;
		end = sunday(tm.year, 10, -1) * 86400 + 3600;
	} else {
		/* 02:00 local standard time, and 02:00 local daylight time. */
		start = sunday(tm.year, 3, 2) * 86400 + 2 * 3600 - z->std_offset;
		end = sunday(tm.year, 11, 1) * 86400 + 2 * 3600 - (z->std_offset + 3600);
	}
	return z->std_offset + (t >= start && t < end ? 3600 : 0);
}

void clock_local(int64_t t, struct clock_tm *tm)
{
	clock_from_unix(t + offset_at(t), tm);
}

bool clock_now(struct clock_tm *tm)
{
	if (!clock_valid()) {
		return false;
	}
	clock_local(clock_unix(), tm);
	return true;
}

int clock_zone_count(void)
{
	return ARRAY_SIZE(zones);
}

int clock_zone(void)
{
	return zone;
}

void clock_set_zone(int z)
{
	if (z < 0 || z >= (int)ARRAY_SIZE(zones)) {
		return;
	}
	zone = z;
#ifdef CONFIG_SETTINGS
	uint8_t v = zone;
	int ret = settings_save_one("clock/zone", &v, sizeof(v));

	if (ret) {
		LOG_ERR("saving the time zone failed (%d)", ret);
	}
#endif
}

const char *clock_zone_name(int z)
{
	return z >= 0 && z < (int)ARRAY_SIZE(zones) ? zones[z].name : "?";
}

void clock_init(void)
{
#ifdef CONFIG_CMDZ5_EMUL
	if (frontpanel_clock_known()) {
		set(frontpanel_unix_time(), CLOCK_SRC_PC);
	}
#endif
}
