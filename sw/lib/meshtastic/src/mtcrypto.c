/*
 * Meshtastic packet encryption, as implemented by the Meshtastic firmware
 * (CryptoEngine): see the comments for the nonce layouts.
 */

#include <string.h>

#include "meshtastic/crypto.h"

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

void mt_channel_crypt(const uint8_t *key, size_t key_len, uint32_t from, uint32_t id,
		      uint8_t *data, size_t len)
{
	struct mt_aes aes;
	uint8_t iv[16] = {0};

	if (key_len == 0 || mt_aes_init(&aes, key, key_len) != 0) {
		return; /* no encryption */
	}
	/* Nonce: packet id as 64-bit LE, sender node number LE, zeros. */
	put_le32(iv, id);
	put_le32(iv + 8, from);
	mt_aes_ctr(&aes, iv, data, len);
}

/* PKI nonce: packet id (LE, 4 bytes), extra nonce (LE), sender (LE), 0. */
static void pki_nonce(uint8_t nonce[13], uint32_t from, uint32_t id, uint32_t extra)
{
	put_le32(nonce, id);
	put_le32(nonce + 4, extra);
	put_le32(nonce + 8, from);
	nonce[12] = 0;
}

void mt_pki_encrypt(const uint8_t shared_key[32], uint32_t from, uint32_t id, uint32_t extra_nonce,
		    const uint8_t *in, size_t len, uint8_t *out)
{
	struct mt_aes aes;
	uint8_t nonce[13];

	mt_aes_init(&aes, shared_key, 32);
	pki_nonce(nonce, from, id, extra_nonce);
	mt_ccm_encrypt(&aes, nonce, in, len, out);
	put_le32(out + len + 8, extra_nonce);
}

int mt_pki_decrypt(const uint8_t shared_key[32], uint32_t from, uint32_t id, const uint8_t *in,
		   size_t len, uint8_t *out)
{
	struct mt_aes aes;
	uint8_t nonce[13];

	if (len <= MT_PKI_OVERHEAD) {
		return -1;
	}
	size_t plain = len - MT_PKI_OVERHEAD;
	const uint8_t *x = in + plain + 8;
	uint32_t extra = x[0] | x[1] << 8 | x[2] << 16 | (uint32_t)x[3] << 24;

	mt_aes_init(&aes, shared_key, 32);
	pki_nonce(nonce, from, id, extra);
	return mt_ccm_decrypt(&aes, nonce, in, plain, out) ? (int)plain : -1;
}

bool mt_pki_shared_key(uint8_t key[32], const uint8_t my_priv[32], const uint8_t their_pub[32])
{
	uint8_t secret[32];
	uint8_t any = 0;

	mt_x25519(secret, my_priv, their_pub);
	for (int i = 0; i < 32; i++) {
		any |= secret[i];
	}
	if (!any) {
		return false;
	}
	mt_sha256(secret, 32, key);
	return true;
}
