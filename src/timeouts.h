/* Copyright 2025 EasyStack, Inc. */



#ifndef __TIMEOUTS_H__
#define __TIMEOUTS_H__

void setup_timeouts(void);
int calc_host_dead_seconds(int io_timeout);
int calc_keepalive_interval_seconds(int base_timeout);
int calc_keepalive_fail_timeout_seconds(int base_timeout);
int calc_keepalive_warn_timeout_seconds(int base_timeout);
int calc_set_bitmap_seconds(int io_timeout);
void log_timeouts(int io_timeout_arg);

#endif