/*
 * GPS service: switches the receiver (Zephyr GNSS device `gnss`) on and off
 * by mode, keeps the last fix, sets the clock and tells the mesh about new
 * positions. Without a receiver, a position entered in the settings can
 * still be shared.
 *
 * Modes:
 *   off       receiver off, no position
 *   fixed     the position entered in the settings, receiver off
 *   periodic  every interval: on until a good fix (or a timeout), then off
 *   always    receiver on all the time
 * While the GPS screen is open the receiver stays on (gps_hold).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum gps_mode {
	GPS_MODE_OFF,
	GPS_MODE_FIXED,
	GPS_MODE_PERIODIC,
	GPS_MODE_ALWAYS,
};

/* What the mesh may share of our position. */
enum gps_share {
	GPS_SHARE_OFF,
	GPS_SHARE_APPROX, /* rounded to a cell of about 6 km (Meshtastic precision 13) */
	GPS_SHARE_EXACT,
};

struct gps_config {
	uint8_t mode;          /* enum gps_mode */
	uint8_t share;         /* enum gps_share */
	uint16_t interval_min; /* periodic mode */
	bool has_fixed;
	int32_t fixed_lat_e7;
	int32_t fixed_lon_e7;
};

struct gps_fix {
	bool valid;
	bool manual; /* the fixed position from the settings */
	int32_t lat_e7, lon_e7;
	bool has_alt;
	int32_t alt_m;
	uint16_t sats;
	uint16_t hdop_x10;
	uint16_t speed_kmh;
	uint16_t heading_deg;
	uint32_t uptime_s; /* when it was taken (mesh_uptime_s base) */
};

struct gps_status {
	bool receiver;   /* the board has a GPS receiver */
	bool powered;
	uint32_t on_s;   /* seconds since the receiver was switched on */
	uint16_t sats;   /* satellites used in the latest data */
	bool has_sky;    /* the receiver reports satellites in view (below) */
	uint8_t in_view; /* satellites in view, all systems */
	uint8_t heard;   /* of those with a signal */
	uint8_t cn0[4];  /* the strongest signals, dB-Hz, 0 = none */
	bool fixed_now;  /* the latest data has a fix */
	struct gps_fix last; /* the last good fix (may be old) */
	uint32_t next_s; /* periodic mode: seconds until the next fix, 0 = now/none */
};

void gps_init(void);
void gps_get_config(struct gps_config *c);
void gps_set_config(const struct gps_config *c);
void gps_status(struct gps_status *s);
/* Our position: the fixed one, or the last GPS fix. False if none. */
bool gps_position(struct gps_fix *out);
/* Keep the receiver on (GPS screen open) or return to the mode. */
void gps_hold(bool on);
