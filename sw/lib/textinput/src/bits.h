/*
 * MSB-first bit reader and little-endian helpers for the model format.
 */

#pragma once

#include <stdint.h>

struct ti_bits {
	const uint8_t *data;
	uint32_t pos;
};

static inline uint32_t ti_get_u32(const uint8_t *p)
{
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t ti_get_u16(const uint8_t *p)
{
	return p[0] | (p[1] << 8);
}

static inline int16_t ti_get_i16(const uint8_t *p)
{
	return (int16_t)ti_get_u16(p);
}

static inline uint32_t ti_bit(struct ti_bits *b)
{
	uint32_t v = (b->data[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;

	b->pos++;
	return v;
}

static inline uint32_t ti_read(struct ti_bits *b, int n)
{
	uint32_t v = 0;

	while (n--) {
		v = (v << 1) | ti_bit(b);
	}
	return v;
}

static inline uint32_t ti_rice(struct ti_bits *b, int k)
{
	uint32_t q = 0;

	while (ti_bit(b)) {
		q++;
	}
	return (q << k) | ti_read(b, k);
}

static inline uint32_t ti_gamma(struct ti_bits *b)
{
	int zeros = 0;

	while (!ti_bit(b)) {
		zeros++;
	}
	return (1u << zeros) | ti_read(b, zeros);
}
