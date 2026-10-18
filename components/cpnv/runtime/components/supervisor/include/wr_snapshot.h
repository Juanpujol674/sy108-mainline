/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_WR_SNAPSHOT_H
#define UDX_WR_SNAPSHOT_H
#include <stdint.h>
struct wr_snapshot_result {
	uint32_t payload;
	unsigned checksum_bits;
	uint8_t payload_sha256[32];
};
/* Input UBI ownership/health and STOP/guard are established by the parent.
 * Inputs are pinned read-only descriptors. Outputs must be two distinct private
 * empty regular files owned by this uid. No live volume is ever written here.
 * Both valid but different copies are refused. Output write descriptors remain
 * owned by caller and MUST be closed before sending controller continuation. */
int wr_snapshot_nv_pair(const int input[2], const int output[2], uint32_t payload,
			uint64_t deadline, struct wr_snapshot_result *);
#endif
