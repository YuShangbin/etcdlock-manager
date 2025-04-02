/* Copyright 2025 EasyStack, Inc. */

#include <stdint.h>                    

#include "etcdlock_manager_internal.h" 
#include "log.h"                       


void setup_timeouts(void)
{
	/*
	 * graceful shutdown is client pids stopping their activity and
	 * releasing their etcdlock leases in response to a killpath program
	 * they configured, or in response to sigterm from etcdlock_manager if they
	 * did not set a killpath program.  It's an opportunity for the client
	 * pid to exit more gracefully than getting sigkill.  If the client
	 * pid does not release leases in response to the killpath/sigterm,
	 * then eventually etcdlock_manager will escalate and send a sigkill.
	 *
	 * It's hard to know what portion of recovery time should be allocated
	 * to graceful shutdown before escalating to sigkill.  The smaller the
	 * watchdog timeout, the less time between entering recovery mode and
	 * the watchdog potentially firing.  10 seconds before the watchdog
	 * will fire, the idea is to give up on graceful shutdown and resort
	 * to sending sigkill to any client pids that have not released their
	 * leases.  This gives 10 sec for the pids to exit from sigkill,
	 * etcdlock_manager to get the exit statuses, clear the expiring wdmd connection,
	 * and hopefully have wdmd ping the watchdog again before it fires.
	 * A graceful shutdown period of less than 10/15 sec seems pointless,
	 * so if there is anything less than 10/15 sec available for a graceful
	 * shutdown we don't bother and go directly to sigkill (this could
	 * of course be changed if programs are indeed able to respond
	 * quickly during graceful shutdown.)
	 */
	if (!com.kill_grace_set && (com.watchdog_fire_timeout < DEFAULT_WATCHDOG_FIRE_TIMEOUT)) {
		if (com.watchdog_fire_timeout < 60 && com.watchdog_fire_timeout >= 30)
			com.kill_grace_seconds = 15;
		else if (com.watchdog_fire_timeout < 30)
			com.kill_grace_seconds = 0;
	}
}

/*
 * Some of these timeouts depend on the the base_timeout, passed as the arg.
 * The base_timeout is only a base unit, and not the actual meaning
 */

int calc_keepalive_interval_seconds(int base_timeout)
{
	return 2 * base_timeout;
}

int calc_keepalive_fail_timeout_seconds(int base_timeout)
{
	return 8 * base_timeout;
}

int calc_keepalive_warn_timeout_seconds(int base_timeout)
{
	return 6 * base_timeout;
}