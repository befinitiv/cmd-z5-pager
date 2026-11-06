/*
 * Ed25519 (RFC 8032) with 16-bit limbs, after the public-domain TweetNaCl
 * design: signing our adverts, verifying received ones. Keys in the
 * orlp/ed25519 format that MeshCore stores: the private key is SHA-512 of the
 * seed, clamped. Constant time where secrets are involved; small rather than
 * fast (a verification takes some 100 ms on the nRF52840).
 */

#include <string.h>

#include "meshcore/crypto.h"
#include "meshtastic/crypto.h"

typedef int64_t gf[16];

static const gf gf0;
static const gf gf1 = {1};
static const gf D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
		     0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const gf D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
		      0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
/* The base point. */
static const gf X = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
		     0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf Y = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
		     0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
/* sqrt(-1) */
static const gf I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
		     0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
/* The group order. */
static const int64_t L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
			      0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
			      0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

/* Field arithmetic mod 2^255 - 19 ----------------------------------------------- */

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
static void sel(gf p, gf q, int b)
{
	int64_t mask = ~((int64_t)b - 1);

	for (int i = 0; i < 16; i++) {
		int64_t t = mask & (p[i] ^ q[i]);

		p[i] ^= t;
		q[i] ^= t;
	}
}

static void pack25519(uint8_t *o, const gf n)
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
		sel(t, m, 1 - b);
	}
	for (int i = 0; i < 16; i++) {
		o[2 * i] = t[i] & 0xff;
		o[2 * i + 1] = t[i] >> 8;
	}
}

static void unpack25519(gf o, const uint8_t *n)
{
	for (int i = 0; i < 16; i++) {
		o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
	}
	o[15] &= 0x7fff;
}

static bool neq(const gf a, const gf b)
{
	uint8_t c[32], d[32];
	uint8_t diff = 0;

	pack25519(c, a);
	pack25519(d, b);
	for (int i = 0; i < 32; i++) {
		diff |= c[i] ^ d[i];
	}
	return diff != 0;
}

static uint8_t parity(const gf a)
{
	uint8_t d[32];

	pack25519(d, a);
	return d[0] & 1;
}

static void add_f(gf o, const gf a, const gf b)
{
	for (int i = 0; i < 16; i++) {
		o[i] = a[i] + b[i];
	}
}

static void sub_f(gf o, const gf a, const gf b)
{
	for (int i = 0; i < 16; i++) {
		o[i] = a[i] - b[i];
	}
}

static void mul_f(gf o, const gf a, const gf b)
{
	int64_t t[31] = {0};

	/* The limbs fit in 32 bits: a single 32x32->64 multiply-accumulate each. */
	for (int i = 0; i < 16; i++) {
		for (int j = 0; j < 16; j++) {
			t[i + j] += (int64_t)(int32_t)a[i] * (int32_t)b[j];
		}
	}
	for (int i = 0; i < 15; i++) {
		t[i] += 38 * t[i + 16];
	}
	memcpy(o, t, sizeof(gf));
	carry(o);
	carry(o);
}

static void sqr_f(gf o, const gf a)
{
	mul_f(o, a, a);
}

static void inv_f(gf o, const gf in)
{
	gf c;

	memcpy(c, in, sizeof(gf));
	for (int a = 253; a >= 0; a--) {
		sqr_f(c, c);
		if (a != 2 && a != 4) {
			mul_f(c, c, in);
		}
	}
	memcpy(o, c, sizeof(gf));
}

/* in^((p - 5) / 8), for the square root. */
static void pow2523(gf o, const gf in)
{
	gf c;

	memcpy(c, in, sizeof(gf));
	for (int a = 250; a >= 0; a--) {
		sqr_f(c, c);
		if (a != 1) {
			mul_f(c, c, in);
		}
	}
	memcpy(o, c, sizeof(gf));
}

/* Points in extended coordinates (X, Y, Z, T) ------------------------------------- */

static void point_add(gf p[4], gf q[4])
{
	gf a, b, c, d, t, e, f, g, h;

	sub_f(a, p[1], p[0]);
	sub_f(t, q[1], q[0]);
	mul_f(a, a, t);
	add_f(b, p[0], p[1]);
	add_f(t, q[0], q[1]);
	mul_f(b, b, t);
	mul_f(c, p[3], q[3]);
	mul_f(c, c, D2);
	mul_f(d, p[2], q[2]);
	add_f(d, d, d);
	sub_f(e, b, a);
	sub_f(f, d, c);
	add_f(g, d, c);
	add_f(h, b, a);
	mul_f(p[0], e, f);
	mul_f(p[1], h, g);
	mul_f(p[2], g, f);
	mul_f(p[3], e, h);
}

static void point_swap(gf p[4], gf q[4], int b)
{
	for (int i = 0; i < 4; i++) {
		sel(p[i], q[i], b);
	}
}

static void point_pack(uint8_t r[32], gf p[4])
{
	gf tx, ty, zi;

	inv_f(zi, p[2]);
	mul_f(tx, p[0], zi);
	mul_f(ty, p[1], zi);
	pack25519(r, ty);
	r[31] ^= parity(tx) << 7;
}

static void scalarmult(gf p[4], gf q[4], const uint8_t s[32])
{
	memcpy(p[0], gf0, sizeof(gf));
	memcpy(p[1], gf1, sizeof(gf));
	memcpy(p[2], gf1, sizeof(gf));
	memcpy(p[3], gf0, sizeof(gf));
	for (int i = 255; i >= 0; i--) {
		int b = (s[i / 8] >> (i & 7)) & 1;

		point_swap(p, q, b);
		point_add(q, p);
		point_add(p, p);
		point_swap(p, q, b);
	}
}

static void scalarbase(gf p[4], const uint8_t s[32])
{
	gf q[4];

	memcpy(q[0], X, sizeof(gf));
	memcpy(q[1], Y, sizeof(gf));
	memcpy(q[2], gf1, sizeof(gf));
	mul_f(q[3], X, Y);
	scalarmult(p, q, s);
}

/* -A from its encoding; false if it is not a point. */
static bool unpack_neg(gf r[4], const uint8_t p[32])
{
	gf t, chk, num, den, den2, den4, den6;

	memcpy(r[2], gf1, sizeof(gf));
	unpack25519(r[1], p);
	sqr_f(num, r[1]);
	mul_f(den, num, D);
	sub_f(num, num, r[2]);
	add_f(den, r[2], den);
	sqr_f(den2, den);
	sqr_f(den4, den2);
	mul_f(den6, den4, den2);
	mul_f(t, den6, num);
	mul_f(t, t, den);
	pow2523(t, t);
	mul_f(t, t, num);
	mul_f(t, t, den);
	mul_f(t, t, den);
	mul_f(r[0], t, den);
	sqr_f(chk, r[0]);
	mul_f(chk, chk, den);
	if (neq(chk, num)) {
		mul_f(r[0], r[0], I);
	}
	sqr_f(chk, r[0]);
	mul_f(chk, chk, den);
	if (neq(chk, num)) {
		return false;
	}
	if (parity(r[0]) == (p[31] >> 7)) {
		sub_f(r[0], gf0, r[0]);
	}
	mul_f(r[3], r[0], r[1]);
	return true;
}

/* Scalars mod L --------------------------------------------------------------------- */

static void mod_l(uint8_t r[32], int64_t x[64])
{
	int64_t c;
	int i, j;

	for (i = 63; i >= 32; i--) {
		c = 0;
		for (j = i - 32; j < i - 12; j++) {
			x[j] += c - 16 * x[i] * L[j - (i - 32)];
			c = (x[j] + 128) >> 8;
			x[j] -= c * 256;
		}
		x[j] += c;
		x[i] = 0;
	}
	c = 0;
	for (j = 0; j < 32; j++) {
		x[j] += c - (x[31] >> 4) * L[j];
		c = x[j] >> 8;
		x[j] &= 255;
	}
	for (j = 0; j < 32; j++) {
		x[j] -= c * L[j];
	}
	for (i = 0; i < 32; i++) {
		x[i + 1] += x[i] >> 8;
		r[i] = x[i] & 255;
	}
}

/* r (64 bytes) mod L, into r[0..31]. */
static void reduce(uint8_t r[64])
{
	int64_t x[64];

	for (int i = 0; i < 64; i++) {
		x[i] = r[i];
	}
	memset(r, 0, 64);
	mod_l(r, x);
}

/* True if the scalar is below L (canonical signature). */
static bool below_l(const uint8_t s[32])
{
	for (int i = 31; i >= 0; i--) {
		if (s[i] != L[i]) {
			return s[i] < L[i];
		}
	}
	return false;
}

/* API -------------------------------------------------------------------------------- */

void mc_ed25519_keypair(uint8_t pub[32], uint8_t prv[64], const uint8_t seed[32])
{
	struct mc_sha512 h;
	gf p[4];

	mc_sha512_init(&h);
	mc_sha512_update(&h, seed, 32);
	mc_sha512_final(&h, prv);
	prv[0] &= 248;
	prv[31] &= 63;
	prv[31] |= 64;
	scalarbase(p, prv);
	point_pack(pub, p);
}

void mc_ed25519_sign(uint8_t sig[64], const void *msg, size_t len, const uint8_t pub[32],
		     const uint8_t prv[64])
{
	struct mc_sha512 h;
	uint8_t r[64], k[64];
	int64_t x[64];
	gf p[4];

	mc_sha512_init(&h);
	mc_sha512_update(&h, prv + 32, 32);
	mc_sha512_update(&h, msg, len);
	mc_sha512_final(&h, r);
	reduce(r);
	scalarbase(p, r);
	point_pack(sig, p);

	mc_sha512_init(&h);
	mc_sha512_update(&h, sig, 32);
	mc_sha512_update(&h, pub, 32);
	mc_sha512_update(&h, msg, len);
	mc_sha512_final(&h, k);
	reduce(k);
	/* S = r + k * a mod L */
	memset(x, 0, sizeof(x));
	for (int i = 0; i < 32; i++) {
		x[i] = r[i];
	}
	for (int i = 0; i < 32; i++) {
		for (int j = 0; j < 32; j++) {
			x[i + j] += k[i] * (int64_t)prv[j];
		}
	}
	mod_l(sig + 32, x);
}

bool mc_ed25519_verify(const uint8_t sig[64], const void *msg, size_t len, const uint8_t pub[32])
{
	struct mc_sha512 h;
	uint8_t k[64], t[32];
	gf p[4], q[4];

	if (!below_l(sig + 32) || !unpack_neg(q, pub)) {
		return false;
	}
	mc_sha512_init(&h);
	mc_sha512_update(&h, sig, 32);
	mc_sha512_update(&h, pub, 32);
	mc_sha512_update(&h, msg, len);
	mc_sha512_final(&h, k);
	reduce(k);
	/* R' = S B - k A; the signature holds if R' = R. */
	scalarmult(p, q, k);
	scalarbase(q, sig + 32);
	point_add(p, q);
	point_pack(t, p);
	return memcmp(t, sig, 32) == 0;
}

bool mc_shared_secret(uint8_t secret[32], const uint8_t my_prv[64], const uint8_t their_pub[32])
{
	gf y, one_plus, one_minus, u;
	uint8_t ub[32], any = 0;

	/* Edwards y to Montgomery u = (1 + y) / (1 - y). */
	unpack25519(y, their_pub);
	add_f(one_plus, gf1, y);
	sub_f(one_minus, gf1, y);
	inv_f(one_minus, one_minus);
	mul_f(u, one_plus, one_minus);
	pack25519(ub, u);
	mt_x25519(secret, my_prv, ub);
	for (int i = 0; i < 32; i++) {
		any |= secret[i];
	}
	return any != 0;
}
