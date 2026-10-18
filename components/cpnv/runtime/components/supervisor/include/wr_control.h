/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_WR_CONTROL_H
#define UDX_WR_CONTROL_H
#include "wr_postload.h"
#include <stdint.h>
#include <sys/types.h>
struct wr_parent;
struct wr_control_request {
	unsigned postload, status;
	char challenge[33];
	struct wr_postload profile;
};
/* Existing local SOCK_SEQPACKET / SO_PEERCRED mechanism. New operation sends
 * the existing profile text and six SCM_RIGHTS descriptors in role order.
 * Numeric FDs inside text are sender-local labels, never receiver authority. */
int wr_control_peer(int socket_fd, uid_t expected_uid);
int wr_control_send(int socket_fd, const struct wr_postload *fresh_or_null);
/* Read-only same-loop challenge: status <32 lowercase hexadecimal bytes>\n.
 * One SOCK_SEQPACKET message, no ancillary FDs. Reply is CPNV_STATUS1 with an
 * exact nonce echo and freshly checked parent/session health fields. */
int wr_control_status_send(int socket_fd, const char *challenge);
int wr_control_receive(int socket_fd, uid_t expected_uid, struct wr_control_request *);
void wr_control_request_close(struct wr_control_request *);
/* One accepted peer, one request, no retry. Parent retains controller identity
 * even if reply delivery fails. expected_uid is 0 in the production daemon. */
int wr_control_dispatch(struct wr_parent *, int socket_fd, uint64_t nonce, uid_t expected_uid);
int wr_control_client(int profile_fd_or_minus_one);
#endif
