/* Copyright 2025 EasyStack, Inc. */

#ifndef __CMD_H__
#define __CMD_H__

#include "list.h"
#include "etcdlock_manager_sock.h"

struct cmd_args {
	struct list_head list; /* thread_pool data */
	int ci_in;
	int ci_target;
	int cl_fd;
	int cl_pid;
	struct em_header header;
};

/* cmds processed by thread pool */
void call_cmd_thread(struct cmd_args *ca);
void call_cmd_daemon(int ci, struct em_header *h_recv, int client_maxi);
void daemon_shutdown_reply(void);

#endif