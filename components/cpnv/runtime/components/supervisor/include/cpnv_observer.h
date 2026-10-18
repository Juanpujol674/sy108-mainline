/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPNV_OBSERVER_H
#define CPNV_OBSERVER_H
#include <stddef.h>
#include <stdint.h>

/* Offline, pure data API. It never opens a device, issues an ioctl, creates a
 * grant, or authorizes a write. A successful check is NOT a readiness claim.
 * State structs must start zero-initialized. Provider/owner fields are caller
 * observations, not independently verified process identities. */
enum cpnv_result {
	CPNV_OK = 0,
	CPNV_PENDING = 1,
	CPNV_FORMAT = -1,
	CPNV_TARGET = -2,
	CPNV_GENERATION = -3,
	CPNV_PROVIDER = -4,
	CPNV_LOST = -5,
	CPNV_BLOCKED = -6,
	CPNV_DRIVER_ERROR = -7,
	CPNV_TIME = -8,
	CPNV_OWNER = -9,
	CPNV_LEASE = -10,
	CPNV_ENVELOPE = -11,
	CPNV_RELOAD = -12
};
struct cpnv_snapshot {
	uint32_t generation, state, blocked;
	int32_t error;
};
struct cpnv_provider {
	uint64_t device, inode;
	uint32_t mode;
};
struct cpnv_epoch {
	uint64_t owner_epoch, deadline_ms, last_ms;
	uint32_t generation[4], armed_mask;
	struct cpnv_provider provider[4];
	unsigned initialized;
	int failure;
};
struct cpnv_observed_owner {
	uint64_t expected_epoch, observed_epoch;
	uint32_t expected_parent, observed_parent, required_mask, live_mask;
	uint32_t owner_count, lease_open, unexpected_lease_bytes, identity_checked;
};
struct cpnv_reload_observation {
	uint64_t cookie;
	uint32_t stopped, started, completed;
};

int cpnv_parse_snapshot(unsigned index, const char *, size_t, struct cpnv_snapshot *);
/* First sample requires snv_lte READY. Other channels may arm later. Once
 * armed, a slot can never be rebound in the same epoch. Errors are sticky. */
int cpnv_observe_epoch(struct cpnv_epoch *, uint64_t owner_epoch, const struct cpnv_snapshot[4],
		       const struct cpnv_provider[4], uint64_t now_ms, uint64_t deadline_ms);
int cpnv_observe_owner(const struct cpnv_observed_owner *);
/* Inspect an existing synthetic/captured RNGRNT01 envelope. No encoder/sender. */
int cpnv_inspect_notify_grant(const uint8_t *, size_t, uint64_t epoch, uint32_t generation);
int cpnv_inspect_nv_grant(const uint8_t *, size_t, uint64_t epoch, uint32_t parent);
/* Observe a 64-byte LE reload QUERY/COMPLETE response, never issue one.
 * Completed requires an observed zero syscall result: the kernel can fill a
 * COMPLETED reply and then fail its final commit. It remains a historical AP
 * fact, not a claim the guard is still held or hardware DMA is stopped. */
int cpnv_observe_reload(struct cpnv_reload_observation *, const uint8_t *, size_t,
			int syscall_result, unsigned is_complete);
#endif
