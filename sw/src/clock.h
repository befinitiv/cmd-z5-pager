/*
 * Wall-clock time. Unknown after boot on the board until it is set (later
 * from GPS or the mesh); the emulator takes the PC's time.
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

void clock_init(void);
void clock_set(int64_t unix_utc, int32_t utc_offset_s);
bool clock_valid(void);
int64_t clock_unix(void);
/* Local time; returns false if the clock was never set. */
bool clock_now(struct clock_tm *tm);
void clock_from_unix(int64_t t, struct clock_tm *tm);
