/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_controller.h"
#include "cpnv_fd_plan.h"
#include "nvs_launch.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static int continuation_pair(int fd[2])
{
	return socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fd);
}
static uint64_t ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0)
		return 0;
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void put(unsigned char *p, uint64_t n, unsigned bytes)
{
	for (unsigned i = 0; i < bytes; i++)
		p[i] = (unsigned char)(n >> (8 * i));
}
static uint64_t get(const unsigned char *p, unsigned bytes)
{
	uint64_t n = 0;
	for (unsigned i = 0; i < bytes; i++)
		n |= (uint64_t)p[i] << (8 * i);
	return n;
}
static int guard(struct wr_roles *r, uint64_t end, uint64_t *last)
{
	uint64_t now = ms();
	if (nvs_cancelled || !now || now < *last || now >= end)
		return -ETIMEDOUT;
	*last = now;
	if (wr_process_live(&r->process[WR_CONTROLLER]))
		return -EIO;
	now = ms();
	if (!now || now < *last || now >= end)
		return -ETIMEDOUT;
	*last = now;
	return 0;
}
static int profile_file(int dir, const struct mc_config *c)
{
	unsigned char b[MC_PROFILE_HEADER + MC_MAX_IMAGES * MC_IMAGE_RECORD];
	size_t n;
	if (mc_warm_profile_encode(c, b, sizeof b, &n))
		return -1;
	char name[80];
	snprintf(name, sizeof name, "cp-profile-%016llx", (unsigned long long)c->epoch);
	int out = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (out < 3)
		return -1;
	int rc = write(out, b, n) == (ssize_t)n && !fsync(out);
	if (close(out))
		rc = 0;
	if (!rc) {
		unlinkat(dir, name, 0);
		return -1;
	}
	int profile = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	int saved = errno;
	unlinkat(dir, name, 0);
	errno = saved;
	return profile;
}
int wr_controller_spawn(struct wr_controller *c, struct wr_factory *f, struct wr_roles *r, int exe,
			int directory, uint64_t deadline)
{
	if (!c || !f || !r || exe < 3 || directory < 3 || r->nonce != f->config.epoch)
		return -EINVAL;
	memset(c, 0, sizeof *c);
	c->lease = c->phase = c->report = c->continuation = c->pidfd = -1;
	int lease[2] = {-1, -1}, phase[2] = {-1, -1}, report[2] = {-1, -1}, cont[2] = {-1, -1},
	    barrier[2] = {-1, -1}, ready[2] = {-1, -1}, profile = -1, error = 0;
	if (pipe2(lease, O_CLOEXEC | O_NONBLOCK) || pipe2(phase, O_CLOEXEC | O_NONBLOCK) ||
	    pipe2(barrier, O_CLOEXEC | O_NONBLOCK) || pipe2(ready, O_CLOEXEC | O_NONBLOCK)) {
		error = -errno;
		goto preparation_failed;
	}
	if (f->config.reload &&
	    (pipe2(report, O_CLOEXEC | O_NONBLOCK) || continuation_pair(cont))) {
		error = -errno;
		goto preparation_failed;
	}
	f->config.lease = lease[0];
	f->config.phase = phase[1];
	f->config.report = report[1];
	f->config.continuation = cont[0];
	profile = profile_file(directory, &f->config);
	if (profile < 3) {
		error = -EPROTO;
		goto preparation_failed;
	}
	if (f->config.image_count > MC_MAX_IMAGES) {
		error = -E2BIG;
		goto preparation_failed;
	}
	int keep[96] = {0,
			1,
			2,
			exe,
			profile,
			barrier[0],
			lease[0],
			phase[1],
			f->config.owner,
			f->config.cmdline,
			ready[1]};
	unsigned n = 11;
	if (f->config.postload_mode) {
		int post[] = {f->config.devdir,	      f->config.status_session,
			      f->config.halo_session, f->config.provider_rx,
			      f->config.provider_tx,  f->config.verification_params,
			      f->config.events};
		for (unsigned i = 0; i < 7; i++)
			keep[n++] = post[i];
	}
	if (f->config.reload) {
		keep[n++] = report[1];
		keep[n++] = cont[0];
	}
	for (unsigned d = 0; d < 3; d++) {
		keep[n++] = f->config.status[d];
		if (f->config.reload) {
			keep[n++] = f->config.start[d];
			keep[n++] = f->config.stop[d];
		}
	}
	for (unsigned i = 0; i < f->config.image_count; i++) {
		if (f->config.images[i].source >= 3)
			keep[n++] = f->config.images[i].source;
		keep[n++] = f->config.images[i].target;
		if (f->config.images[i].backup >= 3)
			keep[n++] = f->config.images[i].backup;
	}
	struct cpb_fd_plan plan;
	error = cpb_fd_plan_prepare(&plan, keep, n);
	if (error)
		goto preparation_failed;
	pid_t parent = getpid(), pid = fork();
	if (pid < 0) {
		error = -errno;
		goto preparation_failed;
	}
	if (!pid) {
		if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent ||
		    cpb_fd_plan_apply_child(&plan) || write(ready[1], "P", 1) != 1)
			_exit(71);
		close(ready[1]);
		char token;
		struct pollfd b = {barrier[0], POLLIN, 0};
		uint64_t now = ms();
		if (!now || now >= deadline || poll(&b, 1, (int)(deadline - now)) != 1 ||
		    !(b.revents & POLLIN) || read(barrier[0], &token, 1) != 1 || token != 'G' ||
		    getppid() != parent)
			_exit(71);
		close(barrier[0]);
		for (unsigned i = 0; i < n; i++)
			if (keep[i] != barrier[0] && keep[i] != ready[1] &&
			    fcntl(keep[i], F_SETFD, keep[i] == exe ? FD_CLOEXEC : 0))
				_exit(71);
		char arg[24];
		snprintf(arg, sizeof arg, "%d", profile);
		char *av[] = {"udx-modem-control-warm", "--profile-fd", arg, NULL};
		char *env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL};
		fexecve(exe, av, env);
		_exit(127);
	}
	close(ready[1]);
	close(profile);
	close(lease[0]);
	close(phase[1]);
	close(barrier[0]);
	if (report[1] >= 0)
		close(report[1]);
	if (cont[0] >= 0)
		close(cont[0]);
	c->lease = lease[1];
	c->phase = phase[0];
	c->report = report[0];
	c->continuation = cont[1];
	c->pid = pid;
	int pf = (int)syscall(SYS_pidfd_open, pid, 0);
	c->pidfd = pf;
	if (pf < 3) {
		int saved = pf < 0 ? -errno : -EBADF;
		close(barrier[1]);
		close(ready[0]);
		return saved;
	}
	int pruned = cpb_fd_plan_wait_ready(&plan, ready[0], pf, deadline);
	close(ready[0]);
	if (pruned) {
		close(barrier[1]);
		return pruned;
	}
	/* Inherited descriptor pruning is complete before the child continues. */
	wr_factory_drop_child_caps(f);
	if (nvs_cancelled || !ms() || ms() >= deadline || write(barrier[1], "G", 1) != 1) {
		close(barrier[1]);
		return -EIO;
	}
	close(barrier[1]);
	uint64_t last = ms();
	int registered = 0;
	while (last && !nvs_cancelled) {
		uint64_t now = ms();
		if (!now || now < last || now >= deadline)
			break;
		last = now;
		if (!wr_roles_register(r, WR_CONTROLLER, pf, exe, pid, parent)) {
			registered = 1;
			break;
		}
		struct pollfd p = {pf, POLLIN, 0};
		if (poll(&p, 1, 5) > 0)
			break;
	}
	if (!registered)
		return -EPERM;
	unsigned char grant[32] = {0};
	memcpy(grant, "MCOWN001", 8);
	put(grant + 8, r->nonce, 8);
	put(grant + 16, (unsigned)parent, 4);
	put(grant + 20, f->config.reload, 4);
	if (guard(r, deadline, &last) || write(c->lease, grant, sizeof grant) != sizeof grant ||
	    guard(r, deadline, &last))
		return -EIO;
	return 0;
preparation_failed:
	{
		int owned[] = {profile,	   lease[0],  lease[1], phase[0], phase[1],
			       report[0],  report[1], cont[0],	cont[1],  barrier[0],
			       barrier[1], ready[0],  ready[1]};
		for (unsigned i = 0; i < sizeof owned / sizeof owned[0]; ++i)
			if (owned[i] >= 0)
				close(owned[i]);
	}
	return error;
}
int wr_controller_report(struct wr_controller *c, struct wr_roles *r, const char magic[8],
			 uint64_t deadline)
{
	unsigned char b[32];
	size_t used = 0;
	uint64_t last = ms();
	if (!last)
		return -EIO;
	while (used < sizeof b) {
		if (guard(r, deadline, &last))
			return -EIO;
		struct pollfd p = {c->report, POLLIN, 0};
		int rc = poll(&p, 1, 25);
		if ((rc < 0 && errno != EINTR) || p.revents & (POLLERR | POLLHUP | POLLNVAL))
			return -EIO;
		if (p.revents & POLLIN) {
			ssize_t n = read(c->report, b + used, sizeof b - used);
			if (n <= 0)
				return -EIO;
			used += (size_t)n;
		}
	}
	if (memcmp(b, magic, 8) || get(b + 8, 8) != r->nonce ||
	    get(b + 16, 4) != (unsigned)c->pid || get(b + 20, 4) != 7 || get(b + 24, 4) != 2u ||
	    get(b + 28, 4) || guard(r, deadline, &last))
		return -EPROTO;
	return 0;
}
int wr_controller_continue(struct wr_controller *c, struct wr_factory *f, struct wr_roles *r,
			   uint64_t deadline)
{
	unsigned char b[32 + 64 * MC_MAX_IMAGES];
	size_t n;
	if (mc_warm_continue_encode(&f->config, b, sizeof b, &n))
		return -EPROTO;
	uint64_t last = ms();
	if (!last || guard(r, deadline, &last))
		return -EIO; /* <=928 bytes, one pipe write <= PIPE_BUF; keep writer alive through
				APLOAD04. */
	int inputs[4];
	unsigned count = 0;
	for (unsigned i = 0; i < f->config.image_count; i++)
		if (f->config.images[i].segment >= 7) {
			if (count + 2 > 4)
				return -EINVAL;
			inputs[count++] = f->config.images[i].source;
			inputs[count++] = f->config.images[i].backup;
		}
	if (count != 4)
		return -EINVAL;
	char control[CMSG_SPACE(sizeof inputs)] = {0};
	struct iovec io = {b, n};
	struct msghdr msg = {.msg_iov = &io,
			     .msg_iovlen = 1,
			     .msg_control = control,
			     .msg_controllen = sizeof control};
	struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
	h->cmsg_level = SOL_SOCKET;
	h->cmsg_type = SCM_RIGHTS;
	h->cmsg_len = CMSG_LEN(sizeof inputs);
	memcpy(CMSG_DATA(h), inputs, sizeof inputs);
	if (sendmsg(c->continuation, &msg, MSG_NOSIGNAL) != (ssize_t)n)
		return -EIO;
	if (guard(r, deadline, &last))
		return -EIO;
	return 0;
}
int wr_controller_stop(struct wr_controller *c, struct wr_roles *r, uint64_t deadline)
{
	if (!c || !r)
		return -EINVAL;
	if (c->lease >= 0) {
		close(c->lease);
		c->lease = -1;
	}
	struct cpb_reap_fact fact;
	return wr_wait_reaped(&r->process[WR_CONTROLLER], deadline, &fact);
}
void wr_controller_close(struct wr_controller *c)
{
	if (!c)
		return;
	c->pid = 0;
	int *f[] = {&c->lease, &c->phase, &c->report, &c->continuation, &c->pidfd};
	for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
		if (*f[i] >= 0)
			close(*f[i]);
		*f[i] = -1;
	}
}
