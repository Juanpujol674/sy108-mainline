/* SPDX-License-Identifier: GPL-2.0-only
 * SY108 service/event owner. It is the only local PMIC lease
 * claimant. The main event loop checks health and renews the PMIC lease.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/magic.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <syslog.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "root-health.h"

#define RUNTIME_SOCKET "/run/udx-runtime.sock"
#define CPNV_SOCKET "/run/udx-cpnv-warm/control.sock"
#define CHOSEN "/sys/firmware/devicetree/base/chosen/"
#ifndef GADGET_UDC
#define GADGET_UDC "/sys/kernel/config/usb_gadget/udx-usb/UDC"
#endif
#define UDC_NAME "29100000.dwc3"
#ifndef USB_SOFT_CONNECT
#define USB_SOFT_CONNECT "/sys/class/udc/" UDC_NAME "/soft_connect"
#endif
#ifndef USB_STATE
#define USB_STATE "/sys/class/udc/" UDC_NAME "/state"
#endif
#ifndef USB_CONTROL_LOCK
#define USB_CONTROL_LOCK "/run/udx-usb/control.lock"
#endif
#define RENEW_MS 20000u
#define QUERY_MS 5000u
#define MAX_PATH 512
#define LOG_TIMEOUT_MS 15000u
#define QUIESCE_TIMEOUT_MS 330000u
#ifndef SYSTEM_QUIESCE
#define SYSTEM_QUIESCE "/usr/sbin/udx-system-quiesce"
#endif
#ifndef TRIAL_ACCEPTED_PATH
#define TRIAL_ACCEPTED_PATH "/run/udx-boot-accepted"
#endif
/* fs/ubifs/ubifs.h; not exported by older libc linux/magic.h headers. */
#define STAGE2_UBIFS_MAGIC 0x24051905
#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

struct cpnv_health {
	int healthy, error;
	unsigned uncertain, nv_live, halo, notify, generation[4], armed;
	unsigned long long epoch;
	char storage[16];
};
struct runtime {
	char rtc[MAX_PATH], board[MAX_PATH], pmsys[MAX_PATH], boot[64];
	int recovery, production, boot_policy;
	unsigned ready_stage;
	int listen, signal_fd, lock, claimed, closing, core_error, cpnv_error;
	int usb_error, event_ack_error;
	pid_t quiesce_pid;
	uint64_t quiesce_deadline;
	int quiesce_state, quiesce_error; /* 0 idle, 1 running, 2 ready, 3 failed */
	char quiesce_target[24];
	unsigned long long poweroff_sequence;
	int usb_reset_pending, usb_lock_held, usb_control_fd;
	uint64_t usb_retry_at;
	struct cpnv_health cpnv;
	unsigned renewals;
	uint64_t last_renew, next_health, last_health, usb_rebind_at;
	uint64_t trial_deadline;
	unsigned long long event_sequence;
	char event_kind[24];
	pid_t log_pid;
	uint64_t log_started;
	int log_error;
};
static int kernel_log = -1;

/* kmsg supplies the single severity-aware log stream consumed by OpenWrt
 * logd. Keep stderr only as a fallback if kmsg is unavailable; duplicating
 * every message on procd stderr incorrectly labels successful work as ERR.
 */
static void runtime_log(int priority, const char *format, ...)
{
	char text[1024];
	va_list args;
	int prefix = snprintf(text, sizeof(text), "<%d>", LOG_USER | priority);
	va_start(args, format);
	int n = vsnprintf(text + prefix, sizeof(text) - prefix, format, args);
	va_end(args);
	if (n <= 0)
		return;
	size_t size = (size_t)n >= sizeof(text) - prefix ? sizeof(text) - 1 : (size_t)n + prefix;
	if (kernel_log >= 0 && write(kernel_log, text, size) == (ssize_t)size)
		return;
	(void)write(STDERR_FILENO, text + prefix, size - prefix);
}

static uint64_t milliseconds(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_BOOTTIME, &t))
		return 0;
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static int read_text(const char *path, char *out, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	ssize_t n = read(fd, out, size - 1);
	int error = errno;
	close(fd);
	if (n < 0)
		return -error;
	if (!n || (size_t)n == size - 1)
		return -EOVERFLOW;
	out[n] = 0;
	return 0;
}
static int write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	size_t size = strlen(text);
	ssize_t n = write(fd, text, size);
	int error = errno;
	close(fd);
	return n < 0 ? -error : (size_t)n == size ? 0 : -EIO;
}
static int exact_file(const char *path, const char *value)
{
	char text[256];
	int ret = read_text(path, text, sizeof(text));
	return ret ? ret : strcmp(text, value) ? -EPROTO : 0;
}
static int unique_path(const char *pattern, char *path, size_t size)
{
	glob_t matches;
	int ret = glob(pattern, GLOB_NOSORT, NULL, &matches);
	if (ret) {
		globfree(&matches);
		return ret == GLOB_NOMATCH ? -ENOENT : -EIO;
	}
	if (matches.gl_pathc != 1 || strlen(matches.gl_pathv[0]) >= size)
		ret = -EEXIST;
	else {
		strcpy(path, matches.gl_pathv[0]);
		ret = 0;
	}
	globfree(&matches);
	return ret;
}
static int wait_fd(int fd, short events, int ms)
{
	struct pollfd p = {fd, events, 0};
	int ret;
	do {
		ret = poll(&p, 1, ms);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0)
		return -errno;
	if (!ret)
		return -ETIMEDOUT;
	return p.revents & events ? 0 : -EPIPE;
}
static int connect_control(const char *path)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	struct ucred peer;
	socklen_t size = sizeof(peer);
	int fd, ret, error;
	if (strlen(path) >= sizeof(address.sun_path))
		return -ENAMETOOLONG;
	strcpy(address.sun_path, path);
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -errno;
	ret = connect(fd, (struct sockaddr *)&address, sizeof(address));
	if (ret && errno != EINPROGRESS) {
		error = -errno;
		goto fail;
	}
	if (ret) {
		error = wait_fd(fd, POLLOUT, 500);
		if (error)
			goto fail;
		size = sizeof(ret);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &ret, &size) || ret) {
			error = ret ? -ret : -errno;
			goto fail;
		}
	}
	size = sizeof(peer);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size)) {
		error = -errno;
		goto fail;
	}
	if (peer.uid) {
		error = -EACCES;
		goto fail;
	}
	return fd;
fail:
	close(fd);
	return error;
}
static int parse_cpnv(const char *text, const char *nonce, struct cpnv_health *h)
{
	char echo[33] = {0}, epoch[17] = {0};
	unsigned ready;
	int used = 0;
	memset(h, 0, sizeof(*h));
	int n = sscanf(
	    text,
	    "CPNV_STATUS1 nonce=%32[0-9a-f] ready=%u uncertain=%u error=%d epoch=%16[0-9a-f] "
	    "generation=%u,%u,%u,%u armed=%u nv_live=%u halo=%u notify=%u storage=%15[a-z]%n",
	    echo, &ready, &h->uncertain, &h->error, epoch, &h->generation[0], &h->generation[1],
	    &h->generation[2], &h->generation[3], &h->armed, &h->nv_live, &h->halo, &h->notify,
	    h->storage, &used);
	if (n != 14 || strlen(echo) != 32 || strcmp(echo, nonce) || strlen(epoch) != 16 ||
	    strcmp(text + used, "\n") || ready > 1 || h->uncertain > 1 || h->nv_live > 2 ||
	    h->halo > 1 || h->notify > 1 || h->error > 0 ||
	    (strcmp(h->storage, "shadow") && strcmp(h->storage, "ubi")))
		return -EPROTO;
	h->epoch = strtoull(epoch, NULL, 16);
	if (!h->epoch)
		return -EPROTO;
	/* Generation zero is the real first cold session. SIM and registration
	 * state intentionally do not participate in event-loop health.
	 */
	h->healthy = ready && !h->uncertain && !h->error && h->nv_live == 1 && h->halo;
	return h->healthy ? 0 : h->error ? h->error : -EHOSTDOWN;
}
static int cpnv_query(const char *path, struct cpnv_health *health)
{
	unsigned char random[16];
	char nonce[33], request[48], reply[512];
	int ret, fd;
	if (getrandom(random, sizeof(random), GRND_NONBLOCK) != sizeof(random))
		return -EAGAIN;
	for (unsigned i = 0; i < sizeof(random); i++)
		snprintf(nonce + 2 * i, 3, "%02x", random[i]);
	snprintf(request, sizeof(request), "status %s\n", nonce);
	fd = connect_control(path);
	if (fd < 0)
		return fd;
	if (send(fd, request, strlen(request), MSG_NOSIGNAL) != (ssize_t)strlen(request)) {
		ret = -EIO;
		goto out;
	}
	ret = wait_fd(fd, POLLIN, 500);
	if (ret)
		goto out;
	ssize_t n = recv(fd, reply, sizeof(reply) - 1, MSG_TRUNC);
	if (n <= 0 || n >= (ssize_t)sizeof(reply) - 1) {
		ret = -EMSGSIZE;
		goto out;
	}
	reply[n] = 0;
	ret = parse_cpnv(reply, nonce, health);
out:
	close(fd);
	return ret;
}

static int runtime_cpnv_query(struct runtime *r)
{
	if (r->recovery)
		return 0;
	int ret = cpnv_query(CPNV_SOCKET, &r->cpnv);
	if (!ret && r->production && strcmp(r->cpnv.storage, "ubi"))
		ret = -EXDEV;
	if (ret)
		r->cpnv.healthy = 0;
	/* A newly started session invalidates an old userspace shutdown receipt.
	 * This only changes readiness; it does not automatically stop/restart CP.
	 */
	if (r->production && r->quiesce_state == 2 && r->cpnv.healthy) {
		r->quiesce_state = 0;
		r->quiesce_target[0] = 0;
		runtime_log(
		    LOG_INFO,
		    "udx-runtime: active baseband session invalidates prior quiesce readiness\n");
	}
	return ret;
}

static int root_health(struct runtime *r)
{
	struct statfs fs;
	int ret;
	if (r->recovery) {
		if ((ret = exact_file("/etc/udx-recovery-root", "OpenWrt Recovery RAM root\n")) ||
		    (ret = exact_file("/proc/1/comm", "procd\n")))
			return ret;
		if (statfs("/", &fs))
			return -errno;
		if (fs.f_type != TMPFS_MAGIC)
			return -EROFS;
	} else if (r->production) {
		struct udx_root_layout layout;
		if ((ret = exact_file("/etc/udx-openwrt-root", "OpenWrt production root\n")) ||
		    (ret = udx_root_check(0, 1, &layout)))
			return ret;
	} else
		return -ENODEV;
	return 0;
}
static int core_health(struct runtime *r)
{
	char text[256];
	int ret;
	if ((ret = read_text(r->pmsys, text, sizeof(text))))
		return ret;
	if (strcmp(text, "released core_reset=0 system_reset=0\n"))
		return -EHOSTDOWN;
	/* The board event endpoint is a required live kernel dependency. Sensor
	 * values, temperature sampling and USB attachment are not health gates.
	 */
	if ((ret = read_text(r->board, text, sizeof(text))))
		return ret;
	return 0;
}
static int rtc_path(struct runtime *r, const char *name, char *path, size_t size)
{
	int n = snprintf(path, size, "%s/%s", r->rtc, name);
	return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}
static int rtc_write(struct runtime *r, const char *name, const char *text)
{
	char path[MAX_PATH];
	int ret = rtc_path(r, name, path, sizeof(path));
	return ret ? ret : write_text(path, text);
}
static int trial_pending(struct runtime *r)
{
	/* Acceptance is specific to this boot; missing/partial/stale files cannot
	 * cancel a deadline. The budget is measured from kernel boot, not owner start.
	 */
	return r->trial_deadline && exact_file(TRIAL_ACCEPTED_PATH, r->boot);
}
static int trial_budget(struct runtime *r, uint64_t now)
{
	return trial_pending(r) && now >= r->trial_deadline ? -ETIME : 0;
}
static int trial_configure(struct runtime *r)
{
	char text[32], extra;
	unsigned seconds;
	int ret = read_text("/etc/udx-trial-seconds", text, sizeof(text));
	if (ret)
		return ret;
	if (sscanf(text, "%u %c", &seconds, &extra) != 1 ||
	    (seconds && (seconds < 30 || seconds > 600)))
		return -EINVAL;
	r->trial_deadline = (uint64_t)seconds * 1000u;
	return 0;
}
static int lease_renew(struct runtime *r)
{
	int ret;
	if (r->closing)
		return -ESHUTDOWN;
	if ((ret = trial_budget(r, milliseconds())))
		return ret;
	if ((ret = rtc_write(r, "udx_trial_lease", "renew\n")))
		return ret;
	r->renewals++;
	r->last_renew = milliseconds();
	return 0;
}
static int log_step(struct runtime *r)
{
	int status;
	if (!r->log_pid)
		return 0;
	pid_t result = waitpid(r->log_pid, &status, WNOHANG);
	if (result < 0) {
		r->log_pid = 0;
		return -errno;
	}
	if (!result) {
		if (milliseconds() - r->log_started < LOG_TIMEOUT_MS)
			return 0;
		(void)kill(-r->log_pid, SIGKILL);
		return -ETIMEDOUT;
	}
	r->log_pid = 0;
	r->log_error = WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EIO;
	if (r->log_error)
		runtime_log(LOG_ERR, "udx-runtime: persistent checkpoint failed status=%d\n",
			    status);
	return 0;
}
static int log_start(struct runtime *r)
{
	if (r->log_pid)
		return 0;
	pid_t parent = getpid(), child = fork();
	if (child < 0)
		return -errno;
	if (!child) {
		sigset_t empty;
		sigemptyset(&empty);
		if (sigprocmask(SIG_SETMASK, &empty, NULL) || setpgid(0, 0) ||
		    prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent)
			_exit(74);
		/* A routing wrapper may use this flag to avoid sending this one worker
		 * back into its own supervisor socket. All other callers use --checkpoint.
		 */
		setenv("UDX_RUNTIME_LOG_WORKER", "1", 1);
		execl("/usr/sbin/save-debug-log", "save-debug-log", (char *)NULL);
		_exit(127);
	}
	r->log_pid = child;
	r->log_started = milliseconds();
	return 0;
}
static void log_failure(struct runtime *r, int error)
{
	if (r->log_error != error)
		runtime_log(LOG_WARNING,
			    "udx-runtime: diagnostic checkpoint unavailable error=%d\n", error);
	r->log_error = error;
}
static int persist_now(struct runtime *r)
{
	int ret;
	/* Finish the one existing worker before creating another .new/rotation. */
	while (r->log_pid) {
		if ((ret = log_step(r)))
			return ret;
		if (r->log_pid)
			(void)poll(NULL, 0, 20);
	}
	if ((ret = log_start(r)))
		return ret;
	while (r->log_pid) {
		if ((ret = log_step(r)))
			return ret;
		if (r->log_pid)
			(void)poll(NULL, 0, 20);
	}
	return r->log_error;
}
static int system_quiesce_start(struct runtime *r, const char *target, unsigned long long sequence)
{
	int ret;
	if (!r->production)
		return -EOPNOTSUPP;
	if (r->closing)
		return -ESHUTDOWN;
	if (r->quiesce_state == 1)
		return strcmp(target, r->quiesce_target) ? -EBUSY : 0;
	if (r->quiesce_state == 2)
		return 0;
	if (r->quiesce_state == 3)
		return r->quiesce_error;
	ret = core_health(r);
	if (!ret)
		ret = lease_renew(r);
	if (!ret && access(SYSTEM_QUIESCE, X_OK))
		ret = -errno;
	if (ret)
		goto failed;
	pid_t parent = getpid(), child = fork();
	if (child < 0) {
		ret = -errno;
		goto failed;
	}
	if (!child) {
		sigset_t empty;
		sigemptyset(&empty);
		if (sigprocmask(SIG_SETMASK, &empty, NULL) || setpgid(0, 0) ||
		    prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent)
			_exit(3);
		execl(SYSTEM_QUIESCE, "udx-system-quiesce", (char *)NULL);
		_exit(3);
	}
	r->quiesce_pid = child;
	r->quiesce_deadline = milliseconds() + QUIESCE_TIMEOUT_MS;
	r->quiesce_state = 1;
	strcpy(r->quiesce_target, target);
	r->poweroff_sequence = sequence;
	runtime_log(LOG_INFO,
		    "udx-runtime: bounded system quiesce started target=%s; core lease remains "
		    "locally checked\n",
		    *target ? target : "prepare");
	return 0;
failed:
	r->quiesce_state = 3;
	r->quiesce_error = ret;
	r->closing = 1;
	return ret;
}

static int queue_reboot(struct runtime *r, const char *target)
{
	char command[80];
	if (!r->boot_policy || (strcmp(target, "openwrt") && strcmp(target, "recovery")))
		return -EINVAL;
	if (r->production && r->quiesce_state != 2)
		return system_quiesce_start(r, target, 0);
	int ret = core_health(r);
	if (!ret)
		ret = lease_renew(r);
	/* An expired budget cannot buy another minute. The existing PMIC window
	 * still permits best-effort orderly Recovery return. Every other preparation
	 * failure also stays closed rather than resuming the normal feed loop.
	 */
	if (ret == -ETIME && !strcmp(target, "recovery"))
		ret = 0;
	r->closing = 1;
	if (!ret) {
		runtime_log(LOG_INFO, "udx-runtime: preparing reboot %s\n", target);
	}
	if (ret)
		return ret;
	/* Explicit maintenance runs in this same loop. A stuck sync cannot be
	 * hidden by a separate heartbeat thread. No new writes begin afterward.
	 */
	sync();
	snprintf(command, sizeof(command), "%s\n", target);
	ret = rtc_write(r, "udx_trial_reboot", command);
	/* Already closed before maintenance; a refused transaction also expires. */
	return ret;
}
static int upgrade_admit(struct runtime *r)
{
	if (!r->boot_policy)
		return -EOPNOTSUPP;
	if (r->closing)
		return -ESHUTDOWN;
	if (r->production && r->quiesce_state != 2)
		return -EBUSY;
	return 0;
}

static int prepare_upgrade(struct runtime *r)
{
	int ret = upgrade_admit(r);
	if (ret)
		return ret;
	/* Complete ordinary maintenance before the terminal window. There is no
	 * owner transfer: this PID performs one NORMAL/60s transaction and closes.
	 * The writer has already durably marked misc pending; any failure reboots
	 * into Recovery unless all image/configuration work later reaches ready.
	 */
	ret = core_health(r);
	if (!ret)
		ret = lease_renew(r);
	r->closing = 1;
	if (ret)
		return ret;
	sync();
	ret = rtc_write(r, "udx_trial_reboot", "upgrade\n");
	if (!ret)
		runtime_log(LOG_INFO, "udx-runtime: final NORMAL 60s upgrade window; no further "
				      "renewal or owner handoff\n");
	return ret;
}

static void recovery_usb_unlock(struct runtime *r)
{
	if (!r->usb_lock_held)
		return;
	/* Unlock explicitly so a just-forked checkpoint child cannot extend the
	 * ownership window before its close-on-exec descriptors are discarded.
	 */
	(void)flock(r->usb_control_fd, LOCK_UN);
	close(r->usb_control_fd);
	r->usb_control_fd = -1;
	r->usb_lock_held = 0;
}

static int recovery_usb_lock(struct runtime *r)
{
	int control = open(USB_CONTROL_LOCK, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (control < 0)
		return -errno;
	if (flock(control, LOCK_EX | LOCK_NB)) {
		int ret = -errno;
		close(control);
		return ret;
	}
	r->usb_control_fd = control;
	r->usb_lock_held = 1;
	return 0;
}

static int recovery_usb_reset_step(struct runtime *r, uint64_t now)
{
	char owner[64];
	int ret;
	if (!r->usb_reset_pending || now < r->usb_retry_at)
		return 0;
	/* A concurrent gadget reconfiguration can defer the request; each attempt is nonblocking
	 * and is rate-limited to once per second.
	 */
	r->usb_retry_at = now + 1000;
	ret = recovery_usb_lock(r);
	if (!ret) {
		ret = read_text(GADGET_UDC, owner, sizeof(owner));
		if (!ret && strcmp(owner, UDC_NAME "\n"))
			ret = -ENOTCONN;
	}
	if (ret) {
		recovery_usb_unlock(r);
		if (r->usb_error != ret) {
			runtime_log(ret == -EBUSY || ret == -EWOULDBLOCK ? LOG_INFO : LOG_WARNING,
				    "udx-runtime: MCU USB AP reset deferred error=%d; ACK already "
				    "attempted\n",
				    ret);
		}
		r->usb_error = ret;
		return 0;
	}
	r->usb_reset_pending = 0;
	ret = write_text(USB_SOFT_CONNECT, "disconnect\n");
	r->usb_error = ret;
	runtime_log(ret ? LOG_ERR : LOG_INFO,
		    "udx-runtime: deferred MCU USB disconnect submitted error=%d; holding "
		    "maintenance locks until one connect\n",
		    ret);
	/* As in the legacy policy, a failed disconnect still gets one connect
	 * attempt. This is a single requested logical reset, not profile probing.
	 */
	r->usb_rebind_at = milliseconds() + 500;
	return 0;
}

static int board_event(struct runtime *r, uint64_t now)
{
	char text[256], ack[24], kind[24], state[24], minor[3], extra;
	unsigned long long sequence;
	int ret;
	if (r->usb_rebind_at) {
		if (now < r->usb_rebind_at)
			return 0;
		ret = write_text(USB_SOFT_CONNECT, "connect\n");
		r->usb_rebind_at = 0;
		recovery_usb_unlock(r);
		r->usb_error = ret;
		char observed[64] = "unavailable";
		(void)read_text(USB_STATE, observed, sizeof(observed));
		observed[strcspn(observed, "\n")] = 0;
		runtime_log(ret ? LOG_ERR : LOG_INFO,
			    "udx-runtime: USB logical connect submitted error=%d state=%s; "
			    "re-enumeration remains observed separately\n",
			    ret, observed);
		return 0;
	}
	if ((ret = read_text(r->board, text, sizeof(text))))
		return ret;
	if (!strcmp(text, "none\n"))
		return recovery_usb_reset_step(r, now);
	if (sscanf(text, "seq=%llu minor=%2[0-9a-f] kind=%23s ack=%23s %c", &sequence, minor, kind,
		   state, &extra) != 4 ||
	    !sequence || strlen(minor) != 2)
		return -EPROTO;
	if (strcmp(state, "pending"))
		return -EIO;
	if ((strcmp(minor, "04") || strcmp(kind, "poweroff")) &&
	    (strcmp(minor, "0a") || strcmp(kind, "usb-reset")))
		return -EPROTO;
	if (!strcmp(kind, "poweroff")) {
		if (trial_pending(r)) {
			runtime_log(LOG_WARNING,
				    "udx-runtime: unaccepted trial poweroff seq=%llu redirected to "
				    "%s without MCU ACK\n",
				    sequence, "recovery");
			return queue_reboot(r, "recovery");
		}
		if (r->production && r->quiesce_state != 2) {
			/* Keep the event queued until bounded netifd/AT/NV shutdown completes.
			 * Do not interrupt an already accepted maintenance operation.
			 */
			if (r->quiesce_state == 1)
				return 0;
			return system_quiesce_start(r, "poweroff", sequence);
		}
		runtime_log(LOG_INFO, "udx-runtime: preparing board poweroff seq=%llu\n", sequence);
		sync(); /* Preparation precedes ACK; AP action follows ACK. */
	}
	char path[MAX_PATH];
	size_t length = strlen(r->board);
	if (length < 5 || strcmp(r->board + length - 5, "event"))
		return -EPROTO;
	if (snprintf(path, sizeof(path), "%.*sevent_ack", (int)(length - 5), r->board) >=
	    (int)sizeof(path))
		return -ENAMETOOLONG;
	snprintf(ack, sizeof(ack), "%llu\n", sequence);
	ret = write_text(path, ack); /* Exactly one ACK attempt; never replay. */
	r->event_ack_error = ret;
	if (ret) {
		runtime_log(LOG_ERR, "udx-runtime: board ACK seq=%llu error=%d\n", sequence, ret);
		/* Stale/already-started means another caller handled this queue entry.
		 * Transport errors follow stock semantics: still execute the AP action.
		 */
		if (ret == -ESTALE || ret == -EALREADY)
			return 0;
	}
	r->event_sequence = sequence;
	strcpy(r->event_kind, kind);
	runtime_log(r->event_ack_error ? LOG_ERR : LOG_INFO,
		    "udx-runtime: board event %llu %s ACK attempted once, result=%d\n", sequence,
		    kind, r->event_ack_error);
	if (!strcmp(kind, "poweroff")) {
		r->closing = 1;
		(void)rtc_write(r, "udx_trial_lease", "close\n");
		if (reboot(RB_POWER_OFF))
			return -errno;
		return -EIO;
	}
	/* ACK each queued event exactly once, coalescing its AP-side work. Drain
	 * the event queue before starting the single deferred reset, so a burst
	 * received during maintenance cannot become successive re-enumerations.
	 */
	if (!r->usb_reset_pending)
		runtime_log(LOG_INFO, "udx-runtime: MCU USB AP reset queued after ACK seq=%llu\n",
			    sequence);
	r->usb_reset_pending = 1;
	return 0;
}

static int system_quiesce_step(struct runtime *r, uint64_t now)
{
	int status, ret;
	if (!r->production || r->quiesce_state != 1)
		return 0;
	pid_t result = waitpid(r->quiesce_pid, &status, WNOHANG);
	if (result < 0) {
		ret = -errno;
		goto failed;
	}
	if (!result) {
		if (now < r->quiesce_deadline)
			return 0;
		(void)kill(-r->quiesce_pid, SIGTERM);
		ret = -ETIMEDOUT;
		goto failed;
	}
	r->quiesce_pid = 0;
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		ret = WIFEXITED(status) && WEXITSTATUS(status) == 3 ? -ETIMEDOUT : -EIO;
		goto failed;
	}
	/* The helper's success means its explicit NV/controller shutdown result
	 * was checked. A generic process disappearance is not sufficient.
	 */
	r->quiesce_state = 2;
	r->cpnv.healthy = 0;
	r->cpnv_error = -ESHUTDOWN;
	runtime_log(LOG_INFO,
		    "udx-runtime: netifd/AT and userspace NV/controller shutdown confirmed\n");
	if (!strcmp(r->quiesce_target, "poweroff")) {
		char event[256], expected[128];
		ret = read_text(r->board, event, sizeof(event));
		snprintf(expected, sizeof(expected),
			 "seq=%llu minor=04 kind=poweroff ack=pending\n", r->poweroff_sequence);
		if (!ret && strcmp(event, expected))
			ret = -ESTALE;
		if (ret)
			goto failed;
		return board_event(r, now);
	}
	if (*r->quiesce_target)
		return queue_reboot(r, r->quiesce_target);
	return 0;
failed:
	r->quiesce_state = 3;
	r->quiesce_error = ret;
	r->closing = 1;
	runtime_log(
	    LOG_ERR,
	    "udx-runtime: system quiesce failed error=%d; no shutdown/flash completion claimed\n",
	    ret);
	return ret;
}

static void status_text(struct runtime *r, char *out, size_t size)
{
	char boot[64];
	strcpy(boot, r->boot);
	boot[strcspn(boot, "\n")] = 0;
	snprintf(out, size,
		 "RUNTIME_STATUS1 boot=%s owner=%ld core=%s core_error=%d baseband=%s "
		 "cpnv_error=%d renewals=%u last_renew_ms=%llu closing=%d board_event=%llu:%s "
		 "ack_error=%d usb_error=%d log_error=%d\n",
		 boot, (long)getpid(), r->core_error ? "failed" : "ready", r->core_error,
		 r->recovery	   ? "disabled"
		 : r->cpnv.healthy ? "ready"
				   : "degraded",
		 r->cpnv_error, r->renewals, (unsigned long long)r->last_renew, r->closing,
		 r->event_sequence, *r->event_kind ? r->event_kind : "none", r->event_ack_error,
		 r->usb_error, r->log_error);
	if (r->production) {
		static const char *const states[] = {"idle", "running", "ready", "failed"};
		size_t length = strlen(out);
		if (length && out[length - 1] == '\n')
			length--;
		if (length < size)
			snprintf(out + length, size - length,
				 " root=overlay quiesce=%s quiesce_error=%d\n",
				 states[r->quiesce_state], r->quiesce_error);
	}
}
static void serve_client(struct runtime *r)
{
	int fd = accept4(r->listen, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (fd < 0)
		return;
	struct ucred peer;
	socklen_t size = sizeof(peer);
	char request[96], reply[512];
	int ret = 0;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid ||
	    wait_fd(fd, POLLIN, 500))
		goto out;
	ssize_t n = recv(fd, request, sizeof(request) - 1, MSG_TRUNC);
	if (n <= 0 || n >= (ssize_t)sizeof(request) - 1)
		goto out;
	request[n] = 0;
	if (!strcmp(request, "status\n"))
		status_text(r, reply, sizeof(reply));
	else if (!r->closing && !strcmp(request, "checkpoint\n")) {
		ret = persist_now(r);
		snprintf(reply, sizeof(reply), ret ? "ERROR %d\n" : "CHECKPOINT_SAVED result=%d\n",
			 ret);
	} else if (!r->closing && (!strcmp(request, "reboot recovery\n") ||
				   !strcmp(request, "reboot openwrt\n"))) {
		const char *target = !strcmp(request, "reboot recovery\n") ? "recovery" : "openwrt";
		ret = queue_reboot(r, target);
		if (ret)
			snprintf(reply, sizeof(reply), "ERROR %d\n", ret);
		else
			snprintf(reply, sizeof(reply), "REBOOT_QUEUED %s %s", target, r->boot);
	} else if (!strcmp(request, "prepare-upgrade\n")) {
		ret = prepare_upgrade(r);
		if (ret)
			snprintf(reply, sizeof(reply), "ERROR %d\n", ret);
		else
			snprintf(reply, sizeof(reply), "UPGRADE_WINDOW_READY seconds=60 boot=%s",
				 r->boot);
	} else if (!strcmp(request, "quiesce\n")) {
		ret = system_quiesce_start(r, "", 0);
		if (ret)
			snprintf(reply, sizeof(reply), "ERROR %d\n", ret);
		else
			snprintf(reply, sizeof(reply), "QUIESCE_QUEUED\n");
	} else if (!strcmp(request, "stop\n")) {
		runtime_log(LOG_INFO, "udx-runtime: explicit lease stop\n");
		r->closing = 1;
		ret = rtc_write(r, "udx_trial_lease", "close\n");
		snprintf(reply, sizeof(reply), "LEASE_STOPPED result=%d\n", ret);
	} else
		snprintf(reply, sizeof(reply), "ERROR %d\n", -EINVAL);
	(void)send(fd, reply, strlen(reply), MSG_NOSIGNAL);
out:
	close(fd);
}
static int open_control(struct runtime *r)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	strcpy(address.sun_path, RUNTIME_SOCKET);
	r->lock = open("/run/udx-runtime.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (r->lock < 0 || flock(r->lock, LOCK_EX | LOCK_NB))
		return -EBUSY;
	/* Holding the singleton lock excludes a live owner; discard its stale endpoint. */
	if (unlink(RUNTIME_SOCKET) && errno != ENOENT)
		return -errno;
	r->listen = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (r->listen < 0)
		return -errno;
	if (bind(r->listen, (struct sockaddr *)&address, sizeof(address)) ||
	    chmod(RUNTIME_SOCKET, 0600) || listen(r->listen, 4))
		return -errno;
	return 0;
}
static int discover(struct runtime *r)
{
	char status[MAX_PATH];
	int ret;
	if ((ret = read_text("/proc/sys/kernel/random/boot_id", r->boot, sizeof(r->boot))))
		return ret;
	if ((ret = unique_path("/sys/bus/platform/devices/*/udx_trial_lease_status", status,
			       sizeof(status))))
		return ret;
	char *slash = strrchr(status, '/');
	if (!slash)
		return -EINVAL;
	*slash = 0;
	strcpy(r->rtc, status);
	r->recovery = !access("/etc/udx-recovery-root", F_OK);
	r->production = !access("/etc/udx-openwrt-root", F_OK);
	if (r->recovery && r->production)
		return -EPROTO;
	r->ready_stage = 0xbe5a;
	char policy_path[MAX_PATH], policy[128], target[24];
	if ((ret = rtc_path(r, "udx_boot_policy", policy_path, sizeof(policy_path))))
		return ret;
	ret = read_text(policy_path, policy, sizeof(policy));
	if (ret)
		return ret;
	int used = 0;
	if (sscanf(policy, "policy=openwrt-v1 target=%23s ready_stage=%x%n", target,
		   &r->ready_stage, &used) != 2 ||
	    strcmp(policy + used, "\n") || strcmp(target, r->recovery ? "recovery" : "openwrt") ||
	    r->ready_stage != (r->recovery ? 0xbc5a : 0xbe5a) || (!r->production && !r->recovery))
		return -EPROTO;
	r->boot_policy = 1;

	if ((ret = unique_path("/sys/bus/platform/drivers/udx710-pmsys/*/state", r->pmsys,
			       sizeof(r->pmsys))))
		return ret;
	return unique_path("/sys/bus/serial/drivers/sy108-mcu/*/event", r->board, sizeof(r->board));
}
static int run_owner(void)
{
	struct runtime r = {.listen = -1, .signal_fd = -1, .lock = -1, .cpnv_error = -EAGAIN};
	sigset_t mask;
	int ret;
	if (geteuid() || access(CHOSEN "udx,trial-local-lease", F_OK))
		return 77;
	kernel_log = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
	umask(077);
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);
	if (sigprocmask(SIG_BLOCK, &mask, NULL))
		return 74;
	r.signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	if (r.signal_fd < 0)
		return 74;
	if ((ret = discover(&r)) || (ret = trial_configure(&r)) || (ret = open_control(&r)) ||
	    (ret = root_health(&r)) || (ret = core_health(&r)))
		goto fail;
	/* A real, current parent-loop reply determines baseband readiness. A
	 * startup fault/no service keeps a debuggable degraded runtime, not a
	 * whole-board reset loop and not a false baseband-ready declaration.
	 */
	r.cpnv_error = runtime_cpnv_query(&r);
	if (r.cpnv_error)
		r.cpnv.healthy = 0;
	r.last_health = milliseconds();
	r.next_health = r.last_health + QUERY_MS;
	if ((ret = rtc_write(&r, "udx_trial_lease_owner", "claim\n")))
		goto fail;
	r.claimed = 1;
	if ((ret = lease_renew(&r)))
		goto fail;
	runtime_log(r.cpnv_error ? LOG_WARNING : LOG_INFO,
		    "udx-runtime: claimed standalone lease pid=%ld baseband=%s error=%d\n",
		    (long)getpid(),
		    r.recovery	     ? "disabled"
		    : r.cpnv.healthy ? "ready"
				     : "degraded",
		    r.cpnv_error);
	for (;;) {
		uint64_t now = milliseconds();
		if (!now) {
			ret = -EIO;
			break;
		}
		int log_error = log_step(&r);
		if (log_error)
			log_failure(&r, log_error);
		if (!r.closing && trial_budget(&r, now)) {
			runtime_log(
			    LOG_WARNING,
			    "udx-runtime: absolute trial budget reached; requesting %s return\n",
			    "recovery");
			if ((ret = queue_reboot(&r, "recovery")))
				break;
		}
		if (!r.closing) {
			if ((ret = system_quiesce_step(&r, now)))
				break;
			if (r.closing)
				continue;
			if ((ret = board_event(&r, now)))
				break;
			if (r.closing)
				continue;
			if (now >= r.next_health) {
				if ((ret = core_health(&r)))
					break;
				int before = r.cpnv_error;
				r.cpnv_error = runtime_cpnv_query(&r);
				if (r.cpnv_error)
					r.cpnv.healthy = 0;
				if (before != r.cpnv_error) {
					runtime_log(r.cpnv_error ? LOG_WARNING : LOG_INFO,
						    "udx-runtime: baseband=%s error=%d\n",
						    r.cpnv.healthy ? "ready" : "degraded",
						    r.cpnv_error);
				}
				r.last_health = milliseconds();
				r.next_health = r.last_health + QUERY_MS;
			}
			if (now - r.last_renew >= RENEW_MS) {
				if ((ret = core_health(&r)) || (ret = lease_renew(&r)))
					break;
			}
		}
		/* A requested shutdown/return cancels this owner's pending USB work.
		 * Do not retain maintenance locks while the existing close path waits
		 * for its reboot or PMIC deadline; no additional connect is submitted.
		 */
		if (r.closing) {
			recovery_usb_unlock(&r);
			r.usb_rebind_at = 0;
			r.usb_reset_pending = 0;
		}
		struct pollfd p[2] = {{r.listen, POLLIN, 0}, {r.signal_fd, POLLIN, 0}};
		int timeout =
		    r.usb_rebind_at && now < r.usb_rebind_at ? (int)(r.usb_rebind_at - now) : 1000;
		if (timeout > 1000)
			timeout = 1000;
		int n = poll(p, 2, timeout);
		if (n < 0 && errno != EINTR) {
			ret = -errno;
			break;
		}
		if (p[1].revents) {
			ret = -ECANCELED;
			break;
		}
		if (p[0].revents & POLLIN)
			serve_client(&r);
	}
fail:
	if (r.quiesce_pid > 0)
		(void)kill(-r.quiesce_pid, SIGTERM);
	recovery_usb_unlock(&r);
	r.core_error = ret;
	runtime_log(LOG_ERR, "udx-runtime: core failure error=%d; no further PMIC renewal\n", ret);
	/* Leave the existing hardware counter running. With no replacement owner
	 * it expires normally; an explicit service restart can claim the live timer.
	 * Procd does not automatically respawn this process.
	 */
	if (r.listen >= 0)
		close(r.listen);
	if (r.signal_fd >= 0)
		close(r.signal_fd);
	if (r.lock >= 0)
		close(r.lock);
	if (kernel_log >= 0)
		close(kernel_log);
	return 74;
}
static int control_client(const char *request)
{
	int fd = connect_control(RUNTIME_SOCKET);
	char reply[512];
	if (fd < 0) {
		fprintf(stderr, "runtime control: %s\n", strerror(-fd));
		return 74;
	}
	if (send(fd, request, strlen(request), MSG_NOSIGNAL) != (ssize_t)strlen(request) ||
	    wait_fd(fd, POLLIN, 45000)) {
		close(fd);
		return 74;
	}
	ssize_t n = recv(fd, reply, sizeof(reply) - 1, MSG_TRUNC);
	close(fd);
	if (n <= 0 || n >= (ssize_t)sizeof(reply) - 1)
		return 74;
	reply[n] = 0;
	fputs(reply, stdout);
	return strncmp(reply, "ERROR ", 6) ? 0 : 74;
}
int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--serve"))
		return run_owner();
	if (argc == 2 && !strcmp(argv[1], "--status"))
		return control_client("status\n");
	if (argc == 2 && !strcmp(argv[1], "--checkpoint"))
		return control_client("checkpoint\n");
	if (argc == 2 && !strcmp(argv[1], "--stop"))
		return control_client("stop\n");
	if (argc == 2 && !strcmp(argv[1], "--quiesce"))
		return control_client("quiesce\n");
	if (argc == 2 && !strcmp(argv[1], "--prepare-upgrade"))
		return control_client("prepare-upgrade\n");
	if (argc == 3 && !strcmp(argv[1], "--reboot")) {
		if (!strcmp(argv[2], "openwrt"))
			return control_client("reboot openwrt\n");
		if (!strcmp(argv[2], "recovery"))
			return control_client("reboot recovery\n");
	}
	fputs("usage: udx-runtime --serve | --status | --checkpoint | --stop | --quiesce | "
	      "--prepare-upgrade | --reboot openwrt|recovery\n",
	      stderr);
	return 64;
}
