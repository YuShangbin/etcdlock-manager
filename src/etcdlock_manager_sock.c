/* Copyright 2025 EasyStack, Inc. */

#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "etcdlock_manager_sock.h"


int etcdlock_manager_socket_address(const char *dir, struct sockaddr_un *addr)
{
	memset(addr, 0, sizeof(struct sockaddr_un));
	addr->sun_family = AF_LOCAL;
	snprintf(addr->sun_path, sizeof(addr->sun_path) - 1, "%s/%s",
		 dir, ETCDLK_MGR_SOCKET_NAME);
	return 0;
}