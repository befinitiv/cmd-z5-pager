/*
 * Cryptography for the MeshCore protocol, in plain C: SHA-512 and Ed25519
 * here, AES, SHA-256 and X25519 from lib/meshtastic.
 *
 * Identities are Ed25519 key pairs. Peers agree on a secret by X25519 with
 * the Edwards public key converted to Montgomery form (as orlp/ed25519's
 * ed25519_key_exchange()); data is AES-128-ECB (zero-padded) with a 2-byte
 * HMAC-SHA256 in front ("encrypt then MAC").
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mc_sha512 {
	uint64_t h[8];
	uint64_t len;
	uint8_t buf[128];
	size_t fill;
};

void mc_sha512_init(struct mc_sha512 *s);
void mc_sha512_update(struct mc_sha512 *s, const void *data, size_t len);
void mc_sha512_final(struct mc_sha512 *s, uint8_t out[64]);

#define MC_PUB_KEY_SIZE   32
#define MC_PRV_KEY_SIZE   64 /* SHA-512 of the seed, clamped (orlp/ed25519 format) */
#define MC_SIGNATURE_SIZE 64

/* Key pair from 32 random bytes. */
void mc_ed25519_keypair(uint8_t pub[32], uint8_t prv[64], const uint8_t seed[32]);
void mc_ed25519_sign(uint8_t sig[64], const void *msg, size_t len, const uint8_t pub[32],
		     const uint8_t prv[64]);
bool mc_ed25519_verify(const uint8_t sig[64], const void *msg, size_t len, const uint8_t pub[32]);
/* Shared secret with a peer (32 bytes, used as is); false for weak keys. */
bool mc_shared_secret(uint8_t secret[32], const uint8_t my_prv[64], const uint8_t their_pub[32]);

void mc_hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len,
		    uint8_t out[32]);

#define MC_MAC_SIZE   2
#define MC_CIPHER_KEY 16

/*
 * Encrypt len bytes (zero-padded to 16) with secret[0..15] and put a 2-byte
 * HMAC (key: the 32-byte secret) in front. Returns the output length.
 */
int mc_encrypt_then_mac(const uint8_t secret[32], uint8_t *out, const uint8_t *in, size_t len);
/* Check the MAC and decrypt; returns the plaintext length (padded) or 0. */
int mc_mac_then_decrypt(const uint8_t secret[32], uint8_t *out, const uint8_t *in, size_t len);

#ifdef __cplusplus
}
#endif
