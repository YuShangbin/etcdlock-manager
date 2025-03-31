/* Copyright 2025 EasyStack, Inc. */

#ifndef __ETCDLOCK_MANAGER_SOCK_H__
#define __ETCDLOCK_MANAGER_SOCK_H__

struct sm_header {
	uint32_t magic;
	uint32_t version;
	uint32_t cmd; /* SM_CMD_ */
	uint32_t cmd_flags;
	uint32_t length;
	uint32_t seq;
	uint32_t data;
	uint32_t data2;
};

#define ETCDLK_MGR_SOCKET_NAME "etcdlock_manager.sock"

#define SM_MAGIC 0x04282010
#define SM_PROTO 0x00000001

enum {
	SM_CMD_ACQUIRE		 = 1,
	SM_CMD_RELEASE		 = 2,
};

int etcdlock_manager_socket_address(const char *dir, struct sockaddr_un *addr);

#endif