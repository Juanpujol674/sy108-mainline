/* SPDX-License-Identifier: GPL-2.0-only */
#include "halo_auth.h"
static void wipe(void *p, size_t n)
{
	volatile uint8_t *q = p;
	while (n--)
		*q++ = 0;
}
static uint8_t reverse(uint8_t x)
{
	x = (uint8_t)((x >> 4) | (x << 4));
	x = (uint8_t)(((x << 2) & 0xcc) | ((x >> 2) & 0x33));
	return (uint8_t)(((x << 1) & 0xaa) | ((x >> 1) & 0x55));
}
/* Instruction-equivalent to 40d02c/40d074. This is NOT a MAC or signature. */
static uint8_t transform(uint8_t x, const uint8_t token[4])
{
	unsigned i;
	for (i = 0; i < 4; i++) {
		uint8_t k = token[i];
		x = (uint8_t)(reverse((uint8_t)(reverse((uint8_t)(x ^ k)) ^ reverse(k))) ^ k);
		x = reverse(x);
	}
	return x;
}
void halo_token_encode(const uint8_t t[4], const uint8_t n[2], uint8_t out[4])
{
	uint8_t a = n[0], b = n[1];
	out[2] = transform(a, t);
	out[3] = transform(b, t);
	out[0] = a;
	out[1] = b;
}
int halo_token_check(const uint8_t t[4], const uint8_t p[4])
{
	unsigned diff = (unsigned)(transform(p[0], t) ^ p[2]);
	diff |= (unsigned)(transform(p[1], t) ^ p[3]);
	return diff == 0;
}
void halo_factory_query_frame(const uint8_t token[4], const uint8_t nonce[2], uint8_t tail,
			      uint8_t out[9])
{
	out[0] = 0xfc;
	out[1] = 0xab;
	out[2] = 0xa1;
	out[3] = 0;
	halo_token_encode(token, nonce, out + 4);
	out[8] = tail;
}
int halo_factory_reply_decode(const uint8_t *p, size_t n, unsigned *out)
{
	if (!p || !out || n != 9)
		return -22;
	*out = p[8];
	return 0;
}
int halo_factory_accept(const struct halo_auth_result *result, const uint8_t *p, size_t n)
{
	unsigned status;
	int e;
	if (!result || result->stage != HALO_AUTH_EXCHANGE_COMPLETE || result->error ||
	    !result->exchange_complete || !result->credential_verified)
		return -129;
	if ((e = halo_factory_reply_decode(p, n, &status)))
		return e;
	return status == 1 ? 0 : status == 0xf0 ? -74 : -129;
}
static int live(const struct halo_auth *s)
{
	return s->result.stage >= HALO_AUTH_TX_MATERIAL &&
	       s->result.stage <= HALO_AUTH_RX_REPORT_ACK;
}
static int transmitting(const struct halo_auth *s)
{
	return s->result.stage == HALO_AUTH_TX_MATERIAL || s->result.stage == HALO_AUTH_TX_FETCH ||
	       s->result.stage == HALO_AUTH_TX_REPORT;
}
static int finish(struct halo_auth *s, int error)
{
	s->result.error = error;
	s->result.stage = error ? HALO_AUTH_FAILED : HALO_AUTH_EXCHANGE_COMPLETE;
	s->result.exchange_complete = !error;
	wipe(s->frame, sizeof(s->frame));
	wipe(s->token, sizeof(s->token));
	s->length = s->offset = 0;
	if (s->provider_active) {
		s->provider_active = 0;
		s->busy = 1;
		s->provider.release(s->provider.ctx, s->result.generation);
		s->busy = 0;
	}
	return error;
}
static int guard(struct halo_auth *s, uint64_t generation, uint64_t now)
{
	if (!s || !generation)
		return -22;
	if (s->busy)
		return -16;
	if (generation != s->result.generation)
		return -116;
	if (!live(s))
		return s->result.error ? s->result.error : -114;
	if (now < s->last_now)
		return finish(s, -5);
	s->last_now = now;
	if (now >= s->deadline)
		return finish(s, -110);
	return 0;
}
static int after_provider(struct halo_auth *s)
{
	uint64_t current;
	int e;
	s->busy = 1;
	e = s->provider.monotonic_ms(s->provider.ctx, &current);
	s->busy = 0;
	if (e)
		return finish(s, -5);
	return guard(s, s->result.generation, current);
}
static int wire_clock(struct halo_auth *s, uint32_t *out)
{
	int e, expired;
	s->busy = 1;
	e = s->provider.clock_word(s->provider.ctx, out);
	s->busy = 0;
	expired = after_provider(s);
	if (expired)
		return expired;
	return e ? (e < 0 ? e : -5) : 0;
}
static int packet(struct halo_auth *s, unsigned command, unsigned size)
{
	uint32_t word;
	uint8_t n[2];
	int e = wire_clock(s, &word);
	if (e)
		return finish(s, e);
	n[0] = (uint8_t)word;
	n[1] = (uint8_t)(word >> 8);
	wipe(s->frame, sizeof(s->frame));
	s->frame[0] = 0xfc;
	s->frame[1] = 0xab;
	s->frame[2] = (uint8_t)command;
	s->frame[3] = (uint8_t)(size - 8);
	halo_token_encode(s->token, n, s->frame + 4);
	s->length = size;
	s->offset = 0;
	return 0;
}
int halo_auth_begin(struct halo_auth *s, const struct halo_credential_provider *p,
		    uint64_t generation, uint64_t now, unsigned ms, unsigned allow)
{
	int e;
	unsigned i;
	uint32_t word;
	struct halo_credential_provider saved = {0};
	if (!s)
		return -22;
	if (s->busy || live(s))
		return -16;
	if (!generation || generation <= s->result.generation || !ms || ms > 15000 ||
	    now > UINT64_MAX - ms)
		return -22;
	if (p)
		saved = *p;
	wipe(s, sizeof(*s));
	s->result.generation = generation;
	s->last_now = now;
	s->deadline = now + ms;
	if (!allow)
		return finish(s, -13);
	p = &saved;
	if (!p->monotonic_ms || !p->clock_word || !p->prepare || !p->verify || !p->release)
		return finish(s, -95);
	s->provider = *p;
	s->provider_active = 1;
	s->result.stage = HALO_AUTH_TX_MATERIAL;
	/* The observed initial token is clock-low32 in network byte order. */
	e = wire_clock(s, &word);
	if (e)
		return finish(s, e);
	for (i = 0; i < 4; i++)
		s->token[i] = (uint8_t)(word >> (24 - 8 * i));
	s->frame[0] = 0xfc;
	s->frame[1] = 0xab;
	s->frame[2] = 0xa0;
	s->frame[3] = 0x90;
	for (i = 0; i < 4; i++)
		s->frame[4 + i] = s->token[i];
	s->busy = 1;
	e = p->prepare(p->ctx, generation, s->deadline, s->frame + 8);
	s->busy = 0;
	{
		int expired = after_provider(s);
		if (expired)
			return expired;
	}
	if (e)
		return finish(s, e < 0 ? e : -71);
	s->length = 152;
	return 0;
}
int halo_auth_tick(struct halo_auth *s, uint64_t generation, uint64_t now)
{
	return guard(s, generation, now);
}
int halo_auth_output(struct halo_auth *s, uint64_t generation, uint64_t now, const uint8_t **p,
		     size_t *n)
{
	int e;
	if (!p || !n)
		return -22;
	*p = 0;
	*n = 0;
	e = guard(s, generation, now);
	if (e)
		return e;
	if (!transmitting(s))
		return -11;
	*p = s->frame + s->offset;
	*n = s->length - s->offset;
	return 0;
}
int halo_auth_written(struct halo_auth *s, uint64_t generation, uint64_t now, size_t n)
{
	int e = guard(s, generation, now);
	if (e)
		return e;
	if (!transmitting(s))
		return -71;
	s->result.write_attempted = 1;
	if (!n || n > s->length - s->offset)
		return finish(s, -5);
	s->offset += n;
	s->result.written += (uint32_t)n;
	if (s->offset != s->length)
		return 0;
	s->result.stage++;
	s->length = s->result.stage == HALO_AUTH_RX_PEER ? 152 : 9;
	s->offset = 0;
	wipe(s->frame, sizeof(s->frame));
	return 0;
}
int halo_auth_feed(struct halo_auth *s, uint64_t generation, uint64_t now, const uint8_t *p,
		   size_t n)
{
	size_t i;
	int e = guard(s, generation, now);
	if (e)
		return e;
	if (transmitting(s))
		return -71;
	if (!p || !n || n > s->length - s->offset)
		return finish(s, -90);
	for (i = 0; i < n; i++) {
		s->frame[s->offset + i] = p[i];
	}
	s->offset += n;
	s->result.received += (uint32_t)n;
	if (s->offset != s->length)
		return 0;
	if (s->result.stage == HALO_AUTH_RX_MATERIAL_ACK) {
		if (s->frame[8] || !halo_token_check(s->token, s->frame + 4))
			return finish(s, -129);
		e = packet(s, 0xc0, 8);
		if (e)
			return e;
		s->result.stage = HALO_AUTH_TX_FETCH;
		return 0;
	}
	if (s->result.stage == HALO_AUTH_RX_PEER) {
		/* Exactly the header fields checked at 40bd78; byte2 is unproven. */
		if (s->frame[0] != 0x79 || s->frame[1] != 0xa5 || s->frame[3] != 0x90)
			return finish(s, -71);
		s->busy = 1;
		e = s->provider.verify(s->provider.ctx, generation, s->deadline, s->frame + 8);
		s->busy = 0;
		{
			int expired = after_provider(s);
			if (expired)
				return expired;
		}
		if (e)
			return finish(s, e < 0 ? e : -129);
		s->result.credential_verified = 1;
		e = packet(s, 0xc1, 9);
		if (e)
			return e; /* [8]=0 only after verify */
		s->result.stage = HALO_AUTH_TX_REPORT;
		return 0;
	}
	if (s->result.stage == HALO_AUTH_RX_REPORT_ACK) {
		if (s->frame[8])
			return finish(s, -129);
		return finish(s, 0);
	}
	return finish(s, -71);
}
int halo_auth_fail(struct halo_auth *s, uint64_t generation, int error)
{
	if (!s || error >= 0)
		return -22;
	if (s->busy)
		return -16;
	if (generation != s->result.generation)
		return -116;
	if (!live(s))
		return -114;
	return finish(s, error);
}
int halo_auth_cancel(struct halo_auth *s, uint64_t generation)
{
	return halo_auth_fail(s, generation, -125);
}
int halo_auth_run(const struct halo_ap_io *io, const struct halo_credential_provider *provider,
		  uint64_t generation, unsigned ms, unsigned allow, struct halo_auth_result *out)
{
	struct halo_auth s = {0};
	unsigned steps = 0;
	uint64_t now;
	int e;
	uint8_t input[152];
	if (!out)
		return -22;
	wipe(out, sizeof(*out));
	out->generation = generation;
	if (!io || !io->read || !io->write || !io->wait || !io->now)
		return out->error = -22;
	if (io->now(io->ctx, &now))
		return out->error = -5;
	e = halo_auth_begin(&s, provider, generation, now, ms, allow);
	if (e) {
		*out = s.result;
		return e;
	}
	while (live(&s)) {
		const uint8_t *p;
		size_t n;
		long actual;
		unsigned writing = transmitting(&s);
		if (++steps > 2048) {
			e = -11;
			break;
		}
		if (io->now(io->ctx, &now)) {
			e = -5;
			break;
		}
		e = halo_auth_tick(&s, generation, now);
		if (e)
			break;
		e = io->wait(io->ctx, writing, s.deadline);
		if (e == -4)
			continue;
		if (e)
			break;
		/* Cancellation/deadline checked by wait and again before the syscall. */
		if (io->now(io->ctx, &now)) {
			e = -5;
			break;
		}
		e = halo_auth_tick(&s, generation, now);
		if (e)
			break;
		if (writing) {
			e = halo_auth_output(&s, generation, now, &p, &n);
			if (e)
				break;
			s.result.write_attempted = 1;
			actual = io->write(io->ctx, p, n);
		} else {
			n = s.length - s.offset;
			actual = io->read(io->ctx, input, n);
		}
		if (actual < 0) {
			if (!writing && (actual == -4 || actual == -11))
				continue;
			e = (int)actual;
			break;
		}
		if (!actual) {
			e = -32;
			break;
		}
		if ((size_t)actual > n) {
			e = -5;
			break;
		}
		/* A nonblocking syscall may still return after scheduler delay. Count
		 * actual transport bytes, but never consume an expired final frame. */
		{
			uint64_t fresh;
			if (io->now(io->ctx, &fresh))
				e = -5;
			else if (fresh < s.last_now)
				e = -5;
			else if (fresh >= s.deadline)
				e = -110;
			else {
				now = fresh;
				e = 0;
			}
			if (e) {
				if (writing)
					s.result.written += (uint32_t)actual;
				else
					s.result.received += (uint32_t)actual;
				break;
			}
		}
		e = writing ? halo_auth_written(&s, generation, now, (size_t)actual)
			    : halo_auth_feed(&s, generation, now, input, (size_t)actual);
		if (e)
			break;
	}
	if (live(&s))
		halo_auth_fail(&s, generation, e ? e : -5);
	wipe(input, sizeof(input));
	*out = s.result;
	return out->error;
}
