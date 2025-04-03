/* Copyright 2025 EasyStack, Inc. */
/*
 * Copyright 2010-2011 Red Hat, Inc.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v2 or (at your option) any later version.
 */

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
#include <pthread.h>
#include <time.h>
#include <dirent.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "etcdlock_manager_internal.h"
#include "log.h"
#include "watchdog.h"

/*
 * Purpose of watchdog: to forcibly reset the host in the case where a
 * supervised pid is running but sanlock daemon does not renew its lease
 * and does not kill the pid (or it kills the pid but the pid does not
 * exit).  So, just before the pid begins running with granted leases,
 * /dev/watchdog needs to be armed to reboot the host if things go bad right
 * after the pid goes ahead.
 */

#include "../wdmd/wdmd.h"

/* tell wdmd to open the watchdog device, set the fire timeout and begin keepalives */
int open_watchdog(int con, int fire_timeout)
{
	int rv;

	if (!com.use_watchdog)
		return 0;

	rv = wdmd_open_watchdog(con, fire_timeout);
	if (rv < 0) {
		/*log_error("wdmd_open_watchdog fire_timeout %d error", fire_timeout);*/
		fprintf(stderr, "wdmd_open_watchdog fire_timeout %d error\n", fire_timeout);
		return -1;
	}

	return 0;
}

/* tell wdmd that this connection is still good and watchdog pings can continue for it */
void update_watchdog(struct etcdlock *elk, uint64_t timestamp,
		     int keepalive_fail_timeout_seconds)
{
	int rv;

	if (!com.use_watchdog)
		return;

	rv = wdmd_test_live(elk->wd_fd, timestamp, timestamp + keepalive_fail_timeout_seconds);
	if (rv < 0)
		/*log_erros(elk, "wdmd_test_live %llu failed %d",
			  (unsigned long long)timestamp, rv);*/
		fprintf(stderr, "wdmd_test_live %llu failed %d\n",
			  (unsigned long long)timestamp, rv);
}

/* connects to the wdmd daemon */
int connect_watchdog(struct etcdlock *elk)
{
	int con;

	if (!com.use_watchdog)
		return 0;

	con = wdmd_connect();
	if (con < 0) {
		/*log_erros(elk, "wdmd_connect failed %d", con);*/
		fprintf(stderr, "wdmd_connect failed %d\n", con);
		return -1;
	}

	return con;
}

/* associate wdmd keepalives to the continued liveness of this lock */
int activate_watchdog(struct etcdlock *elk, uint64_t timestamp,
		      int keepalive_fail_timeout_seconds, int con)
{
	char name[WDMD_NAME_SIZE];
	int test_interval, fire_timeout;
	uint64_t last_keepalive;
	int rv;

	if (!com.use_watchdog)
		return 0;

	memset(name, 0, sizeof(name));

	snprintf(name, WDMD_NAME_SIZE - 1, "etcdlock_%s:%llu",
		 elk->key, (unsigned long long)elk->value);

	rv = wdmd_register(con, name);
	if (rv < 0) {
		/*log_erros(elk, "wdmd_register failed %d", rv);*/
		fprintf(stderr, "wdmd_register failed %d\n", rv);
		goto fail_close;
	}

	/* the refcount tells wdmd that it should not cleanly exit */

	rv = wdmd_refcount_set(con);
	if (rv < 0) {
		/*log_erros(elk, "wdmd_refcount_set failed %d", rv);*/
		fprintf(stderr, "wdmd_refcount_set failed %d\n", rv);
		goto fail_close;
	}

	rv = wdmd_status(con, &test_interval, &fire_timeout, &last_keepalive);
	if (rv < 0) {
		/*log_erros(elk, "wdmd_status failed %d", rv);*/
		fprintf(stderr, "wdmd_status failed %d\n", rv);
		goto fail_clear;
	}

	if (fire_timeout != com.watchdog_fire_timeout) {
		/*log_erros(elk, "wdmd invalid fire_timeout %d vs %d",
			  fire_timeout, com.watchdog_fire_timeout);*/
		fprintf(stderr, "wdmd invalid fire_timeout %d vs %d\n",
			  fire_timeout, com.watchdog_fire_timeout);
		goto fail_clear;
	}

	rv = wdmd_test_live(con, timestamp, timestamp + keepalive_fail_timeout_seconds);
	if (rv < 0) {
		/*log_erros(elk, "wdmd_test_live in create failed %d", rv);*/
		fprintf(stderr, "wdmd_test_live in create failed %d\n", rv);
		goto fail_clear;
	}

	elk->wd_fd = con;
	return 0;

 fail_clear:
	wdmd_refcount_clear(con);
 fail_close:
	close(con);
	return -1;
}

void deactivate_watchdog(struct etcdlock *elk)
{
	int rv;

	if (!com.use_watchdog)
		return;

	/*log_etcdlock(elk, "wdmd_test_live 0 0 to disable");*/
	fprintf(stderr, "wdmd_test_live 0 0 to disable\n");

	rv = wdmd_test_live(elk->wd_fd, 0, 0);
	if (rv < 0) {
		/*log_erros(elk, "wdmd_test_live in deactivate failed %d", rv);*/
		fprintf(stderr, "wdmd_test_live in deactivate failed %d\n", rv);

		/* We really want this to succeed to avoid a reset, so retry
	   	   after a short delay in case the problem was transient... */

		usleep(500000);

		rv = wdmd_test_live(elk->wd_fd, 0, 0);
		if (rv < 0)
			/*log_erros(elk, "wdmd_test_live in deactivate 2 failed %d", rv);*/
			fprintf(stderr, "wdmd_test_live in deactivate 2 failed %d\n", rv);
	}

	wdmd_refcount_clear(elk->wd_fd);
}

void disconnect_watchdog(struct etcdlock *elk)
{
	if (!com.use_watchdog)
		return;

	close(elk->wd_fd);
}

