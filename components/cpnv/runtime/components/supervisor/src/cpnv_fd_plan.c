/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "cpnv_fd_plan.h"
#include "cpnv_linux.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#define VALID 0x43504644u
static int match(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
	       a->st_rdev == b->st_rdev && a->st_uid == b->st_uid;
}
int cpb_fd_plan_prepare(struct cpb_fd_plan *p, const int *fds, unsigned count)
{
	if (!p)
		return -EINVAL;
	memset(p, 0, sizeof *p);
	if (!fds || !count || count > CPB_FD_MAX)
		return -EINVAL;
	p->creator = getpid();
	for (unsigned i = 0; i < count; ++i) {
		int fd = fds[i];
		if (fd == -1)
			continue;
		if (fd < 0)
			return -EBADF;
		unsigned at = 0;
		while (at < p->count && p->keep[at].fd < fd)
			++at;
		if (at < p->count && p->keep[at].fd == fd)
			continue;
		struct cpb_fd_entry e = {.fd = fd};
		e.access = fcntl(fd, F_GETFL);
		if (e.access < 0)
			return -errno;
		e.access &= O_ACCMODE | O_PATH;
		if (fstat(fd, &e.identity))
			return -errno;
		for (unsigned j = p->count; j > at; --j)
			p->keep[j] = p->keep[j - 1];
		p->keep[at] = e;
		++p->count;
	}
	if (!p->count)
		return -EINVAL;
	p->valid = VALID;
	return 0;
}
int cpb_fd_plan_wait_ready(const struct cpb_fd_plan *p, int ack, int child, uint64_t end)
{
	if (!p || p->valid != VALID || getpid() != p->creator || ack < 0 || child < 0 || !end)
		return -EINVAL;
	struct stat st;
	if (fstat(ack, &st))
		return -errno;
	if (!S_ISFIFO(st.st_mode))
		return -EINVAL;
	int flags = fcntl(child, F_GETFL);
	if (flags < 0)
		return -errno;
	if (flags & (O_EXCL | O_TRUNC | O_PATH))
		return -EPERM;
	if ((flags & O_ACCMODE) != O_RDWR || (flags & ~(O_RDWR | O_NONBLOCK | O_LARGEFILE)))
		return -EPERM;
	if (syscall(SYS_pidfd_send_signal, child, 0, NULL, 0u))
		return -errno;
	uint64_t last = 0, now;
	int e;
	for (;;) {
		if ((e = cpb_monotonic_ms(&now)))
			return e;
		if (now < last)
			return -EIO;
		if (now >= end)
			return -ETIMEDOUT;
		last = now;
		uint64_t remaining = end - now;
		struct pollfd fd[2] = {{child, POLLIN, 0}, {ack, POLLIN, 0}};
		int n = poll(fd, 2, remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (fd[0].revents & (POLLERR | POLLNVAL))
			return -EBADF;
		if (fd[0].revents)
			return -ECHILD;
		if (fd[1].revents & (POLLERR | POLLNVAL))
			return -EBADF;
		if (fd[1].revents & POLLIN) {
			char ready;
			ssize_t bytes = read(ack, &ready, 1);
			if (bytes < 0)
				return -errno;
			if (bytes != 1 || ready != 'P')
				return -EPROTO;
			if ((e = cpb_monotonic_ms(&now)))
				return e;
			return now < last ? -EIO : now >= end ? -ETIMEDOUT : 0;
		}
		if (fd[1].revents & POLLHUP)
			return -EPIPE;
	}
}
int cpb_fd_plan_apply_child(const struct cpb_fd_plan *p)
{
	if (!p || p->valid != VALID || !p->count || p->count > CPB_FD_MAX)
		return -EINVAL;
	if (getpid() == p->creator)
		return -EPERM;
	if (p->creator < 1 || getppid() != p->creator)
		return -ESTALE;
	for (unsigned i = 0; i < p->count; ++i) {
		const struct cpb_fd_entry *e = &p->keep[i];
		struct stat st;
		if (e->fd < 0 || (i && e->fd <= p->keep[i - 1].fd))
			return -EINVAL;
		int flags = fcntl(e->fd, F_GETFL);
		if (flags < 0)
			return -errno;
		if ((flags & (O_ACCMODE | O_PATH)) != e->access)
			return -ESTALE;
		if (fstat(e->fd, &st))
			return -errno;
		if (!match(&st, &e->identity))
			return -ESTALE;
	}
	unsigned first = 0;
	for (unsigned i = 0; i < p->count; ++i) {
		unsigned fd = (unsigned)p->keep[i].fd;
		if (first < fd && syscall(SYS_close_range, first, fd - 1u, 0u))
			return -errno;
		first = fd + 1u;
	}
	if (syscall(SYS_close_range, first, UINT_MAX, 0u))
		return -errno;
	return getppid() == p->creator ? 0 : -ESTALE;
}
