#include "gauge.h"

#define MS_PER_H 3600000LL

/* Low enough to read the open-circuit voltage after a while (radio RX, display). */
#define REST_UA        30000
#define REST_MS        (5 * 60 * 1000)
/* Correction towards the voltage estimate: only beyond this difference... */
#define DEADBAND       50 /* ‰ */
/* ...with this time constant, faster near empty. */
#define CORRECT_MS     (60 * 60 * 1000)
#define CORRECT_LOW_MS (10 * 60 * 1000)
#define LOW_PERMILLE   100
/* Averaging of the current for the time estimates. */
#define AVG_MS         (10 * 60 * 1000)
/* The constant-voltage phase: about the last 15 %, taking about 40 min at 400 mA. */
#define CV_PERMILLE    850
#define CV_TAIL_MIN    40

/*
 * Open-circuit voltage at 0, 10, ... 100 % of the charge between empty and
 * full at 4.2 V: a typical LiPo (3.7 V / 4.2 V) cell curve.
 */
static const int16_t ocv_mv[] = {
	3300, 3600, 3670, 3710, 3750, 3790, 3830, 3890, 3970, 4060, 4170,
};
#define OCV_STEPS ((int)(sizeof(ocv_mv) / sizeof(ocv_mv[0])) - 1)

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

int gauge_ocv_permille(int ocv)
{
	if (ocv <= ocv_mv[0]) {
		return 0;
	}
	for (int i = 0; i < OCV_STEPS; i++) {
		if (ocv < ocv_mv[i + 1]) {
			return (i * 1000 + 1000 * (ocv - ocv_mv[i]) / (ocv_mv[i + 1] - ocv_mv[i])) /
			       OCV_STEPS;
		}
	}
	return 1000;
}

static int64_t full_uams(const struct gauge *g)
{
	return (int64_t)g->p.capacity_uah * MS_PER_H;
}

int gauge_permille(const struct gauge *g)
{
	return (int)(g->charge_uams * 1000 / full_uams(g));
}

static void set_permille(struct gauge *g, int permille)
{
	g->charge_uams = full_uams(g) * clamp(permille, 0, 1000) / 1000;
}

/* The voltage without the drop (or rise, while charging) across the cell's resistance. */
static int ocv_estimate(const struct gauge *g, int v_mv, int i_ua)
{
	return v_mv + (int)((int64_t)i_ua * g->p.r_mohm / 1000000);
}

void gauge_init(struct gauge *g, const struct gauge_params *p, int v_mv, int i_ua,
		int saved_permille)
{
	int from_v;

	g->p = *p;
	g->avg_ua = 0;
	g->avg_ms = 0;
	g->charging = false;
	g->rest_ms = 0;
	/*
	 * While charging, the full resistance overcorrects (the 1 ohm includes
	 * the cell's slower polarisation): take half of it.
	 */
	from_v = gauge_ocv_permille(i_ua < 0 ? v_mv + (int)((int64_t)i_ua * p->r_mohm / 2000000)
					     : ocv_estimate(g, v_mv, i_ua));
	/* A stored state is better than one reading, unless they disagree a lot. */
	if (saved_permille >= 0 && saved_permille <= 1000 &&
	    (saved_permille - from_v < 200 && from_v - saved_permille < 200)) {
		set_permille(g, saved_permille);
	} else {
		set_permille(g, from_v);
	}
}

void gauge_update(struct gauge *g, int v_mv, int i_ua, enum gauge_charger ch, uint32_t dt_ms)
{
	int64_t full = full_uams(g);

	g->charge_uams -= (int64_t)i_ua * dt_ms;
	if (ch == GAUGE_FULL) {
		g->charge_uams = full;
	}
	if (g->charge_uams < 0) {
		g->charge_uams = 0;
	} else if (g->charge_uams > full) {
		g->charge_uams = full;
	}
	/*
	 * Average current: restarts when charging starts or stops, a plain mean
	 * until it covers AVG_MS, then a moving average with that time constant.
	 */
	bool charging = ch == GAUGE_CHARGE_CC || ch == GAUGE_CHARGE_CV;

	if (charging != g->charging) {
		g->charging = charging;
		g->avg_ms = 0;
	}
	g->avg_ms = g->avg_ms + dt_ms < AVG_MS ? g->avg_ms + dt_ms : AVG_MS;
	g->avg_ua += (int32_t)(((int64_t)i_ua - g->avg_ua) * (int64_t)dt_ms / g->avg_ms);

	if (ch != GAUGE_DISCHARGING || i_ua > REST_UA) {
		g->rest_ms = 0;
		return;
	}
	g->rest_ms += dt_ms;
	if (g->rest_ms < REST_MS) {
		return;
	}
	/* Nearly at rest: pull the count towards the voltage estimate. */
	int from_v = gauge_ocv_permille(ocv_estimate(g, v_mv, i_ua));
	int now = gauge_permille(g);
	int err = from_v - now;
	int64_t tau = from_v < LOW_PERMILLE ? CORRECT_LOW_MS : CORRECT_MS;

	if (err > DEADBAND || err < -DEADBAND || (from_v < LOW_PERMILLE && err < 0)) {
		g->charge_uams += full * err / 1000 * (int64_t)dt_ms / tau;
	}
}

int gauge_soc(const struct gauge *g, enum gauge_charger ch)
{
	int soc = (gauge_permille(g) + 5) / 10;

	switch (ch) {
	case GAUGE_FULL:
		return 100;
	case GAUGE_CHARGE_CC:
		return soc > 95 ? 95 : soc;
	default:
		return soc > 99 ? 99 : soc;
	}
}

int gauge_tte_min(const struct gauge *g, enum gauge_charger ch)
{
	if (ch != GAUGE_DISCHARGING || g->avg_ua < 1000) {
		return -1;
	}
	return (int)(g->charge_uams / g->avg_ua / 60000);
}

int gauge_ttf_min(const struct gauge *g, enum gauge_charger ch)
{
	int permille = gauge_permille(g);
	/* The measured charge current, or the charger's setting while it is still small. */
	int ua = -g->avg_ua > g->p.charge_ua / 4 ? -g->avg_ua : g->p.charge_ua;

	if (ch == GAUGE_CHARGE_CC) {
		/* Constant current up to the constant-voltage phase, then its tail. */
		int64_t cc = full_uams(g) * (CV_PERMILLE - (permille < CV_PERMILLE ? permille
										  : CV_PERMILLE)) /
			     1000;

		return (int)(cc / (ua > 0 ? ua : 1) / 60000) + CV_TAIL_MIN;
	}
	if (ch == GAUGE_CHARGE_CV) {
		/* The tail shrinks with the remaining charge. */
		int left = 1000 - (permille < CV_PERMILLE ? CV_PERMILLE : permille);

		return CV_TAIL_MIN * left / (1000 - CV_PERMILLE);
	}
	return -1;
}
