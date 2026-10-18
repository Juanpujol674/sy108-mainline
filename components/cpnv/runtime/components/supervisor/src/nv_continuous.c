/* SPDX-License-Identifier: GPL-2.0-only */
#include "platform.inc"
#include "nv_schedule.h"
#include "nv_epoch.h"
#include "wr_quiesce.h"
#include "nv_wire.h"
#include "nv_vendor.h"
struct continuous {
	struct context part[2];
	struct nv_ubi_backend backend;
	struct nvc_schedule schedule;
	struct nvc_epoch epoch;
	uint64_t deadline, last_ms;
	unsigned transaction, progress, committed, nacks, discarded;
	struct wr_quiesce quiesce;
};
static struct continuous *running;
static int continuous_lease(void *ignored, uint64_t epoch)
{
	(void)ignored;
	if (!running || lease(&running->part[0], epoch))
		return -EPIPE;
	return 0;
}
/* The sole writer keeps these descriptors and its lock for its lifetime.
 * Block progress needs only existing cancellation/transport signals; session
 * snapshots are checked at transaction and ACK boundaries below. */
static int continuous_progress(void *ignored, uint64_t epoch)
{
	(void)ignored;
	if (!running)
		return -EPIPE;
	struct context *c = &running->part[0];
	if (!c->authorized || !c->locked || epoch != c->c.epoch)
		return -EACCES;
	struct pollfd p[3] = {{c->c.fd[5], POLLIN | POLLERR | POLLHUP, 0},
			      {c->c.fd[2], POLLERR | POLLHUP, 0},
			      {c->c.revoke_fd, POLLIN, 0}};
	if (poll(p, 3, 0) < 0)
		return -errno;
	return p[0].revents || p[1].revents || p[2].revents ? -EPIPE : 0;
}
static int continuous_parse(int argc, char **argv, struct continuous *s)
{
	char *base[100];
	int count = 1, i;
	uint64_t seen = 0, number[19] = {0};
	static const char *names[] = {
	    "--runtime-primary-fd",    "--runtime-backup-fd",	   "--runtime-size",
	    "--runtime-checksum-bits", "--runtime-primary-dir-fd", "--runtime-backup-dir-fd",
	    "--runtime-primary-rdev",  "--runtime-backup-rdev",	   "--session-halo-fd",
	    "--session-at-fd",	       "--session-nv-fd",	   "--session-sipa-fd",
	    "--generation-halo",       "--generation-at",	   "--generation-nv",
	    "--generation-sipa",       "--session-armed-mask",	   "--quiesce-fd",
	    "--unified-parent-pid"};
	if (argc > 97)
		return -1;
	base[0] = argv[0];
	for (i = 1; i < argc; i++) {
		int k;
		for (k = 0; k < 19; k++)
			if (!strcmp(argv[i], names[k]))
				break;
		if (k < 19) {
			uint64_t bit = UINT64_C(1) << (unsigned)k;
			if ((seen & bit) || ++i == argc || decimal(argv[i], &number[k]))
				return -1;
			seen |= bit;
		} else {
			base[count++] = argv[i];
			if (++i == argc)
				return -1;
			base[count++] = argv[i];
		}
	}
	base[count] = NULL;
	if (parse(count, base, &s->part[0].c) || s->part[0].c.partition != 1 ||
	    s->part[0].c.absolute_deadline)
		return -1;
	{
		uint64_t required = UINT64_C(0x744ff);
		if (seen != required)
			return -1;
	}
	if (number[17] < 3 || number[17] > INT_MAX || number[18] < 2 || number[18] > INT_MAX)
		return -1;
	s->quiesce.fd = (int)number[17];
	s->quiesce.parent = (pid_t)number[18];
	s->quiesce.nonce = s->part[0].c.epoch;
	s->part[1].c = s->part[0].c;
	s->part[1].c.partition = 2;
	for (i = 0; i < 2; i++) {
		if (number[i] < 3 || number[i] > INT_MAX)
			return -1;
		s->part[1].c.fd[i] = (int)number[i];
		{
			if (number[4 + i] < 3 || number[4 + i] > INT_MAX || !number[6 + i])
				return -1;
			s->part[1].c.vol_dir[i] = (int)number[4 + i];
			s->part[1].c.volume_dev[i] = number[6 + i];
		}
	}
	if (!number[2] || number[2] > NV_MAX_IMAGE || number[2] % 512 ||
	    (number[3] != 32 && number[3] != 64))
		return -1;
	s->part[1].c.size = (uint32_t)number[2];
	s->part[1].c.bits = (uint32_t)number[3];
	if (number[16] != 4 || number[10] < 3 || number[10] > INT_MAX || number[14] > UINT32_MAX)
		return -1;
	s->epoch.armed_mask = 4;
	for (i = 0; i < 4; i++)
		s->epoch.fd[i] = -1;
	s->epoch.fd[2] = (int)number[10];
	s->epoch.generation[2] = (uint32_t)number[14];
	/* v3 accepts both known legacy header checksums on read; write policy is
	 * explicitly canonical64 per transaction, never a process-global last-read bit. */
	if (s->part[0].c.bits != 64 || s->part[1].c.bits != 64)
		return -1;
	/* Reject aliases across all storage/capability roles, except common transport. */
	{
		int fd[21], n = 0, j, k;
		fd[n++] = s->quiesce.fd;
		for (i = 0; i < 2; i++) {
			fd[n++] = s->part[i].c.fd[0];
			fd[n++] = s->part[i].c.fd[1];
		}
		for (i = 2; i < 6; i++)
			if (i != 3)
				fd[n++] = s->part[0].c.fd[i];
		if (s->part[0].c.revoke_fd >= 3)
			fd[n++] = s->part[0].c.revoke_fd;
		{
			fd[n++] = s->part[0].c.fd[6];
			for (i = 0; i < 2; i++) {
				fd[n++] = s->part[i].c.vol_dir[0];
				fd[n++] = s->part[i].c.vol_dir[1];
			}
		}
		fd[n++] = s->epoch.fd[2];
		for (j = 0; j < n; j++)
			for (k = 0; k < j; k++)
				if (fd[j] == fd[k])
					return -1;
	}
	return 0;
}
static int same_object(const struct stat *a, const struct stat *b)
{
	return S_ISCHR(a->st_mode) && S_ISCHR(b->st_mode)
		   ? a->st_rdev == b->st_rdev
		   : a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}
static int safe_stdio(struct continuous *s)
{
	int roles[19], n = 0, i, j;
	roles[n++] = s->quiesce.fd;
	struct stat st[3];
	for (i = 0; i < 3; i++)
		if (fstat(i, &st[i]))
			return -1;
	for (i = 0; i < 2; i++) {
		roles[n++] = s->part[i].c.fd[0];
		roles[n++] = s->part[i].c.fd[1];
	}
	for (i = 2; i < 6; i++)
		roles[n++] = s->part[0].c.fd[i];
	if (s->part[0].c.revoke_fd >= 3)
		roles[n++] = s->part[0].c.revoke_fd;
	roles[n++] = s->epoch.fd[2];
	for (i = 0; i < n; i++) {
		struct stat data;
		if (fstat(roles[i], &data))
			return -1;
		for (j = 0; j < 3; j++)
			if (same_object(&data, &st[j]))
				return -1;
	}
	return 0;
}
static int continuous_bootstrap(struct continuous *s)
{
	unsigned i, j;
	if (bootstrap(&s->part[0], 1) || bootstrap(&s->part[1], 0))
		return -1;
	for (i = 0; i < 4; i++)
		for (j = 0; j < i; j++) {
			struct stat *a = &s->part[i / 2].pinned[i % 2],
				    *b = &s->part[j / 2].pinned[j % 2];
			if (a->st_dev == b->st_dev && a->st_ino == b->st_ino)
				return -1;
			if (a->st_rdev == b->st_rdev)
				return -1;
		}
	for (i = 0; i < 4; i++) {
		const struct stat *a = &s->part[i / 2].pinned[i % 2], *o = &s->part[0].pinned[4];
		if (a->st_dev == o->st_dev && a->st_ino == o->st_ino)
			return -1;
	}
	running = s;
	return continuous_lease(NULL, s->part[0].c.epoch) || wr_quiesce_open(&s->quiesce) ? -1 : 0;
}
/* A successful but slow poll/lease/read/write is not a timely authorization.
 * Checking around syscalls cannot retract bytes already accepted by the kernel. */
static int continuous_guard(struct continuous *s, int idle, uint64_t *observed)
{
	uint64_t now;
	unsigned pass;
	for (pass = 0; pass < 2; pass++) {
		if (clock_ms(NULL, &now))
			return NVU_IO;
		if (now < s->last_ms || (!idle && now >= s->deadline))
			return NVU_DEADLINE;
		s->last_ms = now;
		if (!pass && continuous_lease(NULL, s->part[0].c.epoch))
			return NVU_OWNERSHIP;
	}
	if (observed)
		*observed = now;
	return 0;
}
static int set_deadline(struct continuous *s)
{
	uint64_t now;
	if (clock_ms(NULL, &now) || now < s->last_ms || now > UINT64_MAX - s->part[0].c.timeout)
		return NVU_DEADLINE;
	s->last_ms = now;
	s->deadline = now + s->part[0].c.timeout;
	s->transaction = 1;
	return 0;
}
static int continuous_wait(struct continuous *s, int fd, short events, int idle)
{
	struct context *c = &s->part[0];
	uint64_t now;
	int r, timeout;
	struct pollfd p[5] = {{fd, events, 0},
			      {c->c.fd[5], POLLIN, 0},
			      {c->c.fd[2], POLLERR | POLLHUP, 0},
			      {c->c.revoke_fd, POLLIN, 0},
			      {events == POLLIN ? s->quiesce.fd : -1, POLLIN, 0}};
	for (;;) {
		if ((r = continuous_guard(s, idle, &now)))
			return r;
		timeout =
		    idle ? 1000 : (s->deadline - now > 1000 ? 1000 : (int)(s->deadline - now));
		r = poll(p, 5, timeout);
		if (r < 0)
			return NVU_IO;
		if (p[1].revents || p[2].revents || p[3].revents ||
		    (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
			return NVU_OWNERSHIP;
		if ((r = continuous_guard(s, idle, NULL)))
			return r;
		if (events == POLLIN && p[4].revents) {
			int q = wr_quiesce_request(&s->quiesce);
			if ((r = continuous_guard(s, idle, NULL)))
				return r;
			return q == 1 ? WR_QUIESCED : NVU_OWNERSHIP;
		}
		if (p[0].revents & events)
			return 0;
	}
}
static int continuous_read(struct continuous *s, uint8_t *p, size_t n, int first)
{
	int fd = s->part[0].c.fd[2];
	unsigned interrupts = 0;
	while (n) {
		ssize_t got;
		int saved, r, idle = first && !s->transaction;
		if ((r = continuous_wait(s, fd, POLLIN, idle)))
			return r;
		if (idle && (r = set_deadline(s)))
			return r;
		if ((r = continuous_guard(s, 0, NULL)))
			return r;
		got = read(fd, p, n);
		saved = errno;
		if ((r = continuous_guard(s, 0, NULL)))
			return r;
		if (got < 0 && (saved == EINTR || saved == EAGAIN) && interrupts++ < 16)
			continue;
		if (got <= 0 || (size_t)got > n)
			return NVU_IO;
		p += (size_t)got;
		n -= (size_t)got;
	}
	return 0;
}
static int continuous_wire_read(void *context, uint8_t *data, size_t length, int first)
{
	return continuous_read(context, data, length, first);
}
static size_t continuous_write(struct continuous *s, const uint8_t *p, size_t n, int *gate_error)
{
	size_t done = 0;
	*gate_error = 0;
	while (done < n) {
		ssize_t got;
		int io_error = 0;
		if ((*gate_error = continuous_wait(s, s->part[0].c.fd[3], POLLOUT, 0)) ||
		    (*gate_error = continuous_guard(s, 0, NULL)))
			break;
		got = write(s->part[0].c.fd[3], p + done, n - done);
		if (got > 0 && (size_t)got <= n - done)
			done += (size_t)got; /* preserve actual accepted bytes */
		else
			io_error = NVU_IO;
		if ((*gate_error = continuous_guard(s, 0, NULL)))
			break;
		if (io_error) {
			*gate_error = io_error;
			break;
		}
	}
	return done;
}
static int begin_partition(void *v, uint32_t id, struct nv_ubi_backend **out)
{
	struct continuous *s = v;
	struct context *c = &s->part[id - 1];
	struct nv_ubi_ops ops;
	struct nv_ubi_identity identity[2];
	int r;
	if (id != 1 && id != 2)
		return NVU_CONFIG;
	c->deadline = s->deadline;
	{
		c->linux = (struct nv_ubi_linux_context){c, metadata, continuous_lease};
		ops = nv_ubi_linux_ops(&c->linux);
	}
	/* Preserve the Linux adapter context while changing only the hot callback. */
	c->linux.lease_valid = continuous_progress;
	if (!c->inventory_ready) {
		if (ops.inspect(ops.ctx, c->c.fd[0], &c->inventory[0]) ||
		    ops.inspect(ops.ctx, c->c.fd[1], &c->inventory[1]))
			return NVU_IO;
		c->inventory_ready = 1;
	}
	identity[0] = c->inventory[0];
	identity[1] = c->inventory[1];
	r = nv_ubi_bind(&s->backend, &ops, c->c.fd[0], c->c.fd[1], identity, c->c.size, c->c.bits,
			(int)c->c.policy, c->c.epoch, s->deadline, NULL, NULL, 0);
	*out = &s->backend;
	return r ? r : nv_ubi_begin(&s->backend);
}
static void discard_partition(void *v, struct nv_ubi_backend *b)
{
	struct continuous *s = v;
	if (b->state == NVU_STAGING) {
		nv_ubi_abort(b);
		s->discarded++;
	}
	nv_ubi_forget(b);
}
#ifndef NVC_ENTRY
#define NVC_ENTRY main
#endif
int NVC_ENTRY(int argc, char **argv)
{
	struct continuous s;
	struct nvc_actions actions;
	int error = 0;
	const char *reason = "READ_OR_EPOCH_FAILED";
	memset(&s, 0, sizeof(s));
	if (continuous_parse(argc, argv, &s))
		return 64;
	if (safe_stdio(&s))
		return 65;
	if (continuous_bootstrap(&s)) {
		fputs("owner_fd_or_epoch=rejected\n", stderr);
		return 65;
	}
	fprintf(stdout, "nv_storage=ubi wire_frame_bytes=%u\n", NV_FRAME_HEADER + NV_MAX_PAYLOAD);
	fflush(stdout);
	if (wr_quiesce_live(&s.quiesce))
		return 65;
	/* Stock 0x401d20 compares FIX payload word 0 with RUNTIME payload word 1.
	 * This flag controls full runtime writes; it is not a replica generation. */
	{
		uint32_t fix, runtime;
		uint64_t now;
		if (clock_ms(NULL, &now) ||
		    nv_vendor_initial_timestamps(s.part[0].c.fd, s.part[1].c.fd[0], &fix, &runtime,
						 now + 60000))
			return 65;
		nv_vendor_runtime_timestamp(fix, runtime);
	}
	actions = (struct nvc_actions){&s, begin_partition, discard_partition};
	nvc_init(&s.schedule, &actions);
	struct nvc_wire_io wire = {&s, continuous_wire_read};
	for (;;) {
		struct nv_frame f;
		int reply, gate_error = 0;
		uint8_t ack[NV_RESPONSE_SIZE];
		size_t sent;
		if ((error = nvc_wire_read(&wire, packet, sizeof packet, &f))) {
			reason =
			    error == NVU_PROTOCOL ? "FRAME_REJECTED" : "TRUNCATED_OR_EPOCH_FAILED";
			break;
		}
		/* New group resets the transaction timer only after full frame delimitation.
		 * Slow/truncated headers never get renewed deadlines. */
		if (f.group != s.schedule.group && !f.sub && s.schedule.busy && set_deadline(&s)) {
			error = NVU_DEADLINE;
			break;
		}
		reply = nvc_frame(&s.schedule, packet, (size_t)f.payload_size + NV_FRAME_HEADER);
		if (reply < 0) {
			error = reply;
			reason = "SESSION_OR_STORAGE_REJECTED";
			break;
		}
		if (reply == NVC_FINAL) {
			error = nv_ubi_prepare_final_ack(&s.backend, ack);
			if (error) {
				reason = "FINAL_ACK_DENIED";
				break;
			}
			sent = continuous_write(&s, ack, sizeof(ack), &gate_error);
			error = nvc_finish_final_reply(&s.schedule, sent, gate_error);
			fprintf((error || gate_error || sent != sizeof(ack) ||
				 !s.backend.storage_verified)
				    ? stderr
				    : stdout,
				"transaction=final partition=%u group=%u sub=%u "
				"storage_verified=%u reply_written=%u reply_bytes=%zu "
				"post_guard_error=%d reply_accepted_in_epoch=%u\n",
				s.schedule.partition, f.group, f.sub, s.backend.storage_verified,
				(unsigned)(sent == sizeof(ack)), sent, gate_error,
				(unsigned)(!error));
			fflush((error || gate_error || sent != sizeof(ack) ||
				!s.backend.storage_verified)
				   ? stderr
				   : stdout);
			if (error) {
				reason = "FINAL_REPLY_LOST";
				break;
			}
			s.committed++;
			s.transaction = 0;
		} else if (reply == NVC_PROGRESS || reply == NVC_NACK) {
			if (nv_response_encode(&f, reply == NVC_PROGRESS, ack) ||
			    continuous_write(&s, ack, sizeof(ack), &gate_error) != sizeof(ack) ||
			    gate_error) {
				error = gate_error ? gate_error : NVU_REPLY_ERROR;
				reason = "PROGRESS_OR_NACK_LOST";
				break;
			}
			if (reply == NVC_PROGRESS)
				s.progress++;
			else {
				s.nacks++;
				s.transaction = 0;
			}
		} else if (!s.schedule.busy)
			s.transaction = 0;
	}
	unsigned dirty = s.transaction || s.schedule.busy;
	int quiet = error == WR_QUIESCED;
	fprintf(quiet ? stdout : stderr,
		"continuous=stopped reason=%s error=%d committed=%u progress=%u nacks=%u "
		"discarded=%u storage_verified=%u changed_mask=%u\n",
		quiet ? "QUIESCE_REQUEST" : reason, quiet ? 0 : error, s.committed, s.progress,
		s.nacks, s.discarded, s.backend.storage_verified, s.backend.possibly_changed);
	fflush(quiet ? stdout : stderr);
	/* Production quiescence comes from synchronous backend return at this read
	 * boundary, no preceding storage/ACK failure, actual data close and pidfd
	 * exit. Shadow-file updates additionally retain their in-flight state. */
	nvc_discard(&s.schedule);
	nv_ubi_forget(&s.backend);
	nv_vendor_cache_release();
	/* No storage or transport capability remains when the AP-local report is
	 * published. Process exit is still separately required by the parent. */
	int data[6] = {s.part[0].c.fd[0], s.part[0].c.fd[1], s.part[1].c.fd[0],
		       s.part[1].c.fd[1], s.part[0].c.fd[2], s.part[0].c.fd[3]};
	for (unsigned i = 0; i < 6; i++) {
		unsigned j;
		for (j = 0; j < i; j++)
			if (data[j] == data[i])
				break;
		if (j == i && close(data[i]))
			quiet = 0;
	}
	if (quiet && !wr_quiesce_done(&s.quiesce, dirty))
		return 0;
	return 68;
}
