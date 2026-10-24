/* SPDX-License-Identifier: Apache-2.0 */
#include "lvgl_isr_repro.h"

#include <finsh.h>

#include "osal/lv_os_private.h"

#define LVGL_ISR_REPRO_WAIT_MS       100
#define LVGL_ISR_REPRO_REPEAT_COUNT  50U
#define LVGL_ISR_REPRO_WAITER_STACK  1024U
#define LVGL_ISR_REPRO_WAITER_PRIO   6U

static lv_thread_sync_t s_sync;
static rt_sem_t s_run_sem;
static rt_sem_t s_started_sem;
static rt_sem_t s_done_sem;
static rt_sem_t s_exit_sem;
static rt_thread_t s_waiter_thread;
static volatile rt_bool_t s_active;
static volatile rt_bool_t s_stop_waiter;
static volatile rt_uint8_t s_armed_signals;
static volatile rt_uint32_t s_isr_signal_count;
static volatile rt_uint32_t s_isr_error_count;
static volatile rt_uint32_t s_isr_context_error_count;
static volatile lv_result_t s_isr_last_result;
static volatile rt_err_t s_wait_result;

/* Exact behavior from LVGL v9.6.0 before the proposed ISR fix. This is a
 * function-level baseline, not a second firmware build or device simulation. */
static lv_result_t lvgl_isr_repro_upstream_baseline(lv_thread_sync_t *sync)
{
    LV_UNUSED(sync);
    return LV_RESULT_INVALID;
}

static void lvgl_isr_repro_waiter(void *parameter)
{
    RT_UNUSED(parameter);
    for (;;)
    {
        if (rt_sem_take(s_run_sem, RT_WAITING_FOREVER) != RT_EOK)
            continue;
        if (s_stop_waiter)
            break;

        (void)rt_sem_release(s_started_sem);
        s_wait_result = rt_sem_take(s_sync.sem,
                                    rt_tick_from_millisecond(LVGL_ISR_REPRO_WAIT_MS));
        (void)rt_sem_release(s_done_sem);
    }
    (void)rt_sem_release(s_exit_sem);
}

void lvgl_isr_repro_systick(void)
{
    if (!s_active || s_armed_signals == 0U)
        return;

    s_armed_signals--;
    s_isr_signal_count++;
    if (rt_interrupt_get_nest() == 0U)
        s_isr_context_error_count++;

    s_isr_last_result = lv_thread_sync_signal_isr(&s_sync);
    if (s_isr_last_result != LV_RESULT_OK)
        s_isr_error_count++;
}

static rt_bool_t lvgl_isr_repro_waiter_begin(void)
{
    s_wait_result = (rt_err_t)-0x7FFF;
    if (rt_sem_release(s_run_sem) != RT_EOK)
        return RT_FALSE;
    return rt_sem_take(s_started_sem,
                       rt_tick_from_millisecond(LVGL_ISR_REPRO_WAIT_MS)) == RT_EOK;
}

static rt_bool_t lvgl_isr_repro_waiter_finish(rt_err_t expected)
{
    if (rt_sem_take(s_done_sem,
                    rt_tick_from_millisecond(LVGL_ISR_REPRO_WAIT_MS + 100)) != RT_EOK)
        return RT_FALSE;
    return s_wait_result == expected;
}

static void lvgl_isr_repro_arm(rt_uint8_t count)
{
    rt_base_t level = rt_hw_interrupt_disable();
    s_armed_signals = count;
    rt_hw_interrupt_enable(level);
}

static rt_bool_t lvgl_isr_repro_init(void)
{
    s_run_sem = rt_sem_create("lvrrun", 0U, RT_IPC_FLAG_FIFO);
    s_started_sem = rt_sem_create("lvrsat", 0U, RT_IPC_FLAG_FIFO);
    s_done_sem = rt_sem_create("lvrdone", 0U, RT_IPC_FLAG_FIFO);
    s_exit_sem = rt_sem_create("lvrxit", 0U, RT_IPC_FLAG_FIFO);
    if (s_run_sem == RT_NULL || s_started_sem == RT_NULL ||
        s_done_sem == RT_NULL || s_exit_sem == RT_NULL)
        return RT_FALSE;

    if (lv_thread_sync_init(&s_sync) != LV_RESULT_OK)
        return RT_FALSE;

    s_waiter_thread = rt_thread_create("lv_isr_wait", lvgl_isr_repro_waiter,
                                       RT_NULL, LVGL_ISR_REPRO_WAITER_STACK,
                                       LVGL_ISR_REPRO_WAITER_PRIO, 10U);
    if (s_waiter_thread == RT_NULL)
        return RT_FALSE;
    if (rt_thread_startup(s_waiter_thread) != RT_EOK)
    {
        (void)rt_thread_delete(s_waiter_thread);
        s_waiter_thread = RT_NULL;
        return RT_FALSE;
    }

    return RT_TRUE;
}

static void lvgl_isr_repro_cleanup(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    s_armed_signals = 0U;
    s_active = RT_FALSE;
    rt_hw_interrupt_enable(level);

    if (s_waiter_thread != RT_NULL)
    {
        s_stop_waiter = RT_TRUE;
        if (s_sync.sem != RT_NULL)
            (void)rt_sem_release(s_sync.sem);
        if (s_run_sem != RT_NULL)
            (void)rt_sem_release(s_run_sem);
        if (s_exit_sem != RT_NULL &&
            rt_sem_take(s_exit_sem,
                        rt_tick_from_millisecond(LVGL_ISR_REPRO_WAIT_MS + 100)) != RT_EOK)
        {
            s_active = RT_TRUE;
            rt_kprintf("[LVGL-ISR] waiter cleanup timeout; sync objects retained\r\n");
            return;
        }
        s_waiter_thread = RT_NULL;
    }

    if (s_sync.sem != RT_NULL)
        (void)lv_thread_sync_delete(&s_sync);
    if (s_run_sem != RT_NULL) { (void)rt_sem_delete(s_run_sem); s_run_sem = RT_NULL; }
    if (s_started_sem != RT_NULL) { (void)rt_sem_delete(s_started_sem); s_started_sem = RT_NULL; }
    if (s_done_sem != RT_NULL) { (void)rt_sem_delete(s_done_sem); s_done_sem = RT_NULL; }
    if (s_exit_sem != RT_NULL) { (void)rt_sem_delete(s_exit_sem); s_exit_sem = RT_NULL; }
    s_stop_waiter = RT_FALSE;
}

static int lvgl_isr_repro_cmd(int argc, char **argv)
{
    rt_bool_t passed = RT_FALSE;
    rt_uint32_t repeat;
    rt_uint32_t before;
    rt_uint32_t coalesced_calls;
    rt_uint32_t coalesced_errors;

    RT_UNUSED(argc);
    RT_UNUSED(argv);
    if (s_active)
    {
        rt_kprintf("[LVGL-ISR] already running\r\n");
        return -RT_EBUSY;
    }
    s_active = RT_TRUE;
    s_isr_signal_count = 0U;
    s_isr_error_count = 0U;
    s_isr_context_error_count = 0U;
    s_isr_last_result = LV_RESULT_INVALID;

    if (!lvgl_isr_repro_init())
    {
        rt_kprintf("[LVGL-ISR] setup failed\r\n");
        goto cleanup;
    }

    /* Baseline: the exact tagged-upstream body returns invalid and leaves the
     * independent waiter to time out; it never claims to be a device failure. */
    if (!lvgl_isr_repro_waiter_begin() ||
        lvgl_isr_repro_upstream_baseline(&s_sync) != LV_RESULT_INVALID ||
        !lvgl_isr_repro_waiter_finish(-RT_ETIMEOUT))
    {
        rt_kprintf("[LVGL-ISR] function-level baseline mismatch\r\n");
        goto cleanup;
    }

    before = s_isr_signal_count;
    if (!lvgl_isr_repro_waiter_begin())
    {
        rt_kprintf("[LVGL-ISR] waiter start failed\r\n");
        goto cleanup;
    }
    lvgl_isr_repro_arm(1U);
    if (!lvgl_isr_repro_waiter_finish(RT_EOK) ||
        s_isr_signal_count != before + 1U ||
        s_isr_last_result != LV_RESULT_OK ||
        s_isr_context_error_count != 0U || s_isr_error_count != 0U)
    {
        rt_kprintf("[LVGL-ISR] ISR wake failed result=%d nest_errors=%lu\r\n",
                   (int)s_isr_last_result,
                   (unsigned long)s_isr_context_error_count);
        goto cleanup;
    }

    /* With a token already pending, a second ISR release must merge into the
     * bounded binary semaphore and still return LV_RESULT_OK. */
    before = s_isr_signal_count;
    coalesced_errors = s_isr_error_count;
    lvgl_isr_repro_arm(2U);
    rt_thread_mdelay(5);
    coalesced_calls = s_isr_signal_count - before;
    if (coalesced_calls != 2U || s_isr_error_count != coalesced_errors ||
        s_isr_context_error_count != 0U ||
        rt_sem_take(s_sync.sem, RT_WAITING_NO) != RT_EOK ||
        rt_sem_take(s_sync.sem, RT_WAITING_NO) != -RT_ETIMEOUT)
    {
        rt_kprintf("[LVGL-ISR] pending-token merge failed calls=%lu errors=%lu\r\n",
                   (unsigned long)coalesced_calls,
                   (unsigned long)s_isr_error_count);
        goto cleanup;
    }

    before = s_isr_signal_count;
    for (repeat = 0U; repeat < LVGL_ISR_REPRO_REPEAT_COUNT; repeat++)
    {
        if (!lvgl_isr_repro_waiter_begin())
            goto repeat_failed;
        lvgl_isr_repro_arm(1U);
        if (!lvgl_isr_repro_waiter_finish(RT_EOK) ||
            s_isr_last_result != LV_RESULT_OK ||
            s_isr_context_error_count != 0U || s_isr_error_count != 0U)
            goto repeat_failed;
    }
    if (s_isr_signal_count - before != LVGL_ISR_REPRO_REPEAT_COUNT)
        goto repeat_failed;

    passed = RT_TRUE;
    rt_kprintf("[LVGL-ISR] baseline timeout=PASS patched wake=PASS nest=ISR\r\n");
    rt_kprintf("[LVGL-ISR] pending merge=PASS signals=2 pending_tokens=1\r\n");
    rt_kprintf("[LVGL-ISR] repeated wake=%lu/%lu error=%lu\r\n",
               (unsigned long)LVGL_ISR_REPRO_REPEAT_COUNT,
               (unsigned long)LVGL_ISR_REPRO_REPEAT_COUNT,
               (unsigned long)s_isr_error_count);
    goto cleanup;

repeat_failed:
    rt_kprintf("[LVGL-ISR] repeated wake failed at %lu\r\n",
               (unsigned long)repeat);

cleanup:
    lvgl_isr_repro_cleanup();
    return passed ? RT_EOK : -RT_ERROR;
}
MSH_CMD_EXPORT(lvgl_isr_repro_cmd, compare upstream baseline and fixed LVGL ISR sync);
