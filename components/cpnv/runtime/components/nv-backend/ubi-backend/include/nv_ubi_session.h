/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_NV_UBI_SESSION_H
#define UDX_NV_UBI_SESSION_H
#include "nv_ubi_backend.h"
/* Recovered AP framing/commands, independently implemented. This adapter takes
 * exact, already delimited outer frames; read(fd) is NOT a packet boundary.
 * It does not emit progress ACKs or perform transport I/O. */
struct nv_ubi_session {
	struct nv_ubi_backend *backend;
	uint32_t partition, group, sub, sequence, commands, frames, remaining, offset;
	unsigned header_used, seen, active, complete, failed;
	uint8_t header[NV_COMMAND_SIZE];
};
#define NVU_PROTOCOL (-20)
void nv_ubi_session_init(struct nv_ubi_session *, struct nv_ubi_backend *, uint32_t partition);
/* 0 = frame accepted, 1 = valid END committed; negative = terminal failure.
 * END must be the last command AND last byte in its exact outer frame. */
int nv_ubi_session_frame(struct nv_ubi_session *, const uint8_t *, size_t);
int nv_ubi_session_finish(struct nv_ubi_session *);
#endif
