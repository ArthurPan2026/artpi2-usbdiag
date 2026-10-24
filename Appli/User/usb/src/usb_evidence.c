/*
 * USB enumeration evidence pipeline.
 *
 * The producer side is deliberately bounded and non-blocking.  Descriptor
 * parsing runs in this dedicated analysis thread.  Presentation threads only
 * consume the frozen snapshot published after analysis.
 */
#include <rtthread.h>
#include <string.h>
#include "stm32h7rsxx_hal.h"
#include "usbh_core.h"
#include "usb_errno.h"
#include "usb_desc_parser.h"
#include "usb_descriptor_store.h"
#include "usb_evidence.h"
#include "web_snapshot.h"
#include "web_pdf_report.h"
#include "usb_perf.h"
#include "usbdiag_core.h"
#include "usbdiag_session.h"
#include "mongoose.h"
#include "watchdog_supervisor.h"
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#include <finsh.h>
#endif
#define USB_EVIDENCE_STACK_SIZE       6144U
#define USB_EVIDENCE_PRIORITY          6U
#define USB_EVIDENCE_TICK             10U
#define USB_EVIDENCE_WEB_PUBLISH_MS   100U
enum usb_rule_index
{
    USB_RULE_DEV_B_LENGTH,
    USB_RULE_DEV_TYPE,
    USB_RULE_DEV_8B_PREFIX,
    USB_RULE_DEV_FIELDS,
    USB_RULE_CFG_B_LENGTH,
    USB_RULE_CFG_TOTAL_LENGTH,
    USB_RULE_CFG_TREE_WALK,
    USB_RULE_CFG_INTERFACE_COUNT,
    USB_RULE_IF_ENDPOINT_COUNT,
    USB_RULE_EP_PACKET_SIZE,
    USB_RULE_EP_INTERVAL,
    USB_RULE_EP0_STALL,
};

struct usb_rule_definition
{
    const char *id;
    const char *clause;
    const char *expected;
    usb_rule_severity_t severity;
};

struct usb_analysis_context
{
    rt_uint8_t manufacturer_index;
    rt_uint8_t product_index;
    rt_uint8_t serial_index;
    char manufacturer[32];
    char product[32];
    char serial[128];
    char class_name[32];
};

struct usb_interface_group
{
    rt_uint8_t number;
    rt_uint8_t alternate_setting;
    rt_uint8_t declared_endpoints;
    rt_uint8_t observed_endpoints;
    rt_uint16_t declared_endpoint_offset;
};

static const struct usb_rule_definition g_rule_definitions[USB_EVIDENCE_RULE_COUNT] =
{
    { "DEV_B_LENGTH", "USB 2.0 9.6.1", "bLength is 18", USB_RULE_SEVERITY_ERROR },
    { "DEV_TYPE", "USB 2.0 9.4.3, 9.6.1", "response type matches Device request", USB_RULE_SEVERITY_ERROR },
    { "DEV_8B_PREFIX", "USB 2.0 9.4.3, 9.6.1", "first 8 bytes equal the full descriptor prefix", USB_RULE_SEVERITY_ERROR },
    { "DEV_FIELDS", "USB 2.0 9.6.1", "bMaxPacketSize0 is valid for the observed speed", USB_RULE_SEVERITY_ERROR },
    { "CFG_B_LENGTH", "USB 2.0 9.6.3", "Configuration descriptor bLength is 9 and type is 2", USB_RULE_SEVERITY_ERROR },
    { "CFG_TOTAL_LENGTH", "USB 2.0 9.4.3, 9.6.3", "Full configuration request returns exactly wTotalLength bytes", USB_RULE_SEVERITY_ERROR },
    { "CFG_TREE_WALK", "USB 2.0 9.6.3-9.6.6", "every descriptor is bounded and walkable", USB_RULE_SEVERITY_ERROR },
    { "CFG_INTERFACE_COUNT", "USB 2.0 9.6.3", "bNumInterfaces matches unique interfaces", USB_RULE_SEVERITY_ERROR },
    { "IF_ENDPOINT_COUNT", "USB 2.0 9.6.5", "bNumEndpoints matches each alternate setting", USB_RULE_SEVERITY_ERROR },
    { "EP_PACKET_SIZE", "USB 2.0 9.6.6, Table 9-13", "wMaxPacketSize is valid for speed and type", USB_RULE_SEVERITY_ERROR },
    { "EP_INTERVAL", "USB 2.0 9.6.6", "bInterval is valid for speed and type", USB_RULE_SEVERITY_ERROR },
    { "EP0_STALL", "USB 2.0 9.4.3, 9.4.7", "required enumeration requests do not STALL", USB_RULE_SEVERITY_ERROR },
};

static struct rt_messagequeue g_event_queue_object;
static rt_mq_t g_event_queue;
static rt_uint8_t g_event_pool[
    RT_MQ_BUF_SIZE(sizeof(usb_evidence_raw_event_t),
                   USB_EVIDENCE_EVENT_CAPACITY)]
    USB_EVIDENCE_RAM_SECTION;
static rt_thread_t g_analysis_thread;
static rt_uint32_t s_watchdog_usb_progress;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static volatile rt_bool_t s_analysis_profile_active;
static volatile rt_bool_t s_analysis_thread_scheduled;
static volatile rt_uint32_t s_analysis_profile_epoch;
static volatile rt_uint32_t s_analysis_thread_last_cycle;
static volatile rt_uint64_t s_analysis_thread_cycles;
#endif
static rt_mutex_t g_snapshot_lock;
static rt_mutex_t g_producer_lock;
static usb_evidence_snapshot_t g_snapshot USB_EVIDENCE_RAM_SECTION;
/* The observer is normally called from CherryUSB's hub thread.  Keep the
 * large producer record out of that thread's stack, and serialize it for
 * future class-driver callers as well. */
static usb_evidence_raw_event_t g_producer_event USB_EVIDENCE_RAM_SECTION;
/* The analysis thread also receives a complete event.  Keeping this object
 * static prevents an 8-KB control payload from overflowing its RT-Thread
 * stack. */
static usb_evidence_raw_event_t g_analysis_event USB_EVIDENCE_RAM_SECTION;
static struct usb_analysis_context g_context;
static usbdiag_session_t g_rule_session;
static rt_bool_t g_rule_session_active;
static volatile rt_uint32_t g_next_sequence;
static volatile rt_uint32_t g_current_session_id;
static volatile rt_uint32_t g_current_attempt_id;
static volatile rt_uint32_t g_session_drop_owner;
static volatile rt_uint32_t g_dropped_event_count;
static volatile rt_uint32_t g_dropped_payload_bytes;
static volatile rt_uint32_t g_session_dropped_event_count;
static volatile rt_uint32_t g_session_dropped_payload_bytes;
static volatile rt_uint8_t g_current_speed = USB_SPEED_FULL;
static rt_uint32_t g_boot_id;
static rt_uint32_t g_next_snapshot_id;
static rt_uint32_t g_cycles_per_us;
static rt_uint32_t g_timestamp_last_cycle;
static rt_uint32_t g_timestamp_last_tick;
static rt_uint64_t g_timestamp_us_accum;
static rt_bool_t g_timestamp_ready;
static rt_tick_t g_last_web_publish_tick;
static rt_uint32_t g_last_web_publish_revision;
static rt_uint32_t g_last_web_publish_drop_count;
static rt_uint32_t g_last_web_publish_drop_bytes;
static rt_bool_t g_pdf_pregen_pending;
static void usb_evidence_project_core_results(void);
static rt_bool_t usb_evidence_sync_live_session_losses(void);
static void usb_evidence_live_session_losses(rt_uint32_t *session_id,
                                             rt_uint32_t *dropped_events,
                                             rt_uint32_t *dropped_bytes);
static void usb_evidence_process_event(const usb_evidence_raw_event_t *event);

static void usb_evidence_try_schedule_pdf_pregen(void)
{
    web_snapshot_manifest_t snapshot_manifest;
    usb_descriptor_manifest_t descriptor_manifest;

    if (!g_pdf_pregen_pending || !g_snapshot.is_final ||
        !g_snapshot.enumeration_complete || !g_snapshot.device_present)
    {
        return;
    }
    if (web_snapshot_get_manifest(&snapshot_manifest) != RT_EOK ||
        !snapshot_manifest.is_final ||
        !usb_descriptor_store_get_manifest(&descriptor_manifest) ||
        !descriptor_manifest.present ||
        !descriptor_manifest.enumeration_complete ||
        descriptor_manifest.generation == 0U)
    {
        return;
    }
    if (web_pdf_report_pre_generate(snapshot_manifest.token,
                                    descriptor_manifest.generation) == RT_EOK)
    {
        g_pdf_pregen_pending = RT_FALSE;
    }
}

static rt_uint16_t usb_read_le16(const rt_uint8_t *data)
{
    return (rt_uint16_t)data[0] | ((rt_uint16_t)data[1] << 8);
}

static rt_uint32_t usb_evidence_timestamp_us(void)
{
    rt_uint32_t cycle_now;
    rt_uint32_t tick_now;
    rt_uint32_t tick_delta;
    rt_uint32_t cycle_delta;
    rt_uint64_t elapsed_us;
    rt_base_t level;

    /*
     * CYCCNT wraps in seconds on a fast Cortex-M7.  Keep a software
     * accumulator and use the RT-Thread tick delta to account for gaps longer
     * than one CYCCNT period.  The published field remains a bounded uint32_t
     * for wire compatibility; sequence numbers disambiguate a later wrap.
     */
    level = rt_hw_interrupt_disable();
    cycle_now = g_cycles_per_us != 0U ? DWT->CYCCNT : 0U;
    tick_now = (rt_uint32_t)rt_tick_get();

    if (!g_timestamp_ready)
    {
        g_timestamp_last_cycle = cycle_now;
        g_timestamp_last_tick = tick_now;
        g_timestamp_ready = RT_TRUE;
        rt_hw_interrupt_enable(level);
        return 0U;
    }

    tick_delta = tick_now - g_timestamp_last_tick;
    cycle_delta = cycle_now - g_timestamp_last_cycle;
    if (tick_delta == 0U && g_cycles_per_us != 0U)
    {
        elapsed_us = (rt_uint64_t)cycle_delta / g_cycles_per_us;
    }
    else
    {
        elapsed_us = ((rt_uint64_t)tick_delta * 1000000ULL) /
                     RT_TICK_PER_SECOND;
    }
    g_timestamp_us_accum += elapsed_us;
    g_timestamp_last_cycle = cycle_now;
    g_timestamp_last_tick = tick_now;
    rt_hw_interrupt_enable(level);

    return (rt_uint32_t)g_timestamp_us_accum;
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
/* Read the evidence clock without changing the shared timestamp accumulator.
 * Profiling a queued event must not shift later wire timestamps or CRCs. */
static rt_uint32_t usb_evidence_timestamp_us_peek(void)
{
    rt_uint32_t cycle_now;
    rt_uint32_t tick_now;
    rt_uint32_t tick_delta;
    rt_uint32_t cycle_delta;
    rt_uint64_t elapsed_us;
    rt_uint64_t current_us;
    rt_base_t level = rt_hw_interrupt_disable();

    if (!g_timestamp_ready)
    {
        rt_hw_interrupt_enable(level);
        return 0U;
    }
    cycle_now = g_cycles_per_us != 0U ? DWT->CYCCNT : 0U;
    tick_now = (rt_uint32_t)rt_tick_get();
    tick_delta = tick_now - g_timestamp_last_tick;
    cycle_delta = cycle_now - g_timestamp_last_cycle;
    if (tick_delta == 0U && g_cycles_per_us != 0U)
    {
        elapsed_us = (rt_uint64_t)cycle_delta / g_cycles_per_us;
    }
    else
    {
        elapsed_us = ((rt_uint64_t)tick_delta * 1000000ULL) /
                     RT_TICK_PER_SECOND;
    }
    current_us = g_timestamp_us_accum + elapsed_us;
    rt_hw_interrupt_enable(level);
    return (rt_uint32_t)current_us;
}
#endif

rt_bool_t usb_evidence_clock_us(rt_uint32_t *timestamp_us)
{
    if (!timestamp_us || !g_timestamp_ready)
    {
        return RT_FALSE;
    }
    *timestamp_us = usb_evidence_timestamp_us();
    return RT_TRUE;
}

static void usb_evidence_timer_init(void)
{
    /* Board startup owns CYCCNT initialization. Preserve the global counter;
     * if it is unavailable, timestamp_us falls back to RT-Thread ticks. */
    g_cycles_per_us =
        (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U &&
        SystemCoreClock >= 1000000U ?
        SystemCoreClock / 1000000U : 0U;
    g_timestamp_last_cycle = g_cycles_per_us != 0U ? DWT->CYCCNT : 0U;
    g_timestamp_last_tick = (rt_uint32_t)rt_tick_get();
    g_timestamp_us_accum = 0U;
    g_timestamp_ready = RT_TRUE;
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
void usb_evidence_scheduler_profile_enable(rt_bool_t enabled)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (!enabled ||
        (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U ||
        SystemCoreClock == 0U)
    {
        s_analysis_profile_active = RT_FALSE;
        s_analysis_thread_scheduled = RT_FALSE;
        s_analysis_profile_epoch++;
        rt_hw_interrupt_enable(level);
        return;
    }

    s_analysis_thread_cycles = 0U;
    s_analysis_thread_last_cycle = 0U;
    s_analysis_thread_scheduled = RT_FALSE;
    s_analysis_profile_epoch++;
    s_analysis_profile_active = RT_TRUE;
    rt_hw_interrupt_enable(level);
}

void usb_evidence_scheduler_profile_switch(rt_thread_t from,
                                           rt_thread_t to,
                                           rt_uint32_t cycle)
{
    /* RT-Thread calls its scheduler hook with interrupts disabled. Keep this
     * hook path to pointer checks and cycle accumulation only. */
    if (!s_analysis_profile_active || g_analysis_thread == RT_NULL)
    {
        return;
    }

    if (from == g_analysis_thread && s_analysis_thread_scheduled)
    {
        s_analysis_thread_cycles +=
            (rt_uint32_t)(cycle - s_analysis_thread_last_cycle);
        s_analysis_thread_scheduled = RT_FALSE;
    }
    if (to == g_analysis_thread)
    {
        s_analysis_thread_last_cycle = cycle;
        s_analysis_thread_scheduled = RT_TRUE;
    }
}

static rt_bool_t usb_evidence_scheduler_profile_snapshot(
    rt_uint64_t *cycles, rt_uint32_t *epoch)
{
    rt_base_t level;
    rt_bool_t valid = RT_FALSE;

    if (!cycles || !epoch)
    {
        return RT_FALSE;
    }

    level = rt_hw_interrupt_disable();
    if (s_analysis_profile_active && g_analysis_thread != RT_NULL &&
        (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U &&
        SystemCoreClock != 0U)
    {
        rt_uint64_t total = s_analysis_thread_cycles;

        if (s_analysis_thread_scheduled &&
            rt_thread_self() == g_analysis_thread)
        {
            total += (rt_uint32_t)(DWT->CYCCNT -
                                   s_analysis_thread_last_cycle);
        }
        *cycles = total;
        *epoch = s_analysis_profile_epoch;
        valid = RT_TRUE;
    }
    rt_hw_interrupt_enable(level);
    return valid;
}
#endif

#include "text_preview.h"
static void usb_evidence_copy_text(char *destination,
                                   rt_size_t destination_size,
                                   const char *source)
{
    if (!destination || destination_size == 0U)
    {
        return;
    }
    if (!source)
    {
        destination[0] = '\0';
        return;
    }

    (void)work_text_preview_copy(destination, destination_size, source, 1);
}

typedef rt_uint32_t (*usb_evidence_crc32_update_fn_t)(
    rt_uint32_t crc,const void *data,rt_size_t length);

static rt_uint32_t usb_evidence_crc32_update_mongoose(rt_uint32_t crc,
                                                       const void *data,
                                                       rt_size_t length)
{
    return ~mg_crc32(~crc,(const char *)data,(size_t)length);
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static rt_uint32_t usb_evidence_crc32_update_bitwise(rt_uint32_t crc,
                                                      const void *data,
                                                      rt_size_t length)
{
    const rt_uint8_t *cursor=(const rt_uint8_t *)data;
    rt_size_t index;
    for(index=0U;index<length;index++) {
        rt_uint8_t bit;
        crc^=cursor[index];
        for(bit=0U;bit<8U;bit++)
            crc=(crc>>1U)^((crc&1U)!=0U?0xEDB88320U:0U);
    }
    return crc;
}
#endif

rt_uint32_t usb_evidence_bytes_crc32(const void *data, rt_size_t length)
{
    return mg_crc32(0U, (const char *)data, (size_t)length);
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#define USB_EVIDENCE_CRC_BENCH_MAX_BYTES 8192U
#define USB_EVIDENCE_CRC_BENCH_WORK_BYTES 2048U
struct usb_evidence_crc_bench_entry
{
    rt_uint16_t offset;
    rt_uint16_t length;
    rt_uint32_t expected_crc;
};
static rt_uint8_t s_usb_evidence_crc_bench_payloads[
    USB_EVIDENCE_CRC_BENCH_MAX_BYTES];
static struct usb_evidence_crc_bench_entry s_usb_evidence_crc_bench_entries[
    USB_EVIDENCE_SNAPSHOT_EVENT_COUNT];

static rt_uint32_t usb_evidence_bytes_crc32_bitwise_oracle(
    const void *data,rt_size_t length) __attribute__((noinline));

static rt_uint32_t usb_evidence_bytes_crc32_bitwise_oracle(
    const void *data,rt_size_t length)
{
    return usb_evidence_crc32_update_bitwise(0xFFFFFFFFU,data,length)^
           0xFFFFFFFFU;
}

static rt_uint32_t usb_evidence_crc_bench_median(rt_uint32_t samples[7])
{
    rt_uint32_t i;
    for(i=1U;i<7U;i++) {
        rt_uint32_t value=samples[i],position=i;
        while(position>0U&&samples[position-1U]>value) {
            samples[position]=samples[position-1U];position--;
        }
        samples[position]=value;
    }
    return samples[3];
}

static void usb_evidence_bytes_crc_bench_cmd(void)
{
    static const rt_uint8_t vector[]="123456789";
    static volatile rt_uint32_t sink;
    rt_uint32_t old_cycles[7],new_cycles[7];
    rt_uint32_t event_count=0U,total_bytes=0U,max_length=0U,mismatches=0U;
    rt_uint32_t iterations=1U,old_median,new_median,saved_permille;
    rt_uint32_t sample,index;
    rt_uint32_t total_calls;

    if(usb_evidence_bytes_crc32(vector,sizeof(vector)-1U)!=0xCBF43926U||
       usb_evidence_bytes_crc32_bitwise_oracle(vector,sizeof(vector)-1U)!=
           0xCBF43926U) {
        rt_kprintf("[usb_crc_bench] FAIL standard vector\r\n");
        return;
    }
    if(!g_snapshot_lock||
       rt_mutex_take(g_snapshot_lock,RT_WAITING_NO)!=RT_EOK) {
        rt_kprintf("[usb_crc_bench] FAIL snapshot busy\r\n");
        return;
    }
    if(g_snapshot.raw_event_count>USB_EVIDENCE_SNAPSHOT_EVENT_COUNT) {
        rt_mutex_release(g_snapshot_lock);
        rt_kprintf("[usb_crc_bench] FAIL invalid event count\r\n");
        return;
    }
    for(index=0U;index<g_snapshot.raw_event_count;index++) {
        const usb_evidence_raw_event_t *event=&g_snapshot.raw_events[index];
        struct usb_evidence_crc_bench_entry *entry;
        if(event->captured_length==0U)continue;
        if(event->captured_length>USB_EVIDENCE_CAPTURE_MAX) {
            mismatches++;break;
        }
        if(total_bytes+event->captured_length>
           USB_EVIDENCE_CRC_BENCH_MAX_BYTES) {
            mismatches++;break;
        }
        entry=&s_usb_evidence_crc_bench_entries[event_count];
        entry->offset=(rt_uint16_t)total_bytes;
        entry->length=event->captured_length;
        entry->expected_crc=event->payload_crc32;
        rt_memcpy(&s_usb_evidence_crc_bench_payloads[total_bytes],
                  event->raw_response,event->captured_length);
        event_count++;
        total_bytes+=event->captured_length;
        if(event->captured_length>max_length)max_length=event->captured_length;
    }
    rt_mutex_release(g_snapshot_lock);
    if(event_count==0U) {
        rt_kprintf("[usb_crc_bench] FAIL no captured payloads\r\n");
        return;
    }
    if(mismatches!=0U) {
        rt_kprintf("[usb_crc_bench] FAIL invalid or oversized payload set\r\n");
        return;
    }
    for(index=0U;index<event_count;index++) {
        const struct usb_evidence_crc_bench_entry *entry=
            &s_usb_evidence_crc_bench_entries[index];
        const rt_uint8_t *payload=&s_usb_evidence_crc_bench_payloads[entry->offset];
        rt_uint32_t old_crc=usb_evidence_bytes_crc32_bitwise_oracle(
            payload,entry->length);
        rt_uint32_t new_crc=mg_crc32(0U,(const char *)payload,entry->length);
        if(old_crc!=new_crc||new_crc!=entry->expected_crc)mismatches++;
    }
    if(mismatches!=0U) {
        rt_kprintf("[usb_crc_bench] FAIL captured CRC mismatch count=%lu\r\n",
                   (unsigned long)mismatches);
        return;
    }
    iterations=USB_EVIDENCE_CRC_BENCH_WORK_BYTES/total_bytes;
    if(iterations==0U)iterations=1U;

    /* Warm both implementations on every captured payload before timing. */
    for(index=0U;index<event_count;index++) {
        const struct usb_evidence_crc_bench_entry *entry=
            &s_usb_evidence_crc_bench_entries[index];
        const rt_uint8_t *payload=&s_usb_evidence_crc_bench_payloads[entry->offset];
        sink^=usb_evidence_bytes_crc32_bitwise_oracle(
            payload,entry->length);
        sink^=mg_crc32(0U,(const char *)payload,entry->length);
    }
    for(sample=0U;sample<7U;sample++) {
        rt_uint32_t start,elapsed;
        bool measure_old=(sample&1U)==0U;
        for(rt_uint32_t pass=0U;pass<2U;pass++) {
            bool run_old=(pass==0U)?measure_old:!measure_old;
            start=usb_perf_now();
            for(index=0U;index<event_count;index++) {
                const struct usb_evidence_crc_bench_entry *entry=
                    &s_usb_evidence_crc_bench_entries[index];
                const rt_uint8_t *payload=&s_usb_evidence_crc_bench_payloads[entry->offset];
                rt_uint32_t repeat;
                for(repeat=0U;repeat<iterations;repeat++) {
                    sink^=run_old?
                        usb_evidence_bytes_crc32_bitwise_oracle(
                            payload,entry->length):
                        mg_crc32(0U,(const char *)payload,entry->length);
                }
            }
            elapsed=usb_perf_now()-start;
            if(run_old)old_cycles[sample]=elapsed;
            else new_cycles[sample]=elapsed;
        }
    }
    old_median=usb_evidence_crc_bench_median(old_cycles);
    new_median=usb_evidence_crc_bench_median(new_cycles);
    total_calls=event_count*iterations;
    saved_permille=old_median==0U?0U:
        (old_median>new_median?
            (rt_uint32_t)(((rt_uint64_t)(old_median-new_median)*1000U)/
                          old_median):0U);
    rt_kprintf("[usb_crc_bench] %s events=%lu bytes=%lu max_len=%lu calls=%lu "
               "old_cycles/call=%lu new_cycles/call=%lu save_pm=%lu mismatches=%lu\r\n",
        mismatches==0U?"PASS":"FAIL",(unsigned long)event_count,
        (unsigned long)total_bytes,(unsigned long)max_length,
        (unsigned long)total_calls,
        (unsigned long)(old_median/(rt_uint32_t)total_calls),
        (unsigned long)(new_median/(rt_uint32_t)total_calls),
        (unsigned long)saved_permille,(unsigned long)mismatches);
}
MSH_CMD_EXPORT(usb_evidence_bytes_crc_bench_cmd,
               compare CRCs on captured USB payloads);
#endif

static rt_uint32_t usb_evidence_crc32_u8_with_update(
    usb_evidence_crc32_update_fn_t update,rt_uint32_t crc,rt_uint8_t value)
{
    return update(crc,&value,sizeof(value));
}

static rt_uint32_t usb_evidence_crc32_u16_with_update(
    usb_evidence_crc32_update_fn_t update,rt_uint32_t crc,rt_uint16_t value)
{
    rt_uint8_t bytes[2] = { (rt_uint8_t)value,
                            (rt_uint8_t)(value >> 8U) };
    return update(crc,bytes,sizeof(bytes));
}

static rt_uint32_t usb_evidence_crc32_u32_with_update(
    usb_evidence_crc32_update_fn_t update,rt_uint32_t crc,rt_uint32_t value)
{
    rt_uint8_t bytes[4] = { (rt_uint8_t)value,
                            (rt_uint8_t)(value >> 8U),
                            (rt_uint8_t)(value >> 16U),
                            (rt_uint8_t)(value >> 24U) };
    return update(crc,bytes,sizeof(bytes));
}

static rt_uint32_t usb_evidence_crc32_text_with_update(
    usb_evidence_crc32_update_fn_t update,rt_uint32_t crc,const char *text)
{
    rt_uint16_t length = 0U;

    if (text)
    {
        while (text[length] != '\0' && length != 0xFFFFU)
        {
            length++;
        }
    }
    crc = usb_evidence_crc32_u16_with_update(update,crc,length);
    if (length != 0U)
    {
        crc = update(crc,text,length);
    }
    return crc;
}

static rt_uint32_t usb_evidence_snapshot_crc32_impl(
    const usb_evidence_snapshot_t *snapshot,
    usb_evidence_crc32_update_fn_t update)
{
#define usb_evidence_crc32_update(crc,data,length) update((crc),(data),(length))
#define usb_evidence_crc32_u8(crc,value) \
    usb_evidence_crc32_u8_with_update(update,(crc),(value))
#define usb_evidence_crc32_u16(crc,value) \
    usb_evidence_crc32_u16_with_update(update,(crc),(value))
#define usb_evidence_crc32_u32(crc,value) \
    usb_evidence_crc32_u32_with_update(update,(crc),(value))
#define usb_evidence_crc32_text(crc,text) \
    usb_evidence_crc32_text_with_update(update,(crc),(text))
    rt_uint32_t crc = 0xFFFFFFFFU;
    rt_size_t index;
    rt_size_t count;

    if (!snapshot || !update)
    {
        return 0U;
    }

    crc = usb_evidence_crc32_u16(crc, snapshot->schema_version);
    crc = usb_evidence_crc32_u16(crc, snapshot->parser_version);
    crc = usb_evidence_crc32_u16(crc, snapshot->ruleset_version);
    crc = usb_evidence_crc32_u32(crc, snapshot->boot_id);
    crc = usb_evidence_crc32_u32(crc, snapshot->snapshot_id);
    crc = usb_evidence_crc32_u32(crc, snapshot->session_id);
    crc = usb_evidence_crc32_u32(crc, snapshot->attempt_id);
    crc = usb_evidence_crc32_u32(crc, snapshot->revision);
    crc = usb_evidence_crc32_u32(crc, snapshot->last_sequence);
    crc = usb_evidence_crc32_u32(crc, snapshot->last_raw_sequence);
    crc = usb_evidence_crc32_u32(crc, snapshot->analyzed_through_sequence);
    crc = usb_evidence_crc32_u32(crc, snapshot->published_at_us);
    crc = usb_evidence_crc32_u32(crc, snapshot->dropped_event_count);
    crc = usb_evidence_crc32_u32(crc, snapshot->dropped_payload_bytes);
    crc = usb_evidence_crc32_u32(crc, snapshot->session_dropped_event_count);
    crc = usb_evidence_crc32_u32(crc, snapshot->session_dropped_payload_bytes);
    crc = usb_evidence_crc32_u32(crc, snapshot->session_dropped_event_count);
    crc = usb_evidence_crc32_u32(crc, snapshot->session_dropped_payload_bytes);
    crc = usb_evidence_crc32_u32(crc, snapshot->raw_event_overflow_count);
    crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)snapshot->session_state);
    crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)snapshot->capture_integrity);
    crc = usb_evidence_crc32_u8(crc, snapshot->is_final ? 1U : 0U);
    crc = usb_evidence_crc32_u8(crc, snapshot->speed);
    crc = usb_evidence_crc32_u8(crc, snapshot->enumeration_complete ? 1U : 0U);
    crc = usb_evidence_crc32_u8(crc, snapshot->device_present ? 1U : 0U);
    crc = usb_evidence_crc32_u16(crc, snapshot->vendor_id);
    crc = usb_evidence_crc32_u16(crc, snapshot->product_id);
    crc = usb_evidence_crc32_u16(crc, snapshot->bcd_usb);
    crc = usb_evidence_crc32_u16(crc, snapshot->bcd_device);
    crc = usb_evidence_crc32_text(crc, snapshot->manufacturer);
    crc = usb_evidence_crc32_text(crc, snapshot->product);
    crc = usb_evidence_crc32_text(crc, snapshot->serial);
    crc = usb_evidence_crc32_text(crc, snapshot->class_name);
    crc = usb_evidence_crc32_text(crc, snapshot->phase);

    count = snapshot->raw_event_count;
    if (count > USB_EVIDENCE_SNAPSHOT_EVENT_COUNT)
    {
        count = USB_EVIDENCE_SNAPSHOT_EVENT_COUNT;
    }
    crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)count);
    for (index = 0U; index < count; index++)
    {
        const usb_evidence_raw_event_t *event = &snapshot->raw_events[index];
        rt_size_t captured = event->captured_length;

        if (captured > USB_EVIDENCE_CAPTURE_MAX)
        {
            captured = USB_EVIDENCE_CAPTURE_MAX;
        }
        crc = usb_evidence_crc32_u32(crc, event->session_id);
        crc = usb_evidence_crc32_u32(crc, event->attempt_id);
        crc = usb_evidence_crc32_u32(crc, event->sequence);
        crc = usb_evidence_crc32_u32(crc, event->timestamp_us);
        crc = usb_evidence_crc32_u32(crc, event->started_at_us);
        crc = usb_evidence_crc32_u32(crc, event->completed_at_us);
        crc = usb_evidence_crc32_u32(crc, event->duration_us);
        crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)event->event_type);
        crc = usb_evidence_crc32_u8(crc, event->port);
        crc = usb_evidence_crc32_u8(crc, event->port_valid ? 1U : 0U);
        crc = usb_evidence_crc32_u8(crc, event->timing_valid ? 1U : 0U);
        crc = usb_evidence_crc32_u8(crc, event->speed);
        crc = usb_evidence_crc32_u8(crc, event->has_setup ? 1U : 0U);
        if (event->has_setup)
        {
            crc = usb_evidence_crc32_u8(crc, event->setup_packet.bm_request_type);
            crc = usb_evidence_crc32_u8(crc, event->setup_packet.b_request);
            crc = usb_evidence_crc32_u16(crc, event->setup_packet.w_value);
            crc = usb_evidence_crc32_u16(crc, event->setup_packet.w_index);
            crc = usb_evidence_crc32_u16(crc, event->setup_packet.w_length);
        }
        crc = usb_evidence_crc32_u32(crc, (rt_uint32_t)event->return_status);
        crc = usb_evidence_crc32_u8(crc, event->transfer_length_known ? 1U : 0U);
        crc = usb_evidence_crc32_u16(crc, event->transfer_length);
        crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)event->data_direction);
        crc = usb_evidence_crc32_u16(crc, (rt_uint16_t)captured);
        crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)event->capture_status);
        crc = usb_evidence_crc32_u32(crc, event->payload_crc32);
        crc = usb_evidence_crc32_update(crc, event->raw_response, captured);
    }

    count = snapshot->descriptor_node_count;
    if (count > USB_EVIDENCE_DESCRIPTOR_NODE_COUNT)
    {
        count = USB_EVIDENCE_DESCRIPTOR_NODE_COUNT;
    }
    crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)count);
    for (index = 0U; index < count; index++)
    {
        const usb_evidence_descriptor_node_t *node =
            &snapshot->descriptor_nodes[index];
        crc = usb_evidence_crc32_u16(crc, node->node_id);
        crc = usb_evidence_crc32_u16(crc, node->parent_id);
        crc = usb_evidence_crc32_u32(crc, node->source_sequence);
        crc = usb_evidence_crc32_u16(crc, node->source_offset);
        crc = usb_evidence_crc32_u16(crc, node->byte_length);
        crc = usb_evidence_crc32_u16(crc, node->field_start);
        crc = usb_evidence_crc32_u8(crc, node->field_count);
        crc = usb_evidence_crc32_u8(crc, node->depth);
        crc = usb_evidence_crc32_u8(crc, node->descriptor_type);
        crc = usb_evidence_crc32_u8(crc, node->truncated ? 1U : 0U);
        crc = usb_evidence_crc32_text(crc, node->type_name);
        crc = usb_evidence_crc32_text(crc, node->summary);
    }

    count = snapshot->descriptor_field_count;
    if (count > USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT)
    {
        count = USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT;
    }
    crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)count);
    for (index = 0U; index < count; index++)
    {
        const usb_evidence_descriptor_field_t *field =
            &snapshot->descriptor_fields[index];
        crc = usb_evidence_crc32_u16(crc, field->node_id);
        crc = usb_evidence_crc32_u8(crc, field->byte_offset);
        crc = usb_evidence_crc32_u8(crc, field->byte_length);
        crc = usb_evidence_crc32_text(crc, field->name);
        crc = usb_evidence_crc32_text(crc, field->value);
    }

    count = snapshot->timeline_count;
    if (count > USB_EVIDENCE_TIMELINE_COUNT)
    {
        count = USB_EVIDENCE_TIMELINE_COUNT;
    }
    crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)count);
    for (index = 0U; index < count; index++)
    {
        const usb_evidence_timeline_item_t *item = &snapshot->timeline[index];
        crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)item->stage);
        crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)item->status);
        crc = usb_evidence_crc32_u32(crc, item->event_sequence);
        crc = usb_evidence_crc32_u32(crc, item->timestamp_us);
        crc = usb_evidence_crc32_text(crc, item->label);
    }

    for (index = 0U; index < USB_EVIDENCE_RULE_COUNT; index++)
    {
        const usb_evidence_rule_result_t *rule = &snapshot->rules[index];
        rt_size_t reference_count = rule->evidence_ref_count;
        rt_size_t reference_index;

        if (reference_count > USB_EVIDENCE_REFERENCE_COUNT)
        {
            reference_count = USB_EVIDENCE_REFERENCE_COUNT;
        }
        crc = usb_evidence_crc32_text(crc, rule->rule_id);
        crc = usb_evidence_crc32_text(crc, rule->spec_clause);
        crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)rule->severity);
        crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)rule->verdict);
        crc = usb_evidence_crc32_u32(crc, rule->event_sequence);
        crc = usb_evidence_crc32_text(crc, rule->expected);
        crc = usb_evidence_crc32_text(crc, rule->observed);
        crc = usb_evidence_crc32_text(crc, rule->reason_code);
        crc = usb_evidence_crc32_u8(crc, (rt_uint8_t)reference_count);
        crc = usb_evidence_crc32_u16(
            crc, rule->omitted_failure_reference_count);
        for (reference_index = 0U; reference_index < reference_count;
             reference_index++)
        {
            const usb_evidence_reference_t *reference =
                &rule->evidence_refs[reference_index];
            crc = usb_evidence_crc32_u32(crc, reference->event_sequence);
            crc = usb_evidence_crc32_u16(crc, reference->byte_offset);
            crc = usb_evidence_crc32_u16(crc, reference->byte_length);
            crc = usb_evidence_crc32_text(crc, reference->role);
        }
    }
    return crc ^ 0xFFFFFFFFU;
#undef usb_evidence_crc32_update
#undef usb_evidence_crc32_u8
#undef usb_evidence_crc32_u16
#undef usb_evidence_crc32_u32
#undef usb_evidence_crc32_text
}

rt_uint32_t usb_evidence_snapshot_crc32(
    const usb_evidence_snapshot_t *snapshot)
{
    return usb_evidence_snapshot_crc32_impl(
        snapshot,usb_evidence_crc32_update_mongoose);
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static rt_uint32_t usb_evidence_snapshot_crc_bench_median(
    rt_uint32_t samples[5])
{
    rt_uint32_t i;
    for(i=1U;i<5U;i++) {
        rt_uint32_t value=samples[i],position=i;
        while(position>0U&&samples[position-1U]>value) {
            samples[position]=samples[position-1U];position--;
        }
        samples[position]=value;
    }
    return samples[2];
}

static void usb_evidence_snapshot_crc_bench_cmd(void)
{
    static const rt_uint8_t vector[]="123456789";
    rt_uint32_t old_cycles[5],new_cycles[5];
    rt_uint32_t old_state=0xFFFFFFFFU,new_state=0xFFFFFFFFU;
    rt_uint32_t sample,pass,index,old_median,new_median,saved_permille;
    rt_uint32_t expected,payload_bytes=0U,mismatches=0U,old_crc=0U,new_crc=0U;
    rt_uint32_t snapshot_id;
    rt_uint8_t event_count,node_count,field_count;
    rt_uint32_t bench_window_start,time_limit;
    bool timed_out=false;

    old_state=usb_evidence_crc32_update_bitwise(old_state,vector,4U);
    old_state=usb_evidence_crc32_update_bitwise(old_state,vector+4U,5U)^0xFFFFFFFFU;
    new_state=usb_evidence_crc32_update_mongoose(new_state,vector,4U);
    new_state=usb_evidence_crc32_update_mongoose(new_state,vector+4U,5U)^0xFFFFFFFFU;
    if(old_state!=0xCBF43926U||new_state!=0xCBF43926U||
       old_state!=new_state) {
        rt_kprintf("[usb_snapshot_crc_bench] FAIL split-vector\r\n");
        return;
    }
    if(!g_snapshot_lock||
       rt_mutex_take(g_snapshot_lock,RT_WAITING_NO)!=RT_EOK) {
        rt_kprintf("[usb_snapshot_crc_bench] FAIL snapshot busy\r\n");
        return;
    }
    bench_window_start=usb_perf_now();
    time_limit=SystemCoreClock/10U;
    if(time_limit==0U)time_limit=60000000U;
    expected=g_snapshot.content_crc32;
    if(g_snapshot.session_id==0U||g_snapshot.raw_event_count==0U) {
        rt_mutex_release(g_snapshot_lock);
        rt_kprintf("[usb_snapshot_crc_bench] FAIL no published snapshot\r\n");
        return;
    }
    if(g_snapshot.raw_event_count>USB_EVIDENCE_SNAPSHOT_EVENT_COUNT) {
        rt_mutex_release(g_snapshot_lock);
        rt_kprintf("[usb_snapshot_crc_bench] FAIL invalid event count\r\n");
        return;
    }
    snapshot_id=g_snapshot.snapshot_id;
    event_count=g_snapshot.raw_event_count;
    node_count=g_snapshot.descriptor_node_count;
    field_count=g_snapshot.descriptor_field_count;
    for(index=0U;index<g_snapshot.raw_event_count;index++) {
        if(usb_perf_now()-bench_window_start>time_limit) {
            timed_out=true;break;
        }
        if(g_snapshot.raw_events[index].captured_length>
           USB_EVIDENCE_CAPTURE_MAX) {
            rt_mutex_release(g_snapshot_lock);
            rt_kprintf("[usb_snapshot_crc_bench] FAIL invalid payload length\r\n");
            return;
        }
        payload_bytes+=g_snapshot.raw_events[index].captured_length;
    }
    if(timed_out) {
        rt_mutex_release(g_snapshot_lock);
        rt_kprintf("[usb_snapshot_crc_bench] FAIL exceeded 100ms bound\r\n");
        return;
    }
    if(payload_bytes>8192U) {
        rt_mutex_release(g_snapshot_lock);
        rt_kprintf("[usb_snapshot_crc_bench] FAIL snapshot exceeds short-bench bound\r\n");
        return;
    }

    /* Warm both algorithms, then measure the same frozen snapshot without
     * adding samples to the normal USB evidence profiler histogram. */
    old_crc=usb_evidence_snapshot_crc32_impl(
        &g_snapshot,usb_evidence_crc32_update_bitwise);
    if(usb_perf_now()-bench_window_start>time_limit) {
        rt_mutex_release(g_snapshot_lock);
        rt_kprintf("[usb_snapshot_crc_bench] FAIL exceeded 100ms bound\r\n");
        return;
    }
    new_crc=usb_evidence_snapshot_crc32_impl(
        &g_snapshot,usb_evidence_crc32_update_mongoose);
    if(usb_perf_now()-bench_window_start>time_limit) {
        rt_mutex_release(g_snapshot_lock);
        rt_kprintf("[usb_snapshot_crc_bench] FAIL exceeded 100ms bound\r\n");
        return;
    }
    if(old_crc!=new_crc||old_crc!=expected) {
        rt_mutex_release(g_snapshot_lock);
        rt_kprintf("[usb_snapshot_crc_bench] FAIL snapshot pending or CRC mismatch\r\n");
        return;
    }
    for(sample=0U;sample<5U;sample++) {
        bool old_first=(sample&1U)==0U;
        for(pass=0U;pass<2U;pass++) {
            bool run_old=(pass==0U)?old_first:!old_first;
            rt_uint32_t start=usb_perf_now();
            rt_uint32_t crc=usb_evidence_snapshot_crc32_impl(
                &g_snapshot,run_old?usb_evidence_crc32_update_bitwise:
                                   usb_evidence_crc32_update_mongoose);
            rt_uint32_t elapsed=usb_perf_now()-start;
            if(crc!=expected)mismatches++;
            if(run_old)old_cycles[sample]=elapsed;
            else new_cycles[sample]=elapsed;
            if(usb_perf_now()-bench_window_start>time_limit) {
                timed_out=true;break;
            }
        }
        if(timed_out)break;
    }
    rt_mutex_release(g_snapshot_lock);

    if(timed_out) {
        rt_kprintf("[usb_snapshot_crc_bench] FAIL exceeded 100ms bound\r\n");
        return;
    }

    old_median=usb_evidence_snapshot_crc_bench_median(old_cycles);
    new_median=usb_evidence_snapshot_crc_bench_median(new_cycles);
    saved_permille=old_median==0U?0U:
        (old_median>new_median?
            (rt_uint32_t)(((rt_uint64_t)(old_median-new_median)*1000U)/
                          old_median):0U);
    rt_kprintf("[usb_snapshot_crc_bench] %s id=%lu ev=%u bytes=%lu nodes=%u fields=%u\r\n",
        mismatches==0U?"PASS":"FAIL",(unsigned long)snapshot_id,
        (unsigned)event_count,(unsigned long)payload_bytes,
        (unsigned)node_count,(unsigned)field_count);
    rt_kprintf("old_cycles=%lu new_cycles=%lu save_pm=%lu crc=%08lX mismatches=%lu\r\n",
        (unsigned long)old_median,(unsigned long)new_median,
        (unsigned long)saved_permille,(unsigned long)expected,
        (unsigned long)mismatches);
}
MSH_CMD_EXPORT(usb_evidence_snapshot_crc_bench_cmd,
               compare canonical CRC algorithms on a frozen snapshot);
#endif

static rt_uint32_t usb_evidence_make_boot_id(void)
{
    rt_uint32_t value = 0U;

#ifdef UID_BASE
    value = *(volatile rt_uint32_t *)(UID_BASE + 0U) ^
            *(volatile rt_uint32_t *)(UID_BASE + 4U) ^
            *(volatile rt_uint32_t *)(UID_BASE + 8U);
#endif
    value ^= (rt_uint32_t)HAL_GetTick();
    value ^= 0xA17C092EU;
    return value != 0U ? value : 1U;
}

static void usb_evidence_rule_reference_clear(
    usb_evidence_rule_result_t *result)
{
    if (!result)
    {
        return;
    }
    result->event_sequence = 0U;
    result->evidence_ref_count = 0U;
    result->omitted_failure_reference_count = 0U;
    rt_memset(result->evidence_refs, 0, sizeof(result->evidence_refs));
}

static rt_uint32_t usb_evidence_dropped_count(void);
static rt_uint32_t usb_evidence_dropped_payload_count(void);

static void usb_evidence_rule_reference_add(
    usb_evidence_rule_result_t *result, rt_uint32_t event_sequence,
    rt_uint16_t byte_offset, rt_uint16_t byte_length, const char *role)
{
    usb_evidence_reference_t *reference;

    if (!result || event_sequence == 0U ||
        result->evidence_ref_count >= USB_EVIDENCE_REFERENCE_COUNT)
    {
        return;
    }
    reference = &result->evidence_refs[result->evidence_ref_count++];
    reference->event_sequence = event_sequence;
    reference->byte_offset = byte_offset;
    reference->byte_length = byte_length;
    usb_evidence_copy_text(reference->role, sizeof(reference->role), role);
    result->event_sequence = event_sequence;
}

static rt_bool_t usb_evidence_reference_valid(rt_uint32_t sequence,
                                               rt_uint16_t byte_offset,
                                               rt_uint16_t byte_length)
{
    rt_size_t index;

    if (sequence == 0U)
    {
        return RT_FALSE;
    }
    for (index = 0U; index < g_snapshot.raw_event_count; index++)
    {
        const usb_evidence_raw_event_t *event = &g_snapshot.raw_events[index];

        if (event->sequence != sequence)
        {
            continue;
        }
        if (byte_length == 0U)
        {
            return RT_TRUE;
        }
        return (rt_uint32_t)byte_offset + byte_length <=
               event->captured_length;
    }
    return RT_FALSE;
}

static void usb_evidence_timeline_init(void)
{
    static const char *labels[USB_EVIDENCE_TIMELINE_COUNT] =
    {
        "PORT RESET", "GET DEV 8B", "SET ADDRESS", "GET DEV 18B",
        "GET CONFIG", "GET STRING", "SET CONFIG", "ENUM COMPLETE",
    };
    rt_size_t index;

    g_snapshot.timeline_count = USB_EVIDENCE_TIMELINE_COUNT;
    for (index = 0U; index < USB_EVIDENCE_TIMELINE_COUNT; index++)
    {
        g_snapshot.timeline[index].stage =
            (usb_evidence_timeline_stage_t)index;
        g_snapshot.timeline[index].status = USB_EVIDENCE_TIMELINE_WAITING;
        usb_evidence_copy_text(g_snapshot.timeline[index].label,
                               sizeof(g_snapshot.timeline[index].label),
                               labels[index]);
    }
}

static void usb_evidence_timeline_mark(
    usb_evidence_timeline_stage_t stage,
    const usb_evidence_raw_event_t *event)
{
    usb_evidence_timeline_item_t *item;

    if (!event || (rt_size_t)stage >= g_snapshot.timeline_count)
    {
        return;
    }
    item = &g_snapshot.timeline[stage];
    item->status = event->return_status < 0 ?
                   USB_EVIDENCE_TIMELINE_FAILED :
                   USB_EVIDENCE_TIMELINE_OBSERVED;
    item->event_sequence = event->sequence;
    item->timestamp_us = event->timestamp_us;
}

static rt_uint16_t usb_evidence_descriptor_add(
    const usb_evidence_raw_event_t *event, rt_uint16_t source_offset,
    rt_uint16_t length, rt_uint16_t parent_id, rt_uint8_t depth)
{
    usb_evidence_descriptor_node_t *target;
    usb_desc_node_t parsed;
    rt_uint16_t node_id;
    rt_uint8_t field_index;
    rt_uint8_t available_fields;

    if (!event || source_offset >= event->captured_length || length < 2U ||
        source_offset + length > event->captured_length ||
        g_snapshot.descriptor_node_count >=
            USB_EVIDENCE_DESCRIPTOR_NODE_COUNT)
    {
        return USB_EVIDENCE_NODE_NONE;
    }
    for (node_id = 0U; node_id < g_snapshot.descriptor_node_count; node_id++)
    {
        target = &g_snapshot.descriptor_nodes[node_id];
        if (target->source_sequence == event->sequence &&
            target->source_offset == source_offset)
        {
            return target->node_id;
        }
    }
    if (event->raw_response[source_offset + 1U] == 0x03U &&
        (event->setup_packet.w_value >> 8U) == 0x03U)
    {
        if (!usb_desc_parse_string(event->raw_response + source_offset,
                                   length,
                                   (rt_uint8_t)(event->setup_packet.w_value &
                                                0xFFU),
                                   &parsed))
        {
            return USB_EVIDENCE_NODE_NONE;
        }
    }
    else if (!usb_desc_parse_single(event->raw_response + source_offset,
                                   length, &parsed))
    {
        return USB_EVIDENCE_NODE_NONE;
    }

    node_id = g_snapshot.descriptor_node_count++;
    target = &g_snapshot.descriptor_nodes[node_id];
    rt_memset(target, 0, sizeof(*target));
    target->node_id = node_id;
    target->parent_id = parent_id;
    target->source_sequence = event->sequence;
    target->source_offset = source_offset;
    target->byte_length = length;
    target->field_start = g_snapshot.descriptor_field_count;
    target->depth = depth;
    target->descriptor_type = event->raw_response[source_offset + 1U];
    target->truncated = length < event->raw_response[source_offset];
    usb_evidence_copy_text(target->type_name, sizeof(target->type_name),
                           parsed.type_name);
    usb_evidence_copy_text(target->summary, sizeof(target->summary),
                           parsed.summary);

    available_fields = (rt_uint8_t)(USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT -
                                    g_snapshot.descriptor_field_count);
    target->field_count = parsed.field_count < available_fields ?
                          parsed.field_count : available_fields;
    for (field_index = 0U; field_index < target->field_count; field_index++)
    {
        usb_evidence_descriptor_field_t *field =
            &g_snapshot.descriptor_fields[
                g_snapshot.descriptor_field_count++];

        rt_memset(field, 0, sizeof(*field));
        field->node_id = node_id;
        field->byte_offset = parsed.field_offsets[field_index];
        field->byte_length = parsed.field_lengths[field_index];
        usb_evidence_copy_text(field->name, sizeof(field->name),
                               parsed.field_names[field_index]);
        usb_evidence_copy_text(field->value, sizeof(field->value),
                               parsed.field_values[field_index]);
    }
    return node_id;
}

static rt_bool_t usb_evidence_validate_rule_references(void)
{
    rt_bool_t all_valid = RT_TRUE;
    rt_size_t rule_index;

    for (rule_index = 0U; rule_index < USB_EVIDENCE_RULE_COUNT; rule_index++)
    {
        usb_evidence_rule_result_t *rule = &g_snapshot.rules[rule_index];
        rt_size_t reference_index;
        rt_bool_t rule_valid = RT_TRUE;

        if (rule->verdict != USB_RULE_PASS && rule->verdict != USB_RULE_FAIL)
        {
            continue;
        }
        if (rule->evidence_ref_count == 0U)
        {
            rule_valid = RT_FALSE;
        }
        for (reference_index = 0U;
             reference_index < rule->evidence_ref_count; reference_index++)
        {
            if (!usb_evidence_reference_valid(
                    rule->evidence_refs[reference_index].event_sequence,
                    rule->evidence_refs[reference_index].byte_offset,
                    rule->evidence_refs[reference_index].byte_length))
            {
                rule_valid = RT_FALSE;
                break;
            }
        }
        if (!rule_valid)
        {
            all_valid = RT_FALSE;
            rule->verdict = USB_RULE_NOT_OBSERVED;
            usb_evidence_copy_text(rule->reason_code,
                                   sizeof(rule->reason_code),
                                   "EVIDENCE_MISSING");
            usb_evidence_copy_text(rule->observed,
                                   sizeof(rule->observed),
                                   "规则引用的原始事件不在冻结快照中");
            usb_evidence_rule_reference_clear(rule);
        }
    }
    return all_valid;
}

static void usb_evidence_prepare_publication(void)
{
    rt_size_t index;
    rt_bool_t degraded = RT_FALSE;
    rt_uint32_t crc_start;

    g_snapshot.dropped_event_count = usb_evidence_dropped_count();
    g_snapshot.dropped_payload_bytes =
        usb_evidence_dropped_payload_count();
    g_snapshot.last_raw_sequence = g_snapshot.last_sequence;
    g_snapshot.analyzed_through_sequence = g_snapshot.last_sequence;
    g_snapshot.published_at_us = usb_evidence_timestamp_us();
    g_snapshot.snapshot_id = ++g_next_snapshot_id;

    if (g_snapshot.session_dropped_event_count > 0U ||
        g_snapshot.session_dropped_payload_bytes > 0U ||
        g_snapshot.raw_event_overflow_count > 0U)
    {
        degraded = RT_TRUE;
    }
    for (index = 0U; index < g_snapshot.raw_event_count; index++)
    {
        if (g_snapshot.raw_events[index].capture_status ==
            USB_EVIDENCE_CAPTURE_LOCAL_TRUNCATED)
        {
            degraded = RT_TRUE;
            break;
        }
    }
    if (g_snapshot.is_final && !usb_evidence_validate_rule_references())
    {
        degraded = RT_TRUE;
    }
    if (g_rule_session_active &&
        (g_rule_session.status == USBDIAG_SESSION_DEGRADED ||
         g_rule_session.status == USBDIAG_SESSION_CAPACITY_EXCEEDED))
    {
        degraded = RT_TRUE;
    }
    g_snapshot.capture_integrity = degraded ?
        USB_EVIDENCE_INTEGRITY_DEGRADED :
        USB_EVIDENCE_INTEGRITY_COMPLETE;
    if (degraded && g_snapshot.session_state != USB_EVIDENCE_SESSION_FAILED)
    {
        g_snapshot.session_state = USB_EVIDENCE_SESSION_DEGRADED;
    }

    g_snapshot.content_crc32 = 0U;
    crc_start = usb_perf_now();
    g_snapshot.content_crc32 = usb_evidence_snapshot_crc32(&g_snapshot);
    usb_perf_record(USB_PERF_EVIDENCE_CRC, crc_start, usb_perf_now());
}

static void usb_evidence_try_publish(rt_bool_t force)
{
    rt_tick_t now = rt_tick_get();
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    rt_uint32_t evidence_lock_start;
    rt_uint32_t evidence_lock_acquired;
    rt_uint32_t evidence_prepare_end;
    rt_uint32_t web_update_start;
    rt_uint32_t web_update_end;
#endif
    rt_uint32_t dropped_events = usb_evidence_dropped_count();
    rt_uint32_t dropped_bytes = usb_evidence_dropped_payload_count();
    rt_uint32_t producer_session_id;
    rt_uint32_t session_dropped_events;
    rt_uint32_t session_dropped_bytes;
    rt_err_t web_update_result;
    rt_bool_t changed = g_snapshot.revision != g_last_web_publish_revision ||
                        dropped_events != g_last_web_publish_drop_count ||
                        dropped_bytes != g_last_web_publish_drop_bytes;

    usb_evidence_live_session_losses(&producer_session_id,
                                     &session_dropped_events,
                                     &session_dropped_bytes);
    if (producer_session_id == g_snapshot.session_id &&
        (session_dropped_events > g_snapshot.session_dropped_event_count ||
         session_dropped_bytes > g_snapshot.session_dropped_payload_bytes))
        changed = RT_TRUE;

    if (!force && !changed)
    {
        usb_evidence_try_schedule_pdf_pregen();
        return;
    }
    if (!force && g_last_web_publish_tick != 0U &&
        (rt_int32_t)(now - g_last_web_publish_tick) <
            (rt_int32_t)rt_tick_from_millisecond(USB_EVIDENCE_WEB_PUBLISH_MS))
    {
        return;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    evidence_lock_start = usb_perf_now();
#endif
    if (!g_snapshot_lock ||
        rt_mutex_take(g_snapshot_lock, RT_WAITING_FOREVER) != RT_EOK)
    {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        usb_perf_record(USB_PERF_EVIDENCE_PUBLISH_LOCK_WAIT,
                        evidence_lock_start, usb_perf_now());
#endif
        return;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    evidence_lock_acquired = usb_perf_now();
#endif
    (void)usb_evidence_sync_live_session_losses();
    usb_evidence_prepare_publication();
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    evidence_prepare_end = usb_perf_now();
#endif
    rt_mutex_release(g_snapshot_lock);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    usb_perf_record(USB_PERF_EVIDENCE_PUBLISH_LOCK_WAIT,
                    evidence_lock_start, evidence_lock_acquired);
    usb_perf_record(USB_PERF_EVIDENCE_PREPARE,
                    evidence_lock_acquired, evidence_prepare_end);
#endif

    /* This thread is the sole writer of g_snapshot, so the web module can
     * stage and serialize it after the analysis lock has been released. */
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    web_update_start = usb_perf_now();
#endif
    web_update_result = web_snapshot_update_evidence(&g_snapshot);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    web_update_end = usb_perf_now();
    usb_perf_record(USB_PERF_WEB_UPDATE, web_update_start, web_update_end);
#endif
    if (web_update_result == RT_EOK)
    {
        g_last_web_publish_tick = now;
        g_last_web_publish_revision = g_snapshot.revision;
        g_last_web_publish_drop_count = g_snapshot.dropped_event_count;
        g_last_web_publish_drop_bytes = g_snapshot.dropped_payload_bytes;
        usb_evidence_try_schedule_pdf_pregen();
        /* The LVGL owner polls evidence revisions on its bounded timer loop.
         * Avoid synchronously waking it here: the lower-priority analyzer
         * would wait for the full LVGL iteration before draining USB events. */
    }
}

static void usb_evidence_drop_event(void)
{
    rt_base_t level = rt_hw_interrupt_disable();

    g_dropped_event_count++;
    if (g_current_session_id != 0U &&
        g_session_drop_owner == g_current_session_id)
        g_session_dropped_event_count++;
    rt_hw_interrupt_enable(level);
}

static void usb_evidence_add_dropped_payload(rt_uint32_t bytes)
{
    rt_base_t level = rt_hw_interrupt_disable();

    g_dropped_payload_bytes += bytes;
    if (g_current_session_id != 0U &&
        g_session_drop_owner == g_current_session_id)
        g_session_dropped_payload_bytes += bytes;
    rt_hw_interrupt_enable(level);
}

static rt_uint32_t usb_evidence_dropped_count(void)
{
    rt_uint32_t count;
    rt_base_t level = rt_hw_interrupt_disable();

    count = g_dropped_event_count;
    rt_hw_interrupt_enable(level);
    return count;
}

static rt_uint32_t usb_evidence_dropped_payload_count(void)
{
    rt_uint32_t count;
    rt_base_t level = rt_hw_interrupt_disable();

    count = g_dropped_payload_bytes;
    rt_hw_interrupt_enable(level);
    return count;
}

static void usb_evidence_live_session_losses(rt_uint32_t *session_id,
                                             rt_uint32_t *dropped_events,
                                             rt_uint32_t *dropped_bytes)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (session_id)
        *session_id = g_session_drop_owner;
    if (dropped_events)
        *dropped_events = g_session_dropped_event_count;
    if (dropped_bytes)
        *dropped_bytes = g_session_dropped_payload_bytes;
    rt_hw_interrupt_enable(level);
}

static void usb_evidence_fill_header(usb_evidence_raw_event_t *event,
                                     usb_evidence_event_type_t event_type,
                                     rt_uint8_t speed)
{
    rt_base_t level;

    rt_memset(event, 0, sizeof(*event));
    level = rt_hw_interrupt_disable();
    event->session_id = g_current_session_id;
    event->attempt_id = g_current_attempt_id;
    event->sequence = ++g_next_sequence;
    event->session_dropped_event_count =
        g_session_drop_owner == g_current_session_id ?
        g_session_dropped_event_count : 0U;
    event->session_dropped_payload_bytes =
        g_session_drop_owner == g_current_session_id ?
        g_session_dropped_payload_bytes : 0U;
    rt_hw_interrupt_enable(level);

    event->timestamp_us = usb_evidence_timestamp_us();
    event->started_at_us = event->timestamp_us;
    event->completed_at_us = event->timestamp_us;
    event->event_type = event_type;
    event->speed = speed;
}

static void usb_evidence_enqueue(const usb_evidence_raw_event_t *event)
{
    if (!g_event_queue || rt_mq_send(g_event_queue, event, sizeof(*event)) != RT_EOK)
    {
        usb_evidence_drop_event();
    }
}

static rt_bool_t usb_evidence_producer_lock(void)
{
    return g_producer_lock != RT_NULL &&
           rt_mutex_take(g_producer_lock, RT_WAITING_FOREVER) == RT_EOK;
}

static void usb_evidence_producer_unlock(void)
{
    if (g_producer_lock != RT_NULL)
    {
        rt_mutex_release(g_producer_lock);
    }
}

static void usb_evidence_snapshot_reset(rt_uint32_t session_id, rt_uint8_t speed)
{
    rt_size_t index;

    rt_memset(&g_snapshot, 0, sizeof(g_snapshot));
    rt_memset(&g_context, 0, sizeof(g_context));
    g_rule_session_active = RT_FALSE;
    g_snapshot.schema_version = USB_EVIDENCE_SCHEMA_VERSION;
    g_snapshot.parser_version = USB_EVIDENCE_PARSER_VERSION;
    g_snapshot.ruleset_version = USB_EVIDENCE_RULESET_VERSION;
    g_snapshot.boot_id = g_boot_id;
    g_snapshot.session_id = session_id;
    g_snapshot.attempt_id = g_current_attempt_id;
    g_snapshot.speed = speed;
    g_snapshot.session_state = session_id == 0U ?
        USB_EVIDENCE_SESSION_WAITING : USB_EVIDENCE_SESSION_RECORDING;
    g_snapshot.capture_integrity = USB_EVIDENCE_INTEGRITY_COMPLETE;
    usb_evidence_copy_text(g_snapshot.phase, sizeof(g_snapshot.phase),
                           session_id == 0U ? "等待设备" : "枚举中");
    usb_evidence_timeline_init();

    for (index = 0U; index < USB_EVIDENCE_RULE_COUNT; index++)
    {
        g_snapshot.rules[index].rule_id = g_rule_definitions[index].id;
        g_snapshot.rules[index].spec_clause = g_rule_definitions[index].clause;
        g_snapshot.rules[index].severity = g_rule_definitions[index].severity;
        g_snapshot.rules[index].verdict = USB_RULE_NOT_OBSERVED;
        usb_evidence_copy_text(g_snapshot.rules[index].expected,
                               sizeof(g_snapshot.rules[index].expected),
                               g_rule_definitions[index].expected);
        usb_evidence_copy_text(g_snapshot.rules[index].reason_code,
                               sizeof(g_snapshot.rules[index].reason_code),
                               "WAITING_EVIDENCE");
        usb_evidence_copy_text(g_snapshot.rules[index].observed,
                               sizeof(g_snapshot.rules[index].observed),
                               "等待相关控制传输");
    }
    if (session_id != 0U &&
        usbdiag_session_begin(&g_rule_session, session_id,
                              g_current_attempt_id, speed) ==
            USBDIAG_SESSION_NOT_FINAL)
    {
        g_rule_session_active = RT_TRUE;
    }
}

static rt_bool_t usb_evidence_map_core_rule(rt_size_t source,
                                             enum usb_rule_index *destination)
{
    if (!destination)
        return RT_FALSE;
    switch (source)
    {
    case USBDIAG_RULE_DEV_B_LENGTH: *destination = USB_RULE_DEV_B_LENGTH; return RT_TRUE;
    case USBDIAG_RULE_DEV_TYPE: *destination = USB_RULE_DEV_TYPE; return RT_TRUE;
    case USBDIAG_RULE_DEV_8B_PREFIX: *destination = USB_RULE_DEV_8B_PREFIX; return RT_TRUE;
    case USBDIAG_RULE_DEV_FIELDS: *destination = USB_RULE_DEV_FIELDS; return RT_TRUE;
    case USBDIAG_RULE_CFG_B_LENGTH: *destination = USB_RULE_CFG_B_LENGTH; return RT_TRUE;
    case USBDIAG_RULE_CFG_TOTAL_LENGTH: *destination = USB_RULE_CFG_TOTAL_LENGTH; return RT_TRUE;
    case USBDIAG_RULE_CFG_TREE_WALK: *destination = USB_RULE_CFG_TREE_WALK; return RT_TRUE;
    case USBDIAG_RULE_CFG_INTERFACE_COUNT: *destination = USB_RULE_CFG_INTERFACE_COUNT; return RT_TRUE;
    case USBDIAG_RULE_IF_ENDPOINT_COUNT: *destination = USB_RULE_IF_ENDPOINT_COUNT; return RT_TRUE;
    case USBDIAG_RULE_EP_PACKET_SIZE: *destination = USB_RULE_EP_PACKET_SIZE; return RT_TRUE;
    case USBDIAG_RULE_EP_INTERVAL: *destination = USB_RULE_EP_INTERVAL; return RT_TRUE;
    case USBDIAG_RULE_EP0_STALL: *destination = USB_RULE_EP0_STALL; return RT_TRUE;
    default: return RT_FALSE;
    }
}

static usb_rule_verdict_t usb_evidence_map_core_verdict(usbdiag_verdict_t verdict)
{
    switch (verdict)
    {
    case USBDIAG_PASS: return USB_RULE_PASS;
    case USBDIAG_FAIL: return USB_RULE_FAIL;
    case USBDIAG_NOT_APPLICABLE: return USB_RULE_NOT_APPLICABLE;
    case USBDIAG_NOT_OBSERVED:
    default: return USB_RULE_NOT_OBSERVED;
    }
}

static void usb_evidence_format_core_rule(enum usb_rule_index index,
                                          const usbdiag_rule_result_t *source,
                                          char *expected,
                                          rt_size_t expected_size,
                                          char *observed,
                                          rt_size_t observed_size)
{
    rt_snprintf(expected, expected_size, "%s",
                g_rule_definitions[index].expected);
    switch (index)
    {
    case USB_RULE_DEV_B_LENGTH:
        rt_snprintf(observed, observed_size, "bLength=%lu，期望 18",
                    (unsigned long)source->observed_values[0]);
        break;
    case USB_RULE_DEV_TYPE:
        rt_snprintf(observed, observed_size, "bDescriptorType=%lu，期望 1",
                    (unsigned long)source->observed_values[0]);
        break;
    case USB_RULE_DEV_8B_PREFIX:
        rt_snprintf(observed, observed_size, "前 8 字节%s完整设备描述符",
                    source->observed_values[0] ? "与" : "不匹配");
        break;
    case USB_RULE_DEV_FIELDS:
        rt_snprintf(observed, observed_size,
                    "bMaxPacketSize0=%lu，速度=%lu",
                    (unsigned long)source->observed_values[0],
                    (unsigned long)source->expected_values[0]);
        break;
    case USB_RULE_CFG_B_LENGTH:
        rt_snprintf(observed, observed_size,
                    "bLength=%lu，bDescriptorType=%lu",
                    (unsigned long)source->observed_values[0],
                    (unsigned long)source->observed_values[1]);
        break;
    case USB_RULE_CFG_TOTAL_LENGTH:
        rt_snprintf(observed, observed_size,
                    "声明=%lu，实际传输=%lu，请求=%lu",
                    (unsigned long)source->expected_values[0],
                    (unsigned long)source->observed_values[0],
                    (unsigned long)source->observed_values[1]);
        break;
    case USB_RULE_CFG_TREE_WALK:
        rt_snprintf(observed, observed_size, "配置描述符边界%s，声明长度=%lu",
                    source->reason == USBDIAG_REASON_MATCH ? "完整" : "无效",
                    (unsigned long)source->observed_values[0]);
        break;
    case USB_RULE_CFG_INTERFACE_COUNT:
        rt_snprintf(observed, observed_size,
                    "声明接口数=%lu，唯一接口号=%lu",
                    (unsigned long)source->expected_values[0],
                    (unsigned long)source->observed_values[0]);
        break;
    case USB_RULE_IF_ENDPOINT_COUNT:
        rt_snprintf(observed, observed_size,
                    "解析端点=%lu，计数不匹配项=%lu",
                    (unsigned long)source->observed_values[0],
                    (unsigned long)source->observed_values[1]);
        break;
    case USB_RULE_EP_PACKET_SIZE:
        rt_snprintf(observed, observed_size,
                    "端点数=%lu，包大小不符合项=%lu",
                    (unsigned long)source->expected_values[0],
                    (unsigned long)source->observed_values[0]);
        break;
    case USB_RULE_EP_INTERVAL:
        rt_snprintf(observed, observed_size,
                    "端点数=%lu，间隔不符合项=%lu",
                    (unsigned long)source->expected_values[0],
                    (unsigned long)source->observed_values[0]);
        break;
    case USB_RULE_EP0_STALL:
        rt_snprintf(observed, observed_size, "必要请求状态：%s",
                    source->observed_values[0] ? "STALL" : "未 STALL");
        break;
    default:
        rt_snprintf(observed, observed_size, "%s",
                    usbdiag_reason_name(source->reason));
        break;
    }
}

static void usb_evidence_project_core_results(void)
{
    rt_size_t source_index;
    if (!g_rule_session_active)
        return;

    for (source_index = 0U; source_index < USBDIAG_RULE_COUNT; source_index++)
    {
        enum usb_rule_index destination_index;
        const usbdiag_rule_result_t *source =
            &g_rule_session.rules[source_index];
        usb_evidence_rule_result_t *destination;
        char expected[USB_EVIDENCE_EXPECTED_MAX];
        char observed[USB_EVIDENCE_RULE_NOTE_MAX];
        rt_uint8_t ref_index;

        if (!usb_evidence_map_core_rule(source_index, &destination_index))
            continue;
        destination = &g_snapshot.rules[destination_index];
        usb_evidence_format_core_rule(destination_index, source,
                                      expected, sizeof(expected),
                                      observed, sizeof(observed));
        destination->verdict =
            usb_evidence_map_core_verdict(source->verdict);
        destination->event_sequence = 0U;
        destination->evidence_ref_count = 0U;
        destination->omitted_failure_reference_count =
            source->verdict == USBDIAG_FAIL ?
            source->omitted_failure_reference_count : 0U;
        rt_memset(destination->evidence_refs, 0,
                  sizeof(destination->evidence_refs));
        usb_evidence_copy_text(destination->expected,
                               sizeof(destination->expected), expected);
        usb_evidence_copy_text(destination->observed,
                               sizeof(destination->observed), observed);
        usb_evidence_copy_text(destination->reason_code,
                               sizeof(destination->reason_code),
                               usbdiag_reason_name(source->reason));
        for (ref_index = 0U;
             ref_index < source->reference_count &&
             destination->evidence_ref_count < USB_EVIDENCE_REFERENCE_COUNT;
             ref_index++)
        {
            const usbdiag_reference_t *reference = &source->references[ref_index];
            usb_evidence_rule_reference_add(
                destination, reference->event_sequence, reference->byte_offset,
                reference->byte_length,
                usbdiag_reference_role_name(reference->role));
        }
    }
}

static void usb_evidence_feed_core(const usb_evidence_raw_event_t *event)
{
    usbdiag_control_event_t input;
    if (!event || !g_rule_session_active ||
        event->event_type != USB_EVIDENCE_EVENT_CONTROL_TRANSFER)
        return;

    rt_memset(&input, 0, sizeof(input));
    input.session_id = event->session_id;
    input.attempt_id = event->attempt_id;
    input.sequence = event->sequence;
    input.speed = event->speed;
    input.has_setup = event->has_setup == RT_TRUE;
    input.setup.bm_request_type = event->setup_packet.bm_request_type;
    input.setup.request = event->setup_packet.b_request;
    input.setup.value = event->setup_packet.w_value;
    input.setup.index = event->setup_packet.w_index;
    input.setup.length = event->setup_packet.w_length;
    input.transfer_status = event->return_status >= 0 ?
        USBDIAG_TRANSFER_SUCCESS :
        event->return_status == -USB_ERR_STALL ?
        USBDIAG_TRANSFER_STALL : USBDIAG_TRANSFER_ERROR;
    input.capture_status =
        event->capture_status == USB_EVIDENCE_CAPTURE_COMPLETE ?
        USBDIAG_CAPTURE_COMPLETE :
        event->capture_status == USB_EVIDENCE_CAPTURE_LOCAL_TRUNCATED ?
        USBDIAG_CAPTURE_TRUNCATED :
        event->capture_status == USB_EVIDENCE_CAPTURE_NO_DATA_STAGE ?
        USBDIAG_CAPTURE_NO_DATA : USBDIAG_CAPTURE_UNAVAILABLE;
    input.transfer_length_known = event->transfer_length_known == RT_TRUE;
    input.transfer_length = event->transfer_length;
    input.captured_length = event->captured_length;
    input.payload = event->raw_response;
    input.payload_size = event->captured_length;
    (void)usbdiag_session_feed(&g_rule_session, &input);
    usb_evidence_project_core_results();
}

static rt_bool_t usb_evidence_sync_live_session_losses(void)
{
    rt_uint32_t session_id;
    rt_uint32_t dropped_events;
    rt_uint32_t dropped_bytes;

    usb_evidence_live_session_losses(&session_id, &dropped_events, &dropped_bytes);
    if (session_id == 0U || session_id != g_snapshot.session_id)
        return RT_FALSE;
    if (dropped_events <= g_snapshot.session_dropped_event_count &&
        dropped_bytes <= g_snapshot.session_dropped_payload_bytes)
        return RT_FALSE;

    g_snapshot.session_dropped_event_count = dropped_events;
    g_snapshot.session_dropped_payload_bytes = dropped_bytes;
    g_snapshot.dropped_event_count = usb_evidence_dropped_count();
    g_snapshot.dropped_payload_bytes = usb_evidence_dropped_payload_count();
    if (g_rule_session_active)
    {
        (void)usbdiag_session_mark_gap(&g_rule_session);
        usb_evidence_project_core_results();
    }
    g_snapshot.revision++;
    return RT_TRUE;
}

static void usb_evidence_reconcile_idle_queue(void)
{
    rt_ssize_t received;
    rt_uint32_t session_id;
    rt_uint32_t dropped_events;
    rt_uint32_t dropped_bytes;
    rt_uint8_t speed;

    /* Serialize the empty-queue check with producers. This lets the timer
     * account for a lost RESET or tail event even when no later USB event
     * arrives to wake the analysis thread. */
    if (!usb_evidence_producer_lock())
    {
        usb_evidence_try_publish(RT_FALSE);
        return;
    }
    received = rt_mq_recv(g_event_queue, &g_analysis_event,
                          sizeof(g_analysis_event), RT_WAITING_NO);
    usb_evidence_live_session_losses(&session_id, &dropped_events,
                                     &dropped_bytes);
    speed = g_current_speed;
    if (received == (rt_ssize_t)sizeof(g_analysis_event))
    {
        usb_evidence_producer_unlock();
        usb_evidence_process_event(&g_analysis_event);
        return;
    }
    if (received != -RT_ETIMEOUT)
    {
        usb_evidence_producer_unlock();
        usb_evidence_try_publish(RT_FALSE);
        return;
    }

    if (g_snapshot_lock &&
        rt_mutex_take(g_snapshot_lock, RT_WAITING_FOREVER) == RT_EOK)
    {
        if (session_id != 0U && session_id != g_snapshot.session_id &&
            dropped_events + dropped_bytes > 0U)
        {
            /* The RESET notification itself was dropped. Start a degraded
             * generation with a waiting timeline; never invent a USB event. */
            usb_evidence_snapshot_reset(session_id, speed);
            if (g_rule_session_active)
            {
                (void)usbdiag_session_mark_gap(&g_rule_session);
                usb_evidence_project_core_results();
            }
            g_snapshot.session_dropped_event_count = dropped_events;
            g_snapshot.session_dropped_payload_bytes = dropped_bytes;
            g_snapshot.dropped_event_count = usb_evidence_dropped_count();
            g_snapshot.dropped_payload_bytes =
                usb_evidence_dropped_payload_count();
            g_snapshot.session_state = USB_EVIDENCE_SESSION_DEGRADED;
            usb_evidence_copy_text(g_snapshot.phase, sizeof(g_snapshot.phase),
                                   "端口复位观测丢失");
            g_snapshot.revision++;
        }
        else if (session_id != 0U && session_id == g_snapshot.session_id)
        {
            (void)usb_evidence_sync_live_session_losses();
        }
        rt_mutex_release(g_snapshot_lock);
    }
    usb_evidence_producer_unlock();
    usb_evidence_try_publish(RT_FALSE);
}

static rt_bool_t usb_evidence_event_referenced(rt_uint32_t sequence)
{
    rt_size_t rule_index;

    if (sequence == 0U)
    {
        return RT_FALSE;
    }
    for (rule_index = 0U; rule_index < USB_EVIDENCE_RULE_COUNT; rule_index++)
    {
        const usb_evidence_rule_result_t *rule =
            &g_snapshot.rules[rule_index];
        rt_size_t reference_index;

        for (reference_index = 0U;
             reference_index < rule->evidence_ref_count; reference_index++)
        {
            if (rule->evidence_refs[reference_index].event_sequence == sequence)
            {
                return RT_TRUE;
            }
        }
    }
    if (g_rule_session_active)
    {
        uint32_t pinned[USBDIAG_SESSION_EVENT_CAPACITY];
        rt_uint16_t pinned_count = usbdiag_session_get_pins(
            &g_rule_session, pinned, USBDIAG_SESSION_EVENT_CAPACITY);
        rt_uint16_t pin_index;
        if (pinned_count > USBDIAG_SESSION_EVENT_CAPACITY)
            pinned_count = USBDIAG_SESSION_EVENT_CAPACITY;
        for (pin_index = 0U; pin_index < pinned_count; pin_index++)
            if (pinned[pin_index] == sequence)
                return RT_TRUE;
    }
    return RT_FALSE;
}

static void usb_evidence_append_raw_event(const usb_evidence_raw_event_t *event)
{
    rt_uint8_t index;
    rt_uint8_t victim;

    if (event->session_id == g_snapshot.session_id)
    {
        if (event->session_dropped_event_count >
            g_snapshot.session_dropped_event_count)
        {
            g_snapshot.session_dropped_event_count =
                event->session_dropped_event_count;
            if (g_rule_session_active)
                (void)usbdiag_session_mark_gap(&g_rule_session);
        }
        if (event->session_dropped_payload_bytes >
            g_snapshot.session_dropped_payload_bytes)
        {
            g_snapshot.session_dropped_payload_bytes =
                event->session_dropped_payload_bytes;
            if (g_rule_session_active)
                (void)usbdiag_session_mark_gap(&g_rule_session);
        }
    }

    if (g_snapshot.raw_event_count < USB_EVIDENCE_SNAPSHOT_EVENT_COUNT)
    {
        index = g_snapshot.raw_event_count++;
    }
    else
    {
        /* Keep every event referenced by a rule and evict an unreferenced
         * timeline entry when the bounded window is full. */
        victim = USB_EVIDENCE_SNAPSHOT_EVENT_COUNT;
        for (index = 0U; index < USB_EVIDENCE_SNAPSHOT_EVENT_COUNT; index++)
        {
            if (!usb_evidence_event_referenced(
                    g_snapshot.raw_events[index].sequence))
            {
                victim = index;
                break;
            }
        }
        if (victim >= USB_EVIDENCE_SNAPSHOT_EVENT_COUNT)
        {
            g_snapshot.raw_event_overflow_count++;
            g_snapshot.last_sequence = event->sequence;
            g_snapshot.dropped_event_count = usb_evidence_dropped_count();
            if (g_rule_session_active)
            {
                (void)usbdiag_session_mark_gap(&g_rule_session);
                usb_evidence_project_core_results();
            }
            g_snapshot.revision++;
            return;
        }
        if (g_rule_session_active)
        {
            (void)usbdiag_session_forget_event(
                &g_rule_session, g_snapshot.raw_events[victim].sequence);
            usb_evidence_project_core_results();
        }
        if (victim + 1U < USB_EVIDENCE_SNAPSHOT_EVENT_COUNT)
        {
            rt_memmove(&g_snapshot.raw_events[victim],
                       &g_snapshot.raw_events[victim + 1U],
                       (USB_EVIDENCE_SNAPSHOT_EVENT_COUNT - victim - 1U) *
                       sizeof(g_snapshot.raw_events[0]));
        }
        g_snapshot.raw_event_overflow_count++;
        index = USB_EVIDENCE_SNAPSHOT_EVENT_COUNT - 1U;
    }

    g_snapshot.raw_events[index] = *event;
    g_snapshot.last_sequence = event->sequence;
    g_snapshot.last_raw_sequence = event->sequence;
    g_snapshot.dropped_event_count = usb_evidence_dropped_count();
    g_snapshot.revision++;
}

static const char *usb_evidence_class_name(rt_uint8_t device_class)
{
    return usb_desc_class_name(device_class);
}

static void usb_evidence_utf16le_to_utf8(const rt_uint8_t *raw,
                                         rt_uint16_t length,
                                         char *text,
                                         rt_size_t text_size)
{
    rt_uint16_t offset;
    rt_size_t output = 0U;

    if (!text || text_size == 0U)
    {
        return;
    }
    text[0] = '\0';
    if (!raw || length < 2U)
    {
        return;
    }

    for (offset = 2U; offset + 1U < length && output + 1U < text_size; offset += 2U)
    {
        rt_uint16_t codepoint = usb_read_le16(raw + offset);

        if (codepoint < 0x80U)
        {
            text[output++] = (char)codepoint;
        }
        else if (codepoint < 0x800U && output + 2U < text_size)
        {
            text[output++] = (char)(0xC0U | (codepoint >> 6));
            text[output++] = (char)(0x80U | (codepoint & 0x3FU));
        }
        else if (codepoint >= 0xD800U && codepoint <= 0xDFFFU)
        {
            if (output + 1U >= text_size)
            {
                break;
            }
            text[output++] = '?';
        }
        else if (output + 3U < text_size)
        {
            text[output++] = (char)(0xE0U | (codepoint >> 12));
            text[output++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
            text[output++] = (char)(0x80U | (codepoint & 0x3FU));
        }
        else
        {
            break;
        }
    }
    text[output] = '\0';
}

/* Presentation-only descriptor walkers; rule decisions live in usbdiag_session.c. */

static void usb_evidence_handle_device_descriptor(
    const usb_evidence_raw_event_t *event)
{
    const rt_uint8_t *data = event->raw_response;
    rt_uint16_t length = event->captured_length;
    rt_uint16_t node_length;

    if (length == 0U)
        return;
    node_length = length;
    if (length >= 2U && data[0] >= 2U && data[0] < length)
        node_length = data[0];
    (void)usb_evidence_descriptor_add(event, 0U, node_length,
                                      USB_EVIDENCE_NODE_NONE, 0U);

    if (event->capture_status != USB_EVIDENCE_CAPTURE_COMPLETE ||
        length < 18U || data[0] < 18U || data[1] != 1U ||
        (event->setup_packet.w_value & 0xFFU) != 0U)
        return;

    g_context.manufacturer_index = data[14];
    g_context.product_index = data[15];
    g_context.serial_index = data[16];
    usb_evidence_copy_text(g_context.class_name, sizeof(g_context.class_name),
                           usb_evidence_class_name(data[4]));
    g_snapshot.device_present = RT_TRUE;
    g_snapshot.vendor_id = usb_read_le16(data + 8U);
    g_snapshot.product_id = usb_read_le16(data + 10U);
    g_snapshot.bcd_usb = usb_read_le16(data + 2U);
    g_snapshot.bcd_device = usb_read_le16(data + 12U);
    usb_evidence_copy_text(g_snapshot.class_name,
                           sizeof(g_snapshot.class_name),
                           g_context.class_name);
}

static void usb_evidence_build_configuration_tree(
    const usb_evidence_raw_event_t *event)
{
    const rt_uint8_t *data = event->raw_response;
    rt_uint16_t declared_length;
    rt_uint16_t offset;
    rt_uint16_t configuration_node;
    rt_uint16_t interface_node = USB_EVIDENCE_NODE_NONE;

    if (event->capture_status != USB_EVIDENCE_CAPTURE_COMPLETE ||
        event->captured_length < 9U || event->raw_response[0] < 9U ||
        event->raw_response[1] != 2U)
        return;

    declared_length = usb_read_le16(data + 2U);
    if (declared_length < 9U || declared_length > event->captured_length ||
        event->setup_packet.w_length < declared_length ||
        (event->transfer_length_known == RT_TRUE &&
         event->transfer_length != declared_length))
        return;

    configuration_node = usb_evidence_descriptor_add(
        event, 0U, 9U, USB_EVIDENCE_NODE_NONE, 0U);
    if (configuration_node == USB_EVIDENCE_NODE_NONE)
        return;

    offset = 9U;
    while (offset < declared_length)
    {
        rt_uint8_t descriptor_length;
        rt_uint8_t descriptor_type;
        rt_uint16_t parent_id;
        rt_uint8_t depth;

        if (offset + 2U > declared_length)
            break;
        descriptor_length = data[offset];
        descriptor_type = data[offset + 1U];
        if (descriptor_length < 2U ||
            (rt_uint32_t)offset + descriptor_length > declared_length)
            break;

        if (descriptor_type == 4U && descriptor_length >= 9U)
        {
            parent_id = configuration_node;
            depth = 1U;
        }
        else if (interface_node != USB_EVIDENCE_NODE_NONE)
        {
            parent_id = interface_node;
            depth = 2U;
        }
        else
        {
            parent_id = configuration_node;
            depth = 1U;
        }

        {
            rt_uint16_t node_id = usb_evidence_descriptor_add(
                event, offset, descriptor_length, parent_id, depth);
            if (node_id == USB_EVIDENCE_NODE_NONE)
                break;
            if (descriptor_type == 4U && descriptor_length >= 9U)
                interface_node = node_id;
        }
        offset = (rt_uint16_t)(offset + descriptor_length);
    }
}

static void usb_evidence_handle_configuration_descriptor(
    const usb_evidence_raw_event_t *event)
{
    usb_evidence_build_configuration_tree(event);
}

static void usb_evidence_handle_string_descriptor(const usb_evidence_raw_event_t *event)
{
    char text[128];
    rt_uint8_t index = (rt_uint8_t)(event->setup_packet.w_value & 0xFFU);
    rt_uint16_t length;

    if (event->capture_status != USB_EVIDENCE_CAPTURE_COMPLETE ||
        event->captured_length < 2U || event->raw_response[1] != 0x03U)
    {
        return;
    }
    length = event->raw_response[0];
    if (length < 2U || (length & 1U) != 0U ||
        length > event->captured_length)
    {
        return;
    }

    (void)usb_evidence_descriptor_add(event, 0U, length,
                                      USB_EVIDENCE_NODE_NONE, 0U);

    /* String index 0 is the USB Language ID table (for example 0x0409),
     * not a human-readable UTF-16 string.  The descriptor node above has
     * already rendered it as LANGID text; never feed its two-byte IDs into
     * the ordinary UTF-16-to-UTF-8 converter. */
    if (index == 0U)
    {
        return;
    }

    usb_evidence_utf16le_to_utf8(event->raw_response, length, text, sizeof(text));
    if (index == g_context.manufacturer_index)
    {
        usb_evidence_copy_text(g_context.manufacturer,
                               sizeof(g_context.manufacturer), text);
    }
    else if (index == g_context.product_index)
    {
        usb_evidence_copy_text(g_context.product, sizeof(g_context.product),
                               text);
    }
    else if (index == g_context.serial_index)
    {
        usb_evidence_copy_text(g_context.serial, sizeof(g_context.serial), text);
    }
    else
    {
        return;
    }

    usb_evidence_copy_text(g_snapshot.manufacturer,
                           sizeof(g_snapshot.manufacturer),
                           g_context.manufacturer);
    usb_evidence_copy_text(g_snapshot.product, sizeof(g_snapshot.product),
                           g_context.product);
    usb_evidence_copy_text(g_snapshot.serial, sizeof(g_snapshot.serial),
                           g_context.serial);

}

static void usb_evidence_handle_control(const usb_evidence_raw_event_t *event)
{
    rt_uint8_t descriptor_type;

    if (event->setup_packet.b_request == USB_REQUEST_GET_DESCRIPTOR &&
        event->setup_packet.bm_request_type == 0x80U)
    {
        descriptor_type = (rt_uint8_t)(event->setup_packet.w_value >> 8);
        if (descriptor_type == 0x01U)
        {
            usb_evidence_timeline_mark(
                event->setup_packet.w_length == 8U ?
                USB_EVIDENCE_STAGE_DEVICE_8B :
                USB_EVIDENCE_STAGE_DEVICE_18B,
                event);
            usb_evidence_handle_device_descriptor(event);
        }
        else if (descriptor_type == 0x02U)
        {
            usb_evidence_timeline_mark(USB_EVIDENCE_STAGE_CONFIGURATION,
                                       event);
            usb_evidence_handle_configuration_descriptor(event);
        }
        else if (descriptor_type == 0x03U)
        {
            usb_evidence_timeline_mark(USB_EVIDENCE_STAGE_STRINGS, event);
            usb_evidence_handle_string_descriptor(event);
        }
    }
    else if (event->setup_packet.b_request == 0x05U &&
             event->setup_packet.bm_request_type == 0x00U &&
             event->setup_packet.w_length == 0U)
    {
        usb_evidence_timeline_mark(USB_EVIDENCE_STAGE_SET_ADDRESS, event);
    }
    else if (event->setup_packet.b_request == USB_REQUEST_SET_CONFIGURATION &&
             event->setup_packet.bm_request_type == 0x00U &&
             event->setup_packet.w_length == 0U)
    {
        usb_evidence_timeline_mark(USB_EVIDENCE_STAGE_SET_CONFIGURATION,
                                   event);
    }
}

static void usb_evidence_process_event(const usb_evidence_raw_event_t *event)
{
    rt_uint32_t analysis_start = usb_perf_now();
    rt_bool_t cancel_report_pending = RT_FALSE;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    rt_uint32_t event_to_analysis_us;
    rt_uint32_t lock_wait_start;
    rt_uint32_t lock_acquired;
    rt_uint32_t lock_work_end;
    rt_uint32_t publish_start;
    rt_uint32_t publish_end;
    rt_uint32_t total_cycles;
    rt_uint32_t stage_start;
    rt_uint32_t snapshot_reset_cycles = 0U;
    rt_uint32_t feed_core_cycles = 0U;
    rt_uint32_t append_raw_cycles = 0U;
    rt_uint32_t handle_control_cycles = 0U;
    rt_uint64_t event_run_start_cycles = 0U;
    rt_uint64_t event_run_end_cycles = 0U;
    rt_uint64_t lock_run_start_cycles = 0U;
    rt_uint64_t lock_run_end_cycles = 0U;
    rt_uint32_t event_run_start_epoch = 0U;
    rt_uint32_t event_run_end_epoch = 0U;
    rt_uint32_t lock_run_start_epoch = 0U;
    rt_uint32_t lock_run_end_epoch = 0U;
    rt_uint32_t analysis_thread_run_cycles = 0U;
    rt_uint32_t lock_scheduled_cycles = 0U;
    rt_bool_t event_run_start_valid = RT_FALSE;
    rt_bool_t event_run_end_valid = RT_FALSE;
    rt_bool_t lock_run_start_valid = RT_FALSE;
    rt_bool_t lock_run_end_valid = RT_FALSE;
    rt_bool_t analysis_runtime_valid = RT_FALSE;
    rt_bool_t lock_runtime_valid = RT_FALSE;
#endif

    if (!event || !g_snapshot_lock)
    {
        return;
    }

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    event_run_start_valid = usb_evidence_scheduler_profile_snapshot(
        &event_run_start_cycles, &event_run_start_epoch);
    event_to_analysis_us = usb_evidence_timestamp_us_peek() -
                           event->timestamp_us;
    lock_wait_start = usb_perf_now();
#endif
    rt_mutex_take(g_snapshot_lock, RT_WAITING_FOREVER);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    lock_acquired = usb_perf_now();
    lock_run_start_valid = usb_evidence_scheduler_profile_snapshot(
        &lock_run_start_cycles, &lock_run_start_epoch);
#endif
    if (event->event_type == USB_EVIDENCE_EVENT_PORT_RESET)
    {
        g_pdf_pregen_pending = RT_FALSE;
        cancel_report_pending = RT_TRUE;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        stage_start = usb_perf_now();
#endif
        usb_evidence_snapshot_reset(event->session_id, event->speed);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        snapshot_reset_cycles += usb_perf_now() - stage_start;
#endif
        usb_evidence_timeline_mark(USB_EVIDENCE_STAGE_PORT_RESET, event);
    }
    else if (event->session_id != g_snapshot.session_id)
    {
        /* A full queue may drop the reset marker.  Re-anchor on the first
         * control event of the new session instead of silently discarding the
         * rest of that session's evidence. */
        if (event->event_type == USB_EVIDENCE_EVENT_CONTROL_TRANSFER)
        {
            g_pdf_pregen_pending = RT_FALSE;
            cancel_report_pending = RT_TRUE;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
            stage_start = usb_perf_now();
#endif
            usb_evidence_snapshot_reset(event->session_id, event->speed);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
            snapshot_reset_cycles += usb_perf_now() - stage_start;
#endif
            if (g_rule_session_active)
                (void)usbdiag_session_mark_gap(&g_rule_session);
        }
        else
        {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
            lock_run_end_valid = usb_evidence_scheduler_profile_snapshot(
                &lock_run_end_cycles, &lock_run_end_epoch);
            lock_work_end = usb_perf_now();
#endif
            rt_mutex_release(g_snapshot_lock);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
            usb_perf_record_us(USB_PERF_EVENT_TO_ANALYSIS,
                               event_to_analysis_us);
            usb_perf_record(USB_PERF_ANALYSIS_LOCK_WAIT,
                            lock_wait_start, lock_acquired);
            usb_perf_record(USB_PERF_ANALYSIS_LOCK_WORK,
                            lock_acquired, lock_work_end);
#endif
            return;
        }
    }

    if (event->event_type == USB_EVIDENCE_EVENT_CONTROL_TRANSFER)
    {
        /* Feed first so archive eviction can see the newly recorded witness
         * and the still-pending 8-byte/configuration headers. */
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        stage_start = usb_perf_now();
#endif
        usb_evidence_feed_core(event);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        feed_core_cycles = usb_perf_now() - stage_start;
        stage_start = usb_perf_now();
#endif
        usb_evidence_append_raw_event(event);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        append_raw_cycles += usb_perf_now() - stage_start;
        stage_start = usb_perf_now();
#endif
        usb_evidence_handle_control(event);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        handle_control_cycles = usb_perf_now() - stage_start;
#endif
        /* feed_core projected the updated rule state before append so event
         * eviction can protect current references. The remaining projection
         * points inside append handle overflow/eviction rule changes. */
    }
    else if (event->event_type == USB_EVIDENCE_EVENT_ENUMERATION_COMPLETE)
    {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        stage_start = usb_perf_now();
#endif
        usb_evidence_append_raw_event(event);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        append_raw_cycles += usb_perf_now() - stage_start;
#endif
        if (g_rule_session_active)
        {
            (void)usbdiag_session_end(&g_rule_session,
                                      event->return_status >= 0);
            usb_evidence_project_core_results();
        }
        g_snapshot.enumeration_complete = event->return_status >= 0;
        g_snapshot.is_final = RT_TRUE;
        g_pdf_pregen_pending = event->return_status >= 0 ? RT_TRUE : RT_FALSE;
        g_snapshot.session_state = event->return_status >= 0 ?
            USB_EVIDENCE_SESSION_FINAL : USB_EVIDENCE_SESSION_FAILED;
        usb_evidence_timeline_mark(USB_EVIDENCE_STAGE_ENUM_COMPLETE, event);
        usb_evidence_copy_text(g_snapshot.phase, sizeof(g_snapshot.phase),
                               event->return_status >= 0 ? "已完成" : "枚举失败");
    }
    else if (event->event_type == USB_EVIDENCE_EVENT_DISCONNECT)
    {
        rt_bool_t was_complete = g_snapshot.enumeration_complete;
        g_pdf_pregen_pending = RT_FALSE;
        cancel_report_pending = RT_TRUE;

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        stage_start = usb_perf_now();
#endif
        usb_evidence_append_raw_event(event);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        append_raw_cycles += usb_perf_now() - stage_start;
#endif
        if (g_rule_session_active)
        {
            (void)usbdiag_session_end(&g_rule_session, RT_FALSE);
            usb_evidence_project_core_results();
        }
        g_snapshot.enumeration_complete = RT_FALSE;
        g_snapshot.device_present = RT_FALSE;
        g_snapshot.is_final = RT_TRUE;
        g_snapshot.session_state = was_complete ?
            USB_EVIDENCE_SESSION_FINAL : USB_EVIDENCE_SESSION_FAILED;
        usb_evidence_copy_text(g_snapshot.phase, sizeof(g_snapshot.phase),
                               "设备已断开");
    }
    else
    {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        stage_start = usb_perf_now();
#endif
        usb_evidence_append_raw_event(event);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        append_raw_cycles += usb_perf_now() - stage_start;
#endif
        if (event->event_type == USB_EVIDENCE_EVENT_PORT_RESET)
            usb_evidence_project_core_results();
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    lock_run_end_valid = usb_evidence_scheduler_profile_snapshot(
        &lock_run_end_cycles, &lock_run_end_epoch);
    lock_work_end = usb_perf_now();
#endif
    rt_mutex_release(g_snapshot_lock);
    if (cancel_report_pending)
    {
        web_pdf_report_cancel_pending();
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    publish_start = usb_perf_now();
#endif
    usb_evidence_try_publish(
        event->event_type == USB_EVIDENCE_EVENT_PORT_RESET ||
        event->event_type == USB_EVIDENCE_EVENT_ENUMERATION_COMPLETE ||
        event->event_type == USB_EVIDENCE_EVENT_DISCONNECT);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    publish_end = usb_perf_now();
    event_run_end_valid = usb_evidence_scheduler_profile_snapshot(
        &event_run_end_cycles, &event_run_end_epoch);
    total_cycles = publish_end - analysis_start;
    if (event_run_start_valid && event_run_end_valid &&
        event_run_start_epoch == event_run_end_epoch &&
        event_run_end_cycles >= event_run_start_cycles &&
        (event_run_end_cycles - event_run_start_cycles) <= 0xFFFFFFFFULL)
    {
        analysis_thread_run_cycles = (rt_uint32_t)(
            event_run_end_cycles - event_run_start_cycles);
        analysis_runtime_valid = RT_TRUE;
    }
    if (lock_run_start_valid && lock_run_end_valid &&
        lock_run_start_epoch == lock_run_end_epoch &&
        lock_run_end_cycles >= lock_run_start_cycles &&
        (lock_run_end_cycles - lock_run_start_cycles) <= 0xFFFFFFFFULL)
    {
        lock_scheduled_cycles = (rt_uint32_t)(
            lock_run_end_cycles - lock_run_start_cycles);
        lock_runtime_valid = RT_TRUE;
    }
    usb_perf_record_us(USB_PERF_EVENT_TO_ANALYSIS, event_to_analysis_us);
    usb_perf_record(USB_PERF_ANALYSIS_LOCK_WAIT,
                    lock_wait_start, lock_acquired);
    usb_perf_record(USB_PERF_ANALYSIS_LOCK_WORK,
                    lock_acquired, lock_work_end);
    usb_perf_record(USB_PERF_ANALYSIS_PUBLISH,
                    publish_start, publish_end);
    if (snapshot_reset_cycles != 0U)
        usb_perf_record(USB_PERF_ANALYSIS_SNAPSHOT_RESET, 0U,
                        snapshot_reset_cycles);
    if (feed_core_cycles != 0U)
        usb_perf_record(USB_PERF_ANALYSIS_FEED_CORE, 0U,
                        feed_core_cycles);
    if (append_raw_cycles != 0U)
        usb_perf_record(USB_PERF_ANALYSIS_APPEND_RAW, 0U,
                        append_raw_cycles);
    if (handle_control_cycles != 0U)
        usb_perf_record(USB_PERF_ANALYSIS_HANDLE_CONTROL, 0U,
                        handle_control_cycles);
    if (analysis_runtime_valid)
        usb_perf_record(USB_PERF_ANALYSIS_THREAD_RUN, 0U,
                        analysis_thread_run_cycles);
    if (lock_runtime_valid)
        usb_perf_record(USB_PERF_ANALYSIS_LOCK_SCHEDULED, 0U,
                        lock_scheduled_cycles);
    usb_perf_record_analysis_detail(
        event->sequence, (rt_uint32_t)event->event_type,
        event_to_analysis_us, lock_acquired - lock_wait_start,
        lock_work_end - lock_acquired, publish_end - publish_start,
        total_cycles, event->captured_length, snapshot_reset_cycles,
        feed_core_cycles, append_raw_cycles, handle_control_cycles,
        analysis_thread_run_cycles, analysis_runtime_valid,
        lock_scheduled_cycles, lock_runtime_valid);
    usb_perf_record(USB_PERF_ANALYSIS_EVENT, analysis_start, publish_end);
#else
    usb_perf_record(USB_PERF_ANALYSIS_EVENT, analysis_start, usb_perf_now());
#endif
}

static void usb_evidence_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);
    while (1)
    {
        /* A timed receive publishes queue-overflow counters even when no
         * subsequent USB event arrives to wake the analysis thread. */
        rt_ssize_t received = rt_mq_recv(
            g_event_queue, &g_analysis_event, sizeof(g_analysis_event),
            rt_tick_from_millisecond(USB_EVIDENCE_WEB_PUBLISH_MS));
        if (received == (rt_ssize_t)sizeof(g_analysis_event))
        {
            usb_evidence_process_event(&g_analysis_event);
        }
        else if (received == -RT_ETIMEOUT)
        {
            usb_evidence_reconcile_idle_queue();
        }
        watchdog_supervisor_component_progress(
            WATCHDOG_COMPONENT_USB_ANALYZER,
            received == (rt_ssize_t)sizeof(g_analysis_event)
                ? WATCHDOG_COMPONENT_STAGE_ACTIVE
                : WATCHDOG_COMPONENT_STAGE_IDLE,
            ++s_watchdog_usb_progress);
    }
}

int usb_evidence_start(void)
{
    if (g_analysis_thread)
    {
        return RT_EOK;
    }

    usb_evidence_timer_init();
    g_boot_id = usb_evidence_make_boot_id();
    g_next_snapshot_id = 0U;
    g_current_session_id = 0U;
    g_current_attempt_id = 0U;
    g_next_sequence = 0U;
    g_pdf_pregen_pending = RT_FALSE;
    g_dropped_event_count = 0U;
    g_dropped_payload_bytes = 0U;
    g_snapshot_lock = rt_mutex_create("usb_snap", RT_IPC_FLAG_FIFO);
    if (!g_snapshot_lock)
    {
        return -RT_ENOMEM;
    }
    g_producer_lock = rt_mutex_create("usb_prod", RT_IPC_FLAG_FIFO);
    if (!g_producer_lock)
    {
        rt_mutex_delete(g_snapshot_lock);
        g_snapshot_lock = RT_NULL;
        return -RT_ENOMEM;
    }
    g_event_queue = &g_event_queue_object;
    if (rt_mq_init(g_event_queue, "usb_evt", g_event_pool,
                   sizeof(usb_evidence_raw_event_t), sizeof(g_event_pool),
                   RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        rt_mutex_delete(g_producer_lock);
        rt_mutex_delete(g_snapshot_lock);
        g_producer_lock = RT_NULL;
        g_event_queue = RT_NULL;
        g_snapshot_lock = RT_NULL;
        return -RT_ERROR;
    }

    rt_mutex_take(g_snapshot_lock, RT_WAITING_FOREVER);
    usb_evidence_snapshot_reset(0U, USB_SPEED_FULL);
    rt_mutex_release(g_snapshot_lock);

    g_analysis_thread = rt_thread_create("usb_diag", usb_evidence_thread_entry,
                                         RT_NULL, USB_EVIDENCE_STACK_SIZE,
                                         USB_EVIDENCE_PRIORITY,
                                         USB_EVIDENCE_TICK);
    if (!g_analysis_thread)
    {
        rt_mq_detach(g_event_queue);
        rt_mutex_delete(g_producer_lock);
        rt_mutex_delete(g_snapshot_lock);
        g_producer_lock = RT_NULL;
        g_event_queue = RT_NULL;
        g_snapshot_lock = RT_NULL;
        return -RT_ENOMEM;
    }
    if (rt_thread_startup(g_analysis_thread) != RT_EOK)
    {
        return -RT_ERROR;
    }
    watchdog_supervisor_component_expect(WATCHDOG_COMPONENT_USB_ANALYZER, 0);
    return RT_EOK;
}

void usb_evidence_record_port_reset(rt_uint8_t port, rt_uint8_t speed)
{
    if (!usb_evidence_producer_lock())
    {
        usb_evidence_drop_event();
        return;
    }
    {
        rt_base_t level = rt_hw_interrupt_disable();
        g_current_session_id++;
        g_current_attempt_id = 1U;
        g_current_speed = speed;
        g_session_drop_owner = g_current_session_id;
        g_session_dropped_event_count = 0U;
        g_session_dropped_payload_bytes = 0U;
        rt_hw_interrupt_enable(level);
    }
    usb_evidence_fill_header(&g_producer_event,
                             USB_EVIDENCE_EVENT_PORT_RESET, speed);
    g_producer_event.setup_packet.w_index = port;
    g_producer_event.port = port;
    g_producer_event.port_valid = RT_TRUE;
    g_producer_event.has_setup = RT_FALSE;
    g_producer_event.capture_status = USB_EVIDENCE_CAPTURE_NONE;
    usb_evidence_enqueue(&g_producer_event);
    usb_evidence_producer_unlock();
}

void usb_evidence_record_control(rt_uint8_t speed,
                                 rt_uint8_t port,
                                 rt_uint32_t started_at_us,
                                 rt_uint32_t completed_at_us,
                                 rt_bool_t timing_valid,
                                 const usb_evidence_setup_t *setup,
                                 rt_int32_t return_status,
                                 const rt_uint8_t *response)
{
    usb_evidence_raw_event_t *event = &g_producer_event;
    rt_uint16_t captured_length;
    rt_uint16_t capture_limit;
    rt_uint32_t transfer_length;

    if (!setup)
    {
        return;
    }

    if (!usb_evidence_producer_lock())
    {
        usb_evidence_drop_event();
        return;
    }

    usb_evidence_fill_header(event, USB_EVIDENCE_EVENT_CONTROL_TRANSFER,
                             speed);
    event->port = port;
    event->port_valid = RT_TRUE;
    event->timing_valid = timing_valid;
    if (timing_valid)
    {
        event->started_at_us = started_at_us;
        event->completed_at_us = completed_at_us;
        event->timestamp_us = completed_at_us;
        event->duration_us = completed_at_us - started_at_us;
    }
    event->has_setup = RT_TRUE;
    event->setup_packet = *setup;
    event->return_status = return_status;
    event->transfer_length_known = return_status >= 0;
    if (setup->w_length == 0U)
    {
        event->data_direction = USB_EVIDENCE_DATA_NONE;
    }
    else if ((setup->bm_request_type & 0x80U) != 0U)
    {
        event->data_direction = USB_EVIDENCE_DATA_DEVICE_TO_HOST;
    }
    else
    {
        event->data_direction = USB_EVIDENCE_DATA_HOST_TO_DEVICE;
    }

    if (setup->w_length == 0U)
    {
        event->transfer_length = 0U;
        event->captured_length = 0U;
        event->capture_status = USB_EVIDENCE_CAPTURE_NO_DATA_STAGE;
    }
    else if (return_status < 0)
    {
        event->capture_status = event->data_direction == USB_EVIDENCE_DATA_NONE ?
                               USB_EVIDENCE_CAPTURE_NONE :
                               USB_EVIDENCE_CAPTURE_UNAVAILABLE;
    }
    else if (event->data_direction != USB_EVIDENCE_DATA_NONE)
    {
        transfer_length = (rt_uint32_t)return_status;
        event->transfer_length = transfer_length > 0xFFFFU ?
                                0xFFFFU : (rt_uint16_t)transfer_length;
        captured_length = event->transfer_length;
        capture_limit = setup->w_length;
        if (capture_limit > USB_EVIDENCE_CAPTURE_MAX)
        {
            capture_limit = USB_EVIDENCE_CAPTURE_MAX;
        }
        if (captured_length > capture_limit)
        {
            captured_length = capture_limit;
            event->capture_status = USB_EVIDENCE_CAPTURE_TRUNCATED;
            usb_evidence_add_dropped_payload(
                event->transfer_length - captured_length);
        }
        else
        {
            event->capture_status = USB_EVIDENCE_CAPTURE_COMPLETE;
        }
        if (captured_length > 0U && !response)
        {
            usb_evidence_add_dropped_payload(captured_length);
            event->capture_status = USB_EVIDENCE_CAPTURE_UNAVAILABLE;
            captured_length = 0U;
        }
        if (captured_length > 0U)
        {
            rt_memcpy(event->raw_response, response, captured_length);
        }
        event->captured_length = captured_length;
    }
    else
    {
        event->transfer_length = 0U;
        event->captured_length = 0U;
        event->capture_status = USB_EVIDENCE_CAPTURE_NONE;
    }
    event->payload_crc32 = usb_evidence_bytes_crc32(
        event->raw_response, event->captured_length);
    usb_evidence_enqueue(event);
    usb_evidence_producer_unlock();
}

void usb_evidence_record_enumeration_complete(rt_int32_t return_status)
{
    if (!usb_evidence_producer_lock())
    {
        usb_evidence_drop_event();
        return;
    }
    usb_evidence_fill_header(&g_producer_event,
                             USB_EVIDENCE_EVENT_ENUMERATION_COMPLETE,
                             g_current_speed);
    g_producer_event.return_status = return_status;
    g_producer_event.capture_status = USB_EVIDENCE_CAPTURE_NONE;
    usb_evidence_enqueue(&g_producer_event);
    usb_evidence_producer_unlock();
}

void usb_evidence_record_disconnect(void)
{
    if (!usb_evidence_producer_lock())
    {
        usb_evidence_drop_event();
        return;
    }
    usb_evidence_fill_header(&g_producer_event,
                             USB_EVIDENCE_EVENT_DISCONNECT, g_current_speed);
    g_producer_event.has_setup = RT_FALSE;
    g_producer_event.capture_status = USB_EVIDENCE_CAPTURE_NONE;
    usb_evidence_enqueue(&g_producer_event);
    usb_evidence_producer_unlock();
}

void usb_evidence_set_attempt_id(rt_uint32_t attempt_id)
{
    rt_base_t level = rt_hw_interrupt_disable();

    g_current_attempt_id = attempt_id;
    rt_hw_interrupt_enable(level);
}

rt_bool_t usb_evidence_get_snapshot(usb_evidence_snapshot_t *snapshot)
{
    return web_snapshot_get_copy(snapshot);
}

rt_bool_t usb_evidence_get_snapshot_meta(rt_uint32_t *session_id,
                                         rt_uint32_t *revision)
{
    return web_snapshot_get_meta(session_id, revision);
}

const char *usb_evidence_session_state_name(usb_evidence_session_state_t state)
{
    switch (state)
    {
    case USB_EVIDENCE_SESSION_WAITING:
        return "WAITING";
    case USB_EVIDENCE_SESSION_RECORDING:
        return "RECORDING";
    case USB_EVIDENCE_SESSION_ANALYZING:
        return "ANALYZING";
    case USB_EVIDENCE_SESSION_FINAL:
        return "FINAL";
    case USB_EVIDENCE_SESSION_DEGRADED:
        return "DEGRADED";
    case USB_EVIDENCE_SESSION_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *usb_evidence_integrity_name(usb_evidence_integrity_t integrity)
{
    switch (integrity)
    {
    case USB_EVIDENCE_INTEGRITY_COMPLETE:
        return "COMPLETE";
    case USB_EVIDENCE_INTEGRITY_DEGRADED:
        return "DEGRADED";
    case USB_EVIDENCE_INTEGRITY_UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return "UNKNOWN";
    }
}

const char *usb_evidence_capture_status_name(
    usb_evidence_capture_status_t status)
{
    switch (status)
    {
    case USB_EVIDENCE_CAPTURE_COMPLETE:
        return "COMPLETE";
    case USB_EVIDENCE_CAPTURE_LOCAL_TRUNCATED:
        return "LOCAL_TRUNCATED";
    case USB_EVIDENCE_CAPTURE_NO_DATA_STAGE:
        return "NO_DATA_STAGE";
    case USB_EVIDENCE_CAPTURE_UNAVAILABLE:
        return "UNAVAILABLE";
    case USB_EVIDENCE_CAPTURE_NONE:
    default:
        return "NONE";
    }
}
