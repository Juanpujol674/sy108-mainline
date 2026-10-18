/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_snapshot.h"
#include "nv_codec.h"
#include "nv_vendor.h"
#include "sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int clock_ok(uint64_t until, uint64_t *last)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0)
		return -EIO;
	uint64_t n = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
	if (!n || n < *last || n >= until)
		return -ETIMEDOUT;
	*last = n;
	return 0;
}
static int exact(int fd, void *buf, size_t length, int writing, uint64_t until, uint64_t *last)
{
	size_t off = 0;
	while (off < length) {
		int e = clock_ok(until, last);
		if (e)
			return e;
		size_t n = length - off;
		if (n > 65536)
			n = 65536;
		ssize_t got = writing ? pwrite(fd, (char *)buf + off, n, (off_t)off)
				      : pread(fd, (char *)buf + off, n, (off_t)off);
		int saved = errno;
		e = clock_ok(until, last);
		if (e)
			return e;
		if (got <= 0 || (size_t)got > n)
			return got < 0 ? -saved : -EIO;
		off += (size_t)got;
	}
	return 0;
}
static void wipe(void *p, size_t n)
{
	volatile unsigned char *v = p;
	while (n--)
		*v++ = 0;
}
int wr_snapshot_nv_pair(const int in[2], const int out[2], uint32_t size, uint64_t until,
			struct wr_snapshot_result *result)
{
	if (!in || !out || !result || !size || size > NV_MAX_IMAGE || size % 512 || !until)
		return -EINVAL;
	memset(result, 0, sizeof *result);
	struct stat id[4];
	int fds[] = {in[0], in[1], out[0], out[1]};
	uint64_t last = 0;
	int e = clock_ok(until, &last);
	if (e)
		return e;
	for (unsigned i = 0; i < 4; i++) {
		int flags = fcntl(fds[i], F_GETFL);
		if (fds[i] < 3 || flags < 0 || fstat(fds[i], id + i) || id[i].st_uid != geteuid() ||
		    (flags & O_ACCMODE) != (i < 2 ? O_RDONLY : O_RDWR) ||
		    (flags & (O_APPEND | O_DIRECT | O_PATH | O_NONBLOCK)))
			return -EPERM;
		if (i < 2) {
			if (!S_ISCHR(id[i].st_mode) && !S_ISREG(id[i].st_mode))
				return -EPERM;
			if (S_ISREG(id[i].st_mode) &&
			    (id[i].st_size != (off_t)size + NV_HEADER || id[i].st_nlink != 1 ||
			     (id[i].st_mode & 0077)))
				return -EPERM;
		} else if (!S_ISREG(id[i].st_mode) || id[i].st_nlink != 1 ||
			   (id[i].st_mode & 0077) || id[i].st_size)
			return -EPERM;
		for (unsigned j = 0; j < i; j++)
			if ((id[i].st_dev == id[j].st_dev && id[i].st_ino == id[j].st_ino) ||
			    (S_ISCHR(id[i].st_mode) && S_ISCHR(id[j].st_mode) &&
			     id[i].st_rdev == id[j].st_rdev))
				return -EXDEV;
	}
	size_t total = (size_t)size + NV_HEADER;
	unsigned char *a = malloc(total), *b = malloc(total);
	if (!a || !b) {
		free(a);
		free(b);
		return -ENOMEM;
	}
	unsigned bits = 0;
	e = exact(in[0], a, total, 0, until, &last);
	if (!e)
		e = exact(in[1], b, total, 0, until, &last);
	/* Boot loader uses primary when both independently validate, even if unequal.
	 * Single-invalid repair has already completed while CP was stopped. */
	if (!e &&
	    (!nv_vendor_valid(a, total, size, &bits) || !nv_vendor_valid(b, total, size, NULL)))
		e = -EBADMSG;
	if (!e) {
		struct udx_sha256 h;
		udx_sha256_init(&h);
		if (udx_sha256_update(&h, a + NV_HEADER, size) ||
		    udx_sha256_final(&h, result->payload_sha256))
			e = -EIO;
	}
	for (unsigned i = 0; !e && i < 2; i++) {
		e = exact(out[i], a, total, 1, until, &last);
		if (!e && ftruncate(out[i], (off_t)total))
			e = -errno;
		if (!e)
			e = clock_ok(until, &last);
		if (!e && fsync(out[i]))
			e = -errno;
		if (!e)
			e = clock_ok(until, &last);
		if (!e)
			e = exact(out[i], b, total, 0, until, &last);
		if (!e && memcmp(a, b, total))
			e = -EIO;
	}
	for (unsigned i = 0; !e && i < 4; i++) {
		struct stat now;
		if (fstat(fds[i], &now) || id[i].st_dev != now.st_dev ||
		    id[i].st_ino != now.st_ino || id[i].st_rdev != now.st_rdev ||
		    id[i].st_uid != now.st_uid || id[i].st_mode != now.st_mode)
			e = -ESTALE;
	}
	if (!e)
		e = clock_ok(until, &last);
	wipe(a, total);
	wipe(b, total);
	free(a);
	free(b);
	if (e) {
		memset(result, 0, sizeof *result);
		return e;
	}
	result->payload = size;
	result->checksum_bits = bits;
	return 0;
}
