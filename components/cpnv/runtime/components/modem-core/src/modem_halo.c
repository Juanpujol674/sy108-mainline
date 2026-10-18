/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "modem_control.h"
#include "halo_auth.h"
#include "halo_material.h"
#include "halo_feature.h"
#include "halo_peer_gate.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/random.h>
#include <poll.h>
#include <time.h>
static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p)
{
	return get32(p) | (uint64_t)get32(p + 4) << 32;
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
static void wipe(void *p, size_t n)
{
	volatile uint8_t *v = p;
	while (n--)
		*v++ = 0;
}
/* One outstanding request. A fresh private provider stream per controller epoch
 * is mandatory for this adapter. MCSIGN01 op1 is an AP implementation choice,
 * not a recovered stock daemon or a CP wire requirement. There is no op2. */
int mc_provider_rpc(struct mc_runtime *r, unsigned op, const uint8_t *input, size_t in_n,
		    uint8_t *output, size_t out_n)
{
	uint8_t request[176] = {0}, response[176] = {0};
	if (op != 1 || in_n != 128 || out_n != 128)
		return -EINVAL;
	memcpy(request, "MCSIGN01", 8);
	put32(request + 8, op);
	put32(request + 12, (uint32_t)in_n);
	put64(request + 16, r->config.epoch);
	put64(request + 24, r->deadline);
	put32(request + 32, op);
	memcpy(request + 48, input, in_n);
	struct pollfd pending = {r->config.provider_rx, POLLIN, 0};
	if (poll(&pending, 1, 0) < 0)
		return -errno;
	if (pending.revents)
		return -EPROTO;
	int e = mc_exact(r, r->config.provider_tx, 1, request, 48 + in_n);
	if (e)
		goto done;
	e = mc_exact(r, r->config.provider_rx, 0, response, 48 + out_n);
	if (e)
		goto done;
	if (memcmp(response, "MCSIGN01", 8) || get32(response + 8) != op ||
	    get32(response + 12) != out_n || get64(response + 16) != r->config.epoch ||
	    get64(response + 24) != r->deadline || get32(response + 32) != op ||
	    get32(response + 36)) {
		e = -EPROTO;
		goto done;
	}
	if (get32(response + 40)) {
		e = -EKEYREJECTED;
		goto done;
	}
	if (get32(response + 44)) {
		e = -EPROTO;
		goto done;
	}
	pending.revents = 0;
	if (poll(&pending, 1, 0) < 0) {
		e = -errno;
		goto done;
	}
	if (pending.revents) {
		e = -EPROTO;
		goto done;
	}
	e = mc_guard(r);
	if (!e)
		memcpy(output, response + 48, out_n);
done:
	wipe(request, sizeof request);
	wipe(response, sizeof response);
	return e;
}
static int provider_now(void *ctx, uint64_t *out)
{
	int e = mc_guard(ctx);
	return e ? e : mc_now(out);
}
static int provider_clock(void *ctx, uint32_t *out)
{
	int e = mc_guard(ctx);
	if (e)
		return e;
	clock_t c = clock();
	if (c == (clock_t)-1)
		return -EIO;
	*out = (uint32_t)(uint64_t)c;
	return mc_guard(ctx);
}
static int entropy(void *ctx, uint64_t generation, uint64_t deadline, uint8_t out[16])
{
	struct mc_runtime *r = ctx;
	if (generation != r->config.epoch || deadline != r->deadline)
		return -ESTALE;
	int e = mc_guard(r);
	if (e)
		return e;
	ssize_t n = getrandom(out, 16, GRND_NONBLOCK);
	int saved = errno;
	e = mc_guard(r);
	return e ? e : n == 16 ? 0 : n < 0 ? -saved : -EIO;
}
static int private_operation(void *ctx, uint64_t generation, uint64_t deadline,
			     const uint8_t in[128], uint8_t out[128])
{
	struct mc_runtime *r = ctx;
	if (generation != r->config.epoch || deadline != r->deadline)
		return -ESTALE;
	return mc_provider_rpc(r, 1, in, 128, out, 128);
}
static void release(void *ctx, uint64_t g)
{
	(void)ctx;
	(void)g; /* Provider must zeroize its private transient request independently. */
}
static int feature_query(struct mc_runtime *r, const uint8_t token[4], const uint8_t key[8])
{
	uint8_t request[9], reply[HALO_FEATURE_REPLY_BYTES];
	uint32_t word;
	int e = provider_clock(r, &word);
	if (e)
		return e;
	uint8_t nonce[2] = {(uint8_t)word, (uint8_t)(word >> 8)};
	halo_feature_query_frame(token, nonce, (uint8_t)r->config.factory_tail, request);
	if ((e = mc_exact(r, r->halo_tx, 1, request, sizeof request)) ||
	    (e = mc_exact(r, r->halo_fd, 0, reply, sizeof reply)))
		goto done;
	e = halo_feature_decode(reply, sizeof reply, key, &r->feature_bits, &r->feature_status);
	if (!e) {
		e = mc_guard(r);
		if (!e)
			r->feature_validated = 1;
	}
done:
	wipe(request, sizeof request);
	wipe(reply, sizeof reply);
	return e;
}
static int initialized_mark(struct mc_runtime *r)
{
	uint8_t request[16], reply[16];
	unsigned state;
	uint64_t now;
	int e;
	if (!r->auth_complete || !r->factory_accepted || !r->feature_validated)
		return -EACCES;
	halo_ap_mark_frame(request);
	if ((e = mc_exact(r, r->halo_tx, 1, request, sizeof request)) ||
	    (e = mc_exact(r, r->halo_fd, 0, reply, sizeof reply)))
		goto done;
	e = halo_ap_state_decode(reply, sizeof reply, &state);
	if (e)
		goto done;
	if (state > 1) {
		e = -EPROTO;
		goto done;
	}
	if ((e = mc_guard(r)) || (e = mc_now(&now)))
		goto done;
	if (now < r->last_now || now > UINT64_MAX - 30000) {
		e = -EIO;
		goto done;
	}
	r->last_now = now;
	r->mark_reply_state = state;
	r->mark_completed = 1;
	r->next_mark_ms = now + 30000;
done:
	wipe(request, sizeof request);
	wipe(reply, sizeof reply);
	if (e)
		r->maintenance_error = e;
	return e;
}
int mc_halo_maintain(struct mc_runtime *r)
{
	uint64_t now;
	int e;
	if (!r)
		return -EINVAL;
	if (r->maintenance_error)
		return r->maintenance_error;
	if (!r->mark_completed || !r->feature_validated || !r->auth_complete ||
	    !r->factory_accepted)
		return -EACCES;
	if ((e = mc_now(&now)))
		return r->maintenance_error = e;
	if (now < r->last_now)
		return r->maintenance_error = -EIO;
	r->last_now = now;
	if (now < r->next_mark_ms)
		return 0;
	/* Target sleeps 30s after each completed mark, not a catch-up burst. The
	 * new owner performs one bounded attempt; timeout never resends the frame. */
	if ((e = mc_deadline(r, 5000)))
		return r->maintenance_error = e;
	return initialized_mark(r);
}
static int halo_run(struct mc_runtime *r, const struct halo_credential_provider *direct,
		    const uint8_t feature_key[8])
{
	uint8_t factory_token[4] = {0}, params[HALO_VERIFICATION_BYTES] = {0}, factory[9];
	r->auth_complete = 0;
	r->factory_accepted = 0;
	r->feature_validated = 0;
	r->feature_status = 0;
	r->feature_bits = 0;
	r->mark_completed = 0;
	r->mark_reply_state = 0;
	r->next_mark_ms = 0;
	r->maintenance_error = 0;
	r->monitor_faults = 1;
	int e = mc_deadline(r, r->config.halo_ms);
	if (e)
		return e;
	const uint64_t phase_deadline = r->deadline;
	if (!direct) {
		ssize_t n = pread(r->config.verification_params, params, sizeof params, 0);
		if (n != sizeof params) {
			e = n < 0 ? -errno : -EINVAL;
			goto done;
		}
		if ((e = mc_guard(r)))
			goto done;
		feature_key = params + 128;
	}
	unsigned ap_state;
	if ((e = mc_halo_wait_peer_zero(r, &ap_state)))
		goto done;
	/* Raw zero permits only the next exchange attempt. The immutable total
	 * phase bound is never extended. Tighten the effective deadline to the old
	 * <=15s credential/factory/policy budget; provider RPC and callbacks use this
	 * exact same deadline. No fresh mc_deadline() or restoration to the larger
	 * phase bound occurs after the wait. */
	uint64_t now;
	if (mc_now(&now) || now < r->last_now || now >= phase_deadline) {
		e = -ETIMEDOUT;
		goto done;
	}
	r->deadline = phase_deadline - now > 15000 ? now + 15000 : phase_deadline;
	if ((e = mc_guard(r)))
		goto done;
	struct halo_material_provider material = {
	    r, provider_now, entropy, provider_clock, private_operation, release, params};
	struct halo_credential_provider provider;
	struct halo_auth_result result;
	if (direct)
		provider = *direct;
	else {
		e = halo_material_adapter(&material, &provider);
		if (e)
			goto done;
	}
	if (mc_now(&now) || now < r->last_now || now >= r->deadline) {
		e = -ETIMEDOUT;
		goto done;
	}
	unsigned left =
	    (unsigned)(r->deadline - now); /* Runner computes its own absolute deadline from now;
					      callbacks use its exact value. */
	struct halo_auth state = {0};
	e = halo_auth_begin(&state, &provider, r->config.epoch, now, left, 1);
	if (e)
		goto done;
	memcpy(factory_token, state.token, 4);
	/* Use event-loop primitives so one absolute deadline also covers provider RPC. */
	while (state.result.stage != HALO_AUTH_EXCHANGE_COMPLETE &&
	       state.result.stage != HALO_AUTH_FAILED) {
		const uint8_t *p;
		size_t count;
		uint8_t b[152];
		if ((e = mc_guard(r)))
			break;
		now = r->last_now;
		e = halo_auth_output(&state, r->config.epoch, now, &p, &count);
		if (!e) {
			e = mc_exact(r, r->halo_tx, 1, (void *)p, count);
			if (e)
				break;
			e = halo_auth_written(&state, r->config.epoch, r->last_now, count);
		} else if (e == -EAGAIN) {
			count = state.length - state.offset;
			e = mc_exact(r, r->halo_fd, 0, b, count);
			if (e)
				break;
			e = halo_auth_feed(&state, r->config.epoch, r->last_now, b, count);
			wipe(b, sizeof b);
		}
		if (e)
			break;
	}
	if (state.result.stage != HALO_AUTH_EXCHANGE_COMPLETE) {
		if (state.result.stage != HALO_AUTH_FAILED)
			halo_auth_fail(&state, r->config.epoch, e ? e : -EIO);
		e = state.result.error;
		goto done;
	}
	result = state.result;
	if (!result.credential_verified) {
		e = -EKEYREJECTED;
		goto done;
	}
	r->auth_complete = 1;
	uint32_t word;
	if ((e = provider_clock(r, &word)))
		goto done;
	uint8_t nonce[2] = {(uint8_t)word, (uint8_t)(word >> 8)};
	halo_factory_query_frame(factory_token, nonce, (uint8_t)r->config.factory_tail, factory);
	if ((e = mc_exact(r, r->halo_tx, 1, factory, 9)) ||
	    (e = mc_exact(r, r->halo_fd, 0, factory, 9)))
		goto done;
	/* Target table +0x20 admits only completed AP/CP verification (states 8/10),
	 * then +0x10 issues this query. Do not replace that order with a raw byte test
	 * before the verified exchange. No independent ENTITLE1 or expiry exists. */
	if ((e = halo_factory_accept(&result, factory, sizeof factory)))
		goto done;
	r->factory_accepted = 1;
	if ((e = mc_guard(r)) || (e = feature_query(r, factory_token, feature_key)))
		goto done;
	e = initialized_mark(r);
done:
	wipe(factory_token, sizeof factory_token);
	wipe(params, sizeof params);
	wipe(factory, sizeof factory);
	return e;
}

/* The source caller may supply an in-process implementation. Neither this
 * entry nor the CP wire requires the optional MCSIGN01 pipe adapter. */
int mc_halo_with_provider(struct mc_runtime *r, const struct halo_credential_provider *provider,
			  const uint8_t feature_key[8])
{
	if (!r || !provider || !feature_key)
		return -EINVAL;
	return halo_run(r, provider, feature_key);
}
int mc_halo(struct mc_runtime *r)
{
	if (!r)
		return -EINVAL;
	return halo_run(r, NULL, NULL);
}
