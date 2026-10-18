/* SPDX-License-Identifier: GPL-2.0-only */
#include "common.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

_Thread_local char cell_error[512];
_Thread_local enum cell_error_kind cell_error_kind;
int errorf(enum cell_error_kind kind, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vsnprintf(cell_error, sizeof(cell_error), format, ap);
	va_end(ap);
	cell_error_kind = kind;
	return -1;
}
double mono(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
void delay_ms(unsigned ms)
{
	struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
	while (nanosleep(&t, &t) && errno == EINTR) {
	}
}
bool text_equal(const char *a, const char *b)
{
	return a && b && !strcmp(a, b);
}
bool hex_string(const char *s, size_t n)
{
	if (!s || strlen(s) != n)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!strchr("0123456789abcdef", s[i]))
			return false;
	return true;
}
bool integer_text(const char *s, int64_t low, int64_t high, int64_t *out)
{
	if (!s || !*s || (*s != '-' && !isdigit((unsigned char)*s)))
		return false;
	errno = 0;
	char *end;
	long long v = strtoll(s, &end, 10);
	if (errno || *end || v < low || v > high)
		return false;
	if (out)
		*out = v;
	return true;
}
int random_hex(char *out, size_t n)
{
	if (n > 64)
		return errorf(CELL_INVALID, "Random identifier too long");
	unsigned char bytes[64];
	size_t used = 0;
	while (used < n) {
		ssize_t r = getrandom(bytes + used, n - used, 0);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return errorf(CELL_SYSTEM, "getrandom: %s", strerror(errno));
		used += (size_t)r;
	}
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = "0123456789abcdef"[bytes[i] >> 4];
		out[2 * i + 1] = "0123456789abcdef"[bytes[i] & 15];
	}
	out[2 * n] = 0;
	return 0;
}
J *get(J *o, const char *key)
{
	J *v = NULL;
	if (o && json_object_is_type(o, json_type_object))
		json_object_object_get_ex(o, key, &v);
	return v;
}
const char *str(J *o, const char *key)
{
	J *v = get(o, key);
	return json_object_is_type(v, json_type_string) ? json_object_get_string(v) : NULL;
}
void string(J *o, const char *key, const char *s)
{
	json_object_object_add(o, key, s ? json_object_new_string(s) : NULL);
}
void integer(J *o, const char *key, int64_t n)
{
	json_object_object_add(o, key, json_object_new_int64(n));
}
void boolean(J *o, const char *key, bool v)
{
	json_object_object_add(o, key, json_object_new_boolean(v));
}
void real(J *o, const char *key, double v)
{
	json_object_object_add(o, key, isfinite(v) ? json_object_new_double(v) : NULL);
}
J *jclone(J *o)
{
	J *v = NULL;
	if (o && json_object_deep_copy(o, &v, NULL))
		return NULL;
	return v;
}
void merge(J *dst, J *src)
{
	if (!src)
		return;
	json_object_object_foreach(src, key, value)
	{
		json_object_object_add(dst, key, json_object_get(value));
	}
}
bool keys(J *o, const char *const *names, size_t count)
{
	if (!json_object_is_type(o, json_type_object) ||
	    (size_t)json_object_object_length(o) != count)
		return false;
	for (size_t i = 0; i < count; i++) {
		J *v;
		if (!json_object_object_get_ex(o, names[i], &v))
			return false;
	}
	return true;
}

/* Check duplicate keys before json-c would discard them. Also reject non-JSON
 * numeric extensions, embedded NUL strings and excessive nesting at ingress. */
struct scan {
	const char *p, *end;
	bool document;
};
static void space(struct scan *s)
{
	while (s->p < s->end && strchr(" \t\r\n", *s->p))
		s->p++;
}
static J *scan_string(struct scan *s, bool key)
{
	const char *start = s->p++;
	bool escaped = false;
	while (s->p < s->end) {
		unsigned char c = (unsigned char)*s->p++;
		if (!escaped && c == '"') {
			struct json_tokener *t = json_tokener_new();
			J *v = json_tokener_parse_ex(t, start, (int)(s->p - start));
			bool ok = json_tokener_get_error(t) == json_tokener_success &&
				  json_object_is_type(v, json_type_string);
			json_tokener_free(t);
			if (!ok ||
			    ((!s->document || key) && strlen(json_object_get_string(v)) !=
							  (size_t)json_object_get_string_len(v))) {
				json_object_put(v);
				return NULL;
			}
			return v;
		}
		if (c < 32)
			return NULL;
		if (escaped)
			escaped = false;
		else if (c == '\\')
			escaped = true;
	}
	return NULL;
}
static bool scan_value(struct scan *s, unsigned depth)
{
	space(s);
	if (s->p >= s->end || depth > 32)
		return false;
	if (*s->p == '"') {
		J *v = scan_string(s, false);
		bool ok = v != NULL;
		json_object_put(v);
		return ok;
	}
	if (*s->p == '{' || *s->p == '[') {
		bool object = *s->p++ == '{';
		char end = object ? '}' : ']';
		J *seen = object ? json_object_new_object() : NULL;
		bool ok = false;
		space(s);
		if (s->p < s->end && *s->p == end) {
			s->p++;
			ok = true;
			goto done;
		}
		for (unsigned count = 0; count < 4096; count++) {
			if (object) {
				space(s);
				if (s->p >= s->end || *s->p != '"')
					goto done;
				J *key = scan_string(s, true), *old;
				if (!key)
					goto done;
				const char *name = json_object_get_string(key);
				if (json_object_object_get_ex(seen, name, &old)) {
					json_object_put(key);
					goto done;
				}
				json_object_object_add(seen, name, json_object_new_boolean(true));
				json_object_put(key);
				space(s);
				if (s->p >= s->end || *s->p++ != ':')
					goto done;
			}
			if (!scan_value(s, depth + 1))
				goto done;
			space(s);
			if (s->p >= s->end)
				goto done;
			if (*s->p == end) {
				s->p++;
				ok = true;
				goto done;
			}
			if (*s->p++ != ',')
				goto done;
		}
	done:
		json_object_put(seen);
		return ok;
	}
	const char *words[] = {"true", "false", "null"};
	for (size_t i = 0; i < 3; i++) {
		size_t n = strlen(words[i]);
		if ((size_t)(s->end - s->p) >= n && !memcmp(s->p, words[i], n)) {
			s->p += n;
			return true;
		}
	}
	const char *number_start = s->p;
	if (*s->p == '-')
		s->p++;
	if (s->p >= s->end || !isdigit((unsigned char)*s->p))
		return false;
	if (*s->p == '0')
		s->p++;
	else
		while (s->p < s->end && isdigit((unsigned char)*s->p))
			s->p++;
	if (s->p < s->end && *s->p == '.') {
		s->p++;
		if (s->p >= s->end || !isdigit((unsigned char)*s->p))
			return false;
		while (s->p < s->end && isdigit((unsigned char)*s->p))
			s->p++;
	}
	if (s->p < s->end && (*s->p == 'e' || *s->p == 'E')) {
		s->p++;
		if (s->p < s->end && (*s->p == '+' || *s->p == '-'))
			s->p++;
		if (s->p >= s->end || !isdigit((unsigned char)*s->p))
			return false;
		while (s->p < s->end && isdigit((unsigned char)*s->p))
			s->p++;
	}
	size_t length = (size_t)(s->p - number_start);
	if (length > 127)
		return false;
	char numeric[128];
	memcpy(numeric, number_start, length);
	numeric[length] = 0;
	return isfinite(strtod(numeric, NULL));
}
static J *parse_document(const char *data, size_t n, bool document)
{
	if (!data || !n || n > 2097152) {
		errorf(CELL_INVALID, "JSON size invalid");
		return NULL;
	}
	struct scan s = {data, data + n, document};
	if (!scan_value(&s, 0)) {
		errorf(CELL_INVALID, "Invalid JSON or duplicate field");
		return NULL;
	}
	space(&s);
	if (s.p != s.end) {
		errorf(CELL_INVALID, "Trailing JSON data");
		return NULL;
	}
	struct json_tokener *t = json_tokener_new_ex(33);
	json_tokener_set_flags(t, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	J *o = json_tokener_parse_ex(t, data, (int)n);
	if (json_tokener_get_error(t) == json_tokener_continue)
		o = json_tokener_parse_ex(t, " ", 1);
	if (json_tokener_get_error(t) != json_tokener_success) {
		json_object_put(o);
		o = NULL;
		errorf(CELL_INVALID, "Invalid JSON encoding");
	}
	json_tokener_free(t);
	return o;
}
J *parse_json(const char *data, size_t n)
{
	return parse_document(data, n, false);
}
/* Stored SMS text may contain U+0000. Keys and RPC/command inputs stay strict. */
J *parse_json_document(const char *data, size_t n)
{
	return parse_document(data, n, true);
}
char *read_text(const char *path, size_t max, bool trim)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return NULL;
	size_t capacity = max < 4096 ? max : 4096, n = 0;
	char *p = malloc(capacity + 1);
	if (!p) {
		close(fd);
		return NULL;
	}
	for (;;) {
		if (n == capacity) {
			char extra;
			ssize_t r;
			do
				r = read(fd, &extra, 1);
			while (r < 0 && errno == EINTR);
			if (!r)
				break;
			if (r < 0 || n == max) {
				int saved = r < 0 ? errno : EFBIG;
				free(p);
				close(fd);
				errno = saved;
				return NULL;
			}
			size_t next = capacity > max / 2 ? max : capacity * 2;
			if (next <= capacity)
				next = max;
			char *grown = realloc(p, next + 1);
			if (!grown) {
				free(p);
				close(fd);
				return NULL;
			}
			p = grown;
			capacity = next;
			p[n++] = extra;
			if (n == capacity)
				continue;
		}
		ssize_t r = read(fd, p + n, capacity - n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			int saved = errno;
			free(p);
			close(fd);
			errno = saved;
			return NULL;
		}
		if (!r)
			break;
		n += (size_t)r;
	}
	close(fd);
	if (trim) {
		while (n && (!p[n - 1] || isspace((unsigned char)p[n - 1])))
			n--;
		size_t start = 0;
		while (start < n && isspace((unsigned char)p[start]))
			start++;
		memmove(p, p + start, n - start);
		n -= start;
	}
	p[n] = 0;
	return p;
}
J *read_json(const char *path, size_t maximum)
{
	char *p = read_text(path, maximum, false);
	if (!p) {
		int saved = errno;
		errorf(CELL_SYSTEM, "Read %s: %s", path, strerror(saved));
		errno = saved;
		return NULL;
	}
	errno = 0;
	J *o = parse_json(p, strlen(p));
	free(p);
	if (!o)
		errno = EINVAL;
	return o;
}
int mkdir_private(const char *path)
{
	struct stat st;
	if (mkdir(path, 0700) && errno != EEXIST)
		return errorf(CELL_SYSTEM, "mkdir %s: %s", path, strerror(errno));
	if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
		return errorf(CELL_SYSTEM, "Unexpected private directory %s", path);
	return 0;
}
static int write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t r = write(fd, p, n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		p += r;
		n -= (size_t)r;
	}
	return 0;
}
int atomic_json(const char *path, J *o, bool durable)
{
	char temp[PATH_MAX], parent[PATH_MAX];
	if (snprintf(temp, sizeof(temp), "%s.new", path) >= (int)sizeof(temp) ||
	    strlen(path) >= sizeof(parent))
		return errorf(CELL_INVALID, "Path too long");
	snprintf(parent, sizeof(parent), "%s", path);
	char *slash = strrchr(parent, '/');
	if (!slash)
		return errorf(CELL_INVALID, "Expected absolute file path");
	*slash = 0;
	int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return errorf(CELL_SYSTEM, "Open %s: %s", temp, strerror(errno));
	const char *s = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN |
							      JSON_C_TO_STRING_NOSLASHESCAPE);
	int r = write_all(fd, s, strlen(s));
	if (!r && durable)
		r = fsync(fd);
	if (close(fd) && !r)
		r = -1;
	if (!r)
		r = rename(temp, path);
	if (!r && durable) {
		fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			r = -1;
		else {
			r = fsync(fd);
			close(fd);
		}
	}
	if (r)
		return errorf(CELL_SYSTEM, "Persist %s: %s", path, strerror(errno));
	return 0;
}
int seqpacket_pinned(const char *path, const void *request, size_t size, char *response, size_t cap,
		     unsigned ms, pid_t *pid, int *peer_pidfd)
{
	if (peer_pidfd)
		*peer_pidfd = -1;
	if (!cap || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
		return errorf(CELL_INVALID, "Socket size invalid");
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return errorf(CELL_SYSTEM, "socket: %s", strerror(errno));
	struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	struct ucred cred;
	socklen_t len = sizeof(cred);
	int result = -1, pinned = -1;
	if (connect(fd, (void *)&addr, sizeof(addr)))
		goto fail;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid || cred.pid <= 0) {
		errno = EPERM;
		goto fail;
	}
	if (pid)
		*pid = cred.pid;
	if (peer_pidfd) {
		pinned = pidfd_open_checked(cred.pid);
		if (pinned < 0 || pidfd_exited(pinned))
			goto fail;
	}
	if (send(fd, request, size, MSG_NOSIGNAL) != (ssize_t)size)
		goto fail;
	struct iovec iov = {response, cap - 1};
	struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
	ssize_t n = recvmsg(fd, &msg, 0);
	if (n <= 0 || (msg.msg_flags & MSG_TRUNC)) {
		if (n >= 0)
			errno = EPROTO;
		goto fail;
	}
	response[n] = 0;
	result = (int)n;
	if (peer_pidfd) {
		*peer_pidfd = pinned;
		pinned = -1;
	}
	goto done;
fail:
	{
		int saved = errno;
		errorf(CELL_SYSTEM, "Service request failed: %s; do not replay uncertain mutations",
		       strerror(saved));
		errno = saved;
	}
done:
	if (pinned >= 0)
		close(pinned);
	close(fd);
	return result;
}
int seqpacket(const char *path, const void *request, size_t size, char *response, size_t cap,
	      unsigned ms, pid_t *pid)
{
	return seqpacket_pinned(path, request, size, response, cap, ms, pid, NULL);
}
int command_run(const char *const argv[], const char *input, unsigned ms, size_t max, char **out)
{
	int in[2] = {-1, -1}, output[2] = {-1, -1};
	if (out)
		*out = NULL;
	if (pipe2(in, O_CLOEXEC) || pipe2(output, O_CLOEXEC | O_NONBLOCK))
		goto pipe_fail;
	pid_t pid = fork();
	if (pid < 0)
		goto pipe_fail;
	if (!pid) {
		setpgid(0, 0);
		dup2(in[0], STDIN_FILENO);
		dup2(output[1], STDOUT_FILENO);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, STDERR_FILENO);
		close(in[0]);
		close(in[1]);
		close(output[0]);
		close(output[1]);
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	setpgid(pid, pid);
	close(in[0]);
	close(output[1]);
	fcntl(in[1], F_SETFL, O_NONBLOCK);
	char *buffer = malloc(max + 1);
	if (!buffer) {
		kill(-pid, SIGKILL);
		close(in[1]);
		close(output[0]);
		waitpid(pid, NULL, 0);
		return errorf(CELL_SYSTEM, "Out of memory");
	}
	size_t used = 0, sent = 0, size = input ? strlen(input) : 0;
	double end = mono() + ms / 1000.0;
	int status = 0;
	bool done = false, overflow = false;
	while (mono() < end) {
		if (in[1] >= 0) {
			ssize_t n = size > sent ? write(in[1], input + sent, size - sent) : 0;
			if (n > 0)
				sent += (size_t)n;
			if (sent == size || (n < 0 && errno != EAGAIN && errno != EINTR)) {
				close(in[1]);
				in[1] = -1;
			}
		}
		ssize_t n = read(output[0], buffer + used, max - used);
		if (n > 0)
			used += (size_t)n;
		if (used == max) {
			overflow = true;
			break;
		}
		if (waitpid(pid, &status, WNOHANG) == pid) {
			done = true;
			while ((n = read(output[0], buffer + used, max - used)) > 0) {
				used += (size_t)n;
				if (used == max) {
					overflow = true;
					break;
				}
			}
			break;
		}
		struct pollfd fds[2] = {{output[0], POLLIN, 0}, {in[1], POLLOUT, 0}};
		poll(fds, 2, 20);
	}
	if (in[1] >= 0)
		close(in[1]);
	close(output[0]);
	if (!done) {
		kill(-pid, SIGKILL);
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
	}
	if (!done || overflow || sent != size || !WIFEXITED(status) || WEXITSTATUS(status)) {
		free(buffer);
		return errorf(!done ? CELL_UNCERTAIN : CELL_SYSTEM, "%s failed%s", argv[0],
			      !done	 ? ": timeout, outcome uncertain"
			      : overflow ? ": output limit"
					 : "");
	}
	buffer[used] = 0;
	if (out)
		*out = buffer;
	else
		free(buffer);
	return 0;
pipe_fail:
	for (size_t i = 0; i < 2; i++) {
		if (in[i] >= 0)
			close(in[i]);
		if (output[i] >= 0)
			close(output[i]);
	}
	return errorf(CELL_SYSTEM, "Create command pipes: %s", strerror(errno));
}
int pidfd_open_checked(pid_t pid)
{
	return (int)syscall(SYS_pidfd_open, pid, 0);
}
int pidfd_signal(int fd, int sig)
{
	return (int)syscall(SYS_pidfd_send_signal, fd, sig, NULL, 0);
}
bool pidfd_exited(int fd)
{
	struct pollfd p = {fd, POLLIN, 0};
	return poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR));
}
bool volatile_run(void)
{
	struct statfs fs;
	return !statfs("/run", &fs) && fs.f_type == TMPFS_MAGIC;
}
