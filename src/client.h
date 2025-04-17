/* Copyright 2025 EasyStack, Inc. */

#ifndef CLIENT_H
#define CLIENT_H

#define ETCDLK_NAME_LEN 48
#define ETCDLK_MAX_RESOURCES 16

struct etcdlk_resource {
	char name[ETCDLK_NAME_LEN];
};

int etcdlock_acquire(int sock, int res_count, struct etcdlk_resource *res_args[], int vm_pid, char *killpath, char *killargs);
int etcdlock_release(int sock, int res_count, struct etcdlk_resource *res_args[], int vm_pid);

#endif
