/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_parent.h"
#include "wr_notify.h"
#include "session_monitor.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#ifndef WR_PARENT_BUDGET
#define WR_PARENT_BUDGET 600000u
#endif
static uint64_t ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0)
		return 0;
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static int transition(struct wr_parent *p, enum wr_event e, int error, uint32_t generation)
{
	int rc = wr_state_event(&p->transaction, e, ms(), error, generation);
	FILE *log = rc ? stderr : stdout;
	fprintf(log, "warm_parent=phase phase=%s error=%d\n", wr_phase_name(p->transaction.phase),
		rc);
	fflush(log);
	return rc;
}
static void generation_init(struct wr_generation *g, uint64_t nonce)
{
	memset(g, 0, sizeof *g);
	rt_child_init(&g->notify);
	wr_roles_init(&g->roles, nonce);
	g->cp.lease = g->cp.phase = g->cp.report = g->cp.continuation = g->cp.pidfd = -1;
	g->nv.report = g->nv.lease = g->nv.quiesce = g->nv.source_pidfd = -1;
	for (unsigned i = 0; i < 4; i++)
		g->epoch.fd[i] = -1;
}
static int generation_close(struct wr_generation *g)
{
	int e = wr_roles_close(&g->roles);
	if (e)
		return e;
	rt_child_close_exited(&g->notify);
	wr_controller_close(&g->cp);
	wr_nv_close(&g->nv);
	g->cp.pid = 0;
	g->nv.source = 0;
	for (unsigned i = 0; i < 4; i++)
		if (g->epoch.fd[i] >= 0) {
			close(g->epoch.fd[i]);
			g->epoch.fd[i] = -1;
		}
	return 0;
}
static int wait_session(struct wr_generation *g, uint64_t end)
{
	uint64_t last = ms();
	if (!last)
		return -EIO;
	for (;;) {
		uint64_t now = ms();
		if (!now || now < last)
			return -EIO;
		if (now >= end)
			return -ETIMEDOUT;
		if (nvs_cancelled)
			return -ECANCELED;
		last = now;
		int e = wr_process_live(&g->roles.process[WR_CONTROLLER]);
		if (e)
			return e;
		char text[128];
		struct udx_snapshot snap;
		ssize_t n = pread(g->epoch.fd[2], text, sizeof text, 0);
		if (n < 0)
			return -errno;
		if (!n || n > 127 || udx_parse_snapshot(2, text, (size_t)n, &snap) ||
		    snap.state == 2 || snap.error)
			return -EPROTO;
		if (snap.state == 1) {
			g->epoch.pinned = 0;
			return nvc_epoch_capture(&g->epoch) ? -EIO : 0;
		}
		if (poll(NULL, 0, 25) < 0 && errno != EINTR)
			return -errno;
	}
}
static int postload_admission(void *opaque, uint64_t deadline)
{
	return wr_postload_nv_admit(opaque, deadline);
}
static void nv_plan(struct wr_parent *p, struct wr_generation *g, struct wr_nv_plan *n,
		    int previous)
{
	memset(n, 0, sizeof *n);
	n->self_exe = p->self;
	if (p->postload.enabled) {
		n->admission = postload_admission;
		n->admission_context = p;
	}
	n->launch.profile = p->policy.nv;
	n->launch.owner = p->owner;
	n->launch.boot = p->boot;
	n->launch.consumer = p->consumer;
	n->launch.broker = p->broker;
	n->launch.old_pidfd = previous;
	n->launch.cp_phase_fd = g->cp.phase;
	n->launch.cp_parent_pid = g->cp.pid;
	n->launch.cp_owner_pidfd = g->roles.process[WR_CONTROLLER].pidfd;
	n->launch.cp_owner_exe_fd = p->controller;
	n->launch.nonce = g->roles.nonce;
	n->launch.epoch = g->epoch;
}
static int fail(struct wr_parent *p, int e)
{
	rt_child_withdraw(&p->current.notify);
	p->uncertain = 1;
	/* Revoke outstanding leases. This is cancellation, not
	 * proof of exit or storage quiescence: retain live child handles until exit. */
	if (p->current.nv.lease >= 3) {
		close(p->current.nv.lease);
		p->current.nv.lease = -1;
	}
	if (p->current.cp.lease >= 3) {
		close(p->current.cp.lease);
		p->current.cp.lease = -1;
	}
	if (p->transaction.phase != WR_DISABLED)
		transition(p, WR_EV_FAILURE, e, 0);
	return e ? e : -EIO;
}

static int initial_phase_check(struct wr_parent *p, uint64_t end)
{
	if (nvs_cancelled)
		return -ECANCELED;
	uint64_t now = ms();
	if (!now || now < p->transaction.last_ms)
		return -EIO;
	if (now >= end)
		return -ETIMEDOUT;
	p->transaction.last_ms = now;
	return p->postload.ready ? wr_postload_admit(p, end) : 0;
}
int wr_parent_start_initial(struct wr_parent *p, uint64_t nonce)
{
	if (!p || p->started || p->uncertain || p->retired_valid || !nonce ||
	    !p->postload.enabled || p->postload.initial_load != 1)
		return -EPERM;
	int e = wr_policy_validate(&p->policy, 1);
	if (e)
		return e;
	if (nvs_cancelled)
		return -ECANCELED;
	generation_init(&p->current, nonce);
	p->cancelled = 0;
	if (p->notify_enabled)
		p->current.roles.required_mask |= 1u << WR_NOTIFY;
	uint64_t now = ms();
	if (!now || now > UINT64_MAX - WR_PARENT_BUDGET)
		return fail(p, -EIO);
	uint64_t end = now + WR_PARENT_BUDGET;
	struct wr_factory f;
	unsigned factory = 0;
	if ((e = wr_state_init_initial(&p->transaction, nonce, 0, now, WR_PARENT_BUDGET)))
		goto failed;
	if ((e = wr_parent_open_sessions(&p->current.epoch)))
		goto failed;
	/* Initial loading has no old managed NV child to stop. */
	if ((e = initial_phase_check(p, end)))
		goto failed;
	factory = 1;
	e = wr_factory_prepare_initial(&f, p->run, p->cp_owner, -1, -1, -1, -1, nonce, &p->policy);
	if (e)
		goto failed;
	if ((e = initial_phase_check(p, end)) || (e = wr_postload_attach(&p->postload, &f)) ||
	    (e = wr_controller_spawn(&p->current.cp, &f, &p->current.roles, p->controller, p->run,
				     end)) ||
	    (e = wr_controller_report(&p->current.cp, &p->current.roles, "APSTOP04", end)) ||
	    (e = transition(p, WR_EV_STOPPED, 0, 0)))
		goto failed;
	if ((e = initial_phase_check(p, end)) || (e = wr_factory_snapshots(&f, &p->policy, end)) ||
	    (e = initial_phase_check(p, end)) || (e = transition(p, WR_EV_SNAPSHOTS, 0, 0)) ||
	    (e = wr_controller_continue(&p->current.cp, &f, &p->current.roles, end)))
		goto failed;
	if ((e = initial_phase_check(p, end)) ||
	    (e = wr_controller_report(&p->current.cp, &p->current.roles, "APLOAD04", end)) ||
	    (e = transition(p, WR_EV_LOAD_COMPLETE, 0, 0)))
		goto failed;
	{
		int fd = p->current.cp.continuation;
		p->current.cp.continuation = -1;
		if (fd >= 3 && close(fd)) {
			e = -errno;
			goto failed;
		}
	}
	wr_factory_remove_loaded_snapshots(&f);
	wr_factory_close(&f);
	factory = 0;
	if ((e = initial_phase_check(p, end)) ||
	    (e = wr_controller_report(&p->current.cp, &p->current.roles, "APSTRT04", end)) ||
	    (e = transition(p, WR_EV_START_COMPLETE, 0, 0)))
		goto failed;
	/* CP can fill its NV ring before replying to HALO's factory query.
	 * Start the NV service after Alive and the real channel generation are
	 * established. Complete HALO verification remains mandatory for readiness. */
	if ((e = initial_phase_check(p, end)) || (e = wr_postload_wait_channels(p, end)) ||
	    (e = wait_session(&p->current, end)) ||
	    (e = transition(p, WR_EV_NEW_SESSION, 0, p->current.epoch.generation[2])) ||
	    (e = initial_phase_check(p, end)))
		goto failed;

	struct wr_nv_plan plan;
	nv_plan(p, &p->current, &plan, -1);
	if ((e = initial_phase_check(p, end)) ||
	    (e = wr_nv_start(&p->current.nv, &p->current.roles, &plan, end)) ||
	    (e = wr_postload_wait(p, end)) || (e = initial_phase_check(p, end)) ||
	    (e = wr_notify_start(p, &p->current, end)) || (e = initial_phase_check(p, end)) ||
	    (e = transition(p, WR_EV_NV_GRANTED, 0, 0)) || (e = wr_postload_admit(p, end)))
		goto failed;
	p->started = 1;
	return 0;
failed:
	if (factory)
		wr_factory_close(&f);
	return fail(p, e ? e : -EIO);
}
int wr_parent_start(struct wr_parent *p, uint64_t nonce)
{
	if (p && p->postload.initial_load)
		return wr_parent_start_initial(p, nonce);
	if (!p || p->started || p->uncertain || !nonce || wr_policy_validate(&p->policy, 0))
		return -EPERM;
	generation_init(&p->current, nonce);
	p->cancelled = 0;
	if (p->notify_enabled)
		p->current.roles.required_mask |= 1u << WR_NOTIFY;
	uint64_t now = ms();
	if (!now)
		return fail(p, -EIO);
	uint64_t end = now + WR_PARENT_BUDGET;
	struct wr_factory f;
	int e;
	if ((e = wr_parent_open_sessions(&p->current.epoch)))
		return fail(p, e);
	if ((e = wr_factory_preloaded(&f, p->cp_owner, -1, -1, nonce, &p->policy))) {
		wr_factory_close(&f);
		return fail(p, e);
	}
	if (p->postload.enabled && (e = wr_postload_attach(&p->postload, &f))) {
		wr_factory_close(&f);
		return fail(p, e);
	}
	e = wr_controller_spawn(&p->current.cp, &f, &p->current.roles, p->controller, p->run, end);
	wr_factory_close(&f);
	if (e)
		return fail(p, e);
	if ((e = wr_postload_wait_channels(p, end)) || (e = wait_session(&p->current, end)))
		return fail(p, e);
	struct wr_nv_plan n;
	nv_plan(p, &p->current, &n, -1);
	if ((e = wr_postload_nv_admit(p, end)) ||
	    (e = wr_nv_start(&p->current.nv, &p->current.roles, &n, end)) ||
	    (e = wr_postload_wait(p, end)) || (e = wr_postload_admit(p, end)) ||
	    (e = wr_notify_start(p, &p->current, end)) || (e = wr_postload_admit(p, end)))
		return fail(p, e);
	p->started = 1;
	return 0;
}
int wr_parent_check(struct wr_parent *p)
{
	if (!p || !p->started || p->uncertain)
		return -EPERM;
	int e;
	for (unsigned i = 0; i < WR_ROLE_COUNT; i++) {
		if (i == WR_NOTIFY && !p->notify_enabled)
			continue;
		if ((e = wr_process_live(&p->current.roles.process[i])))
			return fail(p, e);
	}
	if (p->notify_enabled && (e = rt_child_check(&p->current.notify)))
		return fail(p, e);
	if (p->postload.enabled && (e = wr_postload_check(p)))
		return fail(p, e);
	return 0;
}
static int reload(struct wr_parent *p, uint64_t nonce, struct wr_postload *fresh)
{
	if (!p || !p->started || p->uncertain || p->retired_valid || !nonce ||
	    nonce == p->current.roles.nonce)
		return -EPERM;
	if (p->postload.enabled && !fresh)
		return -EOPNOTSUPP;
	int e;
	if (fresh && (e = wr_postload_fresh(fresh, 1)))
		return e;
	e = wr_policy_validate(&p->policy, 1);
	if (e)
		return e;
	/* check already records uncertainty and revokes leases on observed failure. */
	if ((e = wr_parent_check(p)))
		return e;
	uint64_t now = ms();
	if (!now)
		return fail(p, -EIO);
	if ((e = wr_state_init(&p->transaction, nonce, p->current.epoch.generation[2], now,
			       WR_PARENT_BUDGET)))
		return fail(p, e);
	uint64_t end = p->transaction.deadline;
	unsigned dirty;
	if ((e = transition(p, WR_EV_BEGIN, 0, 0)) ||
	    (p->notify_enabled && (e = rt_child_revoke(&p->current.notify, end))) ||
	    (e = wr_nv_quiesce(&p->current.nv, &p->current.roles, end, &dirty)))
		return fail(p, e);
	if ((e = transition(p, WR_EV_QUIESCE_REPORT, 0, 0)) ||
	    (e = wr_nv_revoke(&p->current.nv, &p->current.roles, end)))
		return fail(p, e);
	if ((e = transition(p, WR_EV_NV_ALL_EXITED, 0, 0)) ||
	    (e = wr_controller_stop(&p->current.cp, &p->current.roles, end)))
		return fail(p, e);
	if ((e = transition(p, WR_EV_CP_EXITED, 0, 0)))
		return fail(p, e);
	/* Old controller and its borrowed HALO capabilities are gone. A new profile
	 * supplies the next provider/session descriptors; never reuse the consumed
	 * profile or accept its stale event stream as new authentication. */
	if (fresh) {
		wr_postload_close(&p->postload);
		p->postload = *fresh;
		wr_postload_init(fresh);
	}
	struct wr_generation next;
	generation_init(&next, nonce);
	if (p->notify_enabled)
		next.roles.required_mask |= 1u << WR_NOTIFY;
	struct wr_factory f;
	int factory = 0, switched = 0;
	if ((e = wr_parent_open_sessions(&next.epoch)))
		goto failed;
	e = wr_factory_prepare(&f, p->run, p->cp_owner, -1, -1, -1, -1, nonce, &p->policy);
	factory = 1;
	if (e)
		goto failed;
	if (p->postload.enabled && (e = wr_postload_attach(&p->postload, &f)))
		goto failed;
	if ((e = wr_controller_spawn(&next.cp, &f, &next.roles, p->controller, p->run, end)) ||
	    (e = wr_controller_report(&next.cp, &next.roles, "APSTOP04", end)) ||
	    (e = transition(p, WR_EV_STOPPED, 0, 0)))
		goto failed;
	if ((e = wr_factory_snapshots(&f, &p->policy, end)) ||
	    (e = transition(p, WR_EV_SNAPSHOTS, 0, 0)) ||
	    (e = wr_controller_continue(&next.cp, &f, &next.roles, end)))
		goto failed;
	if ((e = wr_controller_report(&next.cp, &next.roles, "APLOAD04", end)) ||
	    (e = transition(p, WR_EV_LOAD_COMPLETE, 0, 0)))
		goto failed;
	close(next.cp.continuation);
	next.cp.continuation = -1;
	wr_factory_remove_loaded_snapshots(&f);
	wr_factory_close(&f);
	factory = 0;
	if ((e = wr_controller_report(&next.cp, &next.roles, "APSTRT04", end)) ||
	    (e = transition(p, WR_EV_START_COMPLETE, 0, 0)))
		goto failed;
	/* Admission callbacks read p->current, so publish the new AP generation
	 * before waiting for its events, while retaining all old exit identities. */
	p->retired = p->current;
	p->retired_valid = 1;
	p->current = next;
	switched = 1;
	if ((e = wr_postload_wait_channels(p, end)) || (e = wait_session(&p->current, end)) ||
	    (e = transition(p, WR_EV_NEW_SESSION, 0, p->current.epoch.generation[2])))
		goto failed;
	struct wr_nv_plan n;
	nv_plan(p, &p->current, &n, p->retired.roles.process[WR_CONSUMER].pidfd);
	if ((e = wr_postload_nv_admit(p, end)) ||
	    (e = wr_nv_start(&p->current.nv, &p->current.roles, &n, end)) ||
	    (e = wr_postload_wait(p, end)) || (e = wr_postload_admit(p, end)) ||
	    (e = wr_notify_start(p, &p->current, end)) ||
	    (e = transition(p, WR_EV_NV_GRANTED, 0, 0)) || (e = wr_postload_admit(p, end)))
		goto failed;

	e = generation_close(&p->retired);
	if (e) {
		if (!p->cleanup_error)
			p->cleanup_error = e;
		return fail(p, e);
	}
	p->retired_valid = 0;
	p->started = 1;
	fprintf(stdout,
		"warm_parent=completed new_nv_stream=1 hardware_ready=unproven "
		"halo_exchange_reported=%u\n",
		p->postload.ready);
	fflush(stdout);
	return 0;
failed:
	if (factory)
		wr_factory_close(&f); /* Keep exact live descendants in current for cancellation;
					 old ones are confirmed exited. */
	if (!switched) {
		p->retired = p->current;
		p->retired_valid = 1;
		p->current = next;
	}
	int cleanup = generation_close(&p->retired);
	if (cleanup) {
		if (!p->cleanup_error)
			p->cleanup_error = cleanup;
	} else
		p->retired_valid = 0;
	return fail(p, e ? e : -EIO);
}
int wr_parent_reload(struct wr_parent *p, uint64_t nonce)
{
	return reload(p, nonce, NULL);
}
int wr_parent_reload_postload(struct wr_parent *p, uint64_t nonce, struct wr_postload *fresh)
{
	if (!fresh)
		return -EINVAL;
	return reload(p, nonce, fresh);
}
static int wait_unregistered_child(int fd, pid_t pid, uint64_t end)
{
	if (pid < 2)
		return 0;
	if (fd < 3)
		return -ENODATA; /* Never downgrade retained identity to a recyclable numeric PID.
				  */
	if (syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0) && errno != ESRCH)
		return -errno;
	uint64_t last = ms();
	if (!last)
		return -EIO;
	for (;;) {
		struct cpb_reap_fact fact;
		int e = cpb_pidfd_reap_child(fd, pid, &fact);
		if (!e)
			return 0;
		if (e != -EAGAIN && e != -EINTR)
			return e;
		uint64_t now = ms();
		if (!now || now < last || now >= end)
			return -EBUSY;
		last = now;
		unsigned pause = end - now > 25 ? 25u : (unsigned)(end - now);
		if (poll(NULL, 0, (int)pause) < 0 && errno != EINTR)
			return -errno;
	}
}
static int cancel_generation(struct wr_generation *g, unsigned notify, uint64_t end)
{
	int error = 0, e;
	rt_child_withdraw(&g->notify);
	if (g->nv.lease >= 3) {
		if (close(g->nv.lease) && !error)
			error = -errno;
		g->nv.lease = -1;
	}
	if (g->cp.lease >= 3) {
		if (close(g->cp.lease) && !error)
			error = -errno;
		g->cp.lease = -1;
	}
	for (unsigned i = WR_ROLE_COUNT; i-- > 0;)
		if (g->roles.process[i].registered) {
			e = wr_process_signal(&g->roles.process[i], SIGTERM);
			if (e && !error)
				error = e;
		}
	if (notify) {
		e = rt_child_revoke(&g->notify, end);
		if (e && !error)
			error = e;
	}
	for (unsigned i = WR_ROLE_COUNT; i-- > 0;)
		if (g->roles.process[i].registered) {
			if (g->roles.process[i].parent == getpid()) {
				struct cpb_reap_fact fact;
				e = wr_wait_reaped(&g->roles.process[i], end, &fact);
			} else
				e = wr_wait_exited(&g->roles.process[i], end);
			if (e && !error)
				error = e;
		}
	/* Keep live child handles on unconfirmed cleanup/error. A successful
	 * signal is only a request. Exact first errno is retained across this call. */
	if (!g->roles.process[WR_CONTROLLER].registered) {
		e = wait_unregistered_child(g->cp.pidfd, g->cp.pid, end);
		if (e && !error)
			error = e;
	}
	if (!g->roles.process[WR_SOURCE].registered) {
		e = wait_unregistered_child(g->nv.source_pidfd, g->nv.source, end);
		if (e && !error)
			error = e;
	}
	if (error)
		return error;
	return generation_close(g);
}
int wr_parent_cancel(struct wr_parent *p, uint64_t end)
{
	if (!p)
		return -EINVAL;
	if (p->cancelled)
		return 0;
	p->uncertain = 1;
	int first = cancel_generation(&p->current, p->notify_enabled, end);
	if (p->retired_valid) {
		int e = cancel_generation(&p->retired, p->notify_enabled, end);
		if (!e)
			p->retired_valid = 0;
		else if (!first)
			first = e;
	}
	if (first) {
		if (!p->cleanup_error)
			p->cleanup_error = first;
		return first;
	}
	if (p->postload.enabled)
		wr_postload_close(&p->postload);
	p->started = 0;
	p->cancelled = 1;
	return 0;
}

/* Procd has already stopped the cellular AT owner. Keep the controller's
 * ownership witness alive while the NV consumer finishes any in-flight commit.
 * This is service shutdown, not a claim of hardware CP reset/STOP. */
int wr_parent_shutdown(struct wr_parent *p, uint64_t deadline)
{
	if (!p || !p->started || p->uncertain || p->retired_valid)
		return -EINVAL;
	unsigned dirty = 0;
	int e;
	if ((e = wr_nv_quiesce(&p->current.nv, &p->current.roles, deadline, &dirty)))
		return e;
	fprintf(stdout, "native_shutdown=nv_quiesced incomplete_unacknowledged=%u\n", dirty);
	fflush(stdout);
	if ((e = wr_nv_revoke(&p->current.nv, &p->current.roles, deadline)))
		return e;
	if (p->notify_enabled && (e = rt_child_revoke(&p->current.notify, deadline)))
		return e;
	if ((e = wr_controller_stop(&p->current.cp, &p->current.roles, deadline)))
		return e;
	if ((e = generation_close(&p->current)))
		return e;
	if (p->postload.enabled)
		wr_postload_close(&p->postload);
	p->started = 0;
	p->cancelled = 1;
	return 0;
}
