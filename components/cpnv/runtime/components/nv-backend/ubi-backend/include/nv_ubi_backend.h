/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_NV_UBI_BACKEND_H
#define UDX_NV_UBI_BACKEND_H
#include <stddef.h>
#include <stdint.h>
#include "nv_codec.h"
/* Backend identity is diagnostic provenance, not a security mechanism. */
enum nv_ubi_kind { NV_UBI_LINUX = 2 };
enum nv_ubi_health {
	NVU_UBI_WRITABLE = 1,
	NVU_VOLUME_UNCORRUPTED = 2,
	NVU_NO_UPDATE_MARKER = 4,
	NVU_HEALTH_REQUIRED = 7
};
struct nv_ubi_identity {
	uint64_t inode, device, rdev, capacity;
	uint32_t leb_size, min_io_size, owner_uid;
	uint32_t volume_type; /* 1 dynamic UBI, 2 static UBI */
	uint32_t health;      /* current affirmative NVU_HEALTH_REQUIRED, never assumed */
};
enum nv_ubi_write_policy {
	NVU_ALLOW_WRITE = 1,
	/* UBI_IOCVOLUP unmaps every reserved LEB, including bytes after the image. */
	NVU_ALLOW_DISCARD_TAIL = 2
};
struct nv_ubi_ops {
	void *ctx;
	enum nv_ubi_kind kind;
	int (*inspect)(void *, int, struct nv_ubi_identity *);
	long (*pread)(void *, int, void *, size_t, uint64_t);
	/* begin_update is UBI_IOCVOLUP with a signed 64-bit exact image length.
	 * Any error is conservatively considered potentially mutating. No retry. */
	int (*begin_update)(void *, int, uint64_t);
	long (*write)(void *, int, const void *, size_t);
	int (*sync)(void *, int);
	/* Required supervisor witness: sole writer, ownership and epoch still valid.
	 * Must pin both fd numbers/open descriptions against close/reuse and exclude
	 * writes via every alias throughout the transaction, including callbacks.
	 * Never implement this as a default-success stub or a borrowed dup-fd check. */
	int (*lease_valid)(void *, uint64_t);
	int (*now_ms)(void *, uint64_t *);
};
enum nv_ubi_state {
	NVU_CLOSED,
	NVU_READY,
	NVU_STAGING,
	NVU_CHECKING,
	NVU_UPDATING_BACKUP,
	NVU_BACKUP_VERIFIED,
	NVU_UPDATING_PRIMARY,
	NVU_BOTH_VERIFIED,
	NVU_REPLY_PREPARED,
	NVU_REPLY_WRITTEN,
	NVU_REPLY_LOST,
	NVU_REJECTED,
	NVU_RECOVERY_REQUIRED,
	NVU_DIVERGENT,
	NVU_CONFLICT,
	NVU_UNCERTAIN
};
enum nv_ubi_error {
	NVU_OK = 0,
	NVU_CONFIG = -1,
	NVU_IO = -2,
	NVU_IMAGE = -3,
	NVU_DISAGREE = -4,
	NVU_STATE = -5,
	NVU_BOUNDS = -6,
	NVU_READONLY = -7,
	NVU_CHANGED = -8,
	NVU_OWNERSHIP = -9,
	NVU_DEADLINE = -10,
	NVU_BUDGET = -11,
	NVU_REPLY_ERROR = -12
};
enum nv_ubi_step {
	NVU_UNTOUCHED,
	NVU_UPDATE_ATTEMPTED,
	NVU_STREAM_FINISHED,
	NVU_SYNCED,
	NVU_READBACK_VERIFIED
};
struct nv_ubi_backend {
	struct nv_ubi_ops io;
	struct nv_ubi_identity identity[2]; /* primary, backup */
	int fd[2], allow_write, last_error;
	uint32_t payload_size, group, sub;
	unsigned checksum_bits, possibly_changed, storage_verified, ack_attempted;
	enum nv_ubi_step step[2];
	enum nv_ubi_state state;
	uint64_t epoch, deadline_ms, last_ms;
	uint32_t calls_left;
	uint8_t *original, *staged;
	void *vendor_state; /* Optional stock-compatible backend private state. */
};
/* Borrowed fd only. No open/close, no automatic repair and no automatic rollback.
 * orig/stage buffers each hold payload+512 and must be non-overlapping.
 * expected[] must come from a trusted, independently verified UBI inventory.
 * checksum_bits is an independently established producer profile (32 or 64),
 * never inferred from a payload for which both checksums happen to agree.
 * allow_write is 0, NVU_ALLOW_WRITE, or both write-policy bits. Larger
 * volume capacity requires explicit NVU_ALLOW_DISCARD_TAIL for writes.
 * Sole ownership covers the WHOLE volume, not just the NV image prefix.
 * Bind takes read snapshots and requires both valid v101 images to agree.
 * Both invalid/split replicas require an external recovery decision, never rand().
 * All callbacks are serialized and non-reentrant; blocking syscalls have no hard
 * kernel preemption deadline. Deadlines are checked before/after each callback. */
int nv_ubi_bind(struct nv_ubi_backend *, const struct nv_ubi_ops *, int primary, int backup,
		const struct nv_ubi_identity expected[2], uint32_t payload_size,
		unsigned checksum_bits, int allow_write, uint64_t epoch, uint64_t deadline_ms,
		uint8_t *original, uint8_t *staged, size_t capacity);
int nv_ubi_begin(struct nv_ubi_backend *);
int nv_ubi_stage(struct nv_ubi_backend *, uint32_t offset, const uint8_t *, size_t);
/* group/sub identify the validated END frame. Caller must reject trailing data
 * before committing. The backend never sends intermediate/progress ACKs. */
int nv_ubi_commit(struct nv_ubi_backend *, uint32_t group, uint32_t sub);
/* Encode one final success BACK36 only after both volumes pass update+sync+full
 * readback and the lease/deadline remain valid. Frame identity is captured by
 * commit, not supplied by the caller here. Does not write the transport. */
int nv_ubi_prepare_final_ack(struct nv_ubi_backend *, uint8_t out[NV_RESPONSE_SIZE]);
/* written==36 means accepted by local transport write, NOT received by CP.
 * Anything else records reply lost while retaining storage_verified=true. */
int nv_ubi_note_ack_result(struct nv_ubi_backend *, size_t written);
void nv_ubi_abort(struct nv_ubi_backend *);
void nv_ubi_forget(struct nv_ubi_backend *); /* wipe RAM, never close borrowed fds */
#endif
