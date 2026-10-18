/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NV_BACKEND_H
#define NV_BACKEND_H
#include <stddef.h>
#include <stdint.h>
/* Borrowed, already-open descriptors only: this API never opens a pathname. */
struct nv_file_info {
	uint64_t inode, size;
	uint32_t dev_major, dev_minor, uid, nlink;
	uint16_t mode;
	int access;
};
struct nv_io {
	void *ctx;
	int (*info)(void *, int, struct nv_file_info *);
	int (*lock)(void *, int); /* exclusive nonblocking advisory lock */
	int (*unlock)(void *, int);
	long (*pread)(void *, int, void *, size_t, uint64_t);
	long (*pwrite)(void *, int, const void *, size_t, uint64_t);
	int (*sync)(void *, int);
	uint32_t owner_uid;
};
#endif
