/* Crypto tests: FIPS-197 / RFC 7748 vectors and vectors from gen_vectors.py. */

#include <string.h>

#include "meshtastic/crypto.h"
#include "test.h"
#include "vectors.h"

static void hex(const char *s, uint8_t *out)
{
	for (; s[0] && s[1]; s += 2) {
		unsigned v;

		sscanf(s, "%2x", &v);
		*out++ = v;
	}
}

void test_crypto(void)
{
	struct mt_aes aes;
	uint8_t key[32], in[16], out[256], exp[32];

	/* FIPS-197 appendix C. */
	for (int i = 0; i < 32; i++) {
		key[i] = i;
	}
	for (int i = 0; i < 16; i++) {
		in[i] = i * 0x11;
	}
	mt_aes_init(&aes, key, 16);
	mt_aes_encrypt(&aes, in, out);
	hex("69c4e0d86a7b0430d8cdb78070b4c55a", exp);
	CHECK_MEM(out, exp, 16, "AES-128");
	mt_aes_init(&aes, key, 32);
	mt_aes_encrypt(&aes, in, out);
	hex("8ea2b7ca516745bfeafc49904b496089", exp);
	CHECK_MEM(out, exp, 16, "AES-256");

	/* SHA-256 */
	mt_sha256("abc", 3, out);
	hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", exp);
	CHECK_MEM(out, exp, 32, "SHA-256 abc");
	mt_sha256(sha_data, sizeof(sha_data), out);
	CHECK_MEM(out, sha_digest, 32, "SHA-256 1000 bytes");

	/* X25519, RFC 7748 section 5.2. */
	uint8_t scalar[32], point[32];

	hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar);
	hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point);
	hex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", exp);
	mt_x25519(out, scalar, point);
	CHECK_MEM(out, exp, 32, "X25519 RFC 7748");

	/* Key pairs and shared secret against Python. */
	mt_x25519_public(out, a_priv);
	CHECK_MEM(out, a_pub, 32, "X25519 public A");
	mt_x25519(out, b_priv, a_pub);
	CHECK_MEM(out, ab_secret, 32, "X25519 shared B*A");
	CHECK(mt_pki_shared_key(out, a_priv, b_pub), "shared key");
	CHECK_MEM(out, ab_shared, 32, "PKI shared key");

	/* Channel encryption (Meshtastic default key, AES-128-CTR), both ways. */
	static const uint8_t default_psk[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
						0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

	memcpy(out, ch_plain, sizeof(ch_plain));
	mt_channel_crypt(default_psk, 16, CH_FROM, CH_ID, out, sizeof(ch_plain));
	CHECK_MEM(out, ch_cipher, sizeof(ch_cipher), "channel AES-128-CTR");
	mt_channel_crypt(default_psk, 16, CH_FROM, CH_ID, out, sizeof(ch_plain));
	CHECK_MEM(out, ch_plain, sizeof(ch_plain), "channel decrypt");
	memcpy(out, ch_plain, sizeof(ch_plain));
	mt_channel_crypt(ch256_key, 32, CH_FROM, CH_ID, out, sizeof(ch_plain));
	CHECK_MEM(out, ch256_cipher, sizeof(ch256_cipher), "channel AES-256-CTR");

	/* Direct message encryption (AES-256-CCM) and decryption, tamper check. */
	uint8_t plain[64];

	mt_pki_encrypt(ab_shared, CH_FROM, CH_ID, PKI_EXTRA, pki_plain, sizeof(pki_plain), out);
	CHECK_MEM(out, pki_cipher, sizeof(pki_cipher), "PKI encrypt");
	int n = mt_pki_decrypt(ab_shared, CH_FROM, CH_ID, pki_cipher, sizeof(pki_cipher), plain);

	CHECK(n == (int)sizeof(pki_plain), "PKI decrypt length %d", n);
	CHECK_MEM(plain, pki_plain, sizeof(pki_plain), "PKI decrypt");
	memcpy(out, pki_cipher, sizeof(pki_cipher));
	out[3] ^= 1;
	CHECK(mt_pki_decrypt(ab_shared, CH_FROM, CH_ID, out, sizeof(pki_cipher), plain) < 0,
	      "tampered PKI message accepted");
}
