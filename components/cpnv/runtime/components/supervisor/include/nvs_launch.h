/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVS_LAUNCH_H
#define NVS_LAUNCH_H
#include "nvs_devices.h"
#include <signal.h>
extern volatile sig_atomic_t nvs_cancelled;
void nvs_cancel_handler(int);
#include "nv_epoch.h"
struct nvs_launch {
	struct nvs_profile profile;
	struct nvs_devices devices;
	struct nvc_epoch epoch;
	int parent_report, parent_lease, quiesce, unified_parent;
	int owner, boot, consumer, broker, transport, tx, old_pidfd, cp_phase_fd, cp_parent_pid,
	    cp_owner_pidfd, cp_owner_exe_fd;
	uint64_t nonce;
};
int nvs_run_broker(struct nvs_launch *);
#endif
