/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_WR_QUIESCE_H
#define UDX_WR_QUIESCE_H
#include <stdint.h>
#include <sys/types.h>
#define WR_QUIESCED (-300)
struct wr_quiesce {
	int fd;
	pid_t parent;
	uint64_t nonce;
	unsigned requested;
};
int wr_quiesce_open(struct wr_quiesce *);
int wr_quiesce_live(struct wr_quiesce *);
int wr_quiesce_request(struct wr_quiesce *);
int wr_quiesce_done(struct wr_quiesce *, unsigned dirty);
int wr_quiesce_send_request(int fd, uint64_t nonce);
int wr_quiesce_receive(int fd, const char magic[8], uint64_t nonce, pid_t consumer,
		       unsigned *dirty);
#endif
