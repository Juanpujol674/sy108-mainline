/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_NV_UBI_LINUX_H
#define UDX_NV_UBI_LINUX_H
#include "nv_ubi_backend.h"
/* The adapter does not infer volume identity/geometry from names, query raw
 * MTD, open sysfs or obtain ownership. The supervisor supplies current verified
 * UBI inventory and a generation-bound sole-writer witness. Missing callbacks
 * fail closed. Metadata must freshly establish device writable, volume not
 * corrupted, and no update/update-marker state. A successful pread alone does
 * not prove this: Linux logs vol->corrupted but may still return bytes.
 * A static cached geometry snapshot alone is not an epoch lease. */
struct nv_ubi_linux_context {
	void *supervisor;
	int (*volume_metadata)(void *, int, struct nv_ubi_identity *);
	int (*lease_valid)(void *, uint64_t);
};
struct nv_ubi_ops nv_ubi_linux_ops(struct nv_ubi_linux_context *);
#endif
