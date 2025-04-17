/* Copyright 2025 EasyStack, Inc. */

#ifndef __ETCDLOCK_MANAGER_INTERNAL_H__
#define __ETCDLOCK_MANAGER_INTERNAL_H__

#include <pthread.h>

#include "libvirt_helper.h"
#include "list.h"
#include "client.h"

#ifndef GNUC_UNUSED
#define GNUC_UNUSED __attribute__((__unused__))
#endif

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

#ifndef EXTERN
#define EXTERN extern
#else
#undef EXTERN
#define EXTERN
#endif

#define DEFAULT_KILL_COUNT_MAX 100
#define DEFAULT_USE_WATCHDOG 1
#define DEFAULT_WATCHDOG_FIRE_TIMEOUT 60
#define DEFAULT_GRACE_SEC 40
#define DEFAULT_HIGH_PRIORITY 0
#define DEFAULT_MLOCK_LEVEL 1 /* 1=CURRENT, 2=CURRENT|FUTURE */
#define DEFAULT_MAX_WORKER_THREADS 8
#define DEFAULT_MIN_WORKER_THREADS 2
#define DEFAULT_BASE_TIMEOUT 10
#define DEFAULT_QUIET_FAIL 1
#define DEFAULT_KEEPALIVE_HISTORY_SIZE 180 /* about 1 hour with 20 sec keepalive interval */
#define DEFAULT_SOCKET_UID 0
#define DEFAULT_SOCKET_GID 0

#define ETCDLK_MGR_CONF_PATH "/etc/etcdlock_manager/etcdlock_manager.conf"

/* command line types and actions */

#define COM_DAEMON      1
#define COM_CLIENT      2

enum { 
	ACT_ACQUIRE = 1, 
	ACT_RELEASE,
	ACT_VERSION,
};

struct command_line {
	int type;				/* COM_ */
	int action;				/* ACT_ */
	int debug;
	int debug_keepalive;
	int debug_hosts;
	int debug_clients;
	int debug_io_submit;
	int debug_io_complete;
	int paxos_debug_all;
	uint64_t debug_cmds;
	int max_sectors_kb_ignore;
	int max_sectors_kb_align;
	int max_sectors_kb_num;
	int quiet_fail;
	int wait;
	int use_watchdog;
	int watchdog_fire_timeout;
	int base_timeout;
	int kill_grace_seconds;		/* -g */
	int kill_grace_set;
	int high_priority;		/* -h */
	int get_hosts;			/* -h */
	int names_log_priority;
	int mlock_level;
	int max_worker_threads;
	int aio_arg;
	int write_init_io_timeout;
	int set_bitmap_seconds;
	int persistent;
	int orphan_set;
	int orphan;
	int used_set;
	int used;
	int all;
	int clear_arg;
	int sector_size;
	int align_size;
	int use_hugepages;
	char *uname;			/* -U */
	int uid;				/* -U */
	char *gname;			/* -G */
	int gid;				/* -G */
	int cid;				/* -C */
	char sort_arg;
	uint64_t host_id;			/* -i */
	uint64_t host_generation;		/* -g */
	uint64_t he_event;			/* -e */
	uint64_t he_data;			/* -d */
	int num_hosts;				/* -n */
	int max_hosts;				/* -m */
	int res_count;
	int sh_retries;
	uint32_t force_mode;
	int keepalive_history_size;
	struct etcdlk_resource *res_args[ETCDLK_MAX_RESOURCES];
	int vm_pid;				/* -pid */
	char *killpath;
	char *killargs;
};

EXTERN struct command_line com;

EXTERN int kill_count_max;
EXTERN int helper_ci;
EXTERN int helper_pid;
EXTERN int helper_kill_fd;
EXTERN int helper_status_fd;
EXTERN uint32_t helper_full_count;
EXTERN int external_shutdown;


struct lease_status {
	int corrupt_result;
	int acquire_last_result;
	int keepalive_last_result;
	uint64_t acquire_last_attempt;
	uint64_t acquire_last_success;
	uint64_t keepalive_last_attempt;
	uint64_t keepalive_last_success;
};

#define ETCDLOCK_KEY_LEN 48
#define ETCDLOCK_VALUE_LEN 10

struct etcdlock {
	struct list_head list;
	char key[ETCDLOCK_KEY_LEN];
	char value[ETCDLOCK_VALUE_LEN];
	uint32_t base_timeout;
	struct keepalive_history *keepalive_history;
	int keepalive_history_size;
	int keepalive_history_next;
	int keepalive_history_prev;
	pthread_mutex_t mutex;
	int thread_stop;
	struct lease_status lease_status;
	pthread_t thread;
	struct client *client;
	int client_ci;
	int killing_pid;
	int keepalive_fail;
	int wd_fd;
	char killpath[HELPER_PATH_LEN];
	char killargs[HELPER_ARGS_LEN];
};

struct keepalive_history {
	uint64_t timestamp;
	int next_timeouts;
	int next_errors;
};

#define CL_KILLPATH_PID 0x00000001 /* include pid as killpath arg */
#define CL_RUNPATH_SENT 0x00000002 /* a RUNPATH msg has been sent to helper */

EXTERN struct list_head etcdlocks;
EXTERN pthread_mutex_t etcdlocks_mutex;

#define COMMAND_MAX 4096

#define ETCDLOCK_MGR_RUN_DIR "ETCDLOCK_MGR_RUN_DIR"
#define DEFAULT_RUN_DIR "/run/etcdlock_manager"

#define ETCDLOCK_MGR_PRIVILEGED "ETCDLOCK_MGR_PRIVILEGED"

#define ETCDLK_MGR_NAME_LEN 48

struct client {
	int used;
	int fd;  /* unset is -1 */
	int pid; /* unset is -1 */
	int cmd_active;
	int cmd_last;
	int pid_dead;
	int suspend;
	int need_free;
	int kill_count;
	int tokens_slots;
	uint32_t flags;
	uint32_t restricted;
	uint64_t kill_last;
	char owner_name[ETCDLK_MGR_NAME_LEN+1];
	char killpath[HELPER_PATH_LEN];
	char killargs[HELPER_ARGS_LEN];
	pthread_mutex_t mutex;
	void *workfn;
	void *deadfn;
	struct token **tokens;
};

/*
 * client array is only touched by main_loop, there is no lock for it.
 * individual cl structs are accessed by worker threads using cl->mutex
 */

EXTERN struct client *client;

#define ETCDLK_MGR_LOG_DIR "/var/log"
#define ETCDLK_MGR_LOGFILE_NAME "etcdlock_manager.log"

#define DAEMON_NAME "etcdlock_manager"

#define ETCDLK_MGR_LOCKFILE_NAME "etcdlock_manager.pid"

struct worker_info {
	unsigned int busy;
	unsigned int cmd_last;
	unsigned int work_count;
	unsigned int io_count;
	unsigned int to_count;
};

EXTERN int daemon_status_num_workers; /* replicate thread pool counter for status reporting */

#define DEFAULT_SOCKET_MODE (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP)

EXTERN int efd;
EXTERN int is_helper;
EXTERN uint64_t helper_last_status;

#endif
