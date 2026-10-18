/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "wr_factory.h"
#include "wr_snapshot.h"
#include "modem_cmdline.h"
#include "sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include "nv_vendor.h"
int wr_open_recovery_nv(const struct nvs_profile *, struct nvs_devices *);
static const char *const names[] = {"modem",   "v3phy",	    "nrphy", "nrdsp1", "nrdsp2",
				    "deltanv", "cpcmdline", "fixnv", "runnv"};
static const uint32_t base[] = {0x8f820000, 0x97000000, 0x93000000, 0x93310000, 0x93390000,
				0x88700000, 0x8a602400, 0x96800000, 0x8d355000};
static const uint32_t cap[] = {0x1030000, 0x600000, 0x400000, 0x80000, 0x100000,
			       0x100000,  0x800,    0x700000, 0x900000};
static const char *const domains[] = {"cptl", "v3phy", "nrphy"};
static uint32_t get32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int fixed(const char *path, int access, long filesystem)
{
	int fd = open(path, access | O_CLOEXEC | O_NOFOLLOW);
	struct stat a, b;
	struct statfs fs;
	if (fd < 3)
		return -1;
	if (fstat(fd, &a) || stat(path, &b) || fstatfs(fd, &fs) || !S_ISREG(a.st_mode) ||
	    a.st_uid || (a.st_mode & 0022) || a.st_dev != b.st_dev || a.st_ino != b.st_ino ||
	    (filesystem && (unsigned long)fs.f_type != (unsigned long)filesystem)) {
		close(fd);
		return -1;
	}
	return fd;
}
static void init(struct wr_factory *f)
{
	memset(f, 0, sizeof *f);
	f->directory = -1;
	f->config.owner = f->config.lease = f->config.phase = f->config.cmdline = f->config.report =
	    f->config.continuation = -1;
	f->config.devdir = f->config.status_session = f->config.halo_session =
	    f->config.provider_rx = f->config.provider_tx = f->config.verification_params =
		f->config.events = -1;
	for (unsigned d = 0; d < 4; d++)
		f->config.status[d] = f->config.start[d] = f->config.stop[d] = -1;
	for (unsigned i = 0; i < 6; i++)
		f->firmware[i] = -1;
	for (unsigned i = 0; i < MC_MAX_IMAGES; i++) {
		f->config.images[i].source = f->config.images[i].target =
		    f->config.images[i].backup = -1;
		f->output[i] = f->backup_output[i] = -1;
	}
}
static int controls(struct wr_factory *f, int reload)
{
	char path[96];
	for (unsigned d = 0; d < 3; d++) {
		int *ids[] = {&f->config.status[d], &f->config.start[d], &f->config.stop[d]};
		const char *leaves[] = {"status", "start", "stop"};
		for (unsigned j = 0; j < (reload ? 3u : 1u); j++) {
			snprintf(path, sizeof path, "/proc/%s/%s", domains[d], leaves[j]);
			*ids[j] = fixed(path, j < 1 ? O_RDONLY : O_WRONLY, 0x9fa0L);
			if (*ids[j] < 3)
				return -EXDEV;
		}
	}
	f->config.cmdline = fixed("/proc/cmdline", O_RDONLY, 0x9fa0L);
	return f->config.cmdline < 3 ? -EXDEV : 0;
}
static int layout(void)
{
	int fd = fixed("/proc/cptl/ldinfo", O_RDONLY, 0x9fa0L);
	if (fd < 3)
		return -EXDEV;
	unsigned char b[40 * 64];
	ssize_t n = pread(fd, b, sizeof b, 0);
	close(fd);
	if (n <= 0 || n == (ssize_t)sizeof b || n % 40)
		return -EPROTO;
	unsigned mask = 0;
	for (ssize_t off = 0; off < n; off += 40) {
		char name[33];
		memcpy(name, b + off, 32);
		name[32] = 0;
		if (!memchr(name, 0, 32))
			return -EPROTO;
		for (unsigned i = 0; i < 9; i++)
			if (!strcmp(name, names[i])) {
				if (mask & (1u << i) || get32(b + off + 32) != base[i] ||
				    get32(b + off + 36) != cap[i])
					return -EXDEV;
				mask |= 1u << i;
			}
	}
	return mask == 511 ? 0 : -EXDEV;
}
static int private_output(struct wr_factory *f, unsigned slot, int backup, int *ro)
{
	char name[64];
	snprintf(name, sizeof name, "snapshot-%016llx-%u-%u", (unsigned long long)f->config.epoch,
		 slot, backup ? 1u : 0u);
	int fd =
	    openat(f->directory, name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 3)
		return -1;
	*ro = openat(f->directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	struct stat a, b;
	if (*ro < 3 || fstat(fd, &a) || fstat(*ro, &b) || a.st_ino != b.st_ino ||
	    a.st_dev != b.st_dev || !S_ISREG(a.st_mode) || a.st_nlink != 1 || a.st_uid ||
	    a.st_size || (a.st_mode & 0077)) {
		close(fd);
		if (*ro >= 0)
			close(*ro);
		*ro = -1;
		return -1;
	}
	return fd;
}
static int add(struct wr_factory *f, unsigned segment, unsigned source, const struct mci_extent *e)
{
	unsigned i = f->config.image_count;
	if (i >= MC_MAX_IMAGES)
		return -E2BIG;
	struct mc_image *v = &f->config.images[i];
	v->domain = 0;
	v->segment = segment;
	v->physical_base = base[segment];
	v->capacity = cap[segment];
	f->extent[i] = *e;
	f->source_index[i] = source;
	char path[96];
	snprintf(path, sizeof path, "/proc/cptl/%s", names[segment]);
	v->target = fixed(path, O_RDWR, 0x9fa0L);
	if (v->target < 3)
		return -EXDEV;
	if (segment >= 7) {
		f->config.image_count++;
		return 0;
	}
	if (segment < 6) {
		v->source = fcntl(f->firmware[source], F_DUPFD_CLOEXEC, 3);
		if (v->source < 3)
			return -errno;
		f->config.image_count++;
		return 0;
	}
	f->output[i] = private_output(f, i, 0, &v->source);
	if (f->output[i] < 3)
		return -EIO;
	if (segment >= 7) {
		f->backup_output[i] = private_output(f, i, 1, &v->backup);
		if (f->backup_output[i] < 3)
			return -EIO;
	}
	f->config.image_count++;
	return 0;
}
int wr_factory_preloaded(struct wr_factory *f, int owner, int lease, int phase, uint64_t nonce,
			 const struct wr_policy *p)
{
	if (!f || !nonce || wr_policy_validate(p, 0))
		return -EPERM;
	init(f);
	f->config.parent = getpid();
	f->config.owner = owner;
	f->config.lease = lease;
	f->config.phase = phase;
	f->config.epoch = nonce;
	f->config.operation_ms = p->nv.timeout_ms;
	return controls(f, 0);
}
static int commandline_build(char built[2048], size_t *used);
static int firmware_attribute(unsigned volume, const char *leaf, char out[64])
{
	char path[160];
	snprintf(path, sizeof path, "/sys/class/ubi/ubi0_%u/%s", volume, leaf);
	int fd = fixed(path, O_RDONLY, 0x62656572L);
	if (fd < 3)
		return -EXDEV;
	ssize_t n = pread(fd, out, 63, 0);
	close(fd);
	if (n <= 0 || n >= 63 || out[n - 1] != '\n' || memchr(out, 0, (size_t)n))
		return -EPROTO;
	out[n - 1] = 0;
	return 0;
}
static int firmware_number(unsigned volume, const char *leaf, uint64_t *out)
{
	char text[64], *end;
	if (firmware_attribute(volume, leaf, text))
		return -EXDEV;
	errno = 0;
	unsigned long long n = strtoull(text, &end, 10);
	if (errno || !*text || *end)
		return -EPROTO;
	*out = n;
	return 0;
}
static int open_ubi_firmware(unsigned segment, uint64_t *bytes)
{
	if (segment >= 6)
		return -EINVAL;
	unsigned volume = 12 + segment, ma, mi;
	int used = 0;
	char text[64], want[64], path[64];
	uint64_t lebs, leb, corrupted, updating;
	snprintf(want, sizeof want, "nr_%s", names[segment]);
	if (firmware_attribute(volume, "name", text) || strcmp(text, want) ||
	    firmware_attribute(volume, "dev", text) ||
	    sscanf(text, "%u:%u%n", &ma, &mi, &used) != 2 || text[used] ||
	    firmware_number(volume, "reserved_ebs", &lebs) ||
	    firmware_number(volume, "usable_eb_size", &leb) ||
	    firmware_number(volume, "corrupted", &corrupted) ||
	    firmware_number(volume, "upd_marker", &updating) || corrupted || updating || !lebs ||
	    !leb || lebs > 67108864 / leb)
		return -EXDEV;
	snprintf(path, sizeof path, "/dev/ubi0_%u", volume);
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	struct stat st;
	if (fd < 3)
		return -errno;
	if (fstat(fd, &st) || !S_ISCHR(st.st_mode) || st.st_uid || (st.st_mode & 0022) ||
	    st.st_rdev != makedev(ma, mi)) {
		close(fd);
		return -EXDEV;
	}
	*bytes = lebs * leb;
	return fd;
}
static int prepare(struct wr_factory *f, int directory, int owner, int lease, int phase, int report,
		   int continuation, uint64_t nonce, const struct wr_policy *p, unsigned initial)
{
	if (!f || !nonce || wr_policy_validate(p, 1))
		return -EPERM;
	init(f);
	f->directory = fcntl(directory, F_DUPFD_CLOEXEC, 3);
	struct stat st;
	if (f->directory < 3 || fstat(f->directory, &st) || !S_ISDIR(st.st_mode) || st.st_uid ||
	    (st.st_mode & 0777) != 0700)
		return -EPERM;
	f->config.parent = getpid();
	f->config.owner = owner;
	f->config.lease = lease;
	f->config.phase = phase;
	f->config.report = report;
	f->config.continuation = continuation;
	f->config.epoch = nonce;
	f->config.reload = 1;
	f->config.operation_ms = p->nv.timeout_ms;
	f->config.continue_ms = p->nv.startup_ms;
	if (controls(f, 1) || layout())
		return -EXDEV;
	if (initial) {
		char text[4096];
		ssize_t n = pread(f->config.cmdline, text, sizeof text, 0);
		enum mcc_boot_hint hint;
		if (n < 0)
			return -errno;
		if (n <= 0 || n >= 4096 || mcc_boot_hint_parse(text, (size_t)n, &hint))
			return -EPROTO;
		if (hint != MCC_BOOT_SHUTDOWN)
			return -ENOLINK;
		/* Prove compatibility with the unchanged stock-style builder and verified
		 * spool tuples before a controller can STOP. Rebuild from fresh metadata
		 * after STOP as before; this is preflight, not a pinned atomic snapshot. */
		char built[2048];
		size_t used;
		int e = commandline_build(built, &used);
		if (e)
			return e;
	}

	for (unsigned i = 0; i < 6; i++) {
		const struct wr_firmware *w = &p->firmware[i];
		char path[128];
		uint64_t input_bytes = 0;
		int fd;
		if (p->firmware_ubi)
			fd = open_ubi_firmware(w->segment, &input_bytes);
		else {
			snprintf(path, sizeof path, "/usr/lib/firmware/udx710/%s.bin",
				 names[w->segment]);
			fd = fixed(path, O_RDONLY, 0);
			struct stat in;
			if (fd < 3 || fstat(fd, &in) || in.st_size <= 0)
				return -EPERM;
			input_bytes = (uint64_t)in.st_size;
		}
		if (fd < 3 || input_bytes > 67108864)
			return -EPERM;
		f->firmware[i] = fd;
		f->firmware_bytes[i] = input_bytes;
		struct mci_plan plan = {0};
		if (!w->format) {
			if (w->offset > input_bytes || w->length > input_bytes - w->offset ||
			    w->target_offset > cap[w->segment] ||
			    w->length > cap[w->segment] - w->target_offset)
				return -ERANGE;
			plan.extent_count = 1;
			plan.extents[0] =
			    (struct mci_extent){w->offset, w->target_offset, w->length};
		} else {
			unsigned char header[MCI_HEADER_BYTES];
			unsigned offset = w->format == 1 ? 0 : 512;
			if (pread(fd, header, sizeof header, offset) != (ssize_t)sizeof header ||
			    mci_parse_sci_header(header, sizeof header, offset, input_bytes,
						 cap[w->segment], &plan))
				return -EPROTO;
			/* Format3 is the native CPROC RAM ABI: SCI runtime payload at target0.
			 * DHTB/certificate bytes belong to the signed storage container, not the
			 * MECP runtime mapping. The legacy AP-write container is format2. */
			if (w->format == 3) {
				if (plan.extent_count != 2 ||
				    plan.extents[1].length <= MCI_SUFFIX_BYTES)
					return -EPROTO;
				plan.extents[0] = plan.extents[1];
				plan.extents[0].target_offset = 0;
				plan.extents[0].length -= MCI_SUFFIX_BYTES;
				plan.extent_count = 1;
				plan.total_bytes = plan.extents[0].length;
			}
		}
		if (w->segment < 3) {
			unsigned char header[16], entry[16];
			const struct mci_extent *runtime = &plan.extents[0];
			if (plan.extent_count != 1 || runtime->target_offset ||
			    runtime->length < 0x610 ||
			    pread(fd, header, sizeof header, (off_t)runtime->source_offset) !=
				(ssize_t)sizeof header ||
			    memcmp(header, "MECPV1.0", 8) || get32(header + 8) != 0x600 ||
			    pread(fd, entry, sizeof entry,
				  (off_t)(runtime->source_offset + 0x600)) !=
				(ssize_t)sizeof entry ||
			    get32(entry) != 0xe59ff000 || get32(entry + 4) != 0xe59ff000 ||
			    get32(entry + 8) < base[w->segment] ||
			    get32(entry + 8) >= base[w->segment] + runtime->length)
				return -ENOEXEC;
		}
		for (unsigned j = 0; j < plan.extent_count; j++) {
			int e = add(f, w->segment, i, &plan.extents[j]);
			if (e)
				return e;
		}
	}
	struct mci_extent cmd = {0, 0, cap[6]}, nv = {512, 0, cap[7]};
	if (add(f, 6, UINT_MAX, &cmd) || add(f, 7, UINT_MAX, &nv))
		return -EIO;
	nv.length = cap[8];
	if (add(f, 8, UINT_MAX, &nv))
		return -EIO;
	/* Reject an impossible physical transfer plan before any STOP: capacity
	 * aliases such as nrphy/nrdsp are not permission for overlapping writes. */
	for (unsigned i = 0; i < f->config.image_count; i++) {
		uint64_t a = f->config.images[i].physical_base + f->extent[i].target_offset;
		for (unsigned j = 0; j < i; j++) {
			uint64_t b = f->config.images[j].physical_base + f->extent[j].target_offset;
			if (a < b + f->extent[j].length && b < a + f->extent[i].length)
				return -ERANGE;
		}
	}
	return 0;
}
int wr_factory_prepare(struct wr_factory *f, int directory, int owner, int lease, int phase,
		       int report, int continuation, uint64_t nonce, const struct wr_policy *p)
{
	return prepare(f, directory, owner, lease, phase, report, continuation, nonce, p, 0);
}
int wr_factory_prepare_initial(struct wr_factory *f, int directory, int owner, int lease, int phase,
			       int report, int continuation, uint64_t nonce,
			       const struct wr_policy *p)
{
	return prepare(f, directory, owner, lease, phase, report, continuation, nonce, p, 1);
}
static int commandline_build(char built[2048], size_t *used)
{
	int ap = fixed("/proc/cmdline", O_RDONLY, 0x9fa0L);
	if (ap < 3)
		return -EXDEV;
	char text[4096];
	ssize_t n = pread(ap, text, sizeof text, 0);
	close(ap);
	if (n <= 0 || n >= 4096)
		return -EPROTO;
	struct mcc_log_tuple t[3];
	static const char *const log[] = {"slog_lte", "slog_nr_phy", "slog_v3_phy"};
	static const unsigned dst[] = {5, 7, 8};
	for (unsigned i = 0; i < 3; i++) {
		char path[128], b[128], tail;
		unsigned a, c, addr, pre, txnum, txsize, rxnum, rxsize;
		snprintf(path, sizeof path, "/sys/class/spool/%s/device/base_addr", log[i]);
		int fd = fixed(path, O_RDONLY, 0x62656572L);
		if (fd < 3)
			return -EXDEV;
		ssize_t got = pread(fd, b, sizeof b - 1, 0);
		close(fd);
		if (got <= 0 || got >= 127)
			return -EPROTO;
		b[got] = 0;
		int used = 0;
		if (sscanf(b, "%u %u 0x%x %u %u %u %u %u%c%n", &a, &c, &addr, &pre, &txnum, &txsize,
			   &rxnum, &rxsize, &tail, &used) != 9 ||
		    tail != '\n' || used != got || a != dst[i] || c != 5 || !addr || pre != 1 ||
		    txnum || txsize || rxsize != 2048 || rxnum != (i ? 1000u : 800u))
			return -EXDEV;
		t[i] = (struct mcc_log_tuple){a, c, addr};
	}
	return mcc_build_nr(text, (size_t)n, t, built, 2048, used) ? -EPROTO : 0;
}
static int commandline(int out, uint64_t deadline, unsigned char hash[32], uint64_t *length)
{
	char built[2048];
	size_t used;
	int e = commandline_build(built, &used);
	if (e)
		return e;
	if (pwrite(out, built, used, 0) != (ssize_t)used || ftruncate(out, (off_t)used) ||
	    fsync(out))
		return -EIO;
	*length = used;
	(void)deadline;
	memset(hash, 0, 32);
	return 0;
}
int wr_factory_snapshots(struct wr_factory *f, const struct wr_policy *p, uint64_t deadline)
{
	if (!f || !p || !f->config.reload || wr_policy_validate(p, 1))
		return -EINVAL;
	struct nvs_devices nv;
	int e;
	if (p->nv.storage == NVS_STORAGE_UBI) {
		e = wr_open_recovery_nv(&p->nv, &nv);
		if (e) {
			fprintf(stderr, "warm_factory=nv_recovery_open error=%d\n", e);
			return e;
		}
		for (unsigned pair = 0; pair < 2 && !e; pair++) {
			int fds[] = {nv.volume[pair * 2], nv.volume[pair * 2 + 1]};
			e = nv_vendor_repair_pair(fds, pair ? 9437184u : 7340032u, deadline);
		}
		nvs_close_devices(&nv);
		if (e) {
			fprintf(stderr, "warm_factory=nv_pair_repair error=%d\n", e);
			return e;
		}
	}
	e = wr_open_readonly_nv(&p->nv, &nv);
	if (e) {
		fprintf(stderr, "warm_factory=nv_metadata error=%d\n", e);
		return e;
	}
	for (unsigned i = 0; !e && i < f->config.image_count; i++) {
		struct mc_image *v = &f->config.images[i];
		if (v->segment < 6) {
			v->offset = f->extent[i].source_offset;
			memset(v->sha256, 0, 32);
			v->target_offset = f->extent[i].target_offset;
			v->length = f->extent[i].length;
		} else if (v->segment == 6) {
			e = commandline(f->output[i], deadline, v->sha256, &v->length);
			v->offset = v->target_offset = 0;
		} else {
			unsigned j = (v->segment - 7) * 2;
			v->source = nv.volume[j];
			v->backup = nv.volume[j + 1];
			nv.volume[j] = nv.volume[j + 1] = -1;
			v->offset = v->target_offset = 0;
			v->length = cap[v->segment];
			memset(v->sha256, 0, 32);
		}
	}
	if (e)
		fprintf(stderr, "warm_factory=snapshot error=%d\n", e);
	nvs_close_devices(&nv);
	if (!e) {
		e = mc_warm_plan_validate(&f->config);
		if (e)
			fprintf(stderr, "warm_factory=final_plan error=%d\n", e);
	}
	/* This is the sole writer factory. Only close, never CLOEXEC, removes every
	 * snapshot write description before the controller continuation is sent. */
	for (unsigned i = 0; i < f->config.image_count; i++) {
		if (f->output[i] >= 3 && close(f->output[i]) && !e)
			e = -errno;
		f->output[i] = -1;
		if (f->backup_output[i] >= 3 && close(f->backup_output[i]) && !e)
			e = -errno;
		f->backup_output[i] = -1;
	}
	return e;
}
void wr_factory_drop_child_caps(struct wr_factory *f)
{
	if (!f || f->caps_dropped)
		return;
	f->caps_dropped = 1;
	if (f->config.postload_mode) {
		int post[] = {f->config.devdir,	      f->config.status_session,
			      f->config.halo_session, f->config.provider_rx,
			      f->config.provider_tx,  f->config.verification_params,
			      f->config.events};
		for (unsigned j = 0; j < 7; j++)
			if (post[j] >= 3)
				close(post[j]);
	}
	for (unsigned d = 0; d < 4; d++) {
		int *ids[] = {&f->config.status[d], &f->config.start[d], &f->config.stop[d]};
		for (unsigned j = 0; j < 3; j++) {
			if (*ids[j] >= 3)
				close(*ids[j]); /* retain wire fd number in config for continuation
						   encoder */
		}
	}
	if (f->config.cmdline >= 3)
		close(f->config.cmdline);
	for (unsigned i = 0; i < MC_MAX_IMAGES; i++) {
		int ids[] = {f->config.images[i].target, f->config.images[i].source,
			     f->config.images[i].backup};
		for (unsigned j = 0; j < 3; j++)
			if (ids[j] >= 3)
				close(ids[j]);
	}
}
void wr_factory_close(struct wr_factory *f)
{
	if (!f)
		return;
	if (f->caps_dropped)
		for (unsigned i = 0; i < f->config.image_count; i++)
			if (f->config.images[i].segment >= 7) {
				if (f->config.images[i].source >= 3)
					close(f->config.images[i].source);
				if (f->config.images[i].backup >= 3)
					close(f->config.images[i].backup);
				f->config.images[i].source = f->config.images[i].backup = -1;
			}
	wr_factory_drop_child_caps(f);
	for (unsigned i = 0; i < 6; i++)
		if (f->firmware[i] >= 0) {
			close(f->firmware[i]);
			f->firmware[i] = -1;
		}
	for (unsigned i = 0; i < MC_MAX_IMAGES; i++) {
		if (f->output[i] >= 0)
			close(f->output[i]);
		if (f->backup_output[i] >= 0)
			close(f->backup_output[i]);
		f->output[i] = f->backup_output[i] = -1;
	}
	if (f->directory >= 0)
		close(f->directory);
	f->directory = -1;
}
/* APLOAD04 is emitted after the controller closes every image FD. At that
 * point these task-owned snapshots have no consumer and can release RAM. */
void wr_factory_remove_loaded_snapshots(struct wr_factory *f)
{
	if (!f || f->directory < 3)
		return;
	for (unsigned i = 0; i < f->config.image_count; i++)
		for (unsigned replica = 0; replica < 2; replica++) {
			if (f->config.images[i].segment != 6)
				continue;
			if (replica && f->config.images[i].segment < 7)
				continue;
			char name[64];
			snprintf(name, sizeof name, "snapshot-%016llx-%u-%u",
				 (unsigned long long)f->config.epoch, i, replica);
			if (unlinkat(f->directory, name, 0) && errno != ENOENT)
				fprintf(stderr, "snapshot_cleanup_error=%d slot=%u replica=%u\n",
					errno, i, replica);
		}
}
