/* Copyright 2025 EasyStack, Inc. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/limits.h>

#include "etcdlock_manager_internal.h"
#include "log.h"


#define LOG_STR_LEN 512

static pthread_t thread_handle;

struct entry {
	int level;
	char str[LOG_STR_LEN];
};

#define LOG_DEFAULT_ENTRIES 4096

static char logfile_path[PATH_MAX];
static FILE *logfile_fp;
static struct entry *log_ents;
static unsigned int log_num_ents = LOG_DEFAULT_ENTRIES;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t log_cond = PTHREAD_COND_INITIALIZER;

static unsigned int log_head_ent; /* add at head */
static unsigned int log_tail_ent; /* remove from tail */
static unsigned int log_thread_done;
static unsigned int log_pending_ents;
static unsigned int log_dropped;

extern int log_stderr_priority;
extern int log_logfile_priority;
extern int log_syslog_priority;

static void write_entry(int level, char *str)
{
	/*if ((level <= log_logfile_priority) && logfile_fp) {
		fprintf(logfile_fp, "%s", str);
		fflush(logfile_fp);
	}
	if (level <= log_syslog_priority)
		syslog(level, "%s", str);*/
	return;
}

static void write_dropped(int level, int num)
{
	char str[LOG_STR_LEN];
	sprintf(str, "dropped %d entries", num);
	write_entry(level, str);
}

static void *log_thread_fn(void *arg GNUC_UNUSED)
{
	char str[LOG_STR_LEN];
	struct entry *e;
	int level, prev_dropped = 0;

	while (1) {
		pthread_mutex_lock(&log_mutex);
		while (log_head_ent == log_tail_ent) {
			if (log_thread_done) {
				pthread_mutex_unlock(&log_mutex);
				goto out;
			}
			pthread_cond_wait(&log_cond, &log_mutex);
		}

		e = &log_ents[log_tail_ent++];
		log_tail_ent = log_tail_ent % log_num_ents;
		log_pending_ents--;

		memcpy(str, e->str, LOG_STR_LEN);
		level = e->level;

		prev_dropped = log_dropped;
		log_dropped = 0;
		pthread_mutex_unlock(&log_mutex);

		if (prev_dropped) {
			write_dropped(level, prev_dropped);
			prev_dropped = 0;
		}

		write_entry(level, str);
	}
 out:
	pthread_exit(NULL);
}

int setup_logging(void)
{
	int fd, rv;

	snprintf(logfile_path, PATH_MAX, "%s/%s", ETCDLK_MGR_LOG_DIR,
        ETCDLK_MGR_LOGFILE_NAME);

	logfile_fp = fopen(logfile_path, "a+");
	if (logfile_fp) {
		fd = fileno(logfile_fp);
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
	}

	log_ents = malloc(log_num_ents * sizeof(struct entry));
	if (!log_ents) {
		fclose(logfile_fp);
		logfile_fp = NULL;
		return -1;
	}
	memset(log_ents, 0, log_num_ents * sizeof(struct entry));

	openlog(DAEMON_NAME, LOG_CONS | LOG_PID, LOG_DAEMON);

	rv = pthread_create(&thread_handle, NULL, log_thread_fn, NULL);
	if (rv)
		return -1;

	return 0;
}

void close_logging(void)
{
	pthread_mutex_lock(&log_mutex);
	log_thread_done = 1;
	pthread_cond_signal(&log_cond);
	pthread_mutex_unlock(&log_mutex);
	pthread_join(thread_handle, NULL);

	pthread_mutex_lock(&log_mutex);
	closelog();
	if (logfile_fp) {
		fclose(logfile_fp);
		logfile_fp = NULL;
	}

	pthread_mutex_unlock(&log_mutex);
}