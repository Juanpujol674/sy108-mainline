/* SPDX-License-Identifier: GPL-2.0-only */
#include "halo_ap_state.h"
void halo_ap_query_frame(uint8_t out[16])
{
	unsigned i;
	for (i = 0; i < 16; i++)
		out[i] = 0;
	out[0] = 0xfc;
	out[1] = 0xab;
	out[2] = 0xa3; /* [9]=0 query, not mark-initialized */
}
void halo_ap_mark_frame(uint8_t out[16])
{
	halo_ap_query_frame(out);
	out[9] = 1;
}
int halo_ap_state_decode(const uint8_t *p, size_t n, unsigned *out)
{
	if (!p || !out || n != 16)
		return -22;
	*out = p[11];
	return 0;
}
int halo_ap_query(const struct halo_ap_io *io, uint64_t generation, unsigned ms,
		  struct halo_ap_result *r)
{
	uint8_t request[16], reply[16];
	uint64_t now, deadline;
	unsigned steps = 0;
	long n;
	int error;
	if (!r)
		return -22;
	{
		uint8_t *p = (uint8_t *)r;
		size_t z = sizeof(*r);
		while (z--)
			*p++ = 0;
	}
	r->generation = generation;
	if (!io || !io->read || !io->write || !io->wait || !io->now || !generation || !ms ||
	    ms > 5000)
		return r->error = -22;
	if (io->now(io->ctx, &now) || now > (uint64_t)INT64_MAX - ms)
		return r->error = -5;
	deadline = now + ms;
	halo_ap_query_frame(request);
	while (r->written < 16 || r->received < 16) {
		unsigned writing = r->written < 16;
		if (++steps > 1024)
			return r->error = -11;
		if (io->now(io->ctx, &now))
			return r->error = -5;
		if (now >= deadline)
			return r->error = -110;
		error = io->wait(io->ctx, writing, deadline);
		if (error == -4)
			continue;
		if (error)
			return r->error = error;
		if (io->now(io->ctx, &now))
			return r->error = -5;
		if (now >= deadline)
			return r->error = -110;
		if (writing) {
			r->write_attempted = 1;
			n = io->write(io->ctx, request + r->written, 16 - r->written);
		} else
			n = io->read(io->ctx, reply + r->received, 16 - r->received);
		/* Never replay a request after failure, timeout or disconnect. A positive
		 * short write advances only the unsent suffix; negative writes are terminal. */
		if (n < 0) {
			if (!writing && (n == -4 || n == -11))
				continue;
			return r->error = (int)n;
		}
		if (!n)
			return r->error = -32;
		if ((size_t)n > 16u - (writing ? r->written : r->received))
			return r->error = -5;
		if (writing)
			r->written += (uint32_t)n;
		else
			r->received += (uint32_t)n;
		/* Scheduling may cross the deadline even with nonblocking descriptors. */
		if (io->now(io->ctx, &now))
			return r->error = -5;
		if (now >= deadline)
			return r->error = -110;
	}
	error = halo_ap_state_decode(reply, sizeof(reply), &r->peer_state);
	if (error)
		return r->error = error;
	r->peer_state_known = r->peer_state == 0 || r->peer_state == 1;
	r->query_complete = 1;
	/* The vendor reads byte 11 without checking response header or correlation.
	 * A complete status reply is not an authenticated license/CP-ready result. */
	return 0;
}
