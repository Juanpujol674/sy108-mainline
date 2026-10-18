/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_REFNOTIFY_TRANSPORT_H
#define UDX_REFNOTIFY_TRANSPORT_H
#include "refnotify.h"
#include <sys/stat.h>
#define RT_DST 5u
#define RT_CHANNEL 4u
#define RT_BUFID 8u
#define RT_HANDSHAKE_SIZE 24u
struct rt_snapshot {
	uint32_t generation;
};
struct rt_config {
	int notify, session, lease, report, rtc, listener;
	uint64_t epoch;
	uint32_t generation;
	pid_t parent;
	uid_t client_uid;
	unsigned io_timeout_ms;
};
struct rt_sources {
	int notify, session, rtc, listener;
	uint32_t generation;
};
struct rt_child {
	pid_t pid;
	int pidfd, lease, report, executable, session;
	uint64_t epoch, ready_deadline;
	uint32_t generation;
	bool ready, granted, grant_sent, exited;
};
int rt_parse_session(const char *, size_t, struct rt_snapshot *);
int rt_read_session(int, struct rt_snapshot *);
uint64_t rt_now_ms(void);
int rt_run(const struct rt_config *);
int rt_open_sources(struct rt_sources *); /* Production fixed allowlisted metadata/node paths. */
void rt_sources_init(struct rt_sources *);
void rt_sources_close(struct rt_sources *);
void rt_child_init(struct rt_child *);
/* Successful fork consumes/closes all parent source FDs. On *any* error the
 * child object retains its exact live pidfd/lease for mandatory cancellation.
 * Caller supplies an already pinned, trusted static daemon executable FD. */
int rt_child_prepare(struct rt_child *, int executable, struct rt_sources *, uint64_t epoch,
		     uint64_t deadline);
/* Explicit central authorization check runs at the actual grant boundary. */
int rt_child_grant(struct rt_child *, uint64_t deadline, int (*authorize)(void *), void *);
int rt_child_check(const struct rt_child *);
void rt_child_withdraw(struct rt_child *);
int rt_child_revoke(struct rt_child *, uint64_t deadline);
void rt_child_close_exited(struct rt_child *); /* Only after successful revoke. */
#endif
