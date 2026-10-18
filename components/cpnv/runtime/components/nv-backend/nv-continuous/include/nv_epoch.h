/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NV_EPOCH_H
#define NV_EPOCH_H
#include <stdint.h>
#include <sys/stat.h>
struct nvc_epoch {
	int fd[4];
	uint32_t generation[4];
	struct stat identity[4];
	unsigned pinned, armed_mask;
};
/* Read current NV channel availability; generation is diagnostic only. */
int nvc_epoch_capture(struct nvc_epoch *);
int nvc_epoch_check(struct nvc_epoch *);
/* Open the actual NV transport after checking its current availability.
 * dir is a borrowed trusted /dev directory; no caller-chosen path. On failure
 * closes its newly opened descriptor. Production function is never in tests. */
int nvc_open_transport(struct nvc_epoch *, int dir, uint64_t expected_rdev);
#endif
