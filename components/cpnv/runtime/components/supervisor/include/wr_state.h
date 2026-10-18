/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_WR_STATE_H
#define UDX_WR_STATE_H
#include <stdint.h>
/* AP-local orchestration states; none of these bytes are a CP wire protocol. */
enum wr_phase {
	WR_DISABLED,
	WR_RUNNING,
	WR_NV_QUIESCING,
	WR_NV_EXITING,
	WR_CONTROL_EXITING,
	WR_STOPPING,
	WR_STOPPED,
	WR_SNAPSHOTS,
	WR_LOADING,
	WR_STARTING,
	WR_NEW_SESSION,
	WR_NV_STARTING,
	WR_READY,
	WR_UNCERTAIN
};
enum wr_event {
	WR_EV_BEGIN = 1,
	WR_EV_QUIESCE_REPORT,
	WR_EV_NV_ALL_EXITED,
	WR_EV_CP_EXITED,
	WR_EV_STOPPED,
	WR_EV_SNAPSHOTS,
	WR_EV_LOAD_COMPLETE,
	WR_EV_START_COMPLETE,
	WR_EV_NEW_SESSION,
	WR_EV_NV_GRANTED,
	WR_EV_FAILURE
};
struct wr_state {
	enum wr_phase phase;
	uint64_t nonce, deadline, last_ms;
	uint32_t old_generation, new_generation;
	unsigned ap_touched, write_possible, prior_nv_ready;
	int error;
};
int wr_state_init(struct wr_state *, uint64_t nonce, uint32_t generation, uint64_t now,
		  uint64_t budget);
/* Initial ownership admission has no old managed NV/controller to quiesce.
 * Starts at STOPPING; never synthesizes old-writer exit events. */
int wr_state_init_initial(struct wr_state *, uint64_t nonce, uint32_t generation, uint64_t now,
			  uint64_t budget);
int wr_state_event(struct wr_state *, enum wr_event, uint64_t now, int error, uint32_t generation);
const char *wr_phase_name(enum wr_phase);
#endif
