/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_HALO_AP_STATE_H
#define UDX_HALO_AP_STATE_H
#include <stddef.h>
#include <stdint.h>
#define HALO_AP_FRAME_BYTES 16
/* Query (command0) and initialized mark (command1) have distinct entrypoints.
 * The mark codec is used only after normal authentication and feature checks;
 * neither frame by itself proves authentication or authorizes a reset. */
struct halo_ap_result {
	uint64_t generation;
	uint32_t written, received;
	unsigned query_complete, peer_state_known, peer_state, write_attempted;
	int error;
};
struct halo_ap_io {
	void *ctx;
	long (*read)(void *, void *, size_t);
	long (*write)(void *, const void *, size_t);
	int (*wait)(void *, unsigned writing, uint64_t absolute_deadline);
	int (*now)(void *, uint64_t *);
};
void halo_ap_query_frame(uint8_t[16]);
void halo_ap_mark_frame(uint8_t[16]);
int halo_ap_state_decode(const uint8_t *, size_t, unsigned *);
int halo_ap_query(const struct halo_ap_io *, uint64_t generation, unsigned timeout_ms,
		  struct halo_ap_result *);
#endif
