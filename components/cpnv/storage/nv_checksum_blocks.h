/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_NV_CHECKSUM_BLOCKS_H
#define UDX_NV_CHECKSUM_BLOCKS_H
#include <stddef.h>
#include <stdint.h>
/* All NV block boundaries and lengths are multiples of the 512-byte sector.
 * Sum without folding until END, then reproduce the selected OEM width. */
static inline uint64_t nv_block_sum(const uint8_t *data, size_t n)
{
	uint64_t sum = 0;
	for (size_t i = 0; i < n; i += 2)
		sum += (uint64_t)data[i] | ((uint64_t)data[i + 1] << 8);
	return sum;
}
static inline uint16_t nv_block_checksum(uint64_t sum, unsigned bits)
{
	if (bits == 32)
		sum = (uint32_t)sum;
	sum = (sum & UINT64_C(0xffff)) + (sum >> 16);
	sum += sum >> 16;
	return (uint16_t)~sum;
}
#endif
