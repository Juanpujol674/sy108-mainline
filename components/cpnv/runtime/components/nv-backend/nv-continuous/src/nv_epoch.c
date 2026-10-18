/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "nv_epoch.h"
#include "session_monitor.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/statfs.h>
#include <unistd.h>
/* NV service readiness is local to the NV transport. Other modem functions
 * have their own owners; their readiness or generation cannot deny NV I/O. */
int nvc_epoch_check(struct nvc_epoch *e)
{
	const unsigned i = 2;
	struct stat st;
	struct statfs fs;
	struct udx_snapshot x;
	char text[128];
	ssize_t n;
	int flags;
	if (!e || e->fd[i] < 3 || fstat(e->fd[i], &st) || !S_ISREG(st.st_mode))
		return -1;
	flags = fcntl(e->fd[i], F_GETFL);
	if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY)
		return -1;
	if (st.st_uid || (st.st_mode & 0222) || fstatfs(e->fd[i], &fs) || fs.f_type != 0x62656572L)
		return -1;
	n = pread(e->fd[i], text, sizeof text, 0);
	if (n <= 0 || n > 127 || udx_parse_snapshot(i, text, (size_t)n, &x) || x.state != 1 ||
	    x.error)
		return -1;
	/* Generation is diagnostic. A successful current READY read is not required
	 * to advance a private counter from a previous AP process. */
	e->generation[i] = x.generation;
	e->identity[i] = st;
	e->armed_mask = 4;
	e->pinned = 1;
	return 0;
}
int nvc_epoch_capture(struct nvc_epoch *e)
{
	return nvc_epoch_check(e);
}
int nvc_open_transport(struct nvc_epoch *e, int dir, uint64_t expected_rdev)
{
	int fd;
	struct stat st;
	struct pollfd p;
	if (!e || dir < 3 || !expected_rdev || fstat(dir, &st) || !S_ISDIR(st.st_mode) ||
	    st.st_uid || (st.st_mode & 0022))
		return -1;
	if (nvc_epoch_check(e))
		return -1;
	fd = openat(dir, "snv_lte", O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	p = (struct pollfd){fd, POLLERR | POLLHUP, 0};
	if (fstat(fd, &st) || !S_ISCHR(st.st_mode) || st.st_uid || (st.st_mode & 0002) ||
	    (uint64_t)st.st_rdev != expected_rdev || poll(&p, 1, 0) < 0 || p.revents) {
		close(fd);
		return -1;
	}
	return fd;
}
