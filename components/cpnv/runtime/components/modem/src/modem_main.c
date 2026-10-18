/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "modem_control.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
static void put32(uint8_t *p, uint32_t x)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (uint8_t)(x >> (i * 8));
}
static void put64(uint8_t *p, uint64_t x)
{
	for (unsigned i = 0; i < 8; i++)
		p[i] = (uint8_t)(x >> (i * 8));
}
int mc_event(struct mc_runtime *r, unsigned type, int error)
{
	uint8_t out[64] = {0};
	memcpy(out, "MCEVENT2", 8);
	put32(out + 8, type);
	put32(out + 12, (uint32_t)error);
	put64(out + 16, r->config.epoch);
	put64(out + 24, r->last_now);
	put32(out + 32, r->status_generation);
	put32(out + 36, r->halo_generation);
	put32(out + 44, r->ap_touched);
	put32(out + 48, r->auth_complete);
	put32(out + 52, r->factory_accepted);
	put32(out + 56, r->feature_validated);
	put32(out + 60, r->mark_completed && !r->maintenance_error);
	return mc_exact(r, r->config.events, 1, out, sizeof out);
}
static char scan[32];
static size_t scan_n;
static unsigned scan_input(const uint8_t *input, size_t n)
{
	static const char *const token[] = {"Modem Alive", "Modem Assert",  "Modem Reset",
					    "HUNG",	   "Modem Blocked", "AGDSP Assert"};
	unsigned seen = 0;
	for (size_t j = 0; j < n; j++) {
		if (scan_n == sizeof scan - 1)
			memmove(scan, scan + 1, --scan_n);
		scan[scan_n++] = (char)input[j];
		scan[scan_n] = 0;
		for (unsigned k = 0; k < sizeof token / sizeof token[0]; k++) {
			size_t len = strlen(token[k]);
			if (scan_n >= len && !memcmp(scan + scan_n - len, token[k], len))
				seen |= 1u << k;
		}
	}
	return seen;
}
int mc_check_faults(struct mc_runtime *r)
{
	for (unsigned i = 0; i < 16; i++) {
		struct pollfd p = {r->status_fd, POLLIN, 0};
		if (poll(&p, 1, 0) < 0)
			return -errno;
		if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
			return -EPIPE;
		if (!(p.revents & POLLIN))
			return 0;
		uint8_t input[256];
		ssize_t n = read(r->status_fd, input, sizeof input);
		if (n < 0)
			return errno == EAGAIN ? 0 : -errno;
		if (!n)
			return -EPIPE;
		if (scan_input(input, (size_t)n) & ~1u)
			return -EHOSTDOWN;
	}
	return -EOVERFLOW;
}
int mc_alive(struct mc_runtime *r, int wait_alive)
{
	unsigned steps = 0;
	for (;;) {
		if (++steps > 65536)
			return -EOVERFLOW;
		int e = mc_wait(r, r->status_fd, 0);
		if (e < 0)
			return e;
		if (e)
			continue;
		uint8_t input[256];
		ssize_t n = read(r->status_fd, input, sizeof input);
		int saved = errno;
		e = mc_guard(r);
		if (e)
			return e;
		if (n < 0) {
			if (saved == EINTR || saved == EAGAIN)
				continue;
			return -saved;
		}
		if (!n)
			return -EPIPE;
		r->status_received += (uint64_t)n;
		unsigned seen = scan_input(input, (size_t)n);
		if (!wait_alive)
			for (unsigned k = 0; k < 6; k++)
				if (seen & (1u << k)) {
					if ((e = mc_event(r, 10 + k, 0)))
						return e;
				}
		if (seen & ~1u)
			return -EHOSTDOWN;
		if (wait_alive && (seen & 1))
			return 0;
	}
}
static int initial_alive(struct mc_runtime *r)
{
	int e = mc_deadline(r, r->config.alive_ms);
	if (e)
		return e;
	scan_n = 0;
	r->status_received = 0;
	for (;;) {
		e = mc_open_status(r);
		if (e)
			return e;
		e = mc_alive(r, 1);
		if ((e != -EPIPE && e != -ENODEV) || r->status_received || r->halo_written ||
		    r->halo_received)
			return e;
		/* No status frame or HALO request has been consumed/sent. OPEN/INIT may
		 * invalidate an early open; close it and wait for the real channel again. */
		mc_close_status(r);
		e = mc_wait(r, -1, 0);
		if (e < 0)
			return e;
	}
}
int mc_run(struct mc_runtime *r)
{
	if (!r)
		return -EINVAL;
	int e;
	if (r->config.reload) {
		e = mc_reload(r);
		if (e)
			goto done;
	} else if ((e = mc_preloaded_adopt(r)))
		goto done;
	if (!r->config.postload_mode) {
		for (;;) {
			e = mc_monitor_started(r);
			if (e)
				goto done;
			for (unsigned i = 0; i < 10; i++) {
				e = mc_wait(r, -1, 0);
				if (e < 0)
					goto done;
			}
		}
	}
	e = initial_alive(r);
	if (e)
		goto done;
	r->monitor_faults = 1;
	e = mc_open_halo(r);
	if (e)
		goto done;
	if ((e = mc_event(r, 1, 0)) || (e = mc_event(r, 10, 0)))
		goto done;
	if (r->config.halo_delay_ms) {
		uint64_t target = 0;
		if (mc_now(&target)) {
			e = -EIO;
			goto done;
		}
		target += r->config.halo_delay_ms;
		if ((e = mc_deadline(r, r->config.halo_delay_ms + r->config.operation_ms)))
			goto done;
		while (r->last_now < target) {
			e = mc_wait(r, -1, 0);
			if (e < 0)
				goto done;
		}
	}
	e = mc_halo(r);
	if (e)
		goto done;
	e = mc_event(r, 2, 0);
	if (e)
		goto done;
	for (;;) {
		if ((e = mc_halo_maintain(r)))
			break;
		if ((e = mc_deadline(r, 1000)))
			break;
		e = mc_alive(r, 0);
		if (e != -ETIMEDOUT)
			break;
	}
done:
	fprintf(stderr,
		"modem=stopped error=%d ap_touched=%u auth_exchange=%u factory_accepted=%u "
		"feature_validated=%u first_mark=%u maintenance_error=%d status_received=%llu "
		"halo_written=%llu halo_received=%llu\n",
		e, r->ap_touched, r->auth_complete, r->factory_accepted, r->feature_validated,
		r->mark_completed, r->maintenance_error, (unsigned long long)r->status_received,
		(unsigned long long)r->halo_written, (unsigned long long)r->halo_received);
	mc_close(r);
	return e;
}
#ifndef UDX_MC_NO_MAIN
int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--help")) {
		puts("udx-modem-control-warm --profile-fd N\nDefault disabled. Borrowed "
		     "descriptors; no pathname configuration, shell, reboot, or automatic reload. "
		     "Profile ABI UDXMC004; STOP/continue only for reload. Explicit preloaded "
		     "postload supported.");
		return 0;
	}
	if (argc != 3 || strcmp(argv[1], "--profile-fd"))
		return 64;
	char *end;
	errno = 0;
	long fd = strtol(argv[2], &end, 10);
	if (errno || *end || fd < 3 || fd > 0x7fffffff)
		return 64;
	struct stat st;
	if (fstat((int)fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 0077) || st.st_nlink > 1 ||
	    (fcntl((int)fd, F_GETFL) & O_ACCMODE) != O_RDONLY || st.st_size < 256 ||
	    st.st_size > 256 + MC_MAX_IMAGES * MC_IMAGE_RECORD)
		return 65;
	uint8_t raw[256 + MC_MAX_IMAGES * MC_IMAGE_RECORD];
	ssize_t got = pread((int)fd, raw, sizeof raw, 0);
	if (got != st.st_size)
		return 65;
	struct mc_runtime r = {0};
	r.status_fd = r.halo_fd = r.halo_tx = -1;
	int e = mc_warm_profile_decode(raw, (size_t)got, &r.config);
	memset(raw, 0, sizeof raw);
	if (e)
		return 64;
	close((int)fd);
	e = mc_validate(&r);
	if (e) {
		fprintf(stderr, "modem=denied error=%d\n", e);
		return 65;
	}
	return mc_run(&r) ? 66 : 0;
}
#endif
