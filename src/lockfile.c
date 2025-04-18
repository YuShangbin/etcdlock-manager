/* Copyright 2025 EasyStack, Inc. */
/*
 * Copyright 2010-2011 Red Hat, Inc.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v2 or (at your option) any later version.
 */

 #define _POSIX_C_SOURCE 200809L

#include <sys/file.h>
#include <sys/sysmacros.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <linux/limits.h>
#include <time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "log.h"
#include "lockfile.h"


int lockfile(const char *dir, const char *name, int uid, int gid)
{
	char path[PATH_MAX];
	char buf[16];
	struct flock lock;
	mode_t old_umask;
	int fd, rv;

	/* Make rundir group writable, allowing creation of the lockfile when
	 * starting as root. */

	old_umask = umask(0002);
	rv = mkdir(dir, 0775);
	if (rv < 0 && errno != EEXIST) {
		umask(old_umask);
		return rv;
	}
	umask(old_umask);

	rv = chown(dir, uid, gid);
	if (rv < 0) {
		/*log_error("lockfile chown error %s: %s",
			  dir, strerror(errno));*/
		fprintf(stderr, "lockfile chown error %s: %s\n",
			  dir, strerror(errno));
		return rv;
	}

	snprintf(path, PATH_MAX, "%s/%s", dir, name);

	fd = open(path, O_CREAT|O_WRONLY|O_CLOEXEC, 0644);
	if (fd < 0) {
		/*log_error("lockfile open error %s: %s",
			  path, strerror(errno));*/
		fprintf(stderr, "lockfile open error %s: %s\n",
			  path, strerror(errno));
		return -1;
	}

	lock.l_type = F_WRLCK;
	lock.l_start = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len = 0;

	rv = fcntl(fd, F_SETLK, &lock);
	if (rv < 0) {
		/*log_error("lockfile setlk error %s: %s",
			  path, strerror(errno));*/
		fprintf(stderr, "lockfile setlk error %s: %s\n",
			  path, strerror(errno));
		goto fail;
	}

	rv = ftruncate(fd, 0);
	if (rv < 0) {
		/*log_error("lockfile truncate error %s: %s",
			  path, strerror(errno));*/
		fprintf(stderr, "lockfile truncate error %s: %s\n",
			  path, strerror(errno));
		goto fail;
	}

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "%d\n", getpid());

	rv = write(fd, buf, strlen(buf));
	if (rv <= 0) {
		/*log_error("lockfile write error %s: %s",
			  path, strerror(errno));*/
		fprintf(stderr, "lockfile write error %s: %s\n",
			  path, strerror(errno));
		goto fail;
	}

	return fd;
 fail:
	close(fd);
	return -1;
}
