/* Copyright 2025 EasyStack, Inc. */

#ifndef __ETCDLOCK_MANAGER_SOCK_H__
#define __ETCDLOCK_MANAGER_SOCK_H__

struct em_header {
	uint32_t magic;
	uint32_t version;
	uint32_t cmd;
	uint32_t cmd_flags;
	uint32_t length;
	uint32_t seq;
	uint32_t data;
	uint32_t data2;
};

#define ETCDLK_MGR_SOCKET_NAME "etcdlock_manager.sock"

#define EM_MAGIC 0x04282010
#define EM_PROTO 0x00000001

enum {
	EM_CMD_ACQUIRE		 = 1,
	EM_CMD_RELEASE		 = 2,
	EM_CMD_VERSION		 = 3,
};

int etcdlock_manager_socket_address(const char *dir, struct sockaddr_un *addr);

#endif