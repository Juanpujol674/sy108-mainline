/* SPDX-License-Identifier: GPL-2.0-only UDX stock NV semantics, no sidecar format. */
#ifndef UDX_NV_VENDOR_H
#define UDX_NV_VENDOR_H
#include <stddef.h>
#include <stdint.h>
/* Stock modem_control 0x405dd8 accepts magic plus either checksum width.
 * The board's independently known payload size bounds untrusted headers. */
int nv_vendor_valid(const uint8_t *, size_t, uint32_t, unsigned *);
/* Boot selection is primary first; runtime cp_diskserver may choose either
 * replica first. The first valid image wins; valid-but-different is allowed. */
int nv_vendor_select(const uint8_t *, int, const uint8_t *, int, uint32_t, unsigned, unsigned *);
/* Explicit boot/recovery only, CP stopped and caller owns both UBI volumes.
 * Repairs ONLY an invalid replica from the valid one, using stock VOLUP size.
 * Both-valid replicas remain unchanged even when their contents differ. */
struct nv_ubi_backend;
int nvc_verify_noop(struct nv_ubi_backend *, uint32_t, uint32_t);
void nv_vendor_runtime_timestamp(uint32_t, uint32_t);
void nv_vendor_cache_release(void);
int nv_vendor_initial_timestamps(const int fix_fd[2], int runtime_fd, uint32_t *, uint32_t *,
				 uint64_t);
int nv_vendor_write_image(int, const uint8_t *, uint32_t, uint64_t);
int nv_vendor_repair_pair(const int fd[2], uint32_t payload, uint64_t deadline);
#endif
