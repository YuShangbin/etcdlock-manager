/* Copyright 2025 EasyStack, Inc. */

#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <linux/time.h>

#include "monotime.h"


uint64_t monotime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec;
}