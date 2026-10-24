#ifndef WORK_USB_PERF_H
#define WORK_USB_PERF_H

#include <rtthread.h>

typedef enum
{
    USB_PERF_HOST_CALLBACK = 0,
    USB_PERF_ANALYSIS_EVENT,
    USB_PERF_EVENT_TO_ANALYSIS,
    USB_PERF_ANALYSIS_LOCK_WAIT,
    USB_PERF_ANALYSIS_LOCK_WORK,
    USB_PERF_ANALYSIS_SNAPSHOT_RESET,
    USB_PERF_ANALYSIS_FEED_CORE,
    USB_PERF_ANALYSIS_APPEND_RAW,
    USB_PERF_ANALYSIS_HANDLE_CONTROL,
    USB_PERF_ANALYSIS_THREAD_RUN,
    USB_PERF_ANALYSIS_LOCK_SCHEDULED,
    USB_PERF_ANALYSIS_PUBLISH,
    USB_PERF_EVIDENCE_PUBLISH_LOCK_WAIT,
    USB_PERF_EVIDENCE_PREPARE,
    USB_PERF_WEB_UPDATE,
    USB_PERF_SNAPSHOT_COPY,
    USB_PERF_EVIDENCE_CRC,
    USB_PERF_JSON_BUILD,
    USB_PERF_JSON_LOCK_WAIT,
    USB_PERF_JSON_CRC,
    USB_PERF_PUBLISH_LOCK,
    USB_PERF_WEB_PUBLISH_LOCK_WAIT,
    USB_PERF_WEB_PUBLISH_LOCK_HOLD,
    USB_PERF_HTTP_HANDLER,
    USB_PERF_LVGL_UPDATE,
    USB_PERF_LVGL_RENDER,
    USB_PERF_DMA2D_BROKER_WAIT,
    USB_PERF_DMA2D_JOB_WAIT,
    USB_PERF_NEMA_FRAME_WAIT,
    USB_PERF_WIFI_PUBLISH,
    USB_PERF_WIFI_SCAN_COPY,
    USB_PERF_WHD_TX_BUFFER_GET,
    USB_PERF_WHD_TX_COPY,
    USB_PERF_WHD_TX_ENQUEUE,
    USB_PERF_WHD_TX_TOTAL,
    USB_PERF_SDIO_MUTEX_WAIT,
    USB_PERF_SDIO_CMD53_READ,
    USB_PERF_SDIO_CMD53_WRITE,
    USB_PERF_MONGOOSE_OWNER_LOOP,
    USB_PERF_MG_MGR_POLL,
    USB_PERF_MONGOOSE_WEB_POLL,
    USB_PERF_WHD_SDPCM_QUEUE_WAIT,
    USB_PERF_WHD_THREAD_NOTIFY,
    USB_PERF_CATEGORY_COUNT
} usb_perf_category_t;

typedef struct
{
    rt_uint32_t valid;
    rt_uint32_t sequence;
    rt_uint32_t event_type;
    rt_uint32_t event_to_analysis_us;
    rt_uint32_t lock_wait_us;
    rt_uint32_t lock_work_us;
    rt_uint32_t publish_us;
    rt_uint32_t total_us;
    rt_uint32_t captured_length;
    rt_uint32_t snapshot_reset_us;
    rt_uint32_t feed_core_us;
    rt_uint32_t append_raw_us;
    rt_uint32_t handle_control_us;
    rt_uint32_t other_lock_us;
    rt_uint32_t analysis_thread_run_us;
    rt_uint32_t analysis_runtime_valid;
    rt_uint32_t lock_scheduled_us;
    rt_uint32_t lock_off_thread_us;
    rt_uint32_t lock_runtime_valid;
    rt_uint32_t total_cycles;
} usb_perf_analysis_detail_t;

rt_uint32_t usb_perf_now(void);
void usb_perf_record(usb_perf_category_t category,
                     rt_uint32_t start_cycles,
                     rt_uint32_t end_cycles);
void usb_perf_record_us(usb_perf_category_t category, rt_uint32_t duration_us);
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
                                    rt_bool_t lock_runtime_valid);
void usb_perf_get_analysis_detail(usb_perf_analysis_detail_t *detail);
void usb_perf_reset(void);

#endif
