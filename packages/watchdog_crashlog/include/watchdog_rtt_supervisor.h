/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WATCHDOG_CRASHLOG_RTT_SUPERVISOR_H
#define WATCHDOG_CRASHLOG_RTT_SUPERVISOR_H

#include <stdint.h>

#include <rtthread.h>

typedef int (*watchdog_rtt_health_check_fn)(void *context,
                                             rt_tick_t now,
                                             uint32_t *health_flags);
typedef void (*watchdog_rtt_feed_fn)(void *context);

typedef struct watchdog_rtt_supervisor_config
{
    const char *thread_name;
    uint32_t poll_interval_ms;
    uint32_t stack_size;
    uint8_t priority;
    uint32_t timeslice;
    watchdog_rtt_health_check_fn health_check;
    watchdog_rtt_feed_fn feed;
    void *context;
} watchdog_rtt_supervisor_config_t;

/*
 * Starts one periodic supervisor thread. The package calls feed only when
 * RT-Thread ticks advance and the product health callback reports healthy.
 * The product owns the hardware watchdog and all health policy.
 */
rt_err_t watchdog_rtt_supervisor_start(
    const watchdog_rtt_supervisor_config_t *config);

#endif
