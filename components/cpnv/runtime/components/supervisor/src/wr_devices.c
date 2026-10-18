/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "nvs_devices.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#define nvs_open open
#define nvs_openat openat
#define nvs_fstat fstat
#define nvs_fstatfs fstatfs
#define nvs_pread pread
#define nvs_close close
static const unsigned ids[4] = {2, 3, 5, 6};
static const char *const names[4] = {"nr_fixnv1", "nr_fixnv2", "nr_runtimenv1", "nr_runtimenv2"};
static int number(const char *s, uint64_t *v)
{
	uint64_t n = 0;
	if (!s || !*s)
		return -1;
	while (*s) {
		unsigned d = (unsigned)(*s++ - '0');
		if (d > 9 || n > (UINT64_MAX - d) / 10)
			return -1;
		n = n * 10 + d;
	}
	*v = n;
	return 0;
}
int nvs_profile_validate(const struct nvs_profile *p)
{
	if (!p || p->enabled != 1 || p->exclusive != 1 || p->checksum_policy != 1)
		return -EPERM;
	/* Historical volumes exceed image size, so no tail-preserving policy is
	 * valid for this exact profile. 3 explicitly means whole-volume discard. */
	if (p->storage != NVS_STORAGE_UBI || p->write_policy != 3u)
		return -EPERM;
	if (!p->timeout_ms || p->timeout_ms > 60000 || !p->startup_ms || p->startup_ms > 120000)
		return -EINVAL;
	return 0;
}
static int attr(int dir, const char *name, char out[64])
{
	int fd = nvs_openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW), r = -EINVAL;
	struct stat st;
	struct statfs fs;
	ssize_t n;
	if (fd < 0)
		return -errno;
	if (nvs_fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode & 0222) ||
	    nvs_fstatfs(fd, &fs) || fs.f_type != 0x62656572L)
		goto done;
	n = nvs_pread(fd, out, 63, 0);
	if (n <= 0 || n >= 63 || out[n - 1] != '\n' || memchr(out, 0, (size_t)n))
		goto done;
	out[n - 1] = 0;
	r = 0;
done:
	nvs_close(fd);
	return r;
}
static int attr_num(int dir, const char *name, uint64_t *v)
{
	char b[64];
	return attr(dir, name, b) || number(b, v) ? -EINVAL : 0;
}
static int attr_dev(int dir, uint64_t *v)
{
	char b[64], *s;
	uint64_t ma, mi;
	if (attr(dir, "dev", b) || !(s = strchr(b, ':')))
		return -EINVAL;
	*s++ = 0;
	if (number(b, &ma) || number(s, &mi) || ma > UINT_MAX || mi > UINT_MAX)
		return -EINVAL;
	*v = (uint64_t)makedev((unsigned)ma, (unsigned)mi);
	return major((dev_t)*v) != ma || minor((dev_t)*v) != mi ? -EINVAL : 0;
}
static int directory_mode(const char *path, int sysfs, int follow_class)
{
	int fd =
	    nvs_open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | (follow_class ? 0 : O_NOFOLLOW));
	struct stat st;
	struct statfs fs;
	if (fd < 0)
		return -1;
	if (nvs_fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid || (st.st_mode & 0022) ||
	    (sysfs && (nvs_fstatfs(fd, &fs) || fs.f_type != 0x62656572L))) {
		nvs_close(fd);
		return -1;
	}
	return fd;
}
static int directory(const char *path, int sysfs)
{
	return directory_mode(path, sysfs, 0);
}
/* Only fixed kernel-owned sysfs class paths may follow their class symlink. */
static int class_directory(const char *path)
{
	return directory_mode(path, 1, 1);
}
static int volume_metadata(struct nvs_devices *d, unsigned i, uint64_t base, uint64_t minio,
			   int allow_damage)
{
	uint64_t lebs, leb, corrupt, marker, dev;
	char name[64], type[64];
	unsigned want_lebs = i < 2 ? 67 : 83;
	if (attr_dev(d->vol_dir[i], &dev) || major((dev_t)dev) != major((dev_t)base) ||
	    minor((dev_t)dev) != ids[i] + 1 || attr_num(d->vol_dir[i], "reserved_ebs", &lebs) ||
	    lebs != want_lebs || attr_num(d->vol_dir[i], "usable_eb_size", &leb) || leb != 126976 ||
	    leb % minio || attr_num(d->vol_dir[i], "corrupted", &corrupt) ||
	    (!allow_damage && corrupt) || attr_num(d->vol_dir[i], "upd_marker", &marker) ||
	    (!allow_damage && marker) || attr(d->vol_dir[i], "name", name) ||
	    strcmp(name, names[i]) || attr(d->vol_dir[i], "type", type) || strcmp(type, "dynamic"))
		return -EINVAL;
	if (d->volume_rdev[i] && d->volume_rdev[i] != dev)
		return -ESTALE;
	d->volume_rdev[i] = dev;
	d->capacity[i] = lebs * leb;
	return 0;
}
void nvs_close_devices(struct nvs_devices *d)
{
	if (!d)
		return;
	for (unsigned i = 0; i < 4; i++) {
		if (d->volume[i] >= 0)
			nvs_close(d->volume[i]);
		if (d->vol_dir[i] >= 0)
			nvs_close(d->vol_dir[i]);
		d->volume[i] = d->vol_dir[i] = -1;
	}
	if (d->dev_dir >= 0)
		nvs_close(d->dev_dir);
	if (d->ubi_dir >= 0)
		nvs_close(d->ubi_dir);
	d->dev_dir = d->ubi_dir = -1;
}
static int open_devices_mode(const struct nvs_profile *p, struct nvs_devices *d, int access,
			     int allow_damage)
{
	uint64_t base, minio, ro;
	int sd = -1, r;
	char path[96];
	struct stat st;
	memset(d, 0, sizeof(*d));
	d->dev_dir = d->ubi_dir = -1;
	for (unsigned i = 0; i < 4; i++)
		d->volume[i] = d->vol_dir[i] = -1;
	if ((r = nvs_profile_validate(p)))
		return r;
	d->dev_dir = directory("/dev", 0);
	d->ubi_dir = class_directory("/sys/class/ubi/ubi0");
	if (d->dev_dir < 3 || d->ubi_dir < 3 || attr_dev(d->ubi_dir, &base) || minor((dev_t)base) ||
	    attr_num(d->ubi_dir, "min_io_size", &minio) || !minio || minio > 126976 ||
	    (minio & (minio - 1)) || 126976 % minio || attr_num(d->ubi_dir, "ro_mode", &ro) || ro)
		goto fail;
	sd = directory("/sys/class/spipe/snv_lte", 1); /* class entry itself is a sysfs symlink */
	if (sd < 0) {
		/* Follow only the kernel-owned class path, then verify sysfs/root metadata.
		 * Character rdev is also checked by nvc_open_transport at actual fresh open. */
		sd = nvs_open("/sys/class/spipe/snv_lte", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		struct statfs fs;
		if (sd < 3 || nvs_fstat(sd, &st) || !S_ISDIR(st.st_mode) || st.st_uid ||
		    nvs_fstatfs(sd, &fs) || fs.f_type != 0x62656572L)
			goto fail;
	}
	if (attr_dev(sd, &d->transport_rdev) || !d->transport_rdev)
		goto fail;
	nvs_close(sd);
	sd = -1;
	for (unsigned i = 0; i < 4; i++) {
		snprintf(path, sizeof path, "/sys/class/ubi/ubi0_%u", ids[i]);
		d->vol_dir[i] = class_directory(path);
		if (d->vol_dir[i] < 3 || volume_metadata(d, i, base, minio, allow_damage))
			goto fail;
		for (unsigned j = 0; j < i; j++)
			if (d->volume_rdev[i] == d->volume_rdev[j])
				goto fail;
	}
	/* All names, geometry, health and policy have passed before opening storage. */
	for (unsigned i = 0; i < 4; i++) {
		snprintf(path, sizeof path, "ubi0_%u", ids[i]);
		d->volume[i] = nvs_openat(d->dev_dir, path, access | O_CLOEXEC | O_NOFOLLOW);
		if (d->volume[i] < 3 || nvs_fstat(d->volume[i], &st) || !S_ISCHR(st.st_mode) ||
		    st.st_uid || (st.st_mode & 0002) || (uint64_t)st.st_rdev != d->volume_rdev[i] ||
		    volume_metadata(d, i, base, minio, allow_damage))
			goto fail;
		for (unsigned j = 0; j < 3; j++) {
			struct stat io;
			if (nvs_fstat((int)j, &io) ||
			    (S_ISCHR(io.st_mode) && io.st_rdev == st.st_rdev))
				goto fail;
		}
	}
	if (attr_num(d->ubi_dir, "ro_mode", &ro) || ro)
		return nvs_close_devices(d), -ESTALE;
	return 0;
fail:
	if (sd >= 0)
		nvs_close(sd);
	nvs_close_devices(d);
	return -EINVAL;
}

int nvs_open_devices(const struct nvs_profile *p, struct nvs_devices *d)
{
	return open_devices_mode(p, d, O_RDWR, 0);
}
int wr_open_readonly_nv(const struct nvs_profile *p, struct nvs_devices *d)
{
	return open_devices_mode(p, d, O_RDONLY, 0);
}

int wr_open_recovery_nv(const struct nvs_profile *p, struct nvs_devices *d)
{
	if (p->storage != NVS_STORAGE_UBI)
		return -EINVAL;
	return open_devices_mode(p, d, O_RDWR, 1);
}
