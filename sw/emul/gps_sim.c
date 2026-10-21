/*
 * Satellite simulation for the emulated GNSS receiver (Zephyr's
 * zephyr,gnss-emul with manual updates). While the receiver is switched on
 * (device power management, as the board will do with the real one), it
 * goes through a time to first fix with a few satellites, then reports the
 * position given on the command line, optionally moving:
 *
 *   --gps=<lat>,<lon>[,<alt m>]   position (default: no satellites, no fix)
 *   --gps-move=<m/s>,<deg>        walk or drive with this speed and heading
 *   --gps-ttff=<s>                time to first fix after switching on (8),
 *                                 plus a random 0..3 s like a real receiver
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gnss.h>
#include <zephyr/drivers/gnss/gnss_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>

#include <cmdline.h>
#include <posix_native_task.h>

#include "emul.h"
#include "frontpanel.h"

#define UPDATE_MS 500
#define M_PER_DEG 111320.0

static const struct device *const gnss = DEVICE_DT_GET(DT_NODELABEL(gnss));

#define DEFAULT_TTFF_S 8

static char *opt_pos;
static char *opt_move;
static uint32_t opt_ttff; /* UINT32_MAX when not given (native_sim default) */

static bool have_pos;
static double lat0, lon0, alt0;
static double speed, heading; /* m/s, degrees */

static void parse_options(void)
{
	if (opt_ttff == UINT32_MAX) {
		opt_ttff = DEFAULT_TTFF_S;
	}
	if (opt_pos) {
		int n = sscanf(opt_pos, "%lf,%lf,%lf", &lat0, &lon0, &alt0);

		have_pos = n >= 2 && fabs(lat0) <= 90 && fabs(lon0) <= 180;
		if (!have_pos) {
			printf("gps: cannot parse --gps=%s (expected lat,lon[,alt])\n", opt_pos);
		}
	}
	if (opt_move && sscanf(opt_move, "%lf,%lf", &speed, &heading) != 2) {
		printf("gps: cannot parse --gps-move=%s (expected m/s,degrees)\n", opt_move);
		speed = 0;
	}
}

static void sim_thread(void *p1, void *p2, void *p3)
{
	int64_t on_since = -1;
	int64_t ttff_ms = 0;
	/* Unix time of the boot, for the time in the fixes. */
	int64_t boot_unix_ms = frontpanel_unix_time() * 1000 - k_uptime_get();

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	parse_options();

	for (;;) {
		enum pm_device_state state;
		struct navigation_data nav = {0};
		struct gnss_info info = {0};
		int64_t now = k_uptime_get();

		k_msleep(UPDATE_MS);
		if (pm_device_state_get(gnss, &state) != 0 || state != PM_DEVICE_STATE_ACTIVE) {
			on_since = -1;
			continue;
		}
		if (on_since < 0) {
			on_since = now;
			uint32_t r;

			/* Host randomness: each emulator instance differs. */
			emul_random(&r, sizeof(r));
			ttff_ms = opt_ttff * 1000LL + r % 3000;
		}
		int64_t on_ms = now - on_since;

		if (!have_pos || on_ms < ttff_ms) {
			/* Searching: satellites appear one by one, no fix. */
			info.satellites_cnt = have_pos ? MIN(on_ms / 2000, 3) : 0;
			info.fix_status = GNSS_FIX_STATUS_NO_FIX;
		} else {
			/* Moving since the start of the emulator, like a real walk. */
			double dist = speed * now / 1000.0;
			double rad = heading * 3.14159265358979 / 180.0;
			double lat = lat0 + dist * cos(rad) / M_PER_DEG;
			double lon = lon0 + dist * sin(rad) / (M_PER_DEG * cos(lat0 * 3.14159265 / 180));

			nav.latitude = (int64_t)llround(lat * 1e9);
			nav.longitude = (int64_t)llround(lon * 1e9);
			nav.altitude = (int32_t)(alt0 * 1000);
			nav.speed = (uint32_t)(speed * 1000);
			nav.bearing = (uint32_t)(heading * 1000);
			info.satellites_cnt = 9;
			info.hdop = 1200;
			info.fix_status = GNSS_FIX_STATUS_GNSS_FIX;
			info.fix_quality = GNSS_FIX_QUALITY_GNSS_SPS;
		}
		gnss_emul_set_data(gnss, &nav, &info, boot_unix_ms);
	}
}

K_THREAD_DEFINE(gps_sim, CONFIG_ARCH_POSIX_RECOMMENDED_STACK_SIZE, sim_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

static void add_options(void)
{
	static struct args_struct_t options[] = {
		{.option = "gps", .name = "lat,lon[,alt]", .type = 's', .dest = &opt_pos,
		 .descript = "Position of the emulated GPS (default: no fix)"},
		{.option = "gps-move", .name = "m/s,deg", .type = 's', .dest = &opt_move,
		 .descript = "Move with this speed and heading"},
		{.option = "gps-ttff", .name = "s", .type = 'u', .dest = &opt_ttff,
		 .descript = "Time to first fix after switching the GPS on (default 8)"},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(add_options, PRE_BOOT_1, 12);
