/* SPDX-License-Identifier: GPL-2.0-only
 * Native original-UBI CP/NV launcher with bundled common HALO parameters.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const unsigned char *data, size_t size)
{
	size_t off = 0;
	while (off < size) {
		ssize_t n = write(fd, data + off, size - off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}
static int private_directory(const char *path, int create)
{
	if (create && mkdir(path, 0700) && errno != EEXIST)
		return -1;
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	struct stat st;
	if (fd < 3)
		return -1;
	if (fstat(fd, &st) || st.st_uid) {
		close(fd);
		errno = EPERM;
		return -1;
	}
	if ((st.st_mode & 0777) != 0700 && (!create || fchmod(fd, 0700))) {
		close(fd);
		errno = EPERM;
		return -1;
	}
	return fd;
}
static int private_file(const char *path, off_t size)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	struct stat st;
	if (fd < 3)
		return -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid || st.st_nlink != 1 ||
	    (st.st_mode & 0077) || (size >= 0 && st.st_size != size)) {
		close(fd);
		errno = EPERM;
		return -1;
	}
	return fd;
}
static int prepare_runtime(void)
{
	int fd = private_directory("/run/udx-cpnv-warm", 1);
	if (fd < 3)
		return -1;
	close(fd);
	puts("baseband runtime: control directory ready");
	return 0;
}
static int launch(void)
{
	int run = private_directory("/run/udx-cpnv-warm", 1);
	if (run < 3)
		return -1;
	close(run);
	int key = private_file("/etc/udx-cpnv-warm/halo-private.bin", 256);
	int verify = private_file("/etc/udx-cpnv-warm/halo-verification.bin", 136);
	int dev = open("/dev", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int status = open("/sys/class/spipe/spipe_lte2/session", O_RDONLY | O_CLOEXEC);
	int halo = open("/sys/class/spipe/spipe_lte3/session", O_RDONLY | O_CLOEXEC);
	if (key < 3 || verify < 3 || dev < 3 || status < 3 || halo < 3)
		return -1;
	int request[2], reply[2];
	if (pipe2(request, O_CLOEXEC | O_NONBLOCK) || pipe2(reply, O_CLOEXEC | O_NONBLOCK))
		return -1;
	char path[] = "/run/udx-cpnv-warm/postload-profile-XXXXXX";
	int out = mkstemp(path);
	if (out < 3)
		return -1;
	char text[512];
	int length = snprintf(text, sizeof text,
			      "profile=initial-load-halo-v1\nap_policy=halo-normal-v1\n"
			      "devdir_fd=%d\nstatus_session_fd=%d\nhalo_session_fd=%d\n"
			      "provider_rx_fd=%d\nprovider_tx_fd=%d\nverification_params_fd=%d\n"
			      "alive_ms=60000\nhalo_ms=120000\nfactory_tail=0\n",
			      dev, status, halo, reply[0], request[1], verify);
	if (length <= 0 || length >= (int)sizeof text ||
	    write_all(out, (unsigned char *)text, (size_t)length) || fsync(out))
		return -1;
	close(out);
	int profile = open(path, O_RDONLY | O_CLOEXEC);
	int saved_errno = errno;
	unlink(path);
	if (profile < 3) {
		errno = saved_errno;
		return -1;
	}
	pid_t parent = getpid(), child = fork();
	if (child < 0)
		return -1;
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent)
			_exit(71);
		int original[] = {request[0], reply[1], key};
		/* Move through high temporary FDs to avoid clobbering another role. */
		int saved[3];
		for (unsigned i = 0; i < 3; i++) {
			saved[i] = fcntl(original[i], F_DUPFD_CLOEXEC, 100);
			if (saved[i] < 0)
				_exit(71);
		}
		for (unsigned i = 0; i < 3; i++)
			if (dup2(saved[i], 3 + (int)i) < 0)
				_exit(71);
		if (syscall(SYS_close_range, 6u, ~0u, 0))
			_exit(71);
		execl("/usr/sbin/udx-halo-provider", "udx-halo-provider", "--request-fd", "3",
		      "--reply-fd", "4", "--key-fd", "5", (char *)NULL);
		_exit(127);
	}
	close(key);
	close(request[0]);
	close(reply[1]);
	int keep[] = {dev, status, halo, reply[0], request[1], verify, profile};
	for (unsigned i = 0; i < sizeof keep / sizeof keep[0]; i++)
		if (fcntl(keep[i], F_SETFD, 0))
			return -1;
	char fd[24];
	snprintf(fd, sizeof fd, "%d", profile);
	puts("baseband: starting controller, HALO and NV services");
	fflush(stdout);
	execl("/usr/sbin/udx-cpnv-parent", "udx-cpnv-parent", "--serve", "--postload-profile-fd",
	      fd, (char *)NULL);
	return -1;
}
int main(int argc, char **argv)
{
	if (argc != 2 || strcmp(argv[1], "start")) {
		puts("udx-cpnv-start start (native original-UBI NV, firmware profile)");
		return 64;
	}
	if (geteuid())
		return 77;
	umask(077);
	if (prepare_runtime() || launch()) {
		perror("native baseband startup");
		return 1;
	}
	return 0;
}
