/*
 * Wall-clock time. Unknown after boot on the board until it is set from GPS
 * or the mesh; the emulator takes the PC's time. Kept in UTC; local time
 * follows the time zone setting, with daylight saving time.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct clock_tm {
	int year;  /* e.g. 2026 */
	int month; /* 1..12 */
	int day;   /* 1..31 */
	int hour, min, sec;
	int wday;  /* 0 = Sunday */
};

/* Where the time came from, in increasing quality. */
enum clock_source {
	CLOCK_SRC_NONE,
	CLOCK_SRC_MESH, /* time field of a received Meshtastic packet */
	CLOCK_SRC_GPS,
	CLOCK_SRC_PC,   /* emulator */
};

void clock_init(void);
/*
 * Offer a time from a source. GPS (and the PC) always set it; the mesh only
 * when there is no better time yet, or its own last time is an hour old.
 * Returns true if the clock was set.
 */
bool clock_offer(int64_t unix_utc, enum clock_source src);
enum clock_source clock_source(void);
bool clock_valid(void);
int64_t clock_unix(void);
/* Local time; returns false if the clock was never set. */
bool clock_now(struct clock_tm *tm);
void clock_from_unix(int64_t t, struct clock_tm *tm);
/* Local time of a unix timestamp (time zone and daylight saving time at t). */
void clock_local(int64_t t, struct clock_tm *tm);

/* Time zones (Settings -> Time zone). */
int clock_zone_count(void);
int clock_zone(void);
/* Select and store. */
void clock_set_zone(int zone);
/* Short name, untranslated (e.g. "CET"). */
const char *clock_zone_name(int zone);
