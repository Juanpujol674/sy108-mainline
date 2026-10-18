/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_MODEM_CONTROL_H
#define UDX_MODEM_CONTROL_H
#include <stdint.h>
#include <stddef.h>
#include "cproc_backend.h"
#define MC_MAX_IMAGES 14u
#define MC_VERIFICATION_BYTES 136u
#define MC_PROFILE_HEADER 256u
#define MC_IMAGE_RECORD 96u
#define MC_WARM_REPORT_BYTES 32u
#define MC_WARM_CONT_HEADER 32u
#define MC_WARM_CONT_RECORD 64u
enum mc_postload_mode { MC_CONTROL_ONLY = 0, MC_HALO_NORMAL = 1 };
/* Explicit AP-local borrowed-fd profile, not a vendor or CP wire structure. */
struct mc_image {
	unsigned domain, segment;
	int source, target, backup;
	uint64_t offset, target_offset, length, capacity, physical_base;
	uint8_t sha256[32];
};
struct mc_config {
	unsigned reload, pmic, image_count, operation_ms, alive_ms, halo_ms, factory_tail,
	    halo_delay_ms, continue_ms, postload_mode;
	uint64_t epoch;
	int parent, owner, lease, devdir, status_session, halo_session, provider_rx, provider_tx,
	    verification_params, events, phase, cmdline;
	int status[4], start[4], stop[4];
	int report, continuation;
	struct mc_image images[MC_MAX_IMAGES];
};
struct mc_runtime {
	struct mc_config config;
	uint64_t deadline, last_now, status_received, halo_written, halo_received, feature_bits,
	    next_mark_ms;
	int status_fd, halo_fd, halo_tx;
	uint32_t status_generation, halo_generation;
	unsigned channels_armed, monitor_faults, ap_touched, auth_complete, factory_accepted,
	    feature_validated, feature_status, mark_completed, mark_reply_state;
	int maintenance_error;
	struct cpb_io cp;
	unsigned preloaded_bound, preloaded_status[3];
};
int mc_warm_profile_decode(const uint8_t *, size_t, struct mc_config *);
int mc_warm_profile_encode(const struct mc_config *, uint8_t *, size_t, size_t *);
int mc_validate(struct mc_runtime *);
/* Production controller execution after decoded profile and mc_validate success. */
int mc_run(struct mc_runtime *);
int mc_warm_plan_validate(const struct mc_config *);
int mc_warm_continue_encode(const struct mc_config *, uint8_t *, size_t, size_t *);
int mc_revalidate_snapshots(struct mc_runtime *);
int mc_guard(struct mc_runtime *);
int mc_now(uint64_t *);
int mc_deadline(struct mc_runtime *, unsigned);
int mc_wait(struct mc_runtime *, int, unsigned);
int mc_exact(struct mc_runtime *, int, int, void *, size_t);
int mc_open_status(struct mc_runtime *);
int mc_open_halo(struct mc_runtime *);
void mc_close_status(struct mc_runtime *);
int mc_load_complete(struct mc_runtime *);
int mc_reload(struct mc_runtime *);
int mc_monitor_started(struct mc_runtime *);
int mc_preloaded_adopt(struct mc_runtime *);
int mc_provider_rpc(struct mc_runtime *, unsigned, const uint8_t *, size_t, uint8_t *, size_t);
struct halo_credential_provider;
/* Source-level provider entry; no mandatory external signer service. */
int mc_halo_with_provider(struct mc_runtime *, const struct halo_credential_provider *,
			  const uint8_t feature_key[8]);
int mc_halo_maintain(struct mc_runtime *);
int mc_halo(struct mc_runtime *);
int mc_check_faults(struct mc_runtime *);
int mc_alive(struct mc_runtime *, int);
int mc_event(struct mc_runtime *, unsigned, int);
void mc_close(struct mc_runtime *);
#endif
