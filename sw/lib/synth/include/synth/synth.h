/*
 * Small software synthesiser for the loudspeaker and the earpiece.
 *
 * A few voices with chiptune waveforms (band-limited pulse, triangle, sine,
 * noise) and attack/decay/sustain/release envelopes, plus a melody sequencer
 * with sample-accurate timing, mixed into signed 16-bit mono samples.
 * Voice 0 plays melodies; single notes use the other voices, so effects and
 * a melody sound at the same time.
 *
 * Plain C, no Zephyr dependency. The caller serialises the calls: notes are
 * started from the application, synth_render() runs in the audio thread.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTH_VOICES 4

enum synth_wave {
	SYNTH_PULSE,    /* duty cycle from the timbre; 50 % = square */
	SYNTH_TRIANGLE,
	SYNTH_SINE,
	SYNTH_NOISE,    /* pseudo-random steps at the note frequency */
};

struct synth_timbre {
	uint8_t wave;        /* enum synth_wave */
	uint8_t duty;        /* pulse: high part in %, 1..99 */
	uint16_t attack_ms;  /* 0 to full level */
	uint16_t decay_ms;   /* full level down to the sustain level */
	uint8_t sustain;     /* % of full level, held until the note ends */
	uint16_t release_ms; /* down to 0 after the note ends */
};

/* A melody step; same layout as struct hw_note. */
struct synth_note {
	uint16_t freq; /* Hz, 0 = rest */
	uint16_t ms;
};

struct synth_voice {
	const struct synth_timbre *timbre;
	float phase; /* 0..1 */
	float inc;   /* cycles per sample */
	float level; /* envelope 0..1 */
	float gain;  /* volume 0..1 */
	uint8_t stage;
	uint32_t hold; /* samples until the release starts */
	uint32_t age;
	uint16_t lfsr;
	float noise;
	/* Melody (voice 0 only). */
	const struct synth_note *notes;
	size_t count, pos;
	uint32_t step_left; /* samples until the next step */
	uint8_t melody_volume;
};

struct synth {
	uint32_t rate;
	struct synth_voice voice[SYNTH_VOICES];
};

void synth_init(struct synth *s, uint32_t rate_hz);
/* Play a note on a free voice (or the oldest one). volume 0..100. */
void synth_note(struct synth *s, const struct synth_timbre *t, uint32_t freq_hz,
		uint32_t ms, uint8_t volume);
/* Play a note sequence on voice 0, replacing a running melody. */
void synth_melody(struct synth *s, const struct synth_timbre *t, const struct synth_note *notes,
		  size_t count, uint8_t volume);
/* Silence everything at once. */
void synth_stop(struct synth *s);
/* True while a voice sounds or a melody is pending. */
bool synth_active(const struct synth *s);
/* Mix the next n samples. Returns synth_active() after the block. */
bool synth_render(struct synth *s, int16_t *out, size_t n);

#ifdef __cplusplus
}
#endif
