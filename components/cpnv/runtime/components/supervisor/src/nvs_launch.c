/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "nvs_launch.h"
#include "cpnv_fd_plan.h"
#include "nvs_report.h"
#include "wr_roles.h"
#include <sys/socket.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
volatile sig_atomic_t nvs_cancelled;
void nvs_cancel_handler(int s)
{
	(void)s;
	nvs_cancelled = 1;
}
#define stopped nvs_cancelled
#define stopping nvs_cancel_handler
static uint64_t millis(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return 0;
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static int cp_valid(const struct nvs_launch *c)
{
	struct pollfd p[2] = {{c->parent_lease, POLLIN, 0}, {c->cp_owner_pidfd, POLLIN, 0}};
	if (c->parent_lease < 3 || getppid() != c->unified_parent || poll(p, 2, 0) < 0 ||
	    p[0].revents || p[1].revents)
		return -1;
	return 0;
}
static int grant_guard(struct nvs_launch *c, uint64_t end, uint64_t *last)
{
	uint64_t now = millis();
	if (stopped || !now || now < *last || now >= end)
		return -1;
	*last = now;
	if (cp_valid(c) || nvc_epoch_check(&c->epoch))
		return -1;
	now = millis();
	if (stopped || !now || now < *last || now >= end)
		return -1;
	*last = now;
	return 0;
}
static int parent_ack(struct nvs_launch *c, uint64_t end)
{
	uint64_t last = millis();
	if (!last)
		return -1;
	for (;;) {
		if (grant_guard(c, end, &last))
			return -1;
		struct pollfd p = {c->parent_report, POLLIN, 0};
		int rc = poll(&p, 1, 25);
		if (rc < 0 && errno != EINTR)
			return -1;
		if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
			return -1;
		if (p.revents & POLLIN) {
			char b;
			rc = recv(c->parent_report, &b, 1, MSG_TRUNC);
			return rc == 1 && b == 'G' && !grant_guard(c, end, &last) ? 0 : -1;
		}
	}
}
static void store32(unsigned char *p, uint32_t n)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (unsigned char)(n >> (i * 8));
}
static int signal_child(int fd, int sig)
{
	int r = (int)syscall(SYS_pidfd_send_signal, fd, sig, NULL, 0);
	return r && errno != ESRCH ? -1 : 0;
}
int nvs_run_broker(struct nvs_launch *c)
{
	char *av[120], nums[52][32];
	unsigned n = 0, v = 0, k = 0;
	int keep[34], lease[2], report[2], status = 0, pidfd = -1, writer_pidfd = -1;
	pid_t pid;
	uint64_t last, now;
	int granted = 0, revoked = 0, reaped = 0;
	if (!c || !c->nonce || c->broker < 3 || c->consumer < 3 || c->old_pidfd == 0)
		return 64;
	if (pipe2(lease, O_CLOEXEC | O_NONBLOCK))
		return 74;
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, report)) {
		close(lease[0]);
		close(lease[1]);
		return 74;
	}
#define STR(a)                                                                                     \
	do {                                                                                       \
		av[n++] = (char *)(a);                                                             \
	} while (0)
#define NUM(key, x)                                                                                \
	do {                                                                                       \
		STR(key);                                                                          \
		snprintf(nums[v], sizeof nums[v], "%llu", (unsigned long long)(x));                \
		STR(nums[v++]);                                                                    \
	} while (0)
	STR("udx-nv-continuous-broker");
	NUM("--parent-report-fd", report[1]);
	NUM("--consumer-fd", c->consumer);
	STR("--");
	STR("--profile");
	STR("v101-stock-compatible-ubi-v1");
	NUM("--partition", 1);
	NUM("--primary-fd", c->devices.volume[0]);
	NUM("--backup-fd", c->devices.volume[1]);
	NUM("--size", 7340032);
	NUM("--checksum-bits", 64);
	NUM("--runtime-primary-fd", c->devices.volume[2]);
	NUM("--runtime-backup-fd", c->devices.volume[3]);
	NUM("--runtime-size", 9437184);
	NUM("--runtime-checksum-bits", 64);
	NUM("--rx-fd", c->transport);
	NUM("--tx-fd", c->transport);
	NUM("--owner-fd", c->owner);
	NUM("--lease-fd", lease[0]);
	NUM("--write-policy", c->profile.write_policy);
	NUM("--epoch", c->nonce);
	NUM("--supervisor-pid", getpid());
	NUM("--timeout-ms", c->profile.timeout_ms);
	{
		NUM("--ubi-device-dir-fd", c->devices.ubi_dir);
		NUM("--primary-dir-fd", c->devices.vol_dir[0]);
		NUM("--backup-dir-fd", c->devices.vol_dir[1]);
		NUM("--runtime-primary-dir-fd", c->devices.vol_dir[2]);
		NUM("--runtime-backup-dir-fd", c->devices.vol_dir[3]);
		NUM("--primary-rdev", c->devices.volume_rdev[0]);
		NUM("--backup-rdev", c->devices.volume_rdev[1]);
		NUM("--runtime-primary-rdev", c->devices.volume_rdev[2]);
		NUM("--runtime-backup-rdev", c->devices.volume_rdev[3]);
		NUM("--transport-rdev", c->devices.transport_rdev);
	}
	NUM("--session-nv-fd", c->epoch.fd[2]);
	NUM("--generation-nv", c->epoch.generation[2]);
	NUM("--session-armed-mask", 4);
	NUM("--quiesce-fd", c->quiesce);
	NUM("--unified-parent-pid", c->unified_parent);
	av[n] = NULL;
	if (n >= 120 || v >= 52) {
		close(lease[0]);
		close(lease[1]);
		close(report[0]);
		close(report[1]);
		return 64;
	}
#undef STR
#undef NUM
	if (stopped || signal(SIGPIPE, SIG_IGN) == SIG_ERR ||
	    signal(SIGTERM, stopping) == SIG_ERR || signal(SIGINT, stopping) == SIG_ERR ||
	    (nvc_epoch_check(&c->epoch) || cp_valid(c))) {
		close(lease[0]);
		close(lease[1]);
		close(report[0]);
		close(report[1]);
		return 77;
	}
	keep[k++] = c->quiesce;
	keep[k++] = 0;
	keep[k++] = 1;
	keep[k++] = 2;
	keep[k++] = report[1];
	keep[k++] = c->consumer;
	keep[k++] = c->broker;
	keep[k++] = c->owner;
	keep[k++] = lease[0];
	keep[k++] = c->transport;
	keep[k++] = c->epoch.fd[2];
	for (unsigned i = 0; i < 4; i++) {
		keep[k++] = c->devices.volume[i];
		keep[k++] = c->devices.vol_dir[i];
	}
	keep[k++] = c->devices.ubi_dir;
	struct cpb_fd_plan plan;
	int planning = cpb_fd_plan_prepare(&plan, keep, k);
	if (planning) {
		close(lease[0]);
		close(lease[1]);
		close(report[0]);
		close(report[1]);
		fprintf(stderr, "broker_fd_plan_error=%d\n", planning);
		return 74;
	}
	pid_t broker_parent = getpid();
	pid = fork();
	if (pid < 0) {
		close(lease[0]);
		close(lease[1]);
		close(report[0]);
		close(report[1]);
		return 74;
	}
	if (!pid) {
		pid_t parent = broker_parent;
		if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent ||
		    cpb_fd_plan_apply_child(&plan))
			_exit(71);
		for (unsigned i = 0; i < k; i++)
			if (fcntl(keep[i], F_SETFD, keep[i] == c->broker ? FD_CLOEXEC : 0))
				_exit(71);
		char *env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL};
		fexecve(c->broker, av, env);
		_exit(127);
	}
	close(c->quiesce);
	c->quiesce = -1;
	close(lease[0]);
	close(report[1]);
	pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
	if (pidfd < 0) {
		close(report[0]);
		close(lease[1]); /* Missing identity: revoke lease; parent retains unresolved
				    descendants. */
		return 74;
	}
	/* Source is the last parent data owner. CLOSE, not just CLOEXEC, precedes
	 * grant. */
	for (unsigned i = 0; i < 4; i++) {
		close(c->devices.volume[i]);
		c->devices.volume[i] = -1;
	}
	close(c->transport);
	c->transport = -1;
	uint64_t handshake_last = millis(), handshake_end = handshake_last + c->profile.timeout_ms;
	int broker_ok = 0;
	while (handshake_last && !stopped) {
		uint64_t observed = millis();
		if (!observed || observed < handshake_last || observed >= handshake_end ||
		    cp_valid(c) || nvc_epoch_check(&c->epoch))
			break;
		handshake_last = observed;
		if (!nvs_pidfd_matches(pidfd, c->broker, pid)) {
			broker_ok = 1;
			break;
		}
		poll(NULL, 0, 5);
	}
	if (!broker_ok || wr_role_send(c->parent_report, WR_BROKER, pidfd, pid, c->nonce) ||
	    parent_ack(c, handshake_end))
		revoked = 1;
	unsigned char grant[32] = {0};
	memcpy(grant, "NVOWN101", 8);
	store32(grant + 8, (uint32_t)c->nonce);
	store32(grant + 12, (uint32_t)(c->nonce >> 32));
	store32(grant + 16, (uint32_t)getpid());
	store32(grant + 20, c->profile.write_policy);
	last = millis();
	if (revoked || !last || grant_guard(c, handshake_end, &last) ||
	    write(lease[1], grant, sizeof grant) != (ssize_t)sizeof grant ||
	    grant_guard(c, handshake_end, &last))
		revoked = 1;
	else {
		/* Bootstrap grant cannot reach the consumer until we ACK the real pidfd. */
		uint64_t end = last + c->profile.timeout_ms;
		int writer_pid;
		for (;;) {
			struct pollfd p[2] = {{report[0], POLLIN, 0}, {pidfd, POLLIN, 0}};
			now = millis();
			if (stopped || !now || now < last || now >= end)
				break;
			last = now;
			int r = poll(p, 2, 50);
			if ((r < 0 && errno != EINTR) || p[1].revents ||
			    (nvc_epoch_check(&c->epoch) || cp_valid(c)))
				break;
			if (p[0].revents) {
				writer_pidfd = nvs_report_receive(report[0], c->consumer, c->nonce,
								  &writer_pid);
				break;
			}
		}
		now = millis();
		if (writer_pidfd >= 3 && (wr_role_send(c->parent_report, WR_CONSUMER, writer_pidfd,
						       writer_pid, c->nonce) ||
					  parent_ack(c, end)))
			revoked = 1;
		if (revoked || writer_pidfd < 3 || grant_guard(c, end, &last))
			revoked = 1;
		else {
			ssize_t accepted = send(report[0], "G", 1, MSG_NOSIGNAL);
			granted = accepted == 1;
			if (!granted || grant_guard(c, end, &last))
				revoked = 1;
		}
		now = last;
	}
	close(report[0]);
	if (revoked) {
		close(lease[1]);
		lease[1] = -1;
	}
	for (;;) {
		struct pollfd watched[3] = {{pidfd, POLLIN, 0},
					    {c->parent_lease, POLLIN, 0},
					    {c->cp_owner_pidfd, POLLIN, 0}};
		pid_t w;
		int r = poll(watched, 3, -1);
		if (r > 0 && watched[0].revents) {
			do {
				w = waitpid(pid, &status, 0);
			} while (w < 0 && errno == EINTR);
			reaped = w == pid;
			break;
		}
		if ((r < 0 && errno != EINTR) || stopped || watched[1].revents ||
		    watched[2].revents)
			revoked = 1;
		if (revoked) {
			if (lease[1] >= 0) {
				close(lease[1]);
				lease[1] = -1;
			}
			struct pollfd child = {pidfd, POLLIN, 0};
			if (poll(&child, 1, 1500) == 0) {
				signal_child(pidfd, SIGTERM);
				poll(&child, 1, 500);
			}
			if (!(child.revents & POLLIN))
				signal_child(pidfd, SIGKILL);
			do {
				w = waitpid(pid, &status, 0);
			} while (w < 0 && errno == EINTR);
			reaped = w == pid;
			break;
		}
	}
	if (lease[1] >= 0)
		close(lease[1]);
	close(pidfd);
	if (writer_pidfd >= 3) {
		struct pollfd p = {writer_pidfd, POLLIN, 0};
		if (poll(&p, 1, 0) == 0)
			signal_child(writer_pidfd, SIGTERM);
		/* Do not publish quiescence merely because the intermediary broker died.
		 * Uninterruptible storage may outlive SIGTERM: retain source/fence until the
		 * exact writer exits. Outer controller must treat timeout as UNCERTAIN. */
		while (poll(&p, 1, 1000) == 0)
			fprintf(stderr,
				"nv_source=writer_still_alive state=UNCERTAIN no_quiescence=1\n");
		if (!(p.revents & POLLIN)) {
			for (;;)
				poll(NULL, 0, 1000);
		}
		close(writer_pidfd);
	}
	/* The consumer exits zero only after its successful quiesce receipt; the
	 * broker propagates that status even if its upstream lease closes meanwhile. */
	if (granted && reaped && status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		fprintf(stdout,
			"nv_source=terminated grant_sent=1 revoked=%d child_status=0 "
			"outcome=QUIESCED\n",
			revoked);
		return 0;
	}
	fprintf(stderr,
		"nv_source=terminated grant_sent=%d revoked=%d child_status=%d "
		"outcome=UNCERTAIN_OR_UNCONFIRMED no_auto_restart=1\n",
		granted, revoked, status);
	return granted ? 74 : 77;
}
