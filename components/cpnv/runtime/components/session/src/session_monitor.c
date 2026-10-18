/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session_monitor.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#ifndef SYSFS_MAGIC
#define SYSFS_MAGIC 0x62656572
#endif
const char *const udx_snapshot_paths[4] = {
    "/sys/class/spipe/spipe_lte2/session", "/sys/class/spipe/stty_lte0/session",
    "/sys/class/spipe/snv_lte/session", "/sys/bus/platform/devices/89600000.sipa-dele/cp_session"};
static const uint32_t channels[4] = {4, 6, 40, 120};
/* Exact one-line decimal records, no signs on u32, no extra tokens/bytes. */
static int decimal(const char **p, const char *end, uint32_t limit, uint32_t *v)
{
	uint32_t n = 0;
	const char *s = *p;
	if (s == end || *s < '0' || *s > '9')
		return -EPROTO;
	do {
		unsigned d = (unsigned)(*s - '0');
		if (d > limit || n > (limit - d) / 10)
			return -ERANGE;
		n = n * 10 + d;
		s++;
	} while (s < end && *s >= '0' && *s <= '9');
	*p = s;
	*v = n;
	return 0;
}
int udx_parse_snapshot(unsigned i, const char *text, size_t len, struct udx_snapshot *out)
{
	if (i >= 4 || !out || !text || !len || len > 127)
		return -EPROTO;
	uint32_t f[7] = {0};
	const char *p = text, *end = text + len;
	int r, negative = 0;
	unsigned fields = i == 3 ? 7 : 5;
	for (unsigned j = 0; j < fields; j++) {
		if (j == 6 && p < end && *p == '-') {
			negative = 1;
			p++;
		}
		uint32_t max = j == 6 ? (negative ? UINT32_C(2147483648) : 0) : UINT32_MAX;
		if ((r = decimal(&p, end, max, &f[j])))
			return r;
		if (j + 1 < fields) {
			if (p == end || *p++ != ' ')
				return -EPROTO;
		}
	}
	if (p != end - 1 || *p != '\n')
		return -EPROTO;
	if (f[0] != 1 || f[1] != 5 || f[2] != channels[i])
		return -EXDEV;
	if ((i < 3 && f[4] > 2) || (i == 3 && (f[4] > 1 || f[5] > 1)))
		return -EPROTO;
	out->generation = f[3];
	out->state = f[4];
	out->blocked = f[5];
	out->error = negative ? (int32_t)(-(int64_t)f[6]) : 0;
	return 0;
}
