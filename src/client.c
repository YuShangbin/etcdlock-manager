/* Copyright 2025 EasyStack, Inc. */

#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <syslog.h>
#include <pthread.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "etcdlock_manager_internal.h"
#include "etcdlock_manager_sock.h"
#include "env.h"
#include "client.h"

#ifndef GNUC_UNUSED
#define GNUC_UNUSED __attribute__((__unused__))
#endif

static int connect_socket(int *sock_fd)
{
	int rv, s;
	struct sockaddr_un addr;
	static const char *run_dir;

	*sock_fd = -1;
	s = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (s < 0)
		return -errno;

	if (run_dir == NULL)
		run_dir = env_get("ETCDLOCK_MGR_RUN_DIR", DEFAULT_RUN_DIR);

	rv = etcdlock_manager_socket_address(run_dir, &addr);
	if (rv < 0) {
		close(s);
		return rv;
	}

	rv = connect(s, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
	if (rv < 0) {
		rv = -errno;
		close(s);
		return rv;
	}
	*sock_fd = s;
	return 0;
}
static int send_header(int sock, int cmd, int datalen, 
	uint32_t data2)
{
	struct em_header header;
	size_t rem = sizeof(header);
	size_t off = 0;
	ssize_t rv;

	memset(&header, 0, sizeof(header));
	header.magic = EM_MAGIC;
	header.version = EM_PROTO;
	header.cmd = cmd;
	header.length = sizeof(header) + datalen;
	header.data2 = data2;

retry:
	rv = send(sock, (char *)&header + off, rem, 0);
	if (rv == -1 && errno == EINTR)
		goto retry;
	if (rv < 0)
		return -errno;
	if (rv < rem) {
		rem -= rv;
		off += rv;
		goto retry;
	}
	return 0;
}

static ssize_t send_data(int sockfd, const void *buf, size_t len, int flags)
{
	size_t rem = len;
	size_t off = 0;
	ssize_t rv;
retry:
	rv = send(sockfd, (char *)buf + off, rem, flags);
	if (rv == -1 && errno == EINTR)
		goto retry;
	if (rv < 0)
		return -errno;
	if (rv < rem) {
		rem -= rv;
		off += rv;
		goto retry;
	}
	return 0;
}

static int recv_result(int fd)
{
	struct em_header h;
	ssize_t rv;

	memset(&h, 0, sizeof(h));
retry:
	rv = recv(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv == -1 && errno == EINTR)
		goto retry;
	if (rv < 0)
		return -errno;
	if (rv != sizeof(h))
		return -1;

	return (int)h.data;
}

int etcdlock_acquire(int sock, char *volume, int vm_pid, char *vm_uri, char *vm_uuid)
{
    struct etcdlock *elk;
    int rv, fd, data2;
    int datalen = 0;
	char *value = NULL;

    /* Allocate memory for etcdlock structure */
    elk = malloc(sizeof(struct etcdlock));
	if (!elk)
        return -ENOMEM;

    datalen += sizeof(struct etcdlock);
	value = malloc(128 * sizeof(char));
	if (value){
		snprintf(value, 128, "%d", vm_pid);
	}
    elk->key = volume;
    elk->value = value;
    elk->vm_uri = vm_uri;
    elk->vm_uuid = vm_uuid;

    if (sock == -1) {
        /* connect to daemon and ask it to acquire a lease for
           another registered pid */

        data2 = vm_pid;

        rv = connect_socket(&fd);
        if (rv < 0){
			rv = -1;
			goto out;
		}           
    } else {
        /* use our own existing registered connection and ask daemon
           to acquire a lease for self */

        data2 = -1;
        fd = sock;
    }

    rv = send_header(fd, EM_CMD_ACQUIRE, datalen, data2);
    if (rv < 0){
		rv = -1;
		goto out;
	}

    rv = send_data(fd, elk, sizeof(struct etcdlock), 0);
    if (rv < 0) {
        rv = -1;
        goto out;
    }

    rv = recv_result(fd);
out:
    if (sock == -1)
		close(fd);

	if (elk)
    	free(elk);

	if (value)
		free(value);

    return rv;
}

/* tell daemon to release lease(s) for given pid.
   I don't think the pid itself will usually tell em to release leases,
   but it will be requested by a manager overseeing the pid */

int etcdlock_release(int sock, int pid, char *volume)
{
	struct etcdlock *elk;
    int fd, rv, data2, datalen;

	elk = malloc(sizeof(struct etcdlock));
	if (!elk)
        return -ENOMEM;

	if (sock == -1) {
		/* connect to daemon and ask it to acquire a lease for
		   another registered pid */

		data2 = pid;

		rv = connect_socket(&fd);
		if (rv < 0){
			rv = -1;
			goto out;
		}
	} else {
		/* use our own existing registered connection and ask daemon
		   to acquire a lease for self */

		data2 = -1;
		fd = sock;
	}

	datalen = sizeof(struct etcdlock);
    elk->key = volume;

	rv = send_header(fd, EM_CMD_RELEASE, datalen, data2);
	if (rv < 0){
		rv = -1;
		goto out;
	}

	rv = send_data(fd, elk, sizeof(struct etcdlock), 0);
	if (rv < 0) {
		rv = -1;
		goto out;
	}

	rv = recv_result(fd);
 out:
	if (sock == -1)
		close(fd);

	if (elk)
		free(elk);

	return rv;
}