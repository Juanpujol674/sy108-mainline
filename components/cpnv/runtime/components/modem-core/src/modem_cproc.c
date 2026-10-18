/* SPDX-License-Identifier: GPL-2.0-only */
#include "cproc_backend.h"
#include "sha256.h"
#define REG 0100000
#define TYPE 0170000
static int eq(const uint8_t *a, const uint8_t *b, size_t n)
{
	unsigned d = 0;
	while (n--)
		d |= *a++ ^ *b++;
	return !d;
}
static int finish(struct cpb_result *r, int err)
{
	r->error = err;
	if (err)
		r->outcome = r->touched ? CPB_UNCERTAIN : err == -125 ? CPB_CANCELLED : CPB_FAILED;
	else
		r->outcome = CPB_AP_COMPLETE;
	return err;
}
static int same_inode(const struct nv_file_info *a, const struct nv_file_info *b)
{
	return a->inode == b->inode && a->dev_major == b->dev_major && a->dev_minor == b->dev_minor;
}
static int private_file(const struct nv_file_info *i, uint32_t uid)
{
	return (i->mode & TYPE) == REG && i->uid == uid && !(i->mode & 0077) && i->nlink == 1;
}
static int budget(const struct cpb_io *io, const struct cpb_request *q, uint64_t deadline)
{
	uint64_t now;
	int r;
	if (io->now(&now))
		return -5;
	if (now >= deadline)
		return -110;
	r = io->wait(-1, q->signal_fd, 0);
	return r < 0 ? r : 0;
}
static int status_read(const struct cpb_io *io, int fd, unsigned *out)
{
	uint8_t b[32];
	long n = io->file.pread(io->file.ctx, fd, b, sizeof(b), 0);
	if (n == 8 && eq(b, (const uint8_t *)"unknown\n", 8))
		*out = CPB_STATUS_UNKNOWN;
	else if (n == 8 && eq(b, (const uint8_t *)"started\n", 8))
		*out = CPB_STATUS_STARTED;
	else if (n == 8 && eq(b, (const uint8_t *)"stopped\n", 8))
		*out = CPB_STATUS_STOPPED;
	else if (n == 7 && eq(b, (const uint8_t *)"wdtirq\n", 7))
		*out = CPB_STATUS_WDT;
	else
		return n < 0 ? (int)n : -71;
	return 0;
}
static int copy_range(const struct cpb_io *io, const struct cpb_request *q, struct cpb_result *r,
		      uint64_t deadline)
{
	uint8_t b[4096];
	uint64_t done = 0;
	unsigned interrupted = 0;
	while (done < q->length) {
		size_t size = q->length - done > sizeof(b) ? sizeof(b) : (size_t)(q->length - done),
		       sent = 0;
		long got;
		int e = budget(io, q, deadline);
		if (e)
			return e;
		got = io->file.pread(io->file.ctx, q->source_fd, b, size, q->source_offset + done);
		if (got == -4 && interrupted++ < 16)
			continue;
		if (got <= 0 || (size_t)got > size)
			return got < 0 ? (int)got : -5;
		while (sent < (size_t)got) {
			long written;
			e = budget(io, q, deadline);
			if (e)
				return e;
			r->touched = 1;
			written =
			    io->file.pwrite(io->file.ctx, q->action_fd, b + sent,
					    (size_t)got - sent, q->target_offset + done + sent);
			/* No negative write retry: even an interrupted control/device operation can
			 * have side effects. Positive short progress is accounted exactly. */
			if (written <= 0 || (size_t)written > (size_t)got - sent)
				return written < 0 ? (int)written : -5;
			sent += (size_t)written;
			r->bytes_written += (uint64_t)written;
		}
		done += (uint64_t)got;
	}
	return 0;
}
int cpb_execute(const struct cpb_io *io, const struct cpb_request *q, struct cpb_result *r)
{
	const char *domain, *leaf = 0;
	struct nv_file_info status, owner, source, target;
	uint64_t deadline;
	int error = 0, locked = 0;
	if (!r)
		return -22;
	{
		uint8_t *p = (uint8_t *)r;
		size_t n = sizeof(*r);
		while (n--)
			*p++ = 0;
	}
	r->outcome = CPB_DENIED;
	if (!io || !q || !io->endpoint || !io->write || !io->wait || !io->now || !io->file.info ||
	    !io->file.pread || !io->file.pwrite || !io->file.lock || !io->file.unlock)
		return r->error = -22;
	r->generation = q->generation;
	domain = cpb_domain(q->domain);
	if (!domain || !q->generation || q->status_fd < 3 || q->action < CPB_STATUS ||
	    q->action > CPB_WATCH || !q->timeout_ms || q->timeout_ms > 60000)
		return r->error = -22;
	if (io->now(&r->submitted_ms) || r->submitted_ms > (uint64_t)INT64_MAX - q->timeout_ms)
		return r->error = -5;
	deadline = r->submitted_ms + q->timeout_ms;
	if (io->file.info(io->file.ctx, q->status_fd, &status) || status.access != 0 ||
	    io->endpoint(q->status_fd, domain, "status"))
		return r->error = -71;
	if (q->action == CPB_STATUS) {
		error = status_read(io, q->status_fd, &r->status);
		goto done;
	}
	if (q->action == CPB_WATCH)
		leaf = "wdtirq";
	else if (q->action == CPB_START)
		leaf = "start";
	else if (q->action == CPB_STOP)
		leaf = "stop";
	else
		leaf = cpb_segment(q->domain, q->segment);
	if (!leaf || q->action_fd < 3 || q->action_fd == q->status_fd ||
	    io->file.info(io->file.ctx, q->action_fd, &target) ||
	    io->endpoint(q->action_fd, domain, leaf))
		return r->error = -22;
	if (q->action == CPB_WATCH) {
		if (target.access != 0)
			return r->error = -13;
		error = io->wait(q->action_fd, q->signal_fd, q->timeout_ms);
		if (error > 0)
			error = status_read(io, q->action_fd, &r->status);
		else if (!error)
			error = -110;
		if (!error && r->status != CPB_STATUS_WDT)
			error = -71;
		goto done;
	}
	if (q->owner_fd < 3 || q->owner_fd == q->action_fd || q->owner_fd == q->status_fd)
		return r->error = -13;
	if (io->file.info(io->file.ctx, q->owner_fd, &owner) ||
	    !private_file(&owner, io->file.owner_uid) || owner.access != 2 ||
	    same_inode(&owner, &target) || same_inode(&owner, &status))
		return r->error = -13;
	if (io->file.lock(io->file.ctx, q->owner_fd))
		return r->error = -11;
	locked = 1;
	if (q->action == CPB_START || q->action == CPB_LOAD) {
		error = status_read(io, q->status_fd, &r->status);
		if (error)
			goto done;
		if (r->status != CPB_STATUS_STOPPED) {
			error = -16;
			goto done;
		}
	}
	if (q->action == CPB_START || q->action == CPB_STOP) {
		long n;
		if (target.access != 1) {
			error = -13;
			goto done;
		}
		error = budget(io, q, deadline);
		if (error)
			goto done;
		r->touched = 1;
		n = io->write(q->action_fd, "1", 1);
		if (n != 1) {
			error = n < 0 ? (int)n : -5;
			goto done;
		}
		r->bytes_written = 1;
		error = status_read(io, q->status_fd, &r->status);
		if (!error &&
		    r->status != (q->action == CPB_START ? CPB_STATUS_STARTED : CPB_STATUS_STOPPED))
			error = -71;
	} else {
		if (q->source_fd < 3 || q->source_fd == q->action_fd ||
		    q->source_fd == q->owner_fd || q->source_fd == q->status_fd || !q->length ||
		    q->length > UINT32_MAX || q->capacity > UINT32_MAX ||
		    q->target_offset > q->capacity || q->length > q->capacity - q->target_offset ||
		    target.access != 2) {
			error = -22;
			goto done;
		}
		if (io->file.info(io->file.ctx, q->source_fd, &source) || source.access != 0 ||
		    ((source.mode & TYPE) != REG && (source.mode & TYPE) != 0020000) ||
		    q->source_offset > INT64_MAX - q->length || same_inode(&source, &target) ||
		    same_inode(&source, &owner)) {
			error = -13;
			goto done;
		}
		/* Character-volume extents were bounded against actual UBI geometry by
		 * the parent. Regular generated/test inputs also expose their byte size. */
		if ((source.mode & TYPE) == REG && (q->source_offset > source.size ||
						    q->length > source.size - q->source_offset)) {
			error = -22;
			goto done;
		}
		error = copy_range(io, q, r, deadline);
		if (error)
			goto done;
	}
done:
	if (locked && io->file.unlock(io->file.ctx, q->owner_fd) && !error)
		error = -5;
	if (io->now(&r->completed_ms) && !error)
		error = -5;
	return finish(r, error);
}
