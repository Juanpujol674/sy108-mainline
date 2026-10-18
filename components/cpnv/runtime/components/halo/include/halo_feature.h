/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_HALO_FEATURE_H
#define UDX_HALO_FEATURE_H
#include <stddef.h>
#include <stdint.h>
#define HALO_FEATURE_REPLY_BYTES 152u
#define HALO_FEATURE_KEY_BYTES 8u
#define HALO_VERIFICATION_BYTES (128u + HALO_FEATURE_KEY_BYTES)
/* Same token transform as factory; the observed ninth request byte was also
 * uninitialized. Caller supplies an explicit padding byte, never heap data. */
void halo_feature_query_frame(const uint8_t token[4], const uint8_t nonce[2], uint8_t tail,
			      uint8_t out[9]);
/* Target status 0 permits decoding bytes9..92. Decode the 84-byte record with
 * caller-owned parameters, check the trailing digest against all first64 bytes,
 * then return bytes48..55 as a big-endian feature bitmap. No particular bit is
 * required to be one by the observed caller. No raw feature/parameter logging. */
int halo_feature_decode(const uint8_t *, size_t, const uint8_t key[8], uint64_t *bitmap,
			unsigned *raw_status);
#endif
