/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_CPROC_BACKEND_H
#define UDX_CPROC_BACKEND_H
#include "nv_backend.h"
enum cpb_action { CPB_STATUS = 1, CPB_START, CPB_STOP, CPB_LOAD, CPB_WATCH };
enum cpb_outcome { CPB_DENIED, CPB_AP_COMPLETE, CPB_FAILED, CPB_UNCERTAIN, CPB_CANCELLED };
enum cpb_status { CPB_STATUS_UNKNOWN, CPB_STATUS_STARTED, CPB_STATUS_STOPPED, CPB_STATUS_WDT };
struct cpb_request {
	unsigned action, domain, segment;
	int status_fd, action_fd, source_fd, owner_fd, signal_fd;
	uint64_t generation, source_offset, target_offset, length, capacity;
	unsigned timeout_ms;
	uint8_t digest[32];
};
struct cpb_result {
	uint64_t generation, bytes_written, submitted_ms, completed_ms;
	unsigned outcome, status, touched, source_hash_verified, target_hash_verified;
	int error;
};
struct cpb_io {
	struct nv_io file;
	int (*endpoint)(int fd, const char *domain, const char *leaf);
	long (*write)(int, const void *, size_t);
	int (*wait)(int fd, int signal_fd, unsigned timeout_ms);
	int (*now)(uint64_t *);
};
int cpb_execute(const struct cpb_io *, const struct cpb_request *, struct cpb_result *);
const char *cpb_domain(unsigned);
const char *cpb_segment(unsigned domain, unsigned segment);
/* Result never claims CP ready, independent halt, NV load or HALO success. */
#endif
