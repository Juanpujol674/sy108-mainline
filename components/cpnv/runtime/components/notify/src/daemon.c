/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "refnotify_transport.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <unistd.h>
static volatile sig_atomic_t cancelled;
static void cancel(int sig)
{
	(void)sig;
	cancelled = 1;
}
static int nonblock(int fd)
{
	int f = fcntl(fd, F_GETFL);
	return f < 0 ? -errno : (f & O_NONBLOCK) ? 0 : -EPERM;
}
static int socket_type(int fd, int type)
{
	int n;
	socklen_t l = sizeof n;
	return getsockopt(fd, SOL_SOCKET, SO_TYPE, &n, &l) || l != sizeof n || n != type
		   ? -EPROTOTYPE
		   : 0;
}
static void put64(uint8_t *p, uint64_t v)
{
	for (unsigned i = 0; i < 8; ++i)
		p[i] = (uint8_t)(v >> (8u * i));
}
static int session_ok(const struct rt_config *c)
{
	struct rt_snapshot s;
	int r = rt_read_session(c->session, &s);
	return r ? r : s.generation != c->generation ? -ESTALE : 0;
}
/* Every I/O operation is preceded by a lease/session check. A simultaneous
 * revoke can still race an already entered syscall; parent must wait for the
 * actual child exit before STOP/reload. Kernel FD generation is the final fence. */
static int guard(const struct rt_config *c)
{
	struct pollfd p = {c->lease, POLLIN, 0};
	int r;
	if (cancelled || getppid() != c->parent)
		return -ECANCELED;
	r = poll(&p, 1, 0);
	if (r < 0)
		return errno == EINTR ? -ECANCELED : -errno;
	if (r)
		return -ECANCELED; /* Lease bytes after GRANT or peer HUP revoke. */
	return session_ok(c);
}
struct runtime {
	const struct rt_config *cfg;
	struct rn_policy policy;
	struct rn_ops ops;
	struct rn_linux linux;
	struct rn_hub hub;
	uint8_t tx[RN_MAX_REPLY];
	size_t tx_len, tx_off;
	unsigned frames;
};
static int dispatch(void *v, const uint8_t *b, size_t n)
{
	struct runtime *r = v;
	struct rn_result out;
	int error = guard(r->cfg);
	if (error)
		return error;
	error = rn_dispatch(b, n, &r->policy, &r->ops, &r->linux, &out);
	r->frames++;
	if (error) {
		fprintf(stderr, "refnotify=reject command=%u local_errno=%d wire_ack=none\n",
			out.command, -error);
		/* Policy/unsupported and field errors consume exactly this framed
		 * request without inventing a wire error format. Framing errors close. */
		return error == -EMSGSIZE || error == -EBADMSG || error == -EINVAL ||
			       error == -EPROTO
			   ? error
			   : 0;
	}
	if (out.reply_len) {
		if (r->tx_len)
			return -ENOBUFS;
		memcpy(r->tx, out.reply, out.reply_len);
		r->tx_len = out.reply_len;
		r->tx_off = 0;
	}
	if (out.association_valid) {
		int evicted[RN_CLIENTS];
		error = rn_hub_publish(&r->hub, r->cfg->epoch, out.association, evicted);
		if (error)
			return error;
		for (size_t i = 0; i < RN_CLIENTS; ++i)
			if (evicted[i] >= 0)
				close(evicted[i]);
	}
	return 0;
}
static int validate(const struct rt_config *c)
{
	struct stat st;
	struct statfs fs;
	if (!c || c->notify < 3 || c->session < 3 || c->lease < 3 || c->report < 3 || !c->epoch ||
	    c->parent < 2 || c->io_timeout_ms < 100 || c->io_timeout_ms > 60000)
		return -EINVAL;
	int fds[6] = {c->notify, c->session, c->lease, c->report, c->rtc, c->listener};
	for (unsigned i = 0; i < 6; ++i)
		for (unsigned j = 0; j < i; ++j)
			if (fds[i] >= 0 && fds[i] == fds[j])
				return -EINVAL;
	if (nonblock(c->notify) || nonblock(c->lease) || nonblock(c->report) ||
	    socket_type(c->lease, SOCK_SEQPACKET) || socket_type(c->report, SOCK_SEQPACKET))
		return -EPERM;
	if (fstat(c->notify, &st))
		return -errno;
	if (!S_ISCHR(st.st_mode) || st.st_uid || (st.st_mode & 0002))
		return -EPERM;
	if (fstat(c->session, &st) || !S_ISREG(st.st_mode) || fstatfs(c->session, &fs))
		return -EPERM;
	if (st.st_uid || (st.st_mode & 0222) || fs.f_type != 0x62656572L)
		return -EPERM;
	if ((fcntl(c->session, F_GETFL) & O_ACCMODE) != O_RDONLY)
		return -EPERM;
	if (c->rtc >= 0) {
		if (fstat(c->rtc, &st) || (fcntl(c->rtc, F_GETFL) & O_ACCMODE) != O_RDONLY)
			return -EPERM;
		if (!S_ISCHR(st.st_mode) || st.st_uid || (st.st_mode & 0002))
			return -EPERM;
	}
	if (c->listener >= 0) {
		int listening;
		socklen_t size = sizeof listening;
		if (nonblock(c->listener) || socket_type(c->listener, SOCK_STREAM) ||
		    getsockopt(c->listener, SOL_SOCKET, SO_ACCEPTCONN, &listening, &size) ||
		    size != sizeof listening || listening != 1)
			return -EPROTOTYPE;
	}
	return session_ok(c);
}
static int handshake(const struct rt_config *c)
{
	uint8_t b[RT_HANDSHAKE_SIZE] = {0}, expected[RT_HANDSHAKE_SIZE] = {0};
	uint64_t start = rt_now_ms(), now;
	ssize_t n;
	if (!start)
		return -EIO;
	memcpy(b, "RNRDY001", 8);
	put64(b + 8, c->epoch);
	rn_put_le32(b + 16, c->generation);
	rn_put_le32(b + 20, RT_BUFID);
	n = send(c->report, b, sizeof b, MSG_NOSIGNAL);
	if (n != (ssize_t)sizeof b)
		return n < 0 ? -errno : -EIO;
	memcpy(expected, "RNGRNT01", 8);
	put64(expected + 8, c->epoch);
	rn_put_le32(expected + 16, c->generation);
	rn_put_le32(expected + 20, RT_BUFID);
	for (;;) {
		struct pollfd p = {c->lease, POLLIN, 0};
		if (cancelled || getppid() != c->parent)
			return -ECANCELED;
		now = rt_now_ms();
		if (!now || now < start || now - start >= 5000)
			return -ETIMEDOUT;
		if (session_ok(c))
			return -ESTALE;
		int ret = poll(&p, 1, 50);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (p.revents & (POLLHUP | POLLERR | POLLNVAL))
			return -ECANCELED;
		if (p.revents & POLLIN) {
			n = recv(c->lease, b, sizeof b, MSG_TRUNC | MSG_DONTWAIT);
			return n == sizeof b && !memcmp(b, expected, sizeof b) ? 0 : -EPROTO;
		}
	}
}
int rt_run(const struct rt_config *c)
{
	struct runtime r = {.cfg = c};
	struct rn_stream stream;
	uint64_t partial_since = 0, tx_since = 0, last;
	int error = validate(c), evicted[RN_CLIENTS];
	if (error)
		return error;
	cancelled = 0;
	struct sigaction sa = {.sa_handler = cancel};
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL) ||
	    signal(SIGPIPE, SIG_IGN) == SIG_ERR || prctl(PR_SET_PDEATHSIG, SIGTERM) ||
	    getppid() != c->parent)
		return -ECANCELED;
	if ((error = handshake(c)))
		return error;
	rn_policy_readonly(&r.policy);
	r.linux.rtc_fd = c->rtc;
	r.ops.read_calendar = c->rtc >= 0 ? rn_linux_read_calendar : NULL;
	r.ops.uptime = rn_linux_uptime;
	/* No write-calendar/clock/hold callbacks are connected to this daemon. */
	rn_hub_init(&r.hub);
	rn_hub_begin(&r.hub, c->epoch);
	rn_stream_init(&stream);
	last = rt_now_ms();
	if (!last) {
		error = -EIO;
		goto done;
	}
	for (;;) {
		if ((error = guard(c)))
			break;
		uint64_t now = rt_now_ms();
		if (!now || now < last) {
			error = -EIO;
			break;
		}
		last = now;
		if ((partial_since && now - partial_since >= c->io_timeout_ms) ||
		    (tx_since && now - tx_since >= c->io_timeout_ms)) {
			error = -ETIMEDOUT;
			break;
		}
		struct pollfd pf[3 + RN_CLIENTS] = {{c->lease, POLLIN, 0},
						    {c->notify, r.tx_len ? POLLOUT : POLLIN, 0},
						    {c->listener, POLLIN, 0}};
		for (size_t i = 0; i < RN_CLIENTS; ++i)
			pf[3 + i] =
			    (struct pollfd){r.hub.clients[i].fd,
					    (short)(r.hub.clients[i].queued ? POLLOUT : POLLIN), 0};
		int ready = poll(pf, 3 + RN_CLIENTS, 50);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			error = -errno;
			break;
		}
		if ((error = guard(c)))
			break;
		if (pf[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			error = -EPIPE;
			break;
		}
		if (pf[2].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			error = -EIO;
			break;
		}
		for (size_t i = 0; i < RN_CLIENTS; ++i) {
			int fd = pf[3 + i].fd;
			if (fd < 0)
				continue;
			if (pf[3 + i].revents & (POLLERR | POLLHUP | POLLNVAL | POLLIN)) {
				rn_hub_remove(&r.hub, fd);
				close(fd);
				continue;
			}
			if (pf[3 + i].revents & POLLOUT) {
				if ((error = guard(c)))
					goto done;
				int ret = rn_hub_flush(&r.hub, c->epoch, fd, rn_linux_send, NULL);
				if (ret && ret != -EAGAIN && ret != -EINTR) {
					rn_hub_remove(&r.hub, fd);
					close(fd);
				}
			}
		}
		if (pf[2].revents & POLLIN) {
			int fd = accept4(c->listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
			if (fd >= 0) {
				if (rn_linux_check_client(fd, c->client_uid) ||
				    rn_hub_subscribe(&r.hub, c->epoch, fd))
					close(fd);
			} else if (errno != EAGAIN && errno != EINTR && errno != ECONNABORTED) {
				error = -errno;
				break;
			}
		}
		if (r.tx_len && (pf[1].revents & POLLOUT)) {
			if ((error = guard(c)))
				break;
			ssize_t n = write(c->notify, r.tx + r.tx_off, r.tx_len - r.tx_off);
			if (n < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					error = -errno;
					break;
				}
			} else if (!n || (size_t)n > r.tx_len - r.tx_off) {
				error = -EIO;
				break;
			} else {
				r.tx_off += (size_t)n;
				if (r.tx_off == r.tx_len) {
					r.tx_len = r.tx_off = 0;
					tx_since = 0;
				}
			}
		} else if (!r.tx_len && (pf[1].revents & POLLIN)) {
			uint8_t b[RN_MAX_FRAME];
			size_t used, want = stream.target - stream.have;
			if ((error = guard(c)))
				break;
			ssize_t n = read(c->notify, b, want);
			if (n < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					error = -errno;
					break;
				}
			} else if (!n) {
				error = rn_stream_finish(&stream);
				if (!error)
					error = -EPIPE;
				break;
			} else if ((size_t)n > want) {
				error = -EIO;
				break;
			} else {
				if (!partial_since)
					partial_since = now;
				error = rn_stream_feed(&stream, b, (size_t)n, &used, dispatch, &r);
				if (error)
					break;
				if (!stream.have)
					partial_since = 0;
				if (r.tx_len && !tx_since)
					tx_since = now;
			}
		}
	}
done:
	rn_hub_end(&r.hub, evicted);
	for (size_t i = 0; i < RN_CLIENTS; ++i)
		if (evicted[i] >= 0)
			close(evicted[i]);
	/* Never transmit pending bytes on cancellation/stale session or reuse FDs. */
	memset(r.tx, 0, sizeof(r.tx));
	fprintf(stderr, "refnotify=exit local_errno=%d frames=%u pending_discarded=1\n", -error,
		r.frames);
	return error;
}
