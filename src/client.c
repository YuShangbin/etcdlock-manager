/* Copyright 2025 EasyStack, Inc. */



static int send_header(int sock, int cmd, int datalen,
		               uint32_t data2)
{
	struct sm_header header;
	size_t rem = sizeof(header);
	size_t off = 0;
	ssize_t rv;

	memset(&header, 0, sizeof(header));
	header.magic = SM_MAGIC;
	header.version = SM_PROTO;
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
	struct sm_header h;
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
    int rv, i, fd, data2;
    int datalen = 0;

    datalen += sizeof(struct etcdlock);
    elk->key = volume;
    elk->value = vm_pid;
    elk->vm_uri = vm_uri;
    elk->vm_uuid = vm_uuid;

    if (sock == -1) {
        /* connect to daemon and ask it to acquire a lease for
           another registered pid */

        data2 = pid;

        rv = connect_socket(&fd);
        if (rv < 0)
            return rv;
    } else {
        /* use our own existing registered connection and ask daemon
           to acquire a lease for self */

        data2 = -1;
        fd = sock;
    }

    rv = send_header(fd, SM_CMD_ACQUIRE, datalen, data2);
    if (rv < 0)
        return rv;

    rv = send_data(fd, &elk, sizeof(struct etcdlock), 0);
    if (rv < 0) {
        rv = -1;
        goto out;
    }

    rv = recv_result(fd);
out:
    if (sock == -1)
        close(fd);
    return rv;
}

/* tell daemon to release lease(s) for given pid.
   I don't think the pid itself will usually tell sm to release leases,
   but it will be requested by a manager overseeing the pid */

int etcdlock_release(int sock, int pid, char *volume)
{
	struct etcdlock *elk;
    int fd, rv, i, data2, datalen;

	if (sock == -1) {
		/* connect to daemon and ask it to acquire a lease for
		   another registered pid */

		data2 = pid;

		rv = connect_socket(&fd);
		if (rv < 0)
			return rv;
	} else {
		/* use our own existing registered connection and ask daemon
		   to acquire a lease for self */

		data2 = -1;
		fd = sock;
	}

	datalen = sizeof(struct etcdlock);
    elk->key = volume;

	rv = send_header(fd, SM_CMD_RELEASE, datalen, data2);
	if (rv < 0)
		goto out;

	rv = send_data(fd, &elk, sizeof(struct etcdlock), 0);
	if (rv < 0) {
		rv = -1;
		goto out;
	}

	rv = recv_result(fd);
 out:
	if (sock == -1)
		close(fd);
	return rv;
}