/* SPDX-License-Identifier: GPL-2.0-only Standard UCI CLI and bounded modem CSV. */
#include "common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef CELL_UCI_CONFIG
#define CELL_UCI_CONFIG "/etc/config/udx-cellular"
#endif

/* uci show emits POSIX-quoted scalar strings, not commands. Parse the output
 * without invoking a shell or exposing credential values in process argv. */
static char *unquote(const char *p)
{
	size_t n = strlen(p);
	char *out = malloc(n + 1);
	if (!out)
		return NULL;
	size_t used = 0;
	char quote = 0;
	for (size_t i = 0; i < n; i++) {
		char c = p[i];
		if (!quote && (c == '\'' || c == '"')) {
			quote = c;
			continue;
		}
		if (quote && c == quote) {
			quote = 0;
			continue;
		}
		if (c == '\\' && quote != '\'') {
			if (++i == n)
				goto bad;
			c = p[i];
		} else if (!quote && isspace((unsigned char)c))
			goto bad;
		if (c == '\n' || c == '\r')
			goto bad;
		out[used++] = c;
	}
	if (quote)
		goto bad;
	out[used] = 0;
	return out;
bad:
	free(out);
	errorf(CELL_INVALID, "Invalid UCI scalar");
	return NULL;
}
J *uci_sections(void)
{
	const char *argv[] = {"/sbin/uci", "-q", "show", "udx-cellular", NULL};
	char *text = NULL;
	if (command_run(argv, NULL, 5000, 131072, &text))
		return NULL;
	J *out = json_object_new_object();
	char *save, *line = strtok_r(text, "\n", &save);
	for (; line; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "udx-cellular.", 13))
			goto bad;
		char *equal = strchr(line, '=');
		if (!equal)
			goto bad;
		*equal++ = 0;
		char *name = line + 13, *key = strchr(name, '.');
		if (key)
			*key++ = 0;
		if (!*name || (key && (!*key || strchr(key, '.'))))
			goto bad;
		char *value = unquote(equal);
		if (!value)
			goto bad;
		J *row = get(out, name);
		if (!row) {
			row = json_object_new_object();
			json_object_object_add(out, name, row);
		}
		string(row, key ? key : ".type", value);
		free(value);
	}
	free(text);
	return out;
bad:
	free(text);
	json_object_put(out);
	errorf(CELL_INVALID, "Invalid UCI cellular output");
	return NULL;
}
static bool uci_key(const char *p)
{
	unsigned parts = 0;
	for (;;) {
		if (!(isalpha((unsigned char)*p) || *p == '_'))
			return false;
		for (p++; isalnum((unsigned char)*p) || *p == '_'; p++) {
		}
		if (!*p)
			return true;
		if (*p++ != '.' || ++parts > 1)
			return false;
	}
}
int uci_commit(J *values, J *deletes)
{
	char *input = NULL;
	size_t length = 0;
	FILE *f = open_memstream(&input, &length);
	if (!f)
		return errorf(CELL_SYSTEM, "UCI buffer allocation");
	if (deletes)
		for (size_t i = 0; i < json_object_array_length(deletes); i++) {
			const char *key =
			    json_object_get_string(json_object_array_get_idx(deletes, i));
			if (!uci_key(key))
				goto bad;
			fprintf(f, "delete udx-cellular.%s\n", key);
		}
	if (values) {
		json_object_object_foreach(values, key, value)
		{
			const char *v = json_object_get_string(value);
			if (!uci_key(key) || !v || strchr(v, '\n') || strchr(v, '\r') ||
			    (!json_object_is_type(value, json_type_string) &&
			     !json_object_is_type(value, json_type_int)) ||
			    (json_object_is_type(value, json_type_string) &&
			     strlen(v) != (size_t)json_object_get_string_len(value)))
				goto bad;
			fprintf(f, "set udx-cellular.%s='", key);
			for (; *v; v++) {
				if (*v == '\'')
					fputs("'\\''", f);
				else
					fputc(*v, f);
			}
			fputs("'\n", f);
		}
	}
	fputs("commit udx-cellular\n", f);
	if (fclose(f)) {
		free(input);
		return errorf(CELL_SYSTEM, "UCI buffer close");
	}
	const char *argv[] = {"/sbin/uci", "-q", "batch", NULL};
	int r = command_run(argv, input, 5000, 65536, NULL);
	if (input) {
		explicit_bzero(input, length);
		free(input);
	}
	if (r)
		return -1;
	if (chmod(CELL_UCI_CONFIG, 0600))
		return errorf(CELL_SYSTEM, "UCI permission update");
	return 0;
bad:
	fclose(f);
	if (input) {
		explicit_bzero(input, length);
		free(input);
	}
	return errorf(CELL_INVALID, "Invalid UCI field or value");
}
bool at_complete(const char *raw)
{
	if (!raw)
		return false;
	size_t n = strlen(raw);
	while (n && isspace((unsigned char)raw[n - 1]))
		n--;
	return n >= 2 && raw[n - 2] == 'O' && raw[n - 1] == 'K' &&
	       (n == 2 || raw[n - 3] == '\r' || raw[n - 3] == '\n');
}
static J *csv(const char *p, size_t n)
{
	J *row = json_object_new_array();
	char *buffer = malloc(n + 1);
	if (!buffer) {
		json_object_put(row);
		return NULL;
	}
	if (!n) {
		free(buffer);
		return row;
	}
	size_t i = 0;
	for (;;) {
		size_t used = 0;
		bool quoted = i < n && p[i] == '"';
		if (quoted)
			i++;
		while (i < n) {
			char c = p[i++];
			if (quoted && c == '"') {
				if (i < n && p[i] == '"') {
					buffer[used++] = '"';
					i++;
					continue;
				}
				quoted = false;
				if (i < n && p[i] != ',')
					goto bad;
				break;
			}
			if (!quoted && c == ',') {
				i--;
				break;
			}
			buffer[used++] = c;
		}
		if (quoted || json_object_array_length(row) >= 1024)
			goto bad;
		buffer[used] = 0;
		json_object_array_add(row, json_object_new_string_len(buffer, (int)used));
		if (i == n)
			break;
		if (p[i++] != ',')
			goto bad;
		if (i == n) {
			json_object_array_add(row, json_object_new_string(""));
			break;
		}
	}
	free(buffer);
	return row;
bad:
	free(buffer);
	json_object_put(row);
	errorf(CELL_INVALID, "Malformed AT CSV row");
	return NULL;
}
J *at_fields(const char *raw, const char *prefix)
{
	if (!at_complete(raw)) {
		errorf(CELL_INVALID, "Missing successful terminal response");
		return NULL;
	}
	J *rows = json_object_new_array();
	const char *p = raw;
	size_t plen = strlen(prefix);
	while (*p) {
		const char *end = p + strcspn(p, "\r\n");
		while (p < end && isspace((unsigned char)*p))
			p++;
		const char *last = end;
		while (last > p && isspace((unsigned char)last[-1]))
			last--;
		if ((size_t)(last - p) >= plen && !memcmp(p, prefix, plen)) {
			const char *value = p + plen;
			while (value < last && isspace((unsigned char)*value))
				value++;
			J *row = csv(value, (size_t)(last - value));
			if (!row) {
				json_object_put(rows);
				return NULL;
			}
			json_object_array_add(rows, row);
		}
		p = end;
		while (*p == '\r' || *p == '\n')
			p++;
	}
	return rows;
}
J *at_one(const char *raw, const char *prefix)
{
	J *rows = at_fields(raw, prefix);
	if (!rows)
		return NULL;
	if (json_object_array_length(rows) != 1) {
		json_object_put(rows);
		errorf(CELL_INVALID, "Missing or ambiguous %s", prefix);
		return NULL;
	}
	J *row = json_object_get(json_object_array_get_idx(rows, 0));
	json_object_put(rows);
	return row;
}
