/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPNV_LINUX_H
#define CPNV_LINUX_H
#include "cpnv_lifecycle.h"
#include "cpnv_observer.h"
#include <sys/types.h>

/* Actual Linux syscall backend, with no CP/NV/device or socket operations.
 * Private duplicated descriptors belong exclusively to opaque objects.
 * Return convention: 0 success/live, 1 observed pidfd exit, negative errno.
 * It does not authenticate a firmware, executable image, controller or grant. */
struct cpb_process;
struct cpb_owner;
struct cpb_record;
struct cpb_process_fact {
	uint64_t token, exe_device, exe_inode;
	uint32_t pid, parent, uid;
	unsigned exited;
};
struct cpb_file_identity {
	uint64_t device, inode, rdev;
	uint32_t mode;
};
struct cpb_alias_fact {
	uint32_t pid, tid, fd, target_index;
};
int cpb_file_identity_from_fd(int, struct cpb_file_identity *);
/* Scan only this already bound process's actual per-thread fd tables. 1 means
 * an alias was observed, 0 none found during this scan; negative errno is unknown.
 * NOT a system-wide or atomic absence proof; never excludes respawners. */
int cpb_process_find_alias(struct cpb_process *, const struct cpb_file_identity *, unsigned count,
			   struct cpb_alias_fact *);
/* Caller supplies an already-acquired pidfd, not just a potentially recycled
 * PID. The expected executable FD is independently selected by the caller.
 * Both are duplicated; original FDs remain caller-owned. This entry accepts
 * only a direct child (expected_parent == getpid()). Numeric parent identity
 * alone is insufficient for grandchildren; use the bound-parent entry. */
int cpb_process_bind(int supplied_pidfd, int expected_exe_fd, pid_t pid, pid_t expected_parent,
		     uid_t expected_uid, uint64_t token, struct cpb_process **);
/* Retains the parent's incarnation/liveness pidfd, not a copy of all parent
 * role checks. The caller must also observe the complete bound parent role
 * to detect later parent exec/credential changes. */
int cpb_process_bind_descendant(int supplied_pidfd, int expected_exe_fd, pid_t pid,
				struct cpb_process *bound_parent, uid_t expected_uid,
				uint64_t token, struct cpb_process **);
/* Exit facts contain the historical binding's exe/parent/uid, not a claim
 * about the final image/credentials. pidfd identity does not freeze exec. */
int cpb_process_observe(struct cpb_process *, struct cpb_process_fact *);
/* Signals only this bound process using its private pidfd; never kill(pid).
 * Exec/credentials can change between observation and signal. This request
 * neither confirms exit nor proves CP STOP, DMA idle or storage quiescence. */
int cpb_process_request_stop(struct cpb_process *);
/* Cancellation-only operations on an already verified, retained incarnation.
 * They do NOT reauthorize the current exe/UID/parent. Facts are historical;
 * 0 from observe_exit means not yet exited, NEVER "live role authorized".
 * No numeric PID lookup/signal fallback. Intended for abnormal-parent/exec
 * changes where strict role observation fails but the old pidfd is retained. */
int cpb_process_observe_exit(struct cpb_process *, struct cpb_process_fact *);
int cpb_process_cancel_retained(struct cpb_process *);
int cpb_process_wait_exit_only(struct cpb_process *, uint64_t deadline_ms,
			       struct cpb_process_fact *);

enum cpb_reap_state { CPB_REAP_UNSET = 0, CPB_REAP_COLLECTED = 1, CPB_REAP_NO_WAITABLE_CHILD = 2 };
struct cpb_reap_fact {
	uint64_t token;
	uint32_t pid, uid;
	int code, status;
	unsigned state, collected_now;
};
/* Nonblocking exact-pidfd collection for a caller-owned direct child. The raw
 * fd must be retained from this caller's own fork (before any reaper), or be
 * privately bound already; callers serialize its lifetime. No PID lookup or
 * waitpid fallback. ECHILD is accepted ONLY with the same pidfd's exit proof,
 * and means NO_WAITABLE_CHILD with unknown status, never "we collected it".
 * __WALL includes clone exit-signal variants without widening the chosen pidfd.
 * -EAGAIN means no terminal wait result yet; other negative errno are failures.
 * This API does not authorize a process, CP action, writer or authentication. */
int cpb_pidfd_reap_child(int retained_pidfd, pid_t expected_pid, struct cpb_reap_fact *);
/* Direct-child bound object only; same observer required. Descendants are
 * rejected even if later adopted by a subreaper. Cached COLLECTED facts retain
 * their original status; collected_now is zero on an idempotent repeat. */
int cpb_process_reap_child(struct cpb_process *, struct cpb_reap_fact *);

int cpb_monotonic_ms(uint64_t *);
/* Actual bounded pidfd wait. Deadline is absolute CLOCK_MONOTONIC ms. A timeout
 * retains the opaque handle; it does not release identities or prove exit. */
int cpb_process_wait_exited(struct cpb_process *, uint64_t deadline_ms, struct cpb_process_fact *);
/* Release identities only after pidfd exit confirmation. No waitpid/reaping;
 * the process's real parent retains that responsibility. */
int cpb_process_release_exited(struct cpb_process **);
/* Real observations mapped to the retained lifecycle ledger. Partial live
 * results are PENDING. Errors return negative errno AND a current-sequence
 * failure receipt with that exact errno and zero success bits. The caller must
 * never reuse a token across process incarnations and must select the correct
 * retained object for each ledger role: objects do not themselves carry epoch. */
int cpb_collect_exits(const struct lc_state *, struct cpb_process *const[LC_ROLES],
		      struct lc_completion *);

/* Private owned directory required (effective uid, mode 0700). Regular 0600
 * lockfile, one hard link, O_NOFOLLOW, flock EX|NB, post-lock path identity.
 * The file is never truncated, rewritten or unlinked. Retained path permits
 * normal cooperating-owner reacquisition after its LAST open-description alias
 * closes. A fork can inherit an alias despite CLOEXEC; release closes only our
 * private descriptor. An inherited alias can also explicitly LOCK_UN; check
 * only checks metadata, not continuous lock ownership. Isolate inherited FDs
 * before child code runs and stabilize the directory's global path. */
int cpb_owner_acquire_at(int private_directory, const char *leaf, struct cpb_owner **);
int cpb_owner_path_identity_check(struct cpb_owner *);
int cpb_owner_release(struct cpb_owner **);
/* Single-purpose post-fork/pre-exec drop of this object's inherited FDs.
 * Original owner process gets -EPERM; ordinary check/release in a child get
 * -EXDEV. Only getpid/close and memory assignments are used: no allocator call.
 * The copied allocation is deliberately left for exec/_exit to reclaim.
 * Other aliases duplicated by the application still require FD isolation. */
int cpb_owner_detach_in_fork_child(struct cpb_owner **);

/* Bounded actual regular-file read: no-follow leaf, exact filesystem type,
 * pinned inode/type/mode/uid and before/after-read metadata checks. Read-only
 * mode does NOT freeze contents or pre-existing writer FDs. No atomic four-
 * channel snapshot is promised. The session wrapper requires real sysfs. */
int cpb_record_open_at(int directory, const char *leaf, long filesystem_type, uid_t expected_uid,
		       struct cpb_record **);
int cpb_session_open_at(int sysfs_directory, const char *leaf, struct cpb_record **);
int cpb_record_snapshot(struct cpb_record *, unsigned channel_index, struct cpnv_snapshot *,
			struct cpnv_provider *);
int cpb_record_close(struct cpb_record **);
int cpb_sessions_sample(struct cpb_record *const[4], struct cpnv_snapshot[4],
			struct cpnv_provider[4]);
#endif
