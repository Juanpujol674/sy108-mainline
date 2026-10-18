/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_WR_PARENT_H
#define UDX_WR_PARENT_H
#include "wr_controller.h"
#include "wr_postload.h"
#include "wr_nv.h"
#include "wr_state.h"
#include "refnotify_transport.h"
struct wr_generation {
	struct rt_child notify;
	struct wr_roles roles;
	struct wr_controller cp;
	struct wr_nv nv;
	struct nvc_epoch epoch;
};
struct wr_parent {
	unsigned notify_enabled;
	int notify_exe;
	struct wr_policy policy;
	int run, owner, boot, cp_owner, self, controller, broker, consumer;
	struct wr_generation current, retired;
	unsigned retired_valid, cancelled;
	int cleanup_error;
	struct wr_state transaction;
	unsigned started, uncertain;
	struct wr_postload postload;
};
int wr_parent_open_sessions(struct nvc_epoch *);
/* Local API: 0 or negative error. Existing argument/state admission failures
 * remain -EPERM. Internal errno-returning checks preserve their cause; legacy
 * epoch 0/-1 validation is mapped to -ESTALE, never interpreted as -EPERM.
 * wait_session distinguishes timeout/cancel from clock/format errors. No
 * wire response, structure layout, authorization or retry permission changes.
 * A return error alone does not prove cleanup; inspect uncertain state. */
int wr_parent_start(struct wr_parent *, uint64_t nonce);
/* Explicit initial-load-halo-v1 only. Requires deployment-provided isolated
 * startup, firmware inputs. Never a warm
 * takeover claim; existing credentials/policy must finish before NV starts. */
int wr_parent_start_initial(struct wr_parent *, uint64_t nonce);
int wr_parent_reload(struct wr_parent *, uint64_t nonce);
/* Reauthentication needs a fresh descriptor profile, loaded by
 * wr_postload_load, with initial-load-halo-v1 (loading=1). Ownership transfers
 * only after the old controller exits ; the
 * supplied object then becomes disabled. Earlier failures leave it untouched.
 * New HALO/factory events must bind to the new generation before NV/notify.
 * The no-profile CLI remains unsupported for an existing HALO session. */
int wr_parent_reload_postload(struct wr_parent *, uint64_t nonce, struct wr_postload *fresh);
int wr_parent_check(struct wr_parent *);
int wr_parent_shutdown(struct wr_parent *, uint64_t deadline);
int wr_parent_cancel(struct wr_parent *, uint64_t deadline);
#endif
