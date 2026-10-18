/* SPDX-License-Identifier: GPL-2.0-only
 * Standard Linux/glibc-or-musl adapter, not a libc or libubi reimplementation.
 * No paths, open(), close(), shell, raw MTD or automatic device discovery.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "nv_ubi_linux.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mtd/ubi-user.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int inspect(void *v, int fd, struct nv_ubi_identity *out)
{
	struct nv_ubi_linux_context *c = v;
	struct stat st;
	struct nv_ubi_identity meta = {0};
	int flags;
	if (!c || !c->volume_metadata || fd < 3 || !out)
		return -EINVAL;
	if (fstat(fd, &st))
		return -errno;
	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return -errno;
	if (!S_ISCHR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0002) ||
	    (flags & O_ACCMODE) != O_RDWR || (flags & (O_APPEND | O_DIRECT | O_PATH | O_NONBLOCK)))
		return -EINVAL;
	if (c->volume_metadata(c->supervisor, fd, &meta))
		return -ESTALE;
	if (meta.inode != (uint64_t)st.st_ino || meta.device != (uint64_t)st.st_dev ||
	    meta.rdev != (uint64_t)st.st_rdev || meta.owner_uid != (uint32_t)st.st_uid)
		return -ESTALE;
	*out = meta;
	return 0;
}
static long read_at(void *c, int fd, void *p, size_t n, uint64_t off)
{
	ssize_t r;
	(void)c;
	if (off > INT64_MAX)
		return -EOVERFLOW;
	r = pread(fd, p, n, (off_t)off);
	return r < 0 ? -errno : (long)r;
}
static int update(void *c, int fd, uint64_t total)
{
	int64_t length;
	(void)c;
	if (total > INT64_MAX)
		return -EOVERFLOW;
	length = (int64_t)total;
	return ioctl(fd, UBI_IOCVOLUP, &length) < 0 ? -errno : 0;
}
static long write_next(void *c, int fd, const void *p, size_t n)
{
	ssize_t r;
	(void)c;
	r = write(fd, p, n);
	return r < 0 ? -errno : (long)r;
}
static int sync_volume(void *c, int fd)
{
	(void)c;
	return fsync(fd) < 0 ? -errno : 0;
}
static int lease(void *v, uint64_t epoch)
{
	struct nv_ubi_linux_context *c = v;
	return c && c->lease_valid ? c->lease_valid(c->supervisor, epoch) : -EACCES;
}
static int now(void *c, uint64_t *out)
{
	struct timespec t;
	(void)c;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return -errno;
	if (t.tv_sec < 0)
		return -EIO;
	*out = (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
	return 0;
}
struct nv_ubi_ops nv_ubi_linux_ops(struct nv_ubi_linux_context *c)
{
	struct nv_ubi_ops o = {c,	   NV_UBI_LINUX, inspect, read_at, update,
			       write_next, sync_volume,	 lease,	  now};
	return o;
}
