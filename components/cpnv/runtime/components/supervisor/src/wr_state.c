/* SPDX-License-Identifier: GPL-2.0-only */
#include "wr_state.h"
#include <errno.h>
#include <string.h>
const char *wr_phase_name(enum wr_phase p)
{
	static const char *const names[] = {
	    "disabled",	   "running",	  "nv-quiescing", "nv-exiting", "control-exiting",
	    "stopping",	   "stopped",	  "snapshots",	  "loading",	"starting",
	    "new-session", "nv-starting", "ready",	  "uncertain"};
	return (unsigned)p < sizeof names / sizeof names[0] ? names[p] : "invalid";
}
int wr_state_init(struct wr_state *s, uint64_t nonce, uint32_t generation, uint64_t now,
		  uint64_t budget)
{
	if (!s || !nonce || !now || !budget || budget > 600000 || now > UINT64_MAX - budget)
		return -EINVAL;
	memset(s, 0, sizeof *s);
	s->phase = WR_RUNNING;
	s->prior_nv_ready = 1;
	s->nonce = nonce;
	s->old_generation = generation;
	s->last_ms = now;
	s->deadline = now + budget;
	return 0;
}
int wr_state_init_initial(struct wr_state *s, uint64_t nonce, uint32_t generation, uint64_t now,
			  uint64_t budget)
{
	int e = wr_state_init(s, nonce, generation, now, budget);
	if (!e) {
		s->phase = WR_STOPPING;
		s->prior_nv_ready = 0;
	}
	return e;
}
static int uncertain(struct wr_state *s, int e)
{
	s->phase = WR_UNCERTAIN;
	s->error = e ? e : -EIO;
	return s->error;
}
int wr_state_event(struct wr_state *s, enum wr_event event, uint64_t now, int error,
		   uint32_t generation)
{
	if (!s || s->phase == WR_DISABLED)
		return -EINVAL;
	if (s->phase == WR_UNCERTAIN)
		return s->error;
	/* A failure is terminal even if recording it occurs after the deadline or
	 * the clock fails. Preserve its cause; this grants no timed transition. */
	if (event == WR_EV_FAILURE)
		return uncertain(s, error);
	if (!now || now < s->last_ms || now >= s->deadline)
		return uncertain(s, -ETIMEDOUT);
	s->last_ms = now;
	if (error)
		return uncertain(s, error);
	switch (s->phase) {
	case WR_RUNNING:
		if (event != WR_EV_BEGIN)
			break;
		s->phase = WR_NV_QUIESCING;
		return 0;
	case WR_NV_QUIESCING:
		if (event != WR_EV_QUIESCE_REPORT)
			break;
		s->phase = WR_NV_EXITING;
		return 0;
	case WR_NV_EXITING:
		if (event != WR_EV_NV_ALL_EXITED)
			break;
		s->phase = WR_CONTROL_EXITING;
		return 0;
	case WR_CONTROL_EXITING:
		if (event != WR_EV_CP_EXITED)
			break;
		s->phase = WR_STOPPING;
		return 0;
	case WR_STOPPING:
		if (event != WR_EV_STOPPED)
			break;
		s->ap_touched = 1;
		s->phase = WR_STOPPED;
		return 0;
	case WR_STOPPED:
		if (event != WR_EV_SNAPSHOTS)
			break;
		s->phase = WR_LOADING;
		return 0;
	case WR_LOADING:
		if (event != WR_EV_LOAD_COMPLETE)
			break;
		s->ap_touched = 1;
		s->phase = WR_STARTING;
		return 0;
	case WR_STARTING:
		if (event != WR_EV_START_COMPLETE)
			break;
		s->phase = WR_NEW_SESSION;
		return 0;
	case WR_NEW_SESSION:
		if (event != WR_EV_NEW_SESSION)
			break;
		s->new_generation = generation;
		s->phase = WR_NV_STARTING;
		return 0;
	case WR_NV_STARTING:
		if (event != WR_EV_NV_GRANTED)
			break;
		s->write_possible = 1;
		s->phase = WR_READY;
		return 0;
	default:
		break;
	}
	return uncertain(s, -EPROTO);
}
