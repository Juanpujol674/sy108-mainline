/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "modem_control.h"
#include "session_monitor.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
static struct mc_runtime *active;
static volatile sig_atomic_t cancelled;
static void cancel_signal(int n)
{
	(void)n;
	cancelled = 1;
}
int mc_now(uint64_t *v)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return -errno;
	*v = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
	return 0;
}
static int same(int a, int b)
{
	struct stat x, y;
	return !fstat(a, &x) && !fstat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
static int path_is(int fd, const char *want)
{
	char link[64], buf[256];
	int n = snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
	if (n < 0 || (size_t)n >= sizeof link)
		return -EINVAL;
	ssize_t got = readlink(link, buf, sizeof buf);
	return got == (ssize_t)strlen(want) && !memcmp(buf, want, (size_t)got) ? 0 : -EXDEV;
}
/* sysfs class paths resolve through symlinks: compare fixed path identity, not
 * readlink spelling. The caller separately requires root-owned sysfs. */
static int canonical_inode(int fd, const char *path)
{
	struct stat a, b;
	if (fstat(fd, &a) || stat(path, &b))
		return -errno;
	return a.st_dev == b.st_dev && a.st_ino == b.st_ino ? 0 : -EXDEV;
}
static int regular(int fd, int access, int private)
{
	struct stat st;
	int f = fcntl(fd, F_GETFL);
	if (f < 0 || fstat(fd, &st))
		return -errno;
	if (!S_ISREG(st.st_mode) || (f & O_ACCMODE) != access || st.st_uid != geteuid() ||
	    (st.st_mode & (private ? 0077 : 0022)) || st.st_nlink != 1)
		return -EPERM;
	return 0;
}
static int fifo(int fd, int access)
{
	struct stat s;
	int f = fcntl(fd, F_GETFL);
	if (f < 0 || fstat(fd, &s))
		return -errno;
	return S_ISFIFO(s.st_mode) && s.st_uid == geteuid() && (f & O_ACCMODE) == access &&
		       (f & O_NONBLOCK)
		   ? 0
		   : -EPERM;
}
static int endpoint(int fd, const char *domain, const char *leaf)
{
	struct stat s;
	struct statfs fs;
	char path[128];
	if (fstat(fd, &s))
		return -errno;
	if (!S_ISREG(s.st_mode) || s.st_uid || fstatfs(fd, &fs) || fs.f_type != 0x9fa0 ||
	    (s.st_mode & 0022))
		return -EPERM;
	int n = snprintf(path, sizeof path, "/proc/%s/%s", domain, leaf);
	return n < 0 || (size_t)n >= sizeof path ? -EINVAL : path_is(fd, path);
}
static int session(int fd, unsigned which, uint32_t *generation, int require_ready)
{
	char b[128];
	struct stat st;
	struct statfs fs;
	if (fstat(fd, &st))
		return -errno;
	if (!S_ISREG(st.st_mode) || (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY)
		return -EPERM;
	if ((st.st_uid || fstatfs(fd, &fs) || fs.f_type != 0x62656572 ||
	     canonical_inode(fd, which ? "/sys/class/spipe/spipe_lte3/session"
				       : "/sys/class/spipe/spipe_lte2/session")))
		return -EXDEV;
	ssize_t n = pread(fd, b, sizeof b, 0);
	struct udx_snapshot x;
	int e = n < 0 ? -errno : udx_parse_snapshot(0, b, (size_t)n, &x);
	if (e)
		return e;
	if (x.state == 2)
		return -ESHUTDOWN;
	if (require_ready && x.state != 1)
		return -ENOLINK;
	if (x.state != 1)
		return 1;
	*generation = x.generation;
	return 0;
}
int mc_guard(struct mc_runtime *r)
{
	uint64_t now = 0, after = 0;
	int e = mc_now(&now);
	if (e)
		return e;
	if (now < r->last_now || now >= r->deadline)
		return -ETIMEDOUT;
	r->last_now = now;
	if (cancelled || getppid() != r->config.parent)
		return -ECANCELED;
	struct pollfd p = {r->config.lease, POLLIN, 0};
	if (poll(&p, 1, 0) < 0)
		return -errno;
	if (p.revents)
		return -ECANCELED;
	if (r->monitor_faults && (e = mc_check_faults(r)))
		return e;
	e = mc_now(&after);
	if (e)
		return e;
	if (after < r->last_now || after >= r->deadline)
		return -ETIMEDOUT;
	r->last_now = after;
	return 0;
}
int mc_deadline(struct mc_runtime *r, unsigned ms)
{
	uint64_t n = 0;
	int e = mc_now(&n);
	if (e)
		return e;
	if (n < r->last_now || n > UINT64_MAX - ms || !ms)
		return -ETIMEDOUT;
	r->last_now = n;
	r->deadline = n + ms;
	return mc_guard(r);
}
int mc_wait(struct mc_runtime *r, int fd, unsigned writing)
{
	int e = mc_guard(r);
	if (e)
		return e;
	uint64_t n = r->last_now, remain = r->deadline - n;
	struct pollfd p[2] = {{r->config.lease, POLLIN, 0},
			      {fd, (short)(writing ? POLLOUT : POLLIN), 0}};
	int got = poll(p, 2, (int)(remain > 100 ? 100 : remain));
	int poll_error = errno;
	e = mc_guard(r);
	if (e)
		return e;
	if (got < 0)
		return poll_error == EINTR ? 1 : -poll_error;
	if (p[0].revents)
		return -ECANCELED;
	if (p[1].revents & (POLLERR | POLLNVAL | POLLHUP))
		return -EPIPE;
	return p[1].revents ? (int)0 : 1;
}
int mc_exact(struct mc_runtime *r, int fd, int writing, void *buffer, size_t size)
{
	size_t done = 0;
	unsigned steps = 0;
	while (done < size) {
		if (++steps > 8192)
			return -ELOOP;
		int e = mc_wait(r, fd, (unsigned)writing);
		if (e < 0)
			return e;
		if (e)
			continue;
		e = mc_guard(r);
		if (e)
			return e;
		ssize_t n = writing ? write(fd, (char *)buffer + done, size - done)
				    : read(fd, (char *)buffer + done, size - done);
		int saved = errno;
		if (n > 0) {
			if (writing && fd == r->halo_tx)
				r->halo_written += (uint64_t)n;
			if (!writing && fd == r->halo_fd)
				r->halo_received += (uint64_t)n;
		}
		e = mc_guard(r);
		if (e)
			return e;
		if (n < 0) {
			if (!writing && (saved == EAGAIN || saved == EINTR))
				continue;
			return -saved;
		}
		if (!n || (size_t)n > size - done)
			return -EPIPE;
		done += (size_t)n;
	}
	return 0;
}
static int info(void *ctx, int fd, struct nv_file_info *out)
{
	(void)ctx;
	struct stat s;
	int f = fcntl(fd, F_GETFL);
	if (f < 0 || fstat(fd, &s))
		return -errno;
	memset(out, 0, sizeof *out);
	out->inode = s.st_ino;
	out->size = (uint64_t)s.st_size;
	out->dev_major = major(s.st_dev);
	out->dev_minor = minor(s.st_dev);
	out->uid = s.st_uid;
	out->nlink = (uint32_t)s.st_nlink;
	out->mode = (uint16_t)s.st_mode;
	out->access = f & O_ACCMODE;
	return 0;
}
static int held(void *ctx, int fd)
{
	struct mc_runtime *r = ctx;
	return fd == r->config.owner ? mc_guard(r) : -EPERM;
}
static long pre(void *ctx, int fd, void *b, size_t n, uint64_t off)
{
	struct mc_runtime *r = ctx;
	int e = mc_guard(r);
	if (e)
		return e;
	ssize_t got = pread(fd, b, n, (off_t)off);
	int err = errno;
	e = mc_guard(r);
	return e ? e : got < 0 ? -err : got;
}
static long put(void *ctx, int fd, const void *b, size_t n, uint64_t off)
{
	struct mc_runtime *r = ctx;
	int e = mc_guard(r);
	if (e)
		return e;
	r->ap_touched = 1;
	ssize_t got = pwrite(fd, b, n, (off_t)off);
	int err = errno;
	e = mc_guard(r);
	return e ? e : got < 0 ? -err : got;
}
static int sync_file(void *ctx, int fd)
{
	int e = mc_guard(ctx);
	if (e)
		return e;
	int got = fsync(fd);
	int saved = errno;
	e = mc_guard(ctx);
	return e ? e : got ? -saved : 0;
}
static long control_write(int fd, const void *b, size_t n)
{
	int e = mc_guard(active);
	if (e)
		return e;
	if (lseek(fd, 0, SEEK_SET) < 0)
		return -errno;
	e = mc_guard(active);
	if (e)
		return e;
	active->ap_touched = 1;
	ssize_t got = write(fd, b, n);
	int err = errno;
	e = mc_guard(active);
	return e ? e : got < 0 ? -err : got;
}
static int cp_wait(int fd, int signal_fd, unsigned ms)
{
	(void)fd;
	(void)signal_fd;
	(void)ms;
	return mc_guard(active);
}
static int cp_now(uint64_t *v)
{
	int e = mc_guard(active);
	if (e)
		return e;
	return mc_now(v);
}
static int close_unlisted(struct mc_runtime *r)
{
	const struct mc_config *c = &r->config;
	int list[96], count = 0;
	int common[] = {0,
			1,
			2,
			c->owner,
			c->lease,
			c->devdir,
			c->status_session,
			c->halo_session,
			c->provider_rx,
			c->provider_tx,
			c->verification_params,
			c->events,
			c->phase,
			c->cmdline,
			c->report,
			c->continuation};
	for (unsigned i = 0; i < sizeof common / sizeof common[0]; i++)
		if (common[i] >= 0)
			list[count++] = common[i];
	for (unsigned d = 0; d < 4; d++) {
		int a[] = {c->status[d], c->start[d], c->stop[d]};
		for (unsigned i = 0; i < 3; i++)
			if (a[i] >= 3)
				list[count++] = a[i];
	}
	for (unsigned i = 0; i < c->image_count; i++) {
		const struct mc_image *v = c->images + i;
		list[count++] = v->source;
		list[count++] = v->target;
		if (v->segment >= 7 && v->backup >= 3)
			list[count++] = v->backup;
	}
	DIR *dir = opendir("/proc/self/fd");
	if (!dir)
		return -errno;
	int directory = dirfd(dir);
	struct dirent *de;
	while ((de = readdir(dir))) {
		char *end;
		long fd = strtol(de->d_name, &end, 10);
		if (*end || fd < 3 || fd == directory)
			continue;
		int keep = 0;
		for (int i = 0; i < count; i++)
			if (list[i] == fd)
				keep = 1;
		if (!keep)
			close((int)fd);
	}
	closedir(dir);
	return 0;
}
static int snapshot_input(int fd)
{
	struct stat st;
	struct statfs fs;
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fstat(fd, &st) || fstatfs(fd, &fs))
		return -errno;
	return S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077) &&
		       st.st_nlink == 1 && (flags & O_ACCMODE) == O_RDONLY && fs.f_type != 0x9fa0 &&
		       fs.f_type != 0x62656572
		   ? 0
		   : -EPERM;
}
static int image_inputs(const struct mc_image *v)
{
	if (v->segment >= 7)
		return v->source == -1 && v->backup == -1 ? 0 : -EINVAL;
	if (v->segment < 6) {
		struct stat st;
		int flags = fcntl(v->source, F_GETFL);
		return flags < 0 || fstat(v->source, &st) || (flags & O_ACCMODE) != O_RDONLY ||
			       (!S_ISCHR(st.st_mode) && !S_ISREG(st.st_mode))
			   ? -EINVAL
			   : 0;
	}
	return snapshot_input(v->source) ||
	       (v->segment >= 7 && (snapshot_input(v->backup) || same(v->source, v->backup)));
}
static int continuation_input(int fd)
{
	int type = 0;
	socklen_t n = sizeof type;
	return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &n) || type != SOCK_STREAM ? -EPERM : 0;
}
static int plan_endpoints(struct mc_runtime *r)
{
	struct mc_config *c = &r->config;
	int protected[] = {c->owner,
			   c->lease,
			   c->phase,
			   c->events,
			   c->provider_rx,
			   c->provider_tx,
			   c->verification_params,
			   c->status_session,
			   c->halo_session,
			   c->devdir,
			   c->cmdline,
			   c->report,
			   c->continuation};
	for (unsigned i = 0; i < sizeof protected / sizeof protected[0]; i++)
		if (protected[i] >= 3)
			for (unsigned j = 0; j < i; j++)
				if (protected[j] >= 3 && same(protected[i], protected[j]))
					return -EINVAL;
	for (unsigned d = 0; d < 4; d++) {
		if (d == 3 && !c->pmic)
			continue;
		int ids[] = {c->status[d], c->start[d], c->stop[d]};
		const char *leaves[] = {"status", "start", "stop"};
		for (unsigned j = 0; j < (c->reload ? 3u : 1u); j++) {
			if (endpoint(ids[j], cpb_domain(d), leaves[j]))
				return -EXDEV;
			for (unsigned k = 0; k < sizeof protected / sizeof protected[0]; k++)
				if (protected[k] >= 3 && same(ids[j], protected[k]))
					return -EINVAL;
			for (int k = 0; k < 3; k++)
				if (same(ids[j], k))
					return -EINVAL;
		}
	}
	for (unsigned i = 0; i < c->image_count; i++) {
		const struct mc_image *v = c->images + i;
		const char *leaf = v->segment == 7   ? "fixnv"
				   : v->segment == 8 ? "runnv"
						     : cpb_segment(v->domain, v->segment);
		if (!leaf || endpoint(v->target, cpb_domain(v->domain), leaf) || image_inputs(v))
			return -EXDEV;
		for (unsigned k = 0; k < sizeof protected / sizeof protected[0]; k++)
			if (protected[k] >= 3 &&
			    (same(v->target, protected[k]) || same(v->source, protected[k]) ||
			     (v->segment >= 7 && same(v->backup, protected[k]))))
				return -EINVAL;
		for (int k = 0; k < 3; k++)
			if (same(v->target, k) || same(v->source, k) ||
			    (v->segment >= 7 && same(v->backup, k)))
				return -EINVAL;
		for (unsigned d = 0; d < 4; d++)
			if (d < 3 || c->pmic) {
				int ids[] = {c->status[d], c->start[d], c->stop[d]};
				for (unsigned k = 0; k < 3; k++)
					if (same(v->target, ids[k]) || same(v->source, ids[k]) ||
					    (v->segment >= 7 && same(v->backup, ids[k])))
						return -EINVAL;
			}
		for (unsigned j = 0; j < c->image_count; j++) {
			if (same(v->source, c->images[j].target) ||
			    (v->segment >= 7 && same(v->backup, c->images[j].target)))
				return -EINVAL;
			if (j < i && same(v->target, c->images[j].target)) {
				const struct mc_image *w = c->images + j;
				if (v->domain != w->domain || v->segment != w->segment ||
				    v->physical_base != w->physical_base ||
				    v->capacity != w->capacity ||
				    (v->target_offset < w->target_offset + w->length &&
				     w->target_offset < v->target_offset + v->length))
					return -EINVAL;
			}
		}
	}
	return 0;
}
static int boot_classification(struct mc_runtime *r)
{
	struct stat st;
	struct statfs fs;
	int fd = r->config.cmdline;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY)
		return -EPERM;
	if ((st.st_uid || fstatfs(fd, &fs) || fs.f_type != 0x9fa0 || path_is(fd, "/proc/cmdline")))
		return -EXDEV;
	char b[4096];
	ssize_t n = pread(fd, b, sizeof b, 0);
	if (n <= 0 || n >= 4096 || memchr(b, 0, (size_t)n))
		return -EPROTO;
	b[n] = 0; /* Preserve observed stock substring test, not an invented token parser. */
	if (!r->config.reload && strstr(b, "modem=shutdown"))
		return -EPERM;
	return 0;
}
int mc_revalidate_snapshots(struct mc_runtime *r)
{
	return mc_warm_plan_validate(&r->config);
}
int mc_validate(struct mc_runtime *r)
{
	struct mc_config *c = &r->config;
	int e;
	if ((e = close_unlisted(r)))
		return e;
	if ((e = plan_endpoints(r)))
		return e;
	if ((e = boot_classification(r)))
		return e;
	for (unsigned i = 0; i < c->image_count; i++) {
		if (c->images[i].segment < 6)
			continue;
		int ids[] = {c->images[i].source, c->images[i].backup};
		for (unsigned j = 0; j < 2; j++)
			if (ids[j] >= 3) {
				struct stat z;
				if (fstat(ids[j], &z) || z.st_size)
					return -EINVAL;
			}
	}
	if (c->reload && (fifo(c->report, O_WRONLY) || continuation_input(c->continuation)))
		return -EPERM;
	if (c->parent != getppid() || regular(c->owner, O_RDWR, 1) || fifo(c->lease, O_RDONLY))
		return -EPERM;
	if (c->postload_mode) {
		if (fifo(c->events, O_WRONLY) || fifo(c->provider_rx, O_RDONLY) ||
		    fifo(c->provider_tx, O_WRONLY) || regular(c->verification_params, O_RDONLY, 1))
			return -EPERM;
		struct stat st;
		if (fstat(c->verification_params, &st) || st.st_size != MC_VERIFICATION_BYTES)
			return -EINVAL;
	}
	int fds[] = {c->owner,		c->lease,	c->events,
		     c->provider_rx,	c->provider_tx, c->verification_params,
		     c->status_session, c->halo_session};
	for (size_t i = 0; i < sizeof fds / sizeof fds[0]; i++) {
		for (int j = 0; j < 3; j++)
			if (same(fds[i], j))
				return -EINVAL;
		for (size_t j = 0; j < i; j++)
			if (same(fds[i], fds[j]))
				return -EINVAL;
	}
	if (c->reload) {
		struct pollfd initial_cont = {c->continuation, POLLIN, 0};
		if (poll(&initial_cont, 1, 0) < 0)
			return -errno;
		if (initial_cont.revents)
			return -EPROTO;
	}
	if (flock(c->owner, LOCK_EX | LOCK_NB))
		return -errno;
	if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != c->parent)
		return -ECANCELED;
	struct sigaction sa = {0};
	sa.sa_handler = cancel_signal;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGTERM, &sa, 0) || sigaction(SIGINT, &sa, 0))
		return -errno;
	signal(SIGPIPE, SIG_IGN);
	/* Initial grant is consumed before guard begins; subsequent bytes/HUP revoke. */
	uint8_t grant[32];
	size_t done = 0;
	uint64_t now = 0;
	if (mc_now(&now))
		return -EIO;
	r->last_now = now;
	r->deadline = now + c->operation_ms;
	while (done < sizeof grant) {
		struct pollfd p = {c->lease, POLLIN, 0};
		if (mc_now(&now) || now >= r->deadline || cancelled || getppid() != c->parent)
			return -ETIMEDOUT;
		int got = poll(&p, 1, 10);
		if (got < 0 && errno != EINTR)
			return -errno;
		if (p.revents & (POLLERR | POLLNVAL | POLLHUP))
			return -EPIPE;
		if (p.revents & POLLIN) {
			ssize_t n = read(c->lease, grant + done, sizeof grant - done);
			if (n <= 0)
				return -EPIPE;
			done += (size_t)n;
		}
	}
	uint8_t want[32] = {0};
	memcpy(want, "MCOWN001", 8);
	for (unsigned i = 0; i < 8; i++)
		want[8 + i] = (uint8_t)(c->epoch >> (8 * i));
	for (unsigned i = 0; i < 4; i++) {
		want[16 + i] = (uint8_t)((unsigned)c->parent >> (8 * i));
		want[20 + i] = (uint8_t)(c->reload >> (8 * i));
	}
	if (memcmp(want, grant, 32))
		return -EPROTO;
	if (c->phase >= 3 && fifo(c->phase, O_WRONLY))
		return -EPERM;

	active = r;
	r->cp = (struct cpb_io){{r, info, held, held, pre, put, sync_file, (uint32_t)geteuid()},
				endpoint,
				control_write,
				cp_wait,
				cp_now};
	return mc_guard(r);
}
/* The OEM waits for a real Alive before opening the HALO RPC channel.
 * An OPEN/INIT transition may race a readiness read; retry only opening an
 * unconsumed channel, under the caller's existing startup deadline. */
static int open_channel(struct mc_runtime *r, unsigned halo)
{
	int e;
	for (;;) {
		uint32_t generation = 0;
		int ready = session(halo ? r->config.halo_session : r->config.status_session, halo,
				    &generation, 0);
		if (ready < 0)
			return ready;
		if (!ready) {
			int fd = -1;
			{
				struct stat d;
				if (fstat(r->config.devdir, &d) || !S_ISDIR(d.st_mode) ||
				    d.st_uid || path_is(r->config.devdir, "/dev"))
					return -EPERM;
				fd = openat(r->config.devdir, halo ? "spipe_lte3" : "spipe_lte2",
					    (halo ? O_RDWR : O_RDONLY) | O_NONBLOCK | O_NOFOLLOW |
						O_CLOEXEC);
				if (fd < 0) {
					e = -errno;
					if (e != -EPIPE && e != -ENODEV && e != -EAGAIN &&
					    e != -EINTR)
						return e;
				} else {
					struct stat st;
					if (fstat(fd, &st)) {
						e = -errno;
						close(fd);
						return e;
					}
					if (!S_ISCHR(st.st_mode) || st.st_uid) {
						close(fd);
						return -EPERM;
					}
					if (halo)
						r->halo_tx = fd;
				}
			}
			if (fd >= 0) {
				if (halo) {
					r->halo_fd = fd;
					r->halo_generation = generation;
					r->channels_armed = 1;
				} else {
					r->status_fd = fd;
					r->status_generation = generation;
				}
				return 0;
			}
		}
		e = mc_wait(r, -1, 0);
		if (e < 0)
			return e;
	}
}
int mc_open_status(struct mc_runtime *r)
{
	return open_channel(r, 0);
}
int mc_open_halo(struct mc_runtime *r)
{
	return open_channel(r, 1);
}
void mc_close_status(struct mc_runtime *r)
{
	if (r->status_fd >= 3)
		close(r->status_fd);
	r->status_fd = -1;
}
void mc_close(struct mc_runtime *r)
{
	if (r->status_fd >= 3)
		close(r->status_fd);
	if (r->halo_fd >= 3 && r->halo_fd != r->status_fd)
		close(r->halo_fd);
	if (r->halo_tx >= 3 && r->halo_tx != r->halo_fd)
		close(r->halo_tx);
	flock(r->config.owner, LOCK_UN);
}
