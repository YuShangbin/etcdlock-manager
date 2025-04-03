/* Copyright 2025 EasyStack, Inc. */

#define _POSIX_C_SOURCE 200112L

#include <unistd.h>
#include <stdint.h>
#include <time.h>

#include "monotime.h"


uint64_t monotime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec;
}