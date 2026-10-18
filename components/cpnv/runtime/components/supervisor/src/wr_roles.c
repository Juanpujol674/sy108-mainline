/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_roles.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static _Atomic uint64_t next_token = 1;
static int token_new(uint64_t *out)
{
	uint64_t n = atomic_load(&next_token);
	for (;;) {
		if (n == UINT64_MAX)
			return -EOVERFLOW;
		if (atomic_compare_exchange_weak(&next_token, &n, n + 1)) {
			*out = n;
			return 0;
		}
	}
}
static uint64_t get(const unsigned char *p, unsigned n)
{
	uint64_t x = 0;
	for (unsigned i = 0; i < n; i++)
		x |= (uint64_t)p[i] << (i * 8);
	return x;
}
static void put(unsigned char *p, uint64_t x, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		p[i] = (unsigned char)(x >> (i * 8));
}
void wr_roles_init(struct wr_roles *r, uint64_t nonce)
{
	memset(r, 0, sizeof *r);
	r->nonce = nonce;
	r->creator = getpid();
	r->required_mask = (1u << WR_NOTIFY) - 1u;
	for (unsigned i = 0; i < WR_ROLE_COUNT; i++)
		r->process[i].pidfd = r->process[i].executable = -1;
}
static int observe(const struct wr_process *p, struct cpb_process_fact *f)
{
	if (!p || !p->registered || !p->backend || !p->token || !p->epoch)
		return -EINVAL;
	int e = cpb_process_observe(p->backend, f);
	if (e < 0)
		return e;
	return f->token != p->token || f->pid != (uint32_t)p->pid ||
		       f->parent != (uint32_t)p->parent
		   ? -ESTALE
		   : e;
}
int wr_process_live(const struct wr_process *p)
{
	struct cpb_process_fact f;
	int e = observe(p, &f);
	return e == 1 ? -ESRCH : e;
}
int wr_process_exited(const struct wr_process *p)
{
	struct cpb_process_fact f;
	int e = observe(p, &f);
	return e == 1 ? 0 : e == 0 ? -EBUSY : e;
}
int wr_roles_register(struct wr_roles *r, enum wr_role role, int pidfd, int exe, pid_t pid,
		      pid_t parent)
{
	if (!r || !r->nonce || (unsigned)role >= WR_ROLE_COUNT || r->process[role].registered ||
	    parent < 1)
		return -EINVAL;
	if (r->creator != getpid())
		return -EXDEV;
	for (unsigned i = 0; i < WR_ROLE_COUNT; ++i)
		if (r->process[i].registered && r->process[i].pid == pid)
			return -EEXIST;
	int parent_role = role == WR_BROKER ? WR_SOURCE : role == WR_CONSUMER ? WR_BROKER : -1;
	if (parent_role >= 0) {
		if (!r->process[parent_role].registered ||
		    r->process[parent_role].epoch != r->nonce ||
		    r->process[parent_role].pid != parent)
			return -ESTALE;
		int e = wr_process_live(&r->process[parent_role]);
		if (e)
			return e;
	} else if (parent != getpid())
		return -ESTALE;
	uint64_t token;
	int rc = token_new(&token);
	if (rc)
		return rc;
	int p = fcntl(pidfd, F_DUPFD_CLOEXEC, 3);
	if (p < 0)
		return -errno;
	int e = fcntl(exe, F_DUPFD_CLOEXEC, 3);
	if (e < 0) {
		int saved = -errno;
		close(p);
		return saved;
	}
	struct cpb_process *backend = NULL;
	rc = parent_role < 0
		 ? cpb_process_bind(p, e, pid, parent, geteuid(), token, &backend)
		 : cpb_process_bind_descendant(p, e, pid, r->process[parent_role].backend,
					       geteuid(), token, &backend);
	if (rc) {
		close(p);
		close(e);
		return rc;
	}
	r->process[role] = (struct wr_process){.pidfd = p,
					       .executable = e,
					       .pid = pid,
					       .parent = parent,
					       .registered = 1,
					       .backend = backend,
					       .token = token,
					       .epoch = r->nonce};
	return 0;
}
int wr_role_send(int fd, enum wr_role role, int pidfd, pid_t pid, uint64_t nonce)
{
	if ((unsigned)role >= WR_ROLE_COUNT || pidfd < 3 || pid < 2 || !nonce)
		return -EINVAL;
	unsigned char b[32] = {0};
	char c[CMSG_SPACE(sizeof(int))] = {0};
	memcpy(b, "WRROLE04", 8);
	put(b + 8, nonce, 8);
	put(b + 16, role, 4);
	put(b + 20, (unsigned)pid, 4);
	struct iovec i = {b, sizeof b};
	struct msghdr m = {
	    .msg_iov = &i, .msg_iovlen = 1, .msg_control = c, .msg_controllen = sizeof c};
	struct cmsghdr *h = CMSG_FIRSTHDR(&m);
	h->cmsg_level = SOL_SOCKET;
	h->cmsg_type = SCM_RIGHTS;
	h->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(h), &pidfd, sizeof pidfd);
	return sendmsg(fd, &m, MSG_NOSIGNAL) == sizeof b ? 0 : -EIO;
}
int wr_role_receive(int fd, struct wr_roles *r, enum wr_role role, int exe, pid_t parent)
{
	unsigned char b[32];
	char c[CMSG_SPACE(sizeof(int) * 4)] = {0};
	struct iovec i = {b, sizeof b};
	struct msghdr m = {
	    .msg_iov = &i, .msg_iovlen = 1, .msg_control = c, .msg_controllen = sizeof c};
	int got = -1, count = 0, bad = 0;
	ssize_t n = recvmsg(fd, &m, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
	if (n < 0)
		return -errno;
	for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h)) {
		if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS ||
		    h->cmsg_len < CMSG_LEN(0)) {
			bad = 1;
			continue;
		}
		size_t bytes = h->cmsg_len - CMSG_LEN(0);
		if (bytes % sizeof(int))
			bad = 1;
		for (size_t j = 0; j < bytes / sizeof(int); j++) {
			int v;
			memcpy(&v, (char *)CMSG_DATA(h) + j * sizeof(int), sizeof v);
			if (!count++)
				got = v;
			else
				close(v);
		}
	}
	int rc = -EPROTO;
	if (bad || n != 32 || (m.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || count != 1 || got < 3 ||
	    !r || memcmp(b, "WRROLE04", 8) || get(b + 8, 8) != r->nonce ||
	    get(b + 16, 4) != (unsigned)role || get(b + 24, 8) || get(b + 20, 4) > INT_MAX)
		goto done;
	rc = wr_roles_register(r, role, got, exe, (pid_t)get(b + 20, 4), parent);
done:
	if (got >= 0)
		close(got);
	return rc;
}
static int observe_retained(const struct wr_process *p, struct cpb_process_fact *f)
{
	if (!p || !p->registered || !p->backend || !p->token || !p->epoch)
		return -EINVAL;
	int e = cpb_process_observe_exit(p->backend, f);
	if (e < 0)
		return e;
	return f->token != p->token || f->pid != (uint32_t)p->pid ||
		       f->parent != (uint32_t)p->parent
		   ? -ESTALE
		   : e;
}
int wr_process_signal(const struct wr_process *p, int signal)
{
	if (signal != SIGTERM)
		return -EINVAL;
	struct cpb_process_fact f;
	int e = observe_retained(p, &f);
	if (e < 0)
		return e;
	return e == 1 ? 0 : cpb_process_cancel_retained(p->backend);
}
int wr_wait_exited(const struct wr_process *p, uint64_t deadline)
{
	struct cpb_process_fact f;
	int e = observe_retained(p, &f);
	if (e < 0)
		return e;
	return e == 1 ? 0 : cpb_process_wait_exit_only(p->backend, deadline, &f);
}
int wr_wait_reaped(const struct wr_process *p, uint64_t deadline, struct cpb_reap_fact *out)
{
	if (out)
		*out = (struct cpb_reap_fact){0};
	if (!out || !deadline)
		return -EINVAL;
	struct cpb_process_fact fact;
	int e = observe_retained(p, &fact);
	if (e < 0)
		return e;
	if (p->parent != getpid())
		return -EPERM;
	if ((e = wr_wait_exited(p, deadline)))
		return e;
	uint64_t last = 0, now;
	for (;;) {
		e = cpb_process_reap_child(p->backend, out);
		if (!e) {
			if (out->token != p->token || out->pid != (uint32_t)p->pid) {
				*out = (struct cpb_reap_fact){0};
				return -ESTALE;
			}
			return 0;
		}
		if (e != -EAGAIN && e != -EINTR)
			return e;
		if ((e = cpb_monotonic_ms(&now)))
			return e;
		if (now < last)
			return -EIO;
		if (now >= deadline)
			return -ETIMEDOUT;
		last = now;
		unsigned pause = deadline - now > 10 ? 10u : (unsigned)(deadline - now);
		if (poll(NULL, 0, (int)pause) < 0 && errno != EINTR)
			return -errno;
	}
}
int wr_roles_exit_receipt(struct wr_roles *r, struct lc_completion *out)
{
	if (out)
		*out = (struct lc_completion){.result = -EINVAL, .effect = LC_NO_EFFECT};
	if (!r || !out || !r->nonce)
		return -EINVAL;
	if (r->creator != getpid()) {
		out->result = -EXDEV;
		return -EXDEV;
	}
	if (r->unresolved_descendants) {
		out->result = -EUCLEAN;
		return -EUCLEAN;
	}
	if (r->receipt_sequence == UINT64_MAX) {
		out->result = -EOVERFLOW;
		return -EOVERFLOW;
	}
	struct lc_state state = {
	    .phase = LC_EXIT_WAIT, .epoch = r->nonce, .sequence = ++r->receipt_sequence};
	out->sequence = state.sequence;
	struct cpb_process *objects[LC_ROLES] = {0};
	for (unsigned i = 0; i < WR_ROLE_COUNT; ++i) {
		struct wr_process *p = &r->process[i];
		if (!p->registered) {
			if (p->backend) {
				out->result = -EPROTO;
				return -EPROTO;
			}
			continue;
		}
		if (p->epoch != r->nonce || !p->token || !p->backend) {
			out->result = -ESTALE;
			return -ESTALE;
		}
		state.registered_mask |= 1u << i;
		state.roles[i] = (struct lc_identity){p->token, (uint32_t)p->pid};
		objects[i] = p->backend;
	}
	return cpb_collect_exits(&state, objects, out);
}
int wr_roles_close(struct wr_roles *r)
{
	if (!r)
		return -EINVAL;
	if (r->creator != getpid())
		return -EXDEV;
	if (r->unresolved_descendants)
		return -EUCLEAN;
	unsigned any = 0;
	for (unsigned i = 0; i < WR_ROLE_COUNT; ++i) {
		if (!r->process[i].registered && r->process[i].backend)
			return -EPROTO;
		any |= r->process[i].registered;
	}
	if (any) {
		struct lc_completion receipt;
		int e = wr_roles_exit_receipt(r, &receipt);
		if (e)
			return e;
		if (receipt.result != LC_OK)
			return -EBUSY;
	}
	/* Collection is a separate local-parent obligation from the exit receipt.
	 * Resolve every direct child before consuming any identity/guard handle. */
	for (unsigned i = 0; i < WR_ROLE_COUNT; ++i) {
		struct wr_process *p = &r->process[i];
		if (p->registered && p->parent == r->creator) {
			struct cpb_reap_fact fact;
			int e = cpb_process_reap_child(p->backend, &fact);
			if (e) {
				if (!r->cleanup_error)
					r->cleanup_error = e;
				return e;
			}
			if (fact.token != p->token || fact.pid != (uint32_t)p->pid) {
				if (!r->cleanup_error)
					r->cleanup_error = -ESTALE;
				return -ESTALE;
			}
		}
	}
	int first = 0;
	for (unsigned i = 0; i < WR_ROLE_COUNT; ++i) {
		struct wr_process *p = &r->process[i];
		if (p->backend) {
			int e = cpb_process_release_exited(&p->backend);
			if (e && !first)
				first = e;
			if (p->backend)
				continue;
		}
		if (p->pidfd >= 0) {
			int fd = p->pidfd;
			p->pidfd = -1;
			if (close(fd) && !first)
				first = -errno;
		}
		if (p->executable >= 0) {
			int fd = p->executable;
			p->executable = -1;
			if (close(fd) && !first)
				first = -errno;
		}
		p->registered = 0;
	}
	if (first) {
		if (!r->cleanup_error)
			r->cleanup_error = first;
		return first;
	}
	wr_roles_init(r, 0);
	return 0;
}
