#include "startup_coordinator.h"
#include <rtthread.h>
#include <string.h>
#include <stdarg.h>
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#include <finsh.h>
#include "build_identity.h"
#include "usb_evidence.h"
#if defined(__arm__)
#include "whd_stm32_sdio.h"
#include "stm32h7rsxx_hal.h"
#endif
#endif

static struct rt_event s_wakeup;
static volatile uint32_t s_present, s_marks[BOOT_STAGE_COUNT];
static volatile uint32_t s_address, s_ui_iterations, s_wakeups;
static volatile rt_bool_t s_initialized, s_render_done, s_reload_done, s_display_failed;
static volatile rt_bool_t s_backlight_after_presented;
static rt_tick_t s_origin;
#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
/* One-shot correlation for the first submitted LVGL frame.  Store raw DWT
 * cycles and separate valid flags; no formatting or conversion runs in the
 * owner thread or the LTDC ISR. */
static volatile uint32_t s_ff_ui_ready_valid, s_ff_first_owner_valid;
static volatile uint32_t s_ff_submit_valid, s_ff_render_start_valid;
static volatile uint32_t s_ff_handler_return_valid, s_ff_nema_done_valid;
static volatile uint32_t s_ff_nema_stats_valid, s_ff_reload_valid;
static volatile uint32_t s_ff_ui_ready_cy, s_ff_first_owner_cy;
static volatile uint32_t s_ff_current_iteration, s_ff_current_owner_cy;
static volatile uint32_t s_ff_current_render_start_valid;
static volatile uint32_t s_ff_current_render_start_cy;
static volatile uint32_t s_ff_submit_iteration, s_ff_submit_owner_cy;
static volatile uint32_t s_ff_render_start_cy, s_ff_submit_cy;
static volatile uint32_t s_ff_handler_return_cy, s_ff_nema_done_cy;
static volatile uint32_t s_ff_nema_submits, s_ff_nema_wait_failures;
static volatile uint32_t s_ff_nema_max_frame_cycles, s_ff_reload_cy;
static volatile uint64_t s_ff_nema_wait_cycles;
#endif
#if defined(RT_USING_CPU_USAGE_TRACER)
static rt_tick_t s_firmware_idle_start, s_firmware_tick_start;
static uint32_t s_firmware_busy_permille;
#endif
#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static volatile rt_bool_t s_cpu_sampling;
static uint32_t s_cpu_last_cycle, s_firmware_cycle_busy_permille;
static uint64_t s_cpu_idle_cycles, s_cpu_total_cycles;
static uint64_t s_firmware_idle_cycles_start,s_firmware_total_cycles_start;
static rt_tick_t s_cpu_window_start;
static uint32_t s_boot_cpu_window_busy_permille,s_boot_cpu_window_ms;
static rt_bool_t s_scheduler_hook_active;
static rt_tick_t s_scheduler_hook_start;
static void boot_cpu_switch(rt_thread_t from, rt_thread_t to)
{
    uint32_t cycle = DWT->CYCCNT;

    usb_evidence_scheduler_profile_switch(from, to, cycle);
    if (!s_cpu_sampling)
    {
        if (s_scheduler_hook_active &&
            rt_tick_get() - s_scheduler_hook_start >=
                rt_tick_from_millisecond(8000))
        {
            usb_evidence_scheduler_profile_enable(RT_FALSE);
            s_scheduler_hook_active = RT_FALSE;
            rt_scheduler_sethook(RT_NULL);
        }
        return;
    }

    uint32_t delta=cycle-s_cpu_last_cycle;
    s_cpu_total_cycles+=delta;
    if(from==rt_cpu_self()->idle_thread)s_cpu_idle_cycles+=delta;
    s_cpu_last_cycle=cycle;
    if(rt_tick_get()-s_cpu_window_start >= rt_tick_from_millisecond(6000)){
        if(s_cpu_total_cycles)s_boot_cpu_window_busy_permille=(uint32_t)
            ((s_cpu_total_cycles-s_cpu_idle_cycles)*1000U/s_cpu_total_cycles);
        s_boot_cpu_window_ms=(uint32_t)((uint64_t)(rt_tick_get()-s_cpu_window_start)*1000U/RT_TICK_PER_SECOND);
        s_cpu_sampling=RT_FALSE;
        s_scheduler_hook_active = RT_FALSE;
        usb_evidence_scheduler_profile_enable(RT_FALSE);
        rt_scheduler_sethook(RT_NULL);
    }
}

static void boot_scheduler_hook_install(void)
{
    if (s_scheduler_hook_active)
    {
        return;
    }
    s_scheduler_hook_start = rt_tick_get();
    s_scheduler_hook_active = RT_TRUE;
    usb_evidence_scheduler_profile_enable(RT_TRUE);
    rt_scheduler_sethook(boot_cpu_switch);
}
#endif

static uint32_t now_ms(void)
{ return (uint32_t)((uint64_t)(rt_tick_get()-s_origin)*1000U/RT_TICK_PER_SECOND); }
int startup_preload_allowed(void)
{
    uint32_t marks = s_present;
    return (marks & (1UL << BOOT_FIRST_PRESENTED)) &&
        (marks & ((1UL << BOOT_TOUCH_READY) | (1UL << BOOT_TOUCH_FAILED))) &&
        (marks & ((1UL << BOOT_RADIO_READY) | (1UL << BOOT_RADIO_FAILED)));
}
void startup_init(void)
{
    if(s_initialized)return;
    s_origin=rt_tick_get();
    if(rt_event_init(&s_wakeup,"uiwake",RT_IPC_FLAG_FIFO)!=RT_EOK)return;
    s_initialized=RT_TRUE;startup_mark(BOOT_APP_ENTRY);
}
void startup_notify_ui(void)
{
    if(s_initialized){(void)rt_event_send(&s_wakeup,1U);}
}
void startup_wait_ui(uint32_t timeout_ms)
{
    rt_uint32_t received;
    if(!s_initialized){rt_thread_mdelay(timeout_ms?timeout_ms:1U);return;}
    if(rt_event_recv(&s_wakeup,1U,RT_EVENT_FLAG_OR|RT_EVENT_FLAG_CLEAR,
        rt_tick_from_millisecond((rt_int32_t)timeout_ms),&received)==RT_EOK)s_wakeups++;
}
void startup_mark(boot_stage_t stage)
{
    if(!s_initialized||(unsigned)stage>=BOOT_STAGE_COUNT)return;
    rt_base_t irq=rt_hw_interrupt_disable();
    uint32_t bit=1UL<<(unsigned)stage;
    rt_bool_t newly_marked=RT_FALSE;
    if(!(s_present&bit)){
#if defined(RT_USING_CPU_USAGE_TRACER)
        rt_thread_t idle = rt_cpu_self()->idle_thread;
        rt_tick_t idle_ticks = idle ? idle->user_time + idle->system_time : 0;
        if(stage==BOOT_FIRMWARE_BEGIN){
            s_firmware_idle_start=idle_ticks;s_firmware_tick_start=rt_tick_get();
        } else if(stage==BOOT_FIRMWARE_READY){
            rt_tick_t elapsed=rt_tick_get()-s_firmware_tick_start;
            rt_tick_t idle_delta=idle_ticks-s_firmware_idle_start;
            if(elapsed)s_firmware_busy_permille=1000U-(uint32_t)
                ((uint64_t)(idle_delta>elapsed?elapsed:idle_delta)*1000U/elapsed);
        }
#endif
#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        if(stage==BOOT_COMPONENTS_BEGIN &&
           (DWT->CTRL&DWT_CTRL_CYCCNTENA_Msk)){
            /* Install the existing single hook before component init starts
             * USB. This profiles analyzer scheduling without starting the
             * separate six-second boot CPU-busy window yet. */
            boot_scheduler_hook_install();
        } else if(stage==BOOT_RADIO_BEGIN &&
                  (DWT->CTRL&DWT_CTRL_CYCCNTENA_Msk)){
            boot_scheduler_hook_install();
            s_cpu_idle_cycles=s_cpu_total_cycles=0;
            s_cpu_window_start=rt_tick_get();
            s_cpu_last_cycle=DWT->CYCCNT;s_cpu_sampling=RT_TRUE;
        } else if(stage==BOOT_FIRMWARE_BEGIN && s_cpu_sampling){
            boot_cpu_switch(rt_thread_self(),rt_thread_self());
            s_firmware_idle_cycles_start=s_cpu_idle_cycles;
            s_firmware_total_cycles_start=s_cpu_total_cycles;
        } else if(stage==BOOT_FIRMWARE_READY && s_cpu_sampling){
            boot_cpu_switch(rt_thread_self(),rt_thread_self());
            uint64_t total=s_cpu_total_cycles-s_firmware_total_cycles_start;
            uint64_t idle=s_cpu_idle_cycles-s_firmware_idle_cycles_start;
            if(total)s_firmware_cycle_busy_permille=(uint32_t)((total-idle)*1000U/total);
        }
#endif
#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        if(stage==BOOT_UI_READY && !s_ff_ui_ready_valid){
            s_ff_ui_ready_cy=DWT->CYCCNT;
            s_ff_ui_ready_valid=1U;
        }
#endif
        if(stage==BOOT_BACKLIGHT)s_backlight_after_presented=(s_present&(1UL<<BOOT_FIRST_PRESENTED))!=0;
        s_marks[stage]=now_ms();s_present|=bit;newly_marked=RT_TRUE;
    }
    rt_hw_interrupt_enable(irq);
    if(newly_marked&&(stage==BOOT_TOUCH_READY||stage==BOOT_RADIO_READY||stage==BOOT_RADIO_FAILED||stage==BOOT_FIRST_PRESENTED))
        startup_notify_ui();
}
static void presented_if_complete(void)
{
    if(s_reload_done&&s_render_done)startup_mark(BOOT_FIRST_PRESENTED);
}
void startup_frame_submit(uint32_t address)
{
    rt_base_t irq=rt_hw_interrupt_disable();
    if((s_present&(1UL<<BOOT_UI_READY))&&!(s_present&(1UL<<BOOT_FIRST_SUBMIT))){
        s_address=address;
#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        s_ff_submit_iteration=s_ff_current_iteration;
        s_ff_submit_owner_cy=s_ff_current_owner_cy;
        s_ff_submit_cy=DWT->CYCCNT;
        s_ff_submit_valid=1U;
        if(s_ff_current_render_start_valid){
            s_ff_render_start_cy=s_ff_current_render_start_cy;
            s_ff_render_start_valid=1U;
        }
#endif
        startup_mark(BOOT_FIRST_SUBMIT);
    }
    rt_hw_interrupt_enable(irq);
}
void startup_frame_render_begin(uint32_t cycle)
{
#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if(!s_ff_submit_valid){
        s_ff_current_render_start_cy=cycle;
        s_ff_current_render_start_valid=1U;
    }
#else
    (void)cycle;
#endif
}
void startup_frame_handler_return(uint32_t cycle)
{
#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if(s_ff_submit_valid &&
       s_ff_submit_iteration==s_ff_current_iteration &&
       !s_ff_handler_return_valid){
        s_ff_handler_return_cy=cycle;
        s_ff_handler_return_valid=1U;
    }
#else
    (void)cycle;
#endif
}
void startup_frame_nema_metrics(uint32_t submits, uint64_t wait_cycles,
                                uint32_t wait_failures,
                                uint32_t max_frame_cycles)
{
#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if(s_ff_submit_valid &&
       s_ff_submit_iteration==s_ff_current_iteration &&
       !s_ff_nema_stats_valid){
        s_ff_nema_submits=submits;
        s_ff_nema_wait_cycles=wait_cycles;
        s_ff_nema_wait_failures=wait_failures;
        s_ff_nema_max_frame_cycles=max_frame_cycles;
        s_ff_nema_stats_valid=1U;
    }
#else
    (void)submits;
    (void)wait_cycles;
    (void)wait_failures;
    (void)max_frame_cycles;
#endif
}
void startup_frame_render_done(void)
{
    if(s_present&(1UL<<BOOT_FIRST_SUBMIT)){
#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        if(s_ff_submit_valid &&
           s_ff_submit_iteration==s_ff_current_iteration &&
           !s_ff_nema_done_valid){
            s_ff_nema_done_cy=DWT->CYCCNT;
            s_ff_nema_done_valid=1U;
        }
#endif
        s_render_done=RT_TRUE;presented_if_complete();
    }
}
void startup_ltdc_reload(uint32_t address)
{
    if((s_present&(1UL<<BOOT_FIRST_SUBMIT))&&address==s_address){
#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        if(s_ff_submit_valid && !s_ff_reload_valid){
            s_ff_reload_cy=DWT->CYCCNT;
            s_ff_reload_valid=1U;
        }
#endif
        s_reload_done=RT_TRUE;startup_mark(BOOT_FIRST_RELOAD);presented_if_complete();
    }
}
int startup_first_presented(void)
{ return (s_present&(1UL<<BOOT_FIRST_PRESENTED))!=0; }
void startup_ui_iteration(void)
{
#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    uint32_t iteration=++s_ui_iterations;
    uint32_t cycle=DWT->CYCCNT;
    s_ff_current_iteration=iteration;
    s_ff_current_owner_cy=cycle;
    s_ff_current_render_start_valid=0U;
    if((s_present&(1UL<<BOOT_UI_READY)) && !s_ff_first_owner_valid){
        s_ff_first_owner_cy=cycle;
        s_ff_first_owner_valid=1U;
    }
#else
    s_ui_iterations++;
#endif
}
int startup_radio_active(void)
{ return (s_present&(1UL<<BOOT_RADIO_BEGIN))&&!(s_present&((1UL<<BOOT_RADIO_READY)|(1UL<<BOOT_RADIO_FAILED))); }
int startup_display_gate_complete(void)
{ return startup_first_presented() || s_display_failed; }
int startup_radio_allowed(void){return startup_display_gate_complete();}
void startup_display_failed(void){s_display_failed=RT_TRUE;startup_notify_ui();}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static void boot_print_line(const char *format,...)
{
    char line[128];va_list args;va_start(args,format);
    rt_vsnprintf(line,sizeof(line),format,args);va_end(args);
    /* Private formatter scratch. Console text from other producers may
     * still interleave on this port; the receiver rejects and retries it. */
    rt_kputs(line);
}
static void boot_status_cmd(void)
{
    static const char *const names[BOOT_STAGE_COUNT]={
        "app_entry","components_begin","components_ready","dma_begin","dma_ready",
        "font_begin","font_ready","display_begin","display_ready","touch_begin",
        "touch_ready","touch_failed","ui_begin","ui_ready","first_submit",
        "first_reload","first_presented","backlight","radio_begin","firmware_begin",
        "firmware_ready","radio_ready","radio_failed","network_ready","usb_ready","config_begin","config_ready"};
    static char json[1536];static uint32_t request;
    uint32_t marks[BOOT_STAGE_COUNT], present, iterations;
#if defined(__arm__)
    uint32_t first_frame_mask=0U, first_frame_iteration=0U;
    uint32_t first_frame_ui_ready_cy=0U, first_frame_first_owner_cy=0U;
    uint32_t first_frame_submit_owner_cy=0U, first_frame_render_start_cy=0U;
    uint32_t first_frame_submit_cy=0U, first_frame_handler_return_cy=0U;
    uint32_t first_frame_nema_done_cy=0U, first_frame_reload_cy=0U;
    uint32_t first_frame_nema_submits=0U, first_frame_nema_wait_failures=0U;
    uint32_t first_frame_nema_max_frame_cycles=0U;
    uint64_t first_frame_nema_wait_cycles=0U;
#endif
    rt_base_t irq=rt_hw_interrupt_disable();
    present=s_present;iterations=s_ui_iterations;
    for(unsigned i=0;i<BOOT_STAGE_COUNT;i++)marks[i]=s_marks[i];
#if defined(__arm__)
    if(s_ff_ui_ready_valid){first_frame_mask|=1U<<0;first_frame_ui_ready_cy=s_ff_ui_ready_cy;}
    if(s_ff_first_owner_valid){first_frame_mask|=1U<<1;first_frame_first_owner_cy=s_ff_first_owner_cy;}
    if(s_ff_submit_valid){
        first_frame_mask|=1U<<2;
        first_frame_iteration=s_ff_submit_iteration;
        first_frame_submit_owner_cy=s_ff_submit_owner_cy;
        first_frame_submit_cy=s_ff_submit_cy;
    }
    if(s_ff_render_start_valid){first_frame_mask|=1U<<3;first_frame_render_start_cy=s_ff_render_start_cy;}
    if(s_ff_handler_return_valid){first_frame_mask|=1U<<4;first_frame_handler_return_cy=s_ff_handler_return_cy;}
    if(s_ff_nema_done_valid){first_frame_mask|=1U<<5;first_frame_nema_done_cy=s_ff_nema_done_cy;}
    if(s_ff_reload_valid){first_frame_mask|=1U<<6;first_frame_reload_cy=s_ff_reload_cy;}
    if(s_ff_nema_stats_valid){
        first_frame_mask|=1U<<7;
        first_frame_nema_submits=s_ff_nema_submits;
        first_frame_nema_wait_cycles=s_ff_nema_wait_cycles;
        first_frame_nema_wait_failures=s_ff_nema_wait_failures;
        first_frame_nema_max_frame_cycles=s_ff_nema_max_frame_cycles;
    }
#endif
    rt_hw_interrupt_enable(irq);
    int len=rt_snprintf(json,sizeof(json),"{\"schema\":\"startup-v1\",\"source_fingerprint\":\"%s\",\"present_mask\":%lu,\"ui_iterations\":%lu,\"wakeups\":%lu,\"backlight_after_presented\":%s,",
        WORK_SOURCE_FINGERPRINT,(unsigned long)present,(unsigned long)iterations,(unsigned long)s_wakeups,
        s_backlight_after_presented?"true":"false");
#if defined(RT_USING_CPU_USAGE_TRACER)
    len+=rt_snprintf(json+len,sizeof(json)-(unsigned)len,"\"firmware_busy_permille\":%lu,",
        (unsigned long)s_firmware_busy_permille);
#endif
#if defined(__arm__)
    whd_stm32_sdio_stats_t sdio;whd_stm32_sdio_get_stats(&sdio);
    len+=rt_snprintf(json+len,sizeof(json)-(unsigned)len,
        "\"sdio_spin_commands\":%lu,\"sdio_spin_data\":%lu,\"sdio_poll_yields\":%lu,",
        (unsigned long)sdio.spin_commands,(unsigned long)sdio.spin_data,(unsigned long)sdio.poll_yields);
    len+=rt_snprintf(json+len,sizeof(json)-(unsigned)len,"\"firmware_cycle_busy_permille\":%lu,",
        (unsigned long)s_firmware_cycle_busy_permille);
    len+=rt_snprintf(json+len,sizeof(json)-(unsigned)len,
        "\"boot_cpu_window_busy_permille\":%lu,\"boot_cpu_window_ms\":%lu,",
        (unsigned long)s_boot_cpu_window_busy_permille,(unsigned long)s_boot_cpu_window_ms);
#endif
#if defined(__arm__)
    len+=rt_snprintf(json+len,sizeof(json)-(unsigned)len,
        "\"first_frame\":{\"valid_mask\":%lu,\"submit_iteration\":%lu,\"core_hz\":%lu,\"ui_ready_cy\":%lu,\"first_owner_cy\":%lu,\"submit_owner_cy\":%lu,\"render_start_cy\":%lu,\"submit_cy\":%lu,\"handler_return_cy\":%lu,\"nema_done_cy\":%lu,\"reload_cy\":%lu,\"nema_submits\":%lu,\"nema_wait_cycles_hi\":%lu,\"nema_wait_cycles_lo\":%lu,\"nema_wait_failures\":%lu,\"nema_max_frame_cycles\":%lu},\"stages_ms\":{",
        (unsigned long)first_frame_mask,(unsigned long)first_frame_iteration,
        (unsigned long)SystemCoreClock,(unsigned long)first_frame_ui_ready_cy,
        (unsigned long)first_frame_first_owner_cy,
        (unsigned long)first_frame_submit_owner_cy,
        (unsigned long)first_frame_render_start_cy,
        (unsigned long)first_frame_submit_cy,
        (unsigned long)first_frame_handler_return_cy,
        (unsigned long)first_frame_nema_done_cy,
        (unsigned long)first_frame_reload_cy,
        (unsigned long)first_frame_nema_submits,
        (unsigned long)(first_frame_nema_wait_cycles>>32),
        (unsigned long)(first_frame_nema_wait_cycles&0xFFFFFFFFULL),
        (unsigned long)first_frame_nema_wait_failures,
        (unsigned long)first_frame_nema_max_frame_cycles);
#else
    len+=rt_snprintf(json+len,sizeof(json)-(unsigned)len,
        "\"first_frame\":{\"valid_mask\":0,\"submit_iteration\":0,\"core_hz\":0,\"ui_ready_cy\":0,\"first_owner_cy\":0,\"submit_owner_cy\":0,\"render_start_cy\":0,\"submit_cy\":0,\"handler_return_cy\":0,\"nema_done_cy\":0,\"reload_cy\":0,\"nema_submits\":0,\"nema_wait_cycles_hi\":0,\"nema_wait_cycles_lo\":0,\"nema_wait_failures\":0,\"nema_max_frame_cycles\":0},\"stages_ms\":{");
#endif
    for(unsigned i=0;i<BOOT_STAGE_COUNT&&len>0&&(unsigned)len<sizeof(json);i++){
        if(present&(1UL<<i))len+=rt_snprintf(json+len,sizeof(json)-(unsigned)len,
            "%s\"%s\":%lu",i?",":"",names[i],(unsigned long)marks[i]);
        else len+=rt_snprintf(json+len,sizeof(json)-(unsigned)len,"%s\"%s\":null",i?",":"",names[i]);
    }
    if(len<0||(unsigned)len+3>=sizeof(json)){rt_kprintf("[BOOT_STATUS] FAIL capacity\n");return;}
    len+=rt_snprintf(json+len,sizeof(json)-(unsigned)len,"}}");
    uint32_t req=++request;
    boot_print_line("[BOOT_STATUS] req=%lu bytes=%u crc32=%08lX\n",(unsigned long)req,
        (unsigned)len,(unsigned long)usb_evidence_bytes_crc32(json,(size_t)len));
    static const char digits[]="0123456789ABCDEF";
    for(unsigned offset=0;offset<(unsigned)len;offset+=16){
        unsigned count=(unsigned)len-offset;if(count>16)count=16;char hex[33];
        for(unsigned i=0;i<count;i++){uint8_t b=(uint8_t)json[offset+i];hex[2*i]=digits[b>>4];hex[2*i+1]=digits[b&15];}
        hex[2*count]=0;boot_print_line("[BOOT_STATUS_HEX] req=%lu offset=%u hex=%s\n",(unsigned long)req,offset,hex);
    }
    boot_print_line("[BOOT_STATUS_END] req=%lu PASS\n",(unsigned long)req);
}
MSH_CMD_EXPORT(boot_status_cmd, CRC-protected startup stages; no credentials);
#endif
