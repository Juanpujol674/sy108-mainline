/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "refnotify_transport.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#define rt_fcntl fcntl
#define rt_open open
#define rt_openat openat
#define rt_fstat fstat
#define rt_fstatfs fstatfs
#define rt_pread pread
#define rt_close close
#define rt_readlinkat readlinkat
#define rt_socket socket
#define rt_bind bind
#define rt_listen listen
void rt_sources_init(struct rt_sources *s)
{
	s->notify = s->session = s->rtc = s->listener = -1;
	s->generation = 0;
}
void rt_sources_close(struct rt_sources *s)
{
	if (!s)
		return;
	int *f[] = {&s->notify, &s->session, &s->rtc, &s->listener};
	for (unsigned i = 0; i < 4; ++i) {
		if (*f[i] >= 0)
			rt_close(*f[i]);
		*f[i] = -1;
	}
}
static int dir(const char *path)
{
	struct stat st;
	struct statfs fs;
	int fd = rt_open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (rt_fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid || (st.st_mode & 0022) ||
	    rt_fstatfs(fd, &fs) || fs.f_type != 0x62656572L) {
		rt_close(fd);
		return -EPERM;
	}
	return fd;
}
static int attribute(int fd)
{
	struct stat st;
	struct statfs fs;
	int f = rt_fcntl(fd, F_GETFL);
	return fd < 0 || f < 0 || (f & O_ACCMODE) != O_RDONLY || rt_fstat(fd, &st) ||
		       !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode & 0222) ||
		       rt_fstatfs(fd, &fs) || fs.f_type != 0x62656572L
		   ? -EPERM
		   : 0;
}
static int devnum(int d, dev_t *out)
{
	char b[64];
	uint32_t values[2] = {0};
	size_t i = 0;
	int fd = rt_openat(d, "dev", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	int e = attribute(fd);
	ssize_t n = e ? -1 : rt_pread(fd, b, sizeof b, 0);
	rt_close(fd);
	if (e)
		return e;
	if (n < 1 || n >= 64)
		return -EPROTO;
	for (unsigned k = 0; k < 2; ++k) {
		size_t begin = i;
		while (i < (size_t)n && b[i] >= '0' && b[i] <= '9') {
			unsigned x = (unsigned)(b[i++] - '0');
			if (values[k] > (UINT32_MAX - x) / 10u)
				return -ERANGE;
			values[k] = values[k] * 10 + x;
		}
		if (begin == i || i >= (size_t)n || b[i++] != (k ? '\n' : ':'))
			return -EPROTO;
	}
	if (i != (size_t)n || !values[0])
		return -EPROTO;
	*out = makedev(values[0], values[1]);
	return major(*out) == values[0] && minor(*out) == values[1] ? 0 : -ERANGE;
}
static int driver(int d, const char *name)
{
	char b[256];
	ssize_t n = rt_readlinkat(d, "device/driver", b, sizeof b - 1);
	if (n < 0)
		return -errno;
	if (!n || n >= (ssize_t)sizeof b - 1)
		return -ENAMETOOLONG;
	b[n] = 0;
	char *p = strrchr(b, '/');
	return strcmp(p ? p + 1 : b, name) ? -ENODEV : 0;
}
static int node(const char *path, int flags, dev_t want)
{
	int fd = rt_open(path, flags | O_CLOEXEC | O_NOFOLLOW);
	struct stat st;
	if (fd < 0)
		return -errno;
	if (rt_fstat(fd, &st) || !S_ISCHR(st.st_mode) || st.st_uid || (st.st_mode & 0002) ||
	    st.st_rdev != want) {
		rt_close(fd);
		return -EPERM;
	}
	return fd;
}
static int snapshot(int fd, struct rt_snapshot *s)
{
	char b[128];
	ssize_t n = rt_pread(fd, b, sizeof b, 0);
	if (n < 0)
		return -errno;
	if (!n || n >= 128)
		return -EPROTO;
	return rt_parse_session(b, (size_t)n, s);
}
int rt_open_sources(struct rt_sources *s)
{
	int d = -1, r = -1, base = -1, e;
	dev_t notify_dev, rtc_dev, after, base_dev;
	struct rt_snapshot a, b;
	struct sockaddr_un address;
	socklen_t length;
	if (!s)
		return -EINVAL;
	rt_sources_init(s);
	d = dir("/sys/class/spipe/spipe_lte8");
	if (d < 0)
		return d;
	if ((e = driver(d, "spipe")) || (e = devnum(d, &notify_dev)))
		goto failed;
	base = dir("/sys/class/spipe/spipe_lte0");
	if (base < 0) {
		e = base;
		base = -1;
		goto failed;
	}
	if ((e = driver(base, "spipe")) || (e = devnum(base, &base_dev)) ||
	    major(base_dev) != major(notify_dev) || minor(base_dev) > UINT_MAX - RT_BUFID ||
	    minor(notify_dev) != minor(base_dev) + RT_BUFID) {
		if (!e)
			e = -EPROTO;
		goto failed;
	}
	rt_close(base);
	base = -1;
	s->session = rt_openat(d, "session", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (s->session < 0) {
		e = -errno;
		goto failed;
	}
	if ((e = attribute(s->session)) || (e = snapshot(s->session, &a)))
		goto failed;
	s->notify = node("/dev/spipe_lte8", O_RDWR | O_NONBLOCK, notify_dev);
	if (s->notify < 0) {
		e = s->notify;
		s->notify = -1;
		goto failed;
	}
	if ((e = snapshot(s->session, &b)) || a.generation != b.generation ||
	    (e = devnum(d, &after)) || after != notify_dev || (e = driver(d, "spipe"))) {
		if (!e)
			e = -ESTALE;
		goto failed;
	}
	s->generation = a.generation;
	rt_close(d);
	d = -1;
	r = dir("/sys/class/rtc/rtc0");
	if (r < 0) {
		e = r;
		r = -1;
		goto failed;
	}
	if ((e = driver(r, "sprd-rtc")) || (e = devnum(r, &rtc_dev)))
		goto failed;
	s->rtc = node("/dev/rtc0", O_RDONLY, rtc_dev);
	if (s->rtc < 0) {
		e = s->rtc;
		s->rtc = -1;
		goto failed;
	}
	if ((e = devnum(r, &after)) || after != rtc_dev || (e = driver(r, "sprd-rtc"))) {
		if (!e)
			e = -ESTALE;
		goto failed;
	}
	rt_close(r);
	r = -1;
	if ((e = rn_linux_timesync_address(&address, &length)))
		goto failed;
	s->listener = rt_socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (s->listener < 0) {
		e = -errno;
		goto failed;
	}
	if (rt_bind(s->listener, (struct sockaddr *)&address, length) ||
	    rt_listen(s->listener, 3)) {
		e = -errno;
		goto failed;
	}
	if ((e = snapshot(s->session, &b)) || b.generation != s->generation) {
		if (!e)
			e = -ESTALE;
		goto failed;
	}
	return 0;
failed:
	if (d >= 0)
		rt_close(d);
	if (r >= 0)
		rt_close(r);
	if (base >= 0)
		rt_close(base);
	rt_sources_close(s);
	return e;
}
