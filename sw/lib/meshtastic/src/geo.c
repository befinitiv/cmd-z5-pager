#include <math.h>

#include "meshtastic/geo.h"

#define EARTH_RADIUS_M 6371000.0
#define DEG_TO_RAD     (3.14159265358979323846 / 180.0)

int32_t mt_geo_reduce(int32_t coord_e7, uint32_t bits)
{
	if (bits == 0 || bits >= 32) {
		return coord_e7;
	}
	uint32_t mask = UINT32_MAX << (32 - bits);
	uint32_t v = ((uint32_t)coord_e7 & mask) + (1u << (31 - bits));

	return (int32_t)v;
}

uint32_t mt_geo_distance_m(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7)
{
	double lat1 = lat1_e7 * 1e-7 * DEG_TO_RAD, lat2 = lat2_e7 * 1e-7 * DEG_TO_RAD;
	double dlat = lat2 - lat1;
	double dlon = ((double)lon2_e7 - lon1_e7) * 1e-7 * DEG_TO_RAD;
	double a = sin(dlat / 2) * sin(dlat / 2) +
		   cos(lat1) * cos(lat2) * sin(dlon / 2) * sin(dlon / 2);

	return (uint32_t)(2.0 * EARTH_RADIUS_M * atan2(sqrt(a), sqrt(1.0 - a)) + 0.5);
}

int mt_geo_bearing_deg(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7)
{
	double lat1 = lat1_e7 * 1e-7 * DEG_TO_RAD, lat2 = lat2_e7 * 1e-7 * DEG_TO_RAD;
	double dlon = ((double)lon2_e7 - lon1_e7) * 1e-7 * DEG_TO_RAD;
	double y = sin(dlon) * cos(lat2);
	double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dlon);
	int deg = (int)lround(atan2(y, x) / DEG_TO_RAD);

	return (deg + 360) % 360;
}

const char *mt_geo_compass(int bearing_deg)
{
	static const char *const points[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

	return points[((bearing_deg % 360 + 360) % 360 + 22) / 45 % 8];
}

uint32_t mt_geo_precision_m(uint32_t bits)
{
	if (bits == 0 || bits >= 32) {
		return 0;
	}
	/* Cell height: 2^(32 - bits) units of 1e-7 degrees, about 1.11 cm each. */
	return (uint32_t)((1ull << (32 - bits)) * 111320ull / 10000000ull);
}
