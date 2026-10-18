/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_WR_FACTORY_H
#define UDX_WR_FACTORY_H
#include "modem_control.h"
#include "modem_image.h"
#include "nvs_devices.h"
#define WR_FIRMWARE_COUNT 6u
struct wr_firmware {
	unsigned segment, format;
	uint64_t offset, length, target_offset;
	unsigned char sha256[32];
};
struct wr_policy {
	unsigned enabled, firmware_ubi;
	struct nvs_profile nv;
	unsigned count;
	struct wr_firmware firmware[WR_FIRMWARE_COUNT];
};
struct wr_factory {
	struct mc_config config;
	uint64_t firmware_bytes[WR_FIRMWARE_COUNT];
	int directory, firmware[WR_FIRMWARE_COUNT], output[MC_MAX_IMAGES],
	    backup_output[MC_MAX_IMAGES];
	struct mci_extent extent[MC_MAX_IMAGES];
	unsigned source_index[MC_MAX_IMAGES], caps_dropped;
};
int wr_policy_parse(const char *, size_t, struct wr_policy *);
int wr_policy_validate(const struct wr_policy *, int reload);
int wr_factory_preloaded(struct wr_factory *, int owner, int lease, int phase, uint64_t nonce,
			 const struct wr_policy *);
int wr_factory_prepare(struct wr_factory *, int directory, int owner, int lease, int phase,
		       int report, int continuation, uint64_t nonce, const struct wr_policy *);
/* Explicit initial-load only: requires exact canonical AP modem=shutdown hint. */
int wr_factory_prepare_initial(struct wr_factory *, int directory, int owner, int lease, int phase,
			       int report, int continuation, uint64_t nonce,
			       const struct wr_policy *);
int wr_factory_snapshots(struct wr_factory *, const struct wr_policy *, uint64_t deadline);
void wr_factory_drop_child_caps(struct wr_factory *);
void wr_factory_close(struct wr_factory *);
void wr_factory_remove_loaded_snapshots(struct wr_factory *);
int wr_open_readonly_nv(const struct nvs_profile *, struct nvs_devices *);
#endif
