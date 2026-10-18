/* SPDX-License-Identifier: GPL-2.0-only */
#include "modem_control.h"
#include "halo_peer_gate.h"
#include "halo_ap_state.h"
#include <errno.h>
#include <stdint.h>
int mc_halo_wait_peer_zero(struct mc_runtime *r, unsigned *raw)
{
	if (raw)
		*raw = 0;
	if (!r || !raw)
		return -EINVAL;
	for (unsigned queries = 0; queries < 65536u; ++queries) {
		uint8_t query[16], reply[16];
		unsigned observed = 0;
		int e = mc_guard(r);
		if (e)
			return e;
		halo_ap_query_frame(query);
		e = mc_exact(r, r->halo_tx, 1, query, sizeof query);
		if (e)
			return e;
		e = mc_exact(r, r->halo_fd, 0, reply, sizeof reply);
		if (e)
			return e;
		e = halo_ap_state_decode(reply, sizeof reply, &observed);
		if (e)
			return e;
		e = mc_guard(r);
		if (e)
			return e;
		if (!observed) {
			*raw = observed;
			return 0;
		}
		/* Every nonzero byte waits; it is not inherited authentication. */
		if (r->last_now > UINT64_MAX - (uint64_t)MC_HALO_REQUERY_MS)
			return -EOVERFLOW;
		const uint64_t retry_at = r->last_now + (uint64_t)MC_HALO_REQUERY_MS;
		while (r->last_now < retry_at) {
			e = mc_wait(r, -1, 0);
			if (e < 0)
				return e;
			/* Do not assume a nonnegative wait means the lease is still live.
			 * The guard owns clock/session/lease and deadline validation. */
			e = mc_guard(r);
			if (e)
				return e;
		}
	}
	return -ELOOP;
}
