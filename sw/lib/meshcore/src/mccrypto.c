/* MeshCore's packet encryption: AES-128-ECB, then a 2-byte HMAC-SHA256. */

#include <string.h>

#include "meshcore/crypto.h"
#include "meshtastic/crypto.h"

void mc_hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len,
		    uint8_t out[32])
{
	struct mt_sha256 s;
	uint8_t k[64], inner[32];

	memset(k, 0, sizeof(k));
	if (key_len > sizeof(k)) {
		mt_sha256(key, key_len, k);
	} else {
		memcpy(k, key, key_len);
	}
	for (int i = 0; i < 64; i++) {
		k[i] ^= 0x36;
	}
	mt_sha256_init(&s);
	mt_sha256_update(&s, k, sizeof(k));
	mt_sha256_update(&s, data, len);
	mt_sha256_final(&s, inner);
	for (int i = 0; i < 64; i++) {
		k[i] ^= 0x36 ^ 0x5c;
	}
	mt_sha256_init(&s);
	mt_sha256_update(&s, k, sizeof(k));
	mt_sha256_update(&s, inner, sizeof(inner));
	mt_sha256_final(&s, out);
}

int mc_encrypt_then_mac(const uint8_t secret[32], uint8_t *out, const uint8_t *in, size_t len)
{
	struct mt_aes aes;
	uint8_t mac[32];
	uint8_t *dst = out + MC_MAC_SIZE;
	size_t n = 0;

	mt_aes_init(&aes, secret, MC_CIPHER_KEY);
	for (size_t off = 0; off < len; off += 16, n += 16) {
		uint8_t b[16] = {0};

		memcpy(b, in + off, len - off < 16 ? len - off : 16);
		mt_aes_encrypt(&aes, b, dst + n);
	}
	mc_hmac_sha256(secret, MC_PUB_KEY_SIZE, dst, n, mac);
	memcpy(out, mac, MC_MAC_SIZE);
	return MC_MAC_SIZE + n;
}

int mc_mac_then_decrypt(const uint8_t secret[32], uint8_t *out, const uint8_t *in, size_t len)
{
	struct mt_aes aes;
	uint8_t mac[32];
	size_t n = len - MC_MAC_SIZE;

	if (len <= MC_MAC_SIZE || n % 16 != 0) {
		return 0;
	}
	mc_hmac_sha256(secret, MC_PUB_KEY_SIZE, in + MC_MAC_SIZE, n, mac);
	if (memcmp(mac, in, MC_MAC_SIZE) != 0) {
		return 0;
	}
	mt_aes_init(&aes, secret, MC_CIPHER_KEY);
	for (size_t off = 0; off < n; off += 16) {
		mt_aes_decrypt(&aes, in + MC_MAC_SIZE + off, out + off);
	}
	return n;
}
