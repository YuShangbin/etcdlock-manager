/* Copyright 2025 EasyStack, Inc. */

#ifndef CLIENT_H
#define CLIENT_H

int etcdlock_acquire(int sock, char *volume, int vm_pid, char *killpath, char *killargs);
int etcdlock_release(int sock, int pid, char *volume);

#endif