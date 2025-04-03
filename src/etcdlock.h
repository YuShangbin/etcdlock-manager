/* Copyright 2025 EasyStack, Inc. */

#ifndef ETCDLOCK_H
#define ETCDLOCK_H

void *lock_thread(void *arg_in);
int acquire_lock_start(struct etcdlock *elk);
int check_etcdlock_lease(struct etcdlock *elk);
int release_lock(const char *key);

#endif