/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_control.h"
#include "wr_parent.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define PROFILE_PREFIX "reload-postload\n"
#define PREFIX_SIZE (sizeof(PROFILE_PREFIX) - 1)
#define PACKET_SIZE (PREFIX_SIZE + 512)
#define RECEIVED_FDS 16u
static const char complete[] = "AP_RELOAD_COMPLETE NV_NEW_STREAM HALO_UNPROVEN\n";
static const char authenticated[] =
    "AP_RELOAD_COMPLETE NV_NEW_STREAM HALO_FACTORY_FEATURE_MARK_VERIFIED\n";
static int valid_challenge(const char *text, size_t length)
{
	if (length != 32)
		return 0;
	for (unsigned i = 0; i < 32; i++)
		if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
			return 0;
	return 1;
}
int wr_control_peer(int fd, uid_t uid)
{
	struct ucred peer;
	int type = 0;
	socklen_t n = sizeof(peer), t = sizeof(type);
	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &t) ||
	    getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &n))
		return -errno;
	return t == sizeof(type) && type == SOCK_SEQPACKET && n == sizeof(peer) && peer.uid == uid
		   ? 0
		   : -EPERM;
}
int wr_control_send(int fd, const struct wr_postload *fresh)
{
	char payload[PACKET_SIZE], control[CMSG_SPACE(sizeof(int) * WR_POSTLOAD_FDS)] = {0};
	size_t size = 7;
	struct iovec io = {payload, size};
	struct msghdr message = {.msg_iov = &io, .msg_iovlen = 1};
	if (!fresh)
		memcpy(payload, "reload\n", 7);
	else {
		int e = wr_postload_fresh(fresh, 1);
		if (e)
			return e;
		int n =
		    snprintf(payload, sizeof(payload),
			     PROFILE_PREFIX
			     "profile=initial-load-halo-v1\nap_policy=halo-normal-v1\n"
			     "devdir_fd=%d\nstatus_session_fd=%d\nhalo_session_fd=%d\nprovider_rx_"
			     "fd=%d\nprovider_tx_fd=%d\nverification_params_fd=%d\n"
			     "alive_ms=%u\nhalo_ms=%u\nfactory_tail=%u\n",
			     fresh->fd[0], fresh->fd[1], fresh->fd[2], fresh->fd[3], fresh->fd[4],
			     fresh->fd[5], fresh->alive_ms, fresh->halo_ms, fresh->factory_tail);
		if (n < 0 || (size_t)n >= sizeof(payload))
			return -EMSGSIZE;
		size = (size_t)n;
		io.iov_len = size;
		message.msg_control = control;
		message.msg_controllen = sizeof(control);
		struct cmsghdr *c = CMSG_FIRSTHDR(&message);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(fresh->fd));
		memcpy(CMSG_DATA(c), fresh->fd, sizeof(fresh->fd));
	}
	ssize_t n = sendmsg(fd, &message, MSG_NOSIGNAL);
	return n < 0 ? -errno : (size_t)n == size ? 0 : -EIO;
}
int wr_control_status_send(int fd, const char *challenge)
{
	if (!challenge || !valid_challenge(challenge, strnlen(challenge, 33)))
		return -EINVAL;
	char packet[40];
	memcpy(packet, "status ", 7);
	memcpy(packet + 7, challenge, 32);
	packet[39] = '\n';
	ssize_t n = send(fd, packet, sizeof packet, MSG_NOSIGNAL);
	return n < 0 ? -errno : n == (ssize_t)sizeof packet ? 0 : -EIO;
}
void wr_control_request_close(struct wr_control_request *q)
{
	if (!q)
		return;
	wr_postload_close(&q->profile);
	wr_postload_init(&q->profile);
	q->postload = q->status = 0;
	memset(q->challenge, 0, sizeof q->challenge);
}
int wr_control_receive(int fd, uid_t uid, struct wr_control_request *out)
{
	char payload[PACKET_SIZE], control[CMSG_SPACE(sizeof(int) * RECEIVED_FDS)] = {0};
	int fds[RECEIVED_FDS];
	unsigned count = 0;
	int error = 0;
	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	wr_postload_init(&out->profile);
	if ((error = wr_control_peer(fd, uid)))
		return error;
	struct iovec io = {payload, sizeof(payload)};
	struct msghdr message = {.msg_iov = &io,
				 .msg_iovlen = 1,
				 .msg_control = control,
				 .msg_controllen = sizeof(control)};
	ssize_t n = recvmsg(fd, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
	if (n < 0)
		return -errno;
	if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
		error = -EMSGSIZE;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
		    c->cmsg_len < CMSG_LEN(0)) {
			error = -EPROTO;
			continue;
		}
		size_t bytes = c->cmsg_len - CMSG_LEN(0);
		if (bytes % sizeof(int))
			error = -EPROTO;
		for (size_t i = 0; i < bytes / sizeof(int); i++) {
			int got;
			memcpy(&got, (char *)CMSG_DATA(c) + i * sizeof(int), sizeof(got));
			if (count < RECEIVED_FDS)
				fds[count++] = got;
			else {
				close(got);
				error = -EMSGSIZE;
			}
		}
	}
	if (error)
		goto done;
	if (n == 40 && !memcmp(payload, "status ", 7) && payload[39] == '\n') {
		if (count || !valid_challenge(payload + 7, 32))
			error = -EPROTO;
		else {
			out->status = 1;
			memcpy(out->challenge, payload + 7, 32);
			out->challenge[32] = 0;
		}
		goto done;
	}
	if (n == 7 && !memcmp(payload, "reload\n", 7)) {
		if (count)
			error = -EPROTO;
		goto done;
	}
	if (n <= (ssize_t)PREFIX_SIZE || memcmp(payload, PROFILE_PREFIX, PREFIX_SIZE) ||
	    count != WR_POSTLOAD_FDS) {
		error = -EPROTO;
		goto done;
	}
	struct wr_postload parsed;
	error = wr_postload_parse(payload + PREFIX_SIZE, (size_t)n - PREFIX_SIZE, &parsed);
	if (error)
		goto done;
	/* Only received descriptors enter validation/ownership. Never close or use
	 * sender-local integers decoded from the profile on an error path. */
	for (unsigned i = 0; i < WR_POSTLOAD_FDS; i++)
		parsed.fd[i] = fds[i];
	if ((error = wr_postload_fresh(&parsed, 1)) ||
	    (error = wr_postload_validate_fds(&parsed, fd)))
		goto done;
	out->profile = parsed;
	out->postload = 1;
	count = 0;
done:
	for (unsigned i = 0; i < count; i++)
		close(fds[i]);
	return error;
}
static uint32_t at_generation(void)
{
	int fd = open("/sys/class/spipe/stty_lte0/session", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	char text[128], tail;
	ssize_t n = read(fd, text, sizeof text - 1);
	close(fd);
	if (n <= 0)
		return 0;
	text[n] = 0;
	unsigned version, dst, channel, generation, state;
	if (sscanf(text, "%u %u %u %u %u%c", &version, &dst, &channel, &generation, &state,
		   &tail) != 6 ||
	    version != 1 || dst != 5 || channel != 6 || state != 1 || tail != '\n')
		return 0;
	return generation;
}
int wr_control_dispatch(struct wr_parent *p, int fd, uint64_t nonce, uid_t uid)
{
	struct wr_control_request request;
	int error;
	char reply[384];
	const char *text;
	if (!p)
		return -EINVAL;
	struct pollfd ready = {fd, POLLIN, 0};
	int rc = poll(&ready, 1, 500);
	if (rc <= 0)
		return rc < 0 ? -errno : -ETIMEDOUT;
	error = wr_control_receive(fd, uid, &request);
	if (error) {
		snprintf(reply, sizeof(reply), "DENIED error=%d\n", error);
		text = reply;
	} else if (request.status) {
		/* Called from the ordinary wr_main event loop. No separate timer, child
		 * heartbeat, CP request or cached response can answer this challenge. */
		error = wr_parent_check(p);
		unsigned healthy = !error && p->started && !p->uncertain && !p->cancelled &&
				   p->postload.enabled && p->postload.ready &&
				   p->current.nv.live == 1;
		snprintf(
		    reply, sizeof reply,
		    "CPNV_STATUS1 nonce=%s ready=%u uncertain=%u error=%d epoch=%016llx "
		    "generation=%u,%u,%u,%u armed=%u nv_live=%u halo=%u notify=%u storage=%s\n",
		    request.challenge, healthy, !!p->uncertain, error,
		    (unsigned long long)p->current.roles.nonce, p->postload.status_generation,
		    at_generation(), p->current.epoch.generation[2], 0u,
		    p->current.epoch.armed_mask, p->current.nv.live, !!p->postload.ready,
		    !!p->notify_enabled, "ubi");
		text = reply;
	} else {
		error = request.postload ? wr_parent_reload_postload(p, nonce, &request.profile)
					 : wr_parent_reload(p, nonce);
		if (error) {
			snprintf(reply, sizeof(reply), "%s error=%d\n",
				 p->uncertain ? "UNCERTAIN" : "REJECTED", error);
			text = reply;
		} else
			text = request.postload ? authenticated : complete;
	}
	wr_control_request_close(&request);
	ssize_t sent = send(fd, text, strlen(text), MSG_NOSIGNAL);
	/* A lost reply cannot undo reload. Do not cancel or erase parent state. */
	return error ? error : sent < 0 ? -errno : (size_t)sent == strlen(text) ? 0 : -EIO;
}
int wr_control_client(int profile_fd)
{
	struct wr_postload profile;
	wr_postload_init(&profile);
	if (geteuid())
		return 77;
	int error = 0;
	if (profile_fd >= 0) {
		error = wr_postload_load(profile_fd, &profile);
		close(profile_fd);
		if (error) {
			fprintf(stderr, "postload_input=invalid error=%d\n", error);
			return 77;
		}
	}
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	strcpy(address.sun_path, "/run/udx-cpnv-warm/control.sock");
	if (fd < 3 || connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
	    (error = wr_control_peer(fd, 0)))
		goto failed;
	error = wr_control_send(fd, profile_fd >= 0 ? &profile : NULL);
	wr_postload_close(&profile);
	if (error)
		goto failed;
	struct pollfd ready = {fd, POLLIN, 0};
	if (poll(&ready, 1, 610000) <= 0)
		goto failed;
	char reply[128];
	ssize_t n = recv(fd, reply, sizeof(reply) - 1, MSG_TRUNC);
	if (n <= 0 || n >= (ssize_t)sizeof(reply))
		goto failed;
	reply[n] = 0;
	fputs(reply, stdout);
	close(fd);
	return !strcmp(reply, profile_fd >= 0 ? authenticated : complete) ? 0 : 74;
failed:
	if (fd >= 0)
		close(fd);
	wr_postload_close(&profile);
	return 74;
}
