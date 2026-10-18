/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "capsule.h"
#include <fcntl.h>
#include <libfdt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int property(const void *dt, int node, const char *name, const char *value)
{
	int bytes;
	const char *p = fdt_getprop(dt, node, name, &bytes);
	return p && bytes == (int)strlen(value) + 1 && !memcmp(p, value, bytes);
}

int main(int argc, char **argv)
{
	struct stat st;
	struct udx_boot_parts parts;
	int fd, result = 1;
	void *data;
	if (argc != 4)
		return 64;
	char *end;
	unsigned long long root_bytes = strtoull(argv[3], &end, 10);
	if (*end || root_bytes < 96 || root_bytes > 397u * 126976u)
		return 1;
	fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return 1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    st.st_size > UDX_BOOT_VOLUME_BYTES) {
		close(fd);
		return 1;
	}
	size_t kernel_bytes = st.st_size;
	data = mmap(NULL, kernel_bytes, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (data == MAP_FAILED)
		return 1;
	if (udx_boot_parts(data, st.st_size, UDX_BOOT_CHOICE_OPENWRT, &parts) ||
	    fdt_check_header(parts.dtb) || fdt_node_check_compatible(parts.dtb, 0, "sprd,sy108"))
		goto done;
	int chosen = fdt_path_offset(parts.dtb, "/chosen"), bytes;
	if (chosen < 0 || !property(parts.dtb, chosen, "udx,boot-policy", "openwrt-v1") ||
	    !property(parts.dtb, chosen, "udx,boot-target", "openwrt") ||
	    !fdt_getprop(parts.dtb, chosen, "udx,trial-local-lease", &bytes))
		goto done;
	unsigned char header[96];
	fd = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		goto done;
	int bad = read(fd, header, sizeof(header)) != sizeof(header);
	close(fd);
	if (bad || memcmp(header, "hsqs", 4) || header[28] != 4 || header[29])
		goto done;
	result = 0;
done:
	munmap(data, kernel_bytes);
	if (result)
		fputs("Unsupported SY108 kernel or rootfs image\n", stderr);
	return result;
}
