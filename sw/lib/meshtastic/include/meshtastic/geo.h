/*
 * Positions: rounding to a precision as Meshtastic does, distance and
 * bearing between two points. Coordinates in degrees * 1e7.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Keep the top `bits` bits of a coordinate and move it to the middle of the
 * remaining cell, as the official firmware does for position_precision.
 * 32 (or more) leaves it unchanged.
 */
int32_t mt_geo_reduce(int32_t coord_e7, uint32_t bits);

/* Great-circle distance in metres (haversine, mean earth radius). */
uint32_t mt_geo_distance_m(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7);

/* Initial bearing from point 1 to point 2 in degrees, 0..359 (0 = north). */
int mt_geo_bearing_deg(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7);

/* Compass point of a bearing: "N", "NE", ... "NW". */
const char *mt_geo_compass(int bearing_deg);

/*
 * Size of a position_precision cell in metres (0 = exact); the position is
 * within half of it ("within 2.9 km" for 13 bits in the official apps).
 */
uint32_t mt_geo_precision_m(uint32_t bits);

#ifdef __cplusplus
}
#endif
