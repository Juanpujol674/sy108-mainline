/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int num(const char *s, uint64_t *v)
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

static int digest(const char *s, unsigned char *v)
{
	if (strlen(s) != 64)
		return -1;
	for (unsigned i = 0; i < 32; i++) {
		unsigned n = 0;
		for (unsigned j = 0; j < 2; j++) {
			char c = s[2 * i + j];
			unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
				     : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
							    : 16;
			if (d > 15)
				return -1;
			n = n * 16 + d;
		}
		v[i] = (unsigned char)n;
	}
	return 0;
}

int cpnv_policy_parse(const char *text, size_t n, struct cpnv_policy *p)
{
	if (!text || !p || !n || n >= 4096 || memchr(text, 0, n))
		return -EINVAL;
	char b[4096];
	memcpy(b, text, n);
	b[n] = 0;
	memset(p, 0, sizeof *p);
	unsigned seen = 0, fw = 0, storage = 0, firmware_source = 0;
	char *save, *s;
	static const char *keys[] = {"enabled", "write_policy", "timeout_ms", "startup_ms"};
	for (s = strtok_r(b, "\n", &save); s; s = strtok_r(NULL, "\n", &save)) {
		if (*s == '#')
			continue;
		if (!strcmp(s, "storage=ubi")) {
			if (storage++)
				return -EINVAL;
			p->nv.storage = 1;
			continue;
		}
		if (!strcmp(s, "firmware_source=ubi") || !strcmp(s, "firmware_source=files")) {
			if (firmware_source++)
				return -EINVAL;
			p->firmware_ubi = !strcmp(s, "firmware_source=ubi");
			continue;
		}
		if (!strncmp(s, "firmware=", 9)) {
			char *token[6], *state;
			unsigned count = 0;
			for (char *v = strtok_r(s + 9, " ", &state); v;
			     v = strtok_r(NULL, " ", &state)) {
				if (count == 6)
					return -EINVAL;
				token[count++] = v;
			}
			if (count != 6 || p->count >= 6)
				return -EINVAL;
			uint64_t segment, format, off, len, target;
			if (num(token[0], &segment) || segment > 5 || num(token[1], &format) ||
			    format > 3 || num(token[3], &off) || num(token[4], &len) ||
			    num(token[5], &target) || (fw & (1u << segment)))
				return -EINVAL;
			struct firmware *f = &p->firmware[p->count++];
			f->segment = (unsigned)segment;
			f->format = (unsigned)format;
			f->offset = off;
			f->length = len;
			f->target_offset = target;
			if (digest(token[2], f->sha256) || (format && (off || len || target)) ||
			    (!format && !len))
				return -EINVAL;
			fw |= 1u << segment;
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq)
			return -EINVAL;
		*eq++ = 0;
		unsigned i;
		for (i = 0; i < 4; i++)
			if (!strcmp(s, keys[i]))
				break;
		uint64_t v;
		if (i == 4 || (seen & (1u << i)) || num(eq, &v) || v > UINT_MAX)
			return -EINVAL;
		seen |= 1u << i;
		switch (i) {
		case 0:
			p->enabled = (unsigned)v;
			break;
		case 1:
			p->nv.write_policy = (unsigned)v;
			break;
		case 2:
			p->nv.timeout_ms = (unsigned)v;
			break;
		case 3:
			p->nv.startup_ms = (unsigned)v;
			break;
		}
	}
	if (seen != 15 || (p->count && fw != 63))
		return -EINVAL;
	p->nv.enabled = p->enabled;
	return 0;
}
