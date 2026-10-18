/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_nv.h"
#include "cpnv_fd_plan.h"
#include "wr_quiesce.h"
#include "nvs_report.h"
#include "session_monitor.h"
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
static uint64_t ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0)
		return 0;
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static uint64_t value(const unsigned char *p, unsigned n)
{
	uint64_t v = 0;
	for (unsigned i = 0; i < n; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}
static int wait_input(int fd, uint64_t end)
{
	uint64_t last = ms();
	if (!last)
		return -EIO;
	for (;;) {
		uint64_t now = ms();
		if (!now || now < last || now >= end || nvs_cancelled)
			return -ETIMEDOUT;
		last = now;
		struct pollfd p = {fd, POLLIN, 0};
		int r = poll(&p, 1, (int)(end - now > 25 ? 25 : end - now));
		if (r < 0 && errno != EINTR)
			return -errno;
		if (p.revents & (POLLERR | POLLNVAL))
			return -EIO;
		if (p.revents & POLLIN)
			return 0;
		if (p.revents & POLLHUP)
			return -EPIPE;
	}
}
static int wait_phase(struct nvs_launch *c, uint64_t deadline)
{
	unsigned char b[32];
	if (wait_input(c->cp_phase_fd, deadline) || read(c->cp_phase_fd, b, sizeof b) != sizeof b ||
	    memcmp(b, "NVLOAD03", 8) || value(b + 8, 8) != c->nonce ||
	    value(b + 16, 4) != (unsigned)c->cp_parent_pid || value(b + 20, 4) != 1 ||
	    value(b + 24, 8) ||
	    nvs_pidfd_matches(c->cp_owner_pidfd, c->cp_owner_exe_fd, c->cp_parent_pid))
		return -EPROTO;
	return ms() && ms() < deadline ? 0 : -ETIMEDOUT;
}
static int source_plan(struct wr_nv_plan *p, struct cpb_fd_plan *plan)
{
	struct nvs_launch *c = &p->launch;
	int keep[32] = {0,
			1,
			2,
			c->parent_report,
			c->parent_lease,
			c->quiesce,
			c->owner,
			c->boot,
			c->consumer,
			c->broker,
			c->cp_phase_fd,
			c->cp_owner_pidfd,
			c->cp_owner_exe_fd,
			c->old_pidfd};
	unsigned n = 14;
	keep[n++] = c->epoch.fd[2];
	return cpb_fd_plan_prepare(plan, keep, n);
}
static int source(struct wr_nv_plan *p, uint64_t deadline, const struct cpb_fd_plan *plan)
{
	struct nvs_launch *c = &p->launch;
	if (cpb_fd_plan_apply_child(plan) || wait_phase(c, deadline))
		return 77;
	uint64_t last = ms();
	if (!last)
		return 77;
	for (;;) {
		char b[128];
		struct udx_snapshot snap;
		ssize_t got = pread(c->epoch.fd[2], b, sizeof b, 0);
		uint64_t now = ms();
		struct pollfd lease = {c->parent_lease, POLLIN, 0};
		if (nvs_cancelled || !now || now < last || now >= deadline ||
		    poll(&lease, 1, 0) < 0 || lease.revents || getppid() != c->unified_parent ||
		    got <= 0 || got >= (ssize_t)sizeof b ||
		    udx_parse_snapshot(2, b, (size_t)got, &snap) || snap.state == 2)
			return 77;
		last = now;
		if (snap.state == 1)
			break;
		poll(NULL, 0, 25);
	}
	if ((c->epoch.pinned ? nvc_epoch_check(&c->epoch) : nvc_epoch_capture(&c->epoch)) ||
	    nvs_open_devices(&c->profile, &c->devices))
		return 77;
	{
		c->transport =
		    nvc_open_transport(&c->epoch, c->devices.dev_dir, c->devices.transport_rdev);
		if (c->transport < 3)
			return 77;
	}
	if (nvc_epoch_check(&c->epoch) || nvs_cancelled || !ms() || ms() >= deadline)
		return 77;
	return nvs_run_broker(c);
}
void wr_nv_close(struct wr_nv *n)
{
	if (!n)
		return;
	n->source = 0;
	if (n->source_pidfd >= 3)
		close(n->source_pidfd);
	n->source_pidfd = -1;
	if (n->report >= 0)
		close(n->report);
	if (n->lease >= 0)
		close(n->lease);
	if (n->quiesce >= 0)
		close(n->quiesce);
	n->report = n->lease = n->quiesce = -1;
	n->live = 0;
}
static int grant_guard(struct wr_nv_plan *p, struct wr_roles *r, uint64_t end, uint64_t *last)
{
	uint64_t now = ms();
	int e;
	if (nvs_cancelled)
		return -ECANCELED;
	if (!now || now < *last)
		return -EIO;
	if (now >= end)
		return -ETIMEDOUT;
	*last = now;
	for (unsigned i = 0; i < WR_ROLE_COUNT; i++)
		if (r->process[i].registered && (e = wr_process_live(&r->process[i])))
			return e;
	if (nvc_epoch_check(&p->launch.epoch))
		return -ESTALE;
	if (p->admission && (e = p->admission(p->admission_context, end)))
		return e;
	now = ms();
	if (nvs_cancelled)
		return -ECANCELED;
	if (!now || now < *last)
		return -EIO;
	if (now >= end)
		return -ETIMEDOUT;
	*last = now;
	return 0;
}
static int start_failed(struct wr_nv *n, int e)
{
	if (n->lease >= 3) {
		close(n->lease);
		n->lease = -1;
	}
	return e;
}
int wr_nv_start(struct wr_nv *n, struct wr_roles *r, struct wr_nv_plan *p, uint64_t deadline)
{
	if (!n || !r || !p || r->nonce != p->launch.nonce || p->self_exe < 3)
		return -EINVAL;
	uint64_t last = ms();
	if (!last)
		return -EIO;
	int e = grant_guard(p, r, deadline, &last);
	if (e)
		return e;
	memset(n, 0, sizeof *n);
	n->report = n->lease = n->quiesce = n->source_pidfd = -1;
	int report[2], lease[2], q[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, report))
		return -errno;
	if (pipe2(lease, O_CLOEXEC | O_NONBLOCK)) {
		close(report[0]);
		close(report[1]);
		return -errno;
	}
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, q)) {
		close(report[0]);
		close(report[1]);
		close(lease[0]);
		close(lease[1]);
		return -errno;
	}
	pid_t parent = getpid();
	p->launch.parent_report = report[1];
	p->launch.parent_lease = lease[0];
	p->launch.quiesce = q[1];
	p->launch.unified_parent = parent;
	struct cpb_fd_plan plan;
	int planning = source_plan(p, &plan);
	if (planning) {
		for (unsigned i = 0; i < 2; i++) {
			close(report[i]);
			close(lease[i]);
			close(q[i]);
		}
		return planning;
	}
	pid_t pid = fork();
	if (pid < 0) {
		for (unsigned i = 0; i < 2; i++) {
			close(report[i]);
			close(lease[i]);
			close(q[i]);
		}
		return -errno;
	}
	if (!pid) {
		if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent)
			_exit(71);
		nvs_cancelled = 0;
		signal(SIGTERM, nvs_cancel_handler);
		signal(SIGINT, nvs_cancel_handler);
		int source_result = source(p, deadline, &plan);
		fprintf(source_result ? stderr : stdout, "warm_source=exit result=%d\n",
			source_result);
		fflush(source_result ? stderr : stdout);
		_exit(source_result);
	}
	r->unresolved_descendants =
	    1; /* Fork may create descendants before their role receipts arrive. */
	close(report[1]);
	close(lease[0]);
	close(q[1]);
	n->report = report[0];
	n->lease = lease[1];
	n->quiesce = q[0];
	n->source = pid;
	int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
	n->source_pidfd = pidfd;
	int binding = pidfd < 0 ? -errno
		      : pidfd < 3
			  ? -EBADF
			  : wr_roles_register(r, WR_SOURCE, pidfd, p->self_exe, pid, parent);
	if (binding) {
		close(n->lease);
		n->lease = -1;
		if (pidfd >= 3)
			(void)syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, NULL, 0u);
		return binding;
	}
	for (unsigned role = WR_BROKER; role <= WR_CONSUMER; role++) {
		if ((e = wait_input(n->report, deadline)) ||
		    (e = wr_role_receive(n->report, r, (enum wr_role)role,
					 role == WR_BROKER ? p->launch.broker : p->launch.consumer,
					 role == WR_BROKER ? pid : r->process[WR_BROKER].pid)) ||
		    (e = grant_guard(p, r, deadline, &last)))
			return start_failed(n, e);
		{
			ssize_t sent = send(n->report, "G", 1, MSG_NOSIGNAL);
			if (sent != 1)
				return start_failed(n, sent < 0 ? -errno : -EIO);
		}
		if ((e = grant_guard(p, r, deadline, &last)))
			return start_failed(n, e);
	}
	unsigned dirty;
	if ((e = wait_input(n->quiesce, deadline)) ||
	    (e = wr_quiesce_receive(n->quiesce, "NVLIVE04", r->nonce, r->process[WR_CONSUMER].pid,
				    &dirty)) ||
	    (e = grant_guard(p, r, deadline, &last)))
		return start_failed(n, e);
	n->live = 1;
	r->unresolved_descendants = 0;
	return 0;
}
int wr_nv_quiesce(struct wr_nv *n, struct wr_roles *r, uint64_t deadline, unsigned *dirty)
{
	if (!n || !r || !n->live || wr_process_live(&r->process[WR_CONSUMER]))
		return -EINVAL;
	if (wr_quiesce_send_request(n->quiesce, r->nonce) || wait_input(n->quiesce, deadline))
		return -EIO;
	if (wr_quiesce_receive(n->quiesce, "NVDONE04", r->nonce, r->process[WR_CONSUMER].pid,
			       dirty) ||
	    !ms() || ms() >= deadline)
		return -EPROTO;
	return 0;
}
int wr_nv_revoke(struct wr_nv *n, struct wr_roles *r, uint64_t deadline)
{
	if (!n || !r)
		return -EINVAL;
	if (n->lease >= 0) {
		close(n->lease);
		n->lease = -1;
	}
	int result = 0;
	for (unsigned role = WR_CONSUMER; role >= WR_SOURCE; role--) {
		int rc = wr_wait_exited(&r->process[role], deadline);
		if (rc)
			result = rc;
	}
	if (!result) {
		struct cpb_reap_fact fact;
		result = wr_wait_reaped(&r->process[WR_SOURCE], deadline, &fact);
		if (!result)
			n->live = 0;
	}
	return result;
}
