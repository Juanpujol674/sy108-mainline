/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_parent.h"
#include "wr_control.h"
#include "wr_notify.h"
#include "modem_cmdline.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
static uint64_t ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0)
		return 0;
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static uint64_t nonce(void)
{
	uint64_t n = 0;
	if (getrandom(&n, sizeof n, GRND_NONBLOCK) != sizeof n)
		return 0;
	return n;
}
static int regular(int fd, int access)
{
	struct stat st;
	int f = fcntl(fd, F_GETFL);
	return fd < 3 || f < 0 || (f & O_ACCMODE) != access ||
		       (f & (O_APPEND | O_DIRECT | O_PATH | O_NONBLOCK)) || fstat(fd, &st) ||
		       !S_ISREG(st.st_mode) || st.st_uid || st.st_nlink != 1 || (st.st_mode & 0077)
		   ? -1
		   : 0;
}
static int exe(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (fd < 3 || fstat(fd, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & 0111)) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}
static int profile(struct wr_policy *p)
{
	int fd = open("/etc/udx-cpnv-warm/profile", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	char b[4096];
	if (regular(fd, O_RDONLY))
		return -1;
	ssize_t n = pread(fd, b, sizeof b, 0);
	close(fd);
	return n <= 0 || n >= 4096 || wr_policy_parse(b, (size_t)n, p) || wr_policy_validate(p, 0)
		   ? -1
		   : 0;
}
static int platform(unsigned initial)
{
	char b[4096];
	int fd = open("/proc/cmdline", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 3)
		return -1;
	ssize_t n = pread(fd, b, sizeof b - 1, 0);
	close(fd);
	if (n <= 0 || n >= 4095 || memchr(b, 0, (size_t)n))
		return -1;
	b[n] = 0;
	enum mcc_boot_hint hint;
	if (initial > 1 || mcc_boot_hint_parse(b, (size_t)n, &hint) ||
	    (initial ? hint != MCC_BOOT_SHUTDOWN : hint == MCC_BOOT_SHUTDOWN)) {
		fprintf(
		    stderr,
		    "warm_parent=admission stage=boot_modem_binding initial_load=%u expected=%s\n",
		    initial, initial ? "modem=shutdown" : "non-shutdown");
		return -1;
	}
	unsigned found = 0;
	char *save;
	for (char *s = strtok_r(b, " \n", &save); s; s = strtok_r(NULL, " \n", &save))
		if (!strncmp(s, "udx.nv=", 7)) {
			const char *expected = "udx.nv=native-exclusive";
			if (strcmp(s, expected)) {
				fprintf(stderr,
					"warm_parent=admission stage=boot_nv_binding expected=%s "
					"observed=%.63s\n",
					expected, s);
				return -1;
			}
			found++;
		}
	if (found != 1) {
		fprintf(stderr, "warm_parent=admission stage=boot_nv_binding token_count=%u\n",
			found);
		return -1;
	}
	/* PID1 is selected by the initramfs; its executable name is not a CP/NV capability. */
	fd = open("/proc/device-tree/compatible", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 3)
		return -1;
	n = pread(fd, b, sizeof b, 0);
	close(fd);
	if (n <= 0 || n >= 4096)
		return -1;
	found = 0;
	for (size_t off = 0; off < (size_t)n;) {
		size_t len = strnlen(b + off, (size_t)n - off);
		if (len == (size_t)n - off)
			return -1;
		if (!strcmp(b + off, "sprd,sy108"))
			found++;
		off += len + 1;
	}
	return found == 1 ? 0 : -1;
}
static int runtime_owner(int dir, int *owner, int *cp_owner)
{
	*owner = openat(dir, "nv-owner.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	*cp_owner = openat(dir, "cp-owner.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (regular(*owner, O_RDWR) || regular(*cp_owner, O_RDWR) ||
	    flock(*cp_owner, LOCK_EX | LOCK_NB)) {
		if (*owner >= 0)
			close(*owner);
		if (*cp_owner >= 0)
			close(*cp_owner);
		*owner = *cp_owner = -1;
		return -1;
	}
	return 0;
}
static int control_socket(const char *path)
{
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	if (strlen(path) >= sizeof a.sun_path) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(a.sun_path, path);
	if (unlink(a.sun_path) && errno != ENOENT)
		return -1;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 3 || bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 1)) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}
/* A matching receipt is required by the formal shutdown helper. Process exit
 * alone cannot establish that NV quiescence and controller revoke succeeded. */
static int shutdown_receipt(struct wr_parent *p, uint64_t epoch, int result)
{
	char boot[64], line[256];
	ssize_t n = pread(p->boot, boot, sizeof boot - 1, 0);
	if (n != 37 || boot[36] != '\n')
		return -EIO;
	boot[36] = 0;
	int fd = openat(p->run, "shutdown.result.new",
			O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return -errno;
	int length =
	    snprintf(line, sizeof line,
		     "CPNV_SHUTDOWN1 boot=%s pid=%ld epoch=%016llx result=%d nv_drained=%u "
		     "controller_closed=%u\n",
		     boot, (long)getpid(), (unsigned long long)epoch, result, !result, !result);
	int error = write(fd, line, (size_t)length) == length && fsync(fd) == 0 ? 0 : -EIO;
	close(fd);
	if (!error && renameat(p->run, "shutdown.result.new", p->run, "shutdown.result"))
		error = -errno;
	if (!error && fsync(p->run))
		error = -errno;
	return error;
}
/* Metadata-only deployment preflight. This consumes the supplied descriptors
 * in this checking process; a launcher must open a fresh set for --serve.
 * No canonical device opens, signing, lease, fork or service startup occurs. */
int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--help")) {
		puts("udx-cpnv-parent --serve [--postload-profile-fd N] | --request-reload "
		     "[--postload-profile-fd N]\nOptional preloaded-halo-v1 or "
		     "initial-load-halo-v1 inherited-FD profile uses the verified HALO exchange, "
		     "factory, checked featureList and initial initialized-mark (halo-normal-v1). "
		     "Initial load also requires firmware inputs and exact modem=shutdown boot "
		     "hint. No private keys on command line.\nConfigured by the service profile.");
		return 0;
	}
	if ((argc == 2 || argc == 4) && !strcmp(argv[1], "--request-reload")) {
		if (argc == 2)
			return wr_control_client(-1);
		if (strcmp(argv[2], "--postload-profile-fd"))
			return 64;
		char *end;
		errno = 0;
		long value = strtol(argv[3], &end, 10);
		if (errno || !*argv[3] || *end || value < 3 || value > 0x7fffffff)
			return 64;
		return wr_control_client((int)value);
	}
	if ((argc != 2 && argc != 4) || strcmp(argv[1], "--serve") ||
	    (argc == 4 && strcmp(argv[2], "--postload-profile-fd")))
		return 64;
	if (geteuid())
		return 77;
	umask(077);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, nvs_cancel_handler);
	signal(SIGINT, nvs_cancel_handler);
	struct wr_parent p = {0};
	wr_postload_init(&p.postload);
	if (argc == 4) {
		char *end;
		errno = 0;
		long profilefd = strtol(argv[3], &end, 10);
		if (errno || !*argv[3] || *end || profilefd < 3 || profilefd > 0x7fffffff)
			return 64;
		int e = wr_postload_load((int)profilefd, &p.postload);
		close((int)profilefd);
		if (e) {
			fprintf(stderr, "warm_parent=postload_input error=%d\n", e);
			return 77;
		}
	}
	rt_child_init(&p.current.notify);
	p.notify_exe = -1;
	p.current.cp.lease = p.current.cp.phase = p.current.cp.report = p.current.cp.continuation =
	    p.current.cp.pidfd = -1;
	p.current.nv.report = p.current.nv.lease = p.current.nv.quiesce =
	    p.current.nv.source_pidfd = -1;
	wr_roles_init(&p.current.roles, 0);
	for (unsigned i = 0; i < 4; i++)
		p.current.epoch.fd[i] = -1;
	int admission = profile(&p.policy);
	if (admission) {
		fprintf(stderr, "warm_parent=admission stage=profile error=%d\n", admission);
		return 77;
	}
	fprintf(stdout, "warm_parent=starting storage=%s initial_load=%u\n", "ubi",
		p.postload.initial_load);
	fflush(stdout);
	if ((admission = platform(p.postload.initial_load))) {
		fprintf(stderr, "warm_parent=admission stage=platform error=%d\n", admission);
		return 77;
	}
	if ((admission = wr_notify_policy(&p))) {
		fprintf(stderr, "warm_parent=admission stage=notify_policy error=%d\n", admission);
		return 77;
	}
	p.run = open("/run/udx-cpnv-warm", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	struct stat st;
	if (p.run < 3 || fstat(p.run, &st) || !S_ISDIR(st.st_mode) || st.st_uid ||
	    (st.st_mode & 0777) != 0700) {
		fprintf(stderr,
			"warm_parent=admission stage=runtime_directory error=invalid-metadata\n");
		return 77;
	}
	/* The live owner lock serializes restart and socket replacement. */
	if (runtime_owner(p.run, &p.owner, &p.cp_owner))
		return 77;
	int socketfd = control_socket("/run/udx-cpnv-warm/control.sock");
	if (socketfd < 3) {
		fprintf(stderr, "warm_parent=admission stage=control_socket errno=%d\n", errno);
		return 77;
	}
	p.boot = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	p.self = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
	p.controller = exe("/usr/sbin/udx-modem-control-warm");
	p.broker = exe("/usr/libexec/udx-nv-broker-v4");
	p.consumer = exe("/usr/libexec/udx-nv-continuous-v4");
	p.notify_exe = p.notify_enabled ? exe("/usr/sbin/udx-refnotifyd") : -1;
	if (p.boot < 3 || p.self < 3 || p.controller < 3 || p.broker < 3 || p.consumer < 3 ||
	    (p.notify_enabled && p.notify_exe < 3)) {
		fprintf(stderr,
			"warm_parent=admission stage=executable_binding boot=%u self=%u "
			"controller=%u broker=%u consumer=%u notify=%u\n",
			p.boot >= 3, p.self >= 3, p.controller >= 3, p.broker >= 3, p.consumer >= 3,
			!p.notify_enabled || p.notify_exe >= 3);
		return 77;
	}
	int error = wr_parent_start(&p, nonce());
	if (!error) {
		fprintf(stdout,
			"baseband_session=ready storage=%s halo_verified=%u nv_live=%d "
			"notify_enabled=%d\n",
			"ubi", p.postload.ready, p.current.nv.live, p.notify_enabled);
		fflush(stdout);
	}
	uint64_t last = ms();
	while (!error && !nvs_cancelled) {
		uint64_t now = ms();
		if (!now || now < last) {
			error = -EIO;
			break;
		}
		last = now;
		if ((error = wr_parent_check(&p)))
			break;
		struct pollfd ready[WR_ROLE_COUNT + 2];
		unsigned count = 0;
		ready[count++] = (struct pollfd){socketfd, POLLIN, 0};
		if (p.postload.enabled)
			ready[count++] = (struct pollfd){p.postload.events, POLLIN, 0};
		for (unsigned i = 0; i < WR_ROLE_COUNT; i++)
			if (p.current.roles.process[i].registered)
				ready[count++] =
				    (struct pollfd){p.current.roles.process[i].pidfd, POLLIN, 0};
		int rc = poll(ready, count, -1);
		if (rc < 0 && errno != EINTR) {
			error = -errno;
			break;
		}
		if (ready[0].revents & POLLIN) {
			int peer = accept4(socketfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
			if (peer < 3)
				continue;
			int action_error = wr_control_dispatch(&p, peer, nonce(), 0);
			error = p.uncertain ? (action_error ? action_error : -EIO) : 0;
			close(peer);
		}
	}
	if (!error && nvs_cancelled && p.started && !p.uncertain) {
		nvs_cancelled = 0;
		uint64_t now = ms(), epoch = p.current.roles.nonce;
		error = now ? wr_parent_shutdown(&p, now + 60000) : -EIO;
		int receipt = shutdown_receipt(&p, epoch, error);
		if (!error)
			error = receipt;
	}
	for (;;) {
		uint64_t now = ms();
		if (now && !wr_parent_cancel(&p, now + 5000))
			break;
		fprintf(stderr, "warm_parent=UNCERTAIN live_pidfds_retained=1 no_reload=1\n");
		poll(NULL, 0, 1000);
	}
	close(socketfd);
	unlink("/run/udx-cpnv-warm/control.sock");
	return error ? 74 : 0;
}
