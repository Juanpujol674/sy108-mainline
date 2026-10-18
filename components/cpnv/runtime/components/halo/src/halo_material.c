/* SPDX-License-Identifier: GPL-2.0-only */
#include "halo_material.h"
static void wipe(void *p, size_t n)
{
	volatile uint8_t *q = p;
	while (n--)
		*q++ = 0;
}
static uint32_t rot(uint32_t x, unsigned n)
{
	return (x << n) | (x >> (32 - n));
}
static void sha1_compress(uint32_t h[5], const uint32_t input[16])
{
	uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
	unsigned i;
	for (i = 0; i < 16; i++)
		w[i] = input[i];
	for (i = 16; i < 80; i++)
		w[i] = rot(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	for (i = 0; i < 80; i++) {
		uint32_t f, k, t;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdc;
		} else {
			f = b ^ c ^ d;
			k = 0xca62c1d6;
		}
		t = rot(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rot(b, 30);
		b = a;
		a = t;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	wipe(w, sizeof(w));
}
/* Target SHA update consumes native little-endian u32 words, not a byte count.
 * Reuse the existing compression for nonce=4 words and feature=16 words. */
static void digest_words(const uint8_t *in, unsigned words, uint8_t out[20])
{
	uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0}, w[16] = {0};
	unsigned i;
	for (i = 0; i < words; i++)
		w[i] = (uint32_t)in[4 * i] | (uint32_t)in[4 * i + 1] << 8 |
		       (uint32_t)in[4 * i + 2] << 16 | (uint32_t)in[4 * i + 3] << 24;
	if (words == 16) {
		sha1_compress(h, w);
		wipe(w, sizeof(w));
		w[0] = 0x80000000;
	} else
		w[words] = 0x80000000;
	w[15] = words * 32;
	sha1_compress(h, w);
	for (i = 0; i < 20; i++)
		out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
	wipe(w, sizeof(w));
	wipe(h, sizeof(h));
}
void halo_nonce_digest(const uint8_t in[16], uint8_t out[20])
{
	digest_words(in, 4, out);
}
void halo_feature_digest(const uint8_t in[64], uint8_t out[20])
{
	digest_words(in, 16, out);
}
void halo_material_block(const uint8_t nonce[16], uint32_t word, uint8_t out[128])
{
	unsigned i;
	out[0] = 0;
	out[1] = 1;
	for (i = 2; i < 103; i++)
		out[i] = 0xff;
	out[103] = 0;
	halo_nonce_digest(nonce, out + 104);
	for (i = 0; i < 4; i++)
		out[124 + i] = (uint8_t)(word >> (24 - 8 * i));
}
static int compare(const uint32_t a[32], const uint32_t b[32])
{
	unsigned i = 32;
	while (i--) {
		if (a[i] > b[i])
			return 1;
		if (a[i] < b[i])
			return -1;
	}
	return 0;
}
static void addmod(uint32_t out[32], const uint32_t a[32], const uint32_t b[32],
		   const uint32_t m[32])
{
	uint64_t carry = 0;
	unsigned i;
	for (i = 0; i < 32; i++) {
		uint64_t x = (uint64_t)a[i] + b[i] + carry;
		out[i] = (uint32_t)x;
		carry = x >> 32;
	}
	if (carry || compare(out, m) >= 0) {
		uint64_t borrow = 0;
		for (i = 0; i < 32; i++) {
			uint64_t sub = (uint64_t)m[i] + borrow;
			uint32_t old = out[i];
			out[i] = (uint32_t)((uint64_t)old - sub);
			borrow = (uint64_t)old < sub;
		}
	}
}
static void multiply(uint32_t out[32], const uint32_t a[32], const uint32_t b[32],
		     const uint32_t m[32])
{
	uint32_t r[32] = {0};
	unsigned bit = 1024;
	while (bit--) {
		addmod(r, r, r, m);
		if ((b[bit / 32] >> (bit % 32)) & 1)
			addmod(r, r, a, m);
	}
	for (bit = 0; bit < 32; bit++)
		out[bit] = r[bit];
	wipe(r, sizeof(r));
}
static void decode(const uint8_t in[128], uint32_t out[32])
{
	unsigned i, j;
	for (i = 0; i < 32; i++) {
		uint32_t n = 0;
		for (j = 0; j < 4; j++)
			n |= (uint32_t)in[127 - (4 * i + j)] << (8 * j);
		out[i] = n;
	}
}
int halo_rsa1024_public(const uint8_t sig[128], const uint8_t modulus[128], uint8_t out[128])
{
	uint32_t m[32], a[32], r[32];
	unsigned i;
	int error = 0;
	if (!sig || !modulus || !out)
		return -22;
	if (!modulus[0] || !(modulus[127] & 1))
		return -22;
	decode(modulus, m);
	decode(sig, a);
	if (compare(a, m) >= 0) {
		error = -129;
		goto done;
	}
	for (i = 0; i < 32; i++)
		r[i] = a[i];
	for (i = 0; i < 16; i++)
		multiply(r, r, r, m);
	multiply(r, r, a, m);
	for (i = 0; i < 128; i++)
		out[127 - i] = (uint8_t)(r[i / 4] >> (8 * (i % 4)));
done:
	wipe(m, sizeof(m));
	wipe(a, sizeof(a));
	wipe(r, sizeof(r));
	return error;
}
int halo_material_verify(const uint8_t material[144], const uint8_t modulus[128],
			 uint32_t *peer_clock)
{
	uint8_t block[128], digest[20];
	unsigned diff = 0, i;
	uint32_t word = 0;
	int e;
	if (!material || !modulus || !peer_clock)
		return -22;
	e = halo_rsa1024_public(material + 16, modulus, block);
	if (e)
		return e;
	diff = block[0] | (unsigned)(block[1] ^ 1) | block[103];
	for (i = 2; i < 103; i++)
		diff |= (unsigned)(block[i] ^ 0xff);
	halo_nonce_digest(material, digest);
	for (i = 0; i < 20; i++)
		diff |= (unsigned)(digest[i] ^ block[104 + i]);
	for (i = 0; i < 4; i++)
		word = (word << 8) | block[124 + i];
	wipe(block, sizeof(block));
	wipe(digest, sizeof(digest));
	if (diff)
		return -129;
	*peer_clock = word;
	return 0; /* No observed freshness check in the AP caller. */
}
static int adapter_monotonic(void *c, uint64_t *out)
{
	struct halo_material_provider *p = c;
	return p->monotonic_ms(p->ctx, out);
}
static int adapter_clock(void *c, uint32_t *out)
{
	struct halo_material_provider *p = c;
	return p->clock_word(p->ctx, out);
}
static int adapter_prepare(void *c, uint64_t generation, uint64_t deadline, uint8_t out[144])
{
	struct halo_material_provider *p = c;
	uint8_t nonce[16], block[128], signature[128];
	uint32_t word;
	int e;
	unsigned i;
	e = p->entropy16(p->ctx, generation, deadline, nonce);
	if (e)
		goto done;
	e = p->clock_word(p->ctx, &word);
	if (e)
		goto done;
	halo_material_block(nonce, word, block);
	e = p->private_operation(p->ctx, generation, deadline, block, signature);
	if (e)
		goto done;
	for (i = 0; i < 16; i++) {
		out[i] = nonce[i];
	}
	for (i = 0; i < 128; i++) {
		out[16 + i] = signature[i];
	}
done:
	wipe(nonce, sizeof(nonce));
	wipe(block, sizeof(block));
	wipe(signature, sizeof(signature));
	return e ? (e < 0 ? e : -5) : 0;
}
static int adapter_verify(void *c, uint64_t generation, uint64_t deadline, const uint8_t in[144])
{
	struct halo_material_provider *p = c;
	uint32_t peer_clock;
	(void)generation;
	(void)deadline;
	return halo_material_verify(in, p->trusted_modulus, &peer_clock);
}
static void adapter_release(void *c, uint64_t generation)
{
	struct halo_material_provider *p = c;
	p->release(p->ctx, generation);
}
int halo_material_adapter(struct halo_material_provider *p, struct halo_credential_provider *out)
{
	if (!out)
		return -22;
	wipe(out, sizeof(*out));
	if (!p || !p->monotonic_ms || !p->entropy16 || !p->clock_word || !p->private_operation ||
	    !p->release || !p->trusted_modulus)
		return -95;
	if (!p->trusted_modulus[0] || !(p->trusted_modulus[127] & 1))
		return -22;
	out->ctx = p;
	out->monotonic_ms = adapter_monotonic;
	out->clock_word = adapter_clock;
	out->prepare = adapter_prepare;
	out->verify = adapter_verify;
	out->release = adapter_release;
	return 0;
}
