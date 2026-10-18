/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_notify.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int wr_notify_policy(struct wr_parent *p)
{
	char b[64];
	struct stat st;
	int fd;
	p->notify_enabled = 0;
	p->notify_exe = -1;
	fd = open("/etc/udx-refnotify/owner-policy", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT ? 0 : -errno;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid || st.st_nlink != 1 ||
	    (st.st_mode & 0777) != 0600) {
		close(fd);
		return -EPERM;
	}
	ssize_t n = pread(fd, b, sizeof b, 0);
	close(fd);
	if (n == 10 && !memcmp(b, "enabled=0\n", 10))
		return 0;
	if (n != 10 || memcmp(b, "enabled=1\n", 10))
		return -EINVAL;
	p->notify_enabled = 1;
	return 0;
}
struct notify_gate {
	struct wr_parent *parent;
	struct wr_generation *generation;
	uint64_t deadline, last;
};
static int final_gate(void *opaque)
{
	struct notify_gate *x = opaque;
	struct wr_parent *p = x->parent;
	struct wr_generation *g = x->generation;
	uint64_t now = rt_now_ms();
	if (nvs_cancelled || !now || now < x->last || now >= x->deadline)
		return -ETIMEDOUT;
	x->last = now;
	if (g->roles.required_mask != ((1u << WR_ROLE_COUNT) - 1u))
		return -EINVAL;
	for (unsigned i = 0; i < WR_ROLE_COUNT; i++)
		if (wr_process_live(&g->roles.process[i]))
			return -ECHILD;
	int e = wr_postload_admit(p, x->deadline);
	if (e)
		return e;
	now = rt_now_ms();
	if (nvs_cancelled || !now || now < x->last || now >= x->deadline)
		return -ETIMEDOUT;
	x->last = now;
	return 0;
}
int wr_notify_start(struct wr_parent *p, struct wr_generation *g, uint64_t deadline)
{
	int admission = wr_postload_admit(p, deadline);
	if (admission)
		return admission;
	if (!p->notify_enabled)
		return 0;
	if (p->postload.enabled && g != &p->current)
		return -EINVAL;
	if (p->notify_exe < 3 || g->notify.pid || g->roles.process[WR_NOTIFY].registered)
		return -EPERM;
	uint64_t last = rt_now_ms();
	if (!last)
		return -EIO;
	for (;;) {
		uint64_t now = rt_now_ms();
		if (!now || now < last || now >= deadline || nvs_cancelled)
			return -ETIMEDOUT;
		last = now;
		admission = wr_postload_admit(p, deadline);
		if (admission)
			return admission;
		for (unsigned i = 0; i < WR_NOTIFY; ++i)
			if (wr_process_live(&g->roles.process[i]))
				return -ECHILD;
		struct rt_sources s;
		int e = rt_open_sources(&s);
		if (e == -EPIPE || e == -EAGAIN) {
			poll(NULL, 0, 25);
			continue;
		}
		if (e)
			return e;
		/* Cap child readiness wait at5s; retaining the exact child on failure
		 * allows the parent cancellation path to prove its actual exit. */
		uint64_t ready_end = deadline - now > 5000 ? now + 5000 : deadline;
		e = rt_child_prepare(&g->notify, p->notify_exe, &s, g->roles.nonce, ready_end);
		rt_sources_close(&s);
		if (e) {
			rt_child_withdraw(&g->notify);
			return e;
		}
		e = wr_roles_register(&g->roles, WR_NOTIFY, g->notify.pidfd, p->notify_exe,
				      g->notify.pid, getpid());
		if (e) {
			rt_child_withdraw(&g->notify);
			return e;
		}
		struct notify_gate gate = {p, g, deadline, last};
		e = rt_child_grant(&g->notify, deadline, final_gate, &gate);
		if (e) {
			rt_child_withdraw(&g->notify);
			return e;
		}
		e = wr_postload_admit(p, deadline);
		if (e) {
			rt_child_withdraw(&g->notify);
			return e;
		}
		fprintf(stdout,
			"warm_parent=notify_granted dst=5 channel=4 ring=8 sbuf_generation=%u "
			"rtc=readonly clock_writes=disabled\n",
			g->notify.generation);
		fflush(stdout);
		return 0;
	}
}
