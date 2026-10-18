/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "refnotify_transport.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int rt_parse_session(const char *s, size_t len, struct rt_snapshot *out)
{
	uint32_t v[5] = {0};
	size_t i = 0;
	if (!s || !out || !len || len >= 128 || s[len - 1] != '\n' || memchr(s, 0, len))
		return -EINVAL;
	for (unsigned f = 0; f < 5; ++f) {
		size_t begin = i;
		while (i < len && s[i] >= '0' && s[i] <= '9') {
			unsigned d = (unsigned)(s[i++] - '0');
			if (v[f] > (UINT32_MAX - d) / 10u)
				return -ERANGE;
			v[f] = v[f] * 10u + d;
		}
		if (i == begin || i >= len || s[i++] != (f == 4 ? '\n' : ' '))
			return -EINVAL;
	}
	if (i != len || v[0] != 1 || v[1] != RT_DST || v[2] != RT_CHANNEL)
		return -EPROTO;
	if (v[4] != 1)
		return -EPIPE;
	out->generation = v[3];
	return 0;
}
int rt_read_session(int fd, struct rt_snapshot *out)
{
	char b[128];
	ssize_t n;
	if (fd < 0 || !out)
		return -EINVAL;
	n = pread(fd, b, sizeof(b), 0);
	if (n < 0)
		return -errno;
	if (!n || n >= (ssize_t)sizeof(b))
		return -EMSGSIZE;
	return rt_parse_session(b, (size_t)n, out);
}
uint64_t rt_now_ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 ||
	    t.tv_nsec >= 1000000000)
		return 0;
	if ((uint64_t)t.tv_sec > (UINT64_MAX - 1000) / 1000)
		return 0;
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
