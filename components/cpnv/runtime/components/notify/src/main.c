/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "refnotify_transport.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int number(const char *s, uint64_t *out)
{
	uint64_t n = 0;
	if (!s || !*s)
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
int main(int argc, char **argv)
{
	uint64_t epoch, gen, parent, uid;
	struct rt_config c;
	if (argc == 2 && !strcmp(argv[1], "--help")) {
		puts("udx-refnotifyd --inherited EPOCH GENERATION PARENT_PID CLIENT_UID\nFixed "
		     "inherited FDs: 3 notify,4 session,5 lease,6 report,7 optional read-only "
		     "RTC,8 optional listening socket. Native parent owns grant/revoke. No device "
		     "opens, time writes, reboot or AT.");
		return 0;
	}
	if (argc != 6 || strcmp(argv[1], "--inherited") || number(argv[2], &epoch) || !epoch ||
	    number(argv[3], &gen) || gen > UINT32_MAX || number(argv[4], &parent) || parent < 2 ||
	    parent > INT_MAX || number(argv[5], &uid) || uid > UINT32_MAX)
		return 64;
	c = (struct rt_config){3,	   4,	5, 6, 7, 8, epoch, (uint32_t)gen, (pid_t)parent,
			       (uid_t)uid, 3000};
	if (fcntl(7, F_GETFD) < 0 && errno == EBADF)
		c.rtc = -1;
	if (fcntl(8, F_GETFD) < 0 && errno == EBADF)
		c.listener = -1;
	int e = rt_run(&c);
	return e == -ECANCELED ? 0 : e ? 74 : 0;
}
