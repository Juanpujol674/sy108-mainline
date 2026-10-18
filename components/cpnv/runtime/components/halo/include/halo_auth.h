/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_HALO_AUTH_H
#define UDX_HALO_AUTH_H
#include "halo_ap_state.h"
#define HALO_AUTH_MAX_FRAME 152u
/* These are exchange stages, not the vendor's global state values. */
enum halo_auth_stage {
	HALO_AUTH_IDLE,
	HALO_AUTH_TX_MATERIAL,
	HALO_AUTH_RX_MATERIAL_ACK,
	HALO_AUTH_TX_FETCH,
	HALO_AUTH_RX_PEER,
	HALO_AUTH_TX_REPORT,
	HALO_AUTH_RX_REPORT_ACK,
	HALO_AUTH_EXCHANGE_COMPLETE,
	HALO_AUTH_FAILED
};
/* Mandatory credential boundary. No default implementation or embedded key.
 * Callbacks return 0 only after their real operation succeeds, negative errno
 * otherwise. They MUST be bounded, nonblocking, non-reentrant and honour the
 * supplied generation/deadline. release is called once even if prepare fails.
 * A blocking security service must use an asynchronous adapter, not block this
 * event loop.
 * prepare: make the exact 144-byte credential-backed AP material.
 * verify: authenticate all 144 received bytes using lawful device credentials,
 * including whatever freshness/session checks that provider can substantiate.
 */
struct halo_credential_provider {
	void *ctx;
	/* Same monotonic domain as begin/tick; checked after every provider call. */
	int (*monotonic_ms)(void *, uint64_t *);
	/* Legacy wire words use clock() CPU ticks, separately from deadline time. */
	int (*clock_word)(void *, uint32_t *);
	int (*prepare)(void *, uint64_t generation, uint64_t deadline, uint8_t out[144]);
	int (*verify)(void *, uint64_t generation, uint64_t deadline, const uint8_t peer[144]);
	void (*release)(void *, uint64_t generation);
};
struct halo_auth_result {
	uint64_t generation;
	uint32_t written, received, stage;
	int error;
	unsigned exchange_complete, credential_verified, write_attempted;
	/* Always zero here: independent factory-license/CP gates are not bypassed. */
	unsigned license_verified, cp_ready;
};
struct halo_auth {
	struct halo_credential_provider provider;
	struct halo_auth_result result;
	uint64_t deadline, last_now;
	uint8_t token[4], frame[HALO_AUTH_MAX_FRAME];
	size_t length, offset;
	unsigned provider_active, busy;
};
/* Zero-initialize a session before first use. One event-loop owner; concurrent
 * callers must serialize. Reuse requires strictly increasing generation and a
 * new exclusive stream after any disconnect/failure. No automatic replay. */
void halo_token_encode(const uint8_t token[4], const uint8_t nonce[2], uint8_t out[4]);
int halo_token_check(const uint8_t token[4], const uint8_t encoded[4]);
/* Factory query uses FC AB A1 00 plus encoded token and a ninth byte that the
 * observed AP caller leaves uninitialized. Callers must explicitly choose that
 * byte under a separately validated profile; this library has no default.
 * Raw byte8 from a complete reply is observation, not authenticated license. */
void halo_factory_query_frame(const uint8_t token[4], const uint8_t nonce[2], uint8_t explicit_tail,
			      uint8_t out[9]);
int halo_factory_reply_decode(const uint8_t *, size_t, unsigned *raw_status);
/* Target policy: completed, credential-verified exchange first; then a full
 * factory reply with status 1. Status 0xf0 denotes a corrupted exchange.
 * No external entitlement flags or expiration are part of this target rule.
 * Caller must keep the same exclusive live stream/epoch between both phases. */
int halo_factory_accept(const struct halo_auth_result *, const uint8_t *, size_t);
int halo_auth_begin(struct halo_auth *, const struct halo_credential_provider *,
		    uint64_t generation, uint64_t now, unsigned timeout_ms, unsigned allow_write);
int halo_auth_tick(struct halo_auth *, uint64_t generation, uint64_t now);
int halo_auth_output(struct halo_auth *, uint64_t generation, uint64_t now, const uint8_t **,
		     size_t *);
int halo_auth_written(struct halo_auth *, uint64_t generation, uint64_t now, size_t);
int halo_auth_feed(struct halo_auth *, uint64_t generation, uint64_t now, const uint8_t *, size_t);
int halo_auth_cancel(struct halo_auth *, uint64_t generation);
int halo_auth_fail(struct halo_auth *, uint64_t generation, int negative_errno);
int halo_auth_run(const struct halo_ap_io *, const struct halo_credential_provider *,
		  uint64_t generation, unsigned timeout_ms, unsigned allow_write,
		  struct halo_auth_result *);
#endif
