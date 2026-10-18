/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVS_DEVICES_H
#define NVS_DEVICES_H
#include <stdint.h>
#include <stddef.h>
enum nvs_storage { NVS_STORAGE_UBI = 0 };
struct nvs_profile {
	unsigned enabled, exclusive, checksum_policy, write_policy, timeout_ms, startup_ms, storage;
};
struct nvs_devices {
	int dev_dir, ubi_dir, vol_dir[4], volume[4];
	uint64_t volume_rdev[4], transport_rdev, capacity[4];
};
int nvs_profile_validate(const struct nvs_profile *);
int nvs_open_devices(const struct nvs_profile *, struct nvs_devices *);
void nvs_close_devices(struct nvs_devices *);
#endif
