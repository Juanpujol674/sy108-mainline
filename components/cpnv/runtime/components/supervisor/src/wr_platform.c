/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_parent.h"
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
#include <time.h>
#include <unistd.h>
int wr_parent_open_sessions(struct nvc_epoch *e)
{
	memset(e, 0, sizeof *e);
	for (unsigned i = 0; i < 4; i++)
		e->fd[i] = -1;
	for (unsigned i = 2; i < 3; i++) {
		e->fd[i] = open(udx_snapshot_paths[i], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		struct stat a, b;
		struct statfs fs;
		if (e->fd[i] < 3 || fstat(e->fd[i], &a) || stat(udx_snapshot_paths[i], &b) ||
		    fstatfs(e->fd[i], &fs) || !S_ISREG(a.st_mode) || a.st_uid ||
		    (a.st_mode & 0222) || fs.f_type != 0x62656572L || a.st_dev != b.st_dev ||
		    a.st_ino != b.st_ino)
			return -EPERM;
	}
	return 0;
}
