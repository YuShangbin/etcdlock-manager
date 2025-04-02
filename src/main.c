/* Copyright 2025 EasyStack, Inc. */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <syslog.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <asm-generic/fcntl.h>
#include <asm-generic/siginfo.h>
#include <asm-generic/signal-defs.h>
#include <linux/sched.h>
#include <asm-generic/socket.h>

#define EXTERN

#include "cmd.h"
#include "env.h"
#include "etcdlock_manager_internal.h"
#include "etcdlock_manager_sock.h"
#include "libvirt_sanlock_helper.h"
#include "list.h"
#include "log.h"
#include "monotime.h"
#include "timeouts.h"


#define SIGRUNPATH 100 /* anything that's not SIGTERM/SIGKILL */
#define CLIENT_NALLOC 1024

struct thread_pool {
	int num_workers;
	int max_workers;
	int free_workers;
	int quit;
	struct list_head work_data;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_cond_t quit_wait;
	struct worker_info *info;
};

static const char *run_dir = NULL;
static int privileged = 1;
static struct pollfd *pollfd;
static int client_size = 0;
static int client_maxi;
static char command[COMMAND_MAX];
static int cmd_argc;
static char **cmd_argv;
static struct thread_pool pool;

int log_stderr_priority = -1;

static int user_to_uid(char *arg)
{
	struct passwd *pw;

	pw = getpwnam(arg);
	if (pw == NULL) {
		log_error("user '%s' not found, "
                          "using uid: %i", arg, DEFAULT_SOCKET_UID);
		return DEFAULT_SOCKET_UID;
	}

	return pw->pw_uid;
}

static int group_to_gid(char *arg)
{
	struct group *gr;

	gr = getgrnam(arg);
	if (gr == NULL) {
		log_error("group '%s' not found, "
                          "using uid: %i", arg, DEFAULT_SOCKET_GID);
		return DEFAULT_SOCKET_GID;
	}

	return gr->gr_gid;
}

#define MAX_CONF_LINE 128

static void get_val_int(char *line, int *val_out)
{
	char key[MAX_CONF_LINE];
	char val[MAX_CONF_LINE];
	int rv;

	rv = sscanf(line, "%[^=]=%s", key, val);
	if (rv != 2)
		return;

	*val_out = atoi(val);
}

static void get_val_str(char *line, char *val_out)
{
	char key[MAX_CONF_LINE];
	char val[MAX_CONF_LINE];
	int rv;

	rv = sscanf(line, "%[^=]=%s", key, val);
	if (rv != 2)
		return;

	strcpy(val_out, val);
}

static void read_config_file(void)
{
	FILE *file;
	struct stat buf;
	char line[MAX_CONF_LINE];
	char str[MAX_CONF_LINE];
	int i, val;

	if (stat(ETCDLK_MGR_CONF_PATH, &buf) < 0) {
		if (errno != ENOENT)
			log_error("%s stat failed: %d", ETCDLK_MGR_CONF_PATH, errno);
		return;
	}

	file = fopen(ETCDLK_MGR_CONF_PATH, "r");
	if (!file)
		return;

	while (fgets(line, MAX_CONF_LINE, file)) {
		if (line[0] == '#')
			continue;
		if (line[0] == '\n')
			continue;

		memset(str, 0, sizeof(str));

		for (i = 0; i < MAX_CONF_LINE; i++) {
			if (line[i] == ' ')
				break;
			if (line[i] == '=')
				break;
			if (line[i] == '\0')
				break;
			if (line[i] == '\n')
				break;
			if (line[i] == '\t')
				break;
			str[i] = line[i];
		}

		if (!strcmp(str, "quiet_fail")) {
			get_val_int(line, &val);
			com.quiet_fail = val;

		} else if (!strcmp(str, "use_watchdog")) {
			get_val_int(line, &val);
			com.use_watchdog = val;

		} else if (!strcmp(str, "base_timeout")) {
			get_val_int(line, &val);
			if (val > 0)
				com.base_timeout = val;

		} else if (!strcmp(str, "watchdog_fire_timeout")) {
			get_val_int(line, &val);
			if (val > 0)
				com.watchdog_fire_timeout = val;

		} else if (!strcmp(str, "kill_grace_seconds")) {
			get_val_int(line, &val);
			if (val <= 60 && val >= 0) {
				com.kill_grace_seconds = val;
				com.kill_grace_set = 1;
			}

		} else if (!strcmp(str, "high_priority")) {
			get_val_int(line, &val);
			com.high_priority = val;

		} else if (!strcmp(str, "mlock_level")) {
			get_val_int(line, &val);
			com.mlock_level = val;

		} else if (!strcmp(str, "uname")) {
			memset(str, 0, sizeof(str));
			get_val_str(line, str);
			com.uname = strdup(str);
			com.uid = user_to_uid(str);

		} else if (!strcmp(str, "gname")) {
			memset(str, 0, sizeof(str));
			get_val_str(line, str);
			com.gname = strdup(str);
			com.gid = group_to_gid(str);

		} else if (!strcmp(str, "keepalive_history_size")) {
			get_val_int(line, &val);
			com.keepalive_history_size = val;

		} else if (!strcmp(str, "max_worker_threads")) {
			get_val_int(line, &val);
			if (val < DEFAULT_MIN_WORKER_THREADS)
				val = DEFAULT_MIN_WORKER_THREADS;
			com.max_worker_threads = val;

		} 
	}

	fclose(file);
}

/*
 * daemon: acquires lease for the etcdlock, associates it with a local
 * pid, and releases it when the associated pid exits.
 *
 * client: ask daemon to acquire/release leases associated with a given pid.
 */
static void print_usage(void)
{
	printf("Usage:\n");
	printf("etcdlock_manager <command> <action> ...\n\n");
 
	printf("commands:\n");
	printf("  daemon        start daemon\n");
	printf("  client        send request to daemon (default type if none given)\n");
	printf("  help          print this usage (defaults in parents)\n");
	printf("  version       print version\n");
	printf("\n");
	printf("etcdlock_manager daemon [options]\n");
	printf("  -D            no fork and print all logging to stderr\n");
	printf("  -Q 0|1        quiet error messages for common lock contention (%d)\n", DEFAULT_QUIET_FAIL);
	printf("  -H <num>      keepalive history size (%d)\n", DEFAULT_KEEPALIVE_HISTORY_SIZE);
	printf("  -U <uid>      user id\n");
	printf("  -G <gid>      group id\n");
	printf("  -t <num>      max worker threads (%d)\n", DEFAULT_MAX_WORKER_THREADS);
	printf("  -g <sec>      seconds for graceful shutdown (%d)\n", DEFAULT_GRACE_SEC);
	printf("  -o <sec>      base timeout (%d)\n", DEFAULT_BASE_TIMEOUT);
	printf("  -w 0|1        use watchdog through wdmd (%d)\n", DEFAULT_USE_WATCHDOG);
	printf("  -h 0|1        use high priority (RR) scheduling (%d)\n", DEFAULT_HIGH_PRIORITY);
	printf("  -l <num>      use mlockall (0 none, 1 current, 2 current and future) (%d)\n", DEFAULT_MLOCK_LEVEL);
	printf("\n");
	printf("etcdlock_manager client acquire -volume <volume> -pid <vm_pid> -uri <vm_uri> -uuid <vm_uuid>\n");
	printf("etcdlock_manager client release -volume <volume> -pid <vm_pid>\n");
	printf("Limits:\n");
	printf("maximum client process connections: 1000\n"); /* NALLOC */
	printf("\n");
}

static int read_command_line(int argc, char *argv[])
{
	char optchar;
	char *optionarg;
	char *p;
	char *arg1 = argv[1];
	char *act;
	int i, j, len, sec, val, begin_command = 0;

	if (argc < 2 || !strcmp(arg1, "help") || !strcmp(arg1, "--help") ||
	    !strcmp(arg1, "-h")) {
		print_usage();
		exit(EXIT_SUCCESS);
	}

	if (!strcmp(arg1, "version")) {
		/* TODO: print correct version */
		printf("%u.%u.%u\n", 1, 0, 1);
		exit(EXIT_SUCCESS);
	}

	if (!strcmp(arg1, "daemon")) {
		com.type = COM_DAEMON;
		i = 2;
	} else if (!strcmp(arg1, "client")) {
		com.type = COM_CLIENT;
		if (argc < 3) {
			print_usage();
			exit(EXIT_FAILURE);
		}
		act = argv[2];
		i = 3;
	} else {
		com.type = COM_CLIENT;
		act = argv[1];
		i = 2;
	}

	switch (com.type) {
	case COM_DAEMON:
		break;

	case COM_CLIENT:
		if (!strcmp(act, "acquire"))
			com.action = ACT_ACQUIRE;
		else if (!strcmp(act, "release"))
			com.action = ACT_RELEASE;
		else {
			log_tool("client action \"%s\" is unknown", act);
			exit(EXIT_FAILURE);
		}
		break;
    };

	for (; i < argc; ) {
		p = argv[i];

		if ((p[0] != '-') || (strlen(p) != 2)) {
			log_tool("unknown option %s", p);
			log_tool("space required before option value");
			exit(EXIT_FAILURE);
		}

		optchar = p[1];
		i++;

		/* the only option that does not have optionarg */
		if (optchar == 'D') {
			com.debug = 1;
			log_stderr_priority = LOG_DEBUG;
			continue;
		}

		if (i >= argc) {
			log_tool("option '%c' requires arg", optchar);
			exit(EXIT_FAILURE);
		}

		optionarg = argv[i];

		switch (optchar) {
		case 'Q':
			com.quiet_fail = atoi(optionarg);
			break;
		case 'H':
			com.keepalive_history_size = atoi(optionarg);
			break;
		case 't':
			com.max_worker_threads = atoi(optionarg);
			if (com.max_worker_threads < DEFAULT_MIN_WORKER_THREADS)
				com.max_worker_threads = DEFAULT_MIN_WORKER_THREADS;
			break;
		case 'w':
			com.use_watchdog = atoi(optionarg);
			com.wait = atoi(optionarg);
			break;
		case 'h':
			com.high_priority = atoi(optionarg);
			break;
		case 'l':
			com.mlock_level = atoi(optionarg);
			break;
		case 'o':
			val = atoi(optionarg);
			if (val > 0)
				com.base_timeout = val;
			break;
		case 'C':
			com.cid = atoi(optionarg);
			break;
		case 'g':
			if (com.type == COM_DAEMON) {
				sec = atoi(optionarg);
				if (sec <= 60 && sec >= 0) {
					com.kill_grace_seconds = sec;
					com.kill_grace_set = 1;
				}
			}
			break;
		case 'U':
			com.uname = optionarg;
			com.uid = user_to_uid(optionarg);
			break;
		case 'G':
			com.gname = optionarg;
			com.gid = group_to_gid(optionarg);
			break;

		case 'c':
			begin_command = 1;
			break;

		case 'volume':
		    com.volume = strdup(optionarg);
		    break;

		case 'pid':
		    com.vm_pid = atoi(optionarg);
		    break;
		
		case 'uri':
		    com.vm_uri = strdup(optionarg);
		    break;

		case 'uuid':
		    com.vm_uuid = strdup(optionarg);
		    break;

		default:
			log_tool("unknown option: %c", optchar);
			exit(EXIT_FAILURE);
		};


		if (begin_command)
			break;

		i++;
	}

	/*
	 * the remaining args are for the command
	 *
	 * example:
	 * cmd_argc = 4 = argc (12) - i (8)
	 * cmd_argv[0] = "/bin/cmd"
	 * cmd_argv[1] = "-X"
	 * cmd_argv[2] = "-Y"
	 * cmd_argv[3] = "-Z"
	 * cmd_argv[4] = NULL (required by execv)
	 */

	if (begin_command) {
		cmd_argc = argc - i;

		if (cmd_argc < 1) {
			log_tool("command option (-c) requires an arg");
			return -EINVAL;
		}

		len = (cmd_argc + 1) * sizeof(char *); /* +1 for final NULL */
		cmd_argv = malloc(len);
		if (!cmd_argv)
			return -ENOMEM;
		memset(cmd_argv, 0, len);

		for (j = 0; j < cmd_argc; j++) {
			cmd_argv[j] = strdup(argv[i++]);
			if (!cmd_argv[j])
				return -ENOMEM;
		}

		strncpy(command, cmd_argv[0], COMMAND_MAX - 1);
	}

	return 0;
}

static void setup_groups(void)
{
	int rv;

	if (!com.uname || !com.gname || !privileged)
		return;

	rv = initgroups(com.uname, com.gid);
	if (rv < 0) {
		log_error("error initializing groups errno %i", errno);
	}
}

static void setup_limits(void)
{
	int rv;
	struct rlimit rlim = { .rlim_cur = -1, .rlim_max= -1 };

	if (!privileged)
		return;

	rv = setrlimit(RLIMIT_MEMLOCK, &rlim);
	if (rv < 0) {
		log_error("cannot set the limits for memlock %i", errno);
		exit(EXIT_FAILURE);
	}

	rv = setrlimit(RLIMIT_RTPRIO, &rlim);
	if (rv < 0) {
		log_error("cannot set the limits for rtprio %i", errno);
		exit(EXIT_FAILURE);
	}

	rv = setrlimit(RLIMIT_CORE, &rlim);
	if (rv < 0) {
		log_error("cannot set the limits for core dumps %i", errno);
		exit(EXIT_FAILURE);
	}
}

/* FIXME: add a mutex for client array so we don't try to expand it
   while a cmd thread is using it.  Or, with a thread pool we know
   when cmd threads are running and can expand when none are. */

static int client_alloc(void)
{
	int i;

	/* pollfd is one element longer as we use an additional element for the
	 * eventfd notification mechanism */
	client = malloc(CLIENT_NALLOC * sizeof(struct client));
	pollfd = malloc((CLIENT_NALLOC+1) * sizeof(struct pollfd));

	if (!client || !pollfd) {
		log_error("can't alloc for client or pollfd array");
		return -ENOMEM;
	}

	for (i = 0; i < CLIENT_NALLOC; i++) {
		memset(&client[i], 0, sizeof(struct client));
		memset(&pollfd[i], 0, sizeof(struct pollfd));

		pthread_mutex_init(&client[i].mutex, NULL);
		client[i].fd = -1;
		client[i].pid = -1;

		pollfd[i].fd = -1;
		pollfd[i].events = 0;
	}
	client_size = CLIENT_NALLOC;
	return 0;
}

static void _client_free(int ci)
{
	struct client *cl = &client[ci];

	if (cl->cmd_active || cl->pid_dead)
		log_client(ci, cl->fd, "free cmd %d dead %d", cl->cmd_active, cl->pid_dead);
	else
		log_client(ci, cl->fd, "free");

	if (!cl->used) {
		/* should never happen */
		log_error("client_free ci %d not used", ci);
		goto out;
	}

	if (cl->pid != -1) {
		/* client_pid_dead() should have set pid to -1 */
		/* should never happen */
		log_error("client_free ci %d live pid %d", ci, cl->pid);
		goto out;
	}

	if (cl->fd == -1) {
		/* should never happen */
		log_error("client_free ci %d is free", ci);
		goto out;
	}

	if (cl->need_free)
		log_debug("client_free ci %d already need_free", ci);

	if (cl->suspend) {
		log_debug("client_free ci %d is suspended", ci);
		cl->need_free = 1;
		goto out;
	}

	if (cl->fd != -1)
		close(cl->fd);

	cl->used = 0;
	cl->fd = -1;
	cl->pid = -1;
	cl->cmd_active = 0;
	cl->pid_dead = 0;
	cl->suspend = 0;
	cl->need_free = 0;
	cl->kill_count = 0;
	cl->kill_last = 0;
	cl->restricted = 0;
	cl->flags = 0;
	memset(cl->owner_name, 0, sizeof(cl->owner_name));
	memset(cl->killpath, 0, HELPER_PATH_LEN);
	memset(cl->killargs, 0, HELPER_ARGS_LEN);
	cl->workfn = NULL;
	cl->deadfn = NULL;

	/* make poll() ignore this connection */
	pollfd[ci].fd = -1;
	pollfd[ci].events = 0;
	pollfd[ci].revents = 0;
 out:
	return;
}

void client_free(int ci);
void client_free(int ci)
{
	struct client *cl = &client[ci];

	pthread_mutex_lock(&cl->mutex);
	_client_free(ci);
	pthread_mutex_unlock(&cl->mutex);
}

/* the connection that we suspend and resume may or may not be the
   same connection as the target client where we set cmd_active */

static int client_suspend(int ci)
{
	struct client *cl = &client[ci];
	int rv = 0;

	pthread_mutex_lock(&cl->mutex);

	if (!cl->used) {
		/* should never happen */
		log_error("client_suspend ci %d not used", ci);
		rv = -1;
		goto out;
	}

	if (cl->fd == -1) {
		/* should never happen */
		log_error("client_suspend ci %d is free", ci);
		rv = -1;
		goto out;
	}

	if (cl->suspend) {
		/* should never happen */
		log_error("client_suspend ci %d is suspended", ci);
		rv = -1;
		goto out;
	}

	log_client(ci, cl->fd, "suspend");

	cl->suspend = 1;

	/* make poll() ignore this connection */
	pollfd[ci].fd = -1;
	pollfd[ci].events = 0;
 out:
	pthread_mutex_unlock(&cl->mutex);

	return rv;
}

void client_resume(int ci);
void client_resume(int ci)
{
	struct client *cl = &client[ci];

	pthread_mutex_lock(&cl->mutex);

	if (!cl->used) {
		/* should never happen */
		log_error("client_resume ci %d not used", ci);
		goto out;
	}

	if (cl->fd == -1) {
		/* should never happen */
		log_error("client_resume ci %d is free", ci);
		goto out;
	}

	if (!cl->suspend) {
		/* should never happen */
		log_error("client_resume ci %d not suspended", ci);
		goto out;
	}

	log_client(ci, cl->fd, "resume");

	cl->suspend = 0;

	if (cl->need_free) {
		log_debug("client_resume ci %d need_free", ci);
		_client_free(ci);
	} else {
		/* make poll() watch this connection */
		pollfd[ci].fd = cl->fd;
		pollfd[ci].events = POLLIN;

		/* interrupt any poll() that might already be running */
		eventfd_write(efd, 1);
	}
 out:
	pthread_mutex_unlock(&cl->mutex);
}

static int client_add(int fd, void (*workfn)(int ci), void (*deadfn)(int ci))
{
	struct client *cl;
	int i;

	for (i = 0; i < client_size; i++) {
		cl = &client[i];
		pthread_mutex_lock(&cl->mutex);
		if (!cl->used) {
			cl->used = 1;
			cl->fd = fd;
			cl->workfn = workfn;
			cl->deadfn = deadfn ? deadfn : client_free;

			/* make poll() watch this connection */
			pollfd[i].fd = fd;
			pollfd[i].events = POLLIN;

			if (i > client_maxi)
				client_maxi = i;
			pthread_mutex_unlock(&cl->mutex);

			log_client(i, fd, "add");
			return i;
		}
		pthread_mutex_unlock(&cl->mutex);
	}

	return -1;
}

static void *thread_pool_worker(void *data)
{
	struct cmd_args *ca;
	long workerid = (long)data;

	pthread_mutex_lock(&pool.mutex);

	while (1) {
		while (!pool.quit && list_empty(&pool.work_data)) {
			pool.free_workers++;
			pthread_cond_wait(&pool.cond, &pool.mutex);
			pool.free_workers--;
		}

		while (!list_empty(&pool.work_data)) {
			ca = list_first_entry(&pool.work_data, struct cmd_args, list);
			list_del(&ca->list);
			pool.info[workerid].busy = 1;
			pool.info[workerid].cmd_last = ca->header.cmd;
			pthread_mutex_unlock(&pool.mutex);

			call_cmd_thread(ca);
			free(ca);

			pthread_mutex_lock(&pool.mutex);
			pool.info[workerid].busy = 0;
			pool.info[workerid].work_count++;
		}

		if (pool.quit)
			break;
	}

	pool.num_workers--;
	daemon_status_num_workers--; /* for status reporting */
	if (!pool.num_workers)
		pthread_cond_signal(&pool.quit_wait);
	pthread_mutex_unlock(&pool.mutex);

	return NULL;
}

static int thread_pool_add_work(struct cmd_args *ca)
{
	pthread_t th;
	int rv;

	pthread_mutex_lock(&pool.mutex);
	if (pool.quit) {
		pthread_mutex_unlock(&pool.mutex);
		return -1;
	}

	list_add_tail(&ca->list, &pool.work_data);

	if (!pool.free_workers && pool.num_workers < pool.max_workers) {
		rv = pthread_create(&th, NULL, thread_pool_worker,
				    (void *)(long)pool.num_workers);
		if (rv) {
			log_error("thread_pool_add_work ci %d error %d", ca->ci_in, rv);
			list_del(&ca->list);
			pthread_mutex_unlock(&pool.mutex);
			rv = -1;
			return rv;
		}
		pool.num_workers++;
		daemon_status_num_workers++; /* for status reporting */
	}

	pthread_cond_signal(&pool.cond);
	pthread_mutex_unlock(&pool.mutex);
	return 0;
}

static void thread_pool_free(void)
{
	pthread_mutex_lock(&pool.mutex);
	pool.quit = 1;
	if (pool.num_workers > 0) {
		pthread_cond_broadcast(&pool.cond);
		pthread_cond_wait(&pool.quit_wait, &pool.mutex);
	}
	pthread_mutex_unlock(&pool.mutex);
}

static int thread_pool_create(int min_workers, int max_workers)
{
	pthread_t th;
	int i, rv = 0;

	memset(&pool, 0, sizeof(pool));
	INIT_LIST_HEAD(&pool.work_data);
	pthread_mutex_init(&pool.mutex, NULL);
	pthread_cond_init(&pool.cond, NULL);
	pthread_cond_init(&pool.quit_wait, NULL);
	pool.max_workers = max_workers;
	pool.info = malloc(max_workers * sizeof(struct worker_info));

	if (!pool.info)
		return -ENOMEM;

	memset(pool.info, 0, max_workers * sizeof(struct worker_info));

	for (i = 0; i < min_workers; i++) {
		rv = pthread_create(&th, NULL, thread_pool_worker,
				    (void *)(long)i);
		if (rv) {
			log_error("thread_pool_create failed %d", rv);
			rv = -1;
			break;
		}
		pool.num_workers++;
		daemon_status_num_workers++; /* for status reporting */
	}

	if (rv < 0)
		thread_pool_free();

	return rv;
}

/*
 * first pipe for daemon to send requests to helper; they are not acknowledged
 * and the daemon does not get any result back for the requests.
 *
 * second pipe for helper to send general status/heartbeat back to the daemon
 * every so often to confirm it's not dead/hung.  If the helper gets stuck or
 * killed, the daemon will not get the status and won't bother sending requests
 * to the helper, and use SIGTERM instead
 */
static int setup_helper(void)
{
	int pid;
	int pw_fd = -1; /* parent write */
	int cr_fd = -1; /* child read */
	int pr_fd = -1; /* parent read */
	int cw_fd = -1; /* child write */
	int pfd[2];
 
	/* we can't allow the main daemon thread to block */
	if (pipe2(pfd, O_NONBLOCK | O_CLOEXEC))
		return -errno;
 
	/* uncomment for rhel7 where this should be available */
	/* fcntl(pfd[1], F_SETPIPE_SZ, 1024*1024); */
 
	cr_fd = pfd[0];
	pw_fd = pfd[1];
 
	if (pipe2(pfd, O_NONBLOCK | O_CLOEXEC)) {
		close(cr_fd);
		close(pw_fd);
		return -errno;
	}
 
	pr_fd = pfd[0];
	cw_fd = pfd[1];
 
	pid = fork();
	if (pid < 0) {
		close(cr_fd);
		close(pw_fd);
		close(pr_fd);
		close(cw_fd);
		return -errno;
	}
 
	if (pid) {
		close(cr_fd);
		close(cw_fd);
		helper_kill_fd = pw_fd;
		helper_status_fd = pr_fd;
		helper_pid = pid;
		return 0;
	} else {
		close(pr_fd);
		close(pw_fd);
		is_helper = 1;
		run_helper(cr_fd, cw_fd, (log_stderr_priority == LOG_DEBUG));
		exit(0);
	}
}

/* Close the helper */
static void close_helper(void)
{
	close(helper_kill_fd);
	close(helper_status_fd);
	helper_kill_fd = -1;
	helper_status_fd = -1;
	pollfd[helper_ci].fd = -1;
	pollfd[helper_ci].events = 0;
	helper_ci = -1;

	/* don't set helper_pid = -1 until we've tried waitpid */
}

static void process_helper(int ci)
{
	struct helper_status hs;
	int rv;

	memset(&hs, 0, sizeof(hs));

	rv = read(client[ci].fd, &hs, sizeof(hs));
	if (!rv || rv == -EAGAIN)
		return;
	if (rv < 0) {
		log_error("process_helper rv %d errno %d", rv, errno);
		goto fail;
	}
	if (rv != sizeof(hs)) {
		log_error("process_helper recv size %d", rv);
		goto fail;
	}

	if (hs.type == HELPER_STATUS && !hs.status)
		helper_last_status = monotime();

	return;

 fail:
	close_helper();
}

static void helper_dead(int ci GNUC_UNUSED)
{
	int pid = helper_pid;
	int rv, status;

	close_helper();

	helper_pid = -1;

	rv = waitpid(pid, &status, WNOHANG);

	if (rv != pid) {
		/* should not happen */
		log_error("helper pid %d dead wait %d", pid, rv);
		return;
	}

	if (WIFEXITED(status)) {
		log_error("helper pid %d exit status %d", pid,
			  WEXITSTATUS(status));
		return;
	}

	if (WIFSIGNALED(status)) {
		log_error("helper pid %d term signal %d", pid,
			  WTERMSIG(status));
		return;
	}

	/* should not happen */
	log_error("helper pid %d state change", pid);
}

static void sigterm_handler(int sig GNUC_UNUSED,
	siginfo_t *info GNUC_UNUSED,
	void *ctx GNUC_UNUSED)
{
	external_shutdown = 1;
}

static void setup_signals(void)
{
	struct sigaction act;
	int rv, i, sig_list[] = { SIGHUP, SIGINT, SIGTERM, 0 };

	memset(&act, 0, sizeof(act));

	act.sa_flags = SA_SIGINFO;
	act.sa_sigaction = sigterm_handler;

	for (i = 0; sig_list[i] != 0; i++) {
		rv = sigaction(sig_list[i], &act, NULL);
		if (rv < 0) {
			log_error("cannot set the signal handler for: %i", sig_list[i]);
			exit(EXIT_FAILURE);
		}
	}
}

static void setup_uid_gid(void)
{
	int rv;

	if (!com.uname || !com.gname || !privileged)
		return;

	rv = setgid(com.gid);
	if (rv < 0) {
		log_error("cannot set group id to %i errno %i", com.gid, errno);
	}

	rv = setuid(com.uid);
	if (rv < 0) {
		log_error("cannot set user id to %i errno %i", com.uid, errno);
	}

	/* When a program is owned by a user (group) other than the real user
	 * (group) ID of the process, the PR_SET_DUMPABLE option gets cleared.
	 * See RLIMIT_CORE in setup_limits and man 5 core.
	 */
	rv = prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
	if (rv < 0) {
		log_error("cannot set dumpable process errno %i", errno);
	}
}

static void setup_priority(void)
{
	struct sched_param sched_param;
	int rv = 0;

	if (com.mlock_level == 1)
		rv = mlockall(MCL_CURRENT);
	else if (com.mlock_level == 2)
		rv = mlockall(MCL_CURRENT | MCL_FUTURE);

	if (rv < 0) {
		log_error("mlockall %d failed: %s",
			  com.mlock_level, strerror(errno));
	}

	if (!com.high_priority)
		return;

	rv = sched_get_priority_max(SCHED_RR);
	if (rv < 0) {
		log_error("could not get max scheduler priority err %d", errno);
		return;
	}

	sched_param.sched_priority = rv;
	rv = sched_setscheduler(0, SCHED_RR|SCHED_RESET_ON_FORK, &sched_param);
	if (rv < 0) {
		log_error("set scheduler RR|RESET_ON_FORK priority %d failed: %s",
			  sched_param.sched_priority, strerror(errno));
	}
}

/*
 * cmd either comes from a registered client/fd, or is targeting a registered
 * client/fd.  The processing of the cmd is closely coordinated with the
 * lifetime of a specific client and to the etcdlock held by that client.  Handling
 * of the client's death or changing of the client's etcdlock will be serialized
 * with the processing of this command.  This means that the end of processing
 * this command needs to check if the client failed during the command
 * processing and handle the cleanup of the client if so.
 */

 static void process_cmd_thread_registered(int ci_in, struct em_header *h_recv)
 {
	 struct cmd_args *ca;
	 struct client *cl;
	 int result = 0;
	 int for_pid = -1, for_client_id = -1;
	 int rv, i, ci_target = -1;
 
	 ca = malloc(sizeof(struct cmd_args));
	 if (!ca) {
		 result = -ENOMEM;
		 goto fail;
	 }
 
	 if (h_recv->data2 != -1) {
		 /* lease for another registered client with pid or client_id specified by data2 */
 
		 for_pid = h_recv->data2;
 
		 for (i = 0; i < client_size; i++) {
			 cl = &client[i];
			 pthread_mutex_lock(&cl->mutex);
 
			 if (for_pid > -1) {
				 if (cl->pid != for_pid) {
					 pthread_mutex_unlock(&cl->mutex);
					 continue;
				 }
			 } else if (for_client_id > -1) {
				 if (i != for_client_id) {
					 pthread_mutex_unlock(&cl->mutex);
					 continue;
				 }
			 }
 
			 ci_target = i;
			 break;
		 }
 
		 log_client(ci_in, client[ci_in].fd, "process reg cmd %u for target ci %d pid %d",
				h_recv->cmd, ci_target, client[ci_target].pid);
	 } else {
		 /* lease for this registered client */
 
		 log_client(ci_in, client[ci_in].fd, "process reg cmd %u", h_recv->cmd);
 
		 ci_target = ci_in;
		 cl = &client[ci_target];
		 pthread_mutex_lock(&cl->mutex);
	 }
 
	 if (!cl->used) {
		 log_error("cmd %d %d,%d,%d not used",
			   h_recv->cmd, ci_target, cl->fd, cl->pid);
		 result = -EBUSY;
		 goto out;
	 }
 
	 if (cl->pid <= 0) {
		 log_error("cmd %d %d,%d,%d no pid",
			   h_recv->cmd, ci_target, cl->fd, cl->pid);
		 result = -EBUSY;
		 goto out;
	 }
 
	 if (cl->pid_dead) {
		 log_error("cmd %d %d,%d,%d pid_dead",
			   h_recv->cmd, ci_target, cl->fd, cl->pid);
		 result = -EBUSY;
		 goto out;
	 }
 
	 if (cl->need_free) {
		 log_error("cmd %d %d,%d,%d need_free",
			   h_recv->cmd, ci_target, cl->fd, cl->pid);
		 result = -EBUSY;
		 goto out;
	 }
 
	 if (cl->kill_count && h_recv->cmd == EM_CMD_ACQUIRE) {
		 /* when pid is being killed, we want killpath to be able
			to inquire and release for it */
		 log_error("cmd %d %d,%d,%d kill_count %d",
			   h_recv->cmd, ci_target, cl->fd, cl->pid, cl->kill_count);
		 result = -EBUSY;
		 goto out;
	 }
 
	 if (cl->cmd_active) {
		 if (com.quiet_fail && cl->cmd_active == EM_CMD_ACQUIRE) {
			 result = -EBUSY;
			 goto out;
		 }
		 log_error("cmd %d %d,%d,%d cmd_active %d",
			   h_recv->cmd, ci_target, cl->fd, cl->pid,
			   cl->cmd_active);
		 result = -EBUSY;
		 goto out;
	 }
 
	 cl->cmd_active = h_recv->cmd;
 
	 /* once cmd_active is set, client_pid_dead() will not call client_free, 
	    so it's the responsiblity of cmd_a,r,i_thread
		to check if pid_dead when clearing cmd_active, and doing the cleanup
		if pid is dead */
  out:
	 pthread_mutex_unlock(&cl->mutex);
 
	 if (result < 0)
		 goto fail;
 
	 ca->ci_in = ci_in;
	 ca->ci_target = ci_target;
	 ca->cl_pid = cl->pid;
	 ca->cl_fd = cl->fd;
	 memcpy(&ca->header, h_recv, sizeof(struct em_header));
 
	 rv = thread_pool_add_work(ca);
	 if (rv < 0) {
		 /* we don't have to worry about client_pid_dead having
			been called while mutex was unlocked with cmd_active set,
			because client_pid_dead is called from the main thread which
			is running this function */
 
		 log_error("create cmd thread failed");
		 pthread_mutex_lock(&cl->mutex);
		 cl->cmd_active = 0;
		 pthread_mutex_unlock(&cl->mutex);
		 result = rv;
		 goto fail;
	 }
	 return;
 
  fail:
	 log_error("process_cmd_thread_reg failed ci %d fd %d cmd %u", ci_in, client[ci_in].fd, h_recv->cmd);
	 send_result(ci_in, client[ci_in].fd, h_recv, result);
	 client_resume(ci_in);
 
	 if (ca)
		 free(ca);
 }

static void process_connection(int ci)
{
	struct em_header h;
	void (*deadfn)(int ci);
	int rv;

	memset(&h, 0, sizeof(h));

 retry:
	rv = recv(client[ci].fd, &h, sizeof(h), MSG_WAITALL);
	if (rv == -1 && errno == EINTR)
		goto retry;
	if (!rv)
		goto dead;

	log_client(ci, client[ci].fd, "recv %d %u", rv, h.cmd);

	if (rv < 0) {
		log_error("client connection %d %d %d recv msg header rv %d errno %d",
			  ci, client[ci].fd, client[ci].pid, rv, errno);
		goto bad;
	}
	if (rv != sizeof(h)) {
		log_error("client connection %d %d %d recv msg header rv %d cmd %u len %u",
			  ci, client[ci].fd, client[ci].pid, rv, h.cmd, h.length);
		goto bad;
	}
	if (h.magic != EM_MAGIC) {
		log_error("client connection %d %d %d recv msg header rv %d cmd %u len %u magic %x vs %x",
			  ci, client[ci].fd, client[ci].pid, rv, h.cmd, h.length, h.magic, EM_MAGIC);
		goto bad;
	}
	if (h.version && (h.cmd != EM_CMD_VERSION) &&
	    (h.version & 0xFFFF0000) > (EM_PROTO & 0xFFFF0000)) {
		log_error("client connection %d %d %d recv msg header rv %d cmd %u len %u version %x",
			  ci, client[ci].fd, client[ci].pid, rv, h.cmd, h.length, h.version);
		goto bad;
	}

	client[ci].cmd_last = h.cmd;

	switch (h.cmd) {
	case EM_CMD_VERSION:
		call_cmd_daemon(ci, &h, client_maxi);
		break;
	case EM_CMD_ACQUIRE:
	case EM_CMD_RELEASE:
		/* the main_loop needs to ignore this connection
		   while the thread is working on it */
		rv = client_suspend(ci);
		if (rv < 0)
			goto bad;
		process_cmd_thread_registered(ci, &h);
		break;
	default:
		log_error("client connection ci %d fd %d pid %d cmd %d unknown",
			  ci, client[ci].fd, client[ci].pid, h.cmd);
		goto bad;
	};

	return;

 bad:
	return;

 dead:
	log_client(ci, client[ci].fd, "recv dead");
	deadfn = client[ci].deadfn;
	if (deadfn)
		deadfn(ci);
}

static void process_listener(int ci GNUC_UNUSED)
{
	int fd;
	int on = 1;

	fd = accept(client[ci].fd, NULL, NULL);
	if (fd < 0)
		return;

	setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));

	client_add(fd, process_connection, NULL);
}

static int setup_listener(void)
{
	struct sockaddr_un addr;
	int rv, fd, ci;

	rv = etcdlock_manager_socket_address(run_dir, &addr);
	if (rv < 0)
		return rv;

	fd = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (fd < 0)
		return fd;

	unlink(addr.sun_path);
	rv = bind(fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
	if (rv < 0)
		goto exit_fail;

	rv = chmod(addr.sun_path, DEFAULT_SOCKET_MODE);
	if (rv < 0)
		goto exit_fail;

	rv = chown(addr.sun_path, com.uid, com.gid);
	if (rv < 0) {
		log_error("could not set socket %s permissions: %s",
			addr.sun_path, strerror(errno));
		goto exit_fail;
	}

	rv = listen(fd, 5);
	if (rv < 0)
		goto exit_fail;

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

	ci = client_add(fd, process_listener, NULL);
	if (ci < 0)
		goto exit_fail;

	strcpy(client[ci].owner_name, "listener");
	return 0;

 exit_fail:
	close(fd);
	return -1;
}

/* Send a SIGRUNPATH, SIGTERM or SIGKILL message to the helper */
static void send_helper_kill(struct etcdlock *elk, struct client *cl, int sig)
{
	struct helper_msg hm;
	int rv;

	/*
	 * We come through here once a second while the pid still has
	 * leases.  We only send a single RUNPATH message, so after
	 * the first RUNPATH goes through we set CL_RUNPATH_SENT to
	 * avoid futher RUNPATH's.
	 */

	if ((cl->flags & CL_RUNPATH_SENT) && (sig == SIGRUNPATH))
		return;

	if (helper_kill_fd == -1) {
		log_error("send_helper_kill pid %d no fd", cl->pid);
		return;
	}

	memset(&hm, 0, sizeof(hm));

	if (sig == SIGRUNPATH) {
		hm.type = HELPER_MSG_RUNPATH;
		memcpy(hm.path, cl->killpath, HELPER_PATH_LEN-1);
		memcpy(hm.args, cl->killargs, HELPER_ARGS_LEN-1);

		/* only include pid if it's requested as a killpath arg */
		if (cl->flags & CL_KILLPATH_PID)
			hm.pid = cl->pid;
	} else {
		hm.type = HELPER_MSG_KILLPID;
		hm.sig = sig;
		hm.pid = cl->pid;
	}

	log_erros(elk, "kill %d sig %d count %d", cl->pid, sig, cl->kill_count);

 retry:
	rv = write(helper_kill_fd, &hm, sizeof(hm));
	if (rv == -1 && errno == EINTR)
		goto retry;

	/* pipe is full, we'll try again in a second */
	if (rv == -1 && errno == EAGAIN) {
		helper_full_count++;
		log_etcdlock(elk, "send_helper_kill pid %d sig %d full_count %u",
			  cl->pid, sig, helper_full_count);
		return;
	}

	/* helper exited or closed fd, quit using helper */
	if (rv == -1 && errno == EPIPE) {
		log_erros(elk, "send_helper_kill EPIPE");
		close_helper();
		return;
	}

	if (rv != sizeof(hm)) {
		/* this shouldn't happen */
		log_erros(elk, "send_helper_kill pid %d error %d %d",
			  cl->pid, rv, errno);
		close_helper();
		return;
	}

	if (sig == SIGRUNPATH)
		cl->flags |= CL_RUNPATH_SENT;
}

static void kill_pid(struct etcdlock *elk)
{
	struct client *cl;
	uint64_t now, last_success;
	int keepalive_fail_timeout_seconds;
	int sig;
	int in_grace;

	/*
	 * remaining pid using elk is stuck, we've made max attempts to
	 * kill it, don't bother cycling through it
	 */
	if (elk->killing_pid > 1)
		return;

	keepalive_fail_timeout_seconds = calc_keepalive_fail_timeout_seconds(elk->base_timeout);

	/*
	 * If we happen to keepalive our lease after we've started killing pid,
	 * the period we allow for graceful shutdown will be extended. This
	 * is an incidental effect, although it may be nice. The previous
	 * behavior would still be ok, where we only ever allow up to
	 * kill_grace_seconds for graceful shutdown before moving to sigkill.
	 */
	pthread_mutex_lock(&elk->mutex);
	last_success = elk->lease_status.keepalive_last_success;
	cl = elk->client;
	pthread_mutex_unlock(&elk->mutex);

	now = monotime();

    pthread_mutex_lock(&cl->mutex);

	if (!cl->used)
		goto unlock;

	if (cl->pid <= 0)
		goto unlock;

	if (cl->kill_count >= kill_count_max)
		goto unlock;

	if (cl->kill_count && (now - cl->kill_last < 1))
		goto unlock;

	cl->kill_last = now;
	cl->kill_count++;

	/*
	 * the transition from using killpath/sigterm to sigkill
	 * is when now >=
	 * last successful lease keepalive +
	 * keepalive_fail_timeout_seconds +
	 * kill_grace_seconds
	 */

	in_grace = now < (last_success + keepalive_fail_timeout_seconds + com.kill_grace_seconds);

	if ((com.kill_grace_seconds > 0) && in_grace && cl->killpath[0]) {
		sig = SIGRUNPATH;
	} else if (in_grace) {
		sig = SIGTERM;
	} else {
		sig = SIGKILL;
	}
unlock:
	pthread_mutex_unlock(&cl->mutex);

	send_helper_kill(elk, cl, sig);
}

static int pid_dead(struct etcdlock *elk)
{
	struct client *cl;
	int stuck = 0, check = 0;
	int ci;

	pthread_mutex_lock(&elk->mutex);
	cl = elk->client;
	pthread_mutex_unlock(&elk->mutex);

    pthread_mutex_lock(&cl->mutex);

	if (!cl->used)
		goto unlock;
	if (cl->pid <= 0)
		goto unlock;

	if (cl->kill_count >= kill_count_max)
		stuck++;
	else
		check++;
unlock:
	pthread_mutex_unlock(&cl->mutex);

	if (stuck && !check && elk->killing_pid < 2) {
		log_erros(elk, "killing pids stuck %d", stuck);
		/* cause kill_pid to give up */
		elk->killing_pid = 2;
	}

	if (stuck || check)
		return 0;

	if (elk->keepalive_fail)
		log_erros(elk, "pid clear");
	else
		log_etcdlock(elk, "pid clear");

	return 1;
}

static unsigned int time_diff(struct timeval *begin, struct timeval *end)
{
	struct timeval result;
	timersub(end, begin, &result);
	return (result.tv_sec * 1000) + (result.tv_usec / 1000);
}

#define STANDARD_CHECK_INTERVAL 1000 /* milliseconds */
#define RECOVERY_CHECK_INTERVAL  200 /* milliseconds */
static int main_loop(void)
{
	void (*workfn) (int ci);
	void (*deadfn) (int ci);
	struct etcdlock *elk, *safe;
	struct timeval now, last_check;
	int poll_timeout, check_interval;
	unsigned int ms;
	int i, rv, empty, check_all;
	char *check_buf = NULL;
	int check_buf_len = 0;
	uint64_t ebuf;

	gettimeofday(&last_check, NULL);
	poll_timeout = STANDARD_CHECK_INTERVAL;
	check_interval = STANDARD_CHECK_INTERVAL;

	while (1) {
		/* as well as the clients, check the eventfd */
		pollfd[client_maxi+1].fd = efd;
		pollfd[client_maxi+1].events = POLLIN;

		rv = poll(pollfd, client_maxi + 2, poll_timeout);
		if (rv == -1 && errno == EINTR)
			continue;
		if (rv < 0) {
			/* not sure */
			log_client(0, 0, "poll err %d", rv);
		}
		for (i = 0; i <= client_maxi + 1; i++) {
			/*
			 * This index for efd has no client array entry.  Its
			 * only purpose is to wake up this poll loop in which
			 * case we just clear any data and continue looking
			 * for other client entries that need processing.
			 */
			if (pollfd[i].fd == efd) {
				if (pollfd[i].revents & POLLIN) {
					log_client(i, efd, "efd wake"); /* N.B. i is not a ci */
					eventfd_read(efd, &ebuf);
				}
				continue;
			}

			/*
			 * FIXME? client_maxi is never reduced so over time we
			 * end up checking and skipping some number of unused
			 * client entries here which seems inefficient.
			 */
			if (client[i].fd < 0)
				continue;

			if (pollfd[i].revents & POLLIN) {
				workfn = client[i].workfn;
				if (workfn)
					workfn(i);
			}
			if (pollfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				log_client(i, client[i].fd, "poll dead");
				deadfn = client[i].deadfn;
				if (deadfn)
					deadfn(i);
			}
		}

		gettimeofday(&now, NULL);
		ms = time_diff(&last_check, &now);
		if (ms < check_interval) {
			poll_timeout = check_interval - ms;
			continue;
		}
		last_check = now;
		check_interval = STANDARD_CHECK_INTERVAL;

		/*
		 * check the condition of each etcdlock,
		 * if pid is being killed, has pid exited?
		 * is its lease keepalived? if not kill pid
		 */
		pthread_mutex_lock(&etcdlocks_mutex);
		list_for_each_entry_safe(elk, safe, &etcdlocks, list) {

			if (elk->killing_pid && pid_dead(elk)) {
				/*
				 * delete elk from etcdlocks so main_loop
				 * will no longer see it.
				 */
				log_etcdlock(elk, "set thread_stop");
				pthread_mutex_lock(&elk->mutex);
				elk->thread_stop = 1;
				deactivate_watchdog(elk);
				pthread_mutex_unlock(&elk->mutex);
				list_del(&elk->list);
				continue;
			}

			if (elk->killing_pid) {
				/*
				 * continue to kill the pid with increasing
				 * levels of severity until it exit
				 */
				kill_pid(elk);
				check_interval = RECOVERY_CHECK_INTERVAL;
				continue;
			}

			/*
			 * check etcdlock lease keepalive
			 */
			rv = check_etcdlock_lease(elk);
			if (rv) {
				elk->keepalive_fail = 1;
				elk->killing_pid = 1;
				kill_pid(elk);
				check_interval = RECOVERY_CHECK_INTERVAL;
			}
		}
		empty = list_empty(&etcdlocks);
		pthread_mutex_unlock(&etcdlocks_mutex);

		if (external_shutdown && empty)
			break;

		if (external_shutdown == 1) {
			log_debug("ignore shutdown, etcdlock exists");
			external_shutdown = 0;
		}

		gettimeofday(&now, NULL);
		ms = time_diff(&last_check, &now);
		if (ms < check_interval)
			poll_timeout = check_interval - ms;
		else
			poll_timeout = 1;
	}

	daemon_shutdown_reply();

	return 0;
}

static int do_daemon(void)
{
	struct utsname nodename;
	int fd, rv;

	run_dir = env_get(ETCDLOCK_MGR_RUN_DIR, DEFAULT_RUN_DIR);
	privileged = env_get_bool(ETCDLOCK_MGR_PRIVILEGED, 1);

	/* This can take a while so do it before forking. */
	setup_groups();

	setup_limits();
	setup_timeouts();
	setup_helper();

	rv = client_alloc();
	if (rv < 0)
		return rv;

	helper_ci = client_add(helper_status_fd, process_helper, helper_dead);
	if (helper_ci < 0)
		return rv;
	strcpy(client[helper_ci].owner_name, "helper");

	setup_signals();
	setup_logging();

	if (strcmp(run_dir, DEFAULT_RUN_DIR))
		log_warn("Using non-standard run directory '%s'", run_dir);

	if (!privileged)
		log_warn("Running in unprivileged mode");

	/* If we run as root, make run_dir owned by root, so we can create the
	 * lockfile when selinux disables DAC_OVERRIDE.
	 * See https://danwalsh.livejournal.com/79643.html */

	fd = lockfile(run_dir, ETCDLK_MGR_LOCKFILE_NAME, com.uid,
		      privileged ? 0 : com.gid);
	if (fd < 0) {
		close_logging();
		return fd;
	}

	setup_uid_gid();

	if (com.base_timeout != DEFAULT_BASE_TIMEOUT || com.watchdog_fire_timeout != DEFAULT_WATCHDOG_FIRE_TIMEOUT) {
		log_warn("etcdlock_manager daemon started base_timeout %u watchdog_fire_timeout %u",
			 com.base_timeout, com.watchdog_fire_timeout);
		syslog(LOG_INFO, "etcdlock_manager daemon started base_timeout %u watchdog_fire_timeout %u",
			 com.base_timeout, com.watchdog_fire_timeout);
	} else {
		log_warn("etcdlock_manager daemon started");
		syslog(LOG_INFO, "etcdlock_manager daemon started");
	}

	setup_priority();

	rv = thread_pool_create(DEFAULT_MIN_WORKER_THREADS, com.max_worker_threads);
	if (rv < 0)
		goto out;

	rv = setup_listener();
	if (rv < 0)
		goto out_threads;

	/* initialize global eventfd for client_resume notification */
	if ((efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) == -1) {
		log_error("couldn't create eventfd");
		goto out_threads;
	}

	main_loop();

 out_threads:
	thread_pool_free();
 out:
	/* order reversed from setup so lockfile is last */
	close_logging();
	close(fd);
	return rv;
}

static int do_client(void)
{
	int id = -1;
	int i, fd;
	int rv = 0;

	switch (com.action) {
	case ACT_ACQUIRE:
		if (com.vm_pid > -1) {
			log_tool("acquire pid %d", com.vm_pid);
			id = com.vm_pid;
		} else if (com.cid > -1) {
			log_tool("acquire client_id %d", com.cid);
			id = com.cid;
		}
		rv = etcdlock_acquire(-1, id, com.volume, com.vm_pid, com.vm_uri, com.vm_uuid);
		log_tool("acquire done %d", rv);
		break;

	case ACT_RELEASE:
		if (com.vm_pid > -1) {
			log_tool("release pid %d", com.vm_pid);
			id = com.vm_pid;
		} else if (com.cid > -1) {
			log_tool("release client_id %d", com.cid);
			id = com.cid;
		}
		rv = etcdlock_release(-1, id, com.volume);
		log_tool("release done %d", rv);
		break;

	default:
		log_tool("action not implemented");
		rv = -1;
	}
 out:
	return rv;
}

int main(int argc, char *argv[])
{
	int rv;

	BUILD_BUG_ON(sizeof(struct helper_msg) != HELPER_MSG_LEN);

	/* initialize global EXTERN variables */
    kill_count_max = DEFAULT_KILL_COUNT_MAX;
	helper_ci = -1;
	helper_pid = -1;
	helper_kill_fd = -1;
	helper_status_fd = -1;

	memset(&com, 0, sizeof(com));
	com.use_watchdog = DEFAULT_USE_WATCHDOG;
	com.watchdog_fire_timeout = DEFAULT_WATCHDOG_FIRE_TIMEOUT;
	com.kill_grace_seconds = DEFAULT_GRACE_SEC;
	com.high_priority = DEFAULT_HIGH_PRIORITY;
	com.mlock_level = DEFAULT_MLOCK_LEVEL;
	com.names_log_priority = LOG_INFO;
	com.max_worker_threads = DEFAULT_MAX_WORKER_THREADS;
	com.base_timeout = DEFAULT_BASE_TIMEOUT;
	com.vm_pid = -1;
	com.cid = -1;
	com.quiet_fail = DEFAULT_QUIET_FAIL;
	com.keepalive_history_size = DEFAULT_KEEPALIVE_HISTORY_SIZE;

	if (getgrnam("etcdlock_manager") && getpwnam("etcdlock_manager")) {
		com.uname = (char *)"etcdlock_manager";
		com.gname = (char *)"etcdlock_manager";
		com.uid = user_to_uid(com.uname);
		com.gid = group_to_gid(com.uname);
	} else {
		com.uname = NULL;
		com.gname = NULL;
		com.uid = DEFAULT_SOCKET_UID;
		com.gid = DEFAULT_SOCKET_GID;
	}

	/*
	 * read_config_file() overrides com default settings,
	 * read_command_line() overrides com default settings and
	 * config file settings.
	 */
	read_config_file();

	rv = read_command_line(argc, argv);
	if (rv < 0)
		goto out;

	switch (com.type) {
	case COM_DAEMON:
		rv = do_daemon();
		break;

	case COM_CLIENT:
		rv = do_client();
		break;
	};
 out:
	return rv == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}