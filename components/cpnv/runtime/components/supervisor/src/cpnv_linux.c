/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "cpnv_linux.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CPB_PROC_MAGIC 0x9fa0L
#define CPB_SYSFS_MAGIC 0x62656572L
struct cpb_process {
	int pidfd, executable, taskdir, parent_pidfd;
	pid_t pid, parent, observer_pid;
	uid_t uid;
	uint64_t token;
	struct stat expected;
	unsigned exited;
	struct cpb_reap_fact reap;
};
struct cpb_owner {
	int fd, directory;
	pid_t creator;
	struct stat identity, directory_identity;
	char leaf[128];
};
struct cpb_record {
	int fd, directory;
	long filesystem;
	struct stat identity;
	char leaf[128];
};

static int duplicate(int fd)
{
	int n = fcntl(fd, F_DUPFD_CLOEXEC, 3);
	return n < 0 ? -errno : n;
}
static int close_one(int *fd)
{
	int old = *fd;
	if (old < 0)
		return 0;
	*fd = -1; /* Linux close consumes its slot even on EINTR: NEVER retry. */
	return close(old) ? -errno : 0;
}
static int first(int a, int b)
{
	return a ? a : b;
}
static int leaf_ok(const char *s)
{
	return s && *s && strlen(s) < 128 && !strchr(s, '/') && strcmp(s, ".") && strcmp(s, "..");
}
static int inode_same(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
	       a->st_uid == b->st_uid;
}
static int exe_same(const struct stat *a, const struct stat *b)
{
	return inode_same(a, b) && a->st_size == b->st_size &&
	       a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
	       a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static int fs_matches(int fd, long magic)
{
	struct statfs fs;
	if (fstatfs(fd, &fs))
		return -errno;
	return (unsigned long)fs.f_type == (unsigned long)magic ? 0 : -EXDEV;
}
static int named_same(int dir, const char *leaf, const struct stat *want)
{
	struct stat st;
	if (fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW))
		return -errno;
	return inode_same(&st, want) ? 0 : -ESTALE;
}
static int open_regular_at(int directory, const char *leaf, int access, long expected_fs)
{
	/* O_PATH obtains metadata without invoking a device's open callback. Reopen
	 * only the same pinned, proven regular inode through trusted procfs; opening
	 * the original leaf again would reintroduce a replace-with-device race. */
	int pin = openat(directory, leaf, O_PATH | O_NOFOLLOW | O_CLOEXEC), proc = -1, fd = -1, e;
	struct stat before, after;
	char number[32];
	if (pin < 0)
		return -errno;
	if (fstat(pin, &before)) {
		e = -errno;
		goto bad;
	}
	if (S_ISLNK(before.st_mode)) {
		e = -ELOOP;
		goto bad;
	}
	if (!S_ISREG(before.st_mode)) {
		e = -EPERM;
		goto bad;
	}
	if (expected_fs && (e = fs_matches(pin, expected_fs)))
		goto bad;
	proc = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (proc < 0) {
		e = -errno;
		goto bad;
	}
	if ((e = fs_matches(proc, CPB_PROC_MAGIC)))
		goto bad;
	int n = snprintf(number, sizeof number, "%d", pin);
	if (n < 0 || (size_t)n >= sizeof number) {
		e = -EOVERFLOW;
		goto bad;
	}
	fd = openat(proc, number, access | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		e = -errno;
		goto bad;
	}
	if (fstat(fd, &after)) {
		e = -errno;
		goto bad;
	}
	if (!inode_same(&before, &after)) {
		e = -ESTALE;
		goto bad;
	}
	e = close_one(&pin);
	e = first(e, close_one(&proc));
	if (e)
		goto bad;
	return fd;
bad:
	(void)close_one(&pin);
	(void)close_one(&proc);
	(void)close_one(&fd);
	return e;
}
static int read_at(int directory, const char *leaf, char *b, size_t size)
{
	int fd = openat(directory, leaf, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return -errno;
	ssize_t n;
	do {
		n = pread(fd, b, size, 0);
	} while (n < 0 && errno == EINTR);
	int e = n < 0 ? -errno : n == 0 ? -ENODATA : (size_t)n == size ? -EOVERFLOW : 0;
	e = first(e, close_one(&fd));
	if (e)
		return e;
	b[n] = 0;
	return (int)n;
}
static int field_number(const char *b, const char *key, uint32_t *out)
{
	size_t k = strlen(key);
	unsigned count = 0;
	uint32_t value = 0;
	for (const char *p = b; *p;) {
		const char *end = strchr(p, '\n');
		if (!end)
			return -EPROTO;
		if ((size_t)(end - p) >= k && !memcmp(p, key, k)) {
			const char *q = p + k;
			while (q < end && (*q == ' ' || *q == '\t'))
				++q;
			if (q == end || *q < '0' || *q > '9')
				return -EPROTO;
			uint32_t v = 0;
			while (q < end && *q >= '0' && *q <= '9') {
				unsigned d = (unsigned)(*q++ - '0');
				if (v > (UINT32_MAX - d) / 10u)
					return -EOVERFLOW;
				v = v * 10u + d;
			}
			if (q != end || ++count != 1)
				return -EPROTO;
			value = v;
		}
		p = end + 1;
	}
	if (count != 1)
		return -ENODATA;
	*out = value;
	return 0;
}
static int fd_exited(int pidfd)
{
	struct pollfd fd = {pidfd, POLLIN, 0};
	int n;
	do {
		n = poll(&fd, 1, 0);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return -errno;
	if (fd.revents & (POLLERR | POLLNVAL))
		return -EBADF;
	if (fd.revents & POLLIN)
		return 1;
	if (fd.revents)
		return -EPROTO;
	return 0;
}
static int exited_now(struct cpb_process *p)
{
	int e = fd_exited(p->pidfd);
	if (e == 1)
		p->exited = 1;
	return e;
}
static int pidfd_identity(struct cpb_process *p)
{
	char path[64], b[4097];
	uint32_t pid;
	int flags = fcntl(p->pidfd, F_GETFL);
	if (flags < 0)
		return -errno;
	/* S83 public pidfd.h maps PIDFD_THREAD to O_EXCL and PIDFD_AUTOKILL to
	 * O_TRUNC. Neither belongs in a passive process-owner observation handle. */
	if (flags & (O_EXCL | O_TRUNC | O_PATH))
		return -EPERM;
	/* signal 0 checks the kernel fd type/permission; it delivers no signal. */
	if (syscall(SYS_pidfd_send_signal, p->pidfd, 0, NULL, 0u))
		return -errno;
	if ((flags & O_ACCMODE) != O_RDWR || (flags & ~(O_RDWR | O_NONBLOCK | O_LARGEFILE)))
		return -EPERM;
	int proc = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (proc < 0)
		return -errno;
	int e = fs_matches(proc, CPB_PROC_MAGIC);
	if (!e) {
		int n = snprintf(path, sizeof path, "self/fdinfo/%d", p->pidfd);
		if (n < 0 || (size_t)n >= sizeof path)
			e = -EOVERFLOW;
	}
	if (!e) {
		int n = read_at(proc, path, b, sizeof b - 1);
		if (n < 0)
			e = n;
	}
	e = first(e, close_one(&proc));
	if (e)
		return e;
	if ((e = field_number(b, "Pid:", &pid)))
		return e;
	return pid == (uint32_t)p->pid ? 0 : -ESTALE;
}
int cpb_process_observe(struct cpb_process *p, struct cpb_process_fact *fact)
{
	if (!p || !fact)
		return -EINVAL;
	if (getpid() != p->observer_pid)
		return -EXDEV;
	int e = p->exited ? 1 : exited_now(p);
	if (e < 0)
		return e;
	*fact = (struct cpb_process_fact){
	    p->token,	       (uint64_t)p->expected.st_dev, (uint64_t)p->expected.st_ino,
	    (uint32_t)p->pid,  (uint32_t)p->parent,	     (uint32_t)p->uid,
	    (unsigned)(e == 1)};
	return e;
}
static int bind_impl(int supplied, int exe, pid_t pid, pid_t parent, uid_t uid, uint64_t token,
		     struct cpb_process **out)
{
	struct cpb_process *p;
	struct stat st;
	struct cpb_process_fact fact;
	int e, proc = -1;
	char leaf[32];
	if (!out || *out || supplied < 0 || exe < 0 || pid < 2 || parent < 1 || !token)
		return -EINVAL;
	if (fstat(exe, &st))
		return -errno;
	int flags = fcntl(exe, F_GETFL);
	if (flags < 0)
		return -errno;
	if ((flags & O_ACCMODE) != O_RDONLY)
		return -EPERM;
	if (!S_ISREG(st.st_mode) || !(st.st_mode & 0111) || (st.st_mode & 0022))
		return -EPERM;
	p = calloc(1, sizeof *p);
	if (!p)
		return -ENOMEM;
	p->pidfd = p->executable = p->taskdir = p->parent_pidfd = -1;
	p->pid = pid;
	p->parent = parent;
	p->observer_pid = getpid();
	p->uid = uid;
	p->token = token;
	p->expected = st;
	e = duplicate(supplied);
	if (e < 0)
		goto bad;
	p->pidfd = e;
	e = duplicate(exe);
	if (e < 0)
		goto bad;
	p->executable = e;
	e = pidfd_identity(p);
	if (e)
		goto bad;
	proc = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (proc < 0) {
		e = -errno;
		goto bad;
	}
	if ((e = fs_matches(proc, CPB_PROC_MAGIC)))
		goto bad;
	int n = snprintf(leaf, sizeof leaf, "%ld", (long)pid);
	if (n < 0 || (size_t)n >= sizeof leaf) {
		e = -EOVERFLOW;
		goto bad;
	}
	p->taskdir = openat(proc, leaf, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (p->taskdir < 0) {
		e = -errno;
		goto bad;
	}
	e = close_one(&proc);
	if (e)
		goto bad;
	e = cpb_process_observe(p, &fact);
	if (e) {
		if (e == 1)
			e = -ESRCH;
		goto bad;
	}
	*out = p;
	return 0;
bad:
	(void)close_one(&proc);
	(void)close_one(&p->pidfd);
	(void)close_one(&p->executable);
	(void)close_one(&p->taskdir);
	free(p);
	return e;
}
int cpb_process_bind(int supplied, int exe, pid_t pid, pid_t parent, uid_t uid, uint64_t token,
		     struct cpb_process **out)
{
	if (parent != getpid())
		return -ESTALE;
	return bind_impl(supplied, exe, pid, parent, uid, token, out);
}
int cpb_process_bind_descendant(int supplied, int exe, pid_t pid, struct cpb_process *parent,
				uid_t uid, uint64_t token, struct cpb_process **out)
{
	struct cpb_process_fact fact;
	int e;
	if (!parent || !out || *out)
		return -EINVAL;
	e = cpb_process_observe(parent, &fact);
	if (e)
		return e == 1 ? -ESRCH : e;
	e = bind_impl(supplied, exe, pid, parent->pid, uid, token, out);
	if (e)
		return e;
	e = duplicate(parent->pidfd);
	if (e >= 0) {
		(*out)->parent_pidfd = e;
		e = cpb_process_observe(parent, &fact);
	}
	if (e) {
		struct cpb_process *p = *out;
		*out = NULL;
		(void)close_one(&p->parent_pidfd);
		(void)close_one(&p->pidfd);
		(void)close_one(&p->executable);
		(void)close_one(&p->taskdir);
		free(p);
		return e == 1 ? -ESRCH : e;
	}
	return 0;
}
int cpb_process_request_stop(struct cpb_process *p)
{
	struct cpb_process_fact fact;
	int e = cpb_process_observe(p, &fact);
	if (e < 0)
		return e;
	if (e == 1)
		return 0;
	if (syscall(SYS_pidfd_send_signal, p->pidfd, SIGTERM, NULL, 0u))
		return -errno;
	return 0; /* request accepted; NOT an exit confirmation */
}
int cpb_process_observe_exit(struct cpb_process *p, struct cpb_process_fact *fact)
{
	if (!fact)
		return -EINVAL;
	*fact = (struct cpb_process_fact){0};
	if (!p)
		return -EINVAL;
	if (getpid() != p->observer_pid)
		return -EXDEV;
	int e = p->exited ? 1 : exited_now(p);
	if (e < 0)
		return e;
	*fact = (struct cpb_process_fact){
	    p->token,	       (uint64_t)p->expected.st_dev, (uint64_t)p->expected.st_ino,
	    (uint32_t)p->pid,  (uint32_t)p->parent,	     (uint32_t)p->uid,
	    (unsigned)(e == 1)};
	return e;
}
int cpb_process_cancel_retained(struct cpb_process *p)
{
	struct cpb_process_fact fact;
	int e = cpb_process_observe_exit(p, &fact);
	if (e < 0)
		return e;
	if (e == 1)
		return 0;
	if (syscall(SYS_pidfd_send_signal, p->pidfd, SIGTERM, NULL, 0u) && errno != ESRCH)
		return -errno;
	return 0; /* Cancellation request only, never a successful role/grant check. */
}
int cpb_pidfd_reap_child(int fd, pid_t expected, struct cpb_reap_fact *out)
{
	if (!out)
		return -EINVAL;
	*out = (struct cpb_reap_fact){0};
	if (fd < 3 || expected < 2)
		return -EINVAL;
	siginfo_t status;
	memset(&status, 0, sizeof status);
	if (waitid(P_PIDFD, (id_t)fd, &status, WEXITED | WNOHANG | __WALL)) {
		int saved = -errno;
		if (saved != -ECHILD)
			return saved;
		int dead = fd_exited(fd);
		if (dead < 0)
			return dead;
		if (dead != 1)
			return saved;
		*out = (struct cpb_reap_fact){.pid = (uint32_t)expected,
					      .state = CPB_REAP_NO_WAITABLE_CHILD};
		return 0;
	}
	if (!status.si_pid)
		return -EAGAIN;
	if (status.si_signo != SIGCHLD || status.si_pid != expected ||
	    (status.si_code != CLD_EXITED && status.si_code != CLD_KILLED &&
	     status.si_code != CLD_DUMPED))
		return -EPROTO;
	*out = (struct cpb_reap_fact){.pid = (uint32_t)status.si_pid,
				      .uid = (uint32_t)status.si_uid,
				      .code = status.si_code,
				      .status = status.si_status,
				      .state = CPB_REAP_COLLECTED,
				      .collected_now = 1};
	return 0;
}
int cpb_process_reap_child(struct cpb_process *p, struct cpb_reap_fact *out)
{
	if (!out)
		return -EINVAL;
	*out = (struct cpb_reap_fact){0};
	if (!p)
		return -EINVAL;
	if (getpid() != p->observer_pid)
		return -EXDEV;
	if (p->parent != p->observer_pid || p->parent_pidfd >= 0)
		return -EPERM;
	struct cpb_process_fact exited;
	int e = cpb_process_observe_exit(p, &exited);
	if (e < 0)
		return e;
	if (e != 1)
		return -EBUSY;
	if (p->reap.state) {
		*out = p->reap;
		return 0;
	}
	struct cpb_reap_fact fact;
	e = cpb_pidfd_reap_child(p->pidfd, p->pid, &fact);
	if (e)
		return e;
	fact.token = p->token;
	*out = fact;
	p->reap = fact;
	p->reap.collected_now = 0;
	return 0;
}
int cpb_monotonic_ms(uint64_t *out)
{
	struct timespec t;
	if (!out)
		return -EINVAL;
	*out = 0;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return -errno;
	if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L ||
	    (uint64_t)t.tv_sec > (UINT64_MAX - 999u) / 1000u)
		return -EOVERFLOW;
	*out = (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
	return 0;
}
int cpb_process_wait_exited(struct cpb_process *p, uint64_t deadline, struct cpb_process_fact *fact)
{
	uint64_t last = 0, now;
	int e;
	if (fact)
		*fact = (struct cpb_process_fact){0};
	if (!p || !fact || !deadline)
		return -EINVAL;
	for (;;) {
		e = cpb_process_observe(p, fact);
		if (e < 0)
			return e;
		if (e == 1)
			return 0;
		if ((e = cpb_monotonic_ms(&now)))
			return e;
		if (now < last)
			return -EIO;
		if (now >= deadline)
			return -ETIMEDOUT;
		last = now;
		uint64_t remaining = deadline - now;
		struct pollfd f = {p->pidfd, POLLIN, 0};
		int n = poll(&f, 1, remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining);
		if (n < 0 && errno != EINTR)
			return -errno;
		if (n >= 0 && (f.revents & (POLLNVAL | POLLERR)))
			return -EBADF;
	}
}
int cpb_process_wait_exit_only(struct cpb_process *p, uint64_t deadline,
			       struct cpb_process_fact *fact)
{
	uint64_t last = 0, now;
	int e;
	if (fact)
		*fact = (struct cpb_process_fact){0};
	if (!p || !fact || !deadline)
		return -EINVAL;
	for (;;) {
		e = cpb_process_observe_exit(p, fact);
		if (e < 0)
			return e;
		if (e == 1)
			return 0;
		if ((e = cpb_monotonic_ms(&now)))
			return e;
		if (now < last)
			return -EIO;
		if (now >= deadline)
			return -ETIMEDOUT;
		last = now;
		uint64_t remaining = deadline - now;
		struct pollfd f = {p->pidfd, POLLIN, 0};
		int n = poll(&f, 1, remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining);
		if (n < 0 && errno != EINTR)
			return -errno;
		if (n >= 0 && (f.revents & (POLLNVAL | POLLERR)))
			return -EBADF;
	}
}
int cpb_process_release_exited(struct cpb_process **slot)
{
	struct cpb_process_fact fact;
	int e;
	if (!slot)
		return -EINVAL;
	if (!*slot)
		return 0;
	struct cpb_process *p = *slot;
	e = cpb_process_observe(p, &fact);
	if (e < 0)
		return e;
	if (e != 1)
		return -EBUSY;
	e = close_one(&p->taskdir);
	e = first(e, close_one(&p->executable));
	e = first(e, close_one(&p->pidfd));
	e = first(e, close_one(&p->parent_pidfd));
	free(p);
	*slot = NULL;
	return e;
}
int cpb_file_identity_from_fd(int fd, struct cpb_file_identity *out)
{
	struct stat st;
	if (!out)
		return -EINVAL;
	*out = (struct cpb_file_identity){0};
	if (fstat(fd, &st))
		return -errno;
	if (!S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode) && !S_ISFIFO(st.st_mode))
		return -EINVAL;
	*out = (struct cpb_file_identity){(uint64_t)st.st_dev, (uint64_t)st.st_ino,
					  (uint64_t)st.st_rdev, (uint32_t)st.st_mode};
	return 0;
}
static int decimal_name(const char *s, uint32_t *out)
{
	uint32_t n = 0;
	if (!*s)
		return -EINVAL;
	while (*s) {
		if (*s < '0' || *s > '9')
			return -EINVAL;
		unsigned d = (unsigned)(*s++ - '0');
		if (n > ((uint32_t)INT_MAX - d) / 10u)
			return -ERANGE;
		n = n * 10u + d;
	}
	*out = n;
	return 0;
}
static int target_matches(const struct stat *st, const struct cpb_file_identity *t)
{
	if (S_ISCHR(st->st_mode) && S_ISCHR(t->mode))
		return (uint64_t)st->st_rdev == t->rdev;
	return (st->st_mode & S_IFMT) == (t->mode & S_IFMT) && (uint64_t)st->st_dev == t->device &&
	       (uint64_t)st->st_ino == t->inode;
}
static int scan_thread(int taskdir, const char *name, const struct cpb_file_identity *targets,
		       unsigned count, uint32_t pid, uint32_t tid, struct cpb_alias_fact *fact)
{
	int dir = openat(taskdir, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (dir < 0)
		return -errno;
	int fd = openat(dir, "fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	int e = fd < 0 ? -errno : 0;
	e = first(e, close_one(&dir));
	if (e) {
		(void)close_one(&fd);
		return e;
	}
	DIR *d = fdopendir(fd);
	if (!d) {
		e = -errno;
		(void)close_one(&fd);
		return e;
	}
	unsigned seen = 0;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(d);
		if (!entry) {
			e = errno ? -errno : 0;
			break;
		}
		uint32_t number;
		if (decimal_name(entry->d_name, &number))
			continue;
		if (++seen > 65536u) {
			e = -E2BIG;
			break;
		}
		struct stat st;
		if (fstatat(dirfd(d), entry->d_name, &st, 0)) {
			if (errno == ENOENT)
				continue; /* fd closed between readdir and stat */
			e = -errno;
			break;
		}
		for (unsigned i = 0; i < count; ++i)
			if (target_matches(&st, &targets[i])) {
				*fact = (struct cpb_alias_fact){pid, tid, number, i};
				e = 1;
				break;
			}
		if (e)
			break;
	}
	int close_error = closedir(d) ? -errno : 0;
	return e < 0 ? e : close_error ? close_error : e;
}
int cpb_process_find_alias(struct cpb_process *p, const struct cpb_file_identity *targets,
			   unsigned count, struct cpb_alias_fact *out)
{
	struct cpb_process_fact process;
	struct cpb_alias_fact found = {0};
	int e;
	if (out)
		*out = found;
	if (!p || !targets || !count || count > 64 || !out)
		return -EINVAL;
	for (unsigned i = 0; i < count; ++i)
		if (!targets[i].inode || (!S_ISREG(targets[i].mode) && !S_ISCHR(targets[i].mode) &&
					  !S_ISFIFO(targets[i].mode)))
			return -EINVAL;
	e = cpb_process_observe(p, &process);
	if (e < 0)
		return e;
	if (e == 1)
		return 0;
	int fd = openat(p->taskdir, "task", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	DIR *d = fdopendir(fd);
	if (!d) {
		e = -errno;
		(void)close_one(&fd);
		return e;
	}
	unsigned seen = 0;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(d);
		if (!entry) {
			e = errno ? -errno : 0;
			break;
		}
		uint32_t tid;
		if (decimal_name(entry->d_name, &tid) || !tid)
			continue;
		if (++seen > 65536u) {
			e = -E2BIG;
			break;
		}
		e = scan_thread(dirfd(d), entry->d_name, targets, count, (uint32_t)p->pid, tid,
				&found);
		if (e == -ENOENT) {
			e = -EAGAIN;
			break;
		} /* thread set changed: do not claim absence */
		if (e)
			break;
	}
	int close_error = closedir(d) ? -errno : 0;
	e = e < 0 ? e : close_error ? close_error : e;
	if (e < 0)
		return e;
	int observed = cpb_process_observe(p, &process);
	if (observed < 0)
		return observed;
	if (observed == 1)
		return 0;
	if (!seen)
		return -ENODATA;
	if (e == 1)
		*out = found;
	return e;
}
int cpb_collect_exits(const struct lc_state *s, struct cpb_process *const p[LC_ROLES],
		      struct lc_completion *out)
{
	struct lc_completion c = {0};
	struct cpb_process_fact f;
	int error;
	if (!out)
		return -EINVAL;
	*out = (struct lc_completion){.result = -EINVAL, .effect = LC_NO_EFFECT};
	if (!s || !p || s->phase != LC_EXIT_WAIT || !s->epoch || !s->sequence ||
	    (s->registered_mask & ~31u))
		return -EINVAL;
	c.sequence = s->sequence;
	c.result = LC_PENDING;
	c.effect = LC_NO_EFFECT;
	for (unsigned i = 0; i < LC_ROLES; ++i) {
		if (!(s->registered_mask & (1u << i))) {
			if (p[i]) {
				error = -EPROTO;
				goto bad;
			}
			continue;
		}
		if (!p[i]) {
			error = -ENODATA;
			goto bad;
		}
		int e = cpb_process_observe(p[i], &f);
		if (e < 0) {
			error = e;
			goto bad;
		}
		if (f.token != s->roles[i].token || f.pid != s->roles[i].pid) {
			error = -ESTALE;
			goto bad;
		}
		if (e == 1) {
			c.exited_mask |= 1u << i;
			c.exit_token[i] = f.token;
		}
	}
	if (c.exited_mask == s->registered_mask) {
		c.result = LC_OK;
		c.effect = LC_APPLIED;
	}
	*out = c;
	return 0;
bad:
	*out = (struct lc_completion){
	    .sequence = s->sequence, .result = error, .effect = LC_NO_EFFECT};
	return error;
}
int cpb_owner_acquire_at(int dir, const char *leaf, struct cpb_owner **out)
{
	struct stat st;
	struct cpb_owner *p;
	int e;
	if (!out || *out || !leaf_ok(leaf))
		return -EINVAL;
	if (fstat(dir, &st))
		return -errno;
	if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 07777) != 0700)
		return -EPERM;
	p = calloc(1, sizeof *p);
	if (!p)
		return -ENOMEM;
	p->fd = p->directory = -1;
	p->creator = getpid();
	p->directory_identity = st;
	strcpy(p->leaf, leaf);
	e = duplicate(dir);
	if (e < 0)
		goto bad;
	p->directory = e;
	p->fd = openat(p->directory, leaf,
		       O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
	if (p->fd < 0) {
		if (errno != EEXIST) {
			e = -errno;
			goto bad;
		}
		e = open_regular_at(p->directory, leaf, O_RDWR, 0);
		if (e < 0)
			goto bad;
		p->fd = e;
	}
	if (fstat(p->fd, &p->identity)) {
		e = -errno;
		goto bad;
	}
	if (!S_ISREG(p->identity.st_mode) || p->identity.st_uid != geteuid() ||
	    p->identity.st_nlink != 1 || (p->identity.st_mode & 07777) != 0600) {
		e = -EPERM;
		goto bad;
	}
	if (flock(p->fd, LOCK_EX | LOCK_NB)) {
		e = -errno;
		goto bad;
	}
	if ((e = cpb_owner_path_identity_check(p)))
		goto bad;
	*out = p;
	return 0;
bad:
	(void)close_one(&p->fd);
	(void)close_one(&p->directory);
	free(p);
	return e;
}
int cpb_owner_path_identity_check(struct cpb_owner *p)
{
	struct stat st;
	if (!p)
		return -EINVAL;
	if (getpid() != p->creator)
		return -EXDEV;
	if (fstat(p->directory, &st))
		return -errno;
	if (!inode_same(&st, &p->directory_identity) || (st.st_mode & 07777) != 0700)
		return -ESTALE;
	if (fstat(p->fd, &st))
		return -errno;
	if (!inode_same(&st, &p->identity) || st.st_nlink != 1)
		return -ESTALE;
	return named_same(p->directory, p->leaf, &p->identity);
}
int cpb_owner_release(struct cpb_owner **slot)
{
	if (!slot)
		return -EINVAL;
	if (!*slot)
		return 0;
	struct cpb_owner *p = *slot;
	if (getpid() != p->creator)
		return -EXDEV;
	int e = close_one(&p->fd);
	e = first(e, close_one(&p->directory));
	free(p);
	*slot = NULL;
	return e;
}
int cpb_owner_detach_in_fork_child(struct cpb_owner **slot)
{
	if (!slot)
		return -EINVAL;
	if (!*slot)
		return 0;
	struct cpb_owner *p = *slot;
	if (getpid() == p->creator)
		return -EPERM;
	int e = close_one(&p->fd);
	e = first(e, close_one(&p->directory));
	/* Avoid free after multithreaded fork: exec/_exit reclaims this copy. */
	*slot = NULL;
	return e;
}
int cpb_record_open_at(int dir, const char *leaf, long filesystem, uid_t uid,
		       struct cpb_record **out)
{
	struct cpb_record *p;
	int e;
	if (!out || *out || !leaf_ok(leaf) || !filesystem)
		return -EINVAL;
	p = calloc(1, sizeof *p);
	if (!p)
		return -ENOMEM;
	p->fd = p->directory = -1;
	p->filesystem = filesystem;
	strcpy(p->leaf, leaf);
	e = duplicate(dir);
	if (e < 0)
		goto bad;
	p->directory = e;
	e = open_regular_at(p->directory, leaf, O_RDONLY, filesystem);
	if (e < 0)
		goto bad;
	p->fd = e;
	if (fstat(p->fd, &p->identity)) {
		e = -errno;
		goto bad;
	}
	if (!S_ISREG(p->identity.st_mode) || (p->identity.st_mode & 0222) ||
	    p->identity.st_uid != uid) {
		e = -EPERM;
		goto bad;
	}
	if ((e = fs_matches(p->fd, filesystem)) ||
	    (e = named_same(p->directory, leaf, &p->identity)))
		goto bad;
	*out = p;
	return 0;
bad:
	(void)close_one(&p->fd);
	(void)close_one(&p->directory);
	free(p);
	return e;
}
int cpb_session_open_at(int dir, const char *leaf, struct cpb_record **out)
{
	return cpb_record_open_at(dir, leaf, CPB_SYSFS_MAGIC, 0, out);
}
int cpb_record_snapshot(struct cpb_record *p, unsigned i, struct cpnv_snapshot *x,
			struct cpnv_provider *provider)
{
	char b[129];
	struct stat before, after;
	ssize_t n;
	int e;
	if (x)
		*x = (struct cpnv_snapshot){0};
	if (provider)
		*provider = (struct cpnv_provider){0};
	if (!p || !x || !provider || i >= 4)
		return -EINVAL;
	if (fstat(p->fd, &before))
		return -errno;
	if (!inode_same(&before, &p->identity))
		return -ESTALE;
	if ((e = named_same(p->directory, p->leaf, &p->identity)) ||
	    (e = fs_matches(p->fd, p->filesystem)))
		return e;
	do {
		n = pread(p->fd, b, sizeof b, 0);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return -errno;
	if (n == 0)
		return -ENODATA;
	if (n > 127)
		return -EOVERFLOW;
	if (fstat(p->fd, &after))
		return -errno;
	if (!exe_same(&before, &after))
		return -ESTALE; /* size/mtime/ctime during this read */
	if ((e = named_same(p->directory, p->leaf, &p->identity)))
		return e;
	if (cpnv_parse_snapshot(i, b, (size_t)n, x))
		return -EPROTO;
	*provider = (struct cpnv_provider){(uint64_t)after.st_dev, (uint64_t)after.st_ino,
					   (uint32_t)after.st_mode};
	return 0;
}
int cpb_record_close(struct cpb_record **slot)
{
	if (!slot)
		return -EINVAL;
	if (!*slot)
		return 0;
	struct cpb_record *p = *slot;
	int e = close_one(&p->fd);
	e = first(e, close_one(&p->directory));
	free(p);
	*slot = NULL;
	return e;
}
int cpb_sessions_sample(struct cpb_record *const p[4], struct cpnv_snapshot out[4],
			struct cpnv_provider providers[4])
{
	struct cpnv_snapshot x[4];
	struct cpnv_provider f[4];
	int e;
	if (out)
		memset(out, 0, sizeof x);
	if (providers)
		memset(providers, 0, sizeof f);
	if (!p || !out || !providers)
		return -EINVAL;
	for (unsigned i = 0; i < 4; ++i) {
		if ((e = cpb_record_snapshot(p[i], i, &x[i], &f[i])))
			return e;
	}
	memcpy(out, x, sizeof x);
	memcpy(providers, f, sizeof f);
	return 0;
}
