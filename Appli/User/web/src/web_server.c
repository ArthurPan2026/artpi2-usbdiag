#include "web_server.h"

#include <rtthread.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#ifdef RT_USING_FINSH
#include <finsh.h>
#endif

#include "web_snapshot.h"
#include "web_pdf_report.h"
#include "web_assets.h"
#include "build_identity.h"
#include "wifi_state.h"
#include "wifi_manager_backend.h"
#include "usb_descriptor_store.h"
#include "usbdiag_session.h"
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#include "lv_port_disp.h"
#endif

#define WEB_SERVER_PORT             80
#define WEB_SERVER_STACK_SIZE       4096U
#define WEB_SERVER_WORKER_STACK_SIZE 4096U
#define WEB_SERVER_PRIORITY         13U
#define WEB_SERVER_WORKER_COUNT     1U
#define WEB_SERVER_MAX_ACTIVE_CLIENTS WEB_SERVER_WORKER_COUNT
#define WEB_SERVER_BACKLOG          WEB_SERVER_MAX_ACTIVE_CLIENTS
#define WEB_SERVER_QUEUE_DEPTH      32U
#define WEB_SERVER_REQUEST_SIZE     1024U
/* No current route returns the full immutable evidence JSON in one response:
 * evidence is paged at 1200 bytes.  Keep enough room for the worst-case WiFi
 * scan JSON (32 long SSIDs plus escaping) while allowing one worker per active
 * client. */
#define WEB_SERVER_PAYLOAD_SIZE     24576U
#define WEB_SERVER_EVIDENCE_PAGE_SIZE 4096U
#define WEB_SERVER_FRAMEBUFFER_PAGE_SIZE WEB_SERVER_EVIDENCE_PAGE_SIZE
#define WEB_SERVER_USB_PAGE_SIZE      1200U
#define WEB_EVIDENCE_TOKEN_TEXT_LENGTH 26U
#define WEB_SERVER_CLIENT_TIMEOUT_S  1
#define WEB_SERVER_SEND_TIMEOUT_S    1
#define WEB_SERVER_SEND_TIMEOUT_MS   (WEB_SERVER_SEND_TIMEOUT_S * 1000U)
#define WEB_SERVER_MAX_REQUESTS_PER_CLIENT 1U
#define WEB_SERVER_SEND_PACE_MS      10U
#define WEB_SERVER_CLOSE_GUARD_MS    10U
#define WEB_SERVER_RESPONSE_GAP_MS   25U
#define WEB_SERVER_ACCEPT_BACKOFF_MS 20U

#define WIFI_WEB_RAM_SECTION \
    __attribute__((section(".wifi_web_ram"), aligned(32)))
#define WEB_THREAD_RAM_SECTION \
    __attribute__((section(".web_thread_ram"), aligned(32)))

#if !defined(MONGOOSE_WEB_BACKEND) || !MONGOOSE_WEB_BACKEND
static rt_thread_t s_server_thread;
static volatile rt_bool_t s_server_online;
static rt_align(4) char s_http_payload[WEB_SERVER_WORKER_COUNT]
                                      [WEB_SERVER_PAYLOAD_SIZE]
    WIFI_WEB_RAM_SECTION;
static struct rt_thread s_server_thread_object WEB_THREAD_RAM_SECTION;
static rt_align(4) rt_uint8_t s_server_stack[WEB_SERVER_STACK_SIZE]
    WEB_THREAD_RAM_SECTION;
static struct rt_thread s_worker_thread_objects[WEB_SERVER_WORKER_COUNT]
    WEB_THREAD_RAM_SECTION;
static rt_align(4) rt_uint8_t s_worker_stacks[WEB_SERVER_WORKER_COUNT]
                                             [WEB_SERVER_WORKER_STACK_SIZE]
    WEB_THREAD_RAM_SECTION;
static struct rt_messagequeue s_client_queue;
static rt_align(RT_ALIGN_SIZE) rt_uint8_t s_client_queue_pool[
    RT_MQ_BUF_SIZE(sizeof(int), WEB_SERVER_QUEUE_DEPTH)];
static rt_bool_t s_client_queue_ready;
static struct rt_mutex s_start_lock;
static volatile rt_bool_t s_start_lock_ready;
static struct rt_mutex s_send_lock;
static volatile rt_bool_t s_send_lock_ready;
static rt_bool_t s_workers_started;
#endif

/* The renderer copies scan records here before formatting them.  It is shared
 * only by the single Mongoose event-loop thread and the short critical
 * section below, never by a socket worker. */
static wifi_ap_record_t s_wifi_scan_records[WIFI_SCAN_MAX_APS]
    WIFI_WEB_RAM_SECTION;
/* USB descriptor routes are rendered by the single Mongoose thread.  Take
 * one store snapshot per route, then format it without repeatedly taking the
 * descriptor-store mutex while USB enumeration is running. */
static usb_descriptor_config_info_t s_usb_configs[
    USB_DESCRIPTOR_MAX_CONFIGURATIONS] WIFI_WEB_RAM_SECTION;
static usb_descriptor_interface_info_t s_usb_interfaces[
    USB_DESCRIPTOR_MAX_INTERFACES] WIFI_WEB_RAM_SECTION;
static usb_descriptor_endpoint_info_t s_usb_endpoints[
    USB_DESCRIPTOR_MAX_ENDPOINTS] WIFI_WEB_RAM_SECTION;
static usb_descriptor_string_info_t s_usb_strings[
    USB_DESCRIPTOR_MAX_STRINGS] WIFI_WEB_RAM_SECTION;

#if !defined(MONGOOSE_WEB_BACKEND) || !MONGOOSE_WEB_BACKEND
struct web_worker
{
    rt_thread_t thread;
    rt_uint8_t index;
};

static struct web_worker s_workers[WEB_SERVER_WORKER_COUNT];
#endif

/* These counters are kept in zero-initialized on-chip RAM.  They make a field
 * test distinguish a bounded HTTP overload from a stalled scheduler. */
#if !defined(MONGOOSE_WEB_BACKEND) || !MONGOOSE_WEB_BACKEND
static volatile rt_uint32_t s_web_active_clients;
static volatile rt_uint32_t s_web_peak_clients;
static volatile rt_uint32_t s_web_accepted;
static volatile rt_uint32_t s_web_completed;
static volatile rt_uint32_t s_web_queue_rejected;
static volatile rt_uint32_t s_web_receive_failed;
static volatile rt_uint32_t s_web_send_failed;
static volatile rt_uint32_t s_web_reply_lock_timeout;
static rt_tick_t s_web_last_response_tick;
#endif

struct web_builder
{
    char *cursor;
    rt_size_t remaining;
    rt_bool_t overflow;
};

#if !defined(MONGOOSE_WEB_BACKEND) || !MONGOOSE_WEB_BACKEND
static void web_counter_snapshot(rt_uint32_t *active,
                                 rt_uint32_t *peak,
                                 rt_uint32_t *accepted,
                                 rt_uint32_t *completed,
                                 rt_uint32_t *queue_rejected,
                                 rt_uint32_t *receive_failed,
                                 rt_uint32_t *send_failed,
                                 rt_uint32_t *reply_lock_timeout);
#endif

#if !defined(MONGOOSE_WEB_BACKEND) || !MONGOOSE_WEB_BACKEND
static void web_counter_inc(volatile rt_uint32_t *counter)
{
    rt_base_t level;

    if (counter == RT_NULL)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    (*counter)++;
    rt_hw_interrupt_enable(level);
}
#endif

#if !defined(MONGOOSE_WEB_BACKEND) || !MONGOOSE_WEB_BACKEND
static void web_print_status(void)
{
    rt_uint32_t active;
    rt_uint32_t peak;
    rt_uint32_t accepted;
    rt_uint32_t completed;
    rt_uint32_t queue_rejected;
    rt_uint32_t receive_failed;
    rt_uint32_t send_failed;
    rt_uint32_t reply_lock_timeout;

    web_counter_snapshot(&active, &peak, &accepted, &completed,
                         &queue_rejected, &receive_failed, &send_failed,
                         &reply_lock_timeout);
    rt_kprintf("web: online=%d active=%lu peak=%lu accepted=%lu "
               "completed=%lu queue_rejected=%lu rx_fail=%lu tx_fail=%lu "
               "lock_timeout=%lu\n",
               web_server_is_online(), (unsigned long)active,
               (unsigned long)peak, (unsigned long)accepted,
               (unsigned long)completed, (unsigned long)queue_rejected,
               (unsigned long)receive_failed, (unsigned long)send_failed,
               (unsigned long)reply_lock_timeout);
}
#endif

#if !defined(MONGOOSE_WEB_BACKEND) || !MONGOOSE_WEB_BACKEND
static rt_bool_t web_client_try_enter(void)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (s_web_active_clients >= WEB_SERVER_MAX_ACTIVE_CLIENTS)
    {
        s_web_queue_rejected++;
        rt_hw_interrupt_enable(level);
        return RT_FALSE;
    }
    s_web_accepted++;
    s_web_active_clients++;
    if (s_web_active_clients > s_web_peak_clients)
    {
        s_web_peak_clients = s_web_active_clients;
    }
    rt_hw_interrupt_enable(level);
    return RT_TRUE;
}

static void web_client_leave(void)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (s_web_active_clients != 0U)
    {
        s_web_active_clients--;
    }
    s_web_completed++;
    rt_hw_interrupt_enable(level);
}
#endif

#if !defined(MONGOOSE_WEB_BACKEND) || !MONGOOSE_WEB_BACKEND
static void web_wait_response_slot(void)
{
    rt_tick_t now = rt_tick_get();
    rt_tick_t gap = rt_tick_from_millisecond(WEB_SERVER_RESPONSE_GAP_MS);

    if (s_web_last_response_tick != 0U)
    {
        rt_int32_t elapsed = (rt_int32_t)(now - s_web_last_response_tick);

        if (elapsed >= 0 && (rt_tick_t)elapsed < gap)
        {
            rt_thread_mdelay(gap - (rt_tick_t)elapsed);
        }
    }
    s_web_last_response_tick = rt_tick_get();
}
#endif

#if !defined(MONGOOSE_WEB_BACKEND) || !MONGOOSE_WEB_BACKEND
static void web_counter_snapshot(rt_uint32_t *active,
                                 rt_uint32_t *peak,
                                 rt_uint32_t *accepted,
                                 rt_uint32_t *completed,
                                 rt_uint32_t *queue_rejected,
                                 rt_uint32_t *receive_failed,
                                 rt_uint32_t *send_failed,
                                 rt_uint32_t *reply_lock_timeout)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (active != RT_NULL) *active = s_web_active_clients;
    if (peak != RT_NULL) *peak = s_web_peak_clients;
    if (accepted != RT_NULL) *accepted = s_web_accepted;
    if (completed != RT_NULL) *completed = s_web_completed;
    if (queue_rejected != RT_NULL) *queue_rejected = s_web_queue_rejected;
    if (receive_failed != RT_NULL) *receive_failed = s_web_receive_failed;
    if (send_failed != RT_NULL) *send_failed = s_web_send_failed;
    if (reply_lock_timeout != RT_NULL)
    {
        *reply_lock_timeout = s_web_reply_lock_timeout;
    }
    rt_hw_interrupt_enable(level);
}

#endif

static void web_append(struct web_builder *builder, const char *format, ...)
{
    va_list arguments;
    int written;

    if (builder == RT_NULL || builder->overflow ||
        builder->cursor == RT_NULL || builder->remaining == 0U)
    {
        return;
    }

    va_start(arguments, format);
    written = rt_vsnprintf(builder->cursor, builder->remaining,
                           format, arguments);
    va_end(arguments);
    if (written < 0 || (rt_size_t)written >= builder->remaining)
    {
        builder->cursor[builder->remaining - 1U] = '\0';
        builder->remaining = 1U;
        builder->overflow = RT_TRUE;
        return;
    }
    builder->cursor += written;
    builder->remaining -= (rt_size_t)written;
}

static void web_append_json_string(struct web_builder *builder,
                                   const char *text)
{
    const rt_uint8_t *source = (const rt_uint8_t *)(text ? text : "");

    web_append(builder, "\"");
    while (*source != 0U && !builder->overflow && builder->remaining > 1U)
    {
        switch (*source)
        {
        case '"':
        case '\\':
            web_append(builder, "\\%c", *source);
            break;
        case '\n':
            web_append(builder, "\\n");
            break;
        case '\r':
            web_append(builder, "\\r");
            break;
        case '\t':
            web_append(builder, "\\t");
            break;
        default:
            if (*source < 0x20U)
            {
                web_append(builder, "\\u%04X", *source);
            }
            else
            {
                web_append(builder, "%c", *source);
            }
            break;
        }
        source++;
    }
    web_append(builder, "\"");
}

static int web_build_wifi_json(char *payload, rt_size_t payload_size)
{
    struct web_builder builder =
    {
        .cursor = payload,
        .remaining = payload_size,
        .overflow = RT_FALSE,
    };
    char status[sizeof(g_wifi.status_msg)];
    char connected_ssid[sizeof(g_wifi.ssid)];
    char ip[sizeof(g_wifi.ip)];
    int connection_status;
    int scan_status;
    int scan_count;
    rt_uint32_t dropped;
    rt_uint32_t generation;
    int index;

    rt_enter_critical();
    connection_status = g_wifi.conn_status;
    scan_status = g_wifi.scan_status;
    scan_count = g_wifi.scan_count;
    if (scan_count < 0)
    {
        scan_count = 0;
    }
    if (scan_count > (int)WIFI_SCAN_MAX_APS)
    {
        scan_count = (int)WIFI_SCAN_MAX_APS;
    }
    dropped = g_wifi.scan_dropped_count;
    generation = g_wifi.scan_updated;
    rt_strncpy(status, g_wifi.status_msg, sizeof(status) - 1U);
    status[sizeof(status) - 1U] = '\0';
    rt_strncpy(connected_ssid, g_wifi.ssid, sizeof(connected_ssid) - 1U);
    connected_ssid[sizeof(connected_ssid) - 1U] = '\0';
    rt_strncpy(ip, g_wifi.ip, sizeof(ip) - 1U);
    ip[sizeof(ip) - 1U] = '\0';
    if (scan_count > 0)
    {
        rt_memcpy(s_wifi_scan_records, g_wifi.scan_records,
                  (rt_size_t)scan_count * sizeof(s_wifi_scan_records[0]));
    }
    rt_exit_critical();

    web_append(&builder, "{\"connection_status\":%d,\"scan_status\":%d,",
               connection_status, scan_status);
    web_append(&builder, "\"scan_count\":%d,\"dropped\":%lu,"
               "\"generation\":%lu,\"ssid\":",
               scan_count, (unsigned long)dropped,
               (unsigned long)generation);
    web_append_json_string(&builder, connected_ssid);
    web_append(&builder, ",\"ip\":");
    web_append_json_string(&builder, ip);
    web_append(&builder, ",\"status\":");
    web_append_json_string(&builder, status);
    web_append(&builder, ",\"aps\":[");
    for (index = 0; index < scan_count && index < (int)WIFI_SCAN_MAX_APS;
         index++)
    {
        const wifi_ap_record_t *record = &s_wifi_scan_records[index];

        if (index != 0)
        {
            web_append(&builder, ",");
        }
        web_append(&builder, "{\"ssid\":");
        web_append_json_string(&builder, record->ssid);
        web_append(&builder, ",\"rssi\":%d,\"channel\":%d,"
                   "\"security\":%lu,\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
                   (int)record->rssi, (int)record->channel,
                   (unsigned long)record->security,
                   record->bssid[0], record->bssid[1], record->bssid[2],
                   record->bssid[3], record->bssid[4], record->bssid[5]);
    }
    web_append(&builder, "]}");

    return builder.overflow ? -RT_ENOSPC :
           (int)(payload_size - builder.remaining);
}

static int web_build_evidence_manifest(char *payload, rt_size_t payload_size)
{
    web_snapshot_manifest_t manifest;
    int length;

    if (payload == RT_NULL || payload_size == 0U)
    {
        return -RT_EINVAL;
    }
    if (web_snapshot_get_manifest(&manifest) != RT_EOK)
    {
        return -RT_EBUSY;
    }
    length = rt_snprintf(
        payload, payload_size,
        "{\"schema_version\":2,\"token\":\"%s\","
        "\"boot_id\":\"%08lX\",\"snapshot_id\":%lu,"
        "\"revision\":%lu,\"event_seq\":%lu,\"is_final\":%s,"
        "\"paged\":true,\"total_bytes\":%lu,\"page_size\":%u,"
        "\"content_crc32\":\"0x%08lX\","
        "\"json_crc32\":\"0x%08lX\",\"raw_event_count\":%u,"
        "\"page_endpoint\":\"/api/evidence/page/%s/\"}",
        manifest.token, (unsigned long)manifest.boot_id,
        (unsigned long)manifest.snapshot_id,
        (unsigned long)manifest.revision,
        (unsigned long)manifest.last_sequence,
        manifest.is_final ? "true" : "false",
        (unsigned long)manifest.total_bytes,
        WEB_SERVER_EVIDENCE_PAGE_SIZE,
        (unsigned long)manifest.content_crc32,
        (unsigned long)manifest.json_crc32,
        (unsigned int)manifest.raw_event_count, manifest.token);
    return length < 0 || (rt_size_t)length >= payload_size ?
           -RT_ENOSPC : length;
}

static rt_uint32_t web_parse_id(const char *text);

static const char *web_usb_speed_name(rt_uint8_t speed)
{
    switch (speed)
    {
    case USB_SPEED_LOW: return "low";
    case USB_SPEED_FULL: return "full";
    case USB_SPEED_HIGH: return "high";
    case USB_SPEED_SUPER: return "super";
    case USB_SPEED_SUPER_PLUS: return "super_plus";
    default: return "unknown";
    }
}

static int web_build_usb_manifest(char *payload, rt_size_t payload_size)
{
    struct web_builder builder =
    {
        .cursor = payload,
        .remaining = payload_size,
        .overflow = RT_FALSE,
    };
    usb_descriptor_manifest_t manifest;
    rt_uint8_t device_descriptor[18U];
    rt_size_t device_descriptor_length;
    rt_size_t config_count;
    rt_uint16_t index;

    if (!payload || payload_size == 0U ||
        !usb_descriptor_store_get_manifest(&manifest))
    {
        return -RT_EBUSY;
    }
    config_count = usb_descriptor_store_copy_configs(
        s_usb_configs, USB_DESCRIPTOR_MAX_CONFIGURATIONS);
    if (manifest.stored_configurations != 0U && config_count == 0U)
    {
        return -RT_EBUSY;
    }
    device_descriptor_length = manifest.present ?
        usb_descriptor_store_copy_device_raw(0U, device_descriptor,
                                             sizeof(device_descriptor)) : 0U;
    if (manifest.present && manifest.enumeration_complete &&
        device_descriptor_length != sizeof(device_descriptor))
    {
        return -RT_EBUSY;
    }
    web_append(&builder,
               "{\"schema_version\":1,\"generation\":%lu,"
               "\"present\":%s,\"enumeration_complete\":%s,"
               "\"port\":%u,\"hub\":%u,\"speed\":%u,\"speed_name\":",
               (unsigned long)manifest.generation,
               manifest.present ? "true" : "false",
               manifest.enumeration_complete ? "true" : "false",
               manifest.port, manifest.hub_index, manifest.speed);
    web_append_json_string(&builder, web_usb_speed_name(manifest.speed));
    web_append(&builder,
               ",\"vid\":%u,\"pid\":%u,\"bcd_usb\":%u,"
               "\"bcd_device\":%u,\"device_class\":%u,"
               "\"device_class_name\":",
               manifest.vendor_id, manifest.product_id, manifest.bcd_usb,
               manifest.bcd_device, manifest.device_class);
    web_append_json_string(&builder, manifest.device_class_name);
    web_append(&builder, ",\"manufacturer\":");
    web_append_json_string(&builder, manifest.manufacturer);
    web_append(&builder, ",\"product\":");
    web_append_json_string(&builder, manifest.product);
    web_append(&builder, ",\"serial\":");
    web_append_json_string(&builder, manifest.serial);
    web_append(&builder,
               ",\"declared_configurations\":%u,"
               "\"stored_configurations\":%u,\"active_config_index\":%u,"
               "\"config_declared_bytes\":%lu,"
               "\"config_captured_bytes\":%lu,\"config_crc32\":\"0x%08lX\","
               "\"config_complete\":%s,\"config_truncated\":%s,"
               "\"interface_count\":%u,\"endpoint_count\":%u,"
               "\"descriptor_node_count\":%u,\"descriptor_field_count\":%u,"
               "\"string_count\":%u,\"class_bind_failures\":%u,"
               "\"unknown_interface_count\":%u,"
               "\"node_truncated\":%s,\"interface_truncated\":%s,"
               "\"endpoint_truncated\":%s,\"config_page_size\":%u,"
               "\"overflow_reason\":",
               manifest.declared_configurations,
               manifest.stored_configurations, manifest.active_config_index,
               (unsigned long)manifest.config_declared_bytes,
               (unsigned long)manifest.config_captured_bytes,
               (unsigned long)manifest.config_crc32,
               manifest.config_complete ? "true" : "false",
               manifest.config_truncated ? "true" : "false",
               manifest.interface_count, manifest.endpoint_count,
               manifest.node_count, manifest.field_count, manifest.string_count,
               manifest.class_bind_failures, manifest.unknown_interface_count,
               manifest.node_truncated ? "true" : "false",
               manifest.interface_truncated ? "true" : "false",
               manifest.endpoint_truncated ? "true" : "false",
               WEB_SERVER_USB_PAGE_SIZE);
    web_append_json_string(&builder, manifest.overflow_reason);
    web_append(&builder, ",\"configurations\":[");
    {
        rt_uint16_t emitted = 0U;

    for (index = 0U; index < config_count; index++)
    {
        const usb_descriptor_config_info_t *config = &s_usb_configs[index];
        if (emitted++ != 0U)
        {
            web_append(&builder, ",");
        }
        web_append(&builder,
                   "{\"index\":%u,\"value\":%u,\"raw_offset\":%lu,"
                   "\"declared_length\":%u,\"captured_length\":%u,"
                   "\"complete\":%s,\"parsed\":%s,\"malformed\":%s,"
                   "\"truncated\":%s,\"status\":",
                   config->index, config->configuration_value,
                   (unsigned long)config->raw_offset,
                   config->declared_length, config->captured_length,
                   config->complete ? "true" : "false",
                   config->parsed ? "true" : "false",
                   config->malformed ? "true" : "false",
                   config->truncated ? "true" : "false");
        web_append_json_string(&builder, config->status);
        web_append(&builder, "}");
    }
    }
    web_append(&builder, "],\"device_descriptor\":");
    if (device_descriptor_length == sizeof(device_descriptor))
    {
        web_append(&builder, "[");
        for (index = 0U; index < sizeof(device_descriptor); index++)
        {
            web_append(&builder, "%s%u", index == 0U ? "" : ",",
                       (unsigned int)device_descriptor[index]);
        }
        web_append(&builder, "]");
    }
    else
    {
        web_append(&builder, "null");
    }
    web_append(&builder, "}");
    return builder.overflow ? -RT_ENOSPC :
           (int)(payload_size - builder.remaining);
}

static int web_build_usb_interfaces(char *payload, rt_size_t payload_size)
{
    struct web_builder builder =
    {
        .cursor = payload,
        .remaining = payload_size,
        .overflow = RT_FALSE,
    };
    usb_descriptor_manifest_t manifest;
    rt_size_t interface_count;
    rt_uint16_t index;
    rt_uint16_t emitted = 0U;

    if (!payload || payload_size == 0U ||
        !usb_descriptor_store_get_manifest(&manifest))
    {
        return -RT_EBUSY;
    }
    interface_count = usb_descriptor_store_copy_interfaces(
        s_usb_interfaces, USB_DESCRIPTOR_MAX_INTERFACES);
    if (manifest.interface_count != 0U && interface_count == 0U)
    {
        return -RT_EBUSY;
    }
    web_append(&builder, "{\"count\":%u,\"items\":[",
               (unsigned int)interface_count);
    for (index = 0U; index < interface_count; index++)
    {
        const usb_descriptor_interface_info_t *info = &s_usb_interfaces[index];
        if (emitted++ != 0U)
        {
            web_append(&builder, ",");
        }
        web_append(&builder,
                   "{\"index\":%u,\"config\":%u,\"number\":%u,"
                   "\"alternate\":%u,\"class_code\":%u,"
                   "\"subclass\":%u,\"protocol\":%u,\"class\":",
                   index, info->config_index, info->interface_number,
                   info->alternate_setting, info->class_code, info->subclass,
                   info->protocol);
        web_append_json_string(&builder, info->class_name);
        web_append(&builder, ",\"endpoint_start\":%u,\"endpoint_count\":%u,"
                   "\"driver_bound\":%s,\"driver_failed\":%s,\"driver\":",
                   info->endpoint_start, info->endpoint_count,
                   info->class_bound ? "true" : "false",
                   info->class_bind_failed ? "true" : "false");
        web_append_json_string(&builder, info->driver_name);
        web_append(&builder, ",\"name\":");
        web_append_json_string(&builder, info->interface_name);
        web_append(&builder, "}");
    }
    web_append(&builder, "]}");
    return builder.overflow ? -RT_ENOSPC :
           (int)(payload_size - builder.remaining);
}

static int web_build_usb_endpoints(char *payload, rt_size_t payload_size)
{
    struct web_builder builder =
    {
        .cursor = payload,
        .remaining = payload_size,
        .overflow = RT_FALSE,
    };
    usb_descriptor_manifest_t manifest;
    rt_size_t endpoint_count;
    rt_uint16_t index;
    rt_uint16_t emitted = 0U;

    if (!payload || payload_size == 0U ||
        !usb_descriptor_store_get_manifest(&manifest))
    {
        return -RT_EBUSY;
    }
    endpoint_count = usb_descriptor_store_copy_endpoints(
        s_usb_endpoints, USB_DESCRIPTOR_MAX_ENDPOINTS);
    if (manifest.endpoint_count != 0U && endpoint_count == 0U)
    {
        return -RT_EBUSY;
    }
    web_append(&builder, "{\"count\":%u,\"items\":[",
               (unsigned int)endpoint_count);
    for (index = 0U; index < endpoint_count; index++)
    {
        const usb_descriptor_endpoint_info_t *endpoint = &s_usb_endpoints[index];
        if (emitted++ != 0U)
        {
            web_append(&builder, ",");
        }
        web_append(&builder,
                   "{\"index\":%u,\"config\":%u,\"interface\":%u,"
                   "\"alternate\":%u,\"address\":%u,\"attributes\":%u,"
                   "\"max_packet_size\":%u,\"interval\":%u,\"node_id\":%u}",
                   index, endpoint->config_index, endpoint->interface_number,
                   endpoint->alternate_setting, endpoint->address,
                   endpoint->attributes, endpoint->max_packet_size,
                   endpoint->interval, endpoint->node_id);
    }
    web_append(&builder, "]}");
    return builder.overflow ? -RT_ENOSPC :
           (int)(payload_size - builder.remaining);
}

static int web_build_usb_strings(char *payload, rt_size_t payload_size)
{
    struct web_builder builder =
    {
        .cursor = payload,
        .remaining = payload_size,
        .overflow = RT_FALSE,
    };
    usb_descriptor_manifest_t manifest;
    rt_size_t string_count;
    rt_uint16_t index;
    rt_uint16_t emitted = 0U;

    if (!payload || payload_size == 0U ||
        !usb_descriptor_store_get_manifest(&manifest))
    {
        return -RT_EBUSY;
    }
    string_count = usb_descriptor_store_copy_strings(
        s_usb_strings, USB_DESCRIPTOR_MAX_STRINGS);
    if (manifest.string_count != 0U && string_count == 0U)
    {
        return -RT_EBUSY;
    }
    web_append(&builder, "{\"count\":%u,\"items\":[",
               (unsigned int)string_count);
    for (index = 0U; index < string_count; index++)
    {
        const usb_descriptor_string_info_t *info = &s_usb_strings[index];
        if (emitted++ != 0U)
        {
            web_append(&builder, ",");
        }
        web_append(&builder,
                   "{\"index\":%u,\"declared_length\":%u,"
                   "\"captured_length\":%u,\"text\":",
                   info->index, info->declared_length, info->raw_length);
        web_append_json_string(&builder, info->text);
        web_append(&builder, "}");
    }
    web_append(&builder, "]}");
    return builder.overflow ? -RT_ENOSPC :
           (int)(payload_size - builder.remaining);
}

static rt_bool_t web_parse_usb_config_page(const char *path,
                                           rt_uint8_t *config_index,
                                           rt_uint32_t *offset)
{
    const char *cursor;
    rt_uint32_t config;

    if (!path || !config_index || !offset ||
        rt_strncmp(path, "/api/usb/config/page/",
                   sizeof("/api/usb/config/page/") - 1U) != 0)
    {
        return RT_FALSE;
    }
    cursor = path + sizeof("/api/usb/config/page/") - 1U;
    config = web_parse_id(cursor);
    while (*cursor >= '0' && *cursor <= '9')
    {
        cursor++;
    }
    if (*cursor != '/')
    {
        return RT_FALSE;
    }
    *config_index = (rt_uint8_t)config;
    *offset = web_parse_id(cursor + 1U);
    return RT_TRUE;
}

static rt_bool_t web_parse_usb_device_page(const char *path,
                                           rt_uint32_t *offset)
{
    if (!path || !offset ||
        rt_strncmp(path, "/api/usb/device/page/",
                   sizeof("/api/usb/device/page/") - 1U) != 0)
    {
        return RT_FALSE;
    }
    *offset = web_parse_id(path + sizeof("/api/usb/device/page/") - 1U);
    return RT_TRUE;
}

static rt_bool_t web_parse_usb_string_page(const char *path,
                                           rt_uint8_t *string_index,
                                           rt_uint32_t *offset)
{
    const char *cursor;
    rt_uint32_t index;

    if (!path || !string_index || !offset ||
        rt_strncmp(path, "/api/usb/string/page/",
                   sizeof("/api/usb/string/page/") - 1U) != 0)
    {
        return RT_FALSE;
    }
    cursor = path + sizeof("/api/usb/string/page/") - 1U;
    index = web_parse_id(cursor);
    while (*cursor >= '0' && *cursor <= '9')
    {
        cursor++;
    }
    if (*cursor != '/')
    {
        return RT_FALSE;
    }
    *string_index = (rt_uint8_t)index;
    *offset = web_parse_id(cursor + 1U);
    return RT_TRUE;
}

static rt_bool_t web_parse_usb_evidence_event_page(
    const char *path, char *token, rt_uint32_t *sequence,
    rt_uint32_t *offset)
{
    const char *cursor;
    rt_size_t index;
    rt_uint32_t value;

    if (!path || !token || !sequence || !offset ||
        rt_strncmp(path, "/api/evidence/event/",
                   sizeof("/api/evidence/event/") - 1U) != 0)
        return RT_FALSE;
    cursor = path + sizeof("/api/evidence/event/") - 1U;
    for (index = 0U; index < WEB_EVIDENCE_TOKEN_TEXT_LENGTH; index++)
    {
        char c = cursor[index];
        rt_bool_t hex = (c >= '0' && c <= '9') ||
                        (c >= 'A' && c <= 'F') ||
                        (c >= 'a' && c <= 'f');
        if ((index == 8U || index == 17U) ? c != '-' : !hex)
            return RT_FALSE;
    }
    if (cursor[WEB_EVIDENCE_TOKEN_TEXT_LENGTH] != '/')
        return RT_FALSE;
    rt_memcpy(token, cursor, WEB_EVIDENCE_TOKEN_TEXT_LENGTH);
    token[WEB_EVIDENCE_TOKEN_TEXT_LENGTH] = '\0';
    cursor += WEB_EVIDENCE_TOKEN_TEXT_LENGTH + 1U;
    value = 0U;
    if (*cursor < '0' || *cursor > '9') return RT_FALSE;
    while (*cursor >= '0' && *cursor <= '9')
    {
        if (value > (0xFFFFFFFFU - (rt_uint32_t)(*cursor - '0')) / 10U)
            return RT_FALSE;
        value = value * 10U + (rt_uint32_t)(*cursor++ - '0');
    }
    if (*cursor++ != '/') return RT_FALSE;
    *sequence = value;
    value = 0U;
    if (*cursor < '0' || *cursor > '9') return RT_FALSE;
    while (*cursor >= '0' && *cursor <= '9')
    {
        if (value > (0xFFFFFFFFU - (rt_uint32_t)(*cursor - '0')) / 10U)
            return RT_FALSE;
        value = value * 10U + (rt_uint32_t)(*cursor++ - '0');
    }
    if (*cursor != '\0') return RT_FALSE;
    *offset = value;
    return RT_TRUE;
}

static rt_bool_t web_parse_usb_evidence_page(const char *path,
                                            char *token,
                                            rt_uint32_t *offset)
{
    const char *cursor;
    rt_size_t index;
    rt_uint32_t value = 0U;

    if (!path || !token || !offset ||
        rt_strncmp(path, "/api/evidence/page/",
                   sizeof("/api/evidence/page/") - 1U) != 0)
        return RT_FALSE;
    cursor = path + sizeof("/api/evidence/page/") - 1U;
    for (index = 0U; index < WEB_EVIDENCE_TOKEN_TEXT_LENGTH; index++)
    {
        char c = cursor[index];
        rt_bool_t hex = (c >= '0' && c <= '9') ||
                        (c >= 'A' && c <= 'F') ||
                        (c >= 'a' && c <= 'f');
        if ((index == 8U || index == 17U) ? c != '-' : !hex)
            return RT_FALSE;
    }
    if (cursor[WEB_EVIDENCE_TOKEN_TEXT_LENGTH] != '/')
        return RT_FALSE;
    rt_memcpy(token, cursor, WEB_EVIDENCE_TOKEN_TEXT_LENGTH);
    token[WEB_EVIDENCE_TOKEN_TEXT_LENGTH] = '\0';
    cursor += WEB_EVIDENCE_TOKEN_TEXT_LENGTH + 1U;
    if (*cursor < '0' || *cursor > '9') return RT_FALSE;
    while (*cursor >= '0' && *cursor <= '9')
    {
        if (value > (0xFFFFFFFFU - (rt_uint32_t)(*cursor - '0')) / 10U)
            return RT_FALSE;
        value = value * 10U + (rt_uint32_t)(*cursor++ - '0');
    }
    if (*cursor != '\0') return RT_FALSE;
    *offset = value;
    return RT_TRUE;
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static rt_bool_t web_parse_framebuffer_page(const char *path,
                                             rt_uint32_t *generation,
                                             rt_uint32_t *offset)
{
    const char *cursor;
    rt_uint32_t values[2] = { 0U, 0U };
    rt_uint32_t field;

    if (!path || !generation || !offset ||
        rt_strncmp(path, "/api/debug/framebuffer/",
                   sizeof("/api/debug/framebuffer/") - 1U) != 0)
    {
        return RT_FALSE;
    }
    cursor = path + sizeof("/api/debug/framebuffer/") - 1U;
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
            if (*cursor++ != '/') return RT_FALSE;
        }
        else if (*cursor != '\0')
        {
            return RT_FALSE;
        }
    }
    *generation = values[0];
    *offset = values[1];
    return RT_TRUE;
}
#endif

static rt_uint32_t web_parse_id(const char *text)
{
    rt_uint32_t value = 0U;

    while (text != RT_NULL && *text >= '0' && *text <= '9')
    {
        value = value * 10U + (rt_uint32_t)(*text - '0');
        text++;
    }
    return value;
}

static rt_bool_t web_parse_report_token(
    const char *path, const char *prefix,
    char token[WEB_SNAPSHOT_TOKEN_SIZE])
{
    const char *cursor;
    rt_size_t index;

    if (!path || !prefix || !token ||
        rt_strncmp(path, prefix, rt_strlen(prefix)) != 0)
    {
        return RT_FALSE;
    }
    cursor = path + rt_strlen(prefix);
    for (index = 0U; index < WEB_EVIDENCE_TOKEN_TEXT_LENGTH; index++)
    {
        char c = cursor[index];
        rt_bool_t hex = (c >= '0' && c <= '9') ||
                        (c >= 'A' && c <= 'F') ||
                        (c >= 'a' && c <= 'f');
        if ((index == 8U || index == 17U) ? c != '-' : !hex)
        {
            return RT_FALSE;
        }
    }
    if (cursor[WEB_EVIDENCE_TOKEN_TEXT_LENGTH] != '\0')
    {
        return RT_FALSE;
    }
    rt_memcpy(token, cursor, WEB_EVIDENCE_TOKEN_TEXT_LENGTH);
    token[WEB_EVIDENCE_TOKEN_TEXT_LENGTH] = '\0';
    return RT_TRUE;
}

static const char *web_pdf_report_state_name(web_pdf_report_state_t state)
{
    switch (state)
    {
    case WEB_PDF_REPORT_QUEUED: return "queued";
    case WEB_PDF_REPORT_RENDERING: return "rendering";
    case WEB_PDF_REPORT_READY: return "ready";
    case WEB_PDF_REPORT_FAILED: return "failed";
    default: return "idle";
    }
}

static int web_pdf_report_info_json(char *payload, rt_size_t payload_size,
                                    const web_pdf_report_info_t *info)
{
    int written;

    if (!payload || !info)
    {
        return -RT_EINVAL;
    }
    written = rt_snprintf(
        payload, payload_size,
        "{\"job_id\":%lu,\"state\":\"%s\","
        "\"evidence_token\":\"%s\",\"pre_generated\":%s,"
        "\"topology_generation\":%lu,\"render_count\":%lu,"
        "\"pdf_bytes\":%lu,\"pdf_crc32\":\"0x%08lX\","
        "\"chunk_size\":%u,"
        "\"error_code\":%ld,\"heap_peak_bytes\":%lu,"
        "\"heap_live_bytes\":%lu,\"heap_used_bytes\":%lu,"
        "\"heap_capacity_bytes\":%lu,\"stack_used_bytes\":%lu,"
        "\"stack_capacity_bytes\":%lu}",
        (unsigned long)info->job_id,
        web_pdf_report_state_name(info->state),
        info->evidence_token,
        info->pre_generated ? "true" : "false",
        (unsigned long)info->topology_generation,
        (unsigned long)info->render_count,
        (unsigned long)info->pdf_bytes,
        (unsigned long)info->pdf_crc32,
        (unsigned int)WEB_PDF_REPORT_CHUNK_SIZE,
        (long)info->error_code,
        (unsigned long)info->heap_peak_bytes,
        (unsigned long)info->heap_live_bytes,
        (unsigned long)info->heap_used_bytes,
        (unsigned long)info->heap_capacity_bytes,
        (unsigned long)info->stack_used_bytes,
        (unsigned long)info->stack_capacity_bytes);
    return written < 0 || (rt_size_t)written >= payload_size ?
           -RT_ENOSPC : written;
}

static rt_bool_t web_parse_report_path(const char *path, const char *prefix,
                                       rt_uint32_t *job_id,
                                       rt_uint32_t *offset,
                                       rt_bool_t require_offset)
{
    const char *cursor;
    rt_uint32_t values[2] = { 0U, 0U };
    rt_uint32_t value_index;
    rt_uint32_t value_count = require_offset ? 2U : 1U;

    if (!path || !prefix || !job_id ||
        rt_strncmp(path, prefix, rt_strlen(prefix)) != 0)
    {
        return RT_FALSE;
    }
    cursor = path + rt_strlen(prefix);
    for (value_index = 0U; value_index < value_count; value_index++)
    {
        if (*cursor < '0' || *cursor > '9')
        {
            return RT_FALSE;
        }
        while (*cursor >= '0' && *cursor <= '9')
        {
            rt_uint32_t digit = (rt_uint32_t)(*cursor++ - '0');
            if (values[value_index] > (0xFFFFFFFFU - digit) / 10U)
            {
                return RT_FALSE;
            }
            values[value_index] = values[value_index] * 10U + digit;
        }
        if (value_index + 1U < value_count)
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
    if (values[0] == 0U)
    {
        return RT_FALSE;
    }
    *job_id = values[0];
    if (offset)
    {
        *offset = values[1];
    }
    return RT_TRUE;
}

static int web_copy_asset(char *payload, rt_size_t payload_size,
                          const unsigned char *plain, rt_size_t plain_length,
                          const unsigned char *compressed,
                          rt_size_t compressed_length, const char *mime,
                          rt_bool_t accepts_gzip, const char **content_type,
                          rt_bool_t *gzip_encoded)
{
    const unsigned char *selected = accepts_gzip ? compressed : plain;
    rt_size_t length = accepts_gzip ? compressed_length : plain_length;

    if (length + 1U > payload_size)
    {
        return -RT_ENOSPC;
    }
    rt_memcpy(payload, selected, length);
    payload[length] = '\0';
    *content_type = mime;
    if (gzip_encoded != RT_NULL) *gzip_encoded = accepts_gzip;
    return (int)length;
}

static int web_payload_for_path(char *payload, rt_size_t payload_size,
                                const char *path, const char **content_type,
                                rt_bool_t accepts_gzip,
                                rt_bool_t *gzip_encoded)
{
    if (gzip_encoded != RT_NULL) *gzip_encoded = RT_FALSE;
    if (rt_strcmp(path, "/api/wifi/status") == 0) {
        *content_type = "application/json; charset=utf-8";
        return wifi_status_json(payload, payload_size);
    }
    *content_type = "application/json; charset=utf-8";
    if (rt_strcmp(path, "/") == 0 || rt_strcmp(path, "/index.html") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_index_html, work_web_index_html_length,
                              work_web_index_html_gzip,
                              work_web_index_html_gzip_length,
                              "text/html; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/style.css") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_style_css, work_web_style_css_length,
                              work_web_style_css_gzip,
                              work_web_style_css_gzip_length,
                              "text/css; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/data.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_data_js, work_web_data_js_length,
                              work_web_data_js_gzip,
                              work_web_data_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/asset_loader.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_asset_loader_js,
                              work_web_asset_loader_js_length,
                              work_web_asset_loader_js_gzip,
                              work_web_asset_loader_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/app.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_app_js, work_web_app_js_length,
                              work_web_app_js_gzip,
                              work_web_app_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/optional_features.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_optional_features_js,
                              work_web_optional_features_js_length,
                              work_web_optional_features_js_gzip,
                              work_web_optional_features_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/evidence_poll.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_evidence_poll_js,
                              work_web_evidence_poll_js_length,
                              work_web_evidence_poll_js_gzip,
                              work_web_evidence_poll_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/tasks.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_tasks_js, work_web_tasks_js_length,
                              work_web_tasks_js_gzip,
                              work_web_tasks_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/build_info.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_build_info_js, work_web_build_info_js_length,
                              work_web_build_info_js_gzip,
                              work_web_build_info_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/history.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_history_js, work_web_history_js_length,
                              work_web_history_js_gzip,
                              work_web_history_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/topology.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_topology_js, work_web_topology_js_length,
                              work_web_topology_js_gzip,
                              work_web_topology_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/topology_raw.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_topology_raw_js,
                              work_web_topology_raw_js_length,
                              work_web_topology_raw_js_gzip,
                              work_web_topology_raw_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/report_pdf.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_report_pdf_js, work_web_report_pdf_js_length,
                              work_web_report_pdf_js_gzip,
                              work_web_report_pdf_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/assets/lists.js") == 0)
    {
        return web_copy_asset(payload, payload_size,
                              work_web_lists_js, work_web_lists_js_length,
                              work_web_lists_js_gzip,
                              work_web_lists_js_gzip_length,
                              "text/javascript; charset=utf-8", accepts_gzip,
                              content_type, gzip_encoded);
    }
    if (rt_strcmp(path, "/api/wifi") == 0)
    {
        return web_build_wifi_json(payload, payload_size);
    }
    if (rt_strcmp(path, "/api/usb/manifest") == 0)
    {
        return web_build_usb_manifest(payload, payload_size);
    }
    if (rt_strcmp(path, "/api/usb/interfaces") == 0)
    {
        return web_build_usb_interfaces(payload, payload_size);
    }
    if (rt_strcmp(path, "/api/usb/endpoints") == 0)
    {
        return web_build_usb_endpoints(payload, payload_size);
    }
    if (rt_strcmp(path, "/api/usb/strings") == 0)
    {
        return web_build_usb_strings(payload, payload_size);
    }
    if (rt_strncmp(path, "/api/usb/config/page/",
                   sizeof("/api/usb/config/page/") - 1U) == 0)
    {
        rt_uint8_t config_index;
        rt_uint32_t offset;
        usb_descriptor_config_info_t config;
        rt_size_t page_buffer_size = WEB_SERVER_USB_PAGE_SIZE;

        if (!web_parse_usb_config_page(path, &config_index, &offset))
        {
            return -RT_ENOENT;
        }
        if (!usb_descriptor_store_get_config_info(config_index, &config) ||
            offset >= config.captured_length)
        {
            return -RT_ENOENT;
        }
        if (page_buffer_size > payload_size)
        {
            page_buffer_size = payload_size;
        }
        *content_type = "application/octet-stream";
        return (int)usb_descriptor_store_copy_config(
            config_index, offset, (rt_uint8_t *)payload, page_buffer_size);
    }
    if (rt_strncmp(path, "/api/usb/device/page/",
                   sizeof("/api/usb/device/page/") - 1U) == 0)
    {
        rt_uint32_t offset;
        usb_descriptor_manifest_t manifest;
        rt_size_t page_buffer_size = WEB_SERVER_USB_PAGE_SIZE;

        if (!web_parse_usb_device_page(path, &offset))
        {
            return -RT_ENOENT;
        }
        if (!usb_descriptor_store_get_manifest(&manifest) ||
            !manifest.present || offset >= USB_SIZEOF_DEVICE_DESC)
        {
            return -RT_ENOENT;
        }
        if (page_buffer_size > payload_size)
        {
            page_buffer_size = payload_size;
        }
        *content_type = "application/octet-stream";
        return (int)usb_descriptor_store_copy_device_raw(
            offset, (rt_uint8_t *)payload, page_buffer_size);
    }
    if (rt_strncmp(path, "/api/usb/string/page/",
                   sizeof("/api/usb/string/page/") - 1U) == 0)
    {
        rt_uint8_t string_index;
        rt_uint32_t offset;
        usb_descriptor_string_info_t string_info;
        rt_size_t page_buffer_size = WEB_SERVER_USB_PAGE_SIZE;

        if (!web_parse_usb_string_page(path, &string_index, &offset))
        {
            return -RT_ENOENT;
        }
        if (!usb_descriptor_store_get_string_info(string_index, &string_info) ||
            offset >= string_info.raw_length)
        {
            return -RT_ENOENT;
        }
        if (page_buffer_size > payload_size)
        {
            page_buffer_size = payload_size;
        }
        *content_type = "application/octet-stream";
        return (int)usb_descriptor_store_copy_string_raw(
            string_index, offset, (rt_uint8_t *)payload, page_buffer_size);
    }
    if (rt_strncmp(path, "/api/evidence/event/",
                   sizeof("/api/evidence/event/") - 1U) == 0)
    {
        rt_uint32_t sequence;
        rt_uint32_t offset;
        rt_uint32_t payload_crc32 = 0U;
        rt_uint32_t total_length = 0U;
        char token[WEB_SNAPSHOT_TOKEN_SIZE];
        usb_evidence_capture_status_t status = USB_EVIDENCE_CAPTURE_NONE;
        rt_size_t page_buffer_size = WEB_SERVER_EVIDENCE_PAGE_SIZE;
        int result;

        if (!web_parse_usb_evidence_event_page(path, token, &sequence, &offset))
        {
            return -RT_ENOENT;
        }
        if (page_buffer_size > payload_size)
        {
            page_buffer_size = payload_size;
        }
        result = web_snapshot_get_event_payload(
            token, sequence, offset, (rt_uint8_t *)payload,
            page_buffer_size, &total_length, &status, &payload_crc32);
        RT_UNUSED(total_length);
        RT_UNUSED(status);
        RT_UNUSED(payload_crc32);
        if (result >= 0)
        {
            *content_type = "application/octet-stream";
        }
        return result;
    }
    if (rt_strncmp(path, "/api/evidence/page/",
                   sizeof("/api/evidence/page/") - 1U) == 0)
    {
        char token[WEB_SNAPSHOT_TOKEN_SIZE];
        rt_uint32_t offset;
        rt_uint32_t total_length = 0U;
        rt_size_t page_buffer_size = WEB_SERVER_EVIDENCE_PAGE_SIZE;

        if (!web_parse_usb_evidence_page(path, token, &offset))
        {
            return -RT_ENOENT;
        }
        *content_type = "application/octet-stream";
        if (page_buffer_size > payload_size)
        {
            page_buffer_size = payload_size;
        }
        return web_snapshot_get_range(token, offset,
                                      (rt_uint8_t *)payload,
                                      page_buffer_size, &total_length);
    }
    if (rt_strcmp(path, "/api/evidence") == 0)
    {
        return web_build_evidence_manifest(payload, payload_size);
    }
    {
        char token[WEB_SNAPSHOT_TOKEN_SIZE];
        rt_uint32_t job_id;
        int result;

        if (web_parse_report_token(path, "/api/report/start/", token))
        {
            web_pdf_report_info_t info;
            result = web_pdf_report_start(token, &job_id);
            if (result != RT_EOK)
            {
                return result;
            }
            result = web_pdf_report_get_info(job_id, &info);
            return result == RT_EOK ?
                   web_pdf_report_info_json(payload, payload_size, &info) :
                   result;
        }
        if (web_parse_report_token(path, "/api/report/lookup/", token))
        {
            web_pdf_report_info_t info;
            result = web_pdf_report_get_info_by_token(token, &info);
            return result == RT_EOK ?
                   web_pdf_report_info_json(payload, payload_size, &info) :
                   result;
        }
    }
    {
        rt_uint32_t job_id;

        if (web_parse_report_path(path, "/api/report/status/",
                                  &job_id, RT_NULL, RT_FALSE))
        {
            web_pdf_report_info_t info;
            int result = web_pdf_report_get_info(job_id, &info);

            if (result != RT_EOK)
            {
                return result;
            }
            return web_pdf_report_info_json(payload, payload_size, &info);
        }
    }
    {
        rt_uint32_t job_id;
        rt_uint32_t offset;

        if (web_parse_report_path(path, "/api/report/chunk/",
                                  &job_id, &offset, RT_TRUE))
        {
            rt_uint32_t total_length = 0U;
            rt_uint32_t pdf_crc32 = 0U;
            int result;

            *content_type = "application/octet-stream";
            result = web_pdf_report_copy_chunk(
                job_id, offset, (rt_uint8_t *)payload, payload_size,
                &total_length, &pdf_crc32);
            RT_UNUSED(total_length);
            RT_UNUSED(pdf_crc32);
            return result;
        }
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (rt_strcmp(path, "/api/debug/framebuffer") == 0)
    {
        lvgl_framebuffer_capture_info_t info;
        int written;

        if (lvgl_framebuffer_capture_get_info(&info) != RT_EOK)
        {
            return -RT_EBUSY;
        }
        written = rt_snprintf(
            payload, payload_size,
            "{\"generation\":%lu,\"width\":%u,\"height\":%u,"
            "\"pixel_format\":\"RGB565\",\"bytes\":%lu,"
            "\"page_size\":%u,\"source_address\":\"0x%08lX\","
            "\"crc32\":\"%08lX\"}",
            (unsigned long)info.generation,
            (unsigned int)LVGL_FRAMEBUFFER_CAPTURE_WIDTH,
            (unsigned int)LVGL_FRAMEBUFFER_CAPTURE_HEIGHT,
            (unsigned long)info.byte_count,
            (unsigned int)WEB_SERVER_FRAMEBUFFER_PAGE_SIZE,
            (unsigned long)info.source_address,
            (unsigned long)info.crc32);
        return written < 0 || (rt_size_t)written >= payload_size ?
               -RT_ENOSPC : written;
    }
    if (rt_strncmp(path, "/api/debug/framebuffer/",
                   sizeof("/api/debug/framebuffer/") - 1U) == 0)
    {
        rt_uint32_t generation;
        rt_uint32_t offset;
        rt_uint32_t total_bytes = 0U;
        rt_size_t page_size = WEB_SERVER_FRAMEBUFFER_PAGE_SIZE;
        int result;

        if (!web_parse_framebuffer_page(path, &generation, &offset))
        {
            return -WEB_SNAPSHOT_ERR_RANGE;
        }
        if (page_size > payload_size) page_size = payload_size;
        *content_type = "application/octet-stream";
        result = lvgl_framebuffer_capture_copy_page(
            generation, offset, (rt_uint8_t *)payload,
            page_size, &total_bytes);
        RT_UNUSED(total_bytes);
        if (result == -1001) return -WEB_SNAPSHOT_ERR_STALE;
        if (result == -1002) return -WEB_SNAPSHOT_ERR_RANGE;
        return result;
    }
#endif
    if (rt_strcmp(path, "/api/build") == 0)
    {
        int written = rt_snprintf(
            payload, payload_size,
            "{\"source_fingerprint\":\"%s\",\"profile\":\"%s\","
            "\"allocator\":\"%s\",\"lvgl_profile\":\"%s\","
            "\"build_type\":\"%s\",\"compiler\":\"%s\","
            "\"rt_thread\":\"%s\",\"lvgl\":\"%s\","
            "\"usbdiag_version\":\"%s\"}",
            WORK_SOURCE_FINGERPRINT, WORK_BUILD_PROFILE,
            WORK_BUILD_ALLOCATOR, WORK_BUILD_LVGL_PROFILE,
            WORK_BUILD_TYPE, WORK_BUILD_COMPILER,
            WORK_BUILD_RTTHREAD_VERSION, WORK_BUILD_LVGL_VERSION,
            usbdiag_version());
        return written < 0 || (rt_size_t)written >= payload_size ?
               -RT_ENOSPC : written;
    }
    if (rt_strcmp(path, "/api/history") == 0)
    {
        return web_snapshot_get_history(payload, payload_size);
    }
    return -RT_ENOENT;
}

#if defined(MONGOOSE_WEB_BACKEND) && MONGOOSE_WEB_BACKEND
int web_server_render(char *payload, rt_size_t payload_size,
                      const char *path, const char **content_type,
                      rt_bool_t accepts_gzip, rt_bool_t *gzip_encoded)
{
    return web_payload_for_path(payload, payload_size, path, content_type,
                                accepts_gzip, gzip_encoded);
}
#endif

#if !defined(MONGOOSE_WEB_BACKEND) || !MONGOOSE_WEB_BACKEND
static int web_send_bytes(int client, const char *data, rt_size_t length,
                          rt_tick_t deadline)
{
    rt_size_t sent = 0U;

    if (data == RT_NULL && length != 0U)
    {
        return -RT_EINVAL;
    }
    while (sent < length)
    {
        int result;

        /* Use the ordinary socket write path.  The local lwIP port supports
         * sendmsg(), but its vector API adds another tcpip message shape with
         * no benefit for these two small, immutable buffers.  Keeping each
         * call contiguous also makes the lifetime of the application buffer
         * unambiguous while the tcpip thread copies it. */
        result = (int)send(client, data + sent, length - sent, MSG_DONTWAIT);

        if (result > 0)
        {
            sent += (rt_size_t)result;
            /* Keep the aggregate SDIO burst bounded even when a response is
             * split into multiple partial send calls. */
            rt_thread_mdelay(WEB_SERVER_SEND_PACE_MS);
            continue;
        }
        if (result == 0)
        {
            /* A zero-byte send cannot make progress for a non-empty
             * response.  Treat it as a disconnected peer instead of waiting
             * for the retry deadline while holding the aggregate TX lock. */
            web_counter_inc(&s_web_send_failed);
            return -RT_ERROR;
        }
        /* A browser refresh may close the old request while the board is
         * still sending it.  Only transient back-pressure is retryable;
         * immediately release the global TX lock for a dead peer. */
        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR)
        {
            web_counter_inc(&s_web_send_failed);
            return -RT_ERROR;
        }
        if ((rt_int32_t)(rt_tick_get() - deadline) >= 0)
        {
            web_counter_inc(&s_web_send_failed);
            return -RT_ERROR;
        }
        rt_thread_mdelay(WEB_SERVER_SEND_PACE_MS);
    }

    return RT_EOK;
}

static int web_send_response(int client, const char *header,
                             rt_size_t header_length, const char *payload,
                             rt_size_t payload_length)
{
    rt_tick_t deadline = rt_tick_get() +
                         rt_tick_from_millisecond(WEB_SERVER_SEND_TIMEOUT_MS);

    if (web_send_bytes(client, header, header_length, deadline) != RT_EOK)
    {
        return -RT_ERROR;
    }
    if (web_send_bytes(client, payload, payload_length, deadline) != RT_EOK)
    {
        return -RT_ERROR;
    }
    return RT_EOK;
}

static void web_configure_client(int client)
{
    const struct timeval receive_timeout =
    {
        .tv_sec = WEB_SERVER_CLIENT_TIMEOUT_S,
        .tv_usec = 0,
    };
    const struct timeval send_timeout =
    {
        .tv_sec = WEB_SERVER_SEND_TIMEOUT_S,
        .tv_usec = 0,
    };

    /* A browser or a broken cable must not hold a worker forever. */
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                     &receive_timeout, sizeof(receive_timeout));
    /* A stalled WiFi peer must not hold the serialized TX path forever. */
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                     &send_timeout, sizeof(send_timeout));
}

static int web_receive_request(int client, char *request, rt_size_t capacity)
{
    rt_size_t used = 0U;

    if (request == RT_NULL || capacity < 2U)
    {
        return -RT_EINVAL;
    }

    while (used < capacity - 1U)
    {
        int received = recv(client, request + used,
                            capacity - used - 1U, 0);

        if (received <= 0)
        {
            web_counter_inc(&s_web_receive_failed);
            return -RT_ERROR;
        }
        used += (rt_size_t)received;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") != RT_NULL ||
            strstr(request, "\n\n") != RT_NULL)
        {
            return (int)used;
        }
    }
    return -RT_ENOSPC;
}

static rt_bool_t web_reply(int client, int status, const char *content_type,
                           const char *payload, rt_size_t length,
                           rt_bool_t keep_alive)
{
    char header[256];
    const char *reason = status == 200 ? "OK" :
                         (status == 404 ? "Not Found" :
                          (status == 503 ? "Service Unavailable" : "Error"));
    int header_length = rt_snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "%s"
        "Cache-Control: no-store\r\n\r\n",
        status, reason, content_type ? content_type :
        "application/json; charset=utf-8", (unsigned long)length,
        keep_alive ? "Connection: keep-alive\r\n"
                   "Keep-Alive: timeout=1, max=8\r\n" :
                   "Connection: close\r\n");

    if (header_length <= 0 || (rt_size_t)header_length >= sizeof(header) ||
        !s_send_lock_ready)
    {
        web_counter_inc(&s_web_reply_lock_timeout);
        return RT_FALSE;
    }

    if (rt_mutex_take(&s_send_lock,
                      rt_tick_from_millisecond(1000U)) == RT_EOK)
    {
        /* Keep the WLAN SDIO TX queue below its burst capacity.  This is
         * intentionally held under the response mutex so multiple workers
         * cannot defeat the aggregate rate limit. */
        web_wait_response_slot();
        rt_bool_t sent = web_send_response(
            client, header, (rt_size_t)header_length, payload, length) == RT_EOK;
        rt_mutex_release(&s_send_lock);
        return sent;
    }
    else
    {
        web_counter_inc(&s_web_reply_lock_timeout);
        return RT_FALSE;
    }
}

static rt_bool_t web_parse_request_line(const char *request, char *method,
                                        rt_size_t method_size, char *path,
                                        rt_size_t path_size)
{
    const char *first_space;
    const char *second_space;
    rt_size_t method_length;
    rt_size_t path_length;

    if (request == RT_NULL || method == RT_NULL || path == RT_NULL ||
        method_size < 2U || path_size < 2U)
    {
        return RT_FALSE;
    }
    first_space = strchr(request, ' ');
    if (first_space == RT_NULL || first_space == request)
    {
        return RT_FALSE;
    }
    second_space = strchr(first_space + 1, ' ');
    if (second_space == RT_NULL || second_space == first_space + 1)
    {
        return RT_FALSE;
    }
    method_length = (rt_size_t)(first_space - request);
    path_length = (rt_size_t)(second_space - first_space - 1);
    if (method_length >= method_size || path_length >= path_size)
    {
        return RT_FALSE;
    }
    rt_memcpy(method, request, method_length);
    method[method_length] = '\0';
    rt_memcpy(path, first_space + 1, path_length);
    path[path_length] = '\0';
    return RT_TRUE;
}

static rt_bool_t web_request_keep_alive(const char *request)
{
    /*
     * The board's WHD transport has a finite TCP/TIME_WAIT budget.  Keeping
     * HTTP/1.1 connections open by default makes a fast browser refresh (or
     * a short concurrent probe) retain workers and TCP PCBs much longer than
     * the response itself.  One request per connection is deliberately the
     * default for this small read-only service; the explicit request parser
     * and response path remain ready for a future opt-in keep-alive policy.
     */
    RT_UNUSED(request);
    return RT_FALSE;
}

static void web_handle_client(int client, char *payload, rt_size_t payload_size)
{
    char request[WEB_SERVER_REQUEST_SIZE];
    rt_uint32_t request_count;

    web_configure_client(client);
    for (request_count = 0U;
         request_count < WEB_SERVER_MAX_REQUESTS_PER_CLIENT;
         request_count++)
    {
        int received;
        char method[8];
        char path[128];
        const char *content_type;
        int payload_length;
        rt_bool_t keep_alive;

        received = web_receive_request(client, request, sizeof(request));
        if (received <= 0)
        {
            return;
        }
        request[received] = '\0';
        method[0] = '\0';
        path[0] = '\0';
        keep_alive = web_request_keep_alive(request);
        if (!web_parse_request_line(request, method, sizeof(method),
                                    path, sizeof(path)) ||
            rt_strcmp(method, "GET") != 0)
        {
            static const char error_json[] =
                "{\"error\":\"method_not_allowed\"}";
            (void)web_reply(client, 404, "application/json; charset=utf-8",
                            error_json, sizeof(error_json) - 1U, RT_FALSE);
            return;
        }

        /* Ignore query strings for the small read-only API. */
        {
            char *query = strchr(path, '?');

            if (query != RT_NULL)
            {
                *query = '\0';
            }
        }

        payload_length = web_payload_for_path(payload, payload_size,
                                              path, &content_type,
                                              RT_FALSE, RT_NULL);
        if (payload_length < 0)
        {
            static const char not_found[] = "{\"error\":\"not_found\"}";
            static const char busy[] = "{\"error\":\"temporarily_busy\"}";
            static const char too_large[] =
                "{\"error\":\"response_too_large\"}";
            int status = payload_length == -RT_EBUSY ? 503 :
                         (payload_length == -RT_ENOSPC ? 500 : 404);
            const char *error = status == 503 ? busy :
                                (status == 500 ? too_large : not_found);

            if (!web_reply(client, status, "application/json; charset=utf-8",
                           error, rt_strlen(error), keep_alive))
            {
                return;
            }
        }
        else
        {
            if (!web_reply(client, 200, content_type, payload,
                           (rt_size_t)payload_length, keep_alive))
            {
                return;
            }
        }
        if (!keep_alive)
        {
            return;
        }
    }
}

static void web_worker_entry(void *parameter)
{
    struct web_worker *worker = (struct web_worker *)parameter;
    int client;

    if (worker == RT_NULL)
    {
        return;
    }

    while (1)
    {
        if (rt_mq_recv(&s_client_queue, &client, sizeof(client),
                       RT_WAITING_FOREVER) != (rt_ssize_t)sizeof(client))
        {
            continue;
        }
        web_handle_client(client, s_http_payload[worker->index],
                          sizeof(s_http_payload[worker->index]));
        /* send() only hands data to lwIP.  Give the WHD/SDIO TX path a short
         * opportunity to drain the final data/FIN burst before the active
         * close, especially for Connection: close browser refreshes. */
        rt_thread_mdelay(WEB_SERVER_CLOSE_GUARD_MS);
        closesocket(client);
        web_client_leave();
    }
}

static int web_start_workers_locked(void)
{
    rt_uint32_t index;

    if (s_workers_started)
    {
        return RT_EOK;
    }
    if (!s_client_queue_ready)
    {
        if (rt_mq_init(&s_client_queue, "webmq", s_client_queue_pool,
                       sizeof(int), sizeof(s_client_queue_pool),
                       RT_IPC_FLAG_FIFO) != RT_EOK)
        {
            return -RT_ERROR;
        }
        s_client_queue_ready = RT_TRUE;
    }

    for (index = 0U; index < WEB_SERVER_WORKER_COUNT; index++)
    {
        char name[RT_NAME_MAX];

        if (s_workers[index].thread != RT_NULL)
        {
            continue;
        }
        rt_snprintf(name, sizeof(name), "webw%lu", (unsigned long)index);
        s_workers[index].index = (rt_uint8_t)index;
        s_workers[index].thread = &s_worker_thread_objects[index];
        if (rt_thread_init(s_workers[index].thread, name, web_worker_entry,
                           &s_workers[index], s_worker_stacks[index],
                           sizeof(s_worker_stacks[index]),
                           WEB_SERVER_PRIORITY, 20U) != RT_EOK)
        {
            s_workers[index].thread = RT_NULL;
            return -RT_ENOMEM;
        }
        if (rt_thread_startup(s_workers[index].thread) != RT_EOK)
        {
            s_workers[index].thread = RT_NULL;
            return -RT_ERROR;
        }
    }
    s_workers_started = RT_TRUE;
    return RT_EOK;
}

static int web_open_listener(void)
{
    int server;
    int reuse = 1;
    struct sockaddr_in address;

    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
    {
        return -1;
    }
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    rt_memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(WEB_SERVER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(server, WEB_SERVER_BACKLOG) < 0)
    {
        closesocket(server);
        return -1;
    }
    return server;
}

static void web_server_thread_entry(void *parameter)
{
    int server = -1;
    rt_bool_t announced = RT_FALSE;

    RT_UNUSED(parameter);
    while (1)
    {
        /* There is deliberately one application worker.  Close the listening
         * socket while that worker is busy so a burst of browser refreshes is
         * rejected at the TCP boundary instead of accumulating established
         * sockets and FIN/TIME_WAIT traffic in lwIP and the WiFi SDIO path. */
        if (s_web_active_clients >= WEB_SERVER_MAX_ACTIVE_CLIENTS)
        {
            if (server >= 0)
            {
                closesocket(server);
                server = -1;
            }
            rt_thread_mdelay(WEB_SERVER_ACCEPT_BACKOFF_MS);
            continue;
        }

        if (server < 0)
        {
            server = web_open_listener();
            if (server < 0)
            {
                s_server_online = RT_FALSE;
                rt_thread_mdelay(100U);
                continue;
            }
            s_server_online = RT_TRUE;
            if (!announced)
            {
                rt_kprintf("[WEB] evidence desk listening on port %d\r\n",
                           WEB_SERVER_PORT);
                announced = RT_TRUE;
            }
        }

        int client = accept(server, RT_NULL, RT_NULL);

        if (client < 0)
        {
            rt_thread_mdelay(100U);
            continue;
        }
        if (!web_client_try_enter())
        {
            /* Drop overload connections without generating another WLAN TX
             * burst.  The caller can retry, while the active workers remain
             * isolated from a connection storm. */
            closesocket(client);
            rt_thread_mdelay(2U);
            continue;
        }
        closesocket(server);
        server = -1;
        web_configure_client(client);
        if (rt_mq_send(&s_client_queue, &client, sizeof(client)) != RT_EOK)
        {
            web_counter_inc(&s_web_queue_rejected);
            closesocket(client);
            web_client_leave();
            rt_thread_mdelay(2U);
        }
    }
}

/*
 * WiFi startup is initiated from the WLAN event thread.  On some boots that
 * thread can run before INIT_APP_EXPORT() callbacks have completed, so the
 * web service must be able to create its synchronization objects lazily.
 * Interrupts are masked only while the one-time object initialization and
 * ready flags are updated; no blocking operation is performed in this
 * critical section.
 */
static int web_server_ensure_primitives(void)
{
    rt_base_t level;
    rt_err_t result = RT_EOK;

    if (s_start_lock_ready && s_send_lock_ready)
    {
        return RT_EOK;
    }

    level = rt_hw_interrupt_disable();
    if (!s_start_lock_ready)
    {
        result = rt_mutex_init(&s_start_lock, "webmtx", RT_IPC_FLAG_FIFO);
        if (result == RT_EOK)
        {
            s_start_lock_ready = RT_TRUE;
        }
    }
    if (result == RT_EOK && !s_send_lock_ready)
    {
        result = rt_mutex_init(&s_send_lock, "webtx", RT_IPC_FLAG_FIFO);
        if (result == RT_EOK)
        {
            s_send_lock_ready = RT_TRUE;
        }
    }
    rt_hw_interrupt_enable(level);

    return result;
}

int web_server_start(void)
{
    int result;

    if (web_server_ensure_primitives() != RT_EOK ||
        rt_mutex_take(&s_start_lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        return -RT_ERROR;
    }
    if (s_server_thread != RT_NULL)
    {
        rt_mutex_release(&s_start_lock);
        return RT_EOK;
    }

    result = web_start_workers_locked();
    if (result != RT_EOK)
    {
        rt_mutex_release(&s_start_lock);
        return result;
    }

    s_server_thread = &s_server_thread_object;
    if (rt_thread_init(s_server_thread, "webdesk", web_server_thread_entry,
                       RT_NULL, s_server_stack, sizeof(s_server_stack),
                       WEB_SERVER_PRIORITY, 10U) != RT_EOK)
    {
        s_server_thread = RT_NULL;
        rt_mutex_release(&s_start_lock);
        return -RT_ENOMEM;
    }
    if (rt_thread_startup(s_server_thread) != RT_EOK)
    {
        s_server_thread = RT_NULL;
        rt_mutex_release(&s_start_lock);
        return -RT_ERROR;
    }
    rt_mutex_release(&s_start_lock);
    return RT_EOK;
}

int web_server_is_online(void)
{
    return s_server_online ? 1 : 0;
}

static int web_server_component_init(void)
{
    return web_server_ensure_primitives();
}
INIT_APP_EXPORT(web_server_component_init);

#ifdef RT_USING_FINSH
static int web_server_msh(int argc, char **argv)
{
    if (argc == 1 || (argc > 1 && rt_strcmp(argv[1], "start") == 0))
    {
        int result = web_server_start();

        rt_kprintf("web: start result=%d online=%d\n",
                   result, web_server_is_online());
        return result;
    }
    if (argc > 1 && (rt_strcmp(argv[1], "status") == 0 ||
                     rt_strcmp(argv[1], "stats") == 0))
    {
        web_print_status();
        return RT_EOK;
    }

    rt_kprintf("Usage: web [start|status|stats]\n");
    return -RT_ERROR;
}
MSH_CMD_EXPORT_ALIAS(web_server_msh, web, start or inspect the HTTP evidence desk);
#endif
#endif
