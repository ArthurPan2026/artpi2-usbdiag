/* SPDX-License-Identifier: Apache-2.0 */
#include "watchdog_rtt_supervisor.h"

#include <string.h>

static watchdog_rtt_supervisor_config_t s_config;
static rt_thread_t s_thread;
static rt_bool_t s_started;

static void watchdog_rtt_supervisor_entry(void *parameter)
{
    rt_tick_t previous_tick = rt_tick_get();
    watchdog_rtt_supervisor_config_t *config =
        (watchdog_rtt_supervisor_config_t *)parameter;

    for (;;)
    {
        rt_tick_t now;
        uint32_t health_flags = 0U;

        rt_thread_mdelay(config->poll_interval_ms);
        now = rt_tick_get();

        /* A runnable thread with a stopped system tick must not feed a
         * hardware watchdog. The hardware timeout remains the backstop. */
        if (now == previous_tick)
        {
            continue;
        }
        previous_tick = now;

        if (config->health_check(config->context, now, &health_flags) != 0)
        {
            config->feed(config->context);
        }
    }
}

rt_err_t watchdog_rtt_supervisor_start(
    const watchdog_rtt_supervisor_config_t *config)
{
    rt_err_t result;

    if (config == RT_NULL || config->health_check == RT_NULL ||
        config->feed == RT_NULL || config->poll_interval_ms == 0U ||
        config->stack_size == 0U)
    {
        return -RT_EINVAL;
    }
    if (s_started != RT_FALSE)
    {
        return -RT_EBUSY;
    }

    memcpy(&s_config, config, sizeof(s_config));
    if (s_config.thread_name == RT_NULL)
    {
        s_config.thread_name = "wdog";
    }
    if (s_config.timeslice == 0U)
    {
        s_config.timeslice = 1U;
    }

    s_thread = rt_thread_create(s_config.thread_name,
                                watchdog_rtt_supervisor_entry,
                                &s_config,
                                s_config.stack_size,
                                s_config.priority,
                                s_config.timeslice);
    if (s_thread == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    result = rt_thread_startup(s_thread);
    if (result != RT_EOK)
    {
        (void)rt_thread_delete(s_thread);
        s_thread = RT_NULL;
        return result;
    }

    s_started = RT_TRUE;
    return RT_EOK;
}
