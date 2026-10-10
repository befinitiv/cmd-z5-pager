#include <math.h>
#include <string.h>

#include "synth/synth.h"

/* One voice at full volume nearly uses the full range; louder mixes clip. */
#define VOICE_SCALE (0.8f * 32767.0f)
/* Gap between melody notes, so repeated notes stay distinct. */
#define MELODY_GAP_MS 15

enum stage {
	ST_OFF,
	ST_ATTACK,
	ST_DECAY,
	ST_SUSTAIN,
	ST_RELEASE,
};

#define SINE_SIZE 256
static float sine_table[SINE_SIZE + 1];

static uint32_t ms_to_samples(const struct synth *s, uint32_t ms)
{
	return (uint32_t)((uint64_t)ms * s->rate / 1000);
}

/* Envelope step per sample for a ramp over ms (whole range 0..1). */
static float rate_per_sample(const struct synth *s, uint32_t ms)
{
	uint32_t n = ms_to_samples(s, ms);

	return n ? 1.0f / n : 1.0f;
}

void synth_init(struct synth *s, uint32_t rate_hz)
{
	memset(s, 0, sizeof(*s));
	s->rate = rate_hz;
	if (sine_table[SINE_SIZE / 4] == 0.0f) {
		for (int i = 0; i <= SINE_SIZE; i++) {
			sine_table[i] = sinf(2.0f * 3.14159265f * i / SINE_SIZE);
		}
	}
}

static void voice_start(struct synth *s, struct synth_voice *v, const struct synth_timbre *t,
			uint32_t freq, uint32_t hold_samples, uint8_t volume)
{
	v->timbre = t;
	v->inc = (float)freq / s->rate;
	v->gain = (volume > 100 ? 100 : volume) / 100.0f;
	v->hold = hold_samples;
	v->age = 0;
	v->lfsr = 0x7fff;
	/* Keep the phase (and level) of a voice that still sounds: no click. */
	if (v->stage == ST_OFF) {
		v->phase = 0.0f;
		v->level = 0.0f;
	}
	v->stage = ST_ATTACK;
}

void synth_note(struct synth *s, const struct synth_timbre *t, uint32_t freq_hz, uint32_t ms,
		uint8_t volume)
{
	struct synth_voice *pick = NULL;

	if (freq_hz == 0 || volume == 0) {
		return;
	}
	/* A free voice, else the one playing longest. Voice 0 is the melody's. */
	for (int i = 1; i < SYNTH_VOICES; i++) {
		struct synth_voice *v = &s->voice[i];

		if (v->stage == ST_OFF) {
			pick = v;
			break;
		}
		if (!pick || v->age > pick->age) {
			pick = v;
		}
	}
	voice_start(s, pick, t, freq_hz, ms_to_samples(s, ms), volume);
}

void synth_melody(struct synth *s, const struct synth_timbre *t, const struct synth_note *notes,
		  size_t count, uint8_t volume)
{
	struct synth_voice *v = &s->voice[0];

	v->timbre = t;
	v->notes = notes;
	v->count = count;
	v->pos = 0;
	v->step_left = 0;
	v->melody_volume = volume;
	if (v->stage != ST_OFF) {
		v->stage = ST_RELEASE;
	}
}

void synth_stop(struct synth *s)
{
	for (int i = 0; i < SYNTH_VOICES; i++) {
		s->voice[i].stage = ST_OFF;
		s->voice[i].notes = NULL;
		s->voice[i].count = 0;
	}
}

bool synth_active(const struct synth *s)
{
	for (int i = 0; i < SYNTH_VOICES; i++) {
		const struct synth_voice *v = &s->voice[i];

		if (v->stage != ST_OFF || v->pos < v->count) {
			return true;
		}
	}
	return false;
}

/* Next melody step on voice 0 when the current one is over. */
static void melody_step(struct synth *s, struct synth_voice *v)
{
	if (v->step_left > 0) {
		v->step_left--;
		return;
	}
	if (v->pos >= v->count) {
		return;
	}
	const struct synth_note *n = &v->notes[v->pos++];
	uint32_t len = ms_to_samples(s, n->ms);
	uint32_t gap = ms_to_samples(s, n->ms > 2 * MELODY_GAP_MS ? MELODY_GAP_MS : 0);

	v->step_left = len > 0 ? len - 1 : 0;
	if (n->freq && v->melody_volume) {
		voice_start(s, v, v->timbre, n->freq, len - gap, v->melody_volume);
	} else if (v->stage != ST_OFF) {
		v->stage = ST_RELEASE;
	}
}

/*
 * PolyBLEP: smooths the jumps of the pulse wave over one sample, which
 * removes most of the aliasing of a naive square at this low sample rate.
 */
static float poly_blep(float t, float dt)
{
	if (t < dt) {
		t /= dt;
		return t + t - t * t - 1.0f;
	}
	if (t > 1.0f - dt) {
		t = (t - 1.0f) / dt;
		return t * t + t + t + 1.0f;
	}
	return 0.0f;
}

static float oscillator(struct synth_voice *v)
{
	const struct synth_timbre *t = v->timbre;
	float p = v->phase;
	float out;

	switch (t->wave) {
	case SYNTH_PULSE: {
		float duty = (t->duty ? t->duty : 50) / 100.0f;
		float p2 = p + 1.0f - duty;

		if (p2 >= 1.0f) {
			p2 -= 1.0f;
		}
		out = (p < duty ? 1.0f : -1.0f) + poly_blep(p, v->inc) - poly_blep(p2, v->inc);
		/* Remove the DC part of an uneven duty cycle. */
		out -= 2.0f * duty - 1.0f;
		break;
	}
	case SYNTH_TRIANGLE:
		out = 4.0f * fabsf(p - 0.5f) - 1.0f;
		break;
	case SYNTH_SINE: {
		float x = p * SINE_SIZE;
		int i = (int)x;

		out = sine_table[i] + (sine_table[i + 1] - sine_table[i]) * (x - i);
		break;
	}
	default: /* SYNTH_NOISE */
		out = v->noise;
		break;
	}

	v->phase += v->inc;
	if (v->phase >= 1.0f) {
		v->phase -= 1.0f;
		if (t->wave == SYNTH_NOISE) {
			/* 15-bit LFSR, as in the Game Boy noise channel. */
			uint16_t bit = (v->lfsr ^ (v->lfsr >> 1)) & 1;

			v->lfsr = (v->lfsr >> 1) | (bit << 14);
			v->noise = (v->lfsr & 1) ? 1.0f : -1.0f;
		}
	}
	return out;
}

static void envelope(const struct synth *s, struct synth_voice *v)
{
	const struct synth_timbre *t = v->timbre;
	float sustain = t->sustain / 100.0f;

	switch (v->stage) {
	case ST_ATTACK:
		v->level += rate_per_sample(s, t->attack_ms);
		if (v->level >= 1.0f) {
			v->level = 1.0f;
			v->stage = ST_DECAY;
		}
		break;
	case ST_DECAY:
		v->level -= rate_per_sample(s, t->decay_ms);
		if (v->level <= sustain) {
			v->level = sustain;
			v->stage = ST_SUSTAIN;
		}
		break;
	case ST_RELEASE:
		v->level -= rate_per_sample(s, t->release_ms);
		if (v->level <= 0.0f) {
			v->level = 0.0f;
			v->stage = ST_OFF;
		}
		return;
	default:
		break;
	}
	if (v->stage != ST_OFF && v->stage != ST_RELEASE) {
		if (v->hold > 0) {
			v->hold--;
		} else {
			v->stage = ST_RELEASE;
		}
	}
}

bool synth_render(struct synth *s, int16_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float mix = 0.0f;

		melody_step(s, &s->voice[0]);
		for (int k = 0; k < SYNTH_VOICES; k++) {
			struct synth_voice *v = &s->voice[k];

			if (v->stage == ST_OFF) {
				continue;
			}
			mix += oscillator(v) * v->level * v->gain;
			envelope(s, v);
			v->age++;
		}
		mix *= VOICE_SCALE;
		out[i] = mix > 32767.0f ? 32767 : mix < -32768.0f ? -32768 : (int16_t)mix;
	}
	return synth_active(s);
}
