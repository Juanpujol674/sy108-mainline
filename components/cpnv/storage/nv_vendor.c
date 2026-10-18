/* SPDX-License-Identifier: GPL-2.0-only Mirrors modem_control::read_nv_partition.
 * Sources: modem_control SHA256 debf289a161c0c2899b2c2e6bf839ddf2de096799916fb1433028f2aebca2b99,
 * function 0x405dd8; libmodem_utils.so 0x21fc. Unlike stock, I/O errors propagate.
 */
#define _GNU_SOURCE
#include "nv_vendor.h"
#include "nv_codec.h"
#include "nv_checksum_blocks.h"
#include <errno.h>
#include <mtd/ubi-user.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int nv_vendor_valid(const uint8_t *p, size_t n, uint32_t payload, unsigned *bits)
{
	if (!p || !payload || payload > NV_MAX_IMAGE || payload % 512 ||
	    n != (size_t)payload + 512 || get32(p) != NV_IMAGE_MAGIC || get32(p + 4) != payload)
		return 0;
	uint32_t sum = get32(p + 8);
	unsigned mode;
	if (sum == nv_checksum(p + 512, payload, 64))
		mode = 64;
	else if (sum == nv_checksum(p + 512, payload, 32))
		mode = 32;
	else
		return 0;
	if (bits)
		*bits = mode;
	return 1;
}
int nv_vendor_select(const uint8_t *a, int have_a, const uint8_t *b, int have_b, uint32_t payload,
		     unsigned first, unsigned *chosen)
{
	if (first > 1 || !chosen)
		return -EINVAL;
	int good[2] = {have_a && nv_vendor_valid(a, (size_t)payload + 512, payload, NULL),
		       have_b && nv_vendor_valid(b, (size_t)payload + 512, payload, NULL)};
	if (good[first]) {
		*chosen = first;
		return 0;
	}
	if (good[first ^ 1u]) {
		*chosen = first ^ 1u;
		return 0;
	}
	return -EBADMSG;
}
static int timely(uint64_t end)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return -errno;
	uint64_t now = (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
	return now < end ? 0 : -ETIMEDOUT;
}
#define IO_CHUNK 65536u
static int read_at(int fd, uint8_t *p, size_t length, uint64_t base, uint64_t end)
{
	size_t off = 0;
	while (off < length) {
		int e = timely(end);
		if (e)
			return e;
		size_t n = length - off;
		if (n > IO_CHUNK)
			n = IO_CHUNK;
		ssize_t got = pread(fd, p + off, n, (off_t)(base + off));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return got < 0 ? -errno : -EIO;
		off += (size_t)got;
	}
	return timely(end);
}
static int valid_stored_image(int fd, uint32_t payload, uint8_t first[8], uint64_t end)
{
	uint8_t header[NV_HEADER];
	int e = read_at(fd, header, sizeof(header), 0, end);
	if (e)
		return e;
	if (get32(header) != NV_IMAGE_MAGIC || get32(header + 4) != payload)
		return -EBADMSG;
	uint8_t *block = malloc(IO_CHUNK);
	if (!block)
		return -ENOMEM;
	uint64_t sum = 0;
	for (uint32_t off = 0; off < payload;) {
		size_t n = payload - off;
		if (n > IO_CHUNK)
			n = IO_CHUNK;
		if ((e = read_at(fd, block, n, (uint64_t)NV_HEADER + off, end)))
			break;
		if (!off && first)
			memcpy(first, block, 8);
		sum += nv_block_sum(block, n);
		off += (uint32_t)n;
	}
	if (!e && get32(header + 8) != nv_block_checksum(sum, 64) &&
	    get32(header + 8) != nv_block_checksum(sum, 32))
		e = -EBADMSG;
	free(block);
	return e;
}
int nv_vendor_repair_pair(const int fd[2], uint32_t payload, uint64_t end)
{
	if (!fd || fd[0] < 3 || fd[1] < 3 || fd[0] == fd[1] || !payload || payload > NV_MAX_IMAGE ||
	    payload % 512)
		return -EINVAL;
	int good[2], e = 0;
	for (unsigned i = 0; i < 2; i++) {
		int r = valid_stored_image(fd[i], payload, NULL, end);
		if (r == -ETIMEDOUT || r == -ENOMEM)
			return r;
		good[i] = !r;
	}
	if (good[0] && good[1])
		return 0;
	if (!good[0] && !good[1])
		return -EBADMSG;
	unsigned source = good[0] ? 0u : 1u, target = source ^ 1u;
	size_t length = (size_t)payload + NV_HEADER;
	uint8_t *data = malloc(IO_CHUNK), *check = malloc(IO_CHUNK);
	if (!data || !check) {
		e = -ENOMEM;
		goto out;
	}
	int64_t update = (int64_t)length;
	if ((e = timely(end)))
		goto out;
	/* The caller has stopped CP and exclusively owns both volumes. The valid
	 * source remains stable while the invalid replica is streamed and checked. */
	if (ioctl(fd[target], UBI_IOCVOLUP, &update)) {
		e = -errno;
		goto out;
	}
	for (size_t off = 0; off < length;) {
		size_t n = length - off;
		if (n > IO_CHUNK)
			n = IO_CHUNK;
		if ((e = read_at(fd[source], data, n, off, end)))
			goto out;
		size_t done = 0;
		while (done < n) {
			if ((e = timely(end)))
				goto out;
			ssize_t wrote = write(fd[target], data + done, n - done);
			if (wrote < 0 && errno == EINTR)
				continue;
			if (wrote <= 0) {
				e = wrote < 0 ? -errno : -EIO;
				goto out;
			}
			done += (size_t)wrote;
		}
		off += n;
	}
	if (fsync(fd[target])) {
		e = -errno;
		goto out;
	}
	for (size_t off = 0; off < length;) {
		size_t n = length - off;
		if (n > IO_CHUNK)
			n = IO_CHUNK;
		if ((e = read_at(fd[source], data, n, off, end)) ||
		    (e = read_at(fd[target], check, n, off, end)))
			goto out;
		if (memcmp(data, check, n)) {
			e = -EIO;
			goto out;
		}
		off += n;
	}
out:
	if (data) {
		explicit_bzero(data, IO_CHUNK);
		free(data);
	}
	if (check) {
		explicit_bzero(check, IO_CHUNK);
		free(check);
	}
	return e;
}

int nv_vendor_write_image(int fd, const uint8_t *image, uint32_t payload, uint64_t end)
{
	size_t length = (size_t)payload + NV_HEADER;
	if (fd < 3 || !nv_vendor_valid(image, length, payload, NULL))
		return -EINVAL;
	uint8_t *check = malloc(IO_CHUNK);
	if (!check)
		return -ENOMEM;
	int e = timely(end);
	int64_t update = (int64_t)length;
	if (!e && ioctl(fd, UBI_IOCVOLUP, &update))
		e = -errno;
	for (size_t off = 0; !e && off < length;) {
		e = timely(end);
		if (e)
			break;
		size_t n = length - off;
		if (n > IO_CHUNK)
			n = IO_CHUNK;
		ssize_t wrote = write(fd, image + off, n);
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0) {
			e = wrote < 0 ? -errno : -EIO;
			break;
		}
		off += (size_t)wrote;
	}
	if (!e && fsync(fd))
		e = -errno;
	for (size_t off = 0; !e && off < length;) {
		size_t n = length - off;
		if (n > IO_CHUNK)
			n = IO_CHUNK;
		e = read_at(fd, check, n, off, end);
		if (!e && memcmp(check, image + off, n))
			e = -EIO;
		off += n;
	}
	explicit_bzero(check, IO_CHUNK);
	free(check);
	return e;
}

int nv_vendor_initial_timestamps(const int fix_fd[2], int runtime_fd, uint32_t *fix_value,
				 uint32_t *runtime_value, uint64_t deadline)
{
	uint8_t first_words[8], head[1024];
	int e = -EBADMSG;
	unsigned first = (rand() & 1) ? 0u : 1u;
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		e = valid_stored_image(fix_fd[first ^ attempt], 7340032u, first_words, deadline);
		if (e == -ETIMEDOUT || e == -ENOMEM)
			return e;
		if (!e)
			break;
	}
	if (e)
		return e;
	e = read_at(runtime_fd, head, sizeof(head), 0, deadline);
	if (e)
		return e;
	*fix_value = get32(first_words);
	*runtime_value = get32(head + 516);
	return 0;
}
