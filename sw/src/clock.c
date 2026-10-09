#include <zephyr/kernel.h>

#include "clock.h"

#ifdef CONFIG_CMDZ5_EMUL
#include "../emul/frontpanel.h"
#endif

static int64_t base_unix;   /* UTC seconds at base_uptime */
static int64_t base_uptime; /* ms */
static int32_t utc_offset;
static bool valid;

void clock_set(int64_t unix_utc, int32_t utc_offset_s)
{
	base_unix = unix_utc;
	base_uptime = k_uptime_get();
	utc_offset = utc_offset_s;
	valid = true;
}

bool clock_valid(void)
{
	return valid;
}

int64_t clock_unix(void)
{
	return base_unix + (k_uptime_get() - base_uptime) / 1000;
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

void clock_local(int64_t t, struct clock_tm *tm)
{
	clock_from_unix(t + utc_offset, tm);
}

bool clock_now(struct clock_tm *tm)
{
	if (!valid) {
		return false;
	}
	clock_from_unix(clock_unix() + utc_offset, tm);
	return true;
}

void clock_init(void)
{
#ifdef CONFIG_CMDZ5_EMUL
	clock_set(frontpanel_unix_time(), frontpanel_utc_offset());
#endif
}
