/* SPDX-License-Identifier: GPL-2.0-only
 * Frozen v1 platform helpers, narrowed fork for continuous service. All transport/storage/ownership
 * handles are explicitly borrowed. No /dev path is opened or discovered here.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "nv_ubi_backend.h"
#include "nv_ubi_linux.h"
#include "nv_ubi_session.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#define SYSFS_MAGIC 0x62656572L
#define LEASE_SIZE 32u
#define PROFILE "v101-stock-compatible-ubi-v1"
struct config {
	int revoke_fd, fd[7]; /* primary,backup,rx,tx,owner,lease,device sysfs dir */
	int vol_dir[2];
	uint32_t size, partition, bits, timeout, policy;
	uint64_t epoch, transport_dev, volume_dev[2], absolute_deadline;
	pid_t supervisor;
};
struct context {
	struct config c;
	struct stat pinned[10];
	int locked, authorized;
	uint64_t deadline;
	struct nv_ubi_linux_context linux;
	struct nv_ubi_identity inventory[2];
	int inventory_ready;
};
static uint8_t packet[NV_MAX_PAYLOAD + NV_FRAME_HEADER];
static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p)
{
	return le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static int decimal(const char *s, uint64_t *out)
{
	uint64_t n = 0;
	if (!*s)
		return -1;
	while (*s) {
		unsigned d = (unsigned)(*s++ - '0');
		if (d > 9 || n > (UINT64_MAX - d) / 10)
			return -1;
		n = n * 10 + d;
	}
	*out = n;
	return 0;
}
static int clock_ms(void *v, uint64_t *out)
{
	struct timespec t;
	(void)v;
	if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0)
		return -EIO;
	*out = (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
	return 0;
}
static int same_file(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_rdev == b->st_rdev &&
	       a->st_uid == b->st_uid && a->st_mode == b->st_mode;
}
static int intact(struct context *c, unsigned n, int fd)
{
	struct stat st;
	return fstat(fd, &st) || !same_file(&st, &c->pinned[n]) ? -ESTALE : 0;
}
static int owner_lock(int fd)
{
	struct flock l;
	memset(&l, 0, sizeof(l));
	l.l_type = F_WRLCK;
	l.l_whence = SEEK_SET;
	return fcntl(fd, F_SETLK, &l);
}
static int lease(void *v, uint64_t epoch)
{
	struct context *c = v;
	struct pollfd p[3];
	if (!c->authorized || !c->locked || epoch != c->c.epoch || getppid() != c->c.supervisor)
		return -EACCES;
	p[0] = (struct pollfd){c->c.fd[5], POLLIN | POLLERR | POLLHUP, 0};
	p[1] = (struct pollfd){c->c.fd[2], POLLERR | POLLHUP, 0};
	p[2] = (struct pollfd){c->c.revoke_fd, POLLIN, 0};
	if (poll(p, 3, 0) < 0)
		return -errno;
	/* Any bytes after the sole grant, pipe EOF, or transport generation HUP
	 * revoke this instance. An epoch is never silently changed in place. */
	if (p[0].revents || p[1].revents || p[2].revents)
		return -EPIPE;
	return 0;
}
/* Read only fixed sysfs attribute names from pinned directory capabilities. */
static int attr(int dir, const char *name, char out[64])
{
	int fd, ret = -1;
	struct stat st;
	struct statfs fs;
	ssize_t n;
	fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || fstatfs(fd, &fs) || fs.f_type != SYSFS_MAGIC || st.st_uid != 0 ||
	    !S_ISREG(st.st_mode) || (st.st_mode & 0222))
		goto done;
	n = pread(fd, out, 63, 0);
	if (n <= 0 || n >= 63 || out[n - 1] != '\n')
		goto done;
	out[n - 1] = 0;
	ret = 0;
done:
	close(fd);
	return ret;
}
static int attr_number(int dir, const char *name, uint64_t *n)
{
	char s[64];
	return attr(dir, name, s) || decimal(s, n) ? -1 : 0;
}
static int attr_dev(int dir, uint64_t *rdev)
{
	char s[64], *p;
	uint64_t major_n, minor_n;
	if (attr(dir, "dev", s) || (p = strchr(s, ':')) == 0)
		return -1;
	*p++ = 0;
	if (decimal(s, &major_n) || decimal(p, &minor_n) || major_n > UINT_MAX ||
	    minor_n > UINT_MAX)
		return -1;
	*rdev = (uint64_t)makedev((unsigned)major_n, (unsigned)minor_n);
	return major((dev_t)*rdev) != major_n || minor((dev_t)*rdev) != minor_n ? -1 : 0;
}
static int metadata(void *v, int fd, struct nv_ubi_identity *out)
{
	struct context *c = v;
	unsigned i = fd == c->c.fd[0] ? 0 : 1;
	struct stat st;
	struct statfs fs;
	uint64_t dev, base, minio, ro, lebs, leb, corrupt, marker;
	char type[64], name[64];
	int vd = c->c.vol_dir[i], dd = c->c.fd[6];
	if ((fd != c->c.fd[0] && fd != c->c.fd[1]) || intact(c, 6, dd) || intact(c, 7 + i, vd) ||
	    fstat(fd, &st))
		return -ESTALE;
	if (fstatfs(vd, &fs) || fs.f_type != SYSFS_MAGIC || fstatfs(dd, &fs) ||
	    fs.f_type != SYSFS_MAGIC)
		return -EINVAL;
	if (attr_dev(vd, &dev) || attr_dev(dd, &base) || dev != (uint64_t)st.st_rdev ||
	    dev != c->c.volume_dev[i] || major((dev_t)base) != major(st.st_rdev) ||
	    minor((dev_t)base) != 0 || attr_number(dd, "min_io_size", &minio) ||
	    attr_number(dd, "ro_mode", &ro) || attr_number(vd, "reserved_ebs", &lebs) ||
	    attr_number(vd, "usable_eb_size", &leb) || attr_number(vd, "corrupted", &corrupt) ||
	    attr_number(vd, "upd_marker", &marker) || attr(vd, "type", type) || ro || corrupt ||
	    marker || !lebs || !leb || leb > UINT32_MAX || minio > UINT32_MAX ||
	    lebs > UINT64_MAX / leb)
		return -EINVAL;
	{
		static const char *const names[2][2] = {{"nr_fixnv1", "nr_fixnv2"},
							{"nr_runtimenv1", "nr_runtimenv2"}};
		if (c->c.partition < 1 || c->c.partition > 2 || attr(vd, "name", name) ||
		    strcmp(name, names[c->c.partition - 1][i]))
			return -EINVAL;
	}
	memset(out, 0, sizeof(*out));
	out->inode = st.st_ino;
	out->device = st.st_dev;
	out->rdev = st.st_rdev;
	out->capacity = lebs * leb;
	out->leb_size = (uint32_t)leb;
	out->min_io_size = (uint32_t)minio;
	out->owner_uid = st.st_uid;
	if (!strcmp(type, "dynamic"))
		out->volume_type = 1;
	else if (!strcmp(type, "static"))
		out->volume_type = 2;
	else
		return -EINVAL;
	out->health = NVU_HEALTH_REQUIRED;
	return 0;
}
static int parse(int argc, char **argv, struct config *c)
{
	uint64_t seen = 0;
	int i;
	memset(c, 0, sizeof(*c));
	for (i = 0; i < 7; i++)
		c->fd[i] = -1;
	c->vol_dir[0] = c->vol_dir[1] = -1;
	c->revoke_fd = -1;
	for (i = 1; i < argc; i++) {
		uint64_t n, bit;
		int k;
		const char *s = argv[i];
		static const char *names[] = {"--primary-fd",
					      "--backup-fd",
					      "--rx-fd",
					      "--tx-fd",
					      "--owner-fd",
					      "--lease-fd",
					      "--ubi-device-dir-fd",
					      "--primary-dir-fd",
					      "--backup-dir-fd",
					      "--size",
					      "--partition",
					      "--checksum-bits",
					      "--timeout-ms",
					      "--write-policy",
					      "--epoch",
					      "--supervisor-pid",
					      "--transport-rdev",
					      "--primary-rdev",
					      "--backup-rdev",
					      "--revoke-fd",
					      "--deadline-ms"};
		if (!strcmp(s, "--profile")) {
			if (seen & (UINT64_C(1) << 21) || ++i == argc || strcmp(argv[i], PROFILE))
				return -1;
			seen |= UINT64_C(1) << 21;
			continue;
		}
		for (k = 0; k < 21; k++)
			if (!strcmp(s, names[k]))
				break;
		if (k == 21 || ++i == argc || decimal(argv[i], &n))
			return -1;
		bit = UINT64_C(1) << k;
		if (seen & bit)
			return -1;
		seen |= bit;
		if (k < 9) {
			if (n < 3 || n > INT_MAX)
				return -1;
			if (k < 7)
				c->fd[k] = (int)n;
			else
				c->vol_dir[k - 7] = (int)n;
		} else if (k == 20) {
			if (!n)
				return -1;
			c->absolute_deadline = n;
		} else if (k == 19) {
			if (n < 3 || n > INT_MAX)
				return -1;
			c->revoke_fd = (int)n;
		} else if (k == 14)
			c->epoch = n;
		else if (k == 16)
			c->transport_dev = n;
		else if (k == 17 || k == 18)
			c->volume_dev[k - 17] = n;
		else {
			if (n > UINT32_MAX)
				return -1;
			switch (k) {
			case 9:
				c->size = (uint32_t)n;
				break;
			case 10:
				c->partition = (uint32_t)n;
				break;
			case 11:
				c->bits = (uint32_t)n;
				break;
			case 12:
				c->timeout = (uint32_t)n;
				break;
			case 13:
				c->policy = (uint32_t)n;
				break;
			case 15:
				if (n > INT_MAX)
					return -1;
				c->supervisor = (pid_t)n;
				break;
			default:
				return -1;
			}
		}
	}
	{
		uint64_t required = UINT64_C(0x3f) | ((UINT64_C(1) << 16) - (UINT64_C(1) << 9)) |
				    (UINT64_C(1) << 21);
		required |= ((UINT64_C(7) << 6) | (UINT64_C(7) << 16));
		if (c->revoke_fd >= 3)
			required |= UINT64_C(1) << 19;
		if (c->absolute_deadline)
			required |= UINT64_C(1) << 20;
		if (seen != required)
			return -1;
	}
	if (!c->size || c->size > NV_MAX_IMAGE || c->size % 512 ||
	    (c->partition != 1 && c->partition != 2) || (c->bits != 32 && c->bits != 64) ||
	    !c->timeout || c->timeout > 60000 || (c->policy != 1 && c->policy != 3) || !c->epoch ||
	    c->supervisor < 2)
		return -1;
	if (c->fd[2] != c->fd[3] || !c->transport_dev || !c->volume_dev[0] || !c->volume_dev[1] ||
	    c->volume_dev[0] == c->volume_dev[1])
		return -1;
	for (i = 0; i < 6; i++) {
		int j;
		if (c->revoke_fd == c->fd[i])
			return -1;
		for (j = 0; j < i; j++)
			if (c->fd[i] == c->fd[j] && !(i == 3 && j == 2))
				return -1;
	}
	return 0;
}
static int bootstrap(struct context *c, int consume_grant)
{
	unsigned i;
	uint8_t grant[LEASE_SIZE];
	size_t used = 0;
	uint64_t now, last;
	int flags;
	if (getppid() != c->c.supervisor || prctl(PR_SET_PDEATHSIG, SIGTERM) ||
	    getppid() != c->c.supervisor)
		return -1;
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR || clock_ms(c, &now))
		return -1;
	last = now;
	c->deadline = now + c->c.timeout;
	if (c->c.absolute_deadline) {
		if (c->c.absolute_deadline <= now)
			return -1;
		if (c->c.absolute_deadline < c->deadline)
			c->deadline = c->c.absolute_deadline;
	}
	for (i = 0; i < 6; i++)
		if (fstat(c->c.fd[i], &c->pinned[i]))
			return -1;
	if (c->c.revoke_fd >= 3 &&
	    (fstat(c->c.revoke_fd, &c->pinned[9]) || !S_ISFIFO(c->pinned[9].st_mode) ||
	     (fcntl(c->c.revoke_fd, F_GETFL) & O_ACCMODE) != O_RDONLY))
		return -1;
	for (i = 0; i < 2; i++) {
		flags = fcntl(c->c.fd[i], F_GETFL);
		if (flags < 0 || (flags & O_ACCMODE) != O_RDWR ||
		    (flags & (O_APPEND | O_DIRECT | O_PATH | O_NONBLOCK)))
			return -1;
	}
	if (c->pinned[0].st_dev == c->pinned[1].st_dev &&
	    c->pinned[0].st_ino == c->pinned[1].st_ino)
		return -1;
	if (!S_ISREG(c->pinned[4].st_mode) || c->pinned[4].st_uid != 0 ||
	    (c->pinned[4].st_mode & 0077) || c->pinned[4].st_nlink > 1 || owner_lock(c->c.fd[4]))
		return -1;
	c->locked = 1;
	if (!S_ISFIFO(c->pinned[5].st_mode) || (fcntl(c->c.fd[5], F_GETFL) & O_ACCMODE) != O_RDONLY)
		return -1;
	for (i = 2; i < 4; i++) {
		flags = fcntl(c->c.fd[i], F_GETFL);
		if (flags < 0 || !(flags & O_NONBLOCK))
			return -1;
		if (!S_ISCHR(c->pinned[i].st_mode) || c->pinned[i].st_uid != 0 ||
		    (c->pinned[i].st_mode & 0002) ||
		    (uint64_t)c->pinned[i].st_rdev != c->c.transport_dev ||
		    (flags & O_ACCMODE) != O_RDWR)
			return -1;
	}
	{
		int dir[3] = {c->c.fd[6], c->c.vol_dir[0], c->c.vol_dir[1]};
		for (i = 0; i < 3; i++) {
			struct statfs fs;
			unsigned j;
			for (j = 0; j < 6; j++)
				if (dir[i] == c->c.fd[j])
					return -1;
			if (fstat(dir[i], &c->pinned[6 + i]) ||
			    !S_ISDIR(c->pinned[6 + i].st_mode) || c->pinned[6 + i].st_uid != 0 ||
			    fstatfs(dir[i], &fs) || fs.f_type != SYSFS_MAGIC)
				return -1;
		}
	}
	if (!consume_grant) {
		c->authorized = 1;
		return lease(c, c->c.epoch);
	}
	/* Supervisor writes this only after stopping the old writer, closing all
	 * inherited data-fd aliases and arranging non-restart for this epoch. */
	while (used < sizeof(grant)) {
		struct pollfd p = {c->c.fd[5], POLLIN, 0};
		ssize_t n;
		if (getppid() != c->c.supervisor || clock_ms(c, &now) || now < last ||
		    now >= c->deadline || poll(&p, 1, (int)(c->deadline - now)) <= 0 ||
		    (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
			return -1;
		last = now;
		n = read(c->c.fd[5], grant + used, sizeof(grant) - used);
		if (n <= 0)
			return -1;
		used += (size_t)n;
	}
	if (memcmp(grant, "NVOWN101", 8) || le64(grant + 8) != c->c.epoch ||
	    le32(grant + 16) != (uint32_t)c->c.supervisor || le32(grant + 20) != c->c.policy ||
	    le64(grant + 24))
		return -1;
	if (clock_ms(c, &now) || now < last || now >= c->deadline)
		return -1;
	c->authorized = 1;
	int valid = lease(c, c->c.epoch);
	last = now;
	if (valid || clock_ms(c, &now) || now < last || now >= c->deadline) {
		c->authorized = 0;
		return -1;
	}
	return 0;
}
