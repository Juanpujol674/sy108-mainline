/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_SHA256_H
#define UDX_SHA256_H
#include <stdint.h>
#include <stddef.h>
struct udx_sha256 {
	uint32_t h[8];
	uint64_t bytes;
	uint8_t block[64];
	unsigned used;
};
void udx_sha256_init(struct udx_sha256 *);
int udx_sha256_update(struct udx_sha256 *, const void *, size_t);
int udx_sha256_final(struct udx_sha256 *, uint8_t out[32]);
#endif
