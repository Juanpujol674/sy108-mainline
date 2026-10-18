/* SPDX-License-Identifier: GPL-2.0-only */
#include "modem_control.h"
#include "halo_peer_gate.h"
#include <errno.h>
#include <string.h>
#include <limits.h>
static uint32_t u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const uint8_t *p)
{
	return u32(p) | (uint64_t)u32(p + 4) << 32;
}
static int fd32(const uint8_t *p)
{
	uint32_t n = u32(p);
	return n >= 3 && n <= INT_MAX ? (int)n : -1;
}
static int decode(const uint8_t *p, size_t n, struct mc_config *c, unsigned pending)
{
	if (!p || !c || n < MC_PROFILE_HEADER || memcmp(p, "UDXMC004", 8) || u32(p + 8) != 1)
		return -EPROTO;
	memset(c, 0, sizeof *c);
	c->reload = u32(p + 12);
	c->epoch = u64(p + 16);
	c->operation_ms = u32(p + 24);
	c->alive_ms = u32(p + 28);
	c->halo_ms = u32(p + 32);
	c->pmic = u32(p + 36);
	c->image_count = u32(p + 40);
	c->factory_tail = u32(p + 44);
	c->postload_mode = u32(p + 216);
	if (c->reload > 1 || c->pmic > 1 || !c->epoch || !c->operation_ms ||
	    c->operation_ms > 60000 || c->postload_mode > 1 || c->image_count > MC_MAX_IMAGES ||
	    n != MC_PROFILE_HEADER + c->image_count * MC_IMAGE_RECORD)
		return -EINVAL;
	if (c->postload_mode) {
		if (!c->alive_ms || c->alive_ms > 60000 || !c->halo_ms ||
		    c->halo_ms > MC_HALO_TOTAL_MAX_MS ||
		    (c->factory_tail & 0xffffff00u) != 0x80000000u)
			return -EINVAL;
	} else if (c->alive_ms || c->halo_ms || c->factory_tail || u32(p + 164) || u32(p + 64) ||
		   u32(p + 68) || u32(p + 72) || u32(p + 76) || u32(p + 80) || u32(p + 84) ||
		   u32(p + 88))
		return -EINVAL;
	if (u32(p + 48))
		return -EPROTO;
	/* Optional PMCP/cali_lib source contract is not complete in this NR profile. */
	if (c->pmic)
		return -EOPNOTSUPP;
	c->parent = fd32(p + 52);
	c->owner = fd32(p + 56);
	c->lease = fd32(p + 60);
	c->devdir = fd32(p + 64);
	c->status_session = fd32(p + 68);
	c->halo_session = fd32(p + 72);
	c->provider_rx = fd32(p + 76);
	c->provider_tx = fd32(p + 80);
	c->verification_params = fd32(p + 84);
	c->events = fd32(p + 88);
	c->phase = fd32(p + 92);
	c->factory_tail &= 255u;
	c->cmdline = fd32(p + 160);
	c->halo_delay_ms = u32(p + 164);
	c->report = fd32(p + 204);
	c->continuation = fd32(p + 208);
	c->continue_ms = u32(p + 212);
	if (c->pmic)
		return -EINVAL;
	if (c->reload) {
		if (c->report < 3 || c->continuation < 3 || !c->continue_ms ||
		    c->continue_ms > 600000)
			return -EINVAL;
	} else if (u32(p + 204) || u32(p + 208) || c->continue_ms)
		return -EINVAL;
	if (c->parent < 3 || c->owner < 3 || c->lease < 3 ||
	    (c->postload_mode &&
	     (c->devdir < 3 || c->status_session < 3 || c->halo_session < 3 || c->provider_rx < 3 ||
	      c->provider_tx < 3 || c->verification_params < 3 || c->events < 3)))
		return -EINVAL;
	for (unsigned d = 0; d < 4; d++) {
		const uint8_t *q = p + 96 + 16 * d;
		if (u32(q) || (d == 3 && (u32(q + 4) || u32(q + 8) || u32(q + 12))))
			return -EINVAL;
		c->status[d] = fd32(q + 4);
		c->start[d] = fd32(q + 8);
		c->stop[d] = fd32(q + 12);
		if (d < 3 &&
		    (c->status[d] < 3 || (c->reload && (c->start[d] < 3 || c->stop[d] < 3))))
			return -EINVAL;
		if (!c->reload && (u32(q + 8) || u32(q + 12)))
			return -EINVAL;
	}
	if (c->cmdline < 3 || (c->postload_mode && (c->halo_delay_ms != 10000)))
		return -EINVAL;
	if (c->phase < 3)
		return -EINVAL;
	for (unsigned i = 168; i < 204; i++)
		if (p[i])
			return -EPROTO;
	for (unsigned i = 220; i < MC_PROFILE_HEADER; i++)
		if (p[i])
			return -EPROTO;
	if ((!c->reload && (c->image_count || c->pmic)) || (c->reload && !c->image_count))
		return -EINVAL;
	for (unsigned i = 0; i < c->image_count; i++) {
		const uint8_t *q = p + MC_PROFILE_HEADER + i * MC_IMAGE_RECORD;
		struct mc_image *v = c->images + i;
		v->domain = u32(q);
		v->segment = u32(q + 4);
		v->source = fd32(q + 8);
		v->target = fd32(q + 12);
		v->offset = u64(q + 16);
		v->length = u64(q + 24);
		v->capacity = u64(q + 32);
		memcpy(v->sha256, q + 40, 32);
		v->backup = fd32(q + 72);
		if (v->segment < 7 && u32(q + 72))
			return -EPROTO;
		v->target_offset = u32(q + 76);
		v->physical_base = u64(q + 80);
		unsigned deferred = 0;
		deferred = pending && v->segment >= 7;
		if (v->domain != 0 || v->segment > 8 || (!deferred && v->source < 3) ||
		    v->target < 3 || !v->capacity || v->capacity > UINT32_MAX ||
		    (!deferred && v->segment >= 7 && v->backup < 3))
			return -EINVAL;
		if (pending) {
			if (v->offset || v->target_offset || v->length)
				return -EINVAL;
			for (unsigned k = 0; k < 32; k++)
				if (v->sha256[k])
					return -EINVAL;
		} else if (!v->length || v->target_offset > v->capacity ||
			   v->length > v->capacity - v->target_offset ||
			   v->offset > INT64_MAX - v->length)
			return -EINVAL;
		for (unsigned j = 88; j < MC_IMAGE_RECORD; j++)
			if (q[j])
				return -EPROTO;
		if (v->physical_base > UINT64_MAX - v->capacity)
			return -EINVAL;
		if (!pending)
			for (unsigned j = 0; j < i; j++) {
				struct mc_image *w = c->images + j;
				uint64_t a = w->physical_base + w->target_offset,
					 b = v->physical_base + v->target_offset;
				if (a < b + v->length && b < a + w->length)
					return -EINVAL;
			}
	}
	return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
	for (unsigned i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
static void putfd(uint8_t *p, int fd)
{
	put32(p, fd < 0 ? 0u : (uint32_t)fd);
}
static int encode(const struct mc_config *c, uint8_t *out, size_t capacity, size_t *used,
		  unsigned pending)
{
	if (!c || !out || !used || c->image_count > MC_MAX_IMAGES || c->factory_tail > 255 ||
	    (!c->postload_mode && c->factory_tail))
		return -EINVAL;
	size_t n = MC_PROFILE_HEADER + c->image_count * MC_IMAGE_RECORD;
	if (capacity < n)
		return -ENOSPC;
	uint8_t p[MC_PROFILE_HEADER + MC_MAX_IMAGES * MC_IMAGE_RECORD];
	memset(p, 0, n);
	memcpy(p, "UDXMC004", 8);
	put32(p + 8, 1);
	put32(p + 12, c->reload);
	put64(p + 16, c->epoch);
	put32(p + 24, c->operation_ms);
	put32(p + 28, c->alive_ms);
	put32(p + 32, c->halo_ms);
	put32(p + 36, c->pmic);
	put32(p + 40, c->image_count);
	put32(p + 44, c->postload_mode ? (0x80000000u | c->factory_tail) : 0u);
	const int fds[] = {c->parent,	   c->owner,	      c->lease,
			   c->devdir,	   c->status_session, c->halo_session,
			   c->provider_rx, c->provider_tx,    c->verification_params,
			   c->events,	   c->phase};
	for (unsigned i = 0; i < 11; i++)
		putfd(p + 52 + 4 * i, fds[i]);
	for (unsigned d = 0; d < 4; d++) {
		putfd(p + 100 + 16 * d, c->status[d]);
		putfd(p + 104 + 16 * d, c->start[d]);
		putfd(p + 108 + 16 * d, c->stop[d]);
	}
	putfd(p + 160, c->cmdline);
	put32(p + 164, c->halo_delay_ms);
	putfd(p + 204, c->report);
	putfd(p + 208, c->continuation);
	put32(p + 212, c->continue_ms);
	put32(p + 216, c->postload_mode);
	for (unsigned i = 0; i < c->image_count; i++) {
		const struct mc_image *v = c->images + i;
		uint8_t *q = p + MC_PROFILE_HEADER + i * MC_IMAGE_RECORD;
		if (v->target_offset > UINT32_MAX || (v->segment < 7 && v->backup >= 0))
			return -EINVAL;
		put32(q, v->domain);
		put32(q + 4, v->segment);
		putfd(q + 8, v->source);
		putfd(q + 12, v->target);
		put64(q + 16, v->offset);
		put64(q + 24, v->length);
		put64(q + 32, v->capacity);
		memcpy(q + 40, v->sha256, 32);
		putfd(q + 72, v->backup);
		put32(q + 76, (uint32_t)v->target_offset);
		put64(q + 80, v->physical_base);
	}
	struct mc_config checked;
	int e = decode(p, n, &checked, pending);
	if (e)
		return e;
	memcpy(out, p, n);
	*used = n;
	return 0;
}

int mc_warm_profile_decode(const uint8_t *p, size_t n, struct mc_config *c)
{
	return decode(p, n, c, 1);
}
int mc_warm_profile_encode(const struct mc_config *c, uint8_t *p, size_t n, size_t *used)
{
	return encode(c, p, n, used, 1);
}
int mc_warm_plan_validate(const struct mc_config *c)
{
	uint8_t raw[MC_PROFILE_HEADER + MC_MAX_IMAGES * MC_IMAGE_RECORD];
	size_t n = 0;
	return encode(c, raw, sizeof raw, &n, 0);
}

int mc_warm_continue_encode(const struct mc_config *c, uint8_t *out, size_t capacity, size_t *used)
{
	if (!c || !out || !used || !c->reload)
		return -EINVAL;
	int e = mc_warm_plan_validate(c);
	if (e)
		return e;
	size_t n = MC_WARM_CONT_HEADER + MC_WARM_CONT_RECORD * c->image_count;
	if (capacity < n)
		return -ENOSPC;
	uint8_t raw[MC_WARM_CONT_HEADER + MC_WARM_CONT_RECORD * MC_MAX_IMAGES];
	memset(raw, 0, n);
	memcpy(raw, "MCCONT04", 8);
	put64(raw + 8, c->epoch);
	put32(raw + 16, (uint32_t)c->parent);
	put32(raw + 20, c->image_count);
	for (unsigned i = 0; i < c->image_count; i++) {
		const struct mc_image *v = c->images + i;
		uint8_t *q = raw + MC_WARM_CONT_HEADER + MC_WARM_CONT_RECORD * i;
		put32(q, i);
		put64(q + 8, v->offset);
		put64(q + 16, v->target_offset);
		put64(q + 24, v->length);
		memcpy(q + 32, v->sha256, 32);
	}
	memcpy(out, raw, n);
	*used = n;
	return 0;
}
