#include "rtthread_app.h"

#include <rtthread.h>
#include <finsh.h>
#include <drv_gpio.h>

#include <lvgl/lvgl.h>
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "ltdc.h"
#include "ltdc_irq_adapter.h"
#include "gpu2d_irq_adapter.h"
#include "dma2d_service.h"
#include "lvgl_nema_port.h"
#include "ui_main.h"
#include "ui_deferred.h"
#include "ui_fonts.h"
#include "usb_host_port.h"
#include "usb_evidence.h"
#include "wifi_service.h"
#include "rt_dma_h7rs.h"
#include "usb_perf.h"
#include "startup_coordinator.h"
#include "watchdog_supervisor.h"

#define LCD_BACKLIGHT_PIN       GET_PIN(G, 15)
#define LVGL_THREAD_STACK_SIZE  (16U * 1024U)
#define LVGL_THREAD_PRIORITY    5U
#define LVGL_THREAD_TIMESLICE   5U

/* Temporary isolation experiment; restore to zero after the WiFi-without-
 * graphics run. */
#define RTTHREAD_DIAGNOSTIC_DISABLE_GRAPHICS 0U

/* Frame profiling variables are shared with lv_port_disp.c and exposed by
 * the FinSH `fps` command.  They are only updated by the LVGL thread. */
#define TICKS_TO_US(ticks) \
    ((uint32_t)(ticks) / (SystemCoreClock / 1000000U))
volatile uint32_t s_fps_enable;
static uint32_t s_fps_frames;
static uint32_t s_fps_render_total;
static uint32_t s_fps_render_max;
static uint32_t s_fps_update_total;
static uint32_t s_fps_update_max;
uint32_t s_fps_flush_total;
uint32_t s_fps_flush_max;
uint32_t s_fps_flush_count;
uint32_t s_fps_flush_pixels_total;
uint32_t s_fps_flush_max_pixels;
int32_t s_fps_flush_last_x1;
int32_t s_fps_flush_last_y1;
int32_t s_fps_flush_last_x2;
int32_t s_fps_flush_last_y2;
static rt_tick_t s_fps_window_start;
static uint32_t s_fps_ltdc_reload_start;
static uint32_t s_fps_ltdc_frame_start;
static volatile rt_bool_t s_lvgl_refresh_request;
static void lvgl_timer_resume(void *unused){RT_UNUSED(unused);startup_notify_ui();}

static uint32_t cycles_to_us64(uint64_t cycles)
{
    if (SystemCoreClock == 0U)
    {
        return 0U;
    }
    return (uint32_t)((cycles * 1000000ULL + (SystemCoreClock / 2U)) /
                      SystemCoreClock);
}

static uint32_t bytes_per_second_mib_x10(uint64_t bytes,
                                         uint32_t elapsed_ms)
{
    if (elapsed_ms == 0U)
    {
        return 0U;
    }
    return (uint32_t)((bytes * 10000ULL) /
                      ((uint64_t)elapsed_ms * 1024ULL * 1024ULL));
}

static void lvgl_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);
    rt_bool_t backlight_on=RT_FALSE, limited=RT_FALSE;
    uint32_t watchdog_progress=0U;

    /* CYCCNT was initialized once before components and remains shared by
     * profiling consumers. LVGL itself uses the RT-Thread tick callback. */

    lv_init();
    /* This must be set after lv_init(), which resets LVGL global state. */
    lv_tick_set_cb((lv_tick_get_cb_t)rt_tick_get);
    lv_timer_handler_set_resume_cb(lvgl_timer_resume,RT_NULL);
    startup_mark(BOOT_FONT_BEGIN);ui_font_gpu_init();startup_mark(BOOT_FONT_READY);

    startup_mark(BOOT_DISPLAY_BEGIN);
    if (lvgl_disp_init() != 0)
    {
        watchdog_supervisor_component_progress(
            WATCHDOG_COMPONENT_LVGL, WATCHDOG_COMPONENT_STAGE_ERROR, 1U);
        rt_kprintf("[LVGL] display init failed\r\n");
        startup_display_failed();return;
    }
    __HAL_LTDC_ENABLE(&hltdc);
    if(ltdc_frame_monitor_start()!=0){
        watchdog_supervisor_component_progress(
            WATCHDOG_COMPONENT_LVGL, WATCHDOG_COMPONENT_STAGE_ERROR, 2U);
        startup_display_failed();return;
    }
    watchdog_supervisor_display_expect();
    startup_mark(BOOT_DISPLAY_READY);startup_mark(BOOT_TOUCH_BEGIN);
    if (lvgl_indev_init() != 0)
    {
        startup_mark(BOOT_TOUCH_FAILED);
        rt_kprintf("[LVGL] GT911 input init failed\r\n");
    }

    startup_mark(BOOT_UI_BEGIN);ui_main_init();startup_mark(BOOT_UI_READY);
    rt_kprintf("[LVGL] UI ready\r\n");

    for (;;)
    {
        uint32_t update_start = DWT->CYCCNT;
        uint32_t render_start;
        uint32_t done;
        uint32_t next_wait;
        startup_ui_iteration();
        rt_bool_t desired_limited=startup_radio_active()?RT_TRUE:RT_FALSE;
        if(desired_limited!=limited){
            limited=desired_limited;
            lv_timer_t *refresh=lv_display_get_refr_timer(lv_display_get_default());
            if(refresh)lv_timer_set_period(refresh,limited?34U:LV_DEF_REFR_PERIOD);
            ui_main_set_startup_limited(limited!=RT_FALSE);
        }

        /* Bracket the complete LVGL iteration, including any one-time
         * descriptor canvas generation done by ui_main_update(). */
        lvgl_nema_frame_begin();
        dma2d_service_frame_begin();

        if (s_lvgl_refresh_request)
        {
            s_lvgl_refresh_request = RT_FALSE;
            lv_obj_invalidate(lv_screen_active());
        }

        /* Drain the non-blocking GT911 event queue in the sole LVGL owner
         * thread before rendering.  The I2C transaction itself runs in the
         * RT-Thread touch worker, so this cannot stall a frame. */
        lvgl_indev_poll();

        /* Apply application state before LVGL enters the direct-buffer
         * synchronization wait.  Descriptor scrolling only updates LVGL
         * objects and off-screen PSRAM canvases; doing it here lets the
         * following handler wait for the next blanking window first, then
         * render the already-prepared state.  This keeps the state update
         * out of the one-frame deadline without changing ownership or
         * buffer synchronization. */
        ui_main_update();
        render_start = DWT->CYCCNT;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        startup_frame_render_begin(render_start);
#endif
        next_wait=lv_timer_handler();
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        startup_frame_handler_return(DWT->CYCCNT);
#endif
        /* A CLICKED callback only enqueues the business operation. Commit it
         * after LVGL has rendered the released state, matching the posted
         * performClick behavior of mobile UI toolkits. */
        ui_deferred_process();
        done = DWT->CYCCNT;
        usb_perf_record(USB_PERF_LVGL_UPDATE, update_start, render_start);
        usb_perf_record(USB_PERF_LVGL_RENDER, render_start, done);

        dma2d_service_frame_end();
        {
            uint32_t nema_wait_start = DWT->CYCCNT;
            lvgl_nema_frame_end();
            lvgl_nema_stats_t first_stats;lvgl_nema_stats_get(&first_stats);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
            startup_frame_nema_metrics(first_stats.submits,
                                       first_stats.total_wait_cycles,
                                       first_stats.wait_failures,
                                       first_stats.max_frame_cycles);
#endif
            if(first_stats.wait_failures==0U&&gpu2d_error_count()==0U)
                startup_frame_render_done();
            usb_perf_record(USB_PERF_NEMA_FRAME_WAIT, nema_wait_start,
                            DWT->CYCCNT);
        }

        if (s_fps_enable)
        {
            uint32_t update = render_start - update_start;
            uint32_t render = done - render_start;

            s_fps_frames++;
            s_fps_render_total += render;
            s_fps_update_total += update;
            if (render > s_fps_render_max)
            {
                s_fps_render_max = render;
            }
            if (update > s_fps_update_max)
            {
                s_fps_update_max = update;
            }
        }

        if(!backlight_on&&startup_first_presented()){
            rt_pin_write(LCD_BACKLIGHT_PIN,PIN_HIGH);
            startup_mark(BOOT_BACKLIGHT);backlight_on=RT_TRUE;
        }
        /* Single-owner cooperative preload, AFTER input/business/render and
         * GPU completion. A hidden construction slice does not draw or start
         * WiFi work. Recompute the timer deadline as style work may arm one. */
        bool first_presented = startup_first_presented() != 0;
        bool constructed = ui_main_nav_icons_step(first_presented);
        if (!constructed)
            constructed = ui_main_home_timeline_step(first_presented);
        if (!constructed)
            constructed = ui_main_preload_step(startup_preload_allowed()!=0,
                                                lv_timer_get_time_until_next());
        /* Object construction can resume an LVGL timer and self-signal the
         * coalesced wake event. An event wait alone then returns immediately,
         * starving lower-priority input/radio/console workers. A finite-build
         * slice must really block; a scheduler yield alone is insufficient. */
        if (constructed) rt_thread_mdelay(2);
        watchdog_supervisor_component_progress(
            WATCHDOG_COMPONENT_LVGL, WATCHDOG_COMPONENT_STAGE_ACTIVE,
            ++watchdog_progress);
        next_wait = lv_timer_get_time_until_next();
        if (constructed && next_wait > 4U) next_wait = 4U;
        if(next_wait==LV_NO_TIMER_READY||next_wait>20U)next_wait=20U;
        if(next_wait==0U)next_wait=1U;
        startup_wait_ui(next_wait);
    }
}

static int lvgl_thread_start(void)
{
    rt_thread_t thread = rt_thread_create("lvgl", lvgl_thread_entry,
                                          RT_NULL, LVGL_THREAD_STACK_SIZE,
                                          LVGL_THREAD_PRIORITY,
                                          LVGL_THREAD_TIMESLICE);
    if (thread == RT_NULL)
    {
        rt_kprintf("[LVGL] thread create failed\r\n");
        return -RT_ENOMEM;
    }
    watchdog_supervisor_component_expect(WATCHDOG_COMPONENT_LVGL, 1);
    {
        int result = rt_thread_startup(thread);
        if (result != RT_EOK)
        {
            watchdog_supervisor_component_progress(
                WATCHDOG_COMPONENT_LVGL, WATCHDOG_COMPONENT_STAGE_ERROR,
                (uint32_t)result);
        }
        return result;
    }
}

void rtthread_app_main(void)
{
    int usb_status;
    int lvgl_status;
    int dma2d_status;
    uint32_t core_progress = 0U;
    rt_tick_t first_present_wait_start=0;

    rt_kprintf("[APP] LVGL application thread\r\n");

    watchdog_supervisor_boot_phase_begin(WATCHDOG_BOOT_OPERATION_USB_START,
                                         30000U);

    startup_mark(BOOT_DMA_BEGIN);
    if (rt_dma_h7rs_init() != RT_EOK)
    {
        rt_kprintf("[DMA] RT-Thread DMA contract initialization failed\r\n");
    }
    watchdog_supervisor_operation_progress(1U, 1U);

    startup_mark(BOOT_DMA_READY);
    /* CubeMX owns peripheral and MPU initialization; RT-Thread owns the
     * application side of the pins and display after the scheduler is
     * running. */
#if RTTHREAD_DIAGNOSTIC_DISABLE_GRAPHICS
    /* MX_LTDC_Init() has already configured the peripheral before the
     * scheduler starts.  Turn its scanout off for the isolation run so the
     * test exercises WHD/SDIO without a second continuous PSRAM master. */
    __HAL_LTDC_DISABLE(&hltdc);
    rt_kprintf("[LVGL] diagnostic graphics disabled\r\n");
#else
    /* HAL configured LTDC already. Stop fetching NOLOAD pixels until the
     * owner has cleared both buffers and installed its refresh callbacks. */
    rt_pin_write(LCD_BACKLIGHT_PIN,PIN_LOW);
    __HAL_LTDC_DISABLE(&hltdc);

    dma2d_status = dma2d_service_init();
    if (dma2d_status != RT_EOK)
    {
        rt_kprintf("[DMA2D] service init failed, status=%d\r\n", dma2d_status);
    }

    first_present_wait_start = rt_tick_get();
    lvgl_status = lvgl_thread_start();
    if (lvgl_status != RT_EOK)
    {
        startup_display_failed();
        rt_kprintf("[LVGL] startup failed, status=%d\r\n", lvgl_status);
    }
    watchdog_supervisor_operation_progress(2U, (uint32_t)lvgl_status);
#endif

#if !RTTHREAD_DIAGNOSTIC_DISABLE_GRAPHICS
    if (lvgl_status == RT_EOK)
    {
        /* Let the LVGL owner present its first valid frame before the
         * application thread starts USB host initialization. A terminal
         * display failure releases the gate immediately; the timeout keeps
         * USB startup bounded when presentation stalls. */
        const rt_tick_t present_wait_limit = rt_tick_from_millisecond(300U);
        while (!startup_display_gate_complete() &&
               rt_tick_get() - first_present_wait_start < present_wait_limit)
        {
            rt_thread_mdelay(10U);
        }
    }
    watchdog_supervisor_operation_progress(3U,
                                           startup_display_gate_complete() ? 1U : 0U);
#endif

    /* Start the evidence recorder before the Host controller itself.  The
     * usb_host_start() wrapper is idempotent and performs that ordering. */
    usb_status = usb_host_start();
    watchdog_supervisor_operation_progress(4U, (uint32_t)usb_status);
    if(usb_status==RT_EOK)startup_mark(BOOT_USB_READY);
    if (usb_status != RT_EOK)
    {
        rt_kprintf("[USB] host unavailable, status=%d\r\n", usb_status);
    }
    watchdog_supervisor_operation_end((uint32_t)usb_status);
    watchdog_supervisor_core_ready();

    for (;;)
    {
        /* This is the application coordinator's periodic core-liveness turn.
         * It is above optional USB/WiFi/display workers and does not require
         * any optional service to be online. */
        rt_thread_mdelay(1000U);
        watchdog_supervisor_core_progress(++core_progress);
    }
}

static void fps_cmd(int argc, char **argv)
{
    rt_tick_t now;
    rt_uint32_t elapsed_ms;
    rt_uint32_t loop_x10;
    rt_uint32_t updates_x10;
    rt_uint32_t reload_count;
    rt_uint32_t reload_x10;
    rt_uint32_t frame_count;
    rt_uint32_t frame_x10;
    rt_uint32_t swap_ratio_x10;
    rt_uint32_t gpu_avg_us;
    rt_uint32_t dma_avg_us;
    rt_uint32_t gpu_max_us;
    rt_uint32_t dma_max_us;
    rt_uint32_t lt_bandwidth_x10;
    rt_uint32_t gpu_bandwidth_x10;
    rt_uint32_t dma_bandwidth_x10;
    rt_uint32_t total_bandwidth_x10;
    const char *refresh_check;
    const char *gpu_check;
    const char *dma_check;
    const char *ltdc_check;
    uint64_t lt_bytes;
    uint64_t total_bytes;
    lvgl_nema_stats_t gpu_stats;
    dma2d_service_stats_t dma_stats;

    if (argc == 2 && rt_strcmp(argv[1], "on") == 0)
    {
        s_fps_enable = 1U;
        s_fps_frames = 0U;
        s_fps_render_total = 0U;
        s_fps_render_max = 0U;
        s_fps_update_total = 0U;
        s_fps_update_max = 0U;
        s_fps_flush_total = 0U;
        s_fps_flush_max = 0U;
        s_fps_flush_count = 0U;
        s_fps_flush_pixels_total = 0U;
        s_fps_flush_max_pixels = 0U;
        s_fps_flush_last_x1 = 0;
        s_fps_flush_last_y1 = 0;
        s_fps_flush_last_x2 = 0;
        s_fps_flush_last_y2 = 0;
        s_fps_window_start = rt_tick_get();
        s_fps_ltdc_reload_start = ltdc_reload_count();
        s_fps_ltdc_frame_start = ltdc_frame_count();
        lvgl_nema_stats_reset();
        dma2d_service_stats_reset();
        rt_kprintf("[fps] profiling on\r\n");
        return;
    }
    if (argc == 2 && rt_strcmp(argv[1], "off") == 0)
    {
        s_fps_enable = 0U;
        rt_kprintf("[fps] profiling off\r\n");
        return;
    }

    now = rt_tick_get();
    elapsed_ms = (rt_uint32_t)(now - s_fps_window_start);
    if (s_fps_frames == 0U || elapsed_ms == 0U)
    {
        rt_kprintf("[fps] run `fps on`, exercise the UI, then `fps`\r\n");
        return;
    }

    loop_x10 = (rt_uint32_t)(s_fps_frames * 10000U / elapsed_ms);
    updates_x10 = (rt_uint32_t)(s_fps_flush_count * 10000U / elapsed_ms);
    rt_kprintf("[fps] loop=%u.%u/s flush-callbacks=%u.%u/s elapsed=%ums\r\n",
               (unsigned)(loop_x10 / 10U), (unsigned)(loop_x10 % 10U),
               (unsigned)(updates_x10 / 10U),
               (unsigned)(updates_x10 % 10U), (unsigned)elapsed_ms);
    rt_kprintf("[fps] render avg=%uus max=%uus; update avg=%uus max=%uus\r\n",
               (unsigned)TICKS_TO_US(s_fps_render_total / s_fps_frames),
               (unsigned)TICKS_TO_US(s_fps_render_max),
               (unsigned)TICKS_TO_US(s_fps_update_total / s_fps_frames),
               (unsigned)TICKS_TO_US(s_fps_update_max));
    if (s_fps_flush_count != 0U)
    {
        rt_kprintf("[fps] flush avg=%uus max=%uus\r\n",
                   (unsigned)TICKS_TO_US(s_fps_flush_total /
                                          s_fps_flush_count),
                   (unsigned)TICKS_TO_US(s_fps_flush_max));
        rt_kprintf("[fps] dirty avg=%lu px max=%lu last=(%ld,%ld)-(%ld,%ld)\r\n",
                   (unsigned long)(s_fps_flush_pixels_total /
                                   s_fps_flush_count),
                   (unsigned long)s_fps_flush_max_pixels,
                   (long)s_fps_flush_last_x1, (long)s_fps_flush_last_y1,
                   (long)s_fps_flush_last_x2, (long)s_fps_flush_last_y2);
    }

    lvgl_nema_stats_get(&gpu_stats);
    dma2d_service_stats_get(&dma_stats);
    reload_count = ltdc_reload_count() - s_fps_ltdc_reload_start;
    reload_x10 = (rt_uint32_t)(reload_count * 10000ULL / elapsed_ms);
    frame_count = ltdc_frame_count() - s_fps_ltdc_frame_start;
    frame_x10 = (rt_uint32_t)(frame_count * 10000ULL / elapsed_ms);
    gpu_avg_us = gpu_stats.active_frames == 0U ? 0U :
                 cycles_to_us64(gpu_stats.total_wait_cycles /
                                gpu_stats.active_frames);
    dma_avg_us = dma_stats.active_frames == 0U ? 0U :
                 cycles_to_us64(dma_stats.total_cycles /
                                dma_stats.active_frames);
    gpu_max_us = cycles_to_us64(gpu_stats.max_frame_cycles);
    dma_max_us = cycles_to_us64(dma_stats.max_frame_cycles);

    /* LTDC scanout is a known RGB565 read from the display PSRAM arena.
     * GPU2D/DMA2D traffic is estimated from submitted areas; this is useful
     * for budgeting bus pressure without presenting it as a silicon counter.
     */
    lt_bytes = (uint64_t)frame_count * 800ULL * 480ULL * 2ULL;
    lt_bandwidth_x10 = bytes_per_second_mib_x10(lt_bytes, elapsed_ms);
    gpu_bandwidth_x10 = bytes_per_second_mib_x10(gpu_stats.total_bytes,
                                                 elapsed_ms);
    dma_bandwidth_x10 = bytes_per_second_mib_x10(dma_stats.total_bytes,
                                                 elapsed_ms);
    total_bytes = lt_bytes + gpu_stats.total_bytes + dma_stats.total_bytes;
    total_bandwidth_x10 = bytes_per_second_mib_x10(total_bytes, elapsed_ms);

    if (frame_count == 0U || reload_count == 0U)
    {
        swap_ratio_x10 = 0U;
        refresh_check = "NO_SAMPLE";
    }
    else
    {
        swap_ratio_x10 = (rt_uint32_t)(reload_count * 1000ULL / frame_count);
        /* A framebuffer swap occurs only when LVGL has a new frame to show;
         * it is not required to happen on every LTDC panel scan. Treat swaps
         * above the physical scan count as suspicious, but report a lower
         * swap/scan ratio as measured cadence rather than synchronization loss. */
        refresh_check = (uint64_t)reload_count * 100ULL <=
                        (uint64_t)frame_count * 105ULL ? "PASS" : "CHECK";
    }
    gpu_check = (gpu2d_error_count() == 0U &&
                 gpu_stats.wait_failures == 0U) ? "PASS" : "CHECK";
    dma_check = (dma2d_service_error_count() == 0U &&
                 dma_stats.errors == 0U) ? "PASS" : "CHECK";
    ltdc_check = hltdc.ErrorCode == 0U ? "PASS" : "CHECK";

    rt_kprintf("[fps] panel scans=%u rate=%u.%uHz\r\n",
               (unsigned)frame_count,
               (unsigned)(frame_x10 / 10U), (unsigned)(frame_x10 % 10U));
    rt_kprintf("[fps] buffer swaps=%u rate=%u.%uHz swap/scan=%u.%u%%\r\n",
               (unsigned)reload_count,
               (unsigned)(reload_x10 / 10U), (unsigned)(reload_x10 % 10U),
               (unsigned)(swap_ratio_x10 / 10U),
               (unsigned)(swap_ratio_x10 % 10U));
    rt_kprintf("[fps] LTDC error=0x%08lx GPU2D error=%lu waiterr=%u\r\n",
               (unsigned long)hltdc.ErrorCode,
               (unsigned long)gpu2d_error_count(),
               (unsigned)gpu_stats.wait_failures);
    rt_kprintf("[fps] GPU2D active=%u submits=%u wait/frame=%uus max=%uus "
               "est=%u.%uMiB/s\r\n",
               (unsigned)gpu_stats.active_frames,
               (unsigned)gpu_stats.submits,
               (unsigned)gpu_avg_us, (unsigned)gpu_max_us,
               (unsigned)(gpu_bandwidth_x10 / 10U),
               (unsigned)(gpu_bandwidth_x10 % 10U));
    rt_kprintf("[fps] DMA2D active=%u jobs=%u wait/frame=%uus max=%uus "
               "error=%u hal_errors=%lu est=%u.%uMiB/s\r\n",
               (unsigned)dma_stats.active_frames,
               (unsigned)dma_stats.jobs,
               (unsigned)dma_avg_us, (unsigned)dma_max_us,
               (unsigned)dma_stats.errors,
               (unsigned long)dma2d_service_error_count(),
               (unsigned)(dma_bandwidth_x10 / 10U),
               (unsigned)(dma_bandwidth_x10 % 10U));
    rt_kprintf("[fps] PSRAM estimated traffic: LTDC=%u.%u + GPU2D=%u.%u "
               "+ DMA2D=%u.%u = %u.%uMiB/s\r\n",
               (unsigned)(lt_bandwidth_x10 / 10U),
               (unsigned)(lt_bandwidth_x10 % 10U),
               (unsigned)(gpu_bandwidth_x10 / 10U),
               (unsigned)(gpu_bandwidth_x10 % 10U),
               (unsigned)(dma_bandwidth_x10 / 10U),
               (unsigned)(dma_bandwidth_x10 % 10U),
               (unsigned)(total_bandwidth_x10 / 10U),
               (unsigned)(total_bandwidth_x10 % 10U));
    rt_kprintf("[fps] check buffer-swap-sanity=%s GPU2D=%s DMA2D=%s LTDC=%s\r\n",
               refresh_check, gpu_check, dma_check, ltdc_check);
    rt_kprintf("[fps] swap sanity allows <= scans +5%%; NO_SAMPLE means no buffer swap\r\n");
}
MSH_CMD_EXPORT(fps_cmd, LVGL frame timing: fps [on|off]);

static void lvgl_refresh_cmd(void)
{
    s_lvgl_refresh_request = RT_TRUE;
    rt_kprintf("[LVGL] refresh requested\r\n");
}
MSH_CMD_EXPORT(lvgl_refresh_cmd, request a complete LVGL redraw);

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static void ui_page_test_print_label(const char *page, rt_uint8_t index,
                                     const char *text)
{
    static const char hex_digits[] = "0123456789ABCDEF";
#define UI_TEXT_HEX_CHUNK_BYTES 16U
    char hex[UI_TEXT_HEX_CHUNK_BYTES * 2U + 1U];
    rt_size_t length = rt_strlen(text);
    rt_size_t offset;

    if (length >= UI_MAIN_DEBUG_LABEL_TEXT_MAX)
    {
        length = UI_MAIN_DEBUG_LABEL_TEXT_MAX - 1U;
    }
    rt_kprintf("[UI_TEXT] page=%s index=%u bytes=%lu crc32=%08lX\r\n",
               page, (unsigned int)index, (unsigned long)length,
               (unsigned long)usb_evidence_bytes_crc32(text, length));

    for (offset = 0U; offset < length; offset += UI_TEXT_HEX_CHUNK_BYTES)
    {
        rt_size_t chunk_length = length - offset;
        rt_size_t byte_index;

        if (chunk_length > UI_TEXT_HEX_CHUNK_BYTES)
        {
            chunk_length = UI_TEXT_HEX_CHUNK_BYTES;
        }
        for (byte_index = 0U; byte_index < chunk_length; byte_index++)
        {
            rt_uint8_t value = (rt_uint8_t)text[offset + byte_index];
            hex[byte_index * 2U] = hex_digits[value >> 4];
            hex[byte_index * 2U + 1U] = hex_digits[value & 0x0FU];
        }
        hex[chunk_length * 2U] = '\0';
        rt_kprintf("[UI_TEXT_HEX] page=%s index=%u offset=%lu hex=%s\r\n",
                   page, (unsigned int)index, (unsigned long)offset, hex);
    }
#undef UI_TEXT_HEX_CHUNK_BYTES
}

static void ui_page_test_cmd(int argc, char **argv)
{
    static const char *const names[] =
        { "live", "descriptors", "diagnostics", "settings" };
    rt_uint32_t index;
    rt_uint32_t child_count = 0U;
    rt_uint8_t label_count = 0U;
    rt_uint8_t timeline_visible_count = 0U;
    char labels[UI_MAIN_DEBUG_LABEL_MAX][UI_MAIN_DEBUG_LABEL_TEXT_MAX];

    if (argc != 2)
    {
        rt_kprintf("usage: ui_page_test_cmd <live|descriptors|diagnostics|settings>\r\n");
        return;
    }
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); index++)
    {
        if (rt_strcmp(argv[1], names[index]) == 0)
        {
            rt_err_t result = ui_main_debug_navigate(
                (int)index, 1000U, &child_count, labels, &label_count,
                &timeline_visible_count);
            if (result == RT_EOK)
            {
                rt_uint8_t label_index;
                rt_kprintf("[UI_TEST] page=%s active=1 child_objects=%lu labels=%u\r\n",
                           names[index], (unsigned long)child_count,
                           (unsigned int)label_count);
                for (label_index = 0U; label_index < label_count; label_index++)
                {
                    ui_page_test_print_label(names[index], label_index,
                                             labels[label_index]);
                }
                if (index == 0U)
                {
                    rt_kprintf("[UI_LAYOUT] timeline_visible=%u/%u\r\n",
                               (unsigned int)timeline_visible_count,
                               (unsigned int)USB_EVIDENCE_TIMELINE_COUNT);
                }
            }
            else
            {
                rt_kprintf("[UI_TEST] page=%s FAIL error=%d\r\n",
                           names[index], result);
            }
            return;
        }
    }
    rt_kprintf("unknown page: %s\r\n", argv[1]);
}
MSH_CMD_EXPORT(ui_page_test_cmd, debug-only UI page and object-count smoke test);

static void ui_framebuffer_capture_cmd(void)
{
    rt_uint32_t generation = 0U;
    rt_uint32_t source_address = 0U;
    rt_uint32_t byte_count = 0U;
    rt_uint32_t crc32 = 0U;
    rt_err_t result = ui_main_debug_capture_framebuffer(
        3000U, &generation, &source_address, &byte_count, &crc32);

    if (result == RT_EOK)
    {
        rt_kprintf("[LVGL_CAPTURE] PASS generation=%lu active_ltdc=0x%08lX "
                   "width=800 height=480 format=RGB565 bytes=%lu crc32=%08lX\r\n",
                   (unsigned long)generation,
                   (unsigned long)source_address,
                   (unsigned long)byte_count,
                   (unsigned long)crc32);
    }
    else
    {
        rt_kprintf("[LVGL_CAPTURE] FAIL error=%d\r\n", result);
    }
}
MSH_CMD_EXPORT(ui_framebuffer_capture_cmd,
               freeze the active LTDC frame for host-side visual regression);
#endif

static void ui_scroll_test_cmd(int argc, char **argv)
{
    if (argc == 2 && rt_strcmp(argv[1], "start") == 0)
    {
        ui_main_request_descriptor_scroll_test(1);
        rt_kprintf("[LVGL] descriptor scroll test requested\r\n");
        return;
    }
    if (argc == 2 && rt_strcmp(argv[1], "stop") == 0)
    {
        ui_main_request_descriptor_scroll_test(0);
        rt_kprintf("[LVGL] descriptor scroll test stop requested\r\n");
        return;
    }
    rt_kprintf("usage: ui_scroll_test_cmd [start|stop]\r\n");
}
MSH_CMD_EXPORT(ui_scroll_test_cmd, benchmark descriptor-page scrolling);
