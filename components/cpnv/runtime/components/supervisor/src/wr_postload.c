/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_postload.h"
#include "wr_parent.h"
#include "halo_peer_gate.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static uint32_t get32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const unsigned char *p)
{
	return get32(p) | (uint64_t)get32(p + 4) << 32;
}
static int now_ms(uint64_t *n)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return -errno;
	if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000 ||
	    (uint64_t)t.tv_sec > (UINT64_MAX - 999) / 1000)
		return -EIO;
	*n = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
	return *n ? 0 : -EIO;
}
static int postload_time(struct wr_postload *h, uint64_t *now)
{
	int e = now_ms(now);
	if (e)
		return e;
	if (*now < h->last_ms)
		return -EIO;
	h->last_ms = *now;
	return 0;
}
void wr_postload_init(struct wr_postload *h)
{
	memset(h, 0, sizeof *h);
	h->events = -1;
	for (unsigned i = 0; i < WR_POSTLOAD_FDS; ++i)
		h->fd[i] = -1;
}
static int number(const char *p, unsigned *n)
{
	uint64_t v = 0;
	if (!*p)
		return -EINVAL;
	for (; *p; ++p) {
		if (*p < '0' || *p > '9' || v > (UINT_MAX - (unsigned)(*p - '0')) / 10)
			return -EINVAL;
		v = v * 10 + (unsigned)(*p - '0');
	}
	*n = (unsigned)v;
	return 0;
}
int wr_postload_parse(const char *text, size_t size, struct wr_postload *out)
{
	if (!out)
		return -EINVAL;
	wr_postload_init(out);
	if (!text || !size || size >= 512 || text[size - 1] != '\n' || memchr(text, 0, size))
		return -EINVAL;
	char b[512];
	memcpy(b, text, size);
	b[size] = 0;
	struct wr_postload h;
	wr_postload_init(&h);
	unsigned mask = 0;
	char *p = b;
	static const char *const keys[] = {
	    "devdir_fd",      "status_session_fd", "halo_session_fd",
	    "provider_rx_fd", "provider_tx_fd",	   "verification_params_fd",
	    "alive_ms",	      "halo_ms",	   "factory_tail"};
	while (*p) {
		char *end = strchr(p, '\n');
		if (!end)
			return -EINVAL;
		*end = 0;
		if (!strcmp(p, "profile=preloaded-halo-v1") ||
		    !strcmp(p, "profile=initial-load-halo-v1")) {
			if (mask & (1u << 9))
				return -EINVAL;
			h.initial_load = !strcmp(p, "profile=initial-load-halo-v1");
			mask |= 1u << 9;
		} else if (!strcmp(p, "ap_policy=halo-normal-v1")) {
			if (mask & (1u << 10))
				return -EINVAL;
			mask |= 1u << 10;
		} else {
			char *eq = strchr(p, '=');
			if (!eq)
				return -EINVAL;
			*eq++ = 0;
			unsigned k, v;
			for (k = 0; k < 9; ++k)
				if (!strcmp(p, keys[k]))
					break;
			if (k == 9 || (mask & (1u << k)) || number(eq, &v))
				return -EINVAL;
			mask |= 1u << k;
			if (k < 6) {
				if (v < 3 || v > INT_MAX)
					return -EINVAL;
				h.fd[k] = (int)v;
			} else if (k == 6)
				h.alive_ms = v;
			else if (k == 7)
				h.halo_ms = v;
			else
				h.factory_tail = v;
		}
		p = end + 1;
	}
	if (mask != 2047 || !h.alive_ms || h.alive_ms > 60000 || !h.halo_ms ||
	    h.halo_ms > MC_HALO_TOTAL_MAX_MS || h.factory_tail > 255)
		return -EINVAL;
	for (unsigned i = 0; i < 6; ++i)
		for (unsigned j = 0; j < i; ++j)
			if (h.fd[i] == h.fd[j])
				return -EINVAL;
	h.enabled = 1;
	*out = h;
	return 0;
}
int wr_postload_load(int fd, struct wr_postload *out)
{
	if (!out)
		return -EINVAL;
	wr_postload_init(out);
	struct stat st;
	int flags = fcntl(fd, F_GETFL);
	if (fd < 3 || flags < 0)
		return -EBADF;
	if (fstat(fd, &st))
		return -errno;
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink > 1 ||
	    (st.st_mode & 0077) || (flags & O_ACCMODE) != O_RDONLY || st.st_size <= 0 ||
	    st.st_size >= 512)
		return -EPERM;
	char b[512];
	ssize_t n = pread(fd, b, sizeof b, 0);
	if (n < 0)
		return -errno;
	if (n != st.st_size)
		return -ESTALE;
	struct wr_postload h;
	int e = wr_postload_parse(b, (size_t)n, &h);
	if (e)
		return e;
	e = wr_postload_validate_fds(&h, fd);
	if (e)
		return e;
	*out = h;
	return 0;
}
int wr_postload_validate_fds(struct wr_postload *h, int excluded_fd)
{
	if (!h || !h->enabled)
		return -EINVAL;
	struct stat ids[6];
	for (unsigned i = 0; i < 6; ++i)
		if (h->fd[i] < 3 || h->fd[i] == excluded_fd)
			return -EINVAL;
	for (unsigned i = 0; i < 6; ++i) {
		int mode = fcntl(h->fd[i], F_GETFL);
		if (mode < 0 || fstat(h->fd[i], &ids[i]))
			return -errno;
		struct stat *s = &ids[i];
		if (s->st_uid != geteuid())
			return -EPERM;
		int access = i == 4 ? O_WRONLY : O_RDONLY;
		if ((mode & O_ACCMODE) != access || (mode & (O_PATH | O_APPEND | O_DIRECT)))
			return -EPERM;
		if (i == 0) {
			if (!S_ISDIR(s->st_mode) || (s->st_mode & 0022))
				return -EPERM;
		} else if (i == 3 || i == 4) {
			if (!S_ISFIFO(s->st_mode) || !(mode & O_NONBLOCK))
				return -EPERM;
		} else if (!S_ISREG(s->st_mode))
			return -EPERM;
		if (i == 5 && (s->st_size != MC_VERIFICATION_BYTES || s->st_nlink != 1 ||
			       (s->st_mode & 0077)))
			return -EPERM;
		for (unsigned j = 0; j < i; ++j)
			if (s->st_dev == ids[j].st_dev && s->st_ino == ids[j].st_ino)
				return -EINVAL;
	}
	/* No authority claim from metadata. The controller repeats canonical device,
	 * session, provider and modulus validation before touching a channel. */
	for (unsigned i = 0; i < 6; ++i)
		if (fcntl(h->fd[i], F_SETFD, FD_CLOEXEC))
			return -errno;
	return 0;
}
int wr_postload_fresh(const struct wr_postload *h, unsigned loading)
{
	if (!h || !h->enabled || h->consumed || h->events >= 0 || h->opened || h->alive ||
	    h->ready || h->used || h->last_ms || h->initial_load > 1 ||
	    h->initial_load != loading || !h->alive_ms || h->alive_ms > 60000 || !h->halo_ms ||
	    h->halo_ms > MC_HALO_TOTAL_MAX_MS || h->factory_tail > 255)
		return -EINVAL;
	for (unsigned i = 0; i < 6; ++i) {
		if (h->fd[i] < 3)
			return -EBADF;
		for (unsigned j = 0; j < i; ++j)
			if (h->fd[i] == h->fd[j])
				return -EINVAL;
	}
	return 0;
}
int wr_postload_attach(struct wr_postload *h, struct wr_factory *f)
{
	if (!f || f->config.postload_mode)
		return -EINVAL;
	int e = wr_postload_fresh(h, f->config.reload);
	if (e)
		return e;
	int event[2];
	if (pipe2(event, O_CLOEXEC | O_NONBLOCK))
		return -errno;
	if (event[0] < 3 || event[1] < 3) {
		close(event[0]);
		close(event[1]);
		return -EBADF;
	}
	struct mc_config *c = &f->config;
	c->postload_mode = MC_HALO_NORMAL;
	c->alive_ms = h->alive_ms;
	c->halo_ms = h->halo_ms;
	c->halo_delay_ms = 10000;
	c->factory_tail = h->factory_tail;
	c->devdir = h->fd[0];
	c->status_session = h->fd[1];
	c->halo_session = h->fd[2];
	c->provider_rx = h->fd[3];
	c->provider_tx = h->fd[4];
	c->verification_params = h->fd[5];
	c->events = event[1];
	for (unsigned i = 0; i < 6; ++i)
		h->fd[i] = -1;
	h->events = event[0];
	h->consumed = 1;
	return 0;
}
static int event(struct wr_parent *p, uint64_t now)
{
	struct wr_postload *h = &p->postload;
	const unsigned char *b = h->partial;
	unsigned type = get32(b + 8), auth = get32(b + 48), policy = get32(b + 52),
		 feature = get32(b + 56), mark = get32(b + 60);
	uint32_t status = get32(b + 32), halo = get32(b + 36);
	if (memcmp(b, "MCEVENT2", 8) || get32(b + 12) || get64(b + 16) != p->current.roles.nonce ||
	    !get64(b + 24) || get64(b + 24) > now || get32(b + 40) ||
	    get32(b + 44) != h->initial_load || auth > 1 || policy > 1 || feature > 1 || mark > 1)
		return -EPROTO;
	if (type >= 11 && type <= 15)
		return -EHOSTDOWN;
	if (type == 1) {
		if (h->opened || auth || policy || feature || mark)
			return -EPROTO;
		h->opened = 1;
		h->status_generation = status;
		h->halo_generation = halo;
		return 0;
	}
	if (!h->opened || status != h->status_generation || halo != h->halo_generation)
		return -ESTALE;
	if (type == 10) {
		if (!h->ready && (auth || policy || feature || mark))
			return -EPROTO;
		if (h->ready && (!auth || !policy || !feature || !mark))
			return -EPROTO;
		h->alive = 1;
		return 0;
	}
	if (type != 2 || !h->alive || h->ready || !auth || !policy || !feature || !mark)
		return -EPROTO;
	h->ready = 1;
	return 0;
}
int wr_postload_check(struct wr_parent *p)
{
	if (!p)
		return -EINVAL;
	struct wr_postload *h = &p->postload;
	if (!h->enabled)
		return 0;
	if (!h->consumed || h->events < 3)
		return -EINVAL;
	uint64_t now;
	int e = postload_time(h, &now);
	if (e)
		return e;
	for (unsigned steps = 0; steps < 32; ++steps) {
		ssize_t n = read(h->events, h->partial + h->used, sizeof h->partial - h->used);
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				e = postload_time(h, &now);
				return e ? e : h->used ? -EAGAIN : 0;
			}
			return -errno;
		}
		if (!n)
			return -EPIPE;
		h->used += (unsigned)n;
		if (h->used == sizeof h->partial) {
			h->used = 0;
			if ((e = postload_time(h, &now)) || (e = event(p, now)))
				return e;
		}
	}
	return -EOVERFLOW;
}
static int wait_postload(struct wr_parent *p, uint64_t deadline, int channels_only)
{
	if (!p)
		return -EINVAL;
	if (!p->postload.enabled)
		return 0;
	uint64_t last = 0, now;
	for (;;) {
		int e = now_ms(&now);
		if (e)
			return e;
		if (now < last || now >= deadline)
			return -ETIMEDOUT;
		last = now;
		if (nvs_cancelled)
			return -ECANCELED;
		if ((e = wr_process_live(&p->current.roles.process[WR_CONTROLLER])))
			return e;
		if ((e = wr_postload_check(p)) && e != -EAGAIN)
			return e;
		if ((channels_only ? p->postload.alive : p->postload.ready) && !p->postload.used) {
			if ((e = wr_process_live(&p->current.roles.process[WR_CONTROLLER])))
				return e;
			if ((e = now_ms(&now)))
				return e;
			if (now < last || now < p->postload.last_ms || now >= deadline)
				return -ETIMEDOUT;
			if (nvs_cancelled)
				return -ECANCELED;
			p->postload.last_ms = now;
			return 0;
		}
		/* A failed NV worker must terminate startup, not leave HALO waiting for
		 * an acknowledgement that can no longer arrive. */
		for (unsigned i = WR_SOURCE; i <= WR_CONSUMER; i++)
			if (p->current.roles.process[i].registered &&
			    (e = wr_process_live(&p->current.roles.process[i])))
				return e;
		struct pollfd fd = {p->postload.events, POLLIN, 0};
		int got = poll(&fd, 1, 25);
		if (got < 0 && errno != EINTR)
			return -errno;
		if (got >= 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL)))
			return -EPIPE;
	}
}
int wr_postload_wait_channels(struct wr_parent *p, uint64_t deadline)
{
	return wait_postload(p, deadline, 1);
}
int wr_postload_wait(struct wr_parent *p, uint64_t deadline)
{
	return wait_postload(p, deadline, 0);
}
int wr_postload_nv_admit(struct wr_parent *p, uint64_t deadline)
{
	if (!p)
		return -EINVAL;
	if (!p->postload.enabled)
		return 0;
	if (nvs_cancelled)
		return -ECANCELED;
	int e = wr_postload_check(p);
	if (e && e != -EAGAIN)
		return e;
	if (!p->postload.opened || !p->postload.alive)
		return -EAGAIN;
	if ((e = wr_process_live(&p->current.roles.process[WR_CONTROLLER])))
		return e;
	uint64_t now;
	if ((e = postload_time(&p->postload, &now)))
		return e;
	if (now >= deadline)
		return -ETIMEDOUT;
	if (nvs_cancelled)
		return -ECANCELED;
	return 0;
}
int wr_postload_admit(struct wr_parent *p, uint64_t deadline)
{
	if (!p)
		return -EINVAL;
	if (!p->postload.enabled)
		return 0;
	if (nvs_cancelled)
		return -ECANCELED;
	int e = wr_postload_check(p);
	if (e)
		return e;
	if (!p->postload.ready || p->postload.used)
		return -EAGAIN;
	if ((e = wr_process_live(&p->current.roles.process[WR_CONTROLLER])))
		return e;

	uint64_t now;
	if ((e = postload_time(&p->postload, &now)))
		return e;
	if (now >= deadline)
		return -ETIMEDOUT;
	if (nvs_cancelled)
		return -ECANCELED;
	return 0;
}
void wr_postload_close(struct wr_postload *h)
{
	if (!h)
		return;
	for (unsigned i = 0; i < 6; ++i)
		if (h->fd[i] >= 3) {
			close(h->fd[i]);
			h->fd[i] = -1;
		}
	if (h->events >= 3)
		close(h->events);
	h->events = -1;
}
