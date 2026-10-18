// SPDX-License-Identifier: GPL-2.0-only
// Negotiated modem addresses are applied only through OpenWrt netifd.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <json-c/json.h>

#ifndef RUN
#define RUN "/run/udx-cellular"
#endif
#ifndef BOOT
#define BOOT "/proc/sys/kernel/random/boot_id"
#endif
#ifndef UBUS
#define UBUS "/bin/ubus"
#endif
typedef struct json_object J;
static volatile sig_atomic_t running = 1;
static char boot[64], active[65], error[256];
static int metric;
static bool requested;
static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void stop(int signo)
{
	(void)signo;
	running = 0;
}
static J *get(J *o, const char *key)
{
	J *v = NULL;
	if (o)
		json_object_object_get_ex(o, key, &v);
	return v;
}
static const char *str(J *o, const char *key)
{
	J *v = get(o, key);
	return v && json_object_is_type(v, json_type_string) ? json_object_get_string(v) : NULL;
}
static void string(J *o, const char *key, const char *value)
{
	json_object_object_add(o, key, value ? json_object_new_string(value) : NULL);
}
static void fail(const char *text)
{
	snprintf(error, sizeof(error), "%s", text);
}
static J *read_json(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return NULL;
	char data[65537];
	ssize_t n = read(fd, data, sizeof(data));
	close(fd);
	if (n <= 0 || n >= 65536)
		return NULL;
	data[n] = 0;
	struct json_tokener *t = json_tokener_new();
	J *o = json_tokener_parse_ex(t, data, n);
	if (json_tokener_get_error(t) != json_tokener_success) {
		json_object_put(o);
		o = NULL;
	}
	json_tokener_free(t);
	return o;
}
static int atomic(J *o)
{
	const char *data = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
	int fd = open(RUN "/cellular-netifd.new",
		      O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	size_t len = strlen(data), off = 0;
	while (off < len) {
		ssize_t n = write(fd, data + off, len - off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			close(fd);
			return -1;
		}
		off += n;
	}
	if (close(fd))
		return -1;
	return rename(RUN "/cellular-netifd.new", RUN "/cellular-netifd.json");
}
// Bound both output and execution time. No shell and no inherited owner FDs.
static J *ubus(const char *object, const char *method, J *args)
{
	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK))
		return NULL;
	pid_t pid = fork();
	if (pid == 0) {
		setpgid(0, 0);
		dup2(pipefd[1], STDOUT_FILENO);
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDERR_FILENO);
		}
		close(pipefd[0]);
		close(pipefd[1]);
		execl(UBUS, "ubus", "-t", "5", "call", object, method,
		      args ? json_object_to_json_string_ext(args, JSON_C_TO_STRING_PLAIN) : "{}",
		      (char *)NULL);
		_exit(127);
	}
	close(pipefd[1]);
	if (pid < 0) {
		close(pipefd[0]);
		return NULL;
	}
	setpgid(pid, pid);
	char data[65537];
	size_t used = 0;
	int status = 0;
	bool done = false, overflow = false;
	double end = now() + 8;
	while (now() < end) {
		ssize_t n = read(pipefd[0], data + used, sizeof(data) - 1 - used);
		if (n > 0)
			used += n;
		if (used == sizeof(data) - 1) {
			overflow = true;
			break;
		}
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			done = true;
			while ((n = read(pipefd[0], data + used, sizeof(data) - 1 - used)) > 0)
				used += n;
			break;
		}
		struct pollfd p = {pipefd[0], POLLIN, 0};
		poll(&p, 1, 20);
	}
	close(pipefd[0]);
	if (!done) {
		kill(-pid, SIGKILL);
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
		return NULL;
	}
	if (overflow || used == sizeof(data) - 1 || !WIFEXITED(status) || WEXITSTATUS(status))
		return NULL;
	data[used] = 0;
	return used ? json_tokener_parse(data) : json_object_new_object();
}
static int request(const char *operation, int seconds)
{
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct timeval tv = {seconds, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), RUN "/control.sock");
	if (connect(fd, (void *)&addr, sizeof(addr))) {
		close(fd);
		return -1;
	}
	struct ucred cred;
	socklen_t size = sizeof(cred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) || cred.uid != 0) {
		close(fd);
		return -1;
	}
	char data[65536];
	int len = snprintf(data, sizeof(data), "{\"operation\":\"%s\",\"arguments\":%s}", operation,
			   !strcmp(operation, "connect") ? "{\"automatic\":true}" : "{}");
	if (send(fd, data, len, MSG_NOSIGNAL) != len) {
		close(fd);
		return -1;
	}
	ssize_t n = recv(fd, data, sizeof(data) - 1, 0);
	int saved = errno;
	close(fd);
	// A receive timeout can follow an accepted mutation. Do not replay it.
	if (n < 0 && (saved == EAGAIN || saved == EWOULDBLOCK))
		return 1;
	if (n <= 0)
		return -1;
	data[n] = 0;
	J *reply = json_tokener_parse(data);
	int result = json_object_get_boolean(get(reply, "accepted")) ? 1 : 0;
	if (!result)
		fail(str(reply, "error") ? str(reply, "error") : "Cellular request rejected");
	json_object_put(reply);
	return result;
}
static bool address(const char *text, int family)
{
	unsigned char data[16];
	return !text || inet_pton(family, text, data) == 1;
}
static bool session_valid(J *s)
{
	const char *id = str(s, "id"), *dev = str(s, "interface"), *apn = str(s, "apn");
	J *dns = get(s, "dns");
	if (!id || !*id || strlen(id) > 64 || !dev || strcmp(dev, "sipa_eth0") || !apn ||
	    strlen(apn) > 253 || !dns || !json_object_is_type(dns, json_type_array))
		return false;
	if (!address(str(s, "ipv4"), AF_INET) || !address(str(s, "ipv6"), AF_INET6) ||
	    !address(str(s, "ipv6_linklocal"), AF_INET6))
		return false;
	for (size_t i = 0; i < json_object_array_length(dns); i++) {
		J *v = json_object_array_get_idx(dns, i);
		if (!json_object_is_type(v, json_type_string))
			return false;
		const char *a = json_object_get_string(v);
		if (!address(a, AF_INET) && !address(a, AF_INET6))
			return false;
	}
	return true;
}
static void add_address(J *array, const char *ip, const char *mask)
{
	if (!ip)
		return;
	J *o = json_object_new_object();
	string(o, "ipaddr", ip);
	string(o, "mask", mask);
	json_object_array_add(array, o);
}
static J *notification(J *session)
{
	J *o = json_object_new_object();
	json_object_object_add(o, "action", json_object_new_int(0));
	string(o, "interface", "wan");
	string(o, "ifname", "sipa_eth0");
	json_object_object_add(o, "link-up", json_object_new_boolean(session != NULL));
	json_object_object_add(o, "keep", json_object_new_boolean(false));
	if (session) {
		J *v4 = json_object_new_array(), *v6 = json_object_new_array(),
		  *routes = json_object_new_array(), *data = json_object_new_object();
		add_address(v4, str(session, "ipv4"), "32");
		add_address(v6, str(session, "ipv6_linklocal"), "64");
		add_address(v6, str(session, "ipv6"), "64");
		if (str(session, "ipv4")) {
			J *route = json_object_new_object();
			string(route, "target", "0.0.0.0");
			string(route, "netmask", "0");
			json_object_object_add(route, "metric", json_object_new_int(metric));
			json_object_array_add(routes, route);
		}
		json_object_object_add(o, "ipaddr", v4);
		json_object_object_add(o, "ip6addr", v6);
		json_object_object_add(o, "routes", routes);
		json_object_object_add(o, "dns", json_object_get(get(session, "dns")));
		json_object_object_add(data, "cid", json_object_new_int(1));
		string(data, "apn", str(session, "apn"));
		string(data, "session_id", str(session, "id"));
		json_object_object_add(o, "data", data);
	}
	return o;
}
static int notify(J *session)
{
	J *args = notification(session), *result = ubus("network.interface", "notify_proto", args);
	json_object_put(args);
	bool ok = result != NULL;
	json_object_put(result);
	if (!ok)
		fail("netifd notify_proto failed");
	return ok ? 0 : -1;
}
static int retire(void)
{
	if (!notify(NULL))
		return 0;
	// During ifdown, netifd has already removed protocol configuration and
	// rejects further notify_proto updates. Verify that state before receipt.
	J *state = ubus("network.interface.wan", "status", NULL), *up = get(state, "up");
	bool down =
	    up && json_object_is_type(up, json_type_boolean) && !json_object_get_boolean(up);
	const char *keys[] = {"ipv4-address", "ipv6-address", "route"};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		J *a = get(state, keys[i]);
		if (a && (!json_object_is_type(a, json_type_array) || json_object_array_length(a)))
			down = false;
	}
	json_object_put(state);
	return down ? 0 : -1;
}
static void append(J *dst, J *src)
{
	if (!src || !json_object_is_type(src, json_type_array))
		return;
	for (size_t i = 0; i < json_object_array_length(src); i++)
		json_object_array_add(dst, json_object_get(json_object_array_get_idx(src, i)));
}
static J *network_state(void)
{
	J *v4 = ubus("network.interface.wan", "status", NULL),
	  *v6 = ubus("network.interface.wan6", "status", NULL);
	if (!v4 || !v6) {
		json_object_put(v4);
		json_object_put(v6);
		return NULL;
	}
	J *o = json_object_new_object(), *addresses4 = json_object_new_array(),
	  *addresses6 = json_object_new_array(), *routes = json_object_new_array(),
	  *dns = json_object_new_array();
	J *sources[] = {get(v4, "ipv4-address"), get(v6, "ipv6-address")};
	J *dest[] = {addresses4, addresses6};
	for (int k = 0; k < 2; k++)
		for (size_t i = 0; sources[k] && i < json_object_array_length(sources[k]); i++) {
			const char *a = str(json_object_array_get_idx(sources[k], i), "address");
			if (!a)
				continue;
			if (k) {
				struct in6_addr ip;
				if (inet_pton(AF_INET6, a, &ip) != 1 ||
				    IN6_IS_ADDR_LINKLOCAL(&ip) || IN6_IS_ADDR_UNSPECIFIED(&ip))
					continue;
			}
			json_object_array_add(dest[k], json_object_new_string(a));
		}
	append(routes, get(v4, "route"));
	append(routes, get(v6, "route"));
	append(dns, get(v4, "dns-server"));
	J *more = get(v6, "dns-server");
	for (size_t i = 0; more && i < json_object_array_length(more); i++) {
		J *v = json_object_array_get_idx(more, i);
		bool seen = false;
		for (size_t j = 0; j < json_object_array_length(dns); j++)
			if (json_object_equal(v, json_object_array_get_idx(dns, j)))
				seen = true;
		if (!seen)
			json_object_array_add(dns, json_object_get(v));
	}
	json_object_object_add(o, "ipv4_addresses", addresses4);
	json_object_object_add(o, "ipv6_global_addresses", addresses6);
	json_object_object_add(o, "routes", routes);
	json_object_object_add(o, "dns_servers", dns);
	json_object_object_add(o, "up",
			       json_object_new_boolean(json_object_get_boolean(get(v4, "up"))));
	json_object_put(v4);
	json_object_put(v6);
	return o;
}
static void publish(J *network)
{
	J *o = json_object_new_object();
	string(o, "boot_id", boot);
	json_object_object_add(o, "uptime", json_object_new_double(now()));
	string(o, "interface", "wan");
	string(o, "device", "sipa_eth0");
	string(o, "session_id", *active ? active : NULL);
	json_object_object_add(o, "connect_requested", json_object_new_boolean(requested));
	string(o, "error", *error ? error : NULL);
	if (network)
		json_object_object_add(o, "network", json_object_get(network));
	if (atomic(o))
		fprintf(stderr, "udx-netifd: status write: %s\n", strerror(errno));
	json_object_put(o);
}
int main(int argc, char **argv)
{
	char *end = NULL;
	if (argc != 4 || strcmp(argv[1], "wan") || strcmp(argv[2], "sipa_eth0") || geteuid() != 0)
		return 2;
	long number = strtol(argv[3], &end, 10);
	if (!*argv[3] || *end || number < 0 || number > 65535)
		return 2;
	metric = number;
	FILE *fp = fopen(BOOT, "r");
	if (!fp || !fgets(boot, sizeof(boot), fp))
		return 1;
	fclose(fp);
	boot[strcspn(boot, "\r\n")] = 0;
	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	signal(SIGPIPE, SIG_IGN);
	struct stat previous = {0};
	bool had_broker = false;
	double next_state = 0;
	J *network = NULL;
	while (running) {
		struct stat st;
		bool broker = !lstat(RUN "/control.sock", &st) && S_ISSOCK(st.st_mode);
		bool changed = false;
		if (!broker || !had_broker || st.st_ino != previous.st_ino ||
		    st.st_ctim.tv_sec != previous.st_ctim.tv_sec ||
		    st.st_ctim.tv_nsec != previous.st_ctim.tv_nsec)
			requested = false;
		if (broker)
			previous = st;
		had_broker = broker;
		if (broker && !requested) {
			int rc = request("connect", 10);
			requested = rc == 1;
			if (rc < 0)
				fail("Cellular connect request unavailable");
		}
		J *row = read_json(RUN "/cellular-data.json"), *session = NULL;
		const char *rowboot = str(row, "boot_id");
		if (broker && rowboot && !strcmp(boot, rowboot))
			session = get(row, "session");
		if (session && !session_valid(session)) {
			session = NULL;
			fail("Invalid negotiated data session");
		}
		const char *id = session ? str(session, "id") : "";
		if (strcmp(id, active)) {
			// Retire old netifd configuration before publishing a replacement.
			bool ready = true;
			if (*active) {
				if (retire())
					ready = false;
				else {
					*active = 0;
					changed = true;
				}
			}
			if (ready && session && !notify(session)) {
				snprintf(active, sizeof(active), "%s", id);
				*error = 0;
				changed = true;
			}
			if (ready && !session) {
				*error = 0;
				changed = true;
			}
		}
		json_object_put(row);
		// The controller waits at most eight seconds for this retirement receipt.
		// Publish it before optional status queries, which can each time out.
		if (changed)
			publish(NULL);
		if (changed || now() >= next_state) {
			json_object_put(network);
			network = network_state();
			if (!network)
				fail("netifd status unavailable");
			next_state = now() + 10;
			publish(network);
		}
		struct timespec pause = {1, 0};
		nanosleep(&pause, NULL);
	}
	// netifd owns this process and orders its teardown before protocol restart.
	if (!retire()) {
		*active = 0;
		*error = 0;
	}
	publish(NULL);
	json_object_put(network);
	if (requested && request("disconnect", 2) < 0)
		fprintf(stderr, "udx-netifd: disconnect completion unavailable\n");
	return 0;
}
