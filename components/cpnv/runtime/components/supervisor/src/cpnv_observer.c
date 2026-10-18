/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv_observer.h"

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p)
{
	return le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static int decimal(const char **p, const char *end, uint32_t max, uint32_t *v)
{
	uint32_t n = 0;
	const char *s = *p;
	if (s == end || *s < '0' || *s > '9')
		return CPNV_FORMAT;
	do {
		unsigned d = (unsigned)(*s - '0');
		if (d > max || n > (max - d) / 10)
			return CPNV_FORMAT;
		n = n * 10 + d;
		++s;
	} while (s < end && *s >= '0' && *s <= '9');
	*p = s;
	*v = n;
	return CPNV_OK;
}
int cpnv_parse_snapshot(unsigned index, const char *text, size_t n, struct cpnv_snapshot *out)
{
	static const uint32_t channels[4] = {4, 6, 40, 120};
	uint32_t f[7] = {0};
	unsigned negative = 0, count = index == 3 ? 7 : 5;
	const char *p, *end;
	if (!out)
		return CPNV_FORMAT;
	*out = (struct cpnv_snapshot){0};
	if (index >= 4 || !text || !n || n > 127)
		return CPNV_FORMAT;
	p = text;
	end = text + n;
	for (unsigned j = 0; j < count; ++j) {
		uint32_t max = UINT32_MAX;
		if (j == 6) {
			if (p < end && *p == '-') {
				negative = 1;
				++p;
			}
			max = negative ? UINT32_C(2147483648) : 0;
		}
		if (decimal(&p, end, max, &f[j]))
			return CPNV_FORMAT;
		if (j + 1 < count && (p == end || *p++ != ' '))
			return CPNV_FORMAT;
	}
	if (p != end - 1 || *p != '\n')
		return CPNV_FORMAT;
	if (f[0] != 1 || f[1] != 5 || f[2] != channels[index])
		return CPNV_TARGET;
	if ((index < 3 && f[4] > 2) || (index == 3 && (f[4] > 1 || f[5] > 1)))
		return CPNV_FORMAT;
	out->generation = f[3];
	out->state = f[4];
	out->blocked = f[5];
	out->error = negative ? (int32_t)(-(int64_t)f[6]) : 0;
	return CPNV_OK;
}
static int fail(struct cpnv_epoch *e, int result)
{
	e->failure = result;
	return result;
}
int cpnv_observe_epoch(struct cpnv_epoch *e, uint64_t epoch, const struct cpnv_snapshot x[4],
		       const struct cpnv_provider p[4], uint64_t now, uint64_t deadline)
{
	struct cpnv_epoch next;
	if (!e)
		return CPNV_FORMAT;
	if (e->failure)
		return e->failure;
	if (!x || !p || !epoch)
		return fail(e, CPNV_FORMAT);
	if (!now || !deadline || now >= deadline ||
	    (e->initialized && (now < e->last_ms || deadline != e->deadline_ms)))
		return fail(e, CPNV_TIME);
	if (e->initialized && epoch != e->owner_epoch)
		return fail(e, CPNV_OWNER);
	if (!e->initialized && x[2].state != 1)
		return fail(e, CPNV_LOST);
	next = *e;
	for (unsigned i = 0; i < 4; ++i) {
		if (!p[i].inode || (p[i].mode & 0170000u) != 0100000u || (p[i].mode & 0222u))
			return fail(e, CPNV_PROVIDER);
		for (unsigned j = 0; j < i; ++j)
			if (p[i].device == p[j].device && p[i].inode == p[j].inode)
				return fail(e, CPNV_PROVIDER);
		if ((i < 3 && (x[i].state > 2 || x[i].blocked || x[i].error)) ||
		    (i == 3 && (x[i].state > 1 || x[i].blocked > 1 || x[i].error > 0)))
			return fail(e, CPNV_FORMAT);
		if (x[i].state == 2)
			return fail(e, CPNV_LOST);
		if (x[i].blocked)
			return fail(e, CPNV_BLOCKED);
		if (x[i].error)
			return fail(e, CPNV_DRIVER_ERROR);
		if (e->initialized &&
		    (p[i].device != e->provider[i].device || p[i].inode != e->provider[i].inode ||
		     p[i].mode != e->provider[i].mode))
			return fail(e, CPNV_PROVIDER);
		if (e->armed_mask & (1u << i)) {
			if (x[i].generation != e->generation[i])
				return fail(e, CPNV_GENERATION);
			if (x[i].state != 1)
				return fail(e, CPNV_LOST);
		} else if (x[i].state == 1) {
			next.generation[i] = x[i].generation;
			next.armed_mask |= 1u << i;
		}
		next.provider[i] = p[i];
	}
	next.owner_epoch = epoch;
	next.deadline_ms = deadline;
	next.last_ms = now;
	next.initialized = 1;
	*e = next;
	return next.armed_mask == 15 ? CPNV_OK : CPNV_PENDING;
}
int cpnv_observe_owner(const struct cpnv_observed_owner *o)
{
	if (!o || !o->expected_epoch || o->expected_epoch != o->observed_epoch ||
	    o->expected_parent < 2 || o->expected_parent != o->observed_parent ||
	    (o->required_mask != 15 && o->required_mask != 31) ||
	    o->live_mask != o->required_mask || o->owner_count != 1 || o->identity_checked != 1)
		return CPNV_OWNER;
	if (o->lease_open != 1 || o->unexpected_lease_bytes)
		return CPNV_LEASE;
	return CPNV_OK;
}
int cpnv_inspect_notify_grant(const uint8_t *b, size_t n, uint64_t epoch, uint32_t generation)
{
	static const char magic[] = "RNGRNT01";
	if (!b || n != 24 || !epoch)
		return CPNV_ENVELOPE;
	for (unsigned i = 0; i < 8; ++i)
		if (b[i] != (uint8_t)magic[i])
			return CPNV_ENVELOPE;
	if (le64(b + 8) != epoch || le32(b + 16) != generation || le32(b + 20) != 8)
		return CPNV_ENVELOPE;
	return CPNV_OK;
}
int cpnv_observe_reload(struct cpnv_reload_observation *g, const uint8_t *b, size_t n,
			int syscall_result, unsigned complete)
{
	uint64_t cookie;
	uint32_t stopped, started;
	if (!g || !b || n != 64 || syscall_result || complete > 1 || g->completed)
		return CPNV_RELOAD;
	cookie = le64(b + 8);
	stopped = le32(b + 24);
	started = le32(b + 28);
	if (le32(b) != 1 || le32(b + 4) != (complete ? 3u : 1u) || !cookie ||
	    (g->cookie && cookie != g->cookie) || le32(b + 16) != 5 || le32(b + 20) != 1 ||
	    stopped > 7 || started > 7 || le32(b + 32) || le32(b + 36) != UINT32_MAX ||
	    (started && stopped != 7) || (g->stopped & ~stopped) || (g->started & ~started) ||
	    (complete && (stopped != 7 || started != 7)))
		return CPNV_RELOAD;
	for (unsigned i = 40; i < 64; ++i)
		if (b[i])
			return CPNV_RELOAD;
	g->cookie = cookie;
	g->stopped = stopped;
	g->started = started;
	g->completed = complete;
	return CPNV_OK;
}
int cpnv_inspect_nv_grant(const uint8_t *b, size_t n, uint64_t epoch, uint32_t parent)
{
	static const char magic[] = "NVOWN101";
	if (!b || n != 32 || !epoch || parent < 2)
		return CPNV_ENVELOPE;
	for (unsigned i = 0; i < 8; ++i)
		if (b[i] != (uint8_t)magic[i])
			return CPNV_ENVELOPE;
	if (le64(b + 8) != epoch || le32(b + 16) != parent || le32(b + 20) != 3 || le64(b + 24))
		return CPNV_ENVELOPE;
	return CPNV_OK;
}
