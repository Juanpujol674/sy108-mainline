/* SPDX-License-Identifier: GPL-2.0-only */
#define NVC_ENTRY consumer_unused_entry
#include "nv_continuous.c"
#include "cpnv_fd_plan.h"
#include <dirent.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include "nvs_report.h"
static volatile sig_atomic_t stopping;
static void stop_requested(int sig)
{
	(void)sig;
	stopping = 1;
}
static int allowed_fd(int fd, const int *keep, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (fd == keep[i])
			return 1;
	return 0;
}
static int close_except(const int *keep, size_t n)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	int own;
	if (!d)
		return -1;
	own = dirfd(d);
	for (;;) {
		uint64_t num;
		errno = 0;
		e = readdir(d);
		if (!e) {
			int error = errno;
			int closed = closedir(d);
			return error || closed ? -1 : 0;
		}
		if (decimal(e->d_name, &num) || num > INT_MAX)
			continue;
		if ((int)num != own && !allowed_fd((int)num, keep, n) && close((int)num)) {
			closedir(d);
			return -1;
		}
	}
}
static int executable_ok(int fd)
{
	struct stat st;
	int flags = fcntl(fd, F_GETFL);
	return flags < 0 || (flags & O_ACCMODE) != O_RDONLY || fstat(fd, &st) ||
		       !S_ISREG(st.st_mode) || !(st.st_mode & 0111)
		   ? -1
		   : 0;
}
static void store32(uint8_t *p, uint32_t n)
{
	unsigned i;
	for (i = 0; i < 4; i++)
		p[i] = (uint8_t)(n >> (8 * i));
}
static int unlock_owner(int fd)
{
	struct flock f;
	memset(&f, 0, sizeof(f));
	f.l_type = F_UNLCK;
	f.l_whence = SEEK_SET;
	return fcntl(fd, F_SETLK, &f);
}
static int broker_roles_distinct(struct continuous *s, int consumer)
{
	int roles[7] = {consumer,	    s->part[0].c.fd[4], s->part[0].c.fd[0],
			s->part[0].c.fd[1], s->part[1].c.fd[0], s->part[1].c.fd[1],
			s->epoch.fd[2]};
	struct stat st[7];
	unsigned i, j;
	for (i = 0; i < 7; i++) {
		if (fstat(roles[i], &st[i]))
			return -1;
		for (j = 0; j < i; j++)
			if (st[i].st_dev == st[j].st_dev && st[i].st_ino == st[j].st_ino)
				return -1;
	}
	return 0;
}
#ifndef NVC_BROKER_ENTRY
#define NVC_BROKER_ENTRY main
#endif
int NVC_BROKER_ENTRY(int argc, char **argv)
{
	uint64_t report_number;
	int report_fd, report_type;
	socklen_t report_len = sizeof report_type;
	if (argc < 4 || strcmp(argv[1], "--parent-report-fd") || decimal(argv[2], &report_number) ||
	    report_number < 3 || report_number > INT_MAX)
		return 64;
	report_fd = (int)report_number;
	if (getsockopt(report_fd, SOL_SOCKET, SO_TYPE, &report_type, &report_len) ||
	    report_type != SOCK_SEQPACKET)
		return 64;
	argc -= 2;
	argv += 2;
	struct continuous s;
	uint64_t num, now = 0, last = 0, limit = 0, revoke_at = 0;
	int consumer, offset, pipefd[2], status = 0, revoked = 0, sent_term = 0;
	unsigned revoke_ticks = 0, i;
	char *child_argv[112], pid_text[32], lease_text[32], revoke_text[32];
	uint8_t grant[LEASE_SIZE];
	pid_t child;
	int child_pidfd;
	if (argc < 5 || argc > 117 || strcmp(argv[1], "--consumer-fd") || decimal(argv[2], &num) ||
	    num < 3 || num > INT_MAX)
		return 64;
	consumer = (int)num;
	offset = 3;
	if (strcmp(argv[offset], "--"))
		return 64;
	memset(&s, 0, sizeof(s));
	if (continuous_parse(argc - offset, argv + offset, &s) || s.part[0].c.revoke_fd != -1 ||
	    executable_ok(consumer))
		return 64;
	for (i = 0; i < 2; i++) {
		unsigned j;
		for (j = 0; j < 7; j++)
			if (consumer == s.part[i].c.fd[j])
				return 64;
		for (j = 0; j < 2; j++)
			if (consumer == s.part[i].c.vol_dir[j])
				return 64;
	}
	if (consumer == s.epoch.fd[2])
		return 64;
	/* Keep storage and process descriptors distinct. */
	if (broker_roles_distinct(&s, consumer))
		return 64;
	if (safe_stdio(&s))
		return 65;
	if (continuous_bootstrap(&s)) {
		fputs("broker=owner_or_epoch_rejected\n", stderr);
		return 65;
	}
	if (signal(SIGTERM, stop_requested) == SIG_ERR ||
	    signal(SIGINT, stop_requested) == SIG_ERR || pipe2(pipefd, O_CLOEXEC) ||
	    unlock_owner(s.part[0].c.fd[4]))
		return 67;
	int keep[25] = {0, 1, 2}, n = 3;
	for (i = 0; i < 5; i++)
		keep[n++] = s.part[0].c.fd[i];
	keep[n++] = s.part[1].c.fd[0];
	keep[n++] = s.part[1].c.fd[1];
	{
		keep[n++] = s.part[0].c.fd[6];
		for (i = 0; i < 2; i++) {
			keep[n++] = s.part[i].c.vol_dir[0];
			keep[n++] = s.part[i].c.vol_dir[1];
		}
	}
	keep[n++] = s.epoch.fd[2];
	keep[n++] = s.quiesce.fd;
	keep[n++] = pipefd[0];
	keep[n++] = consumer;
	keep[n++] = s.part[0].c.fd[5];
	struct cpb_fd_plan plan;
	if (cpb_fd_plan_prepare(&plan, keep, (unsigned)n))
		return 67;
	pid_t consumer_parent = getpid();
	child = fork();
	if (child < 0)
		return 67;
	if (!child) {
		pid_t parent = consumer_parent;
		if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent)
			_exit(71);
		snprintf(pid_text, sizeof(pid_text), "%ld", (long)parent);
		snprintf(lease_text, sizeof(lease_text), "%d", pipefd[0]);
		snprintf(revoke_text, sizeof(revoke_text), "%d", s.part[0].c.fd[5]);
		child_argv[0] = (char *)"udx-nv-continuous";
		for (i = 1; i < (unsigned)(argc - offset); i++)
			child_argv[i] = argv[(unsigned)offset + i];
		child_argv[argc - offset] = (char *)"--revoke-fd";
		child_argv[argc - offset + 1] = revoke_text;
		child_argv[argc - offset + 2] = NULL;
		for (i = 1; i < (unsigned)(argc - offset - 1); i++) {
			if (!strcmp(child_argv[i], "--supervisor-pid"))
				child_argv[i + 1] = pid_text;
			else if (!strcmp(child_argv[i], "--lease-fd"))
				child_argv[i + 1] = lease_text;
		}
		if (cpb_fd_plan_apply_child(&plan))
			_exit(71);
		for (i = 0; i < (unsigned)n; i++)
			if (keep[i] != consumer && fcntl(keep[i], F_SETFD, 0))
				_exit(71);
		if (fcntl(consumer, F_SETFD, FD_CLOEXEC))
			_exit(71);
		fexecve(consumer, child_argv, environ);
		fprintf(stderr, "broker=consumer_exec_failed errno=%d\n", errno);
		_exit(127);
	}
	close(pipefd[0]);
	child_pidfd = (int)syscall(SYS_pidfd_open, child, 0);
	if (child_pidfd < 0) {
		close(pipefd[1]);
		return 71;
	} /* Revoke only: missing identity stays unresolved upstream. */
	/* The child is waiting on its lease. The parent retains its pidfd;
	 * release the broker's storage descriptors before granting the writer. */
	{
		int keep[] = {
		    0, 1, 2, s.part[0].c.fd[5], pipefd[1], child_pidfd, s.epoch.fd[2], report_fd};
		if (close_except(keep, sizeof(keep) / sizeof(keep[0]))) {
			(void)syscall(SYS_pidfd_send_signal, child_pidfd, SIGTERM, NULL, 0u);
			waitpid(child, NULL, 0);
			return 71;
		}
	}
	/* v3 source must hold this exact live consumer pidfd before any grant.
	 * The child is still waiting on its private lease pipe. */
	if (nvs_report_send(report_fd, child_pidfd, (int)child, s.part[0].c.epoch))
		stopping = 1;
	if (clock_ms(NULL, &now))
		stopping = 1;
	last = now;
	limit = now + s.part[0].c.timeout;
	while (!stopping) {
		struct pollfd p[2] = {{report_fd, POLLIN, 0}, {s.part[0].c.fd[5], POLLIN, 0}};
		char ack;
		if (clock_ms(NULL, &now) || now < last || now >= limit ||
		    getppid() != s.part[0].c.supervisor || nvc_epoch_check(&s.epoch)) {
			stopping = 1;
			break;
		}
		last = now;
		int r = poll(p, 2, 50);
		if ((r < 0 && errno != EINTR) || p[1].revents) {
			stopping = 1;
			break;
		}
		/* A complete queued ACK remains valid when sender closes the one-shot
		 * report channel. Authorization lifetime is the separate upstream lease. */
		if (p[0].revents & POLLIN) {
			if (recv(report_fd, &ack, 1, MSG_TRUNC) != 1 || ack != 'G')
				stopping = 1;
			break;
		}
		if (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			stopping = 1;
			break;
		}
	}
	close(report_fd);
	{
		struct pollfd p = {s.part[0].c.fd[5], POLLIN, 0};
		if (clock_ms(NULL, &now) || now < last || now >= limit ||
		    getppid() != s.part[0].c.supervisor || nvc_epoch_check(&s.epoch) ||
		    poll(&p, 1, 0) < 0 || p.revents)
			stopping = 1;
	}
	last = now;
	if (stopping) {
		close(pipefd[1]);
		(void)syscall(SYS_pidfd_send_signal, child_pidfd, SIGTERM, NULL, 0u);
		waitpid(child, &status, 0);
		return 71;
	}
	memset(grant, 0, sizeof(grant));
	memcpy(grant, "NVOWN101", 8);
	store32(grant + 8, (uint32_t)s.part[0].c.epoch);
	store32(grant + 12, (uint32_t)(s.part[0].c.epoch >> 32));
	store32(grant + 16, (uint32_t)getpid());
	store32(grant + 20, s.part[0].c.policy);
	{
		struct pollfd p = {s.part[0].c.fd[5], POLLIN, 0};
		if (clock_ms(NULL, &now) || now < last || now >= limit ||
		    getppid() != s.part[0].c.supervisor || nvc_epoch_check(&s.epoch) ||
		    poll(&p, 1, 0) < 0 || p.revents || (last = now, clock_ms(NULL, &now)) ||
		    now < last || now >= limit ||
		    write(pipefd[1], grant, sizeof(grant)) != (ssize_t)sizeof(grant))
			stopping = 1;
	}
	last = now;
	if (!stopping) {
		fprintf(stdout, "broker=handoff_complete child=%ld local_data_aliases=closed\n",
			(long)child);
		fflush(stdout);
	}
	for (;;) {
		struct pollfd p[2] = {{revoked ? -1 : s.part[0].c.fd[5], POLLIN, 0},
				      {child_pidfd, POLLIN, 0}};
		pid_t r = waitpid(child, &status, WNOHANG);
		if (r == child)
			break;
		if (r < 0 && errno != EINTR) {
			status = -1;
			break;
		}
		if (clock_ms(NULL, &now) || now < last || getppid() != s.part[0].c.supervisor)
			stopping = 1;
		last = now;
		{
			int result = poll(p, 2, revoked ? 50 : 1000);
			if ((result < 0 && errno != EINTR) || p[0].revents)
				stopping = 1;
		}
		if (stopping && !revoked) {
			close(pipefd[1]);
			revoked = 1;
			revoke_at = now;
		}
		if (revoked)
			++revoke_ticks;
		if (revoked && !sent_term && (now - revoke_at >= 500 || revoke_ticks >= 10)) {
			(void)syscall(SYS_pidfd_send_signal, child_pidfd, SIGTERM, NULL, 0u);
			sent_term = 1;
		}
		if (revoked && (now - revoke_at >= 1000 || revoke_ticks >= 20)) {
			(void)syscall(SYS_pidfd_send_signal, child_pidfd, SIGKILL, NULL, 0u);
			pid_t collected;
			do {
				collected = waitpid(child, &status, 0);
			} while (collected < 0 && errno == EINTR);
			if (collected != child)
				status = -1;
			break;
		}
	}
	if (!revoked)
		close(pipefd[1]);
	fprintf(status ? stderr : stdout,
		"broker=child_finished revoked=%d status=%d no_respawn=1\n", revoked, status);
	fflush(status ? stderr : stdout);
	return status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : 72;
}
