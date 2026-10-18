/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_quiesce.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
static void put(unsigned char *p, uint64_t v, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}
static uint64_t get(const unsigned char *p, unsigned n)
{
	uint64_t v = 0;
	for (unsigned i = 0; i < n; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}
int wr_quiesce_open(struct wr_quiesce *q)
{
	struct ucred peer;
	int type;
	socklen_t n = sizeof peer, m = sizeof type;
	struct stat st;
	if (!q || q->fd < 3 || q->parent < 2 || !q->nonce || fstat(q->fd, &st) ||
	    !S_ISSOCK(st.st_mode) || getsockopt(q->fd, SOL_SOCKET, SO_TYPE, &type, &m) ||
	    type != SOCK_SEQPACKET || getsockopt(q->fd, SOL_SOCKET, SO_PEERCRED, &peer, &n) ||
	    n != sizeof peer || peer.pid != q->parent || peer.uid != 0)
		return -EPERM;
	return 0;
}
static int report(int fd, const char magic[8], uint64_t nonce, unsigned flags)
{
	unsigned char b[32] = {0};
	memcpy(b, magic, 8);
	put(b + 8, nonce, 8);
	put(b + 16, (unsigned)getpid(), 4);
	put(b + 20, flags, 4);
	return send(fd, b, sizeof b, MSG_NOSIGNAL | MSG_DONTWAIT) == sizeof b ? 0 : -EIO;
}
int wr_quiesce_live(struct wr_quiesce *q)
{
	return !q || q->requested ? -EINVAL : report(q->fd, "NVLIVE04", q->nonce, 0u);
}
int wr_quiesce_send_request(int fd, uint64_t nonce)
{
	return report(fd, "NVQUIE04", nonce, 0);
}
int wr_quiesce_request(struct wr_quiesce *q)
{
	unsigned char b[32];
	if (!q || q->requested)
		return -EINVAL;
	ssize_t n = recv(q->fd, b, sizeof b, MSG_DONTWAIT | MSG_TRUNC);
	if (n != 32 || memcmp(b, "NVQUIE04", 8) || get(b + 8, 8) != q->nonce ||
	    get(b + 16, 4) != (unsigned)q->parent || get(b + 20, 4) || get(b + 24, 8))
		return -EPROTO;
	q->requested = 1;
	return 1;
}
int wr_quiesce_done(struct wr_quiesce *q, unsigned dirty)
{
	return !q || !q->requested || dirty > 1
		   ? -EINVAL
		   : report(q->fd, "NVDONE04", q->nonce, 1u | (dirty ? 2u : 0u));
}
int wr_quiesce_receive(int fd, const char magic[8], uint64_t nonce, pid_t pid, unsigned *dirty)
{
	unsigned char b[32];
	ssize_t n = recv(fd, b, sizeof b, MSG_DONTWAIT | MSG_TRUNC);
	if (!dirty || n != 32 || memcmp(b, magic, 8) || get(b + 8, 8) != nonce ||
	    get(b + 16, 4) != (unsigned)pid || get(b + 24, 8))
		return -EPROTO;
	unsigned flags = (unsigned)get(b + 20, 4), want = 0u;
	if (!memcmp(magic, "NVDONE04", 8)) {
		if ((flags & ~2u) != (want | 1u))
			return -EPROTO;
	} else if (memcmp(magic, "NVLIVE04", 8) || flags != want)
		return -EPROTO;
	*dirty = (flags & 2u) != 0;
	return 0;
}
