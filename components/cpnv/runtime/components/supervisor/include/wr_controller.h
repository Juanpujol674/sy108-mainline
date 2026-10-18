/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_WR_CONTROLLER_H
#define UDX_WR_CONTROLLER_H
#include "wr_factory.h"
#include "wr_roles.h"
struct wr_controller {
	int lease, phase, report, continuation, pidfd;
	pid_t pid;
};
int wr_controller_spawn(struct wr_controller *, struct wr_factory *, struct wr_roles *,
			int executable, int directory, uint64_t deadline);
int wr_controller_report(struct wr_controller *, struct wr_roles *, const char magic[8],
			 uint64_t deadline);
int wr_controller_continue(struct wr_controller *, struct wr_factory *, struct wr_roles *,
			   uint64_t deadline);
int wr_controller_stop(struct wr_controller *, struct wr_roles *, uint64_t deadline);
void wr_controller_close(struct wr_controller *);
#endif
