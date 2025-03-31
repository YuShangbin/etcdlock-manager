/* Copyright 2025 EasyStack, Inc. */

#include <stdint.h>

#ifndef __LIBVIRT_SANLOCK_HELPER_H__
#define __LIBVIRT_SANLOCK_HELPER_H__

#define HELPER_PATH_LEN  128
#define HELPER_ARGS_LEN  128

#define HELPER_MSG_LEN 512

#define HELPER_MSG_RUNPATH 1
#define HELPER_MSG_KILLPID 2

struct helper_msg {
	uint8_t type;
	uint8_t pad1;
	uint16_t pad2;
	uint32_t flags;
	int pid;
	int sig;
	char path[HELPER_PATH_LEN]; /* 128 */
	char args[HELPER_ARGS_LEN]; /* 128 */
	char pad[240];
};

#define HELPER_STATUS_INTERVAL 30

#define HELPER_STATUS 1

struct helper_status {
	uint8_t type;
	uint8_t status;
	uint16_t len;
};

int run_helper(int in_fd, int out_fd, int log_stderr);

#endif