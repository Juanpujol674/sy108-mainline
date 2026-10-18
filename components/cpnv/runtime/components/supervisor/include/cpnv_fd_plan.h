/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPNV_FD_PLAN_H
#define CPNV_FD_PLAN_H
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#define CPB_FD_MAX 128u
struct cpb_fd_entry {
	int fd, access;
	struct stat identity;
};
struct cpb_fd_plan {
	pid_t creator;
	unsigned count, valid;
	struct cpb_fd_entry keep[CPB_FD_MAX];
};
/* Prepare in the single-threaded/FD-serialized launcher BEFORE fork. -1 marks
 * optional absent entries; other invalid FDs reject. Duplicate entries merge.
 * This is FD-set bookkeeping, never executable/CP authorization. */
int cpb_fd_plan_prepare(struct cpb_fd_plan *, const int *, unsigned count);
/* Only in the fork child, before any untrusted code. All nonlisted descriptors
 * including stdio close using close_range; listed inode/type/access are checked
 * before any close. No malloc/procfs enumeration. Failure can be destructive:
 * the child must _exit, never continue to a grant or exec. No ENOSYS fallback.
 * FD-table sharing (clone CLONE_FILES), threads or FD mutations during this
 * operation are unsupported; normal fork supplies a private descriptor table. */
int cpb_fd_plan_apply_child(const struct cpb_fd_plan *);
/* Parent-side internal pipe acknowledgement of completed FD pruning, not
 * exec success, role authentication, CP readiness or permission to reload.
 * Caller keeps an actual process pidfd acquired before any child reaper. */
int cpb_fd_plan_wait_ready(const struct cpb_fd_plan *, int ack_read_fd, int child_pidfd,
			   uint64_t deadline_ms);
#endif
