/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_HALO_MATERIAL_H
#define UDX_HALO_MATERIAL_H
#include "halo_auth.h"
/* Public, credential-free primitives recovered from the Mate AP implementation.
 * This digest is SHA1(reverse-each-4-byte-word(nonce16)), not SHA1(nonce16).
 * RSA public exponent is fixed at 65537; caller supplies a trusted 128-byte big-endian
 * modulus (the stock AP key is 1023 bits). No default modulus, private key or permissive credential
 * provider. */
void halo_nonce_digest(const uint8_t nonce[16], uint8_t digest[20]);
/* Feature decode hashes 16 words = all 64 bytes, with the same word order. */
void halo_feature_digest(const uint8_t feature[64], uint8_t digest[20]);
void halo_material_block(const uint8_t nonce[16], uint32_t clock_word, uint8_t block[128]);
int halo_rsa1024_public(const uint8_t signature[128], const uint8_t modulus[128],
			uint8_t block[128]);
int halo_material_verify(const uint8_t material[144], const uint8_t trusted_modulus[128],
			 uint32_t *peer_clock);
/* Mandatory private operation receives a fully encoded 128-byte type-1 block
 * in big-endian RSA representation; it returns its 128-byte signature. It must
 * use a legally available device key/security service. Keys are never passed
 * into this library. Deadline/cancellation must be honoured by the callbacks.
 * Legacy nonce generation was weak: secure entropy is intentionally required.
 * Compatibility and freshness policy on a real CP remain untested. */
struct halo_material_provider {
	void *ctx;
	int (*monotonic_ms)(void *, uint64_t *);
	int (*entropy16)(void *, uint64_t generation, uint64_t deadline, uint8_t out[16]);
	int (*clock_word)(void *, uint32_t *);
	int (*private_operation)(void *, uint64_t generation, uint64_t deadline,
				 const uint8_t block[128], uint8_t signature[128]);
	void (*release)(void *, uint64_t generation);
	const uint8_t *trusted_modulus; /* Exactly 128 stable, trusted public bytes. */
};
/* Produces a real adapter only with every required callback and trust input.
 * It does not test whether a private key is present until prepare is invoked. */
int halo_material_adapter(struct halo_material_provider *, struct halo_credential_provider *);
#endif
