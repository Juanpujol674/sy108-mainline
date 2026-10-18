/* SPDX-License-Identifier: GPL-2.0-only Live native owner and AT generation admission. */
#include "cellular.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int owner_query(struct owner_status *out)
{
	out->pidfd = -1;
	char nonce[33], request[48], response[514];
	if (random_hex(nonce, 16) < 0)
		return -1;
	snprintf(request, sizeof(request), "status %s\n", nonce);
	pid_t pid;
	int pinned = -1;
	int n = seqpacket_pinned(CELL_OWNER_SOCKET, request, strlen(request), response,
				 sizeof(response), 2000, &pid, &pinned);
	if (n < 0)
		return -1;
	/* Bounded decimal fields are parsed as text before conversion, avoiding
	 * scanf integer overflow and acceptance of negative unsigned values. */
	char echoed[33], epoch[17], generation[4][21], ready[11], uncertain[11], error[12],
	    armed[11], nv[11], halo[11], notify[11], storage[8];
	int consumed = 0;
	int fields = sscanf(
	    response,
	    "CPNV_STATUS1 nonce=%32[0-9a-f] ready=%10[0-9] uncertain=%10[0-9] error=%11[-0-9] "
	    "epoch=%16[0-9a-f] generation=%20[0-9],%20[0-9],%20[0-9],%20[0-9] armed=%10[0-9] "
	    "nv_live=%10[0-9] halo=%10[0-9] notify=%10[0-9] storage=%7[a-z]\n%n",
	    echoed, ready, uncertain, error, epoch, generation[0], generation[1], generation[2],
	    generation[3], armed, nv, halo, notify, storage, &consumed);
	uint64_t gen[4];
	bool valid = n <= 512 && fields == 14 && consumed == n && response[n - 1] == '\n' &&
		     text_equal(echoed, nonce) && hex_string(epoch, 16) &&
		     strcmp(epoch, "0000000000000000") && text_equal(ready, "1") &&
		     text_equal(uncertain, "0") && text_equal(error, "0") && text_equal(nv, "1") &&
		     text_equal(halo, "1") && integer_text(notify, 0, 1, NULL) &&
		     integer_text(armed, 0, UINT32_MAX, NULL) && text_equal(storage, "ubi");
	for (size_t i = 0; valid && i < 4; i++) {
		errno = 0;
		char *end;
		unsigned long long v = strtoull(generation[i], &end, 10);
		if (errno || *end)
			valid = false;
		else
			gen[i] = v;
	}
	if (!valid) {
		close(pinned);
		return errorf(CELL_SYSTEM, "Native CP/NV owner is not ready or has invalid status");
	}
	char *session = read_text(CELL_AT_SESSION, 256, true);
	if (!session) {
		close(pinned);
		return errorf(CELL_SYSTEM, "AT session is unavailable");
	}
	char version[11], destination[11], channel[11], state[11], generation_text[21];
	consumed = 0;
	fields = sscanf(session, "%10[0-9] %10[0-9] %10[0-9] %20[0-9] %10[0-9]%n", version,
			destination, channel, generation_text, state, &consumed);
	valid = fields == 5 && !session[consumed] && text_equal(version, "1") &&
		text_equal(destination, "5") && text_equal(channel, "6") && text_equal(state, "1");
	uint64_t selected = 0;
	if (valid) {
		errno = 0;
		char *end;
		selected = strtoull(generation_text, &end, 10);
		valid = !errno && !*end && selected == gen[1];
	}
	free(session);
	if (!valid || pidfd_exited(pinned)) {
		close(pinned);
		return errorf(CELL_SYSTEM, "AT generation or native owner is no longer ready");
	}
	out->pid = pid;
	out->pidfd = pinned;
	out->generation = selected;
	memcpy(out->epoch, epoch, 17);
	return 0;
}
int owner_check(struct owner *owner)
{
	struct owner_status current;
	if (owner_query(&current) < 0)
		return -1;
	if (owner->pidfd < 0) {
		owner->pidfd = current.pidfd;
		owner->identity = current;
		owner->identity.pidfd = -1;
	} else {
		close(current.pidfd);
		if (owner->identity.pid != current.pid ||
		    strcmp(owner->identity.epoch, current.epoch) ||
		    owner->identity.generation != current.generation)
			return errorf(CELL_SYSTEM, "Native CP session changed");
	}
	if (pidfd_exited(owner->pidfd))
		return errorf(CELL_SYSTEM, "Native CP owner exited");
	return 0;
}
void owner_close(struct owner *owner)
{
	if (owner->pidfd >= 0)
		close(owner->pidfd);
	owner->pidfd = -1;
}
