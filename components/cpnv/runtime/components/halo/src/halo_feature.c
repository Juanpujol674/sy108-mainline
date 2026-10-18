/* SPDX-License-Identifier: GPL-2.0-only */
#include "halo_feature.h"
#include "halo_auth.h"
#include "halo_material.h"
#include <errno.h>
static void wipe(void *p, size_t n)
{
	volatile uint8_t *v = p;
	while (n--)
		*v++ = 0;
}
static uint8_t reverse(uint8_t x)
{
	x = (uint8_t)((x >> 4) | (x << 4));
	x = (uint8_t)(((x << 2) & 0xcc) | ((x >> 2) & 0x33));
	return (uint8_t)(((x << 1) & 0xaa) | ((x >> 1) & 0x55));
}
void halo_feature_query_frame(const uint8_t token[4], const uint8_t nonce[2], uint8_t tail,
			      uint8_t out[9])
{
	halo_factory_query_frame(token, nonce, tail, out);
	out[2] = 0xa2;
}
int halo_feature_decode(const uint8_t *reply, size_t n, const uint8_t key[8], uint64_t *bitmap,
			unsigned *status)
{
	uint8_t record[84], digest[20];
	unsigned difference = 0;
	uint64_t bits = 0;
	if (bitmap)
		*bitmap = 0;
	if (status)
		*status = 0;
	if (!reply || !key || !bitmap || !status || n != HALO_FEATURE_REPLY_BYTES)
		return -EINVAL;
	*status = reply[8];
	if (*status)
		return -EKEYREJECTED;
	for (unsigned i = 0; i < 84; i++) {
		unsigned at = (2 * i) & 7;
		uint8_t value = reverse(reply[9 + i]) ^ key[at + 1];
		/* Target source/destination alias, so the second pass reads the first result. */
		record[i] = reverse(value) ^ key[at];
	}
	halo_feature_digest(record, digest);
	for (unsigned i = 0; i < 20; i++)
		difference |= (unsigned)(digest[i] ^ record[64 + i]);
	if (!difference)
		for (unsigned i = 48; i < 56; i++)
			bits = (bits << 8) | record[i];
	wipe(record, sizeof(record));
	wipe(digest, sizeof(digest));
	if (difference)
		return -EBADMSG;
	*bitmap = bits;
	return 0;
}
