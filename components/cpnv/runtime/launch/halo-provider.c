/* SPDX-License-Identifier: GPL-2.0-only
 * Source-built private-operation worker for the existing MCSIGN01 adapter.
 * Receives n||d through a private inherited FD; opens no CP or storage path.
 */
#define _GNU_SOURCE
#include "halo_rsa_provider.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <openssl/crypto.h>

static uint32_t get32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const unsigned char *p)
{
	return get32(p) | (uint64_t)get32(p + 4) << 32;
}
static int now_ms(void *ctx, uint64_t *out)
{
	struct timespec t;
	(void)ctx;
	if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0)
		return -EIO;
	*out = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
	return 0;
}
static int unused_clock(void *ctx, uint32_t *out)
{
	(void)ctx;
	(void)out;
	return -EOPNOTSUPP;
}
static int unused_entropy(void *ctx, uint64_t generation, uint64_t deadline, unsigned char out[16])
{
	(void)ctx;
	(void)generation;
	(void)deadline;
	(void)out;
	return -EOPNOTSUPP;
}
static void release(void *ctx, uint64_t generation)
{
	(void)ctx;
	(void)generation;
}
static int fd_number(const char *arg)
{
	char *end;
	errno = 0;
	long n = strtol(arg, &end, 10);
	return errno || !*arg || *end || n < 0 || n > INT_MAX ? -1 : (int)n;
}
static int transfer(int fd, void *buffer, size_t length, int writing, uint64_t deadline)
{
	unsigned char *p = buffer;
	size_t done = 0;
	while (done < length) {
		uint64_t now;
		if (now_ms(NULL, &now))
			return -EIO;
		if (deadline && now >= deadline)
			return -ETIMEDOUT;
		int timeout =
		    deadline ? (int)((deadline - now) > INT_MAX ? INT_MAX : deadline - now) : -1;
		struct pollfd event = {fd, writing ? POLLOUT : POLLIN, 0};
		int rc = poll(&event, 1, timeout);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc <= 0)
			return rc ? -errno : -ETIMEDOUT;
		ssize_t n = writing ? write(fd, p + done, length - done)
				    : read(fd, p + done, length - done);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (!n)
			return !writing && !done ? 1 : -EPIPE;
		if (n < 0)
			return -errno;
		done += (size_t)n;
	}
	return 0;
}
int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--help")) {
		puts("udx-halo-provider --request-fd N --reply-fd N --key-fd N\n"
		     "key FD: private, read-only regular file containing 128-byte big-endian n "
		     "then d.\n"
		     "One private operation per process; pipes remain open until controller EOF.");
		return 0;
	}
	if (argc != 7 || strcmp(argv[1], "--request-fd") || strcmp(argv[3], "--reply-fd") ||
	    strcmp(argv[5], "--key-fd"))
		return 64;
	int rx = fd_number(argv[2]), tx = fd_number(argv[4]), keyfd = fd_number(argv[6]);
	if (rx < 0 || tx < 0 || keyfd < 0 || rx == tx || rx == keyfd || tx == keyfd)
		return 64;
	struct stat key, reader, writer;
	if (fstat(keyfd, &key) || !S_ISREG(key.st_mode) || key.st_uid != geteuid() ||
	    (key.st_mode & 0077) || key.st_nlink != 1 || key.st_size != 256 ||
	    (fcntl(keyfd, F_GETFL) & O_ACCMODE) != O_RDONLY || fstat(rx, &reader) ||
	    !S_ISFIFO(reader.st_mode) || fstat(tx, &writer) || !S_ISFIFO(writer.st_mode) ||
	    (reader.st_dev == writer.st_dev && reader.st_ino == writer.st_ino) ||
	    (fcntl(rx, F_GETFL) & O_ACCMODE) != O_RDONLY ||
	    (fcntl(tx, F_GETFL) & O_ACCMODE) != O_WRONLY)
		return 65;
	struct rlimit core = {0, 0};
	if (setrlimit(RLIMIT_CORE, &core))
		return 65;
	signal(SIGPIPE, SIG_IGN);
	unsigned char material[256] = {0}, request[176] = {0}, reply[176] = {0};
	int result = 1;
	struct halo_rsa_provider rsa = {0};
	struct halo_credential_provider credential;
	if (pread(keyfd, material, sizeof material, 0) != (ssize_t)sizeof material)
		goto done;
	if (close(keyfd))
		goto done;
	keyfd = -1;
	if (!OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, NULL))
		goto done;
	struct halo_material_provider callbacks = {NULL, now_ms,  unused_entropy, unused_clock,
						   NULL, release, material};
	if (halo_rsa_provider_init(&rsa, &callbacks, material, material + 128, &credential))
		goto done;
	OPENSSL_cleanse(material, sizeof material);
	unsigned requests = 0;
	for (;;) {
		/* Idle EOF is normal cancellation. Once a frame begins, bound its remainder. */
		int e = transfer(rx, request, 1, 0, 0);
		if (e == 1) {
			result = 0;
			break;
		}
		if (e)
			break;
		uint64_t now;
		if (now_ms(NULL, &now))
			break;
		if (transfer(rx, request + 1, sizeof request - 1, 0, now + 15000))
			break;
		uint64_t epoch = get64(request + 16), deadline = get64(request + 24);
		if (requests++ || memcmp(request, "MCSIGN01", 8) || get32(request + 8) != 1 ||
		    get32(request + 12) != 128 || !epoch || get32(request + 32) != 1 ||
		    get32(request + 36) || get32(request + 40) || get32(request + 44))
			break;
		if (now_ms(NULL, &now) || deadline <= now || deadline - now > 15000)
			break;
		memcpy(reply, request, 48);
		e = rsa.material.private_operation(rsa.material.ctx, epoch, deadline, request + 48,
						   reply + 48);
		if (e) {
			reply[40] = 1;
			OPENSSL_cleanse(reply + 48, 128);
		}
		if (transfer(tx, reply, sizeof reply, 1, deadline))
			break;
		OPENSSL_cleanse(request, sizeof request);
		OPENSSL_cleanse(reply, sizeof reply);
		/* Do not close the reply pipe here: controller rejects a concurrent HUP. */
	}
done:
	if (keyfd >= 0)
		close(keyfd);
	halo_rsa_provider_clear(&rsa);
	OPENSSL_cleanse(material, sizeof material);
	OPENSSL_cleanse(request, sizeof request);
	OPENSSL_cleanse(reply, sizeof reply);
	if (result)
		fputs("halo-provider: private operation stream failed\n", stderr);
	return result;
}
