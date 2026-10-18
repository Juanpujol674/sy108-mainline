/* SPDX-License-Identifier: GPL-2.0-only */
#include "modem_control.h"
#include "nv_codec.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include "nv_vendor.h"
#include "nv_checksum_blocks.h"
static uint32_t nv_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int nv_read_exact(struct mc_runtime *r, int fd, uint8_t *data, size_t size, uint64_t offset)
{
	size_t done = 0;
	while (done < size) {
		long n =
		    r->cp.file.pread(r->cp.file.ctx, fd, data + done, size - done, offset + done);
		if (n <= 0 || (size_t)n > size - done)
			return n < 0 ? (int)n : -EIO;
		done += (size_t)n;
	}
	return 0;
}
static int nv_validate_source(struct mc_runtime *r, int fd, uint32_t size, uint8_t *buffer,
			      uint32_t *checksum)
{
	uint8_t header[NV_HEADER];
	int error = nv_read_exact(r, fd, header, sizeof(header), 0);
	if (error)
		return error;
	if (nv_le32(header) != NV_IMAGE_MAGIC || nv_le32(header + 4) != size)
		return -EBADMSG;
	uint64_t sum = 0;
	for (uint32_t off = 0; off < size;) {
		size_t n = size - off;
		if (n > 65536)
			n = 65536;
		if ((error = nv_read_exact(r, fd, buffer, n, (uint64_t)NV_HEADER + off)))
			return error;
		sum += nv_block_sum(buffer, n);
		off += (uint32_t)n;
	}
	*checksum = nv_le32(header + 8);
	return *checksum == nv_block_checksum(sum, 64) || *checksum == nv_block_checksum(sum, 32)
		   ? 0
		   : -EBADMSG;
}
static int nv_load(struct mc_runtime *r, const struct mc_image *v)
{
	if (v->offset || v->target_offset || v->source < 3 || v->backup < 3 || !v->length ||
	    v->length > NV_MAX_IMAGE || v->length % 512)
		return -EINVAL;
	uint8_t *buffer = malloc(65536);
	if (!buffer)
		return -ENOMEM;
	int fd[2] = {v->source, v->backup}, valid[2] = {0, 0}, error = 0, read_error = 0;
	uint32_t checksum[2];
	for (unsigned i = 0; i < 2; i++) {
		struct stat st;
		int flags = fcntl(fd[i], F_GETFL);
		if (flags < 0 || fstat(fd[i], &st) || (flags & O_ACCMODE) != O_RDONLY ||
		    !S_ISCHR(st.st_mode)) {
			error = -EINVAL;
			goto out;
		}
		int e = nv_validate_source(r, fd[i], (uint32_t)v->length, buffer, &checksum[i]);
		if (e == -ETIMEDOUT || e == -ECANCELED) {
			error = e;
			goto out;
		}
		valid[i] = !e;
		if (e && e != -EBADMSG && !read_error)
			read_error = e;
	}
	if (!valid[0] && !valid[1]) {
		error = read_error ? read_error : -EBADMSG;
		goto out;
	}
	unsigned selected = valid[0] ? 0u : 1u;
	uint64_t sum = 0;
	/* CP remains stopped and NV descriptors are exclusively owned. Re-read the
	 * selected payload through one bounded buffer instead of retaining replicas. */
	for (uint64_t off = 0; off < v->length;) {
		size_t take = (size_t)(v->length - off);
		if (take > 65536)
			take = 65536;
		if ((error = nv_read_exact(r, fd[selected], buffer, take, NV_HEADER + off)))
			goto out;
		sum += nv_block_sum(buffer, take);
		size_t done = 0;
		while (done < take) {
			long n = r->cp.file.pwrite(r->cp.file.ctx, v->target, buffer + done,
						   take - done, off + done);
			if (n <= 0 || (size_t)n > take - done) {
				error = n < 0 ? (int)n : -EIO;
				goto out;
			}
			r->ap_touched = 1;
			done += (size_t)n;
		}
		off += take;
	}
	if (checksum[selected] != nv_block_checksum(sum, 64) &&
	    checksum[selected] != nv_block_checksum(sum, 32))
		error = -EBADMSG;
out:
	free(buffer);
	return error ? error : mc_guard(r);
}
static int operation(struct mc_runtime *r, unsigned action, unsigned domain,
		     const struct mc_image *v)
{
	int e = mc_deadline(r, r->config.operation_ms);
	if (e)
		return e;
	struct cpb_request q = {0};
	struct cpb_result out;
	q.action = action;
	q.domain = domain;
	q.status_fd = r->config.status[domain];
	q.action_fd = action == CPB_START ? r->config.start[domain] : r->config.stop[domain];
	q.source_fd = -1;
	q.owner_fd = r->config.owner;
	q.signal_fd = -1;
	q.generation = r->config.epoch;
	q.timeout_ms = r->config.operation_ms;
	if (v) {
		q.segment = v->segment;
		q.source_fd = v->source;
		q.action_fd = v->target;
		q.source_offset = v->offset;
		q.target_offset = v->target_offset;
		q.length = v->length;
		q.capacity = v->capacity;
		memcpy(q.digest, v->sha256, 32);
	}
	e = cpb_execute(&r->cp, &q, &out);
	if (out.touched)
		r->ap_touched = 1;
	if (e)
		return e;
	return mc_guard(r);
}
int mc_load_complete(struct mc_runtime *r)
{
	int seen[MC_MAX_IMAGES * 3];
	unsigned count = 0;
	for (unsigned i = 0; i < r->config.image_count; i++) {
		int fds[] = {r->config.images[i].source, r->config.images[i].target,
			     r->config.images[i].backup};
		for (unsigned j = 0; j < 3; j++) {
			int fd = fds[j], duplicate = 0;
			if (fd < 3)
				continue;
			for (unsigned k = 0; k < count; k++)
				if (seen[k] == fd)
					duplicate = 1;
			if (!duplicate) {
				seen[count++] = fd;
				if (close(fd))
					return -errno;
			}
		}
	}
	if (r->config.phase < 3)
		return r->config.reload ? -EPERM : 0;
	uint8_t frame[32] = {0};
	memcpy(frame, "NVLOAD03", 8);
	for (unsigned i = 0; i < 8; i++)
		frame[8 + i] = (uint8_t)(r->config.epoch >> (8 * i));
	for (unsigned i = 0; i < 4; i++)
		frame[16 + i] = (uint8_t)((unsigned)getpid() >> (8 * i));
	frame[20] = 1;
	return mc_exact(r, r->config.phase, 1, frame, 32);
}
static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p)
{
	return get32(p) | (uint64_t)get32(p + 4) << 32;
}
static void put32(uint8_t *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
	for (unsigned i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
static int report(struct mc_runtime *r, const char magic[8])
{
	uint8_t b[32] = {0};
	memcpy(b, magic, 8);
	put64(b + 8, r->config.epoch);
	put32(b + 16, (uint32_t)getpid());
	put32(b + 20, 7);
	put32(b + 24, r->ap_touched ? 2u : 0u);
	return mc_exact(r, r->config.report, 1, b, sizeof b);
}
static int roles(const struct mc_config *c, unsigned pending)
{
	unsigned mask = 0, hits[2] = {0, 0};
	if (c->pmic || !c->reload)
		return -EINVAL;
	for (unsigned i = 0; i < c->image_count; i++) {
		const struct mc_image *v = c->images + i;
		if (v->domain || v->segment > 8)
			return -EINVAL;
		mask |= 1u << v->segment;
		if (v->segment >= 7) {
			if (++hits[v->segment - 7] != 1 || (!pending && v->backup < 3))
				return -EINVAL;
			uint64_t expected = (uint64_t)(v->segment == 7 ? 7 : 9) * 1024 * 1024;
			if ((v->capacity != expected || (!pending && v->length != expected)))
				return -EINVAL;
			if (!pending && (v->offset || v->target_offset ||
					 v->length > NV_MAX_IMAGE || v->length % 512))
				return -EINVAL;
		}
	}
	return mask == 511 ? 0 : -ENODATA;
}
static int status_read(struct mc_runtime *r, unsigned domain, unsigned *value)
{
	int e = mc_deadline(r, r->config.operation_ms);
	if (e)
		return e;
	struct cpb_request q = {0};
	struct cpb_result out;
	q.action = CPB_STATUS;
	q.domain = domain;
	q.generation = r->config.epoch;
	q.status_fd = r->config.status[domain];
	q.timeout_ms = r->config.operation_ms;
	e = cpb_execute(&r->cp, &q, &out);
	if (e)
		return e;
	*value = out.status;
	return mc_guard(r);
}
static int status_expected(struct mc_runtime *r, unsigned domain, unsigned expected)
{
	unsigned value;
	int e = status_read(r, domain, &value);
	return e ? e : value != expected ? -EHOSTDOWN : 0;
}
int mc_preloaded_adopt(struct mc_runtime *r)
{
	if (!r || r->config.reload || r->config.image_count ||
	    r->config.postload_mode > MC_HALO_NORMAL)
		return -EINVAL;
	for (unsigned d = 0; d < 3; d++) {
		unsigned value;
		int e = status_read(r, d, &value);
		if (e)
			return e;
		if (value != CPB_STATUS_UNKNOWN && value != CPB_STATUS_STARTED)
			return -ENOLINK;
		r->preloaded_status[d] = value;
	}
	r->preloaded_bound = 1;
	return mc_load_complete(r);
}
int mc_monitor_started(struct mc_runtime *r)
{
	if (!r->config.reload && !r->preloaded_bound)
		return -EPROTO;
	for (unsigned d = 0; d < 3; d++) {
		int e = status_expected(
		    r, d, r->config.reload ? CPB_STATUS_STARTED : r->preloaded_status[d]);
		if (e)
			return e;
	}
	return mc_deadline(r, 2000);
}
static int accept_continuation(struct mc_runtime *r)
{
	uint8_t raw[32 + 64 * MC_MAX_IMAGES];
	int e;
	int inputs[4] = {-1, -1, -1, -1};
	unsigned count = 0;
	char ancillary[CMSG_SPACE(sizeof inputs)] = {0};
	struct iovec io = {raw, 32};
	struct msghdr msg = {.msg_iov = &io,
			     .msg_iovlen = 1,
			     .msg_control = ancillary,
			     .msg_controllen = sizeof ancillary};
	do {
		e = mc_wait(r, r->config.continuation, 0);
	} while (e > 0);
	if (e)
		return e;
	ssize_t got = recvmsg(r->config.continuation, &msg, MSG_CMSG_CLOEXEC);
	if (got <= 0)
		return got < 0 ? -errno : -EPIPE;
	for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
		if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS ||
		    h->cmsg_len < CMSG_LEN(0))
			continue;
		size_t bytes = h->cmsg_len - CMSG_LEN(0);
		for (size_t j = 0; j < bytes / sizeof(int); j++) {
			int fd;
			memcpy(&fd, (char *)CMSG_DATA(h) + j * sizeof(int), sizeof fd);
			if (count < 4)
				inputs[count++] = fd;
			else
				close(fd);
		}
	}
	if (count != 4 || (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) {
		for (unsigned j = 0; j < count; j++)
			close(inputs[j]);
		return -EPROTO;
	}
	unsigned k = 0;
	for (unsigned i = 0; i < r->config.image_count; i++)
		if (r->config.images[i].segment >= 7) {
			if (k + 2 > count) {
				for (unsigned j = k; j < count; j++)
					close(inputs[j]);
				return -EPROTO;
			}
			r->config.images[i].source = inputs[k++];
			r->config.images[i].backup = inputs[k++];
		}
	if (k != count) {
		for (unsigned j = k; j < count; j++)
			close(inputs[j]);
		return -EPROTO;
	}
	e = got < 32 ? mc_exact(r, r->config.continuation, 0, raw + got, 32 - (size_t)got) : 0;
	if (e)
		return e;
	if (memcmp(raw, "MCCONT04", 8) || get64(raw + 8) != r->config.epoch ||
	    get32(raw + 16) != (uint32_t)r->config.parent ||
	    get32(raw + 20) != r->config.image_count || get64(raw + 24))
		return -EPROTO;
	size_t n = 64 * r->config.image_count;
	e = mc_exact(r, r->config.continuation, 0, raw + 32, n);
	if (e)
		return e;
	struct pollfd p = {r->config.continuation, POLLIN, 0};
	if (poll(&p, 1, 0) < 0)
		return -errno;
	if (p.revents)
		return -EPROTO;
	struct mc_config c = r->config;
	for (unsigned i = 0; i < c.image_count; i++) {
		const uint8_t *q = raw + 32 + 64 * i;
		struct mc_image *v = c.images + i;
		if (get32(q) != i || get32(q + 4))
			return -EPROTO;
		v->offset = get64(q + 8);
		v->target_offset = get64(q + 16);
		v->length = get64(q + 24);
		memcpy(v->sha256, q + 32, 32);
	}
	if ((e = mc_warm_plan_validate(&c)) || (e = roles(&c, 0)))
		return e;
	r->config = c;
	if ((e = mc_revalidate_snapshots(r)) || (e = mc_guard(r)))
		return e;
	int fd = r->config.continuation;
	r->config.continuation = -1;
	if (close(fd))
		return -errno;
	/* Parent snapshot completion cannot stand in for current STOP status. */
	for (unsigned d = 0; d < 3; d++)
		if ((e = status_expected(r, d, CPB_STATUS_STOPPED)))
			return e;
	return 0;
}
static int verify_runtime_entries(struct mc_runtime *r)
{
	static const char *const names[] = {"cptl", "v3phy", "nrphy"};
	for (unsigned segment = 0; segment < 3; segment++) {
		const struct mc_image *image = NULL;
		for (unsigned i = 0; i < r->config.image_count; i++)
			if (r->config.images[i].segment == segment) {
				if (image)
					return -EPROTO;
				image = &r->config.images[i];
			}
		if (!image || image->target_offset || image->length < 0x610)
			return -ENOEXEC;
		unsigned char raw[16];
		int e = mc_guard(r);
		if (e)
			return e;
		ssize_t got = pread(image->target, raw, sizeof raw, 0x600);
		if (got != (ssize_t)sizeof raw)
			return got < 0 ? -errno : -EIO;
		if ((e = mc_guard(r)))
			return e;
		uint32_t first = get32(raw), second = get32(raw + 4), vector = get32(raw + 8);
		if (first != 0xe59ff000 || second != 0xe59ff000 || vector < image->physical_base ||
		    vector >= image->physical_base + image->length)
			return -ENOEXEC;
		fprintf(stderr,
			"cp_entry_verified domain=%s address=%08llx words=%08x,%08x vector=%08x\n",
			names[segment], (unsigned long long)image->physical_base + 0x600, first,
			second, vector);
	}
	return 0;
}
int mc_reload(struct mc_runtime *r)
{
	struct mc_config *c = &r->config;
	int e = roles(c, 1);
	if (e)
		return e;
	/* No source bytes are read before all three real STOP paths succeed. The
	 * sole parent independently establishes old-writer exits and kernel guard. */
	for (unsigned d = 0; d < 3; d++)
		if ((e = operation(r, CPB_STOP, d, 0)))
			return e;
	if ((e = mc_deadline(r, c->continue_ms)) || (e = report(r, "APSTOP04")) ||
	    (e = accept_continuation(r)))
		return e;
	for (unsigned part = 7; part <= 8; part++)
		for (unsigned i = 0; i < c->image_count; i++)
			if (c->images[i].segment == part) {
				if ((e = mc_deadline(r, c->operation_ms)) ||
				    (e = nv_load(r, c->images + i)))
					return e;
			}
	static const unsigned order[] = {0, 5, 1, 2, 3, 4, 6};
	for (unsigned k = 0; k < 7; k++)
		for (unsigned i = 0; i < c->image_count; i++)
			if (c->images[i].segment == order[k]) {
				if ((e = operation(r, CPB_LOAD, 0, c->images + i)))
					return e;
			}
	if ((e = verify_runtime_entries(r)) || (e = mc_load_complete(r)) ||
	    (e = report(r, "APLOAD04")))
		return e;
	for (unsigned d = 0; d < 3; d++)
		if ((e = operation(r, CPB_START, d, 0)))
			return e;
	return report(r, "APSTRT04");
}
