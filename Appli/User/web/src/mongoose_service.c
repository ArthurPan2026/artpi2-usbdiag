/*
 * ART-Pi2 Wi-Fi service.
 *
 * Infineon WHD owns the CYW43438 SDIO device and its complete Wi-Fi control
 * plane.  Mongoose is used only as the event loop, IPv4/DHCP/TCP/IP stack and
 * HTTP server.  The two APIs meet at an Ethernet-frame bridge below; no lwIP
 * or Mongoose native CYW implementation is involved.
 */
#include "mongoose.h"

#include <finsh.h>
#include <rtdevice.h>
#include <rtthread.h>
#include <string.h>
#include <stdlib.h>

#include "cybsp.h"
#include "cyhal_sdio.h"
#include "main.h"
#include "whd.h"
#include "whd_buffer_api.h"
#include "whd_events_int.h"
#include "whd_network_types.h"
#include "whd_resource_api.h"
#include "whd_rtos_diag.h"
#include "whd_stm32_sdio.h"
#include "whd_wlioctl.h"
#include "whd_wifi_api.h"

#include "web_server.h"
#include "web_snapshot.h"
#include "web_pdf_report.h"
#include "usb_perf.h"
#include "wifi_service.h"
#include "wifi_state.h"

#include "wifi_manager_backend.h"
#include "startup_coordinator.h"
#include "watchdog_supervisor.h"

#define MONGOOSE_SERVICE_STACK_SIZE   8192U
#define MONGOOSE_SERVICE_PRIORITY     11U
#define MONGOOSE_SERVICE_TIMESLICE    2U
#define MONGOOSE_COMMAND_QUEUE_DEPTH  4U
#define MONGOOSE_HTTP_PORT            80U
#define MONGOOSE_HTTP_PAYLOAD_SIZE    24576U
#if WEB_PDF_REPORT_CHUNK_SIZE > MONGOOSE_HTTP_PAYLOAD_SIZE
#error "PDF report chunks must fit the fixed Mongoose response payload"
#endif
#define MONGOOSE_HTTP_PDF_STREAM_LOW_WATER 2048U
#define MONGOOSE_HTTP_PDF_STREAM_TIMEOUT_MS 30000U
#define MONGOOSE_RX_QUEUE_SIZE        65536U
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
/* Temporary, explicitly armed Debug-only raw TCP throughput endpoint. */
#define MONGOOSE_WIFI_BENCH_PORT              5201U
#define MONGOOSE_WIFI_BENCH_HEADER_SIZE       12U
#define MONGOOSE_WIFI_BENCH_MIN_BYTES         65536U
#define MONGOOSE_WIFI_BENCH_MAX_BYTES         (8U * 1024U * 1024U)
#define MONGOOSE_WIFI_BENCH_SEND_HIGH_WATER   8192U
#define MONGOOSE_WIFI_BENCH_SEND_LOW_WATER    4096U
#define MONGOOSE_WIFI_BENCH_SEND_MAX_WATER    16384U
#define MONGOOSE_WIFI_BENCH_LISTENER_TTL_MS   180000U
#define MONGOOSE_WIFI_BENCH_CONNECTION_TTL_MS 60000U
#define MONGOOSE_WIFI_BENCH_STATE_MAGIC       0x57425431UL
#endif
#define MONGOOSE_MANAGER_POLLS_PER_TICK 4U
#define MONGOOSE_HTTP_MAX_CONNECTIONS 8U
#define MONGOOSE_HTTP_OVERFLOW_CONNECTIONS 12U
#define MONGOOSE_HTTP_MAX_REQUESTS_PER_CONNECTION 16U
#define MONGOOSE_HTTP_MAX_PDF_RESPONSES_IN_FLIGHT 2U
/* Legacy report chunk readers may poll status while awaiting one report. */
#define MONGOOSE_HTTP_MAX_PDF_REQUESTS_PER_CONNECTION 321U
#define MONGOOSE_HTTP_PDF_KEEPALIVE_MAX_REQUESTS \
    (MONGOOSE_HTTP_MAX_REQUESTS_PER_CONNECTION + \
     MONGOOSE_HTTP_MAX_PDF_REQUESTS_PER_CONNECTION)
#define MONGOOSE_HTTP_IDLE_TIMEOUT_MS 10000U
#define MONGOOSE_HTTP_OVERFLOW_TIMEOUT_MS 1200U
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
/* Debug exports frozen 16 KiB framebuffer pages. Keep those
 * pages on a bounded persistent connection instead of forcing one TCP
 * handshake per page. Release retains the smaller general-response bound. */
#define MONGOOSE_HTTP_KEEPALIVE_MAX_RESPONSE 16384U
#else
#define MONGOOSE_HTTP_KEEPALIVE_MAX_RESPONSE 8192U
#endif
#define MONGOOSE_WHD_STACK_SIZE       8192U
#define MONGOOSE_WHD_PRIORITY         8U
#define MONGOOSE_RECONNECT_BASE_MS    2000U
#define MONGOOSE_RECONNECT_MAX_MS    15000U
#define MONGOOSE_LINK_CHECK_MS        1000U
#define MONGOOSE_DHCP_REQUEST_TIMEOUT_MS 8000U
#define MONGOOSE_SCAN_STOP_TIMEOUT_MS 3000U /* Bound callback wait; do not infer idle on expiry. */

#define MONGOOSE_HTTP_STATE_MAGIC     0x4D485443UL

/* Kept as an explicit build-time switch for repeatable isolation tests. */
#define MONGOOSE_DIAGNOSTIC_DISABLE_SERVICE 0U

#define MONGOOSE_WIFI_RAM_SECTION \
    __attribute__((section(".wifi_web_ram"), aligned(32), used))



struct mongoose_whd_context
{
    whd_driver_t driver;
    whd_interface_t interface;
    cyhal_sdio_t sdio;
    volatile rt_bool_t ready;
    volatile rt_bool_t link_up;
    rt_bool_t scan_active;
    rt_bool_t scan_stopping;
    uint32_t scan_sequence;
    volatile rt_bool_t event_link_down;
    rt_uint32_t net_generation;


};

struct wifi_state g_wifi =
{
    .conn_status = WIFI_STATE_DISCONNECTED,
    .scan_status = WIFI_SCAN_STATUS_IDLE,
    .status_msg = "正在初始化 Infineon WHD WiFi"
};



static struct mg_mgr s_mgr;
static struct mg_tcpip_if s_mongoose_if;
static struct mongoose_whd_context s_whd;
static struct mg_connection *s_http_listener;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static struct mg_connection *s_wifi_bench_listener;
static struct mg_connection *s_wifi_bench_active_connection;
static rt_uint32_t s_wifi_bench_listener_opened_ms;
static rt_uint32_t s_wifi_bench_run_id;
static rt_uint32_t s_wifi_bench_send_high_water = MONGOOSE_WIFI_BENCH_SEND_HIGH_WATER;
static rt_bool_t s_wifi_bench_active;
static rt_uint8_t s_wifi_bench_pattern[1024];
static rt_bool_t s_wifi_bench_pattern_ready;
struct mongoose_wifi_bench_stats
{
    rt_bool_t valid;
    rt_bool_t completed;
    rt_uint8_t mode;
    rt_uint8_t reserved;
    rt_uint32_t run_id;
    rt_uint32_t send_high_water;
    rt_uint32_t expected_bytes;
    rt_uint32_t payload_bytes;
    rt_uint32_t elapsed_ms;
    rt_uint32_t pump_calls;
    rt_uint32_t empty_pump_calls;
    rt_uint32_t append_calls;
    rt_uint32_t append_bytes;
    rt_uint32_t send_len_min;
    rt_uint32_t send_len_max;
    rt_uint32_t rx_budget_exhausted_polls;
    rt_uint32_t rx_queue_drops;
    rt_uint16_t rx_window_bytes;
    rt_uint8_t rx_budget_frames;
    rt_uint8_t reserved2;
};
static struct mongoose_wifi_bench_stats s_wifi_bench_last_stats;
#endif
static struct mg_tcpip_if *s_ifp;
static volatile rt_bool_t s_manager_ready;
static volatile rt_bool_t s_mongoose_net_ready;
static volatile rt_bool_t s_web_requested;
static volatile rt_bool_t s_web_online;
static rt_uint32_t s_watchdog_manager_progress;
static rt_uint32_t s_web_listen_failures;
static rt_tick_t s_dhcp_request_started;
static uint32_t s_dhcp_client_xid;
static bool s_dhcp_xid_seeded;
static char s_http_payload[MONGOOSE_HTTP_PAYLOAD_SIZE]
    MONGOOSE_WIFI_RAM_SECTION;
static whd_scan_result_t s_scan_result;
static struct rt_semaphore s_scan_terminal_sem;
static bool s_scan_terminal_sem_initialized;
static wifi_backend_diagnostics_t s_backend_diagnostics;

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
/* Numeric, bounded event history: no SSID, key, packet bytes or callback I/O.
 * This records firmware notifications, not on-air RF timing. */
struct wifi_radio_event_record {
    uint32_t tick, join_sequence, leave_sequence, type, status, reason;
    uint16_t flags;
    uint8_t interface_index, reserved;
};
#define WIFI_RADIO_EVENT_CAPACITY 16U
static struct wifi_radio_event_record s_radio_events[WIFI_RADIO_EVENT_CAPACITY];
static uint32_t s_radio_event_count;

static void wifi_radio_record(const whd_event_header_t *header)
{
    struct wifi_radio_event_record *record;
    rt_enter_critical();
    record = &s_radio_events[s_radio_event_count % WIFI_RADIO_EVENT_CAPACITY];
    record->tick = (uint32_t)rt_tick_get();
    record->join_sequence = s_backend_diagnostics.last_join.sequence;
    record->leave_sequence = s_backend_diagnostics.last_leave.sequence;
    record->type = header->event_type;
    record->status = header->status;
    record->reason = header->reason;
    record->flags = header->flags;
    record->interface_index = header->bsscfgidx;
    record->reserved = 0U;
    ++s_radio_event_count;
    rt_exit_critical();
}

static void wifi_radio_events_cmd(void)
{
    struct wifi_radio_event_record records[WIFI_RADIO_EVENT_CAPACITY];
    static wifi_snapshot_t scan_view;
    uint32_t total, count, first, i;
    rt_enter_critical();
    total = s_radio_event_count;
    count = total < WIFI_RADIO_EVENT_CAPACITY ? total : WIFI_RADIO_EVENT_CAPACITY;
    first = total - count;
    for (i = 0U; i < count; ++i)
        records[i] = s_radio_events[(first + i) % WIFI_RADIO_EVENT_CAPACITY];
    rt_exit_critical();
    rt_kprintf("[WIFI_EVENTS] total=%lu retained=%lu time=host_tick\n",
        (unsigned long)total, (unsigned long)count);
    for (i = 0U; i < count; ++i) {
        const struct wifi_radio_event_record *record = &records[i];
        rt_kprintf("[WIFI_EVENT] n=%lu tick=%lu join=%lu leave=%lu\n",
            (unsigned long)(first + i), (unsigned long)record->tick,
            (unsigned long)record->join_sequence, (unsigned long)record->leave_sequence);
        rt_kprintf(" type=%lu status=%lu reason=%lu flags=%u bss=%u\n",
            (unsigned long)record->type, (unsigned long)record->status,
            (unsigned long)record->reason, record->flags, record->interface_index);
    }
    if (wifi_get_snapshot(&scan_view) == RT_EOK) {
        for (i = 0U; i < scan_view.scan_count; ++i) {
            const wifi_ap_record_t *ap = &scan_view.access_points[i];
            for (unsigned p = 0U; p < scan_view.network.profile_count; ++p) {
                const wifi_profile_summary_t *profile = &scan_view.network.profiles[p];
                if (profile->ssid_length == ap->ssid_length &&
                    memcmp(profile->ssid, ap->ssid, ap->ssid_length) == 0) {
                    rt_kprintf("[WIFI_SCAN_MATCH] profile=%lu rssi=%d channel=%d security=0x%08lx\n",
                        (unsigned long)profile->id, ap->rssi, ap->channel,
                        (unsigned long)ap->security);
                }
            }
        }
    }
}
MSH_CMD_EXPORT(wifi_radio_events_cmd, debug-only numeric WHD event history);
#endif

enum wifi_backend_diag_kind
{
    WIFI_BACKEND_DIAG_SCAN = 0,
    WIFI_BACKEND_DIAG_JOIN,
    WIFI_BACKEND_DIAG_LEAVE
};

static bool wifi_backend_diag_update(enum wifi_backend_diag_kind kind,
                                     uint32_t sequence,
                                     wifi_backend_diag_stage_t stage,
                                     uint32_t raw_whd_result,
                                     uint32_t raw_event_code,
                                     int32_t backend_result,
                                     wifi_backend_diag_outcome_t outcome);
static bool wifi_backend_diag_is_in_progress(enum wifi_backend_diag_kind kind,
                                             uint32_t sequence);

rt_bool_t mongoose_service_is_ready(void)
{
    return (s_mongoose_net_ready && s_manager_ready) ? RT_TRUE : RT_FALSE;
}

enum mongoose_http_admission
{
    MONGOOSE_HTTP_ADMISSION_REJECTED = 0,
    MONGOOSE_HTTP_ADMISSION_SERVICE,
    MONGOOSE_HTTP_ADMISSION_OVERFLOW
};

enum mongoose_http_evidence_route
{
    MONGOOSE_HTTP_EVIDENCE_NONE = 0,
    MONGOOSE_HTTP_EVIDENCE_MANIFEST,
    MONGOOSE_HTTP_EVIDENCE_PAGE,
    MONGOOSE_HTTP_EVIDENCE_EVENT
};

/* The application owns c->data for HTTP connections.  Mongoose reserves it
 * for protocol-specific scratch state only when an optional protocol (for
 * example upload/dashboard) is used; none of those protocols is enabled by
 * this firmware.  Keeping this state in the connection avoids a second heap
 * allocation for every browser connection. */
struct mongoose_http_connection_state
{
    rt_uint32_t magic;
    rt_uint32_t accepted_ms;
    rt_uint32_t last_activity_ms;
    rt_uint16_t request_count;
    rt_uint16_t pdf_request_count;
    rt_uint8_t admission;
    rt_uint8_t counted;
    rt_uint8_t pdf_asset;
    rt_uint8_t pdf_response_pending;
    rt_uint8_t pdf_stream_active;
    rt_uint32_t pdf_stream_job_id;
    rt_uint32_t pdf_stream_total;
    rt_uint32_t pdf_stream_next_offset;
    rt_uint32_t pdf_stream_last_progress_ms;
};

typedef char mongoose_http_connection_state_fits[
    sizeof(struct mongoose_http_connection_state) <= MG_DATA_SIZE ? 1 : -1];

struct mongoose_http_stats
{
    rt_uint32_t active;
    rt_uint32_t service_active;
    rt_uint32_t overflow_active;
    rt_uint32_t peak;
    rt_uint32_t accepted;
    rt_uint32_t closed;
    rt_uint32_t rejected;
    rt_uint32_t requests;
    rt_uint32_t overload_503;
    rt_uint32_t busy_503;
    rt_uint32_t request_limit;
    rt_uint32_t pipeline_closed;
    rt_uint32_t response_closed;
    rt_uint32_t idle_closed;
    rt_uint32_t errors;
    rt_uint32_t evidence_manifest_overload_503;
    rt_uint32_t evidence_page_overload_503;
    rt_uint32_t evidence_event_overload_503;
    rt_uint32_t evidence_manifest_busy_503;
    rt_uint32_t evidence_page_busy_503;
    rt_uint32_t evidence_event_busy_503;
    rt_uint32_t evidence_page_stale_409;
    rt_uint32_t evidence_event_stale_409;
    rt_uint32_t pdf_stream_active;
    rt_uint32_t pdf_stream_completed;
    rt_uint32_t pdf_stream_aborted;
    rt_uint32_t pdf_stream_timeout;
    rt_uint32_t pdf_stream_pumps;
    rt_uint32_t pdf_stream_max_send_bytes;
};

static struct mongoose_http_stats s_http_stats;
/* Ordinary PDF assets remain capped at two in-flight response queues. */
static rt_uint8_t s_http_pdf_responses_active;

static enum mongoose_http_evidence_route mongoose_http_classify_evidence_uri(
    const struct mg_str *uri)
{
    static const char manifest_path[] = "/api/evidence";
    static const char page_prefix[] = "/api/evidence/page/";
    static const char event_prefix[] = "/api/evidence/event/";
    size_t path_length;

    if (!uri || !uri->buf || uri->len == 0U)
    {
        return MONGOOSE_HTTP_EVIDENCE_NONE;
    }
    path_length = uri->len;
    {
        const char *query = (const char *)memchr(uri->buf, '?', uri->len);
        if (query != RT_NULL)
        {
            path_length = (size_t)(query - uri->buf);
        }
    }
    if (path_length == sizeof(manifest_path) - 1U &&
        memcmp(uri->buf, manifest_path, path_length) == 0)
    {
        return MONGOOSE_HTTP_EVIDENCE_MANIFEST;
    }
    if (path_length >= sizeof(page_prefix) - 1U &&
        memcmp(uri->buf, page_prefix, sizeof(page_prefix) - 1U) == 0)
    {
        return MONGOOSE_HTTP_EVIDENCE_PAGE;
    }
    if (path_length >= sizeof(event_prefix) - 1U &&
        memcmp(uri->buf, event_prefix, sizeof(event_prefix) - 1U) == 0)
    {
        return MONGOOSE_HTTP_EVIDENCE_EVENT;
    }
    return MONGOOSE_HTTP_EVIDENCE_NONE;
}

static void mongoose_http_record_evidence_overload_503(
    enum mongoose_http_evidence_route route)
{
    switch (route)
    {
    case MONGOOSE_HTTP_EVIDENCE_MANIFEST:
        s_http_stats.evidence_manifest_overload_503++;
        break;
    case MONGOOSE_HTTP_EVIDENCE_PAGE:
        s_http_stats.evidence_page_overload_503++;
        break;
    case MONGOOSE_HTTP_EVIDENCE_EVENT:
        s_http_stats.evidence_event_overload_503++;
        break;
    default:
        break;
    }
}

static void mongoose_http_record_evidence_busy_503(
    enum mongoose_http_evidence_route route)
{
    switch (route)
    {
    case MONGOOSE_HTTP_EVIDENCE_MANIFEST:
        s_http_stats.evidence_manifest_busy_503++;
        break;
    case MONGOOSE_HTTP_EVIDENCE_PAGE:
        s_http_stats.evidence_page_busy_503++;
        break;
    case MONGOOSE_HTTP_EVIDENCE_EVENT:
        s_http_stats.evidence_event_busy_503++;
        break;
    default:
        break;
    }
}

static void mongoose_http_record_evidence_stale_409(
    enum mongoose_http_evidence_route route)
{
    if (route == MONGOOSE_HTTP_EVIDENCE_PAGE)
    {
        s_http_stats.evidence_page_stale_409++;
    }
    else if (route == MONGOOSE_HTTP_EVIDENCE_EVENT)
    {
        s_http_stats.evidence_event_stale_409++;
    }
}

/* These objects are provided by the project-local WHD port. */
extern whd_resource_source_t resource_ops;
extern struct whd_buffer_funcs whd_buffer_ops;
extern void whd_stm32_sdio_set_high_speed(rt_bool_t enabled);

static void mongoose_wifi_event(struct mg_tcpip_if *ifp, int event,
                                void *event_data);
static void mongoose_reset_dhcp_session(void);



void *mg_calloc(size_t count, size_t size)
{
    size_t total;
    void *memory;

    if (size != 0U && count > ((size_t)-1) / size)
    {
        return RT_NULL;
    }
    total = count * size;
    memory = rt_malloc(total);
    if (memory != RT_NULL)
    {
        rt_memset(memory, 0, total);
    }
    return memory;
}

void mg_free(void *memory)
{
    if (memory != RT_NULL)
    {
        rt_free(memory);
    }
}

uint64_t mg_millis(void)
{
    return ((uint64_t)rt_tick_get() * 1000ULL) / RT_TICK_PER_SECOND;
}

static void mongoose_log_putchar(char character, void *parameter)
{
    RT_UNUSED(parameter);
    rt_kprintf("%c", character);
}

static void mongoose_scan_begin(void)
{
    rt_enter_critical();
    rt_memset(g_wifi.scan_records, 0, sizeof(g_wifi.scan_records));
    g_wifi.scan_count = 0;
    g_wifi.scan_dropped_count = 0U;
    g_wifi.scan_status = WIFI_SCAN_STATUS_SCANNING;
    g_wifi.scan_updated++;
    rt_exit_critical();
}

static void mongoose_scan_store(const whd_scan_result_t *result)
{
    rt_size_t length;
    int index;

    if (result == RT_NULL || result->SSID.length == 0U)
    {
        return;
    }
    length = result->SSID.length;
    if (length >= WIFI_SCAN_SSID_MAX_LENGTH)
    {
        length = WIFI_SCAN_SSID_MAX_LENGTH - 1U;
    }

    rt_enter_critical();
    if (g_wifi.scan_count >= (int)WIFI_SCAN_MAX_APS)
    {
        g_wifi.scan_dropped_count++;
        g_wifi.scan_status = WIFI_SCAN_STATUS_TRUNCATED;
        rt_exit_critical();
        return;
    }
    index = g_wifi.scan_count++;
    rt_memset(&g_wifi.scan_records[index], 0,
              sizeof(g_wifi.scan_records[index]));
    rt_memcpy(g_wifi.scan_records[index].ssid, result->SSID.value, length);
    g_wifi.scan_records[index].ssid[length] = '\0';
    g_wifi.scan_records[index].rssi = result->signal_strength;
    g_wifi.scan_records[index].ssid_length = (rt_uint8_t)length;
    g_wifi.scan_records[index].security = result->security;
    g_wifi.scan_records[index].security_mode =
        result->security == 0 ? WIFI_SECURITY_OPEN :
        (result->security & (WPA3_SECURITY|ENTERPRISE_ENABLED)) ||
        !(result->security & AES_ENABLED) ? WIFI_SECURITY_UNSUPPORTED :
        ((result->security & WPA2_SECURITY) && (result->security & WPA_SECURITY)) ? WIFI_SECURITY_WPA_MIXED :
        (result->security & WPA2_SECURITY) ? WIFI_SECURITY_WPA2 : WIFI_SECURITY_UNSUPPORTED;
    g_wifi.scan_records[index].ecn = (rt_uint8_t)(result->security & 0xffU);
    g_wifi.scan_records[index].channel = result->channel;
    rt_memcpy(g_wifi.scan_records[index].bssid, result->BSSID.octet, 6U);
    if (result->SSID.length >= WIFI_SCAN_SSID_MAX_LENGTH)
    {
        g_wifi.scan_records[index].flags |= WIFI_AP_FLAG_SSID_TRUNCATED;
    }
    g_wifi.scan_updated++;
    rt_exit_critical();
}

static void mongoose_scan_end(rt_bool_t success)
{
    int count;
    rt_uint32_t dropped;
    int connection_status;
    wifi_backend_diagnostics_t scan_diag;
    bool cancelled = !success && wifi_backend_get_diagnostics(&scan_diag)==RT_EOK &&
        scan_diag.last_scan.outcome == WIFI_BACKEND_DIAG_OUTCOME_ABORTED;

    rt_enter_critical();
    count = g_wifi.scan_count;
    dropped = g_wifi.scan_dropped_count;
    connection_status = g_wifi.conn_status;
    if (!success)
    {
        g_wifi.scan_status = cancelled ? WIFI_SCAN_STATUS_CANCELLED : WIFI_SCAN_STATUS_FAILED;
    }
    else
    {
        g_wifi.scan_status = count == 0 ? WIFI_SCAN_STATUS_EMPTY :
                             (dropped != 0U ? WIFI_SCAN_STATUS_TRUNCATED :
                                              WIFI_SCAN_STATUS_COMPLETE);
    }
    g_wifi.scan_updated++;
    rt_exit_critical();

    /* WHD invokes this from its radio worker. Diagnostics already retain
     * completion/counts; synchronous UART logging here would stall RX and
     * could overwrite a concurrent FinSH printf buffer/structured packet. */

    if (connection_status == WIFI_STATE_CONNECTING ||
        connection_status == WIFI_STATE_CONNECTED)
    {
        return;
    }
    if (cancelled)
    {
        wifi_set_status(WIFI_STATE_DISCONNECTED, "扫描已取消");
    }
    else if (!success)
    {
        wifi_set_status(WIFI_STATE_FAILED, "扫描失败");
    }
    else
    {
        wifi_set_status(WIFI_STATE_DISCONNECTED,
                        count == 0 ? "未发现网络" :
                        (dropped != 0U ? "扫描完成，结果已截断" : "扫描完成"));
    }
}

/* WHD calls this callback from its own worker.  It only copies the result and
 * updates a small state structure; it does not issue another WHD transaction. */
static void mongoose_whd_scan_callback(whd_scan_result_t **result_ptr,
                                        void *user_data,
                                        whd_scan_status_t status)
{
    uint32_t sequence = (uint32_t)(uintptr_t)user_data;
    bool owns_scan;
    bool release_waiter;

    rt_enter_critical();
    owns_scan = s_whd.scan_active && s_whd.scan_sequence == sequence;
    rt_exit_critical();
    if (!owns_scan)
    {
        return;
    }

    if (status == WHD_SCAN_INCOMPLETE)
    {
        if (!wifi_backend_diag_is_in_progress(WIFI_BACKEND_DIAG_SCAN,
                                               sequence))
        {
            return;
        }
        if (result_ptr != RT_NULL && *result_ptr != RT_NULL)
        {
            mongoose_scan_store(*result_ptr);
            rt_memset(*result_ptr, 0, sizeof(**result_ptr));
        }
        return;
    }

    if (status == WHD_SCAN_COMPLETED_SUCCESSFULLY)
    {
        if (!wifi_backend_diag_update(WIFI_BACKEND_DIAG_SCAN, sequence,
                WIFI_BACKEND_DIAG_STAGE_CALLBACK, WHD_SUCCESS,
                (uint32_t)status, RT_EOK,
                WIFI_BACKEND_DIAG_OUTCOME_SUCCESS))
        {
            return;
        }
    }
    else if (status == WHD_SCAN_ABORTED)
    {
        if (!wifi_backend_diag_update(WIFI_BACKEND_DIAG_SCAN, sequence,
                WIFI_BACKEND_DIAG_STAGE_CALLBACK, WHD_SUCCESS,
                (uint32_t)status, RT_EOK,
                WIFI_BACKEND_DIAG_OUTCOME_ABORTED))
        {
            return;
        }
    }
    else if (!wifi_backend_diag_update(WIFI_BACKEND_DIAG_SCAN, sequence,
                 WIFI_BACKEND_DIAG_STAGE_CALLBACK, WHD_SUCCESS,
                 (uint32_t)status, -RT_EIO,
                 WIFI_BACKEND_DIAG_OUTCOME_ERROR))
    {
        return;
    }

    rt_enter_critical();
    owns_scan = s_whd.scan_active && s_whd.scan_sequence == sequence;
    if (owns_scan)
    {
        s_whd.scan_active = RT_FALSE;
        s_whd.scan_stopping = RT_FALSE;
    }
    release_waiter = owns_scan && s_scan_terminal_sem_initialized;
    rt_exit_critical();
    if (!owns_scan)
    {
        return;
    }

    mongoose_scan_end(status == WHD_SCAN_COMPLETED_SUCCESSFULLY);
    if (release_waiter)
    {
        (void)rt_sem_release(&s_scan_terminal_sem);
    }
}

/* The event handler is intentionally short: WHD uses it in its SDIO worker
 * context.  Mongoose observes the resulting link state in its normal poll. */
static void *mongoose_whd_event(whd_interface_t ifp,
    const whd_event_header_t *header, const uint8_t *data, void *context)
{
    (void)ifp; (void)data;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (header) wifi_radio_record(header);
#endif
    if (header && ((header->event_type == WLC_E_LINK && !(header->flags & 1U)) ||
        header->event_type == WLC_E_DEAUTH_IND || header->event_type == WLC_E_DISASSOC_IND)) {
        /* This worker callback records only a link event. Coordinator owns IP/state. */
        s_whd.event_link_down = RT_TRUE;
        s_whd.link_up = RT_FALSE;
    }
    return context;
}

static void mongoose_whd_receive(whd_interface_t ifp, whd_buffer_t buffer)
{
    whd_driver_t driver = s_whd.driver;
    uint8_t *data;
    uint16_t length;

    if (driver == RT_NULL || buffer == RT_NULL)
    {
        return;
    }
    data = whd_buffer_get_current_piece_data_pointer(driver, buffer);
    length = whd_buffer_get_current_piece_size(driver, buffer);
    if (s_mongoose_net_ready && s_whd.link_up && s_ifp != RT_NULL && data != RT_NULL &&
        length != 0U)
    {
        mg_tcpip_qwrite(data, length, s_ifp);
    }
    (void)whd_buffer_release(driver, buffer, WHD_NETWORK_RX);
    (void)ifp;
}

static struct whd_netif_funcs s_whd_netif_ops =
{
    .whd_network_process_ethernet_data = mongoose_whd_receive
};

static rt_bool_t mongoose_whd_tx_error_consumes_buffer(whd_result_t result)
{
    /* The locked WHD CDC/SDPCM implementation releases the frame before
     * returning these errors: CDC header-reservation failure, invalid BSS
     * index, SDPCM queue-lock failure, and a full SDPCM AC queue. Releasing
     * them again here can touch a dynamic buffer after rt_free(). Other
     * errors leave ownership with this caller. Re-audit this table if the
     * locked WHD protocol implementation changes. */
    return result == WHD_BUFFER_ALLOC_FAIL ||
           result == WHD_UNKNOWN_INTERFACE ||
           result == WHD_SEMAPHORE_ERROR;
}

static size_t mongoose_whd_tx(const void *data, size_t length,
                              struct mg_tcpip_if *ifp)
{
    whd_buffer_t buffer = RT_NULL;
    whd_result_t result;
    uint8_t *packet;
    size_t sent = 0U;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    rt_uint32_t total_start_cycles;
    rt_uint32_t total_end_cycles;
    rt_uint32_t get_start_cycles;
    rt_uint32_t get_end_cycles;
    rt_uint32_t copy_start_cycles = 0U;
    rt_uint32_t copy_end_cycles = 0U;
    rt_uint32_t enqueue_start_cycles = 0U;
    rt_uint32_t enqueue_end_cycles = 0U;
    bool copy_timed = false;
    bool enqueue_timed = false;
#endif

    if (!s_whd.ready || !s_whd.link_up || data == RT_NULL ||
        length == 0U || length > UINT16_MAX || s_whd.interface == RT_NULL)
    {
        return 0U;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    total_start_cycles = usb_perf_now();
    get_start_cycles = usb_perf_now();
#endif
    result = whd_host_buffer_get(s_whd.driver, &buffer, WHD_NETWORK_TX,
                                 (uint16_t)length, 1000U);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    get_end_cycles = usb_perf_now();
#endif
    if (result != WHD_SUCCESS || buffer == RT_NULL)
    {
        if (ifp != RT_NULL)
        {
            ifp->nerr++;
        }
        goto done;
    }
    packet = whd_buffer_get_current_piece_data_pointer(s_whd.driver, buffer);
    if (packet == RT_NULL)
    {
        (void)whd_buffer_release(s_whd.driver, buffer, WHD_NETWORK_TX);
        if (ifp != RT_NULL)
        {
            ifp->nerr++;
        }
        goto done;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    copy_start_cycles = usb_perf_now();
#endif
    rt_memcpy(packet, data, length);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    copy_end_cycles = usb_perf_now();
    copy_timed = true;
    enqueue_start_cycles = usb_perf_now();
#endif
    result = whd_network_send_ethernet_data(s_whd.interface, buffer);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    enqueue_end_cycles = usb_perf_now();
    enqueue_timed = true;
#endif
    if (result != WHD_SUCCESS)
    {
        if (!mongoose_whd_tx_error_consumes_buffer(result))
        {
            (void)whd_buffer_release(s_whd.driver, buffer, WHD_NETWORK_TX);
        }
        if (ifp != RT_NULL)
        {
            ifp->nerr++;
        }
        goto done;
    }
    sent = length;

done:
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    total_end_cycles = usb_perf_now();
    usb_perf_record(USB_PERF_WHD_TX_BUFFER_GET, get_start_cycles,
                    get_end_cycles);
    if (copy_timed)
    {
        usb_perf_record(USB_PERF_WHD_TX_COPY, copy_start_cycles,
                        copy_end_cycles);
    }
    if (enqueue_timed)
    {
        usb_perf_record(USB_PERF_WHD_TX_ENQUEUE, enqueue_start_cycles,
                        enqueue_end_cycles);
    }
    usb_perf_record(USB_PERF_WHD_TX_TOTAL, total_start_cycles,
                    total_end_cycles);
#endif
    return sent;
}

static bool mongoose_whd_driver_init(struct mg_tcpip_if *ifp)
{
    return ifp == &s_mongoose_if && s_whd.ready;
}

static bool mongoose_whd_driver_poll(struct mg_tcpip_if *ifp,
                                     bool expired_1000ms)
{
    (void)ifp;
    (void)expired_1000ms;
    return s_whd.ready && s_whd.link_up;
}

static struct mg_tcpip_driver s_mongoose_whd_driver =
{
    .init = mongoose_whd_driver_init,
    .tx = mongoose_whd_tx,
    .rx = NULL,
    .poll = mongoose_whd_driver_poll
};

static whd_result_t mongoose_whd_start(void)
{
    startup_mark(BOOT_RADIO_BEGIN);
    whd_init_config_t init_config;
    whd_sdio_config_t sdio_config;
    whd_mac_t mac;
    uint16_t event_index = 0U;
    uint32_t event_list[] =
    {
        WLC_E_SET_SSID, WLC_E_LINK, WLC_E_AUTH, WLC_E_PSK_SUP, WLC_E_DEAUTH_IND,
        WLC_E_DISASSOC_IND, WLC_E_NONE
    };
    whd_result_t result;

    rt_memset(&s_whd, 0, sizeof(s_whd));
    rt_memset(&init_config, 0, sizeof(init_config));
    init_config.thread_stack_size = MONGOOSE_WHD_STACK_SIZE;
    init_config.thread_priority = MONGOOSE_WHD_PRIORITY;
    init_config.country = WHD_COUNTRY_CHINA;

    rt_memset(&sdio_config, 0, sizeof(sdio_config));
    sdio_config.sdio_1bit_mode = WHD_FALSE;
    sdio_config.high_speed_sdio_clock = WHD_TRUE;
    sdio_config.oob_config.host_oob_pin =
        rt_pin_get(CYBSP_HOST_WAKE_IRQ_PIN_NAME);
    sdio_config.oob_config.drive_mode = CYHAL_GPIO_DRIVE_NONE;
    sdio_config.oob_config.init_drive_state = WHD_FALSE;
    sdio_config.oob_config.dev_gpio_sel = 0U;
    sdio_config.oob_config.is_falling_edge =
        (CYBSP_HOST_WAKE_IRQ_EVENT == CYHAL_GPIO_IRQ_FALL) ?
        WHD_TRUE : WHD_FALSE;
    sdio_config.oob_config.intr_priority = CYBSP_OOB_INTR_PRIORITY;

    whd_stm32_sdio_set_high_speed(sdio_config.high_speed_sdio_clock);
    result = cyhal_sdio_init(&s_whd.sdio);
    if (result != CYHAL_SDIO_RET_NO_ERRORS)
    {
        return WHD_WLAN_SDIO_ERROR;
    }
    result = whd_init(&s_whd.driver, &init_config, &resource_ops,
                      &whd_buffer_ops, &s_whd_netif_ops);
    if (result != WHD_SUCCESS)
    {
        return result;
    }
    result = whd_bus_sdio_attach(s_whd.driver, &sdio_config, &s_whd.sdio);
    if (result != WHD_SUCCESS)
    {
        return result;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    /* SDPCM creates this semaphore inside whd_wifi_on()/whd_thread_init().
     * Register its stable address first; the local RTOS adapter activates the
     * queue snapshot after creation and unregisters it before semaphore delete. */
    (void)whd_rtos_sdpcm_queue_watch_register(s_whd.driver);
#endif
    startup_mark(BOOT_FIRMWARE_BEGIN);
    result = whd_wifi_on(s_whd.driver, &s_whd.interface);
    if(result==WHD_SUCCESS)startup_mark(BOOT_FIRMWARE_READY);
    if (result != WHD_SUCCESS)
    {
        whd_rtos_sdpcm_queue_watch_unregister(s_whd.driver);
        return result;
    }
    (void)whd_wifi_disable_powersave(s_whd.interface);
    result = whd_wifi_set_event_handler(s_whd.interface, event_list,
                                        mongoose_whd_event, RT_NULL,
                                        &event_index);
    if (result != WHD_SUCCESS)
    {
        whd_rtos_sdpcm_queue_watch_unregister(s_whd.driver);
        return result;
    }
    rt_memset(&mac, 0, sizeof(mac));
    result = whd_wifi_get_mac_address(s_whd.interface, &mac);
    if (result != WHD_SUCCESS)
    {
        whd_rtos_sdpcm_queue_watch_unregister(s_whd.driver);
        return result;
    }
    rt_memcpy(s_mongoose_if.mac, mac.octet, sizeof(mac.octet));
    s_whd.ready = RT_TRUE;
    return WHD_SUCCESS;
}

/* Called only by wifi_worker. This holds all WHD/SDIO/firmware work outside
 * the Mongoose event-loop owner so display startup never waits on it. */
int wifi_backend_initialize(void)
{
    whd_result_t result=mongoose_whd_start();
    return result==WHD_SUCCESS?RT_EOK:-RT_ERROR;
}

/* Called by wifi_manager_poll() on the Mongoose owner after the worker reports
 * successful WHD startup. TCP/IP buffers and its RX queue have one owner. */
int wifi_backend_finalize(void)
{
    if(s_ifp!=&s_mongoose_if||!s_whd.ready)return -RT_ERROR;

    s_mongoose_if.driver=&s_mongoose_whd_driver;
    s_mongoose_if.driver_data=&s_whd;
    s_mongoose_if.fn=mongoose_wifi_event;
    s_mongoose_if.recv_queue.size=MONGOOSE_RX_QUEUE_SIZE;
    s_mongoose_if.enable_req_dns=true;
    mg_log_set(MG_LL_NONE);
    rt_memcpy(s_mongoose_if.dhcp_name, "artpi2-usbdiag",
              sizeof("artpi2-usbdiag"));
    mg_tcpip_init(&s_mgr,&s_mongoose_if);
    if(s_mgr.ifp==RT_NULL||s_mongoose_if.tx.buf==RT_NULL||
       s_mongoose_if.recv_queue.buf==RT_NULL) {
        s_mongoose_net_ready=RT_FALSE;
        s_manager_ready=RT_FALSE;
        mg_tcpip_free(&s_mongoose_if);
        s_mgr.ifp=RT_NULL;
        return -RT_ERROR;
    }

    s_mongoose_net_ready=RT_TRUE;
    s_manager_ready=RT_TRUE;
    return RT_EOK;
}

static struct {
    bool (*callback)(void *);
    void *context;
} s_cancel_callback;

static void wifi_backend_diag_increment(uint32_t *value)
{
    if (*value != UINT32_MAX)
    {
        (*value)++;
    }
}

static void wifi_backend_diag_refs(enum wifi_backend_diag_kind kind,
                                   wifi_backend_diag_operation_t **last,
                                   wifi_backend_diag_counters_t **counters)
{
    switch (kind)
    {
    case WIFI_BACKEND_DIAG_SCAN:
        *last = &s_backend_diagnostics.last_scan;
        *counters = &s_backend_diagnostics.scan_counters;
        break;
    case WIFI_BACKEND_DIAG_JOIN:
        *last = &s_backend_diagnostics.last_join;
        *counters = &s_backend_diagnostics.join_counters;
        break;
    default:
        *last = &s_backend_diagnostics.last_leave;
        *counters = &s_backend_diagnostics.leave_counters;
        break;
    }
}

static void wifi_backend_diag_count_outcome(
    wifi_backend_diag_counters_t *counters,
    wifi_backend_diag_outcome_t outcome)
{
    switch (outcome)
    {
    case WIFI_BACKEND_DIAG_OUTCOME_SUCCESS:
        wifi_backend_diag_increment(&counters->successes);
        break;
    case WIFI_BACKEND_DIAG_OUTCOME_BUSY:
        wifi_backend_diag_increment(&counters->busy);
        break;
    case WIFI_BACKEND_DIAG_OUTCOME_ABORTED:
        wifi_backend_diag_increment(&counters->aborted);
        break;
    case WIFI_BACKEND_DIAG_OUTCOME_CANCELLED:
        wifi_backend_diag_increment(&counters->cancelled);
        break;
    case WIFI_BACKEND_DIAG_OUTCOME_ERROR:
        wifi_backend_diag_increment(&counters->errors);
        break;
    default:
        break;
    }
}

static uint32_t wifi_backend_diag_begin(enum wifi_backend_diag_kind kind)
{
    wifi_backend_diag_operation_t *last;
    wifi_backend_diag_counters_t *counters;
    uint32_t sequence;

    rt_enter_critical();
    wifi_backend_diag_refs(kind, &last, &counters);
    sequence = last->sequence + 1U;
    if (sequence == 0U)
    {
        sequence = 1U;
    }
    wifi_backend_diag_increment(&counters->attempts);
    rt_memset(last, 0, sizeof(*last));
    last->sequence = sequence;
    last->stage = WIFI_BACKEND_DIAG_STAGE_REQUEST;
    last->outcome = WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS;
    rt_exit_critical();
    return sequence;
}

static void wifi_backend_diag_note_scan_busy(void)
{
    rt_enter_critical();
    wifi_backend_diag_increment(&s_backend_diagnostics.scan_counters.attempts);
    wifi_backend_diag_increment(&s_backend_diagnostics.scan_counters.busy);
    rt_exit_critical();
}

static bool wifi_backend_diag_update(enum wifi_backend_diag_kind kind,
                                     uint32_t sequence,
                                     wifi_backend_diag_stage_t stage,
                                     uint32_t raw_whd_result,
                                     uint32_t raw_event_code,
                                     int32_t backend_result,
                                     wifi_backend_diag_outcome_t outcome)
{
    wifi_backend_diag_operation_t *last;
    wifi_backend_diag_counters_t *counters;
    bool matched = false;

    rt_enter_critical();
    wifi_backend_diag_refs(kind, &last, &counters);
    if (last->sequence == sequence &&
        last->outcome == WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS)
    {
        if (last->outcome == WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS &&
            outcome != WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS)
        {
            wifi_backend_diag_count_outcome(counters, outcome);
        }
        else if (last->outcome == WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS &&
                 kind == WIFI_BACKEND_DIAG_SCAN &&
                 outcome == WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS &&
                 (raw_whd_result != WHD_SUCCESS || backend_result != RT_EOK))
        {
            /* A failed stop request is a driver error while the scan itself
             * remains pending for its terminal WHD event. */
            wifi_backend_diag_increment(&counters->errors);
        }
        last->stage = (uint8_t)stage;
        if (raw_whd_result != WHD_SUCCESS || last->raw_whd_result == WHD_SUCCESS)
        {
            last->raw_whd_result = raw_whd_result;
        }
        last->raw_event_code = raw_event_code;
        last->backend_result = backend_result;
        last->outcome = (uint8_t)outcome;
        matched = true;
    }
    rt_exit_critical();
    return matched;
}

static bool wifi_backend_diag_is_in_progress(enum wifi_backend_diag_kind kind,
                                             uint32_t sequence)
{
    wifi_backend_diag_operation_t *last;
    wifi_backend_diag_counters_t *counters;
    bool in_progress;

    rt_enter_critical();
    wifi_backend_diag_refs(kind, &last, &counters);
    (void)counters;
    in_progress = last->sequence == sequence &&
                  last->outcome == WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS;
    rt_exit_critical();
    return in_progress;
}

static whd_bool_t radio_cancel(void *unused)
{
    (void)unused;
    return s_cancel_callback.callback && s_cancel_callback.callback(s_cancel_callback.context) ?
        WHD_TRUE : WHD_FALSE;
}
wifi_reason_t wifi_backend_join(const wifi_profile_t *target, bool (*cancel)(void *), void *context)
{
    uint32_t sequence = wifi_backend_diag_begin(WIFI_BACKEND_DIAG_JOIN);
    wifi_reason_t reason;
    wifi_backend_diag_outcome_t outcome;
    whd_result_t result;

    if (target == RT_NULL)
    {
        reason = WIFI_REASON_INVALID;
        outcome = WIFI_BACKEND_DIAG_OUTCOME_ERROR;
        (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_JOIN, sequence,
            WIFI_BACKEND_DIAG_STAGE_REQUEST, 0U, 0U, reason, outcome);
        return reason;
    }
    if (!s_whd.ready || !s_whd.interface)
    {
        reason = WIFI_REASON_IO;
        (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_JOIN, sequence,
            WIFI_BACKEND_DIAG_STAGE_REQUEST, 0U, 0U, reason,
            WIFI_BACKEND_DIAG_OUTCOME_ERROR);
        return reason;
    }
    if (wifi_backend_scan_active())
    {
        reason = WIFI_REASON_BUSY;
        (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_JOIN, sequence,
            WIFI_BACKEND_DIAG_STAGE_REQUEST, 0U, 0U, reason,
            WIFI_BACKEND_DIAG_OUTCOME_BUSY);
        return reason;
    }
    whd_security_t security;
    switch (target->security) {
    case WIFI_SECURITY_OPEN: security=WHD_SECURITY_OPEN; break;
    case WIFI_SECURITY_WPA2: security=WHD_SECURITY_WPA2_AES_PSK; break;
    case WIFI_SECURITY_WPA_MIXED: security=WHD_SECURITY_WPA2_WPA_AES_PSK; break;
    default:
        reason = WIFI_REASON_UNSUPPORTED;
        (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_JOIN, sequence,
            WIFI_BACKEND_DIAG_STAGE_REQUEST, 0U, 0U, reason,
            WIFI_BACKEND_DIAG_OUTCOME_ERROR);
        return reason;
    }
    whd_ssid_t ssid; memset(&ssid,0,sizeof(ssid));
    ssid.length=target->ssid_length;memcpy(ssid.value,target->ssid,ssid.length);
    s_cancel_callback.callback=cancel;s_cancel_callback.context=context;
    result=whd_wifi_join_cancelable(s_whd.interface,&ssid,security,
        target->password,target->password_length,radio_cancel,NULL);
    s_cancel_callback.callback=NULL;s_cancel_callback.context=NULL;
    if ((cancel && cancel(context)) || result==WHD_WAIT_ABORTED)
    {
        reason = WIFI_REASON_CANCELLED;
    }
    else if (result==WHD_SUCCESS)
    {
        reason = WIFI_REASON_NONE;
    }
    else if (result==WHD_NETWORK_NOT_FOUND || result==WHD_ACCESS_POINT_NOT_FOUND || result==WHD_WLAN_NOTFOUND)
    {
        reason = WIFI_REASON_NOT_FOUND;
    }
    else if (result==WHD_NOT_AUTHENTICATED || result==WHD_EAPOL_KEY_FAILURE)
    {
        /* These generic outcomes do not prove a wrong password. In particular
         * the firmware can report AUTH/TIMEOUT before any WPA key exchange.
         * Preserve the exact WHD code; automatic policy may retry next round. */
        reason = WIFI_REASON_AUTH_INCOMPLETE;
    }
    else if (result==WHD_INVALID_KEY || result==WHD_NOT_KEYED ||
             result==WHD_EAPOL_KEY_PACKET_M3_TIMEOUT)
    {
        reason = WIFI_REASON_AUTH;
    }
    else if (result==WHD_UNSUPPORTED || result==WHD_WLAN_UNSUPPORTED)
    {
        reason = WIFI_REASON_UNSUPPORTED;
    }
    else
    {
        reason = WIFI_REASON_IO;
    }
    outcome = reason == WIFI_REASON_NONE ? WIFI_BACKEND_DIAG_OUTCOME_SUCCESS :
              reason == WIFI_REASON_CANCELLED ? WIFI_BACKEND_DIAG_OUTCOME_CANCELLED :
              WIFI_BACKEND_DIAG_OUTCOME_ERROR;
    (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_JOIN, sequence,
        WIFI_BACKEND_DIAG_STAGE_DRIVER, result, 0U, reason, outcome);
    return reason;
}
wifi_reason_t wifi_backend_leave(void)
{
    uint32_t sequence = wifi_backend_diag_begin(WIFI_BACKEND_DIAG_LEAVE);
    whd_result_t result;
    wifi_reason_t reason;

    if(!s_whd.ready || !s_whd.interface)
    {
        reason = WIFI_REASON_IO;
        (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_LEAVE, sequence,
            WIFI_BACKEND_DIAG_STAGE_REQUEST, 0U, 0U, reason,
            WIFI_BACKEND_DIAG_OUTCOME_ERROR);
        return reason;
    }
    result = whd_wifi_leave(s_whd.interface);
    /* Disassociating an already disconnected station is idempotent. Keep the
     * exact WHD result in diagnostics, and do not absorb any other failure. */
    reason = (result == WHD_SUCCESS || result == WHD_WLAN_NOTASSOCIATED) ?
             WIFI_REASON_NONE : WIFI_REASON_IO;
    (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_LEAVE, sequence,
        WIFI_BACKEND_DIAG_STAGE_DRIVER, result, 0U, reason,
        reason == WIFI_REASON_NONE ? WIFI_BACKEND_DIAG_OUTCOME_SUCCESS :
                                     WIFI_BACKEND_DIAG_OUTCOME_ERROR);
    return reason;
}
int wifi_backend_scan(void)
{
    uint32_t sequence;
    whd_result_t result;
    rt_err_t sem_result;
    bool busy;
    bool sem_initialized;

    if(!s_whd.ready || !s_whd.interface)
    {
        sequence = wifi_backend_diag_begin(WIFI_BACKEND_DIAG_SCAN);
        (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_SCAN, sequence,
            WIFI_BACKEND_DIAG_STAGE_REQUEST, 0U, 0U, -RT_EIO,
            WIFI_BACKEND_DIAG_OUTCOME_ERROR);
        return -RT_EIO;
    }
    rt_enter_critical();
    busy = s_whd.scan_active != RT_FALSE;
    if (!busy)
    {
        /* Reserve the single WHD scan callback slot before starting any API
         * work. A callback cannot occur until whd_wifi_scan() is submitted. */
        s_whd.scan_active = RT_TRUE;
        s_whd.scan_stopping = RT_FALSE;
        s_whd.scan_sequence = 0U;
    }
    rt_exit_critical();
    if(busy)
    {
        wifi_backend_diag_note_scan_busy();
        return -RT_EBUSY;
    }
    sequence = wifi_backend_diag_begin(WIFI_BACKEND_DIAG_SCAN);
    rt_enter_critical();
    s_whd.scan_sequence = sequence;
    sem_initialized = s_scan_terminal_sem_initialized;
    rt_exit_critical();
    if (!sem_initialized)
    {
        sem_result = rt_sem_init(&s_scan_terminal_sem, "wfscan", 0,
                                 RT_IPC_FLAG_FIFO);
        if (sem_result != RT_EOK)
        {
            rt_enter_critical();
            if (s_whd.scan_sequence == sequence)
            {
                s_whd.scan_active = RT_FALSE;
                s_whd.scan_stopping = RT_FALSE;
            }
            rt_exit_critical();
            (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_SCAN, sequence,
                WIFI_BACKEND_DIAG_STAGE_REQUEST, 0U, 0U, -RT_EIO,
                WIFI_BACKEND_DIAG_OUTCOME_ERROR);
            return -RT_EIO;
        }
        rt_enter_critical();
        s_scan_terminal_sem_initialized = true;
        rt_exit_critical();
    }
    while (rt_sem_take(&s_scan_terminal_sem, RT_WAITING_NO) == RT_EOK)
    {
        /* Discard only completion tokens left by an already-finalized scan. */
    }
    mongoose_scan_begin();memset(&s_scan_result,0,sizeof(s_scan_result));
    result=whd_wifi_scan(s_whd.interface,WHD_SCAN_TYPE_ACTIVE,WHD_BSS_TYPE_ANY,
        NULL,NULL,NULL,NULL,mongoose_whd_scan_callback,&s_scan_result,
        (void *)(uintptr_t)sequence);
    if(result!=WHD_SUCCESS)
    {
        bool recorded = wifi_backend_diag_update(WIFI_BACKEND_DIAG_SCAN, sequence,
            WIFI_BACKEND_DIAG_STAGE_DRIVER, result, 0U,
            result == WHD_WLAN_BUSY ? -RT_EBUSY : -RT_EIO,
            result == WHD_WLAN_BUSY ? WIFI_BACKEND_DIAG_OUTCOME_BUSY :
                                      WIFI_BACKEND_DIAG_OUTCOME_ERROR);
        if (recorded)
        {
            rt_enter_critical();
            busy = s_whd.scan_active && s_whd.scan_sequence == sequence;
            if (busy)
            {
                s_whd.scan_active = RT_FALSE;
                s_whd.scan_stopping = RT_FALSE;
            }
            rt_exit_critical();
            if (busy)
            {
                mongoose_scan_end(RT_FALSE);
            }
        }
        return result == WHD_WLAN_BUSY ? -RT_EBUSY : -RT_EIO;
    }
    (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_SCAN, sequence,
        WIFI_BACKEND_DIAG_STAGE_DRIVER, result, 0U, RT_EOK,
        WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS);
    return RT_EOK;
}
int wifi_backend_stop_scan(void)
{
    uint32_t sequence;
    rt_tick_t start_tick;
    rt_tick_t timeout_ticks;
    rt_tick_t elapsed_ticks;
    rt_err_t wait_result;
    whd_result_t result;
    bool active;
    bool stopping;
    bool sem_initialized;

    rt_enter_critical();
    active = s_whd.scan_active != RT_FALSE;
    sequence = s_whd.scan_sequence;
    stopping = s_whd.scan_stopping != RT_FALSE;
    sem_initialized = s_scan_terminal_sem_initialized;
    if (active && !stopping)
    {
        s_whd.scan_stopping = RT_TRUE;
    }
    rt_exit_critical();
    if (!active)
    {
        return RT_EOK;
    }
    if (sequence == 0U || !sem_initialized)
    {
        return -RT_ERROR;
    }

    if (!stopping)
    {
        result = whd_wifi_stop_scan(s_whd.interface);
        if (result != WHD_SUCCESS)
        {
            (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_SCAN, sequence,
                WIFI_BACKEND_DIAG_STAGE_DRIVER, result, 0U, -RT_EIO,
                WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS);
            rt_enter_critical();
            active = s_whd.scan_active && s_whd.scan_sequence == sequence;
            if (active)
            {
                s_whd.scan_stopping = RT_FALSE;
            }
            rt_exit_critical();
            return active ? -RT_EIO : RT_EOK;
        }
        (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_SCAN, sequence,
            WIFI_BACKEND_DIAG_STAGE_DRIVER, result, 0U, RT_EOK,
            WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS);
    }

    start_tick = rt_tick_get();
    timeout_ticks = rt_tick_from_millisecond((rt_int32_t)MONGOOSE_SCAN_STOP_TIMEOUT_MS);
    for (;;)
    {
        elapsed_ticks = rt_tick_get() - start_tick;
        if (elapsed_ticks >= timeout_ticks)
        {
            wait_result = -RT_ETIMEOUT;
        }
        else
        {
            wait_result = rt_sem_take(&s_scan_terminal_sem,
                                      (rt_int32_t)(timeout_ticks - elapsed_ticks));
        }

        rt_enter_critical();
        active = s_whd.scan_active && s_whd.scan_sequence == sequence;
        rt_exit_critical();
        if (!active)
        {
            return RT_EOK;
        }
        if (wait_result != RT_EOK)
        {
            /* Leave active/stopping asserted. The coordinator fails closed;
             * a later explicit radio request may wait again for this callback. */
            (void)wifi_backend_diag_update(WIFI_BACKEND_DIAG_SCAN, sequence,
                WIFI_BACKEND_DIAG_STAGE_DRIVER, WHD_SUCCESS, 0U,
                wait_result == -RT_ETIMEOUT ? -RT_ETIMEOUT : -RT_EIO,
                WIFI_BACKEND_DIAG_OUTCOME_IN_PROGRESS);
            return wait_result == -RT_ETIMEOUT ? -RT_ETIMEOUT : -RT_EIO;
        }
        /* A stale semaphore token is not completion; only the callback's
         * protected state transition releases the waiting radio worker. */
    }
}
int wifi_backend_get_diagnostics(wifi_backend_diagnostics_t *out)
{
    if (out == RT_NULL)
    {
        return -RT_EINVAL;
    }
    rt_enter_critical();
    *out = s_backend_diagnostics;
    rt_exit_critical();
    return RT_EOK;
}
bool wifi_backend_scan_active(void)
{
    bool active;
    rt_enter_critical();
    active = s_whd.scan_active != RT_FALSE;
    rt_exit_critical();
    return active;
}
bool wifi_backend_link_ready(void)
{
    return s_whd.ready && s_whd.link_up && !s_whd.event_link_down &&
        whd_wifi_is_ready_to_transceive(s_whd.interface)==WHD_SUCCESS;
}
void wifi_backend_publish_link(bool ready)
{
    if(ready)s_whd.event_link_down=RT_FALSE;
    s_whd.link_up=ready?RT_TRUE:RT_FALSE;
}

static void mongoose_wifi_set_ip_and_status(struct mg_tcpip_if *ifp)
{
    if(ifp->state==MG_TCPIP_STATE_READY) {
        char ip[32];
        mg_snprintf(ip,sizeof(ip),"%M",mg_print_ip4,&ifp->ip);
        wifi_manager_notify_ip_ready(s_whd.net_generation,ip);
    }
}

static void mongoose_reset_dhcp_session(void)
{
    struct mg_connection *connection;

    /* A lost DHCP ACK leaves Mongoose's small built-in state machine in
     * REQUESTING forever.  Reset only the TCP/IP session; the WHD association
     * remains up and will continue to carry the new DHCP exchange. */
    if (!s_manager_ready || s_ifp != &s_mongoose_if)
    {
        return;
    }
    for (connection = s_mgr.conns; connection != RT_NULL;
         connection = connection->next)
    {
        if (!connection->is_listening && !connection->is_udp)
        {
            connection->is_closing = 1U;
        }
    }
    s_mongoose_if.ip = 0U;
    s_mongoose_if.mask = 0U;
    s_mongoose_if.gw = 0U;
    s_mongoose_if.gw_ready = false;
    s_mongoose_if.lease_expire = 0U;
    s_mongoose_if.timer_1000ms = 0U;
    if(!s_dhcp_xid_seeded) {
        (void)mg_random(&s_dhcp_client_xid,sizeof(s_dhcp_client_xid));
        s_dhcp_client_xid^=DWT->CYCCNT^(uint32_t)rt_tick_get();
        s_dhcp_xid_seeded=true;
    }
    /* An odd increment visits every 32-bit value. Reuse this transaction
     * within a DHCP round, rotate it for a new association/session retry.
     * This is freshness bookkeeping, not cryptographic authentication. */
    s_dhcp_client_xid+=0x9E3779B9UL;
    if(!s_dhcp_client_xid)s_dhcp_client_xid+=0x9E3779B9UL;
    s_mongoose_if.dhcp_client_xid=s_dhcp_client_xid;
    s_mongoose_if.state = MG_TCPIP_STATE_UP;
    s_dhcp_request_started = 0U;
    wifi_set_ip("");
}

void wifi_backend_network_reset(uint32_t generation)
{
    s_whd.net_generation=generation;
    wifi_backend_publish_link(false);
    mongoose_reset_dhcp_session();
    s_mongoose_if.state=MG_TCPIP_STATE_DOWN;
    /* Producer is WHD's thread, not an ISR. Discard old session frames atomically. */
    rt_enter_critical();
    s_mongoose_if.recv_queue.head=s_mongoose_if.recv_queue.tail=0;
    rt_exit_critical();
}
void wifi_backend_dhcp_reset(void) { mongoose_reset_dhcp_session(); }

static void mongoose_wifi_event(struct mg_tcpip_if *ifp, int event,
                                void *event_data)
{
    rt_uint8_t state;

    if (ifp == RT_NULL)
    {
        return;
    }
    switch (event)
    {
    case MG_TCPIP_EV_STATE_CHANGE:
        state = event_data == RT_NULL ? ifp->state :
                *(const rt_uint8_t *)event_data;
        if (state == MG_TCPIP_STATE_REQ)
        {
            if (s_dhcp_request_started == 0U)
            {
                s_dhcp_request_started = rt_tick_get();
            }
        }
        else
        {
            s_dhcp_request_started = 0U;
        }
        mongoose_wifi_set_ip_and_status(ifp);
        break;

    case MG_TCPIP_EV_TIMER_1S:
        if (ifp->state == MG_TCPIP_STATE_READY)
        {
            mongoose_wifi_set_ip_and_status(ifp);
        }
        break;

    case MG_TCPIP_EV_WIFI_CONNECT_ERR:
        wifi_set_ip("");
        wifi_set_status(WIFI_STATE_FAILED, "WiFi 连接失败");
        break;

    default:
        break;
    }
}

static struct mongoose_http_connection_state *mongoose_http_state(
    struct mg_connection *connection)
{
    struct mongoose_http_connection_state *state;

    if (connection == RT_NULL || !connection->is_accepted)
    {
        return RT_NULL;
    }
    state = (struct mongoose_http_connection_state *)connection->data;
    return state->magic == MONGOOSE_HTTP_STATE_MAGIC ? state : RT_NULL;
}

static void mongoose_http_pdf_response_release(
    struct mongoose_http_connection_state *state)
{
    if (state != RT_NULL && state->pdf_response_pending)
    {
        if (s_http_pdf_responses_active != 0U)
        {
            s_http_pdf_responses_active--;
        }
        state->pdf_response_pending = 0U;
    }
}

static rt_bool_t mongoose_http_is_pdf_report_final_chunk(
    const char *path, int response_length)
{
    static const char prefix[] = "/api/report/chunk/";
    const char *cursor;
    rt_uint32_t values[2] = { 0U, 0U };
    rt_uint32_t field;

    if (!path || response_length <= 0 ||
        rt_strncmp(path, prefix, sizeof(prefix) - 1U) != 0)
    {
        return RT_FALSE;
    }
    cursor = path + sizeof(prefix) - 1U;
    for (field = 0U; field < 2U; field++)
    {
        if (*cursor < '0' || *cursor > '9')
        {
            return RT_FALSE;
        }
        while (*cursor >= '0' && *cursor <= '9')
        {
            rt_uint32_t digit = (rt_uint32_t)(*cursor++ - '0');
            if (values[field] > (0xFFFFFFFFU - digit) / 10U)
            {
                return RT_FALSE;
            }
            values[field] = values[field] * 10U + digit;
        }
        if (field == 0U)
        {
            if (*cursor++ != '/')
            {
                return RT_FALSE;
            }
        }
        else if (*cursor != '\0')
        {
            return RT_FALSE;
        }
    }
    return web_pdf_report_is_final_chunk(values[0], values[1],
                                         (rt_size_t)response_length);
}

static rt_bool_t mongoose_http_parse_pdf_stream_path(
    const char *path, rt_uint32_t *job_id)
{
    static const char prefix[] = "/api/report/stream/";
    const char *cursor;
    rt_uint32_t value = 0U;

    if (path == RT_NULL || job_id == RT_NULL ||
        rt_strncmp(path, prefix, sizeof(prefix) - 1U) != 0)
    {
        return RT_FALSE;
    }
    cursor = path + sizeof(prefix) - 1U;
    if (*cursor < '0' || *cursor > '9')
    {
        return RT_FALSE;
    }
    while (*cursor >= '0' && *cursor <= '9')
    {
        rt_uint32_t digit = (rt_uint32_t)(*cursor++ - '0');

        if (value > (0xFFFFFFFFU - digit) / 10U)
        {
            return RT_FALSE;
        }
        value = value * 10U + digit;
    }
    if (*cursor != '\0' || value == 0U)
    {
        return RT_FALSE;
    }
    *job_id = value;
    return RT_TRUE;
}

static rt_uint32_t mongoose_http_now_ms(void)
{
    return (rt_uint32_t)mg_millis();
}

static rt_bool_t mongoose_http_elapsed(rt_uint32_t now,
                                       rt_uint32_t since,
                                       rt_uint32_t timeout)
{
    return (rt_uint32_t)(now - since) >= timeout ? RT_TRUE : RT_FALSE;
}

static void mongoose_http_connection_header(char *header,
                                             rt_size_t header_capacity,
                                             rt_bool_t close_after,
                                             rt_uint32_t max_requests)
{
    if (header == RT_NULL || header_capacity == 0U)
    {
        return;
    }
    if (close_after)
    {
        (void)rt_snprintf(header, header_capacity, "Connection: close\r\n");
    }
    else
    {
        (void)rt_snprintf(header, header_capacity,
            "Connection: keep-alive\r\nKeep-Alive: timeout=10, max=%lu\r\n",
            (unsigned long)max_requests);
    }
}

static void mongoose_http_schedule_close(struct mg_connection *connection)
{
    /* is_resp=1 prevents http_cb from consuming a pipelined request while
     * the already queued response is being flushed.  The Mongoose poll loop
     * sends FIN only after send.len reaches zero. */
    connection->is_draining = 1U;
    connection->is_resp = 1U;
}

static void mongoose_http_close_after_response(
    struct mg_connection *connection)
{
    if (!connection->is_draining)
    {
        s_http_stats.response_closed++;
    }
    mongoose_http_schedule_close(connection);
}

static void mongoose_http_stats_snapshot(struct mongoose_http_stats *stats)
{
    rt_base_t level;

    if (stats == RT_NULL)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    *stats = s_http_stats;
    rt_hw_interrupt_enable(level);
}

static rt_bool_t mongoose_http_request_wants_close(
    struct mg_http_message *message)
{
    struct mg_str *header;

    header = mg_http_get_header(message, "Connection");
    if (header != RT_NULL)
    {
        if (mg_strcasecmp(*header, mg_str("close")) == 0)
        {
            return RT_TRUE;
        }
        if (mg_strcasecmp(*header, mg_str("keep-alive")) == 0)
        {
            return RT_FALSE;
        }
    }
    /* HTTP/1.0 is not persistent unless the client explicitly requested it. */
    return mg_strcasecmp(message->proto, mg_str("HTTP/1.0")) == 0 ?
           RT_TRUE : RT_FALSE;
}

static rt_bool_t mongoose_http_accepts_gzip(struct mg_http_message *message)
{
    struct mg_str *header = mg_http_get_header(message, "Accept-Encoding");
    rt_size_t index;

    if (header == RT_NULL) return RT_FALSE;
    for (index = 0U; index + 4U <= header->len; index++)
    {
        const char *value = header->buf;
        rt_size_t cursor;
        rt_bool_t disabled = RT_FALSE;

        if ((index != 0U && value[index - 1U] != ',' &&
             value[index - 1U] != ' ' && value[index - 1U] != '\t') ||
            (value[index] | 0x20) != 'g' ||
            (value[index + 1U] | 0x20) != 'z' ||
            (value[index + 2U] | 0x20) != 'i' ||
            (value[index + 3U] | 0x20) != 'p' ||
            (index + 4U < header->len && value[index + 4U] != ',' &&
             value[index + 4U] != ';' && value[index + 4U] != ' ' &&
             value[index + 4U] != '\t'))
        {
            continue;
        }
        for (cursor = index + 4U;
             cursor + 2U < header->len && value[cursor] != ','; cursor++)
        {
            if ((value[cursor] | 0x20) == 'q' && value[cursor + 1U] == '=')
            {
                rt_size_t quality = cursor + 2U;
                while (quality < header->len && value[quality] == ' ')
                    quality++;
                if (quality < header->len && value[quality] == '0')
                {
                    quality++;
                    if (quality < header->len && value[quality] == '.')
                    {
                        quality++;
                        while (quality < header->len && value[quality] == '0')
                            quality++;
                    }
                    if (quality == header->len || value[quality] == ',' ||
                        value[quality] == ';' || value[quality] == ' ')
                        disabled = RT_TRUE;
                }
                break;
            }
        }
        if (!disabled) return RT_TRUE;
    }
    return RT_FALSE;
}

static rt_bool_t mongoose_http_is_critical_static_asset(const char *path)
{
    return path != RT_NULL &&
           (rt_strcmp(path, "/assets/app.js") == 0 ||
            rt_strcmp(path, "/assets/style.css") == 0) ? RT_TRUE : RT_FALSE;
}

static void mongoose_http_reply_json(struct mg_connection *connection,
                                     int status, const char *headers,
                                     const char *body,
                                     rt_bool_t close_after)
{
    mg_http_reply(connection, status, headers, "%s", body);
    if (close_after)
    {
        mongoose_http_close_after_response(connection);
    }
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
enum mongoose_wifi_bench_phase
{
    MONGOOSE_WIFI_BENCH_HEADER = 0,
    MONGOOSE_WIFI_BENCH_UPLOAD,
    MONGOOSE_WIFI_BENCH_DOWNLOAD,
    MONGOOSE_WIFI_BENCH_DOWNLOAD_DRAIN,
    MONGOOSE_WIFI_BENCH_RESULT,
    MONGOOSE_WIFI_BENCH_CANCELLED
};

struct mongoose_wifi_bench_connection_state
{
    rt_uint32_t magic;
    rt_uint32_t accepted_ms;
    rt_uint32_t started_ms;
    rt_uint32_t expected_bytes;
    rt_uint32_t payload_bytes;
    rt_uint32_t queued_bytes;
    rt_uint32_t elapsed_ms;
    rt_uint32_t run_id;
    rt_uint32_t send_high_water;
    rt_uint32_t pump_calls;
    rt_uint32_t empty_pump_calls;
    rt_uint32_t append_calls;
    rt_uint32_t append_bytes;
    rt_uint32_t send_len_min;
    rt_uint32_t send_len_max;
    rt_uint32_t rx_queue_drop_baseline;
    rt_uint16_t rx_window_bytes;
    rt_uint8_t rx_budget_frames;
    rt_uint8_t reserved2;
    rt_uint8_t header[MONGOOSE_WIFI_BENCH_HEADER_SIZE];
    rt_uint8_t header_bytes;
    rt_uint8_t mode;
    rt_uint8_t phase;
    rt_uint8_t reserved;
};
typedef char mongoose_wifi_bench_state_fits_mg_data[
    sizeof(struct mongoose_wifi_bench_connection_state) <= MG_DATA_SIZE ? 1 : -1];

static rt_uint32_t mongoose_wifi_bench_read_u32_be(const rt_uint8_t *bytes)
{
    return ((rt_uint32_t)bytes[0] << 24U) |
           ((rt_uint32_t)bytes[1] << 16U) |
           ((rt_uint32_t)bytes[2] << 8U) |
           (rt_uint32_t)bytes[3];
}

static void mongoose_wifi_bench_write_u32_be(rt_uint8_t *bytes,
                                              rt_uint32_t value)
{
    bytes[0] = (rt_uint8_t)(value >> 24U);
    bytes[1] = (rt_uint8_t)(value >> 16U);
    bytes[2] = (rt_uint8_t)(value >> 8U);
    bytes[3] = (rt_uint8_t)value;
}

static rt_bool_t mongoose_wifi_bench_parse_header(
    struct mongoose_wifi_bench_connection_state *state)
{
    rt_uint32_t expected;

    if (state == RT_NULL || memcmp(state->header, "AP2T", 4U) != 0 ||
        state->header[5] != 0U || state->header[6] != 0U ||
        state->header[7] != 0U)
    {
        return RT_FALSE;
    }
    expected = mongoose_wifi_bench_read_u32_be(&state->header[8]);
    if (expected < MONGOOSE_WIFI_BENCH_MIN_BYTES ||
        expected > MONGOOSE_WIFI_BENCH_MAX_BYTES)
    {
        return RT_FALSE;
    }
    if (state->header[4] != (rt_uint8_t)'U' &&
        state->header[4] != (rt_uint8_t)'D')
    {
        return RT_FALSE;
    }

    state->mode = state->header[4];
    state->expected_bytes = expected;
    state->started_ms = mongoose_http_now_ms();
    state->phase = state->mode == (rt_uint8_t)'U' ?
                   MONGOOSE_WIFI_BENCH_UPLOAD :
                   MONGOOSE_WIFI_BENCH_DOWNLOAD;
    return RT_TRUE;
}

static rt_bool_t mongoose_wifi_bench_send_result(
    struct mg_connection *connection,
    struct mongoose_wifi_bench_connection_state *state)
{
    rt_uint8_t result[12] = { 'W', 'B', 'R', '1', 0U, 0U, 0U, 0U,
                              0U, 0U, 0U, 0U };
    rt_uint32_t elapsed_ms;

    if (connection == RT_NULL || state == RT_NULL)
    {
        return RT_FALSE;
    }
    elapsed_ms = mongoose_http_now_ms() - state->started_ms;
    state->elapsed_ms = elapsed_ms;
    mongoose_wifi_bench_write_u32_be(&result[4], state->payload_bytes);
    mongoose_wifi_bench_write_u32_be(&result[8], elapsed_ms);
    if (!mg_send(connection, result, sizeof(result)))
    {
        connection->is_closing = 1U;
        return RT_FALSE;
    }
    state->phase = MONGOOSE_WIFI_BENCH_RESULT;
    connection->is_draining = 1U;
    return RT_TRUE;
}

static void mongoose_wifi_bench_pump_download(
    struct mg_connection *connection,
    struct mongoose_wifi_bench_connection_state *state)
{
    if (connection == RT_NULL || state == RT_NULL ||
        state->mode != (rt_uint8_t)'D' ||
        state->phase == MONGOOSE_WIFI_BENCH_RESULT ||
        state->phase == MONGOOSE_WIFI_BENCH_CANCELLED)
    {
        return;
    }

    if (state->phase == MONGOOSE_WIFI_BENCH_DOWNLOAD &&
        state->queued_bytes < state->expected_bytes)
    {
        size_t queued = connection->send.len;
        state->pump_calls++;
        if (state->queued_bytes != 0U && queued == 0U)
            state->empty_pump_calls++;
        if (state->queued_bytes != 0U && queued < state->send_len_min)
            state->send_len_min = (rt_uint32_t)queued;
        if (queued > state->send_len_max)
            state->send_len_max = (rt_uint32_t)queued;
    }

    while (state->phase == MONGOOSE_WIFI_BENCH_DOWNLOAD &&
           state->queued_bytes < state->expected_bytes &&
           connection->send.len < state->send_high_water)
    {
        size_t room = state->send_high_water - connection->send.len;
        size_t remaining = (size_t)(state->expected_bytes -
                                    state->queued_bytes);
        size_t length = sizeof(s_wifi_bench_pattern);

        if (length > room) length = room;
        if (length > remaining) length = remaining;
        if (length == 0U ||
            !mg_send(connection, s_wifi_bench_pattern, length))
        {
            connection->is_closing = 1U;
            return;
        }
        state->queued_bytes += (rt_uint32_t)length;
        state->append_calls++;
        state->append_bytes += (rt_uint32_t)length;
        if (connection->send.len > state->send_len_max)
            state->send_len_max = (rt_uint32_t)connection->send.len;
    }

    if (state->phase == MONGOOSE_WIFI_BENCH_DOWNLOAD &&
        state->queued_bytes == state->expected_bytes)
    {
        state->phase = MONGOOSE_WIFI_BENCH_DOWNLOAD_DRAIN;
    }
    if (state->phase == MONGOOSE_WIFI_BENCH_DOWNLOAD_DRAIN &&
        connection->send.len == 0U)
    {
        state->payload_bytes = state->expected_bytes;
        (void)mongoose_wifi_bench_send_result(connection, state);
    }
}

static void mongoose_wifi_bench_event(struct mg_connection *connection,
                                      int event, void *event_data)
{
    struct mongoose_wifi_bench_connection_state *state;

    (void)event_data;
    if (connection == RT_NULL)
    {
        return;
    }
    if (event == MG_EV_ACCEPT)
    {
        if (!connection->is_accepted)
        {
            return;
        }
        if (s_wifi_bench_active)
        {
            connection->is_closing = 1U;
            return;
        }
        state = (struct mongoose_wifi_bench_connection_state *)
                connection->data;
        rt_memset(state, 0, sizeof(*state));
        state->magic = MONGOOSE_WIFI_BENCH_STATE_MAGIC;
        state->accepted_ms = mongoose_http_now_ms();
        state->phase = MONGOOSE_WIFI_BENCH_HEADER;
        state->run_id = s_wifi_bench_run_id;
        state->send_high_water = s_wifi_bench_send_high_water;
        state->send_len_min = UINT32_MAX;
        state->rx_window_bytes = mg_tcpip_debug_get_advertised_window();
        state->rx_budget_frames =
            (rt_uint8_t)mg_tcpip_debug_get_rx_budget();
        state->rx_queue_drop_baseline = (rt_uint32_t)s_mongoose_if.ndrop;
        mg_tcpip_debug_reset_rx_budget_exhaustions();
        s_wifi_bench_active = RT_TRUE;
        s_wifi_bench_active_connection = connection;
        return;
    }
    if (!connection->is_accepted)
    {
        if (event == MG_EV_CLOSE && connection == s_wifi_bench_listener)
        {
            s_wifi_bench_listener = RT_NULL;
            s_wifi_bench_listener_opened_ms = 0U;
        }
        return;
    }

    state = (struct mongoose_wifi_bench_connection_state *)
            connection->data;
    if (event == MG_EV_CLOSE)
    {
        if (state->magic == MONGOOSE_WIFI_BENCH_STATE_MAGIC)
        {
            rt_memset(&s_wifi_bench_last_stats, 0,
                      sizeof(s_wifi_bench_last_stats));
            s_wifi_bench_last_stats.valid = RT_TRUE;
            s_wifi_bench_last_stats.completed =
                state->phase == MONGOOSE_WIFI_BENCH_RESULT &&
                state->payload_bytes == state->expected_bytes;
            s_wifi_bench_last_stats.mode = state->mode;
            s_wifi_bench_last_stats.run_id = state->run_id;
            s_wifi_bench_last_stats.send_high_water = state->send_high_water;
            s_wifi_bench_last_stats.expected_bytes = state->expected_bytes;
            s_wifi_bench_last_stats.payload_bytes = state->payload_bytes;
            s_wifi_bench_last_stats.elapsed_ms = state->elapsed_ms != 0U ?
                state->elapsed_ms : state->started_ms != 0U ?
                    mongoose_http_now_ms() - state->started_ms : 0U;
            s_wifi_bench_last_stats.pump_calls = state->pump_calls;
            s_wifi_bench_last_stats.empty_pump_calls = state->empty_pump_calls;
            s_wifi_bench_last_stats.append_calls = state->append_calls;
            s_wifi_bench_last_stats.append_bytes = state->append_bytes;
            s_wifi_bench_last_stats.send_len_min =
                state->send_len_min == UINT32_MAX ? 0U : state->send_len_min;
            s_wifi_bench_last_stats.send_len_max = state->send_len_max;
            s_wifi_bench_last_stats.rx_window_bytes = state->rx_window_bytes;
            s_wifi_bench_last_stats.rx_budget_frames =
                state->rx_budget_frames;
            s_wifi_bench_last_stats.rx_budget_exhausted_polls =
                mg_tcpip_debug_get_rx_budget_exhaustions();
            s_wifi_bench_last_stats.rx_queue_drops =
                (rt_uint32_t)s_mongoose_if.ndrop -
                state->rx_queue_drop_baseline;
            state->magic = 0U;
            s_wifi_bench_active = RT_FALSE;
            s_wifi_bench_active_connection = RT_NULL;
        }
        return;
    }
    if (state->magic != MONGOOSE_WIFI_BENCH_STATE_MAGIC)
    {
        return;
    }
    if (event == MG_EV_ERROR)
    {
        connection->is_closing = 1U;
        return;
    }
    if (event == MG_EV_READ)
    {
        if (state->phase == MONGOOSE_WIFI_BENCH_CANCELLED)
        {
            connection->is_closing = 1U;
            return;
        }
        if (state->phase == MONGOOSE_WIFI_BENCH_RESULT)
        {
            if (connection->recv.len != 0U) connection->is_closing = 1U;
            return;
        }
        while (connection->recv.len != 0U &&
               state->phase == MONGOOSE_WIFI_BENCH_HEADER)
        {
            size_t needed = MONGOOSE_WIFI_BENCH_HEADER_SIZE -
                            state->header_bytes;
            size_t copied = connection->recv.len < needed ?
                            connection->recv.len : needed;
            rt_memcpy(&state->header[state->header_bytes],
                      connection->recv.buf, copied);
            (void)mg_iobuf_del(&connection->recv, 0U, copied);
            state->header_bytes += (rt_uint8_t)copied;
            if (state->header_bytes == MONGOOSE_WIFI_BENCH_HEADER_SIZE &&
                !mongoose_wifi_bench_parse_header(state))
            {
                connection->is_closing = 1U;
                return;
            }
        }

        if (state->phase == MONGOOSE_WIFI_BENCH_UPLOAD &&
            connection->recv.len != 0U)
        {
            size_t remaining = (size_t)(state->expected_bytes -
                                        state->payload_bytes);
            size_t received = connection->recv.len;
            if (received > remaining)
            {
                connection->is_closing = 1U;
                return;
            }
            state->payload_bytes += (rt_uint32_t)received;
            (void)mg_iobuf_del(&connection->recv, 0U, received);
            if (state->payload_bytes == state->expected_bytes)
            {
                (void)mongoose_wifi_bench_send_result(connection, state);
            }
        }
        else if (state->mode == (rt_uint8_t)'D' &&
                 connection->recv.len != 0U)
        {
            connection->is_closing = 1U;
            return;
        }
        mongoose_wifi_bench_pump_download(connection, state);
        return;
    }
    if (event == MG_EV_WRITE || event == MG_EV_POLL)
    {
        if (mongoose_http_elapsed(mongoose_http_now_ms(),
                                  state->accepted_ms,
                                  MONGOOSE_WIFI_BENCH_CONNECTION_TTL_MS))
        {
            state->phase = MONGOOSE_WIFI_BENCH_CANCELLED;
            connection->is_closing = 1U;
            return;
        }
        mongoose_wifi_bench_pump_download(connection, state);
    }
}

static void mongoose_wifi_bench_initialize_pattern(void)
{
    rt_uint32_t index;

    if (s_wifi_bench_pattern_ready)
    {
        return;
    }
    for (index = 0U; index < sizeof(s_wifi_bench_pattern); ++index)
    {
        s_wifi_bench_pattern[index] = (rt_uint8_t)(index * 73U + 19U);
    }
    s_wifi_bench_pattern_ready = RT_TRUE;
}

static rt_bool_t mongoose_wifi_bench_parse_parameters(
    const struct mg_http_message *message,
    rt_uint32_t *send_high_water,
    rt_uint16_t *rx_window_bytes,
    rt_uint8_t *rx_budget_frames)
{
    char query[80];
    char *entry;
    rt_bool_t high_water_seen = RT_FALSE;
    rt_bool_t rx_window_seen = RT_FALSE;
    rt_bool_t rx_budget_seen = RT_FALSE;

    if (message == RT_NULL || send_high_water == RT_NULL ||
        rx_window_bytes == RT_NULL || rx_budget_frames == RT_NULL)
        return RT_FALSE;
    *send_high_water = MONGOOSE_WIFI_BENCH_SEND_HIGH_WATER;
    *rx_window_bytes = mg_tcpip_debug_get_advertised_window();
    *rx_budget_frames =
        (rt_uint8_t)mg_tcpip_debug_get_rx_budget();
    if (message->query.len == 0U)
    {
        return RT_TRUE;
    }
    if (message->query.len >= sizeof(query))
        return RT_FALSE;

    rt_memcpy(query, message->query.buf, message->query.len);
    query[message->query.len] = '\0';
    entry = query;
    while (entry != RT_NULL)
    {
        char *next = strchr(entry, '&');
        if (next != RT_NULL)
        {
            *next = '\0';
            ++next;
        }
        if (rt_strncmp(entry, "high_water_kib=", 15U) == 0)
        {
            if (high_water_seen) return RT_FALSE;
            high_water_seen = RT_TRUE;
            if (rt_strcmp(entry + 15U, "4") == 0)
                *send_high_water = MONGOOSE_WIFI_BENCH_SEND_LOW_WATER;
            else if (rt_strcmp(entry + 15U, "8") == 0)
                *send_high_water = MONGOOSE_WIFI_BENCH_SEND_HIGH_WATER;
            else if (rt_strcmp(entry + 15U, "16") == 0)
                *send_high_water = MONGOOSE_WIFI_BENCH_SEND_MAX_WATER;
            else
                return RT_FALSE;
        }
        else if (rt_strncmp(entry, "rx_window_bytes=", 16U) == 0)
        {
            if (rx_window_seen) return RT_FALSE;
            rx_window_seen = RT_TRUE;
            if (rt_strcmp(entry + 16U, "44000") == 0)
                *rx_window_bytes = 44000U;
            else if (rt_strcmp(entry + 16U, "61440") == 0)
                *rx_window_bytes = 61440U;
            else
                return RT_FALSE;
        }
        else if (rt_strncmp(entry, "rx_budget_frames=", 17U) == 0)
        {
            if (rx_budget_seen) return RT_FALSE;
            rx_budget_seen = RT_TRUE;
            if (rt_strcmp(entry + 17U, "4") == 0)
                *rx_budget_frames = 4U;
            else if (rt_strcmp(entry + 17U, "8") == 0)
                *rx_budget_frames = 8U;
            else if (rt_strcmp(entry + 17U, "16") == 0)
                *rx_budget_frames = 16U;
            else
                return RT_FALSE;
        }
        else
        {
            return RT_FALSE;
        }
        entry = next;
    }
    return RT_TRUE;
}

static rt_bool_t mongoose_wifi_bench_start(rt_uint32_t send_high_water,
                                           rt_uint16_t rx_window_bytes,
                                           rt_uint8_t rx_budget_frames)
{
    mongoose_wifi_bench_initialize_pattern();
    if (s_wifi_bench_listener != RT_NULL || s_wifi_bench_active ||
        s_wifi_bench_active_connection != RT_NULL)
    {
        return RT_FALSE;
    }
    if (!mg_tcpip_debug_set_advertised_window(rx_window_bytes))
    {
        return RT_FALSE;
    }
    if (!mg_tcpip_debug_set_rx_budget(rx_budget_frames))
    {
        (void)mg_tcpip_debug_set_advertised_window(44000U);
        return RT_FALSE;
    }
    s_wifi_bench_listener = mg_listen(&s_mgr, "tcp://0.0.0.0:5201",
                                      mongoose_wifi_bench_event, RT_NULL);
    if (s_wifi_bench_listener == RT_NULL)
    {
        (void)mg_tcpip_debug_set_advertised_window(44000U);
        (void)mg_tcpip_debug_set_rx_budget(4U);
        return RT_FALSE;
    }
    ++s_wifi_bench_run_id;
    if (s_wifi_bench_run_id == 0U) ++s_wifi_bench_run_id;
    s_wifi_bench_send_high_water = send_high_water;
    rt_memset(&s_wifi_bench_last_stats, 0, sizeof(s_wifi_bench_last_stats));
    s_wifi_bench_listener_opened_ms = mongoose_http_now_ms();
    return RT_TRUE;
}

static void mongoose_wifi_bench_stop(void)
{
    struct mg_connection *listener = s_wifi_bench_listener;

    s_wifi_bench_listener = RT_NULL;
    s_wifi_bench_listener_opened_ms = 0U;
    if (listener != RT_NULL)
    {
        mg_close_conn(listener);
    }
    if (s_wifi_bench_active_connection != RT_NULL)
    {
        struct mongoose_wifi_bench_connection_state *state =
            (struct mongoose_wifi_bench_connection_state *)
                s_wifi_bench_active_connection->data;
        if (state->phase != MONGOOSE_WIFI_BENCH_RESULT)
            state->phase = MONGOOSE_WIFI_BENCH_CANCELLED;
        s_wifi_bench_active_connection->is_closing = 1U;
    }
    (void)mg_tcpip_debug_set_advertised_window(44000U);
    (void)mg_tcpip_debug_set_rx_budget(4U);
}
#endif
static void mongoose_http_reply_binary(struct mg_connection *connection,
                                       int status, const char *content_type,
                                       const char *payload, rt_size_t length,
                                       rt_bool_t close_after,
                                       rt_uint32_t keepalive_max_requests,
                                       rt_bool_t gzip_encoded)
{
    char response_header[512];
    size_t header_length;
    size_t response_offset;
    size_t response_end;
    size_t required_send_size;
    int formatted_length;
    char connection_header[64];
    const char *reason = status == 200 ? "OK" :
                         (status == 404 ? "Not Found" : "Error");

    mongoose_http_connection_header(connection_header,
                                    sizeof(connection_header),
                                    close_after, keepalive_max_requests);

    formatted_length = rt_snprintf(
        response_header, sizeof(response_header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
        "Content-Length: %lu\r\n%sCache-Control: no-store\r\n%s\r\n",
        status, reason,
        content_type == RT_NULL ? "application/octet-stream" : content_type,
        (unsigned long)length,
        gzip_encoded ? "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n" : "",
        connection_header);
    if (formatted_length <= 0 ||
        (size_t)formatted_length >= sizeof(response_header))
    {
        connection->is_closing = 1U;
        connection->is_resp = 1U;
        return;
    }

    header_length = (size_t)formatted_length;
    response_offset = connection->send.len;
    if (response_offset > (size_t)-1 - header_length ||
        (size_t)length > (size_t)-1 - response_offset - header_length)
    {
        connection->is_closing = 1U;
        connection->is_resp = 1U;
        return;
    }
    response_end = response_offset + header_length + (size_t)length;
    required_send_size = response_end;
    if (required_send_size < connection->send.size)
    {
        required_send_size = connection->send.size;
    }

    /* Match Mongoose's mg_send() guard for the IPv4-only HTTP listener. */
    if (connection->mgr->ifp == RT_NULL ||
        connection->mgr->ifp->ip == 0U ||
        connection->mgr->ifp->state != MG_TCPIP_STATE_READY)
    {
        mg_error(connection, "net down");
        connection->is_resp = 1U;
        return;
    }

    /* Reserve and append the complete response atomically. mg_send() appends
     * via mg_iobuf_add(), which resizes to each append's exact length. Sending
     * the header and body separately can therefore shrink a full reservation
     * after the header, then fail while growing the body after a 200 is queued. */
    if (!mg_iobuf_resize(&connection->send, required_send_size))
    {
        mg_error(connection, "HTTP response queue reserve failed");
        connection->is_resp = 1U;
        return;
    }
    memcpy(connection->send.buf + response_offset, response_header,
           header_length);
    if (length != 0U)
    {
        memcpy(connection->send.buf + response_offset + header_length,
               payload, length);
    }
    connection->send.len = response_end;

    connection->is_resp = 0U;
    if (close_after)
    {
        mongoose_http_close_after_response(connection);
    }
}

static void mongoose_http_pdf_stream_release(
    struct mongoose_http_connection_state *state, rt_bool_t completed,
    rt_bool_t timed_out)
{
    if (state == RT_NULL || !state->pdf_stream_active)
    {
        return;
    }
    web_pdf_report_stream_release(state->pdf_stream_job_id);
    state->pdf_stream_active = 0U;
    state->pdf_stream_job_id = 0U;
    if (s_http_stats.pdf_stream_active != 0U)
    {
        s_http_stats.pdf_stream_active--;
    }
    if (completed)
    {
        s_http_stats.pdf_stream_completed++;
    }
    else
    {
        s_http_stats.pdf_stream_aborted++;
    }
    if (timed_out)
    {
        s_http_stats.pdf_stream_timeout++;
    }
}

static void mongoose_http_pdf_stream_abort(
    struct mg_connection *connection,
    struct mongoose_http_connection_state *state, rt_bool_t timed_out)
{
    mongoose_http_pdf_stream_release(state, RT_FALSE, timed_out);
    if (connection != RT_NULL)
    {
        connection->is_resp = 1U;
        connection->is_closing = 1U;
    }
}

static void mongoose_http_pdf_stream_pump(
    struct mg_connection *connection,
    struct mongoose_http_connection_state *state)
{
    const rt_uint8_t *span;
    rt_size_t span_length;
    rt_uint32_t now;
    int result;

    if (connection == RT_NULL || state == RT_NULL ||
        !state->pdf_stream_active || connection->is_closing)
    {
        return;
    }
    now = mongoose_http_now_ms();
    if (mongoose_http_elapsed(now, state->pdf_stream_last_progress_ms,
                              MONGOOSE_HTTP_PDF_STREAM_TIMEOUT_MS))
    {
        mongoose_http_pdf_stream_abort(connection, state, RT_TRUE);
        return;
    }
    if (connection->send.len > MONGOOSE_HTTP_PDF_STREAM_LOW_WATER)
    {
        return;
    }
    if (state->pdf_stream_next_offset >= state->pdf_stream_total)
    {
        if (connection->send.len == 0U)
        {
            mongoose_http_pdf_stream_release(state, RT_TRUE, RT_FALSE);
        }
        return;
    }

    result = web_pdf_report_stream_span(
        state->pdf_stream_job_id, state->pdf_stream_next_offset,
        &span, &span_length);
    if (result != RT_EOK || span == RT_NULL || span_length == 0U ||
        span_length > state->pdf_stream_total -
                          state->pdf_stream_next_offset)
    {
        mongoose_http_pdf_stream_abort(connection, state, RT_FALSE);
        return;
    }
    if (!mg_send(connection, span, span_length))
    {
        mongoose_http_pdf_stream_abort(connection, state, RT_FALSE);
        return;
    }
    state->pdf_stream_next_offset += (rt_uint32_t)span_length;
    s_http_stats.pdf_stream_pumps++;
    if (connection->send.len > s_http_stats.pdf_stream_max_send_bytes)
    {
        s_http_stats.pdf_stream_max_send_bytes =
            (rt_uint32_t)connection->send.len;
    }
}

static void mongoose_http_start_pdf_stream(
    struct mg_connection *connection,
    struct mongoose_http_connection_state *state, const char *path)
{
    web_pdf_report_info_t info;
    const rt_uint8_t *span;
    rt_size_t span_length;
    char response_header[192];
    rt_uint32_t job_id;
    size_t header_length;
    size_t response_length;
    int result;
    int formatted_length;

    if (!mongoose_http_parse_pdf_stream_path(path, &job_id))
    {
        mongoose_http_reply_json(
            connection, 404,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Connection: close\r\n",
            "{\"error\":\"not_found\"}\n", RT_TRUE);
        return;
    }
    if (s_http_stats.pdf_stream_active != 0U)
    {
        s_http_stats.busy_503++;
        mongoose_http_reply_json(
            connection, 503,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Retry-After: 1\r\nConnection: close\r\n",
            "{\"error\":\"pdf_stream_busy\"}\n", RT_TRUE);
        return;
    }
    result = web_pdf_report_stream_acquire(job_id, &info);
    if (result != RT_EOK)
    {
        int status = result == -RT_EBUSY ? 503 :
                     (result == -WEB_SNAPSHOT_ERR_STALE ? 409 : 500);
        const char *error = status == 503 ?
            "{\"error\":\"pdf_report_busy\"}\n" :
            (status == 409 ? "{\"error\":\"report_changed\"}\n" :
                             "{\"error\":\"pdf_report_unavailable\"}\n");

        if (status == 503)
        {
            s_http_stats.busy_503++;
        }
        mongoose_http_reply_json(
            connection, status,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Connection: close\r\n",
            error, RT_TRUE);
        return;
    }
    if (info.pdf_bytes < 5U || info.pdf_bytes > 1024U * 1024U)
    {
        web_pdf_report_stream_release(job_id);
        mongoose_http_reply_json(
            connection, 500,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Connection: close\r\n",
            "{\"error\":\"pdf_length_invalid\"}\n", RT_TRUE);
        return;
    }
    result = web_pdf_report_stream_span(job_id, 0U, &span, &span_length);
    if (result != RT_EOK || span == RT_NULL || span_length == 0U)
    {
        web_pdf_report_stream_release(job_id);
        mongoose_http_reply_json(
            connection, 503,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Retry-After: 1\r\nConnection: close\r\n",
            "{\"error\":\"pdf_stream_unavailable\"}\n", RT_TRUE);
        return;
    }
    formatted_length = rt_snprintf(
        response_header, sizeof(response_header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/pdf\r\n"
        "Content-Length: %lu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        (unsigned long)info.pdf_bytes);
    if (formatted_length <= 0 ||
        (size_t)formatted_length >= sizeof(response_header))
    {
        web_pdf_report_stream_release(job_id);
        connection->is_closing = 1U;
        connection->is_resp = 1U;
        return;
    }
    header_length = (size_t)formatted_length;
    response_length = header_length + span_length;
    if (response_length < header_length ||
        !mg_iobuf_resize(&connection->send, response_length))
    {
        web_pdf_report_stream_release(job_id);
        s_http_stats.busy_503++;
        mongoose_http_reply_json(
            connection, 503,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Retry-After: 1\r\nConnection: close\r\n",
            "{\"error\":\"pdf_send_queue_busy\"}\n", RT_TRUE);
        return;
    }
    memcpy(connection->send.buf, response_header, header_length);
    memcpy(connection->send.buf + header_length, span, span_length);
    connection->send.len = response_length;
    state->pdf_stream_active = 1U;
    state->pdf_stream_job_id = job_id;
    state->pdf_stream_total = info.pdf_bytes;
    state->pdf_stream_next_offset = (rt_uint32_t)span_length;
    state->pdf_stream_last_progress_ms = mongoose_http_now_ms();
    s_http_stats.pdf_stream_active++;
    s_http_stats.pdf_stream_pumps++;
    if (connection->send.len > s_http_stats.pdf_stream_max_send_bytes)
    {
        s_http_stats.pdf_stream_max_send_bytes =
            (rt_uint32_t)connection->send.len;
    }
    mongoose_http_close_after_response(connection);
}

static void mongoose_http_event(struct mg_connection *connection, int event,
                                void *event_data)
{
    struct mongoose_http_connection_state *state;
    struct mg_http_message *message;
    const char *content_type = RT_NULL;
    char headers[192];
    char connection_header[64];
    char path[160];
    char *query;
    int length;
    rt_uint32_t now;
    rt_uint32_t handler_start;
    rt_uint32_t keepalive_max_requests;
    rt_bool_t close_after;
    rt_bool_t is_pdf_asset_request;
    rt_bool_t is_pdf_stream_request;
    rt_bool_t pdf_report_final_chunk;
    rt_bool_t gzip_encoded = RT_FALSE;
    enum mongoose_http_evidence_route evidence_route =
        MONGOOSE_HTTP_EVIDENCE_NONE;

    if (event == MG_EV_ACCEPT)
    {
        if (connection == RT_NULL || !connection->is_accepted)
        {
            return;
        }
        state = (struct mongoose_http_connection_state *)connection->data;
        rt_memset(state, 0, sizeof(*state));
        state->magic = MONGOOSE_HTTP_STATE_MAGIC;
        state->accepted_ms = mongoose_http_now_ms();
        state->last_activity_ms = state->accepted_ms;
        s_http_stats.accepted++;

        if (s_http_stats.active >=
            MONGOOSE_HTTP_MAX_CONNECTIONS +
            MONGOOSE_HTTP_OVERFLOW_CONNECTIONS)
        {
            /* Refuse only after the bounded overflow slots are exhausted.
             * Normal burst clients therefore receive a 503 instead of an
             * unbounded number of half-open heap allocations. */
            state->admission = MONGOOSE_HTTP_ADMISSION_REJECTED;
            s_http_stats.rejected++;
            connection->is_closing = 1U;
            return;
        }

        state->counted = 1U;
        s_http_stats.active++;
        if (s_http_stats.active > s_http_stats.peak)
        {
            s_http_stats.peak = s_http_stats.active;
        }
        if (s_http_stats.service_active < MONGOOSE_HTTP_MAX_CONNECTIONS)
        {
            state->admission = MONGOOSE_HTTP_ADMISSION_SERVICE;
            s_http_stats.service_active++;
        }
        else
        {
            state->admission = MONGOOSE_HTTP_ADMISSION_OVERFLOW;
            s_http_stats.overflow_active++;
        }
        return;
    }

    state = mongoose_http_state(connection);
    if (event == MG_EV_CLOSE)
    {
        if (state != RT_NULL)
        {
            if (state->pdf_stream_active)
            {
                mongoose_http_pdf_stream_release(state, RT_FALSE, RT_FALSE);
            }
            mongoose_http_pdf_response_release(state);
            if (state->counted)
            {
                if (s_http_stats.active != 0U)
                {
                    s_http_stats.active--;
                }
                if (state->admission == MONGOOSE_HTTP_ADMISSION_SERVICE &&
                    s_http_stats.service_active != 0U)
                {
                    s_http_stats.service_active--;
                }
                else if (state->admission ==
                         MONGOOSE_HTTP_ADMISSION_OVERFLOW &&
                         s_http_stats.overflow_active != 0U)
                {
                    s_http_stats.overflow_active--;
                }
            }
            s_http_stats.closed++;
            state->magic = 0U;
        }
        return;
    }
    if (state == RT_NULL)
    {
        return;
    }

    if (state->pdf_stream_active &&
        (event == MG_EV_READ || event == MG_EV_WRITE || event == MG_EV_POLL))
    {
        if (event == MG_EV_WRITE)
        {
            state->pdf_stream_last_progress_ms = mongoose_http_now_ms();
        }
        if (event == MG_EV_WRITE || event == MG_EV_POLL)
        {
            mongoose_http_pdf_stream_pump(connection, state);
        }
        return;
    }

    if (event == MG_EV_READ || event == MG_EV_WRITE)
    {
        if (event == MG_EV_WRITE && connection->send.len == 0U)
        {
            mongoose_http_pdf_response_release(state);
        }
        state->last_activity_ms = mongoose_http_now_ms();
        return;
    }
    if (event == MG_EV_ERROR)
    {
        s_http_stats.errors++;
        if (state->pdf_stream_active)
        {
            mongoose_http_pdf_stream_abort(connection, state, RT_FALSE);
        }
        return;
    }
    if (event == MG_EV_POLL)
    {
        if (connection->send.len == 0U)
        {
            mongoose_http_pdf_response_release(state);
        }
        if (!connection->is_closing && !connection->is_draining)
        {
            now = event_data == RT_NULL ? mongoose_http_now_ms() :
                  (rt_uint32_t)(*(const rt_uint64_t *)event_data);
            if (mongoose_http_elapsed(
                    now, state->last_activity_ms,
                    state->admission == MONGOOSE_HTTP_ADMISSION_OVERFLOW ?
                    MONGOOSE_HTTP_OVERFLOW_TIMEOUT_MS :
                    MONGOOSE_HTTP_IDLE_TIMEOUT_MS))
            {
                s_http_stats.idle_closed++;
                mongoose_http_schedule_close(connection);
            }
        }
        return;
    }
    if (event != MG_EV_HTTP_MSG || event_data == RT_NULL ||
        connection->is_closing)
    {
        return;
    }

    message = (struct mg_http_message *)event_data;
    state->last_activity_ms = mongoose_http_now_ms();
    evidence_route = mongoose_http_classify_evidence_uri(&message->uri);
    if (state->admission == MONGOOSE_HTTP_ADMISSION_REJECTED)
    {
        return;
    }
    if (connection->send.len != 0U)
    {
        /* A pipelined request arrived before the previous response drained.
         * Preserve response ordering and release the connection instead of
         * appending another potentially large response. */
        s_http_stats.pipeline_closed++;
        mongoose_http_schedule_close(connection);
        return;
    }
    mongoose_http_pdf_response_release(state);

    s_http_stats.requests++;
    if (state->admission == MONGOOSE_HTTP_ADMISSION_OVERFLOW)
    {
        s_http_stats.overload_503++;
        mongoose_http_record_evidence_overload_503(evidence_route);
        mongoose_http_reply_json(
            connection, 503,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Retry-After: 1\r\nConnection: close\r\n",
            "{\"error\":\"server_busy\"}\n", RT_TRUE);
        return;
    }
    if (mg_strcasecmp(message->method, mg_str("GET")) != 0)
    {
        mongoose_http_reply_json(
            connection, 405,
            "Allow: GET\r\nContent-Type: application/json; charset=utf-8\r\n"
            "Connection: close\r\n",
            "{\"error\":\"method_not_allowed\"}\n", RT_TRUE);
        return;
    }
    if (message->uri.len == 0U || message->uri.len >= sizeof(path))
    {
        mongoose_http_reply_json(
            connection, 414,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Connection: close\r\n",
            "{\"error\":\"uri_too_long\"}\n", RT_TRUE);
        return;
    }
    rt_memcpy(path, message->uri.buf, message->uri.len);
    path[message->uri.len] = '\0';
    query = strchr(path, '?');
    if (query != RT_NULL)
    {
        *query = '\0';
    }
    is_pdf_stream_request =
        rt_strncmp(path, "/api/report/stream/",
                   sizeof("/api/report/stream/") - 1U) == 0 ?
        RT_TRUE : RT_FALSE;

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    /* Arm the temporary raw TCP benchmark explicitly.  The listener is only
     * present in the Debug performance profile and auto-closes after 180 s. */
    if (rt_strcmp(path, "/api/debug/wifi-bench/start") == 0)
    {
        rt_uint32_t send_high_water;
        rt_uint16_t rx_window_bytes;
        rt_uint8_t rx_budget_frames;
        char response[192];

        if (!mongoose_wifi_bench_parse_parameters(
                message, &send_high_water, &rx_window_bytes,
                &rx_budget_frames))
        {
            mongoose_http_reply_json(
                connection, 400,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Cache-Control: no-store\r\nConnection: close\r\n",
                "{\"error\":\"invalid_benchmark_parameters\","
                "\"allowed_high_water_kib\":[4,8,16],"
                "\"allowed_rx_window_bytes\":[44000,61440],"
                "\"allowed_rx_budget_frames\":[4,8,16]}\n", RT_TRUE);
            return;
        }
        if (s_wifi_bench_listener != RT_NULL || s_wifi_bench_active ||
            s_wifi_bench_active_connection != RT_NULL)
        {
            mongoose_http_reply_json(
                connection, 409,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Cache-Control: no-store\r\nConnection: close\r\n",
                "{\"error\":\"wifi_bench_already_armed\"}\n", RT_TRUE);
        }
        else if (!mongoose_wifi_bench_start(send_high_water,
                                             rx_window_bytes,
                                             rx_budget_frames))
        {
            mongoose_http_reply_json(
                connection, 503,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Cache-Control: no-store\r\nConnection: close\r\n",
                "{\"error\":\"wifi_bench_listen_failed\"}\n", RT_TRUE);
        }
        else
        {
            (void)rt_snprintf(
                response, sizeof(response),
                "{\"state\":\"listening\",\"port\":5201,"
                "\"max_bytes\":8388608,\"expires_seconds\":180,"
                "\"run_id\":%lu,\"high_water_bytes\":%lu,"
                "\"rx_window_bytes\":%u,"
                "\"rx_budget_frames\":%u}\n",
                (unsigned long)s_wifi_bench_run_id,
                (unsigned long)send_high_water,
                (unsigned int)rx_window_bytes,
                (unsigned int)rx_budget_frames);
            mongoose_http_reply_json(
                connection, 200,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Cache-Control: no-store\r\nConnection: close\r\n",
                response, RT_TRUE);
        }
        return;
    }
    if (rt_strcmp(path, "/api/debug/wifi-bench/stop") == 0)
    {
        mongoose_wifi_bench_stop();
        mongoose_http_reply_json(
            connection, 200,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n",
            "{\"state\":\"stopped\"}\n", RT_TRUE);
        return;
    }
    if (rt_strcmp(path, "/api/debug/wifi-bench/stats") == 0)
    {
        char response[512];

        if (s_wifi_bench_active ||
            s_wifi_bench_active_connection != RT_NULL)
        {
            mongoose_http_reply_json(
                connection, 409,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Cache-Control: no-store\r\nConnection: close\r\n",
                "{\"state\":\"running\"}\n", RT_TRUE);
            return;
        }
        if (!s_wifi_bench_last_stats.valid)
        {
            mongoose_http_reply_json(
                connection, 404,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Cache-Control: no-store\r\nConnection: close\r\n",
                "{\"state\":\"no_sample\"}\n", RT_TRUE);
            return;
        }
        (void)rt_snprintf(
            response, sizeof(response),
            "{\"state\":\"%s\",\"completed\":%s,"
            "\"run_id\":%lu,\"mode\":\"%s\","
            "\"high_water_bytes\":%lu,\"expected_bytes\":%lu,"
            "\"payload_bytes\":%lu,\"elapsed_ms\":%lu,"
            "\"pump_calls\":%lu,\"empty_pump_calls\":%lu,"
            "\"append_calls\":%lu,\"append_bytes\":%lu,"
            "\"send_len_min\":%lu,\"send_len_max\":%lu,"
            "\"rx_window_bytes\":%u,"
            "\"rx_budget_frames\":%u,"
            "\"rx_budget_exhausted_polls\":%lu,"
            "\"rx_queue_drops\":%lu}\n",
            s_wifi_bench_last_stats.completed ? "complete" : "incomplete",
            s_wifi_bench_last_stats.completed ? "true" : "false",
            (unsigned long)s_wifi_bench_last_stats.run_id,
            s_wifi_bench_last_stats.mode == (rt_uint8_t)'D' ? "D" :
                s_wifi_bench_last_stats.mode == (rt_uint8_t)'U' ? "U" : "none",
            (unsigned long)s_wifi_bench_last_stats.send_high_water,
            (unsigned long)s_wifi_bench_last_stats.expected_bytes,
            (unsigned long)s_wifi_bench_last_stats.payload_bytes,
            (unsigned long)s_wifi_bench_last_stats.elapsed_ms,
            (unsigned long)s_wifi_bench_last_stats.pump_calls,
            (unsigned long)s_wifi_bench_last_stats.empty_pump_calls,
            (unsigned long)s_wifi_bench_last_stats.append_calls,
            (unsigned long)s_wifi_bench_last_stats.append_bytes,
            (unsigned long)s_wifi_bench_last_stats.send_len_min,
            (unsigned long)s_wifi_bench_last_stats.send_len_max,
            (unsigned int)s_wifi_bench_last_stats.rx_window_bytes,
            (unsigned int)s_wifi_bench_last_stats.rx_budget_frames,
            (unsigned long)s_wifi_bench_last_stats.rx_budget_exhausted_polls,
            (unsigned long)s_wifi_bench_last_stats.rx_queue_drops);
        mongoose_http_reply_json(
            connection, 200,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n",
            response, RT_TRUE);
        return;
    }
#endif
    is_pdf_asset_request =
        rt_strncmp(path, "/api/report/",
                   sizeof("/api/report/") - 1U) == 0 ?
        RT_TRUE : RT_FALSE;
    if (is_pdf_asset_request)
    {
        if (state->pdf_request_count >=
            MONGOOSE_HTTP_MAX_PDF_REQUESTS_PER_CONNECTION)
        {
            s_http_stats.request_limit++;
            mongoose_http_reply_json(
                connection, 429,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Retry-After: 1\r\nConnection: close\r\n",
                "{\"error\":\"pdf_connection_request_limit\"}\n",
                RT_TRUE);
            return;
        }
        state->pdf_request_count++;
        state->pdf_asset = 1U;

        if (!is_pdf_stream_request && s_http_pdf_responses_active >=
            MONGOOSE_HTTP_MAX_PDF_RESPONSES_IN_FLIGHT)
        {
            s_http_stats.busy_503++;
            mongoose_http_reply_json(
                connection, 503,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Retry-After: 1\r\nConnection: close\r\n",
                "{\"error\":\"pdf_asset_busy\"}\n", RT_TRUE);
            return;
        }
        if (!is_pdf_stream_request)
        {
            s_http_pdf_responses_active++;
            state->pdf_response_pending = 1U;
        }
    }
    else
    {
        state->request_count++;
        if (state->request_count > MONGOOSE_HTTP_MAX_REQUESTS_PER_CONNECTION)
        {
            s_http_stats.request_limit++;
            mongoose_http_reply_json(
                connection, 429,
                "Content-Type: application/json; charset=utf-8\r\n"
                "Retry-After: 1\r\nConnection: close\r\n",
                "{\"error\":\"connection_request_limit\"}\n", RT_TRUE);
            return;
        }
    }

    if (is_pdf_stream_request)
    {
        mongoose_http_start_pdf_stream(connection, state, path);
        return;
    }

    handler_start = usb_perf_now();
    length = web_server_render(s_http_payload, sizeof(s_http_payload), path,
                               &content_type,
                               mongoose_http_accepts_gzip(message),
                               &gzip_encoded);
    if (length < 0)
    {
        int status = length == -WEB_SNAPSHOT_ERR_STALE ? 409 :
                     (length == -WEB_SNAPSHOT_ERR_RANGE ? 416 :
                      (length == -RT_EBUSY ? 503 :
                       (length == -RT_ENOSPC ? 500 : 404)));
        const char *error = status == 409 ?
                            "{\"error\":\"snapshot_changed\"}\n" :
                            (status == 416 ?
                             "{\"error\":\"range_not_satisfiable\"}\n" :
                             (status == 503 ?
                              "{\"error\":\"temporarily_busy\"}\n" :
                              (status == 500 ?
                               "{\"error\":\"response_too_large\"}\n" :
                               "{\"error\":\"not_found\"}\n")));
        if (status == 503)
        {
            s_http_stats.busy_503++;
            if (length == -RT_EBUSY)
            {
                mongoose_http_record_evidence_busy_503(evidence_route);
            }
        }
        else if (length == -WEB_SNAPSHOT_ERR_STALE)
        {
            mongoose_http_record_evidence_stale_409(evidence_route);
        }
        mongoose_http_reply_json(
            connection, status,
            "Content-Type: application/json; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n", error, RT_TRUE);
        return;
    }

    pdf_report_final_chunk = is_pdf_asset_request ?
        mongoose_http_is_pdf_report_final_chunk(path, length) : RT_FALSE;
    if (is_pdf_asset_request)
    {
        keepalive_max_requests =
            MONGOOSE_HTTP_PDF_KEEPALIVE_MAX_REQUESTS;
        close_after = state->pdf_request_count >=
                          MONGOOSE_HTTP_MAX_PDF_REQUESTS_PER_CONNECTION ||
                      pdf_report_final_chunk ||
                      (rt_size_t)length > WEB_PDF_REPORT_CHUNK_SIZE ||
                      mongoose_http_request_wants_close(message);
    }
    else
    {
        keepalive_max_requests = MONGOOSE_HTTP_MAX_REQUESTS_PER_CONNECTION;
        close_after = state->request_count >=
                          MONGOOSE_HTTP_MAX_REQUESTS_PER_CONNECTION ||
                      /* Complete entry module and stylesheet responses before
                       * later requests reuse bounded keep-alive connections. */
                      mongoose_http_is_critical_static_asset(path) ||
                      state->pdf_asset ||
                      (rt_size_t)length >
                          MONGOOSE_HTTP_KEEPALIVE_MAX_RESPONSE ||
                      mongoose_http_request_wants_close(message);
    }
    if (gzip_encoded || (content_type != RT_NULL &&
        rt_strcmp(content_type, "application/octet-stream") == 0))
    {
        mongoose_http_reply_binary(connection, 200, content_type,
                                   s_http_payload, (rt_size_t)length,
                                   close_after, keepalive_max_requests,
                                   gzip_encoded);
    }
    else
    {
        mongoose_http_connection_header(connection_header,
                                        sizeof(connection_header),
                                        close_after,
                                        keepalive_max_requests);
        rt_snprintf(
            headers, sizeof(headers),
            "Content-Type: %s\r\nCache-Control: no-store\r\n%s",
            content_type != RT_NULL ? content_type :
            "application/json; charset=utf-8",
            connection_header);
        mg_http_reply(connection, 200, headers, "%.*s", length,
                      s_http_payload);
        if (close_after)
        {
            mongoose_http_close_after_response(connection);
        }
    }
    usb_perf_record(USB_PERF_HTTP_HANDLER, handler_start, usb_perf_now());
}

static void mongoose_web_poll(void)
{
    if (!s_manager_ready || s_mgr.ifp == RT_NULL)
    {
        s_web_online = RT_FALSE;
        return;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (s_wifi_bench_listener != RT_NULL &&
        mongoose_http_elapsed(mongoose_http_now_ms(),
                              s_wifi_bench_listener_opened_ms,
                              MONGOOSE_WIFI_BENCH_LISTENER_TTL_MS))
    {
        mongoose_wifi_bench_stop();
    }
#endif
    if (s_web_requested && s_http_listener == RT_NULL &&
        s_mgr.ifp->state == MG_TCPIP_STATE_READY)
    {
        s_http_listener = mg_http_listen(&s_mgr, "http://0.0.0.0:80",
                                         mongoose_http_event, RT_NULL);
        if (s_http_listener == RT_NULL)
        {
            s_web_listen_failures++;
        }
    }
    s_web_online = s_http_listener != RT_NULL &&
                   s_mgr.ifp->state == MG_TCPIP_STATE_READY;
}

int web_server_start(void)
{
    rt_base_t level = rt_hw_interrupt_disable();

    s_web_requested = RT_TRUE;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}

int web_server_is_online(void)
{
    return s_web_online ? 1 : 0;
}



static void mongoose_service_entry(void *parameter)
{
    (void)parameter;
    rt_uint32_t manager_poll_index;
    rt_memset(&s_http_stats, 0, sizeof(s_http_stats));
    s_http_pdf_responses_active = 0U;
    wifi_set_ssid("");
    s_mongoose_net_ready=RT_FALSE;
    s_manager_ready=RT_FALSE;
    /* Keep boot/network traffic off the UART trace; detailed output is opt-in
     * through the explicit FinSH status commands. */
    mg_log_set(MG_LL_NONE);
    mg_log_set_fn(mongoose_log_putchar, RT_NULL);
    mg_mgr_init(&s_mgr);
    rt_memset(&s_mongoose_if, 0, sizeof(s_mongoose_if));
    s_ifp = &s_mongoose_if;
    s_mongoose_if.driver=&s_mongoose_whd_driver;
    s_mongoose_if.driver_data=&s_whd;
    s_mongoose_if.fn=mongoose_wifi_event;
    s_mongoose_if.recv_queue.size=MONGOOSE_RX_QUEUE_SIZE;
    s_mongoose_if.enable_req_dns=true;

    if (wifi_manager_service_init() != RT_EOK) {
        wifi_set_status(WIFI_STATE_FAILED, "网络管理初始化失败");
        startup_mark(BOOT_RADIO_FAILED);
        return;
    }

    while (1)
    {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        rt_uint32_t owner_start = usb_perf_now();
        rt_uint32_t manager_start = 0U;
        rt_uint32_t manager_end = 0U;
        rt_uint32_t web_start;
        rt_uint32_t web_end;
        rt_uint32_t owner_end;
        bool manager_polled = false;
#endif
        wifi_manager_poll();
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        if (mongoose_service_is_ready())
        {
            manager_start = usb_perf_now();
            for (manager_poll_index = 0U; manager_poll_index < MONGOOSE_MANAGER_POLLS_PER_TICK; ++manager_poll_index)
            {
                mg_mgr_poll(&s_mgr,0);
            }
            manager_end = usb_perf_now();
            manager_polled = true;
        }
        else
#else
        if (mongoose_service_is_ready())
#endif
        {
#if !defined(WORK_EVIDENCE_DIAGNOSTICS) || !WORK_EVIDENCE_DIAGNOSTICS
            for (manager_poll_index = 0U; manager_poll_index < MONGOOSE_MANAGER_POLLS_PER_TICK; ++manager_poll_index)
            {
                mg_mgr_poll(&s_mgr,0);
            }
#endif
        }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        web_start = usb_perf_now();
#endif
        mongoose_web_poll();
        watchdog_supervisor_component_progress(
            WATCHDOG_COMPONENT_MONGOOSE, WATCHDOG_COMPONENT_STAGE_ACTIVE,
            ++s_watchdog_manager_progress);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        web_end = usb_perf_now();
        owner_end = usb_perf_now();
        if (manager_polled)
        {
            usb_perf_record(USB_PERF_MG_MGR_POLL, manager_start, manager_end);
        }
        usb_perf_record(USB_PERF_MONGOOSE_WEB_POLL, web_start, web_end);
        usb_perf_record(USB_PERF_MONGOOSE_OWNER_LOOP, owner_start, owner_end);
#endif
        rt_thread_mdelay(1U);
    }
}

static int mongoose_service_init(void)
{
#if MONGOOSE_DIAGNOSTIC_DISABLE_SERVICE
    return RT_EOK;
#else
    rt_thread_t thread;
    if (web_pdf_report_init() != RT_EOK)
    {
        rt_kprintf("[PDF] libHaru heap/worker initialization failed\r\n");
        return -RT_ENOMEM;
    }
#ifdef WIFI_AUTO_START_WEB
    s_web_requested = RT_TRUE;
#endif

    thread = rt_thread_create("mongoose", mongoose_service_entry, RT_NULL,
                              MONGOOSE_SERVICE_STACK_SIZE,
                              MONGOOSE_SERVICE_PRIORITY,
                              MONGOOSE_SERVICE_TIMESLICE);
    if (thread == RT_NULL)
    {
        wifi_set_status(WIFI_STATE_FAILED, "WHD 服务线程创建失败");
        return -RT_ENOMEM;
    }
    {
        int result = rt_thread_startup(thread);
        if (result == RT_EOK)
        {
            watchdog_supervisor_component_expect(
                WATCHDOG_COMPONENT_MONGOOSE, 1);
        }
        return result;
    }
#endif
}
INIT_APP_EXPORT(mongoose_service_init);

#ifdef RT_USING_FINSH
static int mongoose_wifi_msh(int argc, char **argv)
{
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (argc > 1 && rt_strcmp(argv[1], "radio") == 0)
    {
        uint32_t rate_500kbps = 0U;
        uint32_t channel = 0U;
        int32_t rssi_dbm = 0;
        whd_result_t ready_result;
        whd_result_t rate_result;
        whd_result_t rssi_result;
        whd_result_t channel_result;

        if (!s_whd.ready || s_whd.interface == RT_NULL)
        {
            rt_kprintf("wifi-radio: interface unavailable\n");
            return -RT_ERROR;
        }
        ready_result = whd_wifi_is_ready_to_transceive(s_whd.interface);
        if (ready_result != WHD_SUCCESS)
        {
            rt_kprintf("wifi-radio: link not ready result=0x%08lx\n",
                       (unsigned long)ready_result);
            return -RT_ERROR;
        }

        rate_result = whd_wifi_get_ioctl_value(s_whd.interface, WLC_GET_RATE,
                                               &rate_500kbps);
        rssi_result = whd_wifi_get_rssi(s_whd.interface, &rssi_dbm);
        channel_result = whd_wifi_get_channel(s_whd.interface, &channel);
        rt_kprintf("wifi-radio: rate_result=0x%08lx rate_500kbps=%lu rate_mbps=%lu.%u "
                   "rssi_result=0x%08lx rssi_dbm=%ld "
                   "channel_result=0x%08lx channel=%lu\n",
                   (unsigned long)rate_result,
                   (unsigned long)rate_500kbps,
                   (unsigned long)(rate_500kbps / 2U),
                   (unsigned int)((rate_500kbps & 1U) != 0U ? 5U : 0U),
                   (unsigned long)rssi_result,
                   (long)rssi_dbm,
                   (unsigned long)channel_result,
                   (unsigned long)channel);
        return rate_result == WHD_SUCCESS && rssi_result == WHD_SUCCESS &&
               channel_result == WHD_SUCCESS ? RT_EOK : -RT_ERROR;
    }
#endif
    if (argc > 1 && rt_strcmp(argv[1], "stats") == 0)
    {
        if (s_whd.driver == RT_NULL)
        {
            rt_kprintf("wifi: WHD not initialized\n");
            return -RT_ERROR;
        }
        (void)whd_print_stats(s_whd.driver, WHD_FALSE);
        return RT_EOK;
    }
    if (argc > 1 && rt_strcmp(argv[1], "scan") == 0)
    {
        return wifi_request_scan();
    }
    if(argc==2 && !rt_strcmp(argv[1],"disconnect"))return wifi_request_disconnect(NULL);
    if(argc==2 && !rt_strcmp(argv[1],"cancel"))return wifi_request_cancel(NULL);
    if(argc==2 && !rt_strcmp(argv[1],"retry_save"))return wifi_request_retry_save(NULL);
    if(argc==2 && !rt_strcmp(argv[1],"temporary"))return wifi_request_use_temporary(NULL);
    if(argc==3 && !rt_strcmp(argv[1],"auto")) {
        if(rt_strcmp(argv[2],"on") && rt_strcmp(argv[2],"off"))return -RT_EINVAL;
        return wifi_request_set_autoconnect(!rt_strcmp(argv[2],"on"),NULL);
    }
    if(argc==3 && !rt_strcmp(argv[1],"connect")) {
        char *end; unsigned long id=strtoul(argv[2],&end,10);
        if(*end||!id||id>UINT32_MAX)return -RT_EINVAL;
        return wifi_request_connect_saved((uint32_t)id,NULL);
    }
    if(argc==4 && !rt_strcmp(argv[1],"forget") && !rt_strcmp(argv[3],"confirm")) {
        char *end;unsigned long id=strtoul(argv[2],&end,10);
        if(*end||!id||id>UINT32_MAX)return -RT_EINVAL;
        return wifi_request_forget((uint32_t)id,NULL);
    }
    if(argc==3 && !rt_strcmp(argv[1],"clear") && !rt_strcmp(argv[2],"confirm"))
        return wifi_request_forget_all(NULL);
    if(argc==2 && !rt_strcmp(argv[1],"list")) {
        static wifi_snapshot_t view;
        if(wifi_get_snapshot(&view)!=RT_EOK)return -RT_EBUSY;
        rt_kprintf("wifi: phase=%s storage=%s saved=%u auto=%u\n",
            wifi_phase_name(view.network.phase),wifi_storage_name(view.network.storage),
            view.network.profile_count,view.network.auto_connect?1:0);
        for(unsigned i=0;i<view.network.profile_count;i++)
            rt_kprintf("wifi: id=%lu security=%u auto=%u\n",
                (unsigned long)view.network.profiles[i].id,
                view.network.profiles[i].security,view.network.profiles[i].auto_connect?1:0);
        return RT_EOK;
    }
    if (argc > 3 && rt_strcmp(argv[1], "connect") == 0)
    {
        rt_kprintf("wifi: use LCD to enter passwords; connect <saved-id> only\n");
        return -RT_EINVAL;
    }
    if (argc == 1 || rt_strcmp(argv[1], "status") == 0)
    {
        whd_stm32_sdio_stats_t sdio_stats;
        whd_result_t link_result = (s_whd.ready &&
                                    s_whd.interface != RT_NULL) ?
                                   whd_wifi_is_ready_to_transceive(
                                       s_whd.interface) :
                                   WHD_INTERFACE_NOT_UP;
        rt_kprintf("wifi: whd=%d link=%d ready_tx=0x%08lx state=%u\n",
                   s_manager_ready ? 1 : 0,
                   s_whd.link_up ? 1 : 0,
                   (unsigned long)link_result,
                   s_ifp == RT_NULL ? 0U : s_ifp->state);
        rt_kprintf("wifi: ip=%s status=%s\n",g_wifi.ip,g_wifi.status_msg);
        static wifi_snapshot_t public_view;
        if(wifi_get_snapshot(&public_view)==RT_EOK) {
            rt_kprintf("wifi: phase=%s reason=%s storage=%s\n",
                wifi_phase_name(public_view.network.phase),
                wifi_reason_name(public_view.network.reason),
                wifi_storage_name(public_view.network.storage));
            rt_kprintf("wifi: saved=%u known=%u profile=%lu op=%lu revision=%lu\n",
                public_view.network.saved_profile_count,public_view.network.profile_count,
                (unsigned long)public_view.network.active_profile_id,
                (unsigned long)public_view.network.operation_id,
                (unsigned long)public_view.network.config_revision);
            rt_kprintf("wifi: auto=%u suspended=%u temporary=%u retry_ms=%lu\n",
                public_view.network.auto_connect,public_view.network.auto_suspended,
                public_view.network.temporary,(unsigned long)public_view.network.retry_in_ms);
            rt_kprintf("wifi: connection_op=%lu saved_op=%lu\n",
                (unsigned long)public_view.network.connection_operation_id,(unsigned long)public_view.network.saved_operation_id);
            rt_kprintf("wifi: config_op=%lu config_saved_op=%lu pending=%u\n",
                (unsigned long)public_view.network.config_operation_id,(unsigned long)public_view.network.config_saved_operation_id,
                public_view.network.pending_changes);
        }
        rt_kprintf("wifi: net_rx=%lu net_tx=%lu net_drop=%lu net_err=%lu\n",
                   s_ifp == RT_NULL ? 0UL : (unsigned long)s_ifp->nrecv,
                   s_ifp == RT_NULL ? 0UL : (unsigned long)s_ifp->nsent,
                   s_ifp == RT_NULL ? 0UL : (unsigned long)s_ifp->ndrop,
                   s_ifp == RT_NULL ? 0UL : (unsigned long)s_ifp->nerr);
        whd_stm32_sdio_get_stats(&sdio_stats);
        wifi_backend_diagnostics_t backend_diag;
        if(wifi_backend_get_diagnostics(&backend_diag)==RT_EOK) {
            rt_kprintf("wifi-driver: scan=%lu raw=0x%08lx outcome=%u\n",
                (unsigned long)backend_diag.scan_counters.attempts,
                (unsigned long)backend_diag.last_scan.raw_whd_result,backend_diag.last_scan.outcome);
            rt_kprintf("wifi-driver: join=%lu raw=0x%08lx leave=%lu raw=0x%08lx\n",
                (unsigned long)backend_diag.join_counters.attempts,(unsigned long)backend_diag.last_join.raw_whd_result,
                (unsigned long)backend_diag.leave_counters.attempts,(unsigned long)backend_diag.last_leave.raw_whd_result);
        }
        rt_kprintf("sdio-recovery: timeouts=%lu aborts=%lu recovered=%lu failed=%lu\n",
            (unsigned long)sdio_stats.timeouts,(unsigned long)sdio_stats.timeout_abort_attempts,
            (unsigned long)sdio_stats.timeout_recoveries,(unsigned long)sdio_stats.timeout_recovery_failures);
        rt_kprintf("sdio: clock=%luHz width=%ubit idma_direct=%lu/%luB "
                   "idma_linked=%lu/%luB pad=%lu err=%lu\n",
                   (unsigned long)sdio_stats.clock_hz,
                   (unsigned int)sdio_stats.bus_width,
                   (unsigned long)sdio_stats.direct_transfers,
                   (unsigned long)sdio_stats.direct_bytes,
                   (unsigned long)sdio_stats.linked_transfers,
                   (unsigned long)sdio_stats.linked_bytes,
                   (unsigned long)sdio_stats.padding_bytes,
                   (unsigned long)sdio_stats.errors);
        if (sdio_stats.errors != 0U)
        {
            rt_kprintf("sdio-last-error: code=0x%08lx sta=0x%08lx "
                       "arg=0x%08lx data=0x%08lx len=%lu dma=%lu "
                       "resp=0x%08lx direct=%u\n",
                       (unsigned long)sdio_stats.last_error_code,
                       (unsigned long)sdio_stats.last_error_status,
                       (unsigned long)sdio_stats.last_error_argument,
                       (unsigned long)sdio_stats.last_error_data,
                       (unsigned long)sdio_stats.last_error_length,
                       (unsigned long)sdio_stats.last_error_transfer_length,
                       (unsigned long)sdio_stats.last_error_response,
                       (unsigned int)sdio_stats.last_error_direct);
        }
        return RT_EOK;
    }
    rt_kprintf("Usage: wifi status|stats|scan|list|connect <saved-id>\n");
    rt_kprintf("wifi disconnect|cancel|auto on/off|forget <id> confirm|clear confirm\n");
    return -RT_ERROR;
}
MSH_CMD_EXPORT_ALIAS(mongoose_wifi_msh, wifi, inspect or control WHD WiFi);

static int mongoose_web_msh(int argc, char **argv)
{
    if (argc == 1 || (argc > 1 && rt_strcmp(argv[1], "start") == 0))
    {
        int result = web_server_start();
        rt_kprintf("web: mongoose start result=%d online=%d\n",
                   result, web_server_is_online());
        return result;
    }
    if (argc > 1 && rt_strcmp(argv[1], "status") == 0)
    {
        struct mongoose_http_stats stats;

        mongoose_http_stats_snapshot(&stats);
        rt_kprintf("web: online=%d requested=%d listener=%p active=%lu/%u "
                   "service=%lu/%u overflow=%lu/%u\n",
                   web_server_is_online(), s_web_requested ? 1 : 0,
                   s_http_listener,
                   (unsigned long)stats.active,
                   (unsigned int)(MONGOOSE_HTTP_MAX_CONNECTIONS +
                                  MONGOOSE_HTTP_OVERFLOW_CONNECTIONS),
                   (unsigned long)stats.service_active,
                   (unsigned int)MONGOOSE_HTTP_MAX_CONNECTIONS,
                   (unsigned long)stats.overflow_active,
                   (unsigned int)MONGOOSE_HTTP_OVERFLOW_CONNECTIONS);
        rt_kprintf("web: peak=%lu accepted=%lu closed=%lu rejected=%lu "
                   "requests=%lu\n",
                   (unsigned long)stats.peak,
                   (unsigned long)stats.accepted,
                   (unsigned long)stats.closed,
                   (unsigned long)stats.rejected,
                   (unsigned long)stats.requests);
        rt_kprintf("web: overload503=%lu busy503=%lu limit=%lu pipeline=%lu "
                   "respclose=%lu\n",
                   (unsigned long)stats.overload_503,
                   (unsigned long)stats.busy_503,
                   (unsigned long)stats.request_limit,
                   (unsigned long)stats.pipeline_closed,
                   (unsigned long)stats.response_closed);
        rt_kprintf("web-evidence: overload503 manifest=%lu page=%lu event=%lu\n",
                   (unsigned long)stats.evidence_manifest_overload_503,
                   (unsigned long)stats.evidence_page_overload_503,
                   (unsigned long)stats.evidence_event_overload_503);
        rt_kprintf("web-evidence: busy503 manifest=%lu page=%lu event=%lu "
                   "stale409 page=%lu event=%lu\n",
                   (unsigned long)stats.evidence_manifest_busy_503,
                   (unsigned long)stats.evidence_page_busy_503,
                   (unsigned long)stats.evidence_event_busy_503,
                   (unsigned long)stats.evidence_page_stale_409,
                   (unsigned long)stats.evidence_event_stale_409);
        rt_kprintf("web: idleclose=%lu errors=%lu rxq=%u\n",
                   (unsigned long)stats.idle_closed,
                   (unsigned long)stats.errors,
                   (unsigned int)MONGOOSE_RX_QUEUE_SIZE);
        rt_kprintf("web-pdf-stream: active=%lu complete=%lu abort=%lu "
                   "timeout=%lu pumps=%lu maxq=%lu\n",
                   (unsigned long)stats.pdf_stream_active,
                   (unsigned long)stats.pdf_stream_completed,
                   (unsigned long)stats.pdf_stream_aborted,
                   (unsigned long)stats.pdf_stream_timeout,
                   (unsigned long)stats.pdf_stream_pumps,
                   (unsigned long)stats.pdf_stream_max_send_bytes);
        return RT_EOK;
    }
    rt_kprintf("Usage: web [start|status]\n");
    return -RT_ERROR;
}
MSH_CMD_EXPORT_ALIAS(mongoose_web_msh, web, start or inspect Mongoose HTTP desk);
#endif
