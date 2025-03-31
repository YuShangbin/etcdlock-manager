/* Copyright 2025 EasyStack, Inc. */

static void cmd_acquire(struct cmd_args *ca, uint32_t cmd)
{
	struct client *cl;
	struct etcdlock elk;
	int fd, rv;
	int pid_dead = 0;
	int result = 0;
	int cl_ci = ca->ci_target;
	int cl_fd = ca->cl_fd;
	int cl_pid = ca->cl_pid;

	cl = &client[cl_ci];
	fd = client[ca->ci_in].fd;

	log_cmd(cmd, "cmd_acquire %d,%d,%d ci_in %d fd %d flags %x",
		  cl_ci, cl_fd, cl_pid, ca->ci_in, fd, ca->header.cmd_flags);

	pthread_mutex_lock(&cl->mutex);
	if (cl->pid_dead) {
		result = -ESTALE;
		pthread_mutex_unlock(&cl->mutex);
		goto done;
	}

	/* TODO: get acquire params from client*/
	
	pthread_mutex_unlock(&cl->mutex);

	/*
	 * receive etcdlock params, acquire lock
	 */

	rv = recv_loop(fd, &elk, sizeof(struct etcdlock), MSG_WAITALL);
	if (rv != sizeof(struct etcdlock)) {
		log_error("cmd_acquire %d,%d,%d recv elk %d %d",
				  cl_ci, cl_fd, cl_pid, rv, errno);
		result = -ENOTCONN;
		goto done;
	}

	rv = acquire_lock_start(&elk);
	if (rv < 0) {
		result = rv;
		goto done;
	}

	/*
	 * Success acquiring the lock:
	 * lock mutex,
	 * 1. if pid is live, clear cmd_active, unlock mutex
	 * 2. if pid is dead, clear cmd_active, unlock mutex, release the lock, client_free
	 *
	 * Failure acquiring the lock:
	 * lock mutex,
	 * 3. if pid is live, clear cmd_active, unlock mutex, release the lock
	 * 4. if pid is dead, clear cmd_active, unlock mutex, release the lock, client_free
	 *
	 * If we find client_pid_dead
	 * has already happened when we look at pid_dead, then we know that it
	 * won't be called again, and it's our responsibility to call client_free.
	 */

	/*
	 * We hold both etcdlocks_mutex and cl->mutex at once to create the crucial
	 * linkage between the client pid and the etcdlock.
	 *
	 * Warning:
	 * We could deadlock if we hold cl->mutex and take etcdlocks_mutex,
	 * because pid_dead() and kill_pid() hold etcdlocks_mutex and take
	 * cl->mutex.  So, lock etcdlocks_mutex first, then cl->mutex to avoid the
	 * deadlock.
	 */

 done:
	pthread_mutex_lock(&etcdlocks_mutex);
	pthread_mutex_lock(&cl->mutex);
	log_cmd(cmd, "cmd_acquire %d,%d,%d result %d pid_dead %d",
		  cl_ci, cl_fd, cl_pid, result, cl->pid_dead);

	pid_dead = cl->pid_dead;
	cl->cmd_active = 0;

	pthread_mutex_unlock(&cl->mutex);
	pthread_mutex_unlock(&etcdlocks_mutex);

	/* 1. Success acquiring leases, and pid is live */

	if (!result && !pid_dead) {
		/* work done before mutex unlock */
		goto reply;
	}

	/* 2. Success acquiring lock, and pid is dead */

	if (!result && pid_dead) {
		release_lock(elk->key);
		client_free(cl_ci);
		result = -ENOTTY;
		goto reply;
	}

	/* 3. Failure acquiring leases, and pid is live */

	if (result && !pid_dead) {
		release_lock(elk->key);
		goto reply;
	}

	/* 4. Failure acquiring leases, and pid is dead */

	if (result && pid_dead) {
		release_lock(elk->key);
		client_free(cl_ci);
		goto reply;
	}

 reply:
	log_cmd(cmd, "cmd_acquire %d,%d,%d result %d",
		  cl_ci, cl_fd, cl_pid, result);
	send_result(ca->ci_in, fd, &ca->header, result);
	client_resume(ca->ci_in);
}

static void cmd_release(struct cmd_args *ca, uint32_t cmd)
{
	struct client *cl;
	int fd, rv, pid_dead;
	int result = 0;
	int cl_ci = ca->ci_target;
	int cl_fd = ca->cl_fd;
	int cl_pid = ca->cl_pid;

	cl = &client[cl_ci];
	fd = client[ca->ci_in].fd;

	log_cmd(cmd, "cmd_release %d,%d,%d ci_in %d fd %d",
		  cl_ci, cl_fd, cl_pid, ca->ci_in, fd);

	rv = recv_loop(fd, &elk, sizeof(struct etcdlock), MSG_WAITALL);
	if (rv != sizeof(struct etcdlock)) {
    	log_error("cmd_release %d,%d,%d recv elk %d %d",
				  cl_ci, cl_fd, cl_pid, rv, errno);
		result = -ENOTCONN;
		goto out;
	}

	result = release_lock(elk->key);

out:	
	pthread_mutex_lock(&cl->mutex);
	log_cmd(cmd, "cmd_release %d,%d,%d result %d pid_dead %d",
		  cl_ci, cl_fd, cl_pid, result, cl->pid_dead);

	pid_dead = cl->pid_dead;
	cl->cmd_active = 0;

	if (!pid_dead && cl->kill_count) {
		cl->kill_count = 0;
		cl->kill_last = 0;
		cl->flags &= ~CL_RUNPATH_SENT;

		log_cmd(cmd, "cmd_release %d,%d,%d clear kill state",
				cl_ci, cl_fd, cl_pid);
	}
	pthread_mutex_unlock(&cl->mutex);

	if (pid_dead) {
		client_free(cl_ci);
	}

	/* delete elk from etcdlocks list */
	pthread_mutex_lock(&etcdlocks_mutex);
    list_del(&elk->list);
	pthread_mutex_unlock(&etcdlocks_mutex);

	send_result(ca->ci_in, fd, &ca->header, result);
	client_resume(ca->ci_in);
}

void call_cmd_thread(struct cmd_args *ca)
{
	uint32_t cmd = ca->header.cmd;

	switch (cmd) {
	case SM_CMD_ACQUIRE:
		cmd_acquire(ca, cmd);
		break;
	case SM_CMD_RELEASE:
		cmd_release(ca, cmd);
		break;
	};
}

void call_cmd_daemon(int ci, struct sm_header *h_recv, int client_maxi)
{
	int rv, pid, auto_close = 1;
	int fd = client[ci].fd;
	uint32_t cmd = h_recv->cmd;

	switch (cmd) {
	case SM_CMD_VERSION:
		cmd_version(ci, fd, h_recv);
		auto_close = 0;
		break;
	};

	/*
	 * Previously just called close(fd) and did not set client[ci].fd = -1.
	 * This meant that a new client ci could get this fd and use it.
	 *
	 * When a poll error occurs because this ci was finished, then
	 * client_free(ci) would be called for this ci.  client_free would
	 * see cl->fd was still set and call close() on it, even though that
	 * fd was now in use by another ci.
	 *
	 * We could probably get by with just doing this here:
	 * client[ci].fd = -1;
	 * close(fd);
	 *
	 * and then handling the full client_free in response to
	 * the poll error (as done previously), but I see no reason
	 * to avoid the full client_free here.
	 */
	if (auto_close)
		client_free(ci);
}

void daemon_shutdown_reply(void)
{
	struct sm_header h;

	/* shutdown wait was not used */
	if (shutdown_reply_fd == -1)
		return;

	memset(&h, 0, sizeof(h));
	h.magic = SM_MAGIC;
	h.version = SM_PROTO;
	h.length = sizeof(h);

	send_all(shutdown_reply_fd, &h, sizeof(h), MSG_NOSIGNAL);
	close(shutdown_reply_fd);

	client_resume(shutdown_reply_ci);
}