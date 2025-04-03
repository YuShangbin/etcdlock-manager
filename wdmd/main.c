/* Copyright 2025 EasyStack, Inc. */
/*
 * Copyright 2011-2012 Red Hat, Inc.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v2 or (at your option) any later version.
 */

#define _POSIX_C_SOURCE 200112L

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <stdint.h>
#include <stddef.h>
#include <grp.h>
#include <fcntl.h>
#include <libgen.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <poll.h>
#include <syslog.h>
#include <dirent.h>
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/signalfd.h>
#include <linux/watchdog.h>
#include <linux/limits.h>
#include <asm-generic/signal-defs.h>
#include <linux/sched.h>

#include "wdmd.h"
#include "wdmd_sock.h"

#ifndef GNUC_UNUSED
#define GNUC_UNUSED __attribute__((__unused__))
#endif


#define DEFAULT_TEST_INTERVAL 10
#define RECOVER_TEST_INTERVAL 1
#define DEFAULT_FIRE_TIMEOUT 60
#define DEFAULT_HIGH_PRIORITY 0

/*
 * If the group name specified here, or specified on the
 * command line is not found, then default to gid 0 (root).
 */
#define SOCKET_GNAME "etcdlock_manager"
#define DEFAULT_SOCKET_GID 0
#define DEFAULT_SOCKET_MODE (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP)

#define WDPATH_SIZE 64
#define WD_ID_SIZE 64

static int standard_test_interval = DEFAULT_TEST_INTERVAL;
static int test_interval= DEFAULT_TEST_INTERVAL;
static int fire_timeout = DEFAULT_FIRE_TIMEOUT;
static int high_priority = DEFAULT_HIGH_PRIORITY;
static int daemon_quit;
static int daemon_debug;
static int try_timeout;
static int forcefire;
static int socket_gid;
static char *socket_gname = (char *)SOCKET_GNAME;
static time_t last_keepalive;
static time_t last_closeunclean;
static char lockfile_path[PATH_MAX];
static int test_loop_enable;
static int dev_fd = -1;
static int shm_fd;
static int itco; /* watchdog_identity is "iTCO_wdt" */
static int daemon_setup_done;

static int allow_scripts;
static int kill_script_sec;
static const char *scripts_dir = "/etc/wdmd.d";
static char watchdog_path[WDPATH_SIZE];
static char option_path[WDPATH_SIZE];
static char saved_path[WDPATH_SIZE];
static char watchdog_identity[WD_ID_SIZE];

struct script_status {
	uint64_t start;
	int pid;
	int last_result;
	unsigned int run_count;
	unsigned int fail_count;
	unsigned int good_count;
	unsigned int kill_count;
	unsigned int long_count;
	char name[PATH_MAX];
};

#define MAX_SCRIPTS 8
static struct script_status scripts[MAX_SCRIPTS];

struct client {
	int used;
	int fd;
	int pid;
	int pid_dead;
	int refcount;
	int internal;
	uint64_t renewal;
	uint64_t expire;
	void *workfn;
	void *deadfn;
	char name[WDMD_NAME_SIZE];
};

#define CLIENT_NALLOC 16
static int client_maxi;
static int client_size = 0;
static struct client *client = NULL;
static struct pollfd *pollfd = NULL;

#define log_debug(fmt, args...) \
do { \
	if (daemon_debug) \
		fprintf(stderr, "%llu %s " fmt "\n", (unsigned long long)time(NULL), time_str(), ##args); \
} while (0)

#define log_error(fmt, args...) \
do { \
	log_debug(fmt, ##args); \
	syslog(LOG_ERR, fmt, ##args); \
} while (0)

#define log_info(fmt, args...) \
do { \
	log_debug(fmt, ##args); \
	syslog(LOG_INFO, fmt, ##args); \
} while (0)

#define log_script(i) \
	log_error("script %.64s last_result %d start %llu run %u fail %u good %u kill %u long %u", \
		  scripts[i].name, scripts[i].last_result, \
		  (unsigned long long)scripts[i].start, \
		  scripts[i].run_count, scripts[i].fail_count, \
		  scripts[i].good_count, scripts[i].kill_count, \
		  scripts[i].long_count);


static uint64_t monotime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec;
}

char time_str_buf[128];

static char *time_str(void)
{
	struct timeval cur_time;
	struct tm time_info;

	gettimeofday(&cur_time, NULL);
	localtime_r(&cur_time.tv_sec, &time_info);
	strftime(time_str_buf, sizeof(time_str_buf), "%Y-%m-%d %H:%M:%S ", &time_info);
	return time_str_buf;
}

/*
 * test clients
 */

static void client_alloc(void)
{
	int i;

	if (!client) {
		client = malloc(CLIENT_NALLOC * sizeof(struct client));
		pollfd = malloc(CLIENT_NALLOC * sizeof(struct pollfd));
	} else {
		client = realloc(client, (client_size + CLIENT_NALLOC) *
				 sizeof(struct client));
		pollfd = realloc(pollfd, (client_size + CLIENT_NALLOC) *
				 sizeof(struct pollfd));
		if (!pollfd)
			log_error("can't alloc for pollfd");
	}
	if (!client || !pollfd)
		log_error("can't alloc for client array");

	for (i = client_size; i < client_size + CLIENT_NALLOC; i++) {
		memset(&client[i], 0, sizeof(struct client));
		client[i].fd = -1;
		pollfd[i].fd = -1;
		pollfd[i].revents = 0;
	}
	client_size += CLIENT_NALLOC;
}

static int client_add(int fd, void (*workfn)(int ci), void (*deadfn)(int ci))
{
	int i;

	if (!client)
		client_alloc();
 again:
	for (i = 0; i < client_size; i++) {
		if (!client[i].used) {
			client[i].used = 1;
			client[i].workfn = workfn;
			client[i].deadfn = deadfn;
			client[i].fd = fd;
			pollfd[i].fd = fd;
			pollfd[i].events = POLLIN;
			if (i > client_maxi)
				client_maxi = i;
			return i;
		}
	}

	client_alloc();
	goto again;
}

static void client_pid_dead(int ci)
{
	if (!client[ci].expire) {
		log_debug("client_pid_dead ci %d", ci);

		close(client[ci].fd);

		/* refcount automatically dropped if a client with
		   no expiration is closed */

		client[ci].used = 0;
		memset(&client[ci], 0, sizeof(struct client));

		client[ci].fd = -1;
		pollfd[ci].fd = -1;
		pollfd[ci].events = 0;
	} else {
		/*
		 * Leave used and expire set so that test_clients will continue
		 * monitoring this client and expire if necessary.
		 *
		 * Leave refcount set so that the daemon will not cleanly shut
		 * down if it gets a sigterm.
		 *
		 * This case of a client con with an expire time being closed
		 * is a fatal condition; there's no way to clear or extend the
		 * expire time and no way to cleanly shut down the daemon.
		 * This should never happen.
		 *
		 * (We don't enforce that a client with an expire time also has refcount
		 * set, but I can't think of case where setting expire but not refcount
		 * would be useful.)
		 */

		log_error("client dead ci %d fd %d pid %d renewal %llu expire %llu %s",
			  ci, client[ci].fd, client[ci].pid,
			  (unsigned long long)client[ci].renewal,
			  (unsigned long long)client[ci].expire,
			  client[ci].name);

		close(client[ci].fd);

		client[ci].pid_dead = 1;

		client[ci].fd = -1;
		pollfd[ci].fd = -1;
		pollfd[ci].events = 0;
	}
}

static int get_peer_pid(int fd, int *pid)
{
	struct ucred cred;
	unsigned int cl = sizeof(cred);

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) != 0)
		return -1;

	*pid = cred.pid;
	return 0;
}

#define DEBUG_SIZE (1024 * 1024)
#define LINE_SIZE 256

char debug_buf[DEBUG_SIZE];

static void dump_debug(int fd)
{
	char line[LINE_SIZE];
	uint64_t now;
	int line_len;
	int debug_len = 0;
	int i;

	memset(debug_buf, 0, DEBUG_SIZE);

	now = monotime();

	memset(line, 0, sizeof(line));
	snprintf(line, 255, "wdmd %d socket_gid %d high_priority %d now %llu last_keepalive %llu last_closeunclean %llu allow_scripts %d kill_script_sec %d fire_timeout %d identity \"%s\"\n",
		 getpid(), socket_gid, high_priority,
		 (unsigned long long)now,
		 (unsigned long long)last_keepalive,
		 (unsigned long long)last_closeunclean,
		 allow_scripts, kill_script_sec, fire_timeout,
		 watchdog_identity);

	line_len = strlen(line);
	strncat(debug_buf, line, LINE_SIZE);
	debug_len += line_len;

	for (i = 0; i < MAX_SCRIPTS; i++) {
		if (!scripts[i].name[0])
			continue;
		memset(line, 0, sizeof(line));
		snprintf(line, 255, "script %d name %.64s pid %d now %llu start %llu last_result %d run %u fail %u good %u kill %u long %u\n",
			 i, scripts[i].name, scripts[i].pid,
			 (unsigned long long)now,
			 (unsigned long long)scripts[i].start,
			 scripts[i].last_result,
			 scripts[i].run_count,
			 scripts[i].fail_count,
			 scripts[i].good_count,
			 scripts[i].kill_count,
			 scripts[i].long_count);

		line_len = strlen(line);

		if (debug_len + line_len >= DEBUG_SIZE - 1)
			goto out;

		strncat(debug_buf, line, LINE_SIZE);
		debug_len += line_len;
	}

	for (i = 0; i < client_size; i++) {
		if (!client[i].used)
			continue;
		memset(line, 0, sizeof(line));
		snprintf(line, 255, "client %d name %.64s pid %d fd %d dead %d ref %d now %llu renewal %llu expire %llu\n",
			 i, client[i].name, client[i].pid, client[i].fd, client[i].pid_dead, client[i].refcount,
			 (unsigned long long)now,
			 (unsigned long long)client[i].renewal,
			 (unsigned long long)client[i].expire);

		line_len = strlen(line);

		if (debug_len + line_len >= DEBUG_SIZE - 1)
			goto out;

		strncat(debug_buf, line, LINE_SIZE);
		debug_len += line_len;
	}
 out:
	send(fd, debug_buf, debug_len, MSG_NOSIGNAL);
}

static void _init_test_interval(void)
{
	if (fire_timeout >= 60) {
		standard_test_interval = 10;
		test_interval = 10;
	} else if (fire_timeout >= 30 && fire_timeout < 60) {
		standard_test_interval = 5;
		test_interval = 5;
	} else if (fire_timeout >= 10 && fire_timeout < 30) {
		standard_test_interval = 2;
		test_interval = 2;
	} else {
		standard_test_interval = 1;
		test_interval = 1;
	}
}

static int open_dev(void)
{
	int fd;

	if (dev_fd != -1) {
		log_error("watchdog already open fd %d", dev_fd);
		return -1;
	}

	fd = open(watchdog_path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		log_error("open %s error %d", watchdog_path, errno);
		return fd;
	}

	dev_fd = fd;
	return 0;
}

static void close_watchdog(void)
{
	int rv;

	if (dev_fd == -1) {
		log_debug("close_watchdog already closed");
		return;
	}

	rv = write(dev_fd, "V", 1);
	if (rv < 0)
		log_error("%s disarm write error %d", watchdog_path, errno);
	else if (daemon_setup_done)
		log_info("%s disarmed", watchdog_path);

	close(dev_fd);
	dev_fd = -1;
}

static void close_watchdog_unclean(void)
{
	if (dev_fd == -1)
		return;

	log_error("%s closed unclean", watchdog_path);
	close(dev_fd);
	dev_fd = -1;

	last_closeunclean = monotime();
}

static void pet_watchdog(void)
{
	int rv, unused;

	rv = ioctl(dev_fd, WDIOC_KEEPALIVE, &unused);

	last_keepalive = monotime();
	log_debug("keepalive %d", rv);
}

static int _open_watchdog_itco(struct wdmd_header *h)
{
	int get_timeout_itco, get_timeout_real, set_timeout_itco, set_timeout_real;
	int rv;

	/* Don't check dev_fd for -1 because dev_fd will be closed
	   and set to -1 prior to timeout in close_watchdog_unclean().  */

	if (test_loop_enable)
		return 0;

	if (!h->fire_timeout)
		return -1;

	rv = open_dev();
	if (rv < 0)
		return -1;

	get_timeout_real = 0;
	get_timeout_itco = 0;

	rv = ioctl(dev_fd, WDIOC_GETTIMEOUT, &get_timeout_itco);
	if (rv < 0) {
		log_error("open_watchdog gettimeout error %d", errno);
		close_watchdog();
		return -1;
	}

	get_timeout_real = get_timeout_itco * 2;

	if (get_timeout_real == h->fire_timeout) {
		/* success, requested value matches the default value */
		fire_timeout = get_timeout_real;
		_init_test_interval();
		log_error("%s open with timeout %d", watchdog_path, get_timeout_real);
		pet_watchdog();
		test_loop_enable = 1;
		return 0;
	}

	set_timeout_real = h->fire_timeout;
	set_timeout_itco = set_timeout_real / 2;

	rv = ioctl(dev_fd, WDIOC_SETTIMEOUT, &set_timeout_itco);
	if (rv < 0) {
		log_error("open_watchdog settimeout %d error %d", set_timeout_real, errno);
		close_watchdog();
		return -1;
	}

	get_timeout_real = 0;
	get_timeout_itco = 0;

	rv = ioctl(dev_fd, WDIOC_GETTIMEOUT, &get_timeout_itco);
	if (rv < 0) {
		log_error("open_watchdog gettimeout check error %d", errno);
		close_watchdog();
		return -1;
	}

	get_timeout_real = get_timeout_itco * 2;

	if (get_timeout_real == set_timeout_real) {
		/* success setting a custom timeout */
		fire_timeout = get_timeout_real;
		_init_test_interval();
		log_error("%s open with timeout %d", watchdog_path, get_timeout_real);
		pet_watchdog();
		test_loop_enable = 1;
		return 0;
	}
	
	/* failed to set a custom timeout */
	log_error("open_watchdog gettimeout value real %d itco %d expect real %d",
		  get_timeout_real, get_timeout_itco, set_timeout_real);
	close_watchdog();
	return -1;
}

static int _open_watchdog(struct wdmd_header *h)
{
	int get_timeout, set_timeout;
	int rv;

	if (itco)
		return _open_watchdog_itco(h);

	/* Don't check dev_fd for -1 because dev_fd will be closed
	   and set to -1 prior to timeout in close_watchdog_unclean().  */

	if (test_loop_enable)
		return 0;

	if (!h->fire_timeout)
		return -1;

	rv = open_dev();
	if (rv < 0)
		return -1;

	get_timeout = 0;

	rv = ioctl(dev_fd, WDIOC_GETTIMEOUT, &get_timeout);
	if (rv < 0) {
		log_error("open_watchdog gettimeout error %d", errno);
		close_watchdog();
		return -1;
	}

	if (get_timeout == h->fire_timeout) {
		/* success, requested value matches the default value */
		fire_timeout = get_timeout;
		_init_test_interval();
		log_error("%s open with timeout %d", watchdog_path, get_timeout);
		pet_watchdog();
		test_loop_enable = 1;
		return 0;
	}

	set_timeout = h->fire_timeout;

	rv = ioctl(dev_fd, WDIOC_SETTIMEOUT, &set_timeout);
	if (rv < 0) {
		log_error("open_watchdog settimeout %d error %d", set_timeout, errno);
		close_watchdog();
		return -1;
	}

	get_timeout = 0;

	rv = ioctl(dev_fd, WDIOC_GETTIMEOUT, &get_timeout);
	if (rv < 0) {
		log_error("open_watchdog gettimeout check error %d", errno);
		close_watchdog();
		return -1;
	}

	if (get_timeout == set_timeout) {
		/* success setting a custom timeout */
		fire_timeout = get_timeout;
		_init_test_interval();
		log_error("%s open with timeout %d", watchdog_path, get_timeout);
		pet_watchdog();
		test_loop_enable = 1;
		return 0;
	}
	
	/* failed to set a custom timeout */
	log_error("open_watchdog gettimeout value %d set %d",
		  get_timeout, set_timeout);
	close_watchdog();
	return -1;
}

static void process_connection(int ci)
{
	struct wdmd_header h;
	struct wdmd_header h_ret;
	void (*deadfn)(int ci);
	int rv, pid;

	memset(&h, 0, sizeof(h));

	rv = recv(client[ci].fd, &h, sizeof(h), MSG_WAITALL);
	if (!rv)
		return;
	if (rv < 0) {
		log_error("ci %d recv error %d", ci, errno);
		goto dead;
	}
	if (rv != sizeof(h)) {
		log_error("ci %d recv size %d", ci, rv);
		goto dead;
	}

	switch(h.cmd) {
	case CMD_REGISTER:
		/* TODO: allow client to reconnect, search clients for h.name
		   and copy the renewal and expire times, then clear the
		   old client entry */

		rv = get_peer_pid(client[ci].fd, &pid);
		if (rv < 0)
			goto dead;
		client[ci].pid = pid;
		memcpy(client[ci].name, h.name, WDMD_NAME_SIZE);
		log_debug("register ci %d fd %d pid %d %s", ci, client[ci].fd,
			  pid, client[ci].name);
		break;

	case CMD_REFCOUNT_SET:
		client[ci].refcount = 1;
		break;

	case CMD_REFCOUNT_CLEAR:
		client[ci].refcount = 0;
		break;

	case CMD_OPEN_WATCHDOG:
		memcpy(&h_ret, &h, sizeof(h));
		rv = _open_watchdog(&h);
		if (rv < 0)
			h_ret.fire_timeout = 0;
		else
			h_ret.fire_timeout = fire_timeout;
		log_debug("open_watchdog fire_timeout %u result %u", h.fire_timeout, fire_timeout);
		send(client[ci].fd, &h_ret, sizeof(h_ret), MSG_NOSIGNAL);
		break;

	case CMD_TEST_LIVE:
		client[ci].renewal = h.renewal_time;
		client[ci].expire = h.expire_time;
		log_debug("test_live ci %d renewal %llu expire %llu", ci,
			  (unsigned long long)client[ci].renewal,
			  (unsigned long long)client[ci].expire);
		break;

	case CMD_STATUS:
		memcpy(&h_ret, &h, sizeof(h));
		h_ret.test_interval = test_interval;
		h_ret.fire_timeout = fire_timeout;
		h_ret.last_keepalive = last_keepalive;
		send(client[ci].fd, &h_ret, sizeof(h_ret), MSG_NOSIGNAL);
		break;

	case CMD_DUMP_DEBUG:
		strncpy(client[ci].name, "dump", WDMD_NAME_SIZE);
		dump_debug(client[ci].fd);
		break;
	};

	return;

 dead:
	deadfn = client[ci].deadfn;
	if (deadfn)
		deadfn(ci);
}

static void process_listener(int ci)
{
	int fd;
	int on = 1;

	fd = accept(client[ci].fd, NULL, NULL);
	if (fd < 0)
		return;

	setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));

	client_add(fd, process_connection, client_pid_dead);
}

static void close_clients(void)
{
}

static int setup_listener_socket(int *listener_socket)
{
	int rv, s;
	struct sockaddr_un addr;

	s = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (s < 0)
		return -errno;

	rv = wdmd_socket_address(&addr);
	if (rv < 0)
		return rv;

	unlink(addr.sun_path);
	rv = bind(s, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
	if (rv < 0) {
		rv = -errno;
		close(s);
		return rv;
	}

	rv = listen(s, 5);
	if (rv < 0) {
		rv = -errno;
		close(s);
		return rv;
	}

	rv = chmod(addr.sun_path, DEFAULT_SOCKET_MODE);
	if (rv < 0) {
		rv = -errno;
		close(s);
		return rv;
	}

	rv = chown(addr.sun_path, -1, socket_gid);
	if (rv < 0) {
		rv = -errno;
		close(s);
		return rv;
	}

	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);

	*listener_socket = s;
	return 0;
}

static int setup_clients(void)
{
	int rv, fd = -1, ci;

	rv = setup_listener_socket(&fd);
	if (rv < 0)
		return rv;

	ci = client_add(fd, process_listener, client_pid_dead);
	strncpy(client[ci].name, "listen", WDMD_NAME_SIZE);
	client[ci].internal = 1;
	return 0;
}

static int test_clients(void)
{
	uint64_t t;
	time_t last_ping;
	int fail_count = 0;
	int i;

	t = monotime();

	for (i = 0; i < client_size; i++) {
		if (!client[i].used)
		       continue;
		if (!client[i].expire)
			continue;

		if (last_keepalive > last_closeunclean)
			last_ping = last_keepalive;
		else
			last_ping = last_closeunclean;

		if (t >= client[i].expire) {
			log_error("test failed rem %d now %llu ping %llu close %llu renewal %llu expire %llu client %d %s",
				  fire_timeout - (int)(t - last_ping),
				  (unsigned long long)t,
				  (unsigned long long)last_keepalive,
				  (unsigned long long)last_closeunclean,
				  (unsigned long long)client[i].renewal,
				  (unsigned long long)client[i].expire,
				  client[i].pid, client[i].name);
			fail_count++;
			continue;
		}

		/*
		 * If we can patch the kernel to avoid a close-ping,
		 * then we can remove this early/preemptive fail/close
		 * of the device, but instead just not pet the device
		 * when the expiration time is reached.  Also see
		 * close_watchdog_unclean() below.
		 *
		 * We do this fail/close (which generates a ping)
		 * TEST_INTERVAL before the expire time because we want
		 * the device to fire at most 60 seconds after the
		 * expiration time.  That means we need the last ping
		 * (from close) to be TEST_INTERVAL before to the
		 * expiration time.
		 *
		 * If we did the close at/after the expiration time,
		 * then the ping from the close would mean that the
		 * device would fire between 60 and 70 seconds after the
		 * expiration time.
		 */

		if (t >= client[i].expire - standard_test_interval) {
			log_error("test warning now %llu ping %llu close %llu renewal %llu expire %llu client %d %s",
				  (unsigned long long)t,
				  (unsigned long long)last_keepalive,
				  (unsigned long long)last_closeunclean,
				  (unsigned long long)client[i].renewal,
				  (unsigned long long)client[i].expire,
				  client[i].pid, client[i].name);
			fail_count++;
			continue;
		}
	}

	return fail_count;
}

static int active_clients(void)
{
	int i;

	for (i = 0; i < client_size; i++) {
		if (client[i].refcount)
			return 1;
	}
	return 0;
}

static void count_clients(int *active, int *external)
{
	int act = 0, ext = 0;
	int i;

	for (i = 0; i < client_size; i++) {
		if (!client[i].used)
			continue;
		if (client[i].refcount)
			act++;
		if (!client[i].internal)
			ext++;
	}
	if (active)
		*active = act;
	if (external)
		*external = ext;
}


#ifdef TEST_FILES
#define FILES_DIR "/run/wdmd/test_files"
const char *files_built = " files";
static DIR *files_dir;

static void close_files(void)
{
	closedir(files_dir);
}

static int setup_files(void)
{
	mode_t old_umask;
	int rv;

	old_umask = umask(0022);
	rv = mkdir(FILES_DIR, 0777);
	if (rv < 0 && errno != EEXIST)
		goto out;

	files_dir = opendir(FILES_DIR);
	if (!files_dir)
		rv = -errno;
	else
		rv = 0;
 out:
	umask(old_umask);
	return rv;
}

static int read_file(char *name, uint64_t *renewal, uint64_t *expire)
{
	FILE *file;
	char path[PATH_MAX];

	snprintf(path, PATH_MAX-1, "%s/%s", FILES_DIR, name);

	file = fopen(path, "r");
	if (!file)
		return -1;

	fscanf(file, "renewal %llu expire %llu", renewal, expire);

	fclose(file);
	return 0;
}

static int test_files(void)
{
	struct dirent *de;
	uint64_t t, renewal, expire;
	int fail_count = 0;
	int rv;

	while ((de = readdir(files_dir))) {
		if (de->d_name[0] == '.')
			continue;

		rv = read_file(de->d_name, &renewal, &expire);
		if (rv < 0)
			continue;

		t = monotime();

		if (t >= expire) {
			log_error("test failed file %s renewal %llu expire %llu ",
				  de->d_name,
				  (unsigned long long)renewal,
				  (unsigned long long)expire);
			fail_count++;
		}
	}

	return fail_count;
}

#else

static void close_files(void) { }
static int setup_files(void) { return 0; }
static int test_files(void) { return 0; }

#endif /* TEST_FILES */

static int find_script(char *name)
{
	int i;

	for (i = 0; i < MAX_SCRIPTS; i++) {
		if (!strncmp(scripts[i].name, name, PATH_MAX))
			return i;
	}
	return -1;
}

static int add_script(char *name)
{
	int i;

	for (i = 0; i < MAX_SCRIPTS; i++) {
		if (scripts[i].name[0])
			continue;

		log_debug("add_script %d %s", i, name);
		strncpy(scripts[i].name, name, PATH_MAX);
		return i;
	}
	log_debug("script %s no space", name);
	return -1;
}

static int check_path(char *path)
{
	struct stat st;
	int rv;

	rv = stat(path, &st);

	if (rv < 0)
		return -errno;

	if (!(S_ISREG(st.st_mode)))
		return -1;

	if (!(st.st_mode & S_IXUSR))
		return -1;

	return 0;
}

static int run_script(int i)
{
	char path[PATH_MAX];
	int pid, rv;

	memset(path, 0, sizeof(path));
	snprintf(path, PATH_MAX-1, "%s/%s", scripts_dir, scripts[i].name);

	rv = check_path(path);
	if (rv < 0)
		return rv;

	pid = fork();
	if (pid < 0)
		return -errno;

	if (pid) {
		log_debug("script %s pid %d", scripts[i].name, pid);
		return pid;
	} else {
		execlp(path, path, NULL);
		exit(EXIT_FAILURE);
	}
}

static void close_scripts(void)
{
}

static int setup_scripts(void)
{
	char path[PATH_MAX];
	struct dirent **namelist;
	int i, s, rv, de_count;

	if (!allow_scripts)
		return 0;

	de_count = scandir(scripts_dir, &namelist, 0, alphasort);
	if (de_count < 0)
		return 0;

	for (i = 0; i < de_count; i++) {
		if (namelist[i]->d_name[0] == '.')
			goto next;

		memset(path, 0, sizeof(path));
		snprintf(path, PATH_MAX-1, "%s/%s", scripts_dir, namelist[i]->d_name);

		rv = check_path(path);
		if (rv < 0) {
			log_debug("script %s ignore %d", namelist[i]->d_name, rv);
			goto next;
		}

		s = find_script(namelist[i]->d_name);
		if (s < 0)
			add_script(namelist[i]->d_name);
 next:
		free(namelist[i]);
	}
	free(namelist);

	return 0;
}

static int test_scripts(void)
{
	int i, rv, pid, result, running, fail_count, status;
	uint64_t begin, now;

	if (!allow_scripts)
		return 0;

	fail_count = 0;

	begin = monotime();

	for (i = 0; i < MAX_SCRIPTS; i++) {
		if (!scripts[i].name[0])
			continue;

		/* pid didn't exit in previous cycle */
		if (scripts[i].pid)
			continue;

		/*
		 * after a script reports success, don't call it again before
		 * the normal test interval; this is needed because the test
		 * interval becomes shorter when failures occur
		 */

		if (!scripts[i].last_result &&
		    ((begin - scripts[i].start) < (standard_test_interval - 1)))
			continue;

		pid = run_script(i);

		if (pid <= 0) {
			log_error("script %s removed %d", scripts[i].name, pid);
			memset(&scripts[i], 0, sizeof(struct script_status));
		} else {
			scripts[i].pid = pid;
			scripts[i].start = begin;
			scripts[i].run_count++;
		}
	}

	/* wait up to standard_test_interval-1 for the pids to finish */

	while (1) {
		running = 0;

		for (i = 0; i < MAX_SCRIPTS; i++) {
			if (!scripts[i].name[0])
				continue;

			if (!scripts[i].pid)
				continue;

			rv = waitpid(scripts[i].pid, &status, WNOHANG);

			if (rv < 0) {
				/* shouldn't happen */
				log_error("script %s pid %d waitpid error %d %d",
					  scripts[i].name, scripts[i].pid, rv, errno);
				log_script(i);
				running++;

			} else if (!rv) {
				/* pid still running, has not changed state */
				running++;

			} else if (rv == scripts[i].pid) {
				/* pid state has changed */

				if (WIFEXITED(status)) {
					/* pid exited with an exit code */
					result = WEXITSTATUS(status);

					if (result) {
						log_error("script %s pid %d exit status %d",
							  scripts[i].name, scripts[i].pid,
							  result);

						scripts[i].fail_count++;
						scripts[i].last_result = result;
						scripts[i].pid = 0;
						fail_count++;

						log_script(i);
					} else {
						scripts[i].good_count++;
						scripts[i].last_result = 0;
						scripts[i].pid = 0;
					}

				} else if (WIFSIGNALED(status)) {
					/* pid terminated due to a signal */

					log_error("script %s pid %d term signal %d",
						  scripts[i].name, scripts[i].pid,
						  WTERMSIG(status));

					scripts[i].kill_count++;
					scripts[i].last_result = EINTR;
					scripts[i].pid = 0;
					fail_count++;

					log_script(i);
				} else {
					/* pid state changed but still running */
					running++;
				}

			} else {
				/* shouldn't happen */
				log_error("script %s pid %d waitpid rv %d",
					  scripts[i].name, scripts[i].pid, rv);
				log_script(i);

				running++;
			}

			/* option to kill script after it's run for kill_script_sec */

			if (scripts[i].pid && kill_script_sec &&
			    (monotime() - scripts[i].start >= kill_script_sec)) {
				kill(scripts[i].pid, SIGKILL);
			}
		}

		if (!running)
			break;

		if (monotime() - begin >= standard_test_interval - 1)
			break;

		sleep(1);
	}

	if (!running)
		goto out;

	/* any pids that have not exited count as a failed for this cycle */

	now = monotime();

	for (i = 0; i < MAX_SCRIPTS; i++) {
		if (!scripts[i].name[0])
			continue;

		if (!scripts[i].pid)
			continue;

		scripts[i].long_count++;
		fail_count++;

		log_error("script %s pid %d start %llu now %llu taking too long",
			  scripts[i].name, scripts[i].pid,
			  (unsigned long long)scripts[i].start,
			  (unsigned long long)now);
		log_script(i);
	}

 out:
	return fail_count;
}

static int setup_identity(char *wdpath)
{
	char sysfs_path[PATH_MAX] = { 0 };
	char *base, *p;
	int fd, rv;

	/*
	 * This function will be called multiple times when probing
	 * different watchdog paths for one that works.
	 */
	itco = 0;
	memset(watchdog_identity, 0, sizeof(watchdog_identity));

	/*
	 * $ cat /sys/class/watchdog/watchdog0/identity
	 * iTCO_wdt
	 */
	if (!(base = basename(wdpath)))
		return -1;

	snprintf(sysfs_path, PATH_MAX-1, "/sys/class/watchdog/%s/identity", base);

	if ((fd = open(sysfs_path, O_RDONLY)) < 0)
		return -1;

	rv = read(fd, watchdog_identity, WD_ID_SIZE-1);

	close(fd);

	if (rv <= 0)
		return -1;

	if ((p = strchr(watchdog_identity, '\n')))
		*p = '\0';

	log_debug("%s %s %s", wdpath, sysfs_path, watchdog_identity);

	if (!strcmp(watchdog_identity, "iTCO_wdt"))
		itco = 1;

	return 0;
}

static int _setup_watchdog(char *path)
{
	struct stat buf;
	int rv, timeout;

	strncpy(watchdog_path, path, WDPATH_SIZE);
	watchdog_path[WDPATH_SIZE - 1] = '\0';

	rv = stat(watchdog_path, &buf);
	if (rv < 0)
		return -1;

	rv = open_dev();
	if (rv < 0)
		return -1;

	timeout = 0;

	rv = ioctl(dev_fd, WDIOC_GETTIMEOUT, &timeout);
	if (rv < 0) {
		log_error("%s failed to report timeout", watchdog_path);
		close_watchdog();
		return -1;
	}

	log_debug("%s gettimeout reported %u", watchdog_path, timeout);

	close_watchdog();

	/* TODO: save watchdog_path in /run/wdmd/saved_path,
	 * and in startup read that file, copying it to saved_path */

	return 0;
}

/*
 * Success: returns 0 with watchdog_path set.
 * Order of preference:
 * . saved path (path used before daemon restart)
 * . command line option (-w)
 * . /dev/watchdog0
 * . /dev/watchdog1
 * . /dev/watchdog
 */

static int setup_watchdog(void)
{
	int rv;

	if (!saved_path[0])
		goto opt;

	rv = _setup_watchdog(saved_path);
	if (!rv)
		return 0;

 opt:
	if (!option_path[0] || !strcmp(saved_path, option_path))
		goto zero;

	rv = _setup_watchdog(option_path);
	if (!rv)
		return 0;

 zero:
	if (!strcmp(saved_path, "/dev/watchdog0") ||
	    !strcmp(option_path, "/dev/watchdog0"))
		goto one;

	rv = _setup_watchdog((char *)"/dev/watchdog0");
	if (!rv)
		return 0;

 one:
	if (!strcmp(saved_path, "/dev/watchdog1") ||
	    !strcmp(option_path, "/dev/watchdog1"))
		goto old;

	rv = _setup_watchdog((char *)"/dev/watchdog1");
	if (!rv)
		return 0;

 old:
	if (!strcmp(saved_path, "/dev/watchdog") ||
	    !strcmp(option_path, "/dev/watchdog"))
		goto out;

	rv = _setup_watchdog((char *)"/dev/watchdog");
	if (!rv)
		return 0;

 out:
	log_error("no watchdog device, load a watchdog driver");
	return -1;

}

/*
 * iTCO_wdt actual firing timeout is double the value used in get/set!
 * https://bugzilla.kernel.org/show_bug.cgi?id=213809
 */
static int _try_timeout_itco(const char *path)
{
	struct stat buf;
	int try_timeout_real, try_timeout_itco, get_timeout_real, get_timeout_itco, set_timeout_real, set_timeout_itco;
	int unused, fd, err, rv, rv2;

	rv = stat(path, &buf);
	if (rv < 0) {
		fprintf(stderr, "%s stat error %d\n", path, errno);
		return -1;
	}

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "%s open error %d\n", path, errno);
		return fd;
	}

	printf("%s %s open fd %d\n", time_str(), path, fd);

	get_timeout_real = 0;
	get_timeout_itco = 0;

	rv = ioctl(fd, WDIOC_GETTIMEOUT, &get_timeout_itco);
	if (rv < 0) {
		fprintf(stderr, "%s gettimeout error %d\n", path, errno);
		rv = -1;
		goto out;
	}

	get_timeout_real = get_timeout_itco * 2;

	printf("%s %s gettimeout real %d itco %d\n", time_str(), path, get_timeout_real, get_timeout_itco);

	if (get_timeout_real == try_timeout)
		goto keepalive;

	try_timeout_real = try_timeout;
	try_timeout_itco = try_timeout_real / 2;
	set_timeout_real = try_timeout;
	set_timeout_itco = set_timeout_real / 2;

	rv = ioctl(fd, WDIOC_SETTIMEOUT, &set_timeout_itco);
	if (rv < 0) {
		fprintf(stderr, "%s settimeout real %d itco %d error %d\n", path, set_timeout_real, set_timeout_itco, errno);
		rv = -1;
		goto out;
	}

	set_timeout_real = set_timeout_itco * 2;

	printf("%s %s settimeout real %d itco %d result real %d itco %d\n", time_str(), path,
	       try_timeout_real, try_timeout_itco, set_timeout_real, set_timeout_itco);

	if (set_timeout_itco != try_timeout_itco) {
		fprintf(stderr, "%s settimeout real %d itco %d failed\n", path, try_timeout_real, try_timeout_itco);
		rv = -1;
		goto out;
	}

	get_timeout_real = 0;
	get_timeout_itco = 0;

	rv = ioctl(fd, WDIOC_GETTIMEOUT, &get_timeout_itco);
	if (rv < 0) {
		fprintf(stderr, "%s gettimeout error %d\n", path, errno);
		rv = -1;
		goto out;
	}

	get_timeout_real = get_timeout_itco * 2;

	printf("%s %s gettimeout real %d itco %d\n", time_str(), path, get_timeout_real, get_timeout_itco);

 keepalive:

	rv = ioctl(fd, WDIOC_KEEPALIVE, &unused);
	if (rv < 0) {
		fprintf(stderr, "%s keepalive error %d\n", path, errno);
		rv = -1;
		goto out;
	}

	printf("%s %s keepalive fd %d result %d\n", time_str(), path, fd, rv);

	if (forcefire) {
		int sleep_sec = 0;
		int i;
		setbuf(stdout, NULL);
		printf("%s waiting for watchdog to reset machine:\n", time_str());
		for (i = 1; i < get_timeout_real + 5; i++) {
			sleep(1);
			sleep_sec++;
			if (sleep_sec >= get_timeout_real+1)
				printf("%s %d %s failed to fire after timeout %d seconds\n", time_str(), i, path, get_timeout_real);
			else
				printf("%s %d\n", time_str(), i);
		}
	}

	rv = 0;
 out:
	err = write(fd, "V", 1);
	if (err < 0) {
		fprintf(stderr, "trytimeout failed to disarm %s error %d %d\n", path, err, errno);
		openlog("wdmd", LOG_CONS | LOG_PID, LOG_DAEMON);
		syslog(LOG_ERR, "trytimeout failed to disarm %s error %d %d\n", path, err, errno);
	}

	printf("%s %s disarm write V fd %d result %d\n", time_str(), path, fd, rv);

	rv2 = close(fd);

	printf("%s %s close fd %d result %d\n", time_str(), path, fd, rv2);

	return rv;
}

static int _try_timeout(const char *path)
{
	struct stat buf;
	int get_timeout, set_timeout;
	int unused, fd, err, rv, rv2;

	rv = stat(path, &buf);
	if (rv < 0) {
		fprintf(stderr, "%s stat error %d\n", path, errno);
		return -1;
	}

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "%s open error %d\n", path, errno);
		return fd;
	}

	printf("%s %s open fd %d\n", time_str(), path, fd);

	get_timeout = 0;

	rv = ioctl(fd, WDIOC_GETTIMEOUT, &get_timeout);
	if (rv < 0) {
		fprintf(stderr, "%s gettimeout error %d\n", path, errno);
		rv = -1;
		goto out;
	}

	printf("%s %s gettimeout %d\n", time_str(), path, get_timeout);

	if (get_timeout == try_timeout)
		goto keepalive;

	set_timeout = try_timeout;

	rv = ioctl(fd, WDIOC_SETTIMEOUT, &set_timeout);
	if (rv < 0) {
		fprintf(stderr, "%s settimeout %d error %d\n", path, set_timeout, errno);
		rv = -1;
		goto out;
	}

	printf("%s %s settimeout %d result %d\n", time_str(), path, try_timeout, set_timeout);

	if (set_timeout != try_timeout) {
		fprintf(stderr, "%s settimeout %d failed\n", path, try_timeout);
		rv = -1;
		goto out;
	}

	get_timeout = 0;

	rv = ioctl(fd, WDIOC_GETTIMEOUT, &get_timeout);
	if (rv < 0) {
		fprintf(stderr, "%s gettimeout error %d\n", path, errno);
		rv = -1;
		goto out;
	}

	printf("%s %s gettimeout %d\n", time_str(), path, get_timeout);

 keepalive:

	rv = ioctl(fd, WDIOC_KEEPALIVE, &unused);
	if (rv < 0) {
		fprintf(stderr, "%s keepalive error %d\n", path, errno);
		rv = -1;
		goto out;
	}

	printf("%s %s keepalive fd %d result %d\n", time_str(), path, fd, rv);

	if (forcefire) {
		int sleep_sec = 0;
		int i;
		setbuf(stdout, NULL);
		printf("%s waiting for watchdog to reset machine:\n", time_str());
		for (i = 1; i < get_timeout + 5; i++) {
			sleep(1);
			sleep_sec++;
			if (sleep_sec >= get_timeout+1)
				printf("%s %d %s failed to fire after timeout %d seconds\n", time_str(), i, path, get_timeout);
			else
				printf("%s %d\n", time_str(), i);
		}
	}

	rv = 0;
 out:
	err = write(fd, "V", 1);
	if (err < 0) {
		fprintf(stderr, "trytimeout failed to disarm %s error %d %d\n", path, err, errno);
		openlog("wdmd", LOG_CONS | LOG_PID, LOG_DAEMON);
		syslog(LOG_ERR, "trytimeout failed to disarm %s error %d %d\n", path, err, errno);
	}

	printf("%s %s disarm write V fd %d result %d\n", time_str(), path, fd, rv);

	rv2 = close(fd);

	printf("%s %s close fd %d result %d\n", time_str(), path, fd, rv2);

	return rv;
}

static int _probe_dev_itco(const char *path)
{
	struct stat buf;
	int fd, err, rv, timeout_real, timeout_itco;

	rv = stat(path, &buf);
	if (rv < 0) {
		fprintf(stderr, "error %d stat %s\n", errno, path);
		return -1;
	}

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "error %d open %s\n", errno, path);
		return fd;
	}

	timeout_real = 0;
	timeout_itco = 0;

	rv = ioctl(fd, WDIOC_GETTIMEOUT, &timeout_itco);
	if (rv < 0) {
		fprintf(stderr, "error %d ioctl gettimeout %s\n", errno, path);
		rv = -1;
		goto out;
	}

	timeout_real = timeout_itco * 2;

	if (timeout_real == fire_timeout) {
		printf("%s\n", path);
		rv = 0;
		goto out;
	}

	timeout_real = fire_timeout;
	timeout_itco = timeout_real / 2;

	rv = ioctl(fd, WDIOC_SETTIMEOUT, &timeout_itco);
	if (rv < 0) {
		fprintf(stderr, "error %d ioctl settimeout %s\n", errno, path);
		rv = -1;
		goto out;
	}

	timeout_real = timeout_itco * 2;

	if (timeout_real != fire_timeout) {
		fprintf(stderr, "error %d invalid timeout %s\n", errno, path);
		rv = -1;
		goto out;
	}

	printf("%s\n", path);
	rv = 0;

 out:
	err = write(fd, "V", 1);
	if (err < 0) {
		fprintf(stderr, "probe failed to disarm %s error %d %d\n", path, err, errno);
		openlog("wdmd", LOG_CONS | LOG_PID, LOG_DAEMON);
		syslog(LOG_ERR, "probe failed to disarm %s error %d %d\n", path, err, errno);
	}

	close(fd);
	return rv;
}

static int _probe_dev(const char *path)
{
	struct stat buf;
	int fd, err, rv, timeout;

	rv = stat(path, &buf);
	if (rv < 0) {
		fprintf(stderr, "error %d stat %s\n", errno, path);
		return -1;
	}

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "error %d open %s\n", errno, path);
		return fd;
	}

	timeout = 0;

	rv = ioctl(fd, WDIOC_GETTIMEOUT, &timeout);
	if (rv < 0) {
		fprintf(stderr, "error %d ioctl gettimeout %s\n", errno, path);
		rv = -1;
		goto out;
	}

	if (timeout == fire_timeout) {
		printf("%s\n", path);
		rv = 0;
		goto out;
	}

	timeout = fire_timeout;

	rv = ioctl(fd, WDIOC_SETTIMEOUT, &timeout);
	if (rv < 0) {
		fprintf(stderr, "error %d ioctl settimeout %s\n", errno, path);
		rv = -1;
		goto out;
	}

	if (timeout != fire_timeout) {
		fprintf(stderr, "error %d invalid timeout %s\n", errno, path);
		rv = -1;
		goto out;
	}

	printf("%s\n", path);
	rv = 0;

 out:
	err = write(fd, "V", 1);
	if (err < 0) {
		fprintf(stderr, "probe failed to disarm %s error %d %d\n", path, err, errno);
		openlog("wdmd", LOG_CONS | LOG_PID, LOG_DAEMON);
		syslog(LOG_ERR, "probe failed to disarm %s error %d %d\n", path, err, errno);
	}

	close(fd);
	return rv;
}

static int probe_dev(const char *wdpath)
{
	char *path = (char *)wdpath;

	setup_identity(path);  /* sets itco=1 if iTCO_wdt */

	if (try_timeout) {
		/*
		 * Used to test support for a given timeout with: wdmd -t <secs>
		 * or to test firing for a given timeout with: wdmd -F -t <secs>
		 */
		if (itco)
			return _try_timeout_itco(path);
		else
			return _try_timeout(path);
	} else {
		/*
		 * Used to print on stdout just the path of the watchdog device
		 * that wdmd would use with: wdmd -p
		 */
		if (itco)
			return _probe_dev_itco(path);
		else
			return _probe_dev(path);
	}
}

/*
 * Confusingly, this is the top level function for both
 * wdmd -t (test timeout) and wdmd -p (print functional watchdog device).
 */
static int probe_watchdog(void)
{
	int rv;

	if (!saved_path[0])
		goto opt;

	rv = probe_dev(saved_path);
	if (!rv)
		return 0;

 opt:
	if (!option_path[0] || !strcmp(saved_path, option_path))
		goto zero;

	rv = probe_dev(option_path);
	if (!rv)
		return 0;

 zero:
	if (!strcmp(saved_path, "/dev/watchdog0") ||
	    !strcmp(option_path, "/dev/watchdog0"))
		goto one;

	rv = probe_dev((char *)"/dev/watchdog0");
	if (!rv)
		return 0;

 one:
	if (!strcmp(saved_path, "/dev/watchdog1") ||
	    !strcmp(option_path, "/dev/watchdog1"))
		goto old;

	rv = probe_dev((char *)"/dev/watchdog1");
	if (!rv)
		return 0;

 old:
	if (!strcmp(saved_path, "/dev/watchdog") ||
	    !strcmp(option_path, "/dev/watchdog"))
		goto out;

	rv = probe_dev((char *)"/dev/watchdog");
	if (!rv)
		return 0;

 out:
	fprintf(stderr, "no watchdog device, load a watchdog driver\n");
	return -1;

}

static void process_signals(int ci)
{
	struct signalfd_siginfo fdsi;
	ssize_t rv;
	int fd = client[ci].fd;

	rv = read(fd, &fdsi, sizeof(struct signalfd_siginfo));
	if (rv != sizeof(struct signalfd_siginfo)) {
		return;
	}

	if ((fdsi.ssi_signo == SIGTERM) ||
			(fdsi.ssi_signo == SIGINT)) {
		if (!active_clients())
			daemon_quit = 1;
	}

	if (fdsi.ssi_signo == SIGHUP) {
		setup_scripts();
	}
}

static int setup_signals(void)
{
	sigset_t mask;
	int fd, rv, ci;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);

	rv = sigprocmask(SIG_BLOCK, &mask, NULL);
	if (rv < 0)
		return rv;

	fd = signalfd(-1, &mask, 0);
	if (fd < 0)
		return -errno;

	ci = client_add(fd, process_signals, client_pid_dead);
	strncpy(client[ci].name, "signal", WDMD_NAME_SIZE);
	client[ci].internal = 1;
	return 0;
}

/*
 * We're trying to detect whether the last wdmd exited uncleanly and the
 * system has not been reset since.  In that case we don't want to start
 * and open /dev/watchdog, because that will ping the wd which will extend
 * the pending reset, which needs to happen on schedule.
 *
 * To detect this, we want to do/set something on the system that will
 * not go away (be cleared) if we exit, but will go away if the system
 * is reset.  If we were certain there was a tmpfs file system we could
 * use, then we could create a file there and just refuse to start if
 * the file exists.
 *
 * Until we are certain of tmpfs somewhere, create a shared mem object
 * on the system.
 */

static int setup_shm(void)
{
	int rv;

	rv = shm_open("/wdmd", O_RDWR|O_CREAT|O_EXCL, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
	if (rv < 0) {
		log_error("other wdmd not cleanly stopped, shm_open error %d", errno);
		return rv;
	}
	shm_fd = rv;
	return 0;
}

static void close_shm(void)
{
	shm_unlink("/wdmd");
	close(shm_fd);
}

static int test_loop(void)
{
	void (*workfn) (int ci);
	void (*deadfn) (int ci);
	uint64_t test_time;
	int resetting = 0;
	int active_usage, external_usage;
	int poll_timeout;
	int sleep_seconds;
	int fail_count;
	int rv, i;

	test_time = 0;
	poll_timeout = test_interval * 1000;

	while (1) {
		rv = poll(pollfd, client_maxi + 1, poll_timeout);
		if (rv == -1 && errno == EINTR)
			continue;
		if (rv < 0) {
			/* not sure */
		}
		for (i = 0; i <= client_maxi; i++) {
			if (client[i].fd < 0)
				continue;
			if (pollfd[i].revents & POLLIN) {
				workfn = client[i].workfn;
				if (workfn)
					workfn(i);
			}
			if (pollfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				deadfn = client[i].deadfn;
				if (deadfn)
					deadfn(i);
			}
		}

		count_clients(&active_usage, &external_usage);

		if (daemon_quit && !active_usage)
			break;

		/*
		 * No client has called open_watchdog() so the wd device is not open yet.
		 */
		if (!test_loop_enable)
			continue;

		/*
		 * active_usage are client connections with a refcount.
		 * external_usage are any clients other than internal.
		 * (open_watchdog happens with external but not active
		 * connections.)
		 *
		 * checking resetting here is critical to avoiding
		 * unnecessary resets: while in recovery mode we
		 * have done close_watchdog_unclean, then all clients
		 * are cleared, and we need the loop below to see
		 * no further failures and reopen and pet the watchdog
		 * again to avoid a reset.  After it's been reopened,
		 * and no longer used due to all clients being cleared,
		 * then it's ok to get here and close cleanly.
		 */
		if (!active_usage && !external_usage && !resetting) {
			log_debug("close watchdog unused");
			close_watchdog();
			test_loop_enable = 0;
			test_interval = standard_test_interval;
			poll_timeout = test_interval * 1000;
			test_time = 0;
			continue;
		}

		if (monotime() - test_time >= test_interval) {
			test_time = monotime();
			log_debug("test_time %llu",
				  (unsigned long long)test_time);

			fail_count = 0;
			fail_count += test_files();
			fail_count += test_scripts();
			fail_count += test_clients();

			if (!fail_count) {
				if (dev_fd == -1) {
					open_dev();
					pet_watchdog();
					log_error("%s reopen", watchdog_path);
				} else {
					pet_watchdog();
				}

				test_interval = standard_test_interval;
				resetting = 0;
			} else {
				/* If we can patch the kernel so that close
				   does not generate a ping, then we can skip
				   this close, and just not pet the device in
				   this case.  Also see test_client above. */
				close_watchdog_unclean();

				test_interval = RECOVER_TEST_INTERVAL;
				resetting = 1;
			}
		}

		sleep_seconds = test_time + test_interval - monotime();
		poll_timeout = (sleep_seconds > 0) ? sleep_seconds * 1000 : 500;

		log_debug("test_interval %d sleep_seconds %d poll_timeout %d",
			  test_interval, sleep_seconds, poll_timeout);
	}

	return 0;
}

static int lockfile(void)
{
	char buf[16];
	struct flock lock;
	mode_t old_umask;
	int fd, rv;

	old_umask = umask(0022);
	rv = mkdir(WDMD_RUN_DIR, 0775);
	if (rv < 0 && errno != EEXIST) {
		umask(old_umask);
		return rv;
	}
	umask(old_umask);

	sprintf(lockfile_path, "%s/wdmd.pid", WDMD_RUN_DIR);

	fd = open(lockfile_path, O_CREAT|O_WRONLY|O_CLOEXEC, 0644);
	if (fd < 0) {
		log_error("lockfile open error %s: %s",
			  lockfile_path, strerror(errno));
		return -1;
	}

	lock.l_type = F_WRLCK;
	lock.l_start = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len = 0;

	rv = fcntl(fd, F_SETLK, &lock);
	if (rv < 0) {
		log_error("daemon already running (lockfile %s %d)", lockfile_path, errno);
		goto fail;
	}

	rv = ftruncate(fd, 0);
	if (rv < 0) {
		log_error("lockfile truncate error %s: %s",
			  lockfile_path, strerror(errno));
		goto fail;
	}

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "%d\n", getpid());

	rv = write(fd, buf, strlen(buf));
	if (rv <= 0) {
		log_error("lockfile write error %s: %s",
			  lockfile_path, strerror(errno));
		goto fail;
	}

	return fd;
 fail:
	close(fd);
	return -1;
}

static void setup_priority(void)
{
	struct sched_param sched_param;
	int rv;

	if (!high_priority)
		return;

	rv = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (rv < 0) {
		log_info("mlockall failed");
	}

	rv = sched_get_priority_max(SCHED_RR);
	if (rv < 0) {
		log_info("could not get max scheduler priority err %d", errno);
		return;
	}

	sched_param.sched_priority = rv;
	rv = sched_setscheduler(0, SCHED_RR|SCHED_RESET_ON_FORK, &sched_param);
	if (rv < 0) {
		log_info("could not set RR|RESET_ON_FORK priority %d err %d",
			  sched_param.sched_priority, errno);
	}
}

static int group_to_gid(char *arg)
{
	struct group *gr;

	gr = getgrnam(arg);
	if (gr == NULL) {
		log_info("group '%s' not found, using socket gid: %i", arg, DEFAULT_SOCKET_GID);
		return DEFAULT_SOCKET_GID;
	}

	return gr->gr_gid;
}

static void print_debug_and_exit(void)
{
	struct sockaddr_un addr;
	struct wdmd_header h;
	int rv, s;

	s = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (s < 0)
		exit(1);

	rv = wdmd_socket_address(&addr);
	if (rv < 0)
		exit(1);

	rv = connect(s, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
	if (rv < 0)
		exit(1);

	memset(&h, 0, sizeof(h));
	h.cmd = CMD_DUMP_DEBUG;

	rv = send(s, (void *)&h, sizeof(struct wdmd_header), 0);
	if (rv < 0)
		exit(1);

	rv = recv(s, &debug_buf, DEBUG_SIZE, 0);
	if (rv < 0)
		exit(1);

	rv = write(STDOUT_FILENO, debug_buf, strlen(debug_buf));

	exit(0);
}

static void print_usage_and_exit(int status)
{
	printf("Usage:\n");
	printf("wdmd [options]\n\n");
	printf("--version, -V          print version\n");
	printf("--help, -h             print usage\n");
	printf("--dump, -d             print debug from daemon\n");
	printf("--probe, -p            print path of functional watchdog device\n");
	printf("--trytimeout, -t <sec> set the timeout value for watchdog device\n");
	printf("--forcefire, -F        force watchdog to fire and reset machine, use with -t\n");
	printf("-D                     debug: no fork and print all logging to stderr\n");
	printf("-H 0|1                 use high priority features (1 yes, 0 no, default %d)\n",
				       DEFAULT_HIGH_PRIORITY);
	printf("-G <name>              group ownership for the socket\n");
	printf("-S 0|1                 allow script tests (default %d)\n", allow_scripts);
	printf("-s <path>              path to scripts dir (default %s)\n", scripts_dir);
	printf("-k <num>               kill unfinished scripts after num seconds (default %d)\n",
				       kill_script_sec);
	printf("-w <path>              path to the watchdog device to try first\n");
	exit(status);
}

#define VERSION "1.0.1"

static void print_version_and_exit(void)
{
	printf("wdmd version %s\n", VERSION);
	exit(0);
}

/* If wdmd exits abnormally, /dev/watchdog will eventually fire, and clients
   can detect wdmd is gone and begin to shut down cleanly ahead of the reset.
   But what if wdmd is restarted before the wd fires?  It will begin petting
   /dev/watchdog again, leaving the previous clients unprotected.  I don't
   know if this situation is important enough to try to prevent.  One way
   would be for wdmd to fail starting if it found a pid file left over from
   its previous run. */

int main(int argc, char *argv[])
{
	int do_probe = 0;
	int rv;

	while (1) {
	    int c;
	    int option_index = 0;

	    static struct option long_options[] = {
	        {"help",       no_argument,       0,  'h' },
	        {"probe",      no_argument,       0,  'p' },
	        {"dump",       no_argument,       0,  'd' },
	        {"trytimeout", required_argument, 0,  't' },
	        {"forcefire",  no_argument,       0,  'F' },
	        {"version",    no_argument,       0,  'V' },
	        {0,            0,                 0,  0 }
	    };

	    c = getopt_long(argc, argv, "hpdVDH:G:S:s:k:w:t:F",
	                    long_options, &option_index);
	    if (c == -1)
	         break;

	    switch (c) {
	        case 'h':
                    print_usage_and_exit(0);
	            break;
		case 'p':
		    do_probe = 1;
		    break;
		case 't':
		    do_probe = 1;
		    try_timeout = atoi(optarg);
		    break;
		case 'F':
		    forcefire = 1;
		    break;
		case 'd':
		    print_debug_and_exit();
		    break;
	        case 'V':
                    print_version_and_exit();
	            break;
	        case 'D':
	            daemon_debug = 1;
	            break;
	        case 'G':
	            socket_gname = strdup(optarg);
	            break;
	        case 'H':
	            high_priority = atoi(optarg);
	            break;
		case 'S':
		    allow_scripts = atoi(optarg);
		    break;
		case 's':
		    scripts_dir = strdup(optarg);
		    break;
		case 'k':
		    kill_script_sec = atoi(optarg);
		    break;
		case 'w':
		    snprintf(option_path, WDPATH_SIZE, "%s", optarg);
		    option_path[WDPATH_SIZE - 1] = '\0';
		    break;
	    }
	}

	if (forcefire && !do_probe) {
		fprintf(stderr, "Use force fire (-F) with a timeout (-t).\n");
		exit(EXIT_FAILURE);
	}

	if (do_probe) {
		rv = setup_shm();
		if (rv < 0) {
			fprintf(stderr, "cannot probe watchdog devices while wdmd is in use.\n");
			openlog("wdmd-probe", LOG_CONS | LOG_PID, LOG_DAEMON);
			syslog(LOG_ERR, "cannot probe watchdog devices while wdmd is in use.\n");
			exit(EXIT_FAILURE);
		}

		rv = probe_watchdog();

		close_shm();

		if (rv < 0)
			exit(EXIT_FAILURE);
		else
			exit(EXIT_SUCCESS);
	}

	socket_gid = group_to_gid(socket_gname);

	if (!daemon_debug) {
		if (daemon(0, 0) < 0) {
			fprintf(stderr, "cannot fork daemon\n");
			exit(EXIT_FAILURE);
		}
	}

	openlog("wdmd", LOG_CONS | LOG_PID, LOG_DAEMON);

	setup_priority();

	rv = lockfile();
	if (rv < 0)
		goto out;

	rv = setup_shm();
	if (rv < 0)
		goto out_lockfile;
		  
	rv = setup_signals();
	if (rv < 0)
		goto out_shm;

	rv = setup_scripts();
	if (rv < 0)
		goto out_lockfile;

	rv = setup_files();
	if (rv < 0)
		goto out_scripts;

	rv = setup_clients();
	if (rv < 0)
		goto out_files;

	/* Sets watchdog_path */
	rv = setup_watchdog();
	if (rv < 0)
		goto out_clients;

	/* Sets watchdog_identity and itco */
	setup_identity(watchdog_path);

	log_info("wdmd started S%d H%d G%d using %s \"%s\"", allow_scripts, high_priority,
		 socket_gid, watchdog_path, watchdog_identity[0] ? watchdog_identity : "unknown");

	daemon_setup_done = 1;

	rv = test_loop();

	close_watchdog();
 out_clients:
	close_clients();
 out_files:
	close_files();
 out_scripts:
	close_scripts();
 out_shm:
	close_shm();
 out_lockfile:
	unlink(lockfile_path);
 out:
	return rv;
}

