/* Copyright 2025 EasyStack, Inc. */
/* This file provides functions for etcdlock manager calls libvirt sanlock helper */


#include <errno.h>      
#include <poll.h>
#include <stdio.h>     
#include <stdlib.h>     
#include <string.h>     
#include <unistd.h>     
#include <signal.h>
#include <ctype.h>
#include <time.h>

#include <grp.h>        
#include <sys/prctl.h>  
#include <sys/time.h>   
#include <sys/wait.h>   

#include "libvirt_helper.h"
#include "monotime.h"

#define MAX_AV_COUNT 8

static void run_path(struct helper_msg *hm)
{
	char arg[HELPER_ARGS_LEN];
	char *args = hm->args;
	char *av[MAX_AV_COUNT + 1]; /* +1 for NULL */
	int av_count = 0;
	int i, arg_len, args_len;

	fprintf(stderr, "run_path begin.\n");

	for (i = 0; i < MAX_AV_COUNT + 1; i++)
		av[i] = NULL;

	av[av_count++] = strdup(hm->path);

	if (!args[0])
		goto pid_arg;

	/* this should already be done, but make sure */
	args[HELPER_ARGS_LEN - 1] = '\0';

	memset(&arg, 0, sizeof(arg));
	arg_len = 0;
	args_len = strlen(args);

	for (i = 0; i < args_len; i++) {
		if (!args[i])
			break;

		if (av_count == MAX_AV_COUNT)
			break;

		if (args[i] == '\\') {
			if (i == (args_len - 1))
				break;
			i++;

			if (args[i] == '\\') {
				arg[arg_len++] = args[i];
				continue;
			}
			if (isspace(args[i])) {
				arg[arg_len++] = args[i];
				continue;
			} else {
				break;
			}
		}

		if (isalnum(args[i]) || ispunct(args[i])) {
			arg[arg_len++] = args[i];
		} else if (isspace(args[i])) {
			if (arg_len)
				av[av_count++] = strdup(arg);

			memset(arg, 0, sizeof(arg));
			arg_len = 0;
		} else {
			break;
		}
	}

	if ((av_count < MAX_AV_COUNT) && arg_len) {
		av[av_count++] = strdup(arg);
	}

 pid_arg:
	if ((av_count < MAX_AV_COUNT) && hm->pid) {
		memset(arg, 0, sizeof(arg));
		snprintf(arg, sizeof(arg)-1, "%d", hm->pid);
		av[av_count++] = strdup(arg);
	}

	fprintf(stderr, "run_path begin to exec killpath.\n");

	execvp(av[0], av);
}

static int read_hm(int fd, struct helper_msg *hm)
{
	int rv;
 retry:
	rv = read(fd, hm, sizeof(struct helper_msg));
	if (rv == -1 && errno == EINTR)
		goto retry;

	if (rv != sizeof(struct helper_msg))
		return -1;
	return 0;
}

static int send_status(int fd)
{
	struct helper_status hs;
	int rv;

	memset(&hs, 0, sizeof(hs));

	hs.type = HELPER_STATUS;

	rv = write(fd, &hs, sizeof(hs));

	if (rv == sizeof(hs))
		return 0;
	return -1;
}

#define log_debug(fmt, args...) \
do { \
	if (log_stderr) \
		fprintf(stderr, "helper %ld " fmt "\n", time(NULL), ##args); \
} while (0)

#define STANDARD_TIMEOUT_MS (HELPER_STATUS_INTERVAL*1000)
#define RECOVERY_TIMEOUT_MS 1000

int run_helper(int in_fd, int out_fd, int log_stderr)
{
	char name[16];
	struct pollfd pollfd;
	struct helper_msg hm;
	unsigned int fork_count = 0;
	unsigned int wait_count = 0;
	time_t now, last_send, last_good = 0;
	int timeout = STANDARD_TIMEOUT_MS;
	int rv, pid, status;

	memset(name, 0, sizeof(name));
	sprintf(name, "%s", "sanlock-helper");
	prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);

	rv = setgroups(0, NULL);
	if (rv < 0)
		log_debug("error clearing helper groups errno %i", errno);

	memset(&pollfd, 0, sizeof(pollfd));
	pollfd.fd = in_fd;
	pollfd.events = POLLIN;

	now = monotime();
	last_send = now;
	rv = send_status(out_fd);
	if (!rv)
		last_good = now;

	while (1) {
		rv = poll(&pollfd, 1, timeout);
		if (rv == -1 && errno == EINTR)
			continue;

		if (rv < 0)
			exit(0);

		now = monotime();

		if (now - last_good >= HELPER_STATUS_INTERVAL &&
		    now - last_send >= 2) {
			last_send = now;
			rv = send_status(out_fd);
			if (!rv)
				last_good = now;
		}

		memset(&hm, 0, sizeof(hm));

		if (pollfd.revents & POLLIN) {
			rv = read_hm(in_fd, &hm);
			if (rv)
				continue;

			/* terminated at sender, but confirm for checker */
			hm.path[HELPER_PATH_LEN-1] = '\0';
			hm.args[HELPER_ARGS_LEN-1] = '\0';

			if (hm.type == HELPER_MSG_RUNPATH) {
				fprintf(stderr, "run_helper recv runpath.\n");
				pid = fork();
				if (!pid) {
					run_path(&hm);
					exit(-1);
				}

				fork_count++;

				log_debug("helper fork %d count %d %d %s %s",
					  pid, fork_count, wait_count,
					  hm.path, hm.args);
			} else if (hm.type == HELPER_MSG_KILLPID) {
				fprintf(stderr, "run_helper recv kill pid.\n");
				kill(hm.pid, hm.sig);

			}
		}

		if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			exit(0);

		/* collect child exits until no more children exist (ECHILD)
		   or none are ready (WNOHANG) */

		while (1) {
			rv = waitpid(-1, &status, WNOHANG);
			if (rv > 0) {
				wait_count++;

				log_debug("helper wait %d count %d %d",
					  rv, fork_count, wait_count);
				continue;
			}

			/* no more children to wait for or no children
			   have exited */

			if (rv < 0 && errno == ECHILD) {
				if (timeout == RECOVERY_TIMEOUT_MS) {
					log_debug("helper no children count %d %d",
						  fork_count, wait_count);
				}
				timeout = STANDARD_TIMEOUT_MS;
			} else {
				timeout = RECOVERY_TIMEOUT_MS;
			}
			break;
		}
	}

	return 0;
}
