/*
 * Cryptography for the Meshtastic protocol, in plain C without dependencies:
 * AES-128/256 (encryption direction only, enough for CTR and CCM), SHA-256
 * and X25519.
 *
 * Channel messages: AES-CTR with the channel key. Direct messages: AES-256-CCM
 * with SHA-256 of the X25519 shared secret ("PKI").
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mt_aes {
	uint8_t rk[240]; /* expanded key, (rounds + 1) x 16 bytes */
	int rounds;
};

/* key_len 16 or 32. Returns 0 or -1. */
int mt_aes_init(struct mt_aes *aes, const uint8_t *key, size_t key_len);
void mt_aes_encrypt(const struct mt_aes *aes, const uint8_t in[16], uint8_t out[16]);

struct mt_sha256 {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	size_t fill;
};

void mt_sha256_init(struct mt_sha256 *s);
void mt_sha256_update(struct mt_sha256 *s, const void *data, size_t len);
void mt_sha256_final(struct mt_sha256 *s, uint8_t out[32]);
void mt_sha256(const void *data, size_t len, uint8_t out[32]);

/* X25519: out = scalar * point (RFC 7748). */
void mt_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
/* Public key for a private key (32 random bytes; clamped internally). */
void mt_x25519_public(uint8_t pub[32], const uint8_t priv[32]);

/*
 * AES-CTR with a 16-byte initial counter block; the last 4 bytes count
 * (big-endian), as in the Meshtastic firmware. Encrypts or decrypts in place.
 */
void mt_aes_ctr(const struct mt_aes *aes, const uint8_t iv[16], uint8_t *data, size_t len);

/*
 * AES-CCM (RFC 3610) with a 13-byte nonce, 8-byte tag and no additional data.
 * Encrypt: out = ciphertext (len bytes) followed by the tag.
 * Decrypt: in = ciphertext + tag (len + 8 bytes); returns false on a wrong tag.
 */
void mt_ccm_encrypt(const struct mt_aes *aes, const uint8_t nonce[13], const uint8_t *in,
		    size_t len, uint8_t *out);
bool mt_ccm_decrypt(const struct mt_aes *aes, const uint8_t nonce[13], const uint8_t *in,
		    size_t len, uint8_t *out);

/* Meshtastic channel encryption (in place); key_len 0 = no encryption. */
void mt_channel_crypt(const uint8_t *key, size_t key_len, uint32_t from, uint32_t id,
		      uint8_t *data, size_t len);

#define MT_PKI_OVERHEAD 12 /* 8-byte tag + 4-byte extra nonce */

/*
 * Direct message encryption. shared_key = SHA-256 of the X25519 secret.
 * out receives len + MT_PKI_OVERHEAD bytes.
 */
void mt_pki_encrypt(const uint8_t shared_key[32], uint32_t from, uint32_t id, uint32_t extra_nonce,
		    const uint8_t *in, size_t len, uint8_t *out);
/* in: len bytes including the overhead. Returns the plaintext length or -1. */
int mt_pki_decrypt(const uint8_t shared_key[32], uint32_t from, uint32_t id, const uint8_t *in,
		   size_t len, uint8_t *out);

/* SHA-256 of the X25519 secret; false for weak keys (all-zero secret). */
bool mt_pki_shared_key(uint8_t key[32], const uint8_t my_priv[32], const uint8_t their_pub[32]);

#ifdef __cplusplus
}
#endif
