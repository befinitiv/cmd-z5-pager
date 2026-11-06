/*
 * Crypto tests: FIPS 180-4 / RFC 8032 / FIPS-197 / RFC 4231 vectors, vectors
 * from gen_vectors.py, and MeshCore's own known-good key pair.
 */

#include <string.h>

#include "meshcore/crypto.h"
#include "meshcore/packet.h"
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
	struct mc_sha512 sha;
	struct mt_aes aes;
	uint8_t out[64], exp[64], key[32], in[64], pub[32], prv[64], sig[64], msg[64];
	uint8_t pub_b[32], prv_b[64], s1[32], s2[32];

	/* SHA-512("abc") */
	mc_sha512_init(&sha);
	mc_sha512_update(&sha, "abc", 3);
	mc_sha512_final(&sha, out);
	hex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
	    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
	    exp);
	CHECK_MEM(out, exp, 64, "SHA-512 abc");
	/* Two blocks: 112 bytes. */
	mc_sha512_init(&sha);
	mc_sha512_update(&sha,
			 "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
			 112);
	mc_sha512_final(&sha, out);
	hex("8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
	    "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
	    exp);
	CHECK_MEM(out, exp, 64, "SHA-512 two blocks");

	/* RFC 8032 test 1: empty message. */
	hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", key);
	mc_ed25519_keypair(pub, prv, key);
	hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", exp);
	CHECK_MEM(pub, exp, 32, "Ed25519 public key");
	mc_ed25519_sign(sig, "", 0, pub, prv);
	hex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
	    "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
	    exp);
	CHECK_MEM(sig, exp, 64, "Ed25519 signature");
	CHECK(mc_ed25519_verify(sig, "", 0, pub), "Ed25519 verify");
	sig[5] ^= 1;
	CHECK(!mc_ed25519_verify(sig, "", 0, pub), "Ed25519 verify of a bad signature");

	/* Against Python's cryptography. */
	hex(SEED_A, key);
	mc_ed25519_keypair(pub, prv, key);
	hex(PUB_A, exp);
	CHECK_MEM(pub, exp, 32, "public key A");
	hex(MSG, msg);
	mc_ed25519_sign(sig, msg, strlen(MSG) / 2, pub, prv);
	hex(SIG_A, exp);
	CHECK_MEM(sig, exp, 64, "signature A");
	CHECK(mc_ed25519_verify(sig, msg, strlen(MSG) / 2, pub), "verify A");
	msg[0] ^= 1;
	CHECK(!mc_ed25519_verify(sig, msg, strlen(MSG) / 2, pub), "verify of another message");

	hex(SEED_B, key);
	mc_ed25519_keypair(pub_b, prv_b, key);
	CHECK(mc_shared_secret(s1, prv, pub_b), "secret A-B");
	CHECK(mc_shared_secret(s2, prv_b, pub), "secret B-A");
	hex(SECRET_AB, exp);
	CHECK_MEM(s1, exp, 32, "secret A-B");
	CHECK_MEM(s2, exp, 32, "secret B-A");

	/* MeshCore's test client key pair (Identity.cpp): secrets agree with it. */
	{
		uint8_t tprv[64], tpub[32];

		hex("7065e18fd9fabb70c1ed90dca19907de698c88b709ea146eafd93d9b830c7b60"
		    "c4681193c79bbc39945ba8064104bb618f8fd7a84a0af6f57033d6e8ddcd6471",
		    tprv);
		hex("1ec77175b0918ed206f9ae04ec136d6d5d4315bb26305427f645b492e9350c10", tpub);
		mc_shared_secret(s1, prv, tpub);
		mc_shared_secret(s2, tprv, pub);
		CHECK_MEM(s1, s2, 32, "secret with MeshCore's test client");
	}

	/* FIPS-197 C.1: AES-128 decryption. */
	for (int i = 0; i < 16; i++) {
		key[i] = i;
	}
	hex("69c4e0d86a7b0430d8cdb78070b4c55a", in);
	mt_aes_init(&aes, key, 16);
	mt_aes_decrypt(&aes, in, out);
	hex("00112233445566778899aabbccddeeff", exp);
	CHECK_MEM(out, exp, 16, "AES-128 decrypt");
	/* C.3: AES-256. */
	for (int i = 0; i < 32; i++) {
		key[i] = i;
	}
	hex("8ea2b7ca516745bfeafc49904b496089", in);
	mt_aes_init(&aes, key, 32);
	mt_aes_decrypt(&aes, in, out);
	CHECK_MEM(out, exp, 16, "AES-256 decrypt");

	/* RFC 4231 test case 2. */
	mc_hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, out);
	hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", exp);
	CHECK_MEM(out, exp, 32, "HMAC-SHA256");

	/* Encrypt-then-MAC round trip, and a changed byte is refused. */
	{
		uint8_t secret[32] = {0}, buf[64], plain[64];
		int n, m;

		memcpy(secret, mc_public_secret, 16);
		n = mc_encrypt_then_mac(secret, buf, (const uint8_t *)"hello world", 11);
		CHECK(n == 2 + 16, "encrypted length %d", n);
		m = mc_mac_then_decrypt(secret, plain, buf, n);
		CHECK(m == 16 && memcmp(plain, "hello world\0\0\0\0\0", 16) == 0, "decrypt");
		buf[5] ^= 1;
		CHECK(mc_mac_then_decrypt(secret, plain, buf, n) == 0, "tampered refused");
	}
}
