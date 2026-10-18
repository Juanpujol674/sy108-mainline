/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "cpnv_linux.h"
#include "refnotify_transport.h"
#include "cpnv_fd_plan.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
void rt_child_init(struct rt_child *c)
{
	memset(c, 0, sizeof *c);
	c->pidfd = c->lease = c->report = c->executable = c->session = -1;
}
static int dead(const struct rt_child *c)
{
	struct pollfd p = {c->pidfd, POLLIN, 0};
	int n = poll(&p, 1, 0);
	return n == 1 && (p.revents & POLLIN) && !(p.revents & (POLLERR | POLLNVAL));
}
static int identity(const struct rt_child *c)
{
	char path[64];
	struct stat expected, actual;
	snprintf(path, sizeof path, "/proc/%ld/exe", (long)c->pid);
	if (fstat(c->executable, &expected) || stat(path, &actual))
		return -errno;
	return expected.st_dev == actual.st_dev && expected.st_ino == actual.st_ino ? 0 : -ESTALE;
}
int rt_child_check(const struct rt_child *c)
{
	if (!c || c->pid < 2 || c->pidfd < 0 || !c->granted)
		return -EINVAL;
	struct pollfd p = {c->pidfd, POLLIN, 0};
	if (poll(&p, 1, 0) < 0)
		return -errno;
	if (p.revents & (POLLERR | POLLNVAL))
		return -EBADF;
	return p.revents ? -ECHILD : 0;
}
static void put64(uint8_t *p, uint64_t n)
{
	for (unsigned i = 0; i < 8; ++i)
		p[i] = (uint8_t)(n >> (8u * i));
}
static int duplicate(int fd)
{
	if (fd < 0)
		return -1;
	return fcntl(fd, F_DUPFD_CLOEXEC, 20);
}
static void exec_child(int executable, struct rt_sources *s, int lease, int report, uint64_t epoch,
		       pid_t parent, const struct cpb_fd_plan *plan)
{
	int src[7] = {s->notify, s->session, lease, report, s->rtc, s->listener, executable},
	    copy[7];
	if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent ||
	    cpb_fd_plan_apply_child(plan))
		_exit(125);
	for (unsigned i = 0; i < 7; ++i) {
		copy[i] = duplicate(src[i]);
		if (src[i] >= 0 && copy[i] < 0)
			_exit(125);
	}
	for (unsigned i = 0; i < 7; ++i) {
		int target = (int)i + 3;
		if (copy[i] < 0)
			close(target);
		else if (dup2(copy[i], target) < 0)
			_exit(125);
	}
	if (fcntl(9, F_SETFD, FD_CLOEXEC) || syscall(SYS_close_range, 10u, ~0u, 0u))
		_exit(125);
	char e[32], g[32], p[32], uid[32];
	snprintf(e, sizeof e, "%llu", (unsigned long long)epoch);
	snprintf(g, sizeof g, "%u", s->generation);
	snprintf(p, sizeof p, "%ld", (long)parent);
	snprintf(uid, sizeof uid, "%lu", (unsigned long)geteuid());
	char *argv[] = {"udx-refnotifyd", "--inherited", e, g, p, uid, NULL};
	char *env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL};
	fexecve(9, argv, env);
	_exit(126);
}
void rt_child_withdraw(struct rt_child *c)
{
	if (!c)
		return;
	if (c->lease >= 0) {
		close(c->lease);
		c->lease = -1;
	}
	c->granted = false;
}
static int failed(struct rt_child *c, int error)
{
	rt_child_withdraw(c);
	return error;
}
int rt_child_prepare(struct rt_child *c, int executable, struct rt_sources *s, uint64_t epoch,
		     uint64_t deadline)
{
	int lease[2] = {-1, -1}, report[2] = {-1, -1};
	struct stat st;
	uint8_t b[RT_HANDSHAKE_SIZE], expected[RT_HANDSHAKE_SIZE] = {0};
	uint64_t last = rt_now_ms();
	if (!c || !s || c->pid || executable < 3 || s->notify < 3 || s->session < 3 || !epoch ||
	    !last || deadline <= last || deadline - last > 60000)
		return -EINVAL;
	if (fstat(executable, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 0022) ||
	    !(st.st_mode & 0111))
		return -EPERM;
	c->executable = fcntl(executable, F_DUPFD_CLOEXEC, 3);
	if (c->executable < 0)
		return -errno;
	c->session = fcntl(s->session, F_DUPFD_CLOEXEC, 3);
	if (c->session < 0) {
		int e = -errno;
		close(c->executable);
		if (c->session >= 0)
			close(c->session);
		rt_child_init(c);
		return e;
	}
	c->ready_deadline = deadline;
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, lease) ||
	    socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, report)) {
		int e = -errno;
		for (unsigned i = 0; i < 2; ++i) {
			if (lease[i] >= 0)
				close(lease[i]);
			if (report[i] >= 0)
				close(report[i]);
		}
		close(c->executable);
		if (c->session >= 0)
			close(c->session);
		rt_child_init(c);
		return e;
	}
	c->epoch = epoch;
	c->generation = s->generation;
	struct cpb_fd_plan plan;
	int keep[] = {0,	  1,	    2,	       c->executable, s->notify,
		      s->session, lease[1], report[1], s->rtc,	      s->listener};
	int planning = cpb_fd_plan_prepare(&plan, keep, sizeof keep / sizeof keep[0]);
	if (planning) {
		for (unsigned i = 0; i < 2; ++i) {
			close(lease[i]);
			close(report[i]);
		}
		close(c->executable);
		close(c->session);
		rt_child_init(c);
		return planning;
	}
	pid_t parent = getpid(), pid = fork();
	if (!pid)
		exec_child(c->executable, s, lease[1], report[1], epoch, parent, &plan);
	if (pid < 0) {
		int e = -errno;
		for (unsigned i = 0; i < 2; ++i) {
			close(lease[i]);
			close(report[i]);
		}
		close(c->executable);
		if (c->session >= 0)
			close(c->session);
		rt_child_init(c);
		return e;
	}
	c->pid = pid;
	c->lease = lease[0];
	c->report = report[0];
	close(lease[1]);
	close(report[1]);
	/* The unique parent retains no data/RTC/listener aliases. One read-only
	 * session snapshot FD remains bound for final grant and runtime checks. */
	rt_sources_close(s);
	c->pidfd = (int)syscall(SYS_pidfd_open, pid, 0u);
	if (c->pidfd < 0)
		return failed(c, -errno);
	memcpy(expected, "RNRDY001", 8);
	put64(expected + 8, epoch);
	rn_put_le32(expected + 16, c->generation);
	rn_put_le32(expected + 20, RT_BUFID);
	for (;;) {
		uint64_t now = rt_now_ms();
		if (!now || now < last || now >= deadline)
			return failed(c, -ETIMEDOUT);
		last = now;
		struct pollfd p[2] = {{c->pidfd, POLLIN, 0}, {c->report, POLLIN, 0}};
		int ret = poll(p, 2, 25);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return failed(c, -errno);
		}
		if (p[0].revents)
			return failed(c, -ECHILD);
		if (p[1].revents & (POLLHUP | POLLERR | POLLNVAL))
			return failed(c, -EPROTO);
		if (p[1].revents & POLLIN) {
			ssize_t n = recv(c->report, b, sizeof b, MSG_TRUNC | MSG_DONTWAIT);
			if (n != (ssize_t)sizeof b || memcmp(b, expected, sizeof b))
				return failed(c, -EPROTO);
			int e = identity(c);
			if (e)
				return failed(c, e);
			struct rt_snapshot snapshot;
			if ((e = rt_read_session(c->session, &snapshot)) ||
			    snapshot.generation != c->generation)
				return failed(c, e ? e : -ESTALE);
			now = rt_now_ms();
			if (!now || now < last || now >= deadline)
				return failed(c, -ETIMEDOUT);
			c->ready = true;
			return 0;
		}
	}
}
static int grant_check(struct rt_child *c, uint64_t deadline, uint64_t *last,
		       int (*authorize)(void *), void *context)
{
	uint64_t now = rt_now_ms();
	if (!now || now < *last || now >= deadline || now >= c->ready_deadline)
		return -ETIMEDOUT;
	*last = now;
	if (dead(c))
		return -ECHILD;
	int e = identity(c);
	if (e)
		return e;
	struct rt_snapshot snapshot;
	if ((e = rt_read_session(c->session, &snapshot)) || snapshot.generation != c->generation)
		return e ? e : -ESTALE;
	if ((e = authorize(context)))
		return e;
	now = rt_now_ms();
	if (!now || now < *last || now >= deadline || now >= c->ready_deadline)
		return -ETIMEDOUT;
	*last = now;
	return 0;
}
int rt_child_grant(struct rt_child *c, uint64_t deadline, int (*authorize)(void *), void *context)
{
	if (!c || !c->ready || c->granted || c->lease < 3 || c->session < 3 || !authorize)
		return -EINVAL;
	uint64_t last = rt_now_ms();
	if (!last)
		return failed(c, -EIO);
	int e = grant_check(c, deadline, &last, authorize, context);
	if (e)
		return failed(c, e);
	uint8_t b[RT_HANDSHAKE_SIZE] = {0};
	memcpy(b, "RNGRNT01", 8);
	put64(b + 8, c->epoch);
	rn_put_le32(b + 16, c->generation);
	rn_put_le32(b + 20, RT_BUFID);
	ssize_t n = send(c->lease, b, sizeof b, MSG_NOSIGNAL);
	if (n != (ssize_t)sizeof b)
		return failed(c, n < 0 ? -errno : -EIO);
	c->grant_sent = c->granted =
	    true; /* A successfully sent grant is a fact even if postcheck revokes it. */
	e = grant_check(c, deadline, &last, authorize, context);
	if (e)
		return failed(c, e);
	return 0;
}
int rt_child_revoke(struct rt_child *c, uint64_t deadline)
{
	if (!c)
		return -EINVAL;
	rt_child_withdraw(c);
	if (c->pid < 2 || c->exited)
		return 0;
	/* Only a retained pidfd is eligible for signaling; a numeric child PID
	 * alone is not reusable cancellation authority. */
	if (c->pidfd >= 0) {
		if (syscall(SYS_pidfd_send_signal, c->pidfd, SIGTERM, NULL, 0u) && errno != ESRCH)
			return -errno;
	} else
		return -ENODATA; /* No numeric-PID fallback after identity loss. */
	uint64_t last = rt_now_ms();
	if (!last)
		return -EIO;
	for (;;) {
		struct cpb_reap_fact fact;
		int e = cpb_pidfd_reap_child(c->pidfd, c->pid, &fact);
		if (!e) {
			c->exited = true;
			return 0;
		}
		if (e != -EAGAIN && e != -EINTR)
			return e;
		uint64_t now = rt_now_ms();
		if (!now || now < last || now >= deadline)
			return -EBUSY;
		last = now;
		unsigned pause = deadline - now > 10 ? 10u : (unsigned)(deadline - now);
		if (poll(NULL, 0, (int)pause) < 0 && errno != EINTR)
			return -errno;
	}
}
void rt_child_close_exited(struct rt_child *c)
{
	if (!c)
		return;
	/* Fail closed if caller skipped the required exit proof. */
	if (c->pid >= 2 && !c->exited)
		return;
	int f[5] = {c->pidfd, c->lease, c->report, c->executable, c->session};
	for (unsigned i = 0; i < 5; ++i)
		if (f[i] >= 0)
			close(f[i]);
	rt_child_init(c);
}
