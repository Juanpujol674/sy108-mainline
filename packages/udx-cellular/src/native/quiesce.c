/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CELL_CP_RUN
#define CELL_CP_RUN "/run/udx-cpnv-warm"
#endif
static double deadline;
static char boot[37];
static int wait_exit(int fd)
{
	while (!pidfd_exited(fd)) {
		if (mono() >= deadline) {
			errno = ETIMEDOUT;
			return errorf(CELL_SYSTEM, "Owner shutdown deadline");
		}
		delay_ms(50);
	}
	return 0;
}
static bool exists(const char *path)
{
	struct stat st;
	return !lstat(path, &st);
}
static bool cellular_receipt(J *row, pid_t expected)
{
	int64_t pid = json_object_get_int64(get(row, "pid"));
	return text_equal(str(row, "boot_id"), boot) &&
	       json_object_is_type(get(row, "pid"), json_type_int) && pid > 0 && pid <= INT_MAX &&
	       (!expected || pid == expected) &&
	       json_object_get_boolean(get(row, "network_retired")) &&
	       json_object_get_boolean(get(row, "at_closed")) &&
	       !json_object_get_boolean(get(row, "error"));
}
static int no_live_owner(pid_t pid)
{
	int fd = pidfd_open_checked(pid);
	if (fd < 0) {
		if (errno == ESRCH)
			return 0;
		return errorf(CELL_SYSTEM, "Inspect previous owner: %s", strerror(errno));
	}
	bool exited = pidfd_exited(fd);
	close(fd);
	return exited ? 0 : errorf(CELL_SYSTEM, "Prior owner has not exited");
}
static J *cellular(void)
{
	const char *request = "{\"operation\":\"quiesce\",\"arguments\":{}}";
	char response[8193];
	pid_t pid;
	int fd = -1;
	int n = seqpacket_pinned(CELL_RUN "/control.sock", request, strlen(request), response,
				 sizeof(response), 3000, &pid, &fd);
	if (n < 0) {
		int saved = errno;
		if (saved != ENOENT && saved != ECONNREFUSED)
			return NULL;
		J *row = read_json(CELL_RUN "/shutdown.json", 65536);
		if (row && cellular_receipt(row, 0)) {
			if (no_live_owner((pid_t)json_object_get_int64(get(row, "pid"))) < 0) {
				json_object_put(row);
				return NULL;
			}
			return row;
		}
		json_object_put(row);
		if (!exists(CELL_RUN "/transport.json") && !exists(CELL_RUN "/control.sock")) {
			row = json_object_new_object();
			boolean(row, "not_started", true);
			return row;
		}
		errorf(CELL_SYSTEM, "Missing matching cellular shutdown receipt");
		return NULL;
	}
	J *reply = parse_json(response, (size_t)n);
	if (!reply || !json_object_get_boolean(get(reply, "accepted"))) {
		if (reply)
			errorf(CELL_SYSTEM, "Cellular quiesce rejected: %s",
			       str(reply, "error") ? str(reply, "error") : "invalid reply");
		json_object_put(reply);
		close(fd);
		return NULL;
	}
	json_object_put(reply);
	int rc = wait_exit(fd);
	close(fd);
	if (rc < 0)
		return NULL;
	J *row = read_json(CELL_RUN "/shutdown.json", 65536);
	if (!row || !cellular_receipt(row, pid)) {
		json_object_put(row);
		errorf(CELL_SYSTEM, "Cellular shutdown receipt missing or failed");
		return NULL;
	}
	return row;
}
static J *native_receipt(pid_t expected, const char *epoch)
{
	char *value = read_text(CELL_CP_RUN "/shutdown.result", 512, false);
	if (!value) {
		errorf(CELL_SYSTEM, "Native shutdown receipt unavailable: %s", strerror(errno));
		return NULL;
	}
	char actual_boot[37], actual_epoch[17], pid_text[12];
	int consumed = 0;
	int fields =
	    sscanf(value,
		   "CPNV_SHUTDOWN1 boot=%36[0-9a-f-] pid=%11[0-9] epoch=%16[0-9a-f] result=0 "
		   "nv_drained=1 controller_closed=1\n%n",
		   actual_boot, pid_text, actual_epoch, &consumed);
	int64_t pid = 0;
	bool valid = fields == 3 && consumed == (int)strlen(value) && value[consumed - 1] == '\n' &&
		     text_equal(actual_boot, boot) && hex_string(actual_epoch, 16) &&
		     integer_text(pid_text, 1, INT_MAX, &pid) && (!expected || pid == expected) &&
		     (!epoch || text_equal(epoch, actual_epoch));
	free(value);
	if (!valid) {
		errorf(CELL_SYSTEM, "Native NV/controller shutdown receipt missing or failed");
		return NULL;
	}
	if (!expected && no_live_owner((pid_t)pid) < 0)
		return NULL;
	J *row = json_object_new_object();
	integer(row, "pid", pid);
	string(row, "epoch", actual_epoch);
	boolean(row, "nv_drained", true);
	boolean(row, "controller_closed", true);
	return row;
}
static J *native(void)
{
	struct owner_status row;
	if (owner_query(&row) < 0) {
		int saved = errno;
		if (saved != ENOENT && saved != ECONNREFUSED)
			return NULL;
		if (exists(CELL_CP_RUN "/shutdown.result"))
			return native_receipt(0, NULL);
		/* Absence of a socket alone is not proof of a completed NV drain. */
		if (!exists(CELL_CP_RUN "/control.sock") && !exists(CELL_CP_RUN "/owner.pid") &&
		    !exists(CELL_CP_RUN "/status.json")) {
			J *value = json_object_new_object();
			boolean(value, "not_started", true);
			return value;
		}
		errorf(CELL_SYSTEM, "No matching successful native shutdown receipt");
		return NULL;
	}
	int rc = pidfd_signal(row.pidfd, SIGTERM);
	if (!rc)
		rc = wait_exit(row.pidfd);
	close(row.pidfd);
	if (rc < 0)
		return NULL;
	return native_receipt(row.pid, row.epoch);
}
int main(int argc, char **argv)
{
	bool cell_only = argc == 2 && !strcmp(argv[1], "--cellular-only");
	if (geteuid() || (argc != 1 && !cell_only))
		return 64;
	signal(SIGPIPE, SIG_IGN);
	char *id = read_text(CELL_BOOT, 64, true);
	if (!id || strlen(id) != 36) {
		free(id);
		fprintf(stderr, "Boot identity unavailable\n");
		return 1;
	}
	memcpy(boot, id, 37);
	free(id);
	deadline = mono() + 300;
	int lock =
	    open("/run/udx-system-quiesce.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
		if (lock >= 0)
			close(lock);
		fprintf(stderr, "System quiesce is already active or unavailable\n");
		return 75;
	}
	J *row = json_object_new_object();
	string(row, "boot_id", boot);
	string(row, "phase", "cellular");
	real(row, "started_uptime", mono());
	J *result = cellular();
	int status = 0;
	if (!result)
		goto fail;
	json_object_object_add(row, "cellular", result);
	if (!cell_only) {
		result = native();
		if (!result)
			goto fail;
		json_object_object_add(row, "native", result);
	}
	sync();
	string(row, "phase", cell_only ? "cellular_complete" : "complete");
	goto done;
fail:
	status = errno == ETIMEDOUT ? 3 : 1;
	string(row, "phase", "failed");
	string(row, "error", cell_error);
done:
	real(row, "finished_uptime", mono());
	if (atomic_json("/run/udx-system-quiesce.json", row, false) < 0)
		status = 1;
	puts(json_object_to_json_string_ext(row, JSON_C_TO_STRING_PLAIN |
						     JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(row);
	close(lock);
	return status;
}
