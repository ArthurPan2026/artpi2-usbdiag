/*
 * MIT License
 *
 * Copyright (c) 2024 Evlers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Change Logs:
 * Date         Author      Notes
 * 2023-12-21   Evlers      first implementation
 */

#include <stdio.h>
#include <string.h>
#include "rtthread.h"
#include "whd.h"
#include "whd_thread.h"
#include "cyabs_rtos.h"
#include "whd_rtos_diag.h"

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS && \
    !defined(PROTO_MSGBUF)
#include "whd_int.h"

static cy_semaphore_t * volatile s_sdpcm_queue_watch_mutex;
static const volatile uint32_t *s_sdpcm_queue_watch_counts;
static uint32_t s_sdpcm_queue_previous[WHD_RTOS_SDPCM_QUEUE_COUNT];
static whd_rtos_sdpcm_queue_stats_t s_sdpcm_queue_stats;

static void whd_rtos_sdpcm_queue_watch_unregister_semaphore(
    cy_semaphore_t *semaphore)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (s_sdpcm_queue_stats.watching &&
        s_sdpcm_queue_watch_mutex == semaphore)
    {
        s_sdpcm_queue_stats.watching = false;
        s_sdpcm_queue_watch_mutex = NULL;
        s_sdpcm_queue_watch_counts = NULL;
    }
    rt_hw_interrupt_enable(level);
}

static void whd_rtos_sdpcm_queue_watch_snapshot(
    cy_semaphore_t *semaphore)
{
    rt_base_t level;

    /* Most WHD semaphore releases are unrelated to SDPCM. Reject them with
     * one pointer read and no interrupt masking; recheck after masking to
     * cover watcher registration/unregistration races. */
    if (s_sdpcm_queue_watch_mutex != semaphore)
    {
        return;
    }
    level = rt_hw_interrupt_disable();

    if (s_sdpcm_queue_stats.watching &&
        s_sdpcm_queue_watch_mutex == semaphore &&
        s_sdpcm_queue_watch_counts != NULL)
    {
        uint32_t queue;

        for (queue = 0U; queue < WHD_RTOS_SDPCM_QUEUE_COUNT; queue++)
        {
            uint32_t current = s_sdpcm_queue_watch_counts[queue];
            uint32_t previous = s_sdpcm_queue_previous[queue];

            s_sdpcm_queue_stats.current[queue] = current;
            if (current > s_sdpcm_queue_stats.high_water[queue])
            {
                s_sdpcm_queue_stats.high_water[queue] = current;
            }
            if (current > previous)
            {
                s_sdpcm_queue_stats.enqueued[queue] += current - previous;
            }
            else if (current < previous)
            {
                s_sdpcm_queue_stats.dequeued[queue] += previous - current;
            }
            s_sdpcm_queue_previous[queue] = current;
        }
        s_sdpcm_queue_stats.snapshots++;
    }
    rt_hw_interrupt_enable(level);
}
#endif

bool whd_rtos_sdpcm_queue_watch_register(whd_driver_t driver)
{
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS && \
    !defined(PROTO_MSGBUF)
    cy_semaphore_t *mutex;
    const volatile uint32_t *queue_counts;
    rt_base_t level;
    uint32_t queue;
    bool registered;

    if (driver == NULL)
    {
        return false;
    }
    mutex = &driver->sdpcm_info.send_queue_mutex;
    queue_counts = driver->sdpcm_info.npkt_in_q;
    level = rt_hw_interrupt_disable();
    if (s_sdpcm_queue_stats.watching)
    {
        registered = s_sdpcm_queue_watch_mutex == mutex;
    }
    else
    {
        memset(&s_sdpcm_queue_stats, 0, sizeof(s_sdpcm_queue_stats));
        s_sdpcm_queue_watch_mutex = mutex;
        s_sdpcm_queue_watch_counts = queue_counts;
        for (queue = 0U; queue < WHD_RTOS_SDPCM_QUEUE_COUNT; queue++)
        {
            uint32_t current = queue_counts[queue];

            s_sdpcm_queue_previous[queue] = current;
            s_sdpcm_queue_stats.current[queue] = current;
            s_sdpcm_queue_stats.high_water[queue] = current;
        }
        s_sdpcm_queue_stats.watching = true;
        registered = true;
    }
    rt_hw_interrupt_enable(level);
    return registered;
#else
    RT_UNUSED(driver);
    return false;
#endif
}

void whd_rtos_sdpcm_queue_watch_unregister(whd_driver_t driver)
{
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS && \
    !defined(PROTO_MSGBUF)
    if (driver != NULL)
    {
        whd_rtos_sdpcm_queue_watch_unregister_semaphore(
            &driver->sdpcm_info.send_queue_mutex);
    }
#else
    RT_UNUSED(driver);
#endif
}

void whd_rtos_sdpcm_queue_stats_get(whd_rtos_sdpcm_queue_stats_t *stats)
{
    if (stats == NULL)
    {
        return;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS && \
    !defined(PROTO_MSGBUF)
    {
        rt_base_t level = rt_hw_interrupt_disable();

        *stats = s_sdpcm_queue_stats;
        rt_hw_interrupt_enable(level);
    }
#else
    memset(stats, 0, sizeof(*stats));
#endif
}

void whd_rtos_sdpcm_queue_stats_reset(void)
{
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS && \
    !defined(PROTO_MSGBUF)
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t queue;

    for (queue = 0U; queue < WHD_RTOS_SDPCM_QUEUE_COUNT; queue++)
    {
        s_sdpcm_queue_previous[queue] = s_sdpcm_queue_stats.current[queue];
        s_sdpcm_queue_stats.high_water[queue] =
            s_sdpcm_queue_stats.current[queue];
        s_sdpcm_queue_stats.enqueued[queue] = 0U;
        s_sdpcm_queue_stats.dequeued[queue] = 0U;
    }
    s_sdpcm_queue_stats.snapshots = 0U;
    rt_hw_interrupt_enable(level);
#endif
}


cy_rslt_t cy_rtos_create_thread(cy_thread_t *thread, cy_thread_entry_fn_t entry_function,
                                const char *name, void *stack, uint32_t stack_size, cy_thread_priority_t priority,
                                cy_thread_arg_t arg)
{
    if ((thread == NULL) || (stack_size < CY_RTOS_MIN_STACK_SIZE))
    {
        return CY_RTOS_BAD_PARAM;
    }
    else
    {
        rt_thread_t thread;

        if ((thread = rt_thread_create(name, entry_function, arg, stack_size, priority, 2)) == RT_NULL)
        {
            return CY_RTOS_NO_MEMORY;
        }

        if (rt_thread_startup(thread) != RT_EOK)
        {
            return CY_RSLT_TYPE_ERROR;
        }
    }

    return CY_RSLT_SUCCESS;
}

/**
 * Exit the current thread
 *
 * @returns WHD_SUCCESS on success or otherwise
 */
cy_rslt_t cy_rtos_exit_thread(void)
{
    return CY_RSLT_SUCCESS;
}

/**
 * Blocks the current thread until the indicated thread is complete
 *
 * @param thread         : handle of the thread to terminate
 *
 * @returns CY_RSLT_SUCCESS on success, error otherwise
 */
cy_rslt_t cy_rtos_join_thread(cy_thread_t *thread)
{
    return CY_RSLT_SUCCESS;
}

/**
 * Creates a semaphore
 *
 * @param semaphore         : pointer to variable which will receive handle of created semaphore
 *
 * @returns CY_RSLT_SUCCESS on success, error otherwise
 */
cy_rslt_t cy_rtos_init_semaphore(cy_semaphore_t *semaphore, uint32_t maxcount, uint32_t initcount)
{
    cy_rslt_t status = CY_RSLT_SUCCESS;

    if (semaphore == NULL)
    {
        status = CY_RTOS_BAD_PARAM;
    }
    else
    {
        *semaphore = rt_sem_create("cyw", initcount, RT_IPC_FLAG_PRIO);
        if (*semaphore == NULL)
        {
            status = CY_RTOS_NO_MEMORY;
        }
    }

    return status;
}

/**
 * Gets a semaphore
 *
 * If value of semaphore is larger than zero, then the semaphore is decremented and function returns
 * Else If value of semaphore is zero, then current thread is suspended until semaphore is set.
 * Value of semaphore should never be below zero
 *
 * Must not be called from interrupt context, since it could block, and since an interrupt is not a
 * normal thread, so could cause RTOS problems if it tries to suspend it.
 * If called from interrupt context time out value should be 0.
 *
 * @param semaphore   : Pointer to variable which will receive handle of created semaphore
 * @param timeout_ms  : Maximum period to block for. Can be passed CY_RTOS_NEVER_TIMEOUT to request no timeout
 * @param will_set_in_isr : True if the semaphore will be set in an ISR. Currently only used for NoOS/NoNS
 *
 */
cy_rslt_t cy_rtos_get_semaphore(cy_semaphore_t *semaphore, uint32_t timeout_ms,
                                bool will_set_in_isr)
{
    /* In rtthread, semaphores cannot be take in interrupts */
    if ((semaphore == NULL) || (will_set_in_isr))
    {
        return CY_RTOS_BAD_PARAM;
    }
    else
    {
        rt_tick_t ticks = rt_tick_from_millisecond(timeout_ms);
        if (rt_sem_take(*semaphore, ticks) != RT_EOK)
        {
            return CY_RTOS_TIMEOUT;
        }
    }

    return CY_RSLT_SUCCESS;
}

/**
 * Sets a semaphore
 *
 * If any threads are waiting on the semaphore, the first thread is resumed
 * Else increment semaphore.
 *
 * Can be called from interrupt context, so must be able to handle resuming other
 * threads from interrupt context.
 *
 * @param semaphore       : Pointer to variable which will receive handle of created semaphore
 * @param called_from_ISR : Value of WHD_TRUE indicates calling from interrupt context
 *                          Value of WHD_FALSE indicates calling from normal thread context
 *
 * @return cy_rslt_t : CY_RSLT_SUCCESS if semaphore was successfully set
 *                        : error if an error occurred
 *
 */
cy_rslt_t cy_rtos_set_semaphore(cy_semaphore_t *semaphore, bool called_from_ISR)
{
    if ((semaphore == NULL))
    {
        return CY_RTOS_BAD_PARAM;
    }
    else
    {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS && \
    !defined(PROTO_MSGBUF)
        if (!called_from_ISR)
        {
            /* The SDPCM queue counters are protected by this semaphore. Take
             * a tiny, ordered snapshot before releasing it; no logging or
             * histogram update runs while the queue lock is held. */
            whd_rtos_sdpcm_queue_watch_snapshot(semaphore);
        }
#else
        RT_UNUSED(called_from_ISR);
#endif
        if (rt_sem_release(*semaphore) != RT_EOK)
        {
            return CY_RTOS_GENERAL_ERROR;
        }
    }

    return CY_RSLT_SUCCESS;
}

/**
 * Deletes a semaphore
 *
 * WHD uses this function to delete a semaphore.
 *
 * @param semaphore         : Pointer to the semaphore handle
 *
 * @return cy_rslt_t : CY_RSLT_SUCCESS if semaphore was successfully deleted
 *                        : error if an error occurred
 *
 */
cy_rslt_t cy_rtos_deinit_semaphore(cy_semaphore_t *semaphore)
{
    if (semaphore == NULL)
    {
        return CY_RTOS_BAD_PARAM;
    }
    else
    {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS && \
    !defined(PROTO_MSGBUF)
        /* WHD calls this after its worker stops using the SDPCM queues. Clear
         * the borrowed pointers before the semaphore/driver can be freed. */
        whd_rtos_sdpcm_queue_watch_unregister_semaphore(semaphore);
#endif
        if (rt_sem_delete(*semaphore) != RT_EOK)
        {
            return CY_RTOS_GENERAL_ERROR;
        }
    }

    return CY_RSLT_SUCCESS;
}

/**
 * Gets time in milliseconds since RTOS start
 *
 * @Note: since this is only 32 bits, it will roll over every 49 days, 17 hours.
 *
 * @returns Time in milliseconds since RTOS started.
 */
cy_rslt_t cy_rtos_get_time(cy_time_t *tval)
{
    *tval = rt_tick_get_millisecond();

    return CY_RSLT_SUCCESS;
}

/**
 * Delay for a number of milliseconds
 *
 * @return cy_rslt_t : CY_RSLT_SUCCESS if delay was successful
 *                        : error if an error occurred
 *
 */
cy_rslt_t cy_rtos_delay_milliseconds(cy_time_t num_ms)
{
    rt_thread_mdelay(num_ms);

    return CY_RSLT_SUCCESS;
}


#ifdef WHD_USE_CUSTOM_MALLOC_IMPL
/* Use the system's built-in memory allocation */
void *whd_mem_malloc (size_t size)
{
    return rt_malloc(size);
}

void *whd_mem_calloc(size_t nitems, size_t size)
{
    return rt_calloc(nitems, size);
}

void whd_mem_free(void *ptr)
{
    rt_free(ptr);
}

#endif /* ifndef WHD_USE_CUSTOM_MALLOC_IMPL */

