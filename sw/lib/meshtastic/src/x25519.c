/*
 * X25519 (RFC 7748) with 16-bit limbs, after the public-domain TweetNaCl
 * design. Constant time; slow but small (one operation per direct message).
 */

#include <string.h>

#include "meshtastic/crypto.h"

typedef int64_t gf[16];

static const gf k121665 = {0xDB41, 1};

static void carry(gf o)
{
	for (int i = 0; i < 16; i++) {
		int64_t c;

		o[i] += (int64_t)1 << 16;
		c = o[i] >> 16;
		o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
		o[i] -= c * ((int64_t)1 << 16);
	}
}

/* Swap p and q if b == 1, in constant time. */
static void cswap(gf p, gf q, int b)
{
	int64_t mask = ~((int64_t)b - 1);

	for (int i = 0; i < 16; i++) {
		int64_t t = mask & (p[i] ^ q[i]);

		p[i] ^= t;
		q[i] ^= t;
	}
}

static void pack(uint8_t *o, const gf n)
{
	gf m, t;

	memcpy(t, n, sizeof(gf));
	carry(t);
	carry(t);
	carry(t);
	for (int j = 0; j < 2; j++) {
		int b;

		m[0] = t[0] - 0xffed;
		for (int i = 1; i < 15; i++) {
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		b = (m[15] >> 16) & 1;
		m[14] &= 0xffff;
		cswap(t, m, 1 - b);
	}
	for (int i = 0; i < 16; i++) {
		o[2 * i] = t[i] & 0xff;
		o[2 * i + 1] = t[i] >> 8;
	}
}

static void unpack(gf o, const uint8_t *n)
{
	for (int i = 0; i < 16; i++) {
		o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
	}
	o[15] &= 0x7fff;
}

static void add(gf o, const gf a, const gf b)
{
	for (int i = 0; i < 16; i++) {
		o[i] = a[i] + b[i];
	}
}

static void sub(gf o, const gf a, const gf b)
{
	for (int i = 0; i < 16; i++) {
		o[i] = a[i] - b[i];
	}
}

static void mul(gf o, const gf a, const gf b)
{
	int64_t t[31] = {0};

	for (int i = 0; i < 16; i++) {
		for (int j = 0; j < 16; j++) {
			t[i + j] += a[i] * b[j];
		}
	}
	for (int i = 0; i < 15; i++) {
		t[i] += 38 * t[i + 16];
	}
	memcpy(o, t, sizeof(gf));
	carry(o);
	carry(o);
}

static void sqr(gf o, const gf a)
{
	mul(o, a, a);
}

static void invert(gf o, const gf in)
{
	gf c;

	memcpy(c, in, sizeof(gf));
	for (int a = 253; a >= 0; a--) {
		sqr(c, c);
		if (a != 2 && a != 4) {
			mul(c, c, in);
		}
	}
	memcpy(o, c, sizeof(gf));
}

void mt_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
	uint8_t z[32];
	gf x, a, b, c, d, e, f;

	memcpy(z, scalar, 32);
	z[31] = (z[31] & 127) | 64;
	z[0] &= 248;
	unpack(x, point);
	memcpy(b, x, sizeof(gf));
	memset(a, 0, sizeof(gf));
	memset(c, 0, sizeof(gf));
	memset(d, 0, sizeof(gf));
	a[0] = d[0] = 1;
	for (int i = 254; i >= 0; i--) {
		int r = (z[i >> 3] >> (i & 7)) & 1;

		cswap(a, b, r);
		cswap(c, d, r);
		add(e, a, c);
		sub(a, a, c);
		add(c, b, d);
		sub(b, b, d);
		sqr(d, e);
		sqr(f, a);
		mul(a, c, a);
		mul(c, b, e);
		add(e, a, c);
		sub(a, a, c);
		sqr(b, a);
		sub(c, d, f);
		mul(a, c, k121665);
		add(a, a, d);
		mul(c, c, a);
		mul(a, d, f);
		mul(d, b, x);
		sqr(b, e);
		cswap(a, b, r);
		cswap(c, d, r);
	}
	invert(c, c);
	mul(a, a, c);
	pack(out, a);
}

void mt_x25519_public(uint8_t pub[32], const uint8_t priv[32])
{
	static const uint8_t base[32] = {9};

	mt_x25519(pub, priv, base);
}
