/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_SESSION_MONITOR_H
#define UDX_SESSION_MONITOR_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#define UDX_SNAPSHOT_COUNT 4
#define UDX_INITIAL_SESSION_MS UINT64_C(120000)
enum udx_session_reason {
	UDX_SESSION_OK,
	UDX_SESSION_READ,
	UDX_SESSION_FORMAT,
	UDX_SESSION_TARGET,
	UDX_SESSION_GENERATION,
	UDX_SESSION_LOST,
	UDX_SESSION_BLOCKED,
	UDX_SESSION_DRIVER_ERROR,
	UDX_SESSION_TIMEOUT,
	UDX_SESSION_PROVIDER,
	UDX_SESSION_STOPPING
};
struct udx_snapshot {
	uint32_t generation, state, blocked;
	int32_t error;
};
struct udx_session_monitor {
	uint32_t generation[4];
	unsigned char armed[4];
	dev_t device[4];
	ino_t inode[4];
	uint64_t deadline;
	int started;
	int fault_index;
	enum udx_session_reason reason;
	int error;
};
extern const char *const udx_snapshot_paths[4];
int udx_parse_snapshot(unsigned index, const char *text, size_t size, struct udx_snapshot *out);
#endif
