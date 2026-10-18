/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_WR_NV_H
#define UDX_WR_NV_H
#include "nvs_launch.h"
#include "wr_roles.h"
struct wr_nv {
	int report, lease, quiesce, source_pidfd;
	pid_t source;
	unsigned live;
};
struct wr_nv_plan {
	struct nvs_launch launch;
	int self_exe;
	/* Parent-only, invoked before creating a source and around each lease grant.
	 * No callback executes in the child. Negative errno revokes the lease. */
	int (*admission)(void *, uint64_t);
	void *admission_context;
};
int wr_nv_start(struct wr_nv *, struct wr_roles *, struct wr_nv_plan *, uint64_t deadline);
int wr_nv_quiesce(struct wr_nv *, struct wr_roles *, uint64_t deadline, unsigned *dirty);
int wr_nv_revoke(struct wr_nv *, struct wr_roles *, uint64_t deadline);
void wr_nv_close(struct wr_nv *);
#endif
