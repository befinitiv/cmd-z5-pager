/*
 * Fuel gauge against a simulated cell that is worse than the gauge assumes
 * (950 instead of 1100 mAh, 240 instead of 200 mOhm) and a current
 * measurement with an offset, over a realistic day: radio receiving and
 * display (8 mA), the GPS for 120 s every 17 min (165 mA), now and then a
 * transmission.
 */

#include <stdio.h>
#include <stdlib.h>

#include "gauge.h"

static int failures;

#define CHECK(cond, ...)                                                                   \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL line %d: ", __LINE__);                                \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
			failures++;                                                        \
		}                                                                          \
	} while (0)

/* The simulated cell: true charge and voltage. */
#define TRUE_UAH  950000
#define TRUE_MOHM 240
#define OFFSET_UA 500

static int64_t true_uams;

static int true_permille(void)
{
	return (int)(true_uams * 1000 / (TRUE_UAH * 3600000LL));
}

/* Same curve shape as the gauge's table, over the true capacity. */
static int true_ocv(void)
{
	static const int ocv[] = {3300, 3600, 3670, 3710, 3750, 3790, 3830, 3890, 3970, 4060, 4170};
	int pos = true_permille() * 10;
	int i = pos / 1000 >= 10 ? 9 : pos / 1000;

	return ocv[i] + (ocv[i + 1] - ocv[i]) * (pos - i * 1000) / 1000;
}

static const struct gauge_params params = {
	.capacity_uah = 1100000,
	.r_mohm = 200,
	.charge_ua = 400000,
};

/* Load at time t (s) of a day: base, GPS bursts, transmissions. */
static int load_ua(int t)
{
	int ua = 8000;

	if (t % 1020 < 120) {
		ua += 165000;
	}
	if (t % 900 == 450) {
		ua += 100000;
	}
	return ua;
}

static void step(struct gauge *g, int ua, enum gauge_charger ch)
{
	true_uams -= (int64_t)ua * 1000;
	if (true_uams < 0) {
		true_uams = 0;
	}
	int v = true_ocv() - (int)((int64_t)ua * TRUE_MOHM / 1000000);
	int noise = rand() % 2001 - 1000;

	gauge_update(g, v, ua + OFFSET_UA + noise, ch, 1000);
}

int main(void)
{
	struct gauge g;
	int worst_over = 0, at_true_empty = -1;

	/* OCV table round trip. */
	CHECK(gauge_ocv_permille(3300) == 0 && gauge_ocv_permille(4170) == 1000 &&
		      gauge_ocv_permille(3790) == 500,
	      "ocv table %d %d %d", gauge_ocv_permille(3300), gauge_ocv_permille(4170),
	      gauge_ocv_permille(3790));

	/* Start at rest, half full: from the voltage, within a few percent. */
	true_uams = TRUE_UAH * 3600000LL / 2;
	gauge_init(&g, &params, true_ocv(), 0, -1);
	CHECK(abs(gauge_permille(&g) - 500) < 30, "start at 50 %%: %d ‰", gauge_permille(&g));

	/* A stored state that disagrees by more than 20 % is not used. */
	gauge_init(&g, &params, true_ocv(), 0, 900);
	CHECK(abs(gauge_permille(&g) - 500) < 30, "bad stored state used: %d ‰", gauge_permille(&g));

	/* Charge: constant current up to ~85 %, then constant voltage, then full. */
	for (int t = 0; true_permille() < 1000; t++) {
		int ua = true_permille() < 850 ? -400000 : -40000 - 360000 * (1000 - true_permille()) / 150;
		enum gauge_charger ch = true_permille() < 850 ? GAUGE_CHARGE_CC : GAUGE_CHARGE_CV;

		step(&g, ua, ch);
		CHECK(ch != GAUGE_CHARGE_CC || gauge_soc(&g, ch) <= 95, "%d %% while CC",
		      gauge_soc(&g, ch));
		if (t == 600) {
			int ttf = gauge_ttf_min(&g, ch);

			CHECK(ttf > 30 && ttf < 150, "time to full while charging: %d min", ttf);
		}
		if (t > 6 * 3600) {
			CHECK(0, "charge never ended");
			break;
		}
	}
	step(&g, 0, GAUGE_FULL);
	CHECK(gauge_soc(&g, GAUGE_FULL) == 100, "full: %d %%", gauge_soc(&g, GAUGE_FULL));

	/*
	 * Discharge over a day. The gauge assumes 1100 mAh, the cell has 950: the
	 * count runs ahead of the truth and the voltage correction has to catch
	 * up, at the latest near empty.
	 */
	for (int t = 0; t < 48 * 3600; t++) {
		step(&g, load_ua(t), GAUGE_DISCHARGING);
		int over = gauge_permille(&g) - true_permille();

		if (true_permille() > 100 && over > worst_over) {
			worst_over = over;
		}
		if (t == 3 * 3600) {
			int tte = gauge_tte_min(&g, GAUGE_DISCHARGING);

			/* Average load about 27 mA: about 30 h from ~95 %. */
			CHECK(tte > 15 * 60 && tte < 45 * 60, "time to empty %d min", tte);
		}
		if (true_permille() == 0) {
			at_true_empty = gauge_permille(&g);
			break;
		}
	}
	printf("discharge: worst overestimate %d ‰ above 10 %%, at true empty %d ‰\n",
	       worst_over, at_true_empty);
	CHECK(worst_over < 200, "overestimate %d ‰", worst_over);
	CHECK(at_true_empty >= 0 && at_true_empty <= 60, "at true empty the gauge shows %d ‰",
	      at_true_empty);

	printf(failures ? "%d failures\n" : "OK\n", failures);
	return failures != 0;
}
