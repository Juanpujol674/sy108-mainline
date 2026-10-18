/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_HALO_RSA_PROVIDER_H
#define UDX_HALO_RSA_PROVIDER_H
#include "halo_material.h"
#include <openssl/evp.h>

/* OpenSSL 3 implementation of the existing material callback, not another HALO
 * protocol. Caller supplies 128-byte big-endian n/d and trusted peer modulus.
 * Target ABI: 0x4101fc argument A is d, B is n; 0x40fc04 imports the external
 * bytes as big endian. This API names its arguments n then d explicitly.
 * e is the target's 65537. No key files, device reads, RPC or embedded key.
 * The caller must own/serialize the object and clear it after the last exchange.
 * callback ctx/release remain owned by caller; the key is copied by OpenSSL.
 * Native validation and a glibc/libcrypto fully static selftest are available;
 * the five musl production programs remain a separate link configuration. */
struct halo_rsa_provider {
	EVP_PKEY *key;
	struct halo_material_provider callbacks, material;
	uint8_t peer_modulus[128];
};
/* A fresh zero-initialized object is required. The callbacks' private_operation
 * is unused; all other material callbacks are retained.
 * Output plugs directly into mc_halo_with_provider(). */
int halo_rsa_provider_init(struct halo_rsa_provider *,
			   const struct halo_material_provider *callbacks,
			   const uint8_t local_modulus[128], const uint8_t private_exponent[128],
			   struct halo_credential_provider *out);
void halo_rsa_provider_clear(struct halo_rsa_provider *);
#endif
