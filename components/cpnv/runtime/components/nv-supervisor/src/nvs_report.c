/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "nvs_report.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static void put(unsigned char *p, uint64_t x, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		p[i] = (unsigned char)(x >> (i * 8));
}
static uint64_t get(const unsigned char *p, unsigned n)
{
	uint64_t v = 0;
	for (unsigned i = 0; i < n; i++)
		v |= (uint64_t)p[i] << (i * 8);
	return v;
}
int nvs_report_send(int fd, int pidfd, int pid, uint64_t nonce)
{
	unsigned char payload[32] = {0};
	char control[CMSG_SPACE(sizeof(int))] = {0};
	struct iovec iov = {payload, sizeof payload};
	struct msghdr msg = {.msg_iov = &iov,
			     .msg_iovlen = 1,
			     .msg_control = control,
			     .msg_controllen = sizeof control};
	memcpy(payload, "NVCHILD3", 8);
	put(payload + 8, nonce, 8);
	put(payload + 16, (unsigned)pid, 4);
	struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &pidfd, sizeof pidfd);
	return sendmsg(fd, &msg, MSG_NOSIGNAL) == sizeof payload ? 0 : -1;
}
int nvs_pidfd_matches(int fd, int exe, int pid)
{
	char path[64], info[256];
	ssize_t n;
	(void)exe;
	if (fd < 3 || pid < 2)
		return -1;
	struct pollfd p = {fd, POLLIN, 0};
	if (syscall(SYS_pidfd_send_signal, fd, 0, NULL, 0) || poll(&p, 1, 0) != 0)
		return -1;
	snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
	int f = open(path, O_RDONLY | O_CLOEXEC);
	if (f < 0)
		return -1;
	n = read(f, info, sizeof info - 1);
	close(f);
	if (n <= 0)
		return -1;
	info[n] = 0;
	char *s = strstr(info, "\nPid:\t"), *end;
	if (!s)
		return -1;
	errno = 0;
	long observed = strtol(s + 6, &end, 10);
	if (errno || observed != pid || *end != '\n')
		return -1;
	if (syscall(SYS_pidfd_send_signal, fd, 0, NULL, 0) || poll(&p, 1, 0) != 0)
		return -1;
	return 0;
}
int nvs_report_receive(int fd, int exe, uint64_t nonce, int *pid_out)
{
	unsigned char payload[32];
	char control[CMSG_SPACE(sizeof(int) * 4)] = {0};
	struct iovec iov = {payload, sizeof payload};
	struct msghdr msg = {.msg_iov = &iov,
			     .msg_iovlen = 1,
			     .msg_control = control,
			     .msg_controllen = sizeof control};
	int got = -1, count = 0, bad = 0;
	ssize_t n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
	if (n < 0)
		return -1;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
		    c->cmsg_len < CMSG_LEN(0)) {
			bad = 1;
			continue;
		}
		size_t bytes = c->cmsg_len - CMSG_LEN(0);
		if (bytes % sizeof(int))
			bad = 1;
		for (size_t i = 0; i < bytes / sizeof(int); i++) {
			int f;
			memcpy(&f, (char *)CMSG_DATA(c) + i * sizeof(int), sizeof f);
			if (count++ == 0)
				got = f;
			else
				close(f);
		}
	}
	if (bad || n != 32 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || count != 1 || got < 3 ||
	    memcmp(payload, "NVCHILD3", 8) || get(payload + 8, 8) != nonce)
		goto fail;
	for (unsigned i = 20; i < 32; i++)
		if (payload[i])
			goto fail;
	uint64_t pid = get(payload + 16, 4);
	if (pid < 2 || pid > 2147483647)
		goto fail;
	if (nvs_pidfd_matches(got, exe, (int)pid))
		goto fail;
	*pid_out = (int)pid;
	return got;
fail:
	if (got >= 0)
		close(got);
	return -1;
}
