#include "usb_perf.h"

#include <finsh.h>
#include <string.h>
#include "stm32h7rsxx_hal.h"
#include "whd_mongoose_buffer.h"
#include "whd_rtos_diag.h"

#define USB_PERF_BUCKET_COUNT 16U

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
struct usb_perf_histogram
{
    rt_uint32_t buckets[USB_PERF_BUCKET_COUNT];
    rt_uint32_t samples;
    rt_uint32_t sum_us;
    rt_uint32_t max_us;
};

static struct usb_perf_histogram s_histograms[USB_PERF_CATEGORY_COUNT];
static usb_perf_analysis_detail_t s_analysis_detail;
static const char *const s_category_names[USB_PERF_CATEGORY_COUNT] =
{
    "host_cb", "analysis", "event_to_analysis", "analysis_lock_wait",
    "analysis_lock_work", "analysis_snapshot_reset", "analysis_feed_core",
    "analysis_append_raw", "analysis_handle_control", "analysis_thread_run",
    "analysis_lock_scheduled", "analysis_publish",
    "evidence_pub_lock_wait",
    "evidence_prepare", "web_update", "snapshot_copy", "evidence_crc",
    "json_build", "json_lock_wait", "json_crc", "publish_lock",
    "web_pub_lock_wait", "web_pub_lock_hold",
    "http_handler", "lvgl_update", "lvgl_render", "dma2d_broker",
    "dma2d_job", "nema_frame_wait", "wifi_publish", "wifi_scan_copy",
    "whd_tx_get", "whd_tx_copy", "whd_tx_enqueue", "whd_tx_total",
    "sdio_mutex_wait", "sdio_cmd53_read", "sdio_cmd53_write",
    "mongoose_owner", "mg_mgr_poll", "mongoose_web_poll",
    "sdpcm_queue_sem_wait", "whd_thread_notify"
};

static rt_uint32_t usb_perf_cycles_to_us(rt_uint32_t cycles)
{
    rt_uint32_t frequency = SystemCoreClock;
    if (frequency == 0U)
    {
        return 0U;
    }
    return (rt_uint32_t)(((rt_uint64_t)cycles * 1000000ULL +
                          frequency / 2U) / frequency);
}

static rt_uint8_t usb_perf_bucket(rt_uint32_t usec)
{
    rt_uint8_t bucket = 0U;
    rt_uint32_t bound = 1U;

    while (usec > bound && bucket < USB_PERF_BUCKET_COUNT - 1U)
    {
        bound <<= 1U;
        bucket++;
    }
    return bucket;
}

static rt_uint32_t usb_perf_percentile(
    const struct usb_perf_histogram *histogram, rt_uint32_t percentile)
{
    rt_uint32_t target;
    rt_uint32_t cumulative = 0U;
    rt_uint8_t bucket;

    if (histogram->samples == 0U)
    {
        return 0U;
    }
    target = (histogram->samples * percentile + 99U) / 100U;
    for (bucket = 0U; bucket < USB_PERF_BUCKET_COUNT; bucket++)
    {
        cumulative += histogram->buckets[bucket];
        if (cumulative >= target)
        {
            return bucket == USB_PERF_BUCKET_COUNT - 1U ? 65535U :
                   (1U << bucket);
        }
    }
    return 65535U;
}
#endif

rt_uint32_t usb_perf_now(void)
{
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    /* Read-only: rt_hw_board_init() owns the shared counter. */
    return (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U ? DWT->CYCCNT : 0U;
#else
    return 0U;
#endif
}

void usb_perf_record_us(usb_perf_category_t category, rt_uint32_t duration_us)
{
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    rt_uint8_t bucket;
    rt_base_t level;
    struct usb_perf_histogram *histogram;

    if ((rt_uint32_t)category >= USB_PERF_CATEGORY_COUNT)
    {
        return;
    }
    bucket = usb_perf_bucket(duration_us);
    histogram = &s_histograms[category];
    level = rt_hw_interrupt_disable();
    histogram->buckets[bucket]++;
    histogram->samples++;
    histogram->sum_us += duration_us;
    if (duration_us > histogram->max_us)
    {
        histogram->max_us = duration_us;
    }
    rt_hw_interrupt_enable(level);
#else
    RT_UNUSED(category);
    RT_UNUSED(duration_us);
#endif
}

void usb_perf_record(usb_perf_category_t category,
                     rt_uint32_t start_cycles,
                     rt_uint32_t end_cycles)
{
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U ||
        SystemCoreClock == 0U)
    {
        return;
    }
    usb_perf_record_us(category,
                       usb_perf_cycles_to_us(end_cycles - start_cycles));
#else
    RT_UNUSED(category);
    RT_UNUSED(start_cycles);
    RT_UNUSED(end_cycles);
#endif
}

void usb_perf_record_analysis_detail(rt_uint32_t sequence,
                                    rt_uint32_t event_type,
                                    rt_uint32_t event_to_analysis_us,
                                    rt_uint32_t lock_wait_cycles,
                                    rt_uint32_t lock_work_cycles,
                                    rt_uint32_t publish_cycles,
                                    rt_uint32_t total_cycles,
                                    rt_uint32_t captured_length,
                                    rt_uint32_t snapshot_reset_cycles,
                                    rt_uint32_t feed_core_cycles,
                                    rt_uint32_t append_raw_cycles,
                                    rt_uint32_t handle_control_cycles,
                                    rt_uint32_t analysis_thread_run_cycles,
                                    rt_bool_t analysis_runtime_valid,
                                    rt_uint32_t lock_scheduled_cycles,
                                    rt_bool_t lock_runtime_valid)
{
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U ||
        SystemCoreClock == 0U)
    {
        return;
    }
    rt_base_t level;
    rt_uint32_t lock_wait_us = usb_perf_cycles_to_us(lock_wait_cycles);
    rt_uint32_t lock_work_us = usb_perf_cycles_to_us(lock_work_cycles);
    rt_uint32_t publish_us = usb_perf_cycles_to_us(publish_cycles);
    rt_uint32_t total_us = usb_perf_cycles_to_us(total_cycles);
    rt_uint32_t snapshot_reset_us =
        usb_perf_cycles_to_us(snapshot_reset_cycles);
    rt_uint32_t feed_core_us = usb_perf_cycles_to_us(feed_core_cycles);
    rt_uint32_t append_raw_us = usb_perf_cycles_to_us(append_raw_cycles);
    rt_uint32_t handle_control_us =
        usb_perf_cycles_to_us(handle_control_cycles);
    rt_uint32_t analysis_thread_run_us =
        usb_perf_cycles_to_us(analysis_thread_run_cycles);
    rt_uint32_t lock_scheduled_us =
        usb_perf_cycles_to_us(lock_scheduled_cycles);
    rt_uint32_t off_thread_cycles =
        lock_work_cycles > lock_scheduled_cycles ?
        lock_work_cycles - lock_scheduled_cycles : 0U;
    rt_uint32_t lock_off_thread_us = usb_perf_cycles_to_us(off_thread_cycles);
    rt_uint32_t measured_lock_cycles = snapshot_reset_cycles +
        feed_core_cycles + append_raw_cycles + handle_control_cycles;
    rt_uint32_t other_lock_cycles = lock_work_cycles > measured_lock_cycles ?
        lock_work_cycles - measured_lock_cycles : 0U;
    rt_uint32_t other_lock_us = usb_perf_cycles_to_us(other_lock_cycles);

    level = rt_hw_interrupt_disable();
    if (!s_analysis_detail.valid ||
        total_cycles > s_analysis_detail.total_cycles)
    {
        s_analysis_detail.valid = 1U;
        s_analysis_detail.sequence = sequence;
        s_analysis_detail.event_type = event_type;
        s_analysis_detail.event_to_analysis_us = event_to_analysis_us;
        s_analysis_detail.lock_wait_us = lock_wait_us;
        s_analysis_detail.lock_work_us = lock_work_us;
        s_analysis_detail.publish_us = publish_us;
        s_analysis_detail.total_us = total_us;
        s_analysis_detail.captured_length = captured_length;
        s_analysis_detail.snapshot_reset_us = snapshot_reset_us;
        s_analysis_detail.feed_core_us = feed_core_us;
        s_analysis_detail.append_raw_us = append_raw_us;
        s_analysis_detail.handle_control_us = handle_control_us;
        s_analysis_detail.other_lock_us = other_lock_us;
        s_analysis_detail.analysis_thread_run_us = analysis_thread_run_us;
        s_analysis_detail.analysis_runtime_valid =
            analysis_runtime_valid ? 1U : 0U;
        s_analysis_detail.lock_scheduled_us = lock_scheduled_us;
        s_analysis_detail.lock_off_thread_us = lock_off_thread_us;
        s_analysis_detail.lock_runtime_valid =
            lock_runtime_valid ? 1U : 0U;
        s_analysis_detail.total_cycles = total_cycles;
    }
    rt_hw_interrupt_enable(level);
#else
    RT_UNUSED(sequence);
    RT_UNUSED(event_type);
    RT_UNUSED(event_to_analysis_us);
    RT_UNUSED(lock_wait_cycles);
    RT_UNUSED(lock_work_cycles);
    RT_UNUSED(publish_cycles);
    RT_UNUSED(total_cycles);
    RT_UNUSED(captured_length);
    RT_UNUSED(snapshot_reset_cycles);
    RT_UNUSED(feed_core_cycles);
    RT_UNUSED(append_raw_cycles);
    RT_UNUSED(handle_control_cycles);
    RT_UNUSED(analysis_thread_run_cycles);
    RT_UNUSED(analysis_runtime_valid);
    RT_UNUSED(lock_scheduled_cycles);
    RT_UNUSED(lock_runtime_valid);
#endif
}

void usb_perf_get_analysis_detail(usb_perf_analysis_detail_t *detail)
{
    if (!detail)
    {
        return;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    {
        rt_base_t level = rt_hw_interrupt_disable();
        *detail = s_analysis_detail;
        rt_hw_interrupt_enable(level);
    }
#else
    rt_memset(detail, 0, sizeof(*detail));
#endif
}

void usb_perf_reset(void)
{
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    rt_base_t level = rt_hw_interrupt_disable();
    rt_memset(s_histograms, 0, sizeof(s_histograms));
    rt_memset(&s_analysis_detail, 0, sizeof(s_analysis_detail));
    rt_hw_interrupt_enable(level);
#endif
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static void evidence_perf_cmd(int argc, char **argv)
{
    rt_uint8_t category;

    if (argc == 2 && rt_strcmp(argv[1], "reset") == 0)
    {
        usb_perf_reset();
        whd_mongoose_buffer_reset_stats();
        whd_rtos_sdpcm_queue_stats_reset();
        rt_kprintf("[evidence_perf] counters reset\r\n");
        return;
    }
    if (argc != 1)
    {
        rt_kprintf("Usage: evidence_perf [reset]\r\n");
        return;
    }
    rt_kprintf("[evidence_perf] DWT categories in us; event_to_analysis "
               "uses evidence timestamps; bucket percentiles are upper bounds\r\n");
    for (category = 0U; category < USB_PERF_CATEGORY_COUNT; category++)
    {
        const struct usb_perf_histogram *histogram =
            &s_histograms[category];
        if (histogram->samples == 0U)
        {
            continue;
        }
        rt_kprintf("%s n=%lu avg=%lu p50<=%lu p95<=%lu max=%lu us\r\n",
                   s_category_names[category],
                   (unsigned long)histogram->samples,
                   (unsigned long)(histogram->sum_us / histogram->samples),
                   (unsigned long)usb_perf_percentile(histogram, 50U),
                   (unsigned long)usb_perf_percentile(histogram, 95U),
                   (unsigned long)histogram->max_us);
    }
    {
        whd_mongoose_buffer_stats_t stats;

        whd_mongoose_buffer_get_stats(&stats);
        rt_kprintf("[whd_pool] shared32 current=%lu high=%lu hit_tx=%lu hit_rx=%lu\r\n",
                   (unsigned long)stats.current,
                   (unsigned long)stats.high_water,
                   (unsigned long)stats.tx_hits,
                   (unsigned long)stats.rx_hits);
        rt_kprintf("[whd_pool] malloc_tx=%lu/%lu fail=%lu full=%lu oversize=%lu\r\n",
                   (unsigned long)stats.tx_malloc_attempts,
                   (unsigned long)stats.tx_malloc_successes,
                   (unsigned long)stats.tx_malloc_failures,
                   (unsigned long)stats.tx_fallback_pool_full,
                   (unsigned long)stats.tx_fallback_oversize);
        rt_kprintf("[whd_pool] malloc_rx=%lu/%lu fail=%lu full=%lu oversize=%lu\r\n",
                   (unsigned long)stats.rx_malloc_attempts,
                   (unsigned long)stats.rx_malloc_successes,
                   (unsigned long)stats.rx_malloc_failures,
                   (unsigned long)stats.rx_fallback_pool_full,
                   (unsigned long)stats.rx_fallback_oversize);
        rt_kprintf("[whd_pool] reject_badarg=%lu reject_capacity_tx=%lu rx=%lu\r\n",
                   (unsigned long)stats.invalid_arguments,
                   (unsigned long)stats.tx_capacity_rejects,
                   (unsigned long)stats.rx_capacity_rejects);
    }
    {
        whd_rtos_sdpcm_queue_stats_t stats;
        unsigned int queue;

        whd_rtos_sdpcm_queue_stats_get(&stats);
        rt_kprintf("[sdpcm_queue] watching=%u snapshots=%lu\r\n",
                   stats.watching ? 1U : 0U,
                   (unsigned long)stats.snapshots);
        for (queue = 0U; queue < WHD_RTOS_SDPCM_QUEUE_COUNT; queue++)
        {
            rt_kprintf("[sdpcm_queue] q%u current=%lu high=%lu enq=%lu deq=%lu\r\n",
                       queue,
                       (unsigned long)stats.current[queue],
                       (unsigned long)stats.high_water[queue],
                       (unsigned long)stats.enqueued[queue],
                       (unsigned long)stats.dequeued[queue]);
        }
    }
    {
        usb_perf_analysis_detail_t detail;
        usb_perf_get_analysis_detail(&detail);
        if (detail.valid)
        {
            rt_kprintf("[analysis_max] seq=%lu type=%lu total=%lu age=%lu us\r\n",
                       (unsigned long)detail.sequence,
                       (unsigned long)detail.event_type,
                       (unsigned long)detail.total_us,
                       (unsigned long)detail.event_to_analysis_us);
            rt_kprintf("[analysis_max] wait=%lu work=%lu publish=%lu us\r\n",
                       (unsigned long)detail.lock_wait_us,
                       (unsigned long)detail.lock_work_us,
                       (unsigned long)detail.publish_us);
            rt_kprintf("[analysis_stages] captured=%lu reset=%lu feed=%lu "
                       "append=%lu control=%lu other=%lu us "
                       "thread_valid=%lu thread_run=%lu us "
                       "lock_valid=%lu lock_run=%lu off_thread=%lu us\r\n",
                       (unsigned long)detail.captured_length,
                       (unsigned long)detail.snapshot_reset_us,
                       (unsigned long)detail.feed_core_us,
                       (unsigned long)detail.append_raw_us,
                       (unsigned long)detail.handle_control_us,
                       (unsigned long)detail.other_lock_us,
                       (unsigned long)detail.analysis_runtime_valid,
                       (unsigned long)detail.analysis_thread_run_us,
                       (unsigned long)detail.lock_runtime_valid,
                       (unsigned long)detail.lock_scheduled_us,
                       (unsigned long)detail.lock_off_thread_us);
        }
    }
}
MSH_CMD_EXPORT(evidence_perf_cmd, show/reset evidence path latency histograms);
#endif
