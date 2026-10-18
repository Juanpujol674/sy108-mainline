#include "nv_codec.h"

/* Byte accesses avoid alignment, aliasing, and host-endian assumptions. */
static uint32_t nv_load32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void nv_store32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

int nv_frame_decode(const uint8_t *raw, size_t len, struct nv_frame *out)
{
	uint32_t size;

	if (raw == NULL || out == NULL || len < NV_FRAME_HEADER)
		return -1;
	if (nv_load32(raw) != NV_FRAME_MAGIC)
		return -1;
	size = nv_load32(raw + 16);
	/* Subtract after checking len, avoiding size + 20 overflow altogether. */
	if (size > NV_MAX_PAYLOAD || len - NV_FRAME_HEADER != (size_t)size)
		return -1;
	out->opaque = nv_load32(raw + 4);
	out->group = nv_load32(raw + 8);
	out->sub = nv_load32(raw + 12);
	out->payload_size = size;
	out->payload = raw + NV_FRAME_HEADER;
	return 0;
}

int nv_response_encode(const struct nv_frame *frame, int status, uint8_t out[NV_RESPONSE_SIZE])
{
	size_t i;

	if (frame == NULL || out == NULL || (status != 0 && status != 1))
		return -1;
	for (i = 0; i < NV_RESPONSE_SIZE; ++i)
		out[i] = 0;
	nv_store32(out, NV_FRAME_MAGIC);
	nv_store32(out + 8, frame->group);
	nv_store32(out + 12, frame->sub);
	nv_store32(out + 16, 1);
	out[20] = (uint8_t)status;
	return 0;
}

int nv_command_decode(const uint8_t raw[NV_COMMAND_SIZE], struct nv_command *out)
{
	if (raw == NULL || out == NULL)
		return -1;
	out->kind = nv_load32(raw);
	out->sequence = nv_load32(raw + 4);
	out->arg = nv_load32(raw + 8);
	out->sector = nv_load32(raw + 12);
	out->count = nv_load32(raw + 16);
	return 0;
}

uint16_t nv_checksum(const uint8_t *data, size_t len, unsigned bits)
{
	size_t i;
	uint32_t sum32 = 0;
	uint64_t sum64 = 0;

	if ((bits != 32 && bits != 64) || len > NV_MAX_IMAGE || (data == NULL && len != 0))
		return 0;
	/* Separate unsigned types reproduce the accumulator width in checksum.asm.
	 * Do not repeatedly fold: the binary performs exactly these two folds.
	 */
	if (bits == 32) {
		for (i = 0; i + 1 < len; i += 2)
			sum32 += (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8);
		if (i < len)
			sum32 += data[i];
		sum32 = (sum32 & UINT32_C(0xffff)) + (sum32 >> 16);
		sum32 += sum32 >> 16;
		return (uint16_t)~sum32;
	}
	for (i = 0; i + 1 < len; i += 2)
		sum64 += (uint64_t)data[i] | ((uint64_t)data[i + 1] << 8);
	if (i < len)
		sum64 += data[i];
	sum64 = (sum64 & UINT64_C(0xffff)) + (sum64 >> 16);
	sum64 += sum64 >> 16;
	return (uint16_t)~sum64;
}
