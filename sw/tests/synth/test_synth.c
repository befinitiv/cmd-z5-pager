/*
 * Host tests of lib/synth: pitch, envelopes, melody timing, voice mixing.
 * Also writes build-tests/synth_demo.wav to listen to the timbres.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synth/synth.h"

#define RATE 15625

static int failures;

#define CHECK(cond, ...)                                                                   \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
			failures++;                                                        \
		}                                                                          \
	} while (0)

static const struct synth_timbre square = {SYNTH_PULSE, 50, 1, 0, 100, 5};
static const struct synth_timbre sine = {SYNTH_SINE, 0, 2, 0, 100, 10};
static const struct synth_timbre pluck = {SYNTH_PULSE, 25, 2, 120, 60, 30};
static const struct synth_timbre noise = {SYNTH_NOISE, 0, 1, 40, 0, 20};

static int16_t buf[RATE * 4];

/* Frequency from upward zero crossings over [from, to). */
static float measure_freq(const int16_t *b, int from, int to)
{
	int crossings = 0, first = -1, last = -1;

	for (int i = from + 1; i < to; i++) {
		if (b[i - 1] < 0 && b[i] >= 0) {
			if (first < 0) {
				first = i;
			}
			last = i;
			crossings++;
		}
	}
	return crossings > 1 ? (float)(crossings - 1) * RATE / (last - first) : 0.0f;
}

static int peak(const int16_t *b, int from, int to)
{
	int p = 0;

	for (int i = from; i < to; i++) {
		p = abs(b[i]) > p ? abs(b[i]) : p;
	}
	return p;
}

static void write_wav(const char *path, const int16_t *b, int n)
{
	FILE *f = fopen(path, "wb");
	uint32_t size = n * 2, rate = RATE, byte_rate = RATE * 2, fmt_len = 16;
	uint16_t pcm = 1, ch = 1, align = 2, bits = 16;

	if (!f) {
		return;
	}
	fwrite("RIFF", 1, 4, f);
	uint32_t riff = 36 + size;
	fwrite(&riff, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmt_len, 4, 1, f);
	fwrite(&pcm, 2, 1, f);
	fwrite(&ch, 2, 1, f);
	fwrite(&rate, 4, 1, f);
	fwrite(&byte_rate, 4, 1, f);
	fwrite(&align, 2, 1, f);
	fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&size, 4, 1, f);
	fwrite(b, 2, n, f);
	fclose(f);
}

int main(void)
{
	struct synth s;

	/* Pitch of a sine and a square note. */
	synth_init(&s, RATE);
	synth_note(&s, &sine, 440, 500, 100);
	synth_render(&s, buf, RATE / 2);
	float f = measure_freq(buf, RATE / 10, RATE / 2 - 200);
	CHECK(fabsf(f - 440) < 3, "sine %.1f Hz", f);
	CHECK(peak(buf, 1000, 5000) > 20000, "sine level %d", peak(buf, 1000, 5000));

	synth_init(&s, RATE);
	synth_note(&s, &square, 1000, 300, 50);
	synth_render(&s, buf, RATE / 4);
	f = measure_freq(buf, 200, RATE / 4 - 200);
	CHECK(fabsf(f - 1000) < 5, "square %.1f Hz", f);
	/* Volume 50: about half of full scale. */
	CHECK(peak(buf, 200, 2000) > 11000 && peak(buf, 200, 2000) < 17000, "square 50%% %d",
	      peak(buf, 200, 2000));

	/* The note ends after its time plus the release, then the synth is idle. */
	synth_init(&s, RATE);
	synth_note(&s, &square, 800, 100, 100);
	bool active = synth_render(&s, buf, RATE * 100 / 1000 + RATE * 4 / 1000);
	CHECK(active, "idle during the release");
	active = synth_render(&s, buf, RATE * 10 / 1000);
	CHECK(!active, "still active after the release");
	/* The last millisecond of the release is at the start of this block. */
	CHECK(peak(buf, RATE * 2 / 1000, RATE * 10 / 1000) < 100, "sound after the end %d",
	      peak(buf, RATE * 2 / 1000, RATE * 10 / 1000));

	/* Melody timing: three 100 ms steps, the last one a rest. */
	static const struct synth_note mel[] = {{523, 100}, {659, 100}, {0, 100}};

	synth_init(&s, RATE);
	synth_melody(&s, &square, mel, 3, 80);
	synth_render(&s, buf, RATE * 300 / 1000);
	f = measure_freq(buf, RATE * 10 / 1000, RATE * 80 / 1000);
	CHECK(fabsf(f - 523) < 6, "melody note 1 %.1f Hz", f);
	f = measure_freq(buf, RATE * 110 / 1000, RATE * 180 / 1000);
	CHECK(fabsf(f - 659) < 6, "melody note 2 %.1f Hz", f);
	CHECK(peak(buf, RATE * 220 / 1000, RATE * 300 / 1000) < 100, "rest not silent");
	CHECK(!synth_active(&s) || !synth_render(&s, buf, 10), "melody not finished");

	/* A note during a melody mixes with it instead of replacing it. */
	synth_init(&s, RATE);
	static const struct synth_note long_note[] = {{262, 400}};
	synth_melody(&s, &sine, long_note, 1, 60);
	synth_render(&s, buf, 500);
	synth_note(&s, &sine, 1047, 200, 60);
	synth_render(&s, buf, 1000);
	int above = 0;
	for (int i = 1; i < 1000; i++) {
		above += (buf[i - 1] < 0 && buf[i] >= 0);
	}
	/* 1047 Hz dominates the crossings, 262 Hz is still underneath. */
	CHECK(s.voice[0].stage != 0 && above > 50, "mix: melody stage %d, crossings %d",
	      s.voice[0].stage, above);

	/* All four voices at once stay within range (clipped, no overflow). */
	synth_init(&s, RATE);
	for (int i = 0; i < 5; i++) {
		synth_note(&s, &square, 300 + 100 * i, 200, 100);
	}
	synth_render(&s, buf, 2000);
	CHECK(peak(buf, 0, 2000) <= 32768, "overflow");

	/* Stop silences at once. */
	synth_stop(&s);
	CHECK(!synth_render(&s, buf, 100) && peak(buf, 0, 100) == 0, "stop");

	/* Noise: many sign changes, decays to silence (sustain 0). */
	synth_init(&s, RATE);
	synth_note(&s, &noise, 4000, 300, 80);
	synth_render(&s, buf, RATE / 10);
	CHECK(peak(buf, 0, 300) > 5000, "noise level %d", peak(buf, 0, 300));
	CHECK(peak(buf, RATE * 60 / 1000, RATE / 10) < 100, "noise did not decay");

	/* Demo: the timbres one after another. */
	int n = 0;
	static const struct synth_note demo[] = {
		{523, 150}, {659, 150}, {784, 150}, {1047, 300}, {0, 150},
	};
	const struct synth_timbre *timbres[] = {&square, &pluck, &sine};
	for (int t = 0; t < 3; t++) {
		synth_init(&s, RATE);
		synth_melody(&s, timbres[t], demo, 5, 80);
		synth_render(&s, buf + n, RATE * 900 / 1000);
		n += RATE * 900 / 1000;
	}
	synth_init(&s, RATE);
	synth_note(&s, &noise, 3000, 60, 80);
	synth_render(&s, buf + n, RATE / 5);
	n += RATE / 5;
	write_wav("build-tests/synth_demo.wav", buf, n);

	printf(failures ? "%d failures\n" : "OK (0 failures)\n", failures);
	return failures != 0;
}
