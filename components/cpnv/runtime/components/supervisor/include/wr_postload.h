/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_WR_POSTLOAD_H
#define UDX_WR_POSTLOAD_H
#include <stddef.h>
#include <stdint.h>
#define WR_POSTLOAD_FDS 6u
struct wr_factory;
struct wr_parent;
/* Explicit AP-local preloaded or initial-load mode. Provider protocol/policy are project
 * adapters, not evidence that stock firmware requires an external service. */
struct wr_postload {
	unsigned enabled, initial_load, consumed, alive_ms, halo_ms, factory_tail, opened, alive,
	    ready, used;
	int fd[WR_POSTLOAD_FDS], events;
	uint32_t status_generation, halo_generation;
	uint64_t last_ms;
	unsigned char partial[64];
};
void wr_postload_init(struct wr_postload *);
int wr_postload_parse(const char *, size_t, struct wr_postload *);
/* On success transfers the six supplied inherited descriptors to this object;
 * caller closes profile_fd separately. Failure does not adopt/close inputs.
 * Single-threaded FD-serialized bootstrap; no hidden aliases are proved absent. */
int wr_postload_load(int profile_fd, struct wr_postload *);
/* Validate already mapped role FDs, including SCM_RIGHTS delivery. No ownership
 * transfer or close on failure; excluded_fd may be -1. */
int wr_postload_validate_fds(struct wr_postload *, int excluded_fd);
int wr_postload_fresh(const struct wr_postload *, unsigned loading);
int wr_postload_attach(struct wr_postload *, struct wr_factory *);
/* NV is part of CP startup: wait for the live CP/channel identity, then
 * service its requests while the independent HALO exchange completes.
 * Only wr_postload_wait/admit permit final public readiness. */
int wr_postload_wait_channels(struct wr_parent *, uint64_t deadline);
int wr_postload_nv_admit(struct wr_parent *, uint64_t deadline);
int wr_postload_wait(struct wr_parent *, uint64_t deadline);
int wr_postload_check(struct wr_parent *);
/* Parent-side authorization freshness at a concrete grant boundary. Not a
 * new authentication operation or an atomic guarantee against future events. */
int wr_postload_admit(struct wr_parent *, uint64_t deadline);
void wr_postload_close(struct wr_postload *);
#endif
