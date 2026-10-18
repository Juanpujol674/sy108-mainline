/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_WR_ROLES_H
#define UDX_WR_ROLES_H
#include <stdint.h>
#include "cpnv_linux.h"
#include <sys/types.h>
#include <sys/stat.h>
#define WR_ROLE_COUNT 5u
enum wr_role { WR_CONTROLLER = 0, WR_SOURCE = 1, WR_BROKER = 2, WR_CONSUMER = 3, WR_NOTIFY = 4 };
struct wr_process {
	int pidfd, executable;
	pid_t pid, parent;
	unsigned registered;
	struct cpb_process *backend;
	uint64_t token, epoch;
};
struct wr_roles {
	unsigned required_mask;
	struct wr_process process[WR_ROLE_COUNT];
	uint64_t nonce, receipt_sequence;
	pid_t creator;
	int cleanup_error;
	unsigned unresolved_descendants;
};
void wr_roles_init(struct wr_roles *, uint64_t);
int wr_process_live(const struct wr_process *);
int wr_process_exited(const struct wr_process *);
int wr_roles_register(struct wr_roles *, enum wr_role, int pidfd, int exe, pid_t pid, pid_t parent);
int wr_role_send(int socket_fd, enum wr_role, int pidfd, pid_t pid, uint64_t nonce);
int wr_role_receive(int socket_fd, struct wr_roles *, enum wr_role, int exe, pid_t parent);
int wr_process_signal(const struct wr_process *, int signal);
int wr_wait_exited(const struct wr_process *, uint64_t deadline);
/* Direct-child wait + exact-pidfd collection, one immutable absolute bound.
 * EINTR/EAGAIN are retried with bounded sleeps. Descendant/subreaper collection
 * is outside this API. Facts are child-exit diagnostics, not authorization. */
int wr_wait_reaped(const struct wr_process *, uint64_t deadline, struct cpb_reap_fact *);
/* Fails without closing any role while one retained role is not exited or a
 * direct-child collection obligation remains unresolved. May collect some
 * children first; cached facts make recovery idempotent without losing status. */
int wr_roles_close(struct wr_roles *);
int wr_roles_exit_receipt(struct wr_roles *, struct lc_completion *);
#endif
