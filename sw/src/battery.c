#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery.h"
#include "battery_backend.h"

LOG_MODULE_REGISTER(battery, CONFIG_CMDZ5_APP_LOG_LEVEL);

#define SAMPLE_MS 1000
/* Store the fuel gauge state now and then, so a reset loses little. */
#define SAVE_INTERVAL_S (6 * 3600)
#define MINUTE          60
#define HOUR_MINUTES    60

K_THREAD_STACK_DEFINE(battery_stack, 2048);
static struct k_thread battery_thread;

static K_MUTEX_DEFINE(lock);
static struct battery_status status = {.soc = -1, .tte_min = -1, .ttf_min = -1};

/* The samples of the last minute, and the sums of the minutes before. */
static int32_t sec_ua[MINUTE];
static int sec_count, sec_pos;
static int64_t min_sum[HOUR_MINUTES - 1];
static int min_count[HOUR_MINUTES - 1];
static int min_pos;
static int64_t cur_sum;
static int cur_count;
static uint64_t drawn_uams; /* µA·ms */

static void add_sample(int ua, uint32_t dt_ms)
{
	sec_ua[sec_pos] = ua;
	sec_pos = (sec_pos + 1) % MINUTE;
	sec_count = MIN(sec_count + 1, MINUTE);

	cur_sum += ua;
	if (++cur_count == MINUTE) {
		min_sum[min_pos] = cur_sum;
		min_count[min_pos] = cur_count;
		min_pos = (min_pos + 1) % ARRAY_SIZE(min_sum);
		cur_sum = 0;
		cur_count = 0;
	}
	if (ua > 0) {
		drawn_uams += (uint64_t)ua * dt_ms;
	}
}

static int avg_minute(void)
{
	int64_t sum = 0;

	for (int i = 0; i < sec_count; i++) {
		sum += sec_ua[i];
	}
	return sec_count ? sum / sec_count : 0;
}

static int avg_hour(void)
{
	int64_t sum = cur_sum;
	int count = cur_count;

	for (size_t i = 0; i < ARRAY_SIZE(min_sum); i++) {
		sum += min_sum[i];
		count += min_count[i];
	}
	return count ? sum / count : 0;
}

static void run(void *p1, void *p2, void *p3)
{
	int64_t last = k_uptime_get();
	uint32_t since_save = 0; /* ms */

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct battery_sample s;
		struct battery_gauge g;
		int64_t now;
		uint32_t dt;

		k_msleep(SAMPLE_MS);
		if (battery_backend_read(&s) != 0) {
			continue;
		}
		now = k_uptime_get();
		dt = now - last;
		last = now;

		/* The lock also keeps battery_save() away from the fuel gauge. */
		k_mutex_lock(&lock, K_FOREVER);
		battery_backend_gauge(&s, dt, &g);
		add_sample(s.current_ua, dt);
		status.valid = true;
		status.usb = s.usb;
		status.charge = s.charge;
		status.voltage_mv = s.voltage_mv;
		status.current_ua = s.current_ua;
		status.temp_dc = s.temp_dc;
		status.avg_min_ua = avg_minute();
		status.avg_hour_ua = avg_hour();
		status.soc = g.soc;
		status.tte_min = g.tte_min;
		status.ttf_min = g.ttf_min;
		status.drawn_uah = drawn_uams / 3600000;
		status.uptime_s += (dt + 500) / 1000;
		since_save += dt;
		if (since_save >= SAVE_INTERVAL_S * 1000U) {
			since_save = 0;
			battery_backend_save();
		}
		k_mutex_unlock(&lock);

		LOG_DBG("%d mV, %d uA, %d%%", s.voltage_mv, s.current_ua, g.soc);
	}
}

void battery_init(void)
{
	if (battery_backend_init() != 0) {
		LOG_ERR("no battery measurement");
		return;
	}
	k_thread_create(&battery_thread, battery_stack, K_THREAD_STACK_SIZEOF(battery_stack), run,
			NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&battery_thread, "battery");
}

void battery_get(struct battery_status *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	*out = status;
	k_mutex_unlock(&lock);
}

void battery_save(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (status.valid) {
		battery_backend_save();
	}
	k_mutex_unlock(&lock);
}
