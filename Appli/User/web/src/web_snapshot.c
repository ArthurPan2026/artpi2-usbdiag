/*
 * Frozen USB evidence snapshot shared by the Web evidence desk and the
 * application diagnostics.  Only the analysis thread publishes snapshots;
 * readers receive a locked copy of the already analysed data.
 */
#include <rtthread.h>
#include <stdarg.h>
#include <stddef.h>
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#include <finsh.h>
#endif
#include "usb_evidence.h"
#include "web_snapshot.h"
#include "usb_perf.h"
#include "mongoose.h"

#define WEB_SNAPSHOT_LOCK_TIMEOUT_MS 100U
#define USB_EVIDENCE_PRODUCER_VERSION 2U

/* The frozen evidence copy and its JSON projection are much larger than the
 * 64-KB non-cacheable AXI window reserved for USB DMA.  They are CPU-only
 * objects and therefore live in the initialized, non-cacheable PSRAM window
 * owned by the application linker script. */
#define WIFI_WEB_RAM_SECTION \
    __attribute__((section(".wifi_web_ram"), aligned(32)))
#define USB_EVIDENCE_RAM_SECTION \
    __attribute__((section(".usb_evidence_ram"), aligned(32)))

static usb_evidence_snapshot_t g_evidence_primary WIFI_WEB_RAM_SECTION;
static usb_evidence_snapshot_t g_evidence_secondary USB_EVIDENCE_RAM_SECTION;
static usb_evidence_snapshot_t *g_evidence_published = &g_evidence_primary;
static usb_evidence_snapshot_t *g_evidence_candidate = &g_evidence_secondary;
#define g_evidence (*g_evidence_published)
#define g_evidence_build (*g_evidence_candidate)
static char g_json_primary[WEB_SNAPSHOT_JSON_SIZE] WIFI_WEB_RAM_SECTION;
/* Build the next projection in the inactive buffer, then publish it with a
 * pointer swap.  Readers hold g_snapshot_lock only while copying bytes. */
static char g_json_secondary[WEB_SNAPSHOT_JSON_SIZE]
    USB_EVIDENCE_RAM_SECTION;
static char *g_json_current = g_json_primary;
static char *g_json_build = g_json_secondary;
static struct rt_mutex g_snapshot_lock;
static rt_bool_t g_snapshot_lock_ready;
static struct rt_mutex g_json_build_lock;
static rt_bool_t g_json_build_lock_ready;
static rt_bool_t g_json_overflow;
static rt_uint32_t g_json_length;
static rt_uint32_t g_json_crc32;
static char g_generation_token[WEB_SNAPSHOT_TOKEN_SIZE];

struct web_snapshot_summary
{
    rt_uint32_t snapshot_id;
    rt_uint32_t session_id;
    rt_uint32_t revision;
    rt_bool_t is_final;
    usb_evidence_session_state_t session_state;
    usb_evidence_integrity_t capture_integrity;
    rt_uint32_t content_crc32;
    usb_rule_verdict_t conclusion;
};

static struct web_snapshot_summary g_history[3];
static rt_uint8_t g_history_count;

static rt_bool_t web_snapshot_begin(void)
{
    if (!g_snapshot_lock_ready)
    {
        return RT_FALSE;
    }
    /* Rebuilding the JSON projection can take longer than a normal API
     * request on external PSRAM.  A reader must never occupy a Web worker
     * forever if the USB analysis thread is publishing a snapshot. */
    return rt_mutex_take(&g_snapshot_lock,
                         rt_tick_from_millisecond(
                             WEB_SNAPSHOT_LOCK_TIMEOUT_MS)) == RT_EOK;
}

static void web_snapshot_end(void)
{
    if (g_snapshot_lock_ready)
    {
        rt_mutex_release(&g_snapshot_lock);
    }
}

static void json_append(char **cursor, rt_size_t *remaining,
                        const char *format, ...)
{
    va_list arguments;
    int written;

    if (g_json_overflow || !cursor || !*cursor || !remaining ||
        *remaining == 0U)
    {
        return;
    }
    va_start(arguments, format);
    written = rt_vsnprintf(*cursor, *remaining, format, arguments);
    va_end(arguments);
    if (written < 0)
    {
        (*cursor)[0] = '\0';
        *remaining = 1U;
        g_json_overflow = RT_TRUE;
        return;
    }
    if ((rt_size_t)written >= *remaining)
    {
        (*cursor)[*remaining - 1U] = '\0';
        *remaining = 1U;
        g_json_overflow = RT_TRUE;
        return;
    }
    *cursor += written;
    *remaining -= (rt_size_t)written;
}

static void json_append_bytes(char **cursor, rt_size_t *remaining,
                              const rt_uint8_t *bytes, rt_size_t length)
{
    if (g_json_overflow || !cursor || !*cursor || !remaining ||
        *remaining == 0U)
    {
        return;
    }
    if (length >= *remaining)
    {
        (*cursor)[0] = '\0';
        *remaining = 1U;
        g_json_overflow = RT_TRUE;
        return;
    }
    if (length != 0U)
    {
        rt_memcpy(*cursor, bytes, length);
        *cursor += length;
        *remaining -= length;
    }
    **cursor = '\0';
}

static void json_append_string(char **cursor, rt_size_t *remaining,
                               const char *text)
{
    static const rt_uint8_t quote = '"';
    static const rt_uint8_t hex_digits[] = "0123456789ABCDEF";
    const rt_uint8_t *source = (const rt_uint8_t *)(text ? text : "");

    json_append_bytes(cursor, remaining, &quote, 1U);
    while (*source && *remaining > 1U && !g_json_overflow)
    {
        if (*source >= 0x20U && *source < 0x80U &&
            *source != '"' && *source != '\\')
        {
            const rt_uint8_t *run = source;

            do
            {
                source++;
            }
            while (*source >= 0x20U && *source < 0x80U &&
                   *source != '"' && *source != '\\');
            json_append_bytes(cursor, remaining, run,
                              (rt_size_t)(source - run));
            continue;
        }
        if (*source == '"' || *source == '\\' || *source == '\n' ||
            *source == '\r' || *source == '\t')
        {
            rt_uint8_t escaped[2];

            escaped[0] = '\\';
            escaped[1] = *source == '\n' ? 'n' :
                         (*source == '\r' ? 'r' :
                          (*source == '\t' ? 't' : *source));
            json_append_bytes(cursor, remaining, escaped, sizeof(escaped));
            source++;
            continue;
        }
        if (*source < 0x20U)
        {
            rt_uint8_t escaped[6] =
                { '\\', 'u', '0', '0', 0U, 0U };

            escaped[4] = hex_digits[*source >> 4U];
            escaped[5] = hex_digits[*source & 0x0FU];
            json_append_bytes(cursor, remaining, escaped, sizeof(escaped));
            source++;
            continue;
        }
        else
        {
            /* Evidence text can originate in USB string descriptors and is
             * not guaranteed to be UTF-8. Preserve valid UTF-8 sequences,
             * but encode malformed bytes so every response remains valid. */
            rt_size_t sequence_length = 0U;
            rt_size_t index;
            rt_bool_t valid = RT_TRUE;
            rt_uint8_t lead = *source;

            if (lead >= 0xC2U && lead <= 0xDFU)
            {
                sequence_length = 2U;
            }
            else if (lead >= 0xE0U && lead <= 0xEFU)
            {
                sequence_length = 3U;
            }
            else if (lead >= 0xF0U && lead <= 0xF4U)
            {
                sequence_length = 4U;
            }
            else
            {
                valid = RT_FALSE;
            }

            for (index = 1U; valid && index < sequence_length; index++)
            {
                rt_uint8_t continuation = source[index];

                if (continuation == 0U ||
                    (continuation & 0xC0U) != 0x80U)
                {
                    valid = RT_FALSE;
                }
            }

            /* Reject overlong encodings, UTF-16 surrogate encodings, and
             * code points beyond Unicode U+10FFFF. */
            if (valid && sequence_length == 3U &&
                ((lead == 0xE0U && source[1] < 0xA0U) ||
                 (lead == 0xEDU && source[1] >= 0xA0U)))
            {
                valid = RT_FALSE;
            }
            if (valid && sequence_length == 4U &&
                ((lead == 0xF0U && source[1] < 0x90U) ||
                 (lead == 0xF4U && source[1] >= 0x90U)))
            {
                valid = RT_FALSE;
            }

            if (valid)
            {
                json_append_bytes(cursor, remaining, source, sequence_length);
                source += sequence_length;
                continue;
            }
            else
            {
                rt_uint8_t escaped[6] =
                    { '\\', 'u', '0', '0', 0U, 0U };

                escaped[4] = hex_digits[lead >> 4U];
                escaped[5] = hex_digits[lead & 0x0FU];
                json_append_bytes(cursor, remaining, escaped, sizeof(escaped));
                source++;
            }
        }
    }
    json_append_bytes(cursor, remaining, &quote, 1U);
}

static const char *verdict_name(usb_rule_verdict_t verdict)
{
    switch (verdict)
    {
    case USB_RULE_PASS:
        return "PASS";
    case USB_RULE_FAIL:
        return "FAIL";
    case USB_RULE_NOT_APPLICABLE:
        return "NOT_APPLICABLE";
    default:
        return "NOT_OBSERVED";
    }
}

static usb_rule_verdict_t web_snapshot_conclusion(
    const usb_evidence_snapshot_t *snapshot)
{
    rt_size_t index;
    rt_bool_t has_not_observed = RT_FALSE;
    rt_bool_t has_pass = RT_FALSE;

    for (index = 0U; index < USB_EVIDENCE_RULE_COUNT; index++)
    {
        if (snapshot->rules[index].verdict == USB_RULE_FAIL)
        {
            return USB_RULE_FAIL;
        }
        if (snapshot->rules[index].verdict == USB_RULE_NOT_OBSERVED)
        {
            has_not_observed = RT_TRUE;
        }
        if (snapshot->rules[index].verdict == USB_RULE_PASS)
        {
            has_pass = RT_TRUE;
        }
    }
    if (!snapshot->is_final || has_not_observed ||
        snapshot->capture_integrity != USB_EVIDENCE_INTEGRITY_COMPLETE)
    {
        return USB_RULE_NOT_OBSERVED;
    }
    return has_pass ? USB_RULE_PASS : USB_RULE_NOT_APPLICABLE;
}

static const char *capture_name(usb_evidence_capture_status_t status)
{
    return usb_evidence_capture_status_name(status);
}

static const char *data_direction_name(
    usb_evidence_data_direction_t direction)
{
    switch (direction)
    {
    case USB_EVIDENCE_DATA_DEVICE_TO_HOST:
        return "DEVICE_TO_HOST";
    case USB_EVIDENCE_DATA_HOST_TO_DEVICE:
        return "HOST_TO_DEVICE";
    default:
        return "NONE";
    }
}

static const char *event_name(usb_evidence_event_type_t event_type)
{
    switch (event_type)
    {
    case USB_EVIDENCE_EVENT_PORT_RESET:
        return "PORT_RESET";
    case USB_EVIDENCE_EVENT_CONTROL_TRANSFER:
        return "CONTROL_TRANSFER";
    case USB_EVIDENCE_EVENT_ENUMERATION_COMPLETE:
        return "ENUMERATION_COMPLETE";
    case USB_EVIDENCE_EVENT_DISCONNECT:
        return "DISCONNECT";
    default:
        return "UNKNOWN";
    }
}

static const char *timeline_stage_name(usb_evidence_timeline_stage_t stage)
{
    static const char *names[USB_EVIDENCE_TIMELINE_COUNT] =
    {
        "PORT_RESET", "DEVICE_8B", "SET_ADDRESS", "DEVICE_18B",
        "CONFIGURATION", "STRINGS", "SET_CONFIGURATION", "ENUM_COMPLETE",
    };

    return (rt_size_t)stage < USB_EVIDENCE_TIMELINE_COUNT ?
           names[stage] : "UNKNOWN";
}

static const char *timeline_status_name(usb_evidence_timeline_status_t status)
{
    switch (status)
    {
    case USB_EVIDENCE_TIMELINE_OBSERVED: return "OBSERVED";
    case USB_EVIDENCE_TIMELINE_FAILED: return "FAILED";
    default: return "WAITING";
    }
}

/* Schema v2 keeps raw transfer bytes on the versioned event endpoint. */
#undef g_evidence
static int web_snapshot_rebuild_schema(
    const usb_evidence_snapshot_t *source, char *buffer)
{
#define g_evidence (*source)
    char *cursor = buffer;
    rt_size_t remaining = WEB_SNAPSHOT_JSON_SIZE;
    rt_uint32_t passed = 0U;
    rt_uint32_t failed = 0U;
    rt_uint32_t not_observed = 0U;
    rt_uint32_t not_applicable = 0U;
    rt_size_t index;

    g_json_overflow = RT_FALSE;
    for (index = 0U; index < USB_EVIDENCE_RULE_COUNT; index++)
    {
        switch (g_evidence.rules[index].verdict)
        {
        case USB_RULE_PASS: passed++; break;
        case USB_RULE_FAIL: failed++; break;
        case USB_RULE_NOT_APPLICABLE: not_applicable++; break;
        default: not_observed++; break;
        }
    }

    json_append(&cursor, &remaining,
                "{\"schema_version\":%u,\"parser_version\":%u,"
                "\"ruleset_version\":%u,\"boot_id\":\"%08lX\","
                "\"snapshot_id\":%lu,\"session_id\":%lu,"
                "\"attempt_id\":%lu,\"revision\":%lu,"
                "\"event_seq\":%lu,\"session_state\":",
                g_evidence.schema_version, g_evidence.parser_version,
                g_evidence.ruleset_version, (unsigned long)g_evidence.boot_id,
                (unsigned long)g_evidence.snapshot_id,
                (unsigned long)g_evidence.session_id,
                (unsigned long)g_evidence.attempt_id,
                (unsigned long)g_evidence.revision,
                (unsigned long)g_evidence.last_sequence);
    json_append_string(&cursor, &remaining,
                       usb_evidence_session_state_name(g_evidence.session_state));
    json_append(&cursor, &remaining, ",\"is_final\":%s,\"capture_integrity\":",
                g_evidence.is_final ? "true" : "false");
    json_append_string(&cursor, &remaining,
                       usb_evidence_integrity_name(g_evidence.capture_integrity));
    json_append(&cursor, &remaining,
                ",\"content_crc32\":\"0x%08lX\","
                "\"dropped_event_count\":%lu,\"dropped_payload_bytes\":%lu,"
                "\"session_dropped_event_count\":%lu,"
                "\"session_dropped_payload_bytes\":%lu,"
                "\"raw_event_overflow_count\":%lu,\"phase\":",
                (unsigned long)g_evidence.content_crc32,
                (unsigned long)g_evidence.dropped_event_count,
                (unsigned long)g_evidence.dropped_payload_bytes,
                (unsigned long)g_evidence.session_dropped_event_count,
                (unsigned long)g_evidence.session_dropped_payload_bytes,
                (unsigned long)g_evidence.raw_event_overflow_count);
    json_append_string(&cursor, &remaining, g_evidence.phase);
    json_append(&cursor, &remaining, ",\"producer_version\":%u,"
                "\"device\":{\"present\":%s,\"vid\":%u,\"pid\":%u,"
                "\"speed\":%u,\"bcd_usb\":%u,\"bcd_device\":%u,"
                "\"manufacturer\":",
                USB_EVIDENCE_PRODUCER_VERSION,
                g_evidence.device_present ? "true" : "false",
                g_evidence.vendor_id, g_evidence.product_id,
                g_evidence.speed, g_evidence.bcd_usb, g_evidence.bcd_device);
    json_append_string(&cursor, &remaining, g_evidence.manufacturer);
    json_append(&cursor, &remaining, ",\"product\":");
    json_append_string(&cursor, &remaining, g_evidence.product);
    json_append(&cursor, &remaining, ",\"serial\":");
    json_append_string(&cursor, &remaining, g_evidence.serial);
    json_append(&cursor, &remaining, ",\"class\":");
    json_append_string(&cursor, &remaining, g_evidence.class_name);
    json_append(&cursor, &remaining, "},\"rule_summary\":{\"total\":%u,"
                "\"passed\":%lu,\"failed\":%lu,\"not_observed\":%lu,"
                "\"not_applicable\":%lu},\"rule_results\":[",
                USB_EVIDENCE_RULE_COUNT, (unsigned long)passed,
                (unsigned long)failed, (unsigned long)not_observed,
                (unsigned long)not_applicable);

    for (index = 0U; index < USB_EVIDENCE_RULE_COUNT; index++)
    {
        const usb_evidence_rule_result_t *rule = &g_evidence.rules[index];
        rt_size_t reference_index;

        if (index != 0U) json_append(&cursor, &remaining, ",");
        json_append(&cursor, &remaining, "{\"id\":");
        json_append_string(&cursor, &remaining, rule->rule_id);
        json_append(&cursor, &remaining, ",\"clause\":");
        json_append_string(&cursor, &remaining, rule->spec_clause);
        json_append(&cursor, &remaining, ",\"severity\":%u,\"verdict\":",
                    rule->severity);
        json_append_string(&cursor, &remaining, verdict_name(rule->verdict));
        json_append(&cursor, &remaining, ",\"event_sequence\":%lu,\"expected\":",
                    (unsigned long)rule->event_sequence);
        json_append_string(&cursor, &remaining, rule->expected);
        json_append(&cursor, &remaining, ",\"observed\":");
        json_append_string(&cursor, &remaining, rule->observed);
        json_append(&cursor, &remaining, ",\"reason_code\":");
        json_append_string(&cursor, &remaining, rule->reason_code);
        json_append(&cursor, &remaining,
                    ",\"omitted_failure_reference_count\":%u,"
                    "\"evidence_refs\":[",
                    rule->omitted_failure_reference_count);
        for (reference_index = 0U;
             reference_index < rule->evidence_ref_count; reference_index++)
        {
            const usb_evidence_reference_t *reference =
                &rule->evidence_refs[reference_index];

            if (reference_index != 0U) json_append(&cursor, &remaining, ",");
            json_append(&cursor, &remaining,
                        "{\"event_sequence\":%lu,\"byte_offset\":%u,"
                        "\"byte_length\":%u,\"role\":",
                        (unsigned long)reference->event_sequence,
                        reference->byte_offset, reference->byte_length);
            json_append_string(&cursor, &remaining, reference->role);
            json_append(&cursor, &remaining, "}");
        }
        json_append(&cursor, &remaining, "]}");
    }

    json_append(&cursor, &remaining, "],\"raw_events\":[");
    for (index = 0U; index < g_evidence.raw_event_count; index++)
    {
        const usb_evidence_raw_event_t *event = &g_evidence.raw_events[index];

        if (index != 0U) json_append(&cursor, &remaining, ",");
        json_append(&cursor, &remaining,
                    "{\"sequence\":%lu,\"timestamp_us\":%lu,"
                    "\"attempt_id\":%lu,\"port\":%u,"
                    "\"port_valid\":%s,\"timing_valid\":%s,"
                    "\"started_at_us\":%lu,\"completed_at_us\":%lu,"
                    "\"duration_us\":%lu,"
                    "\"type\":",
                    (unsigned long)event->sequence,
                    (unsigned long)event->timestamp_us,
                    (unsigned long)event->attempt_id,
                    event->port,
                    event->port_valid ? "true" : "false",
                    event->timing_valid ? "true" : "false",
                    (unsigned long)event->started_at_us,
                    (unsigned long)event->completed_at_us,
                    (unsigned long)event->duration_us);
        json_append_string(&cursor, &remaining, event_name(event->event_type));
        json_append(&cursor, &remaining,
                    ",\"speed\":%u,\"has_setup\":%s,\"setup\":{"
                    "\"bmRequestType\":%u,\"bRequest\":%u,"
                    "\"wValue\":%u,\"wIndex\":%u,\"wLength\":%u},"
                    "\"return_status\":%ld,\"transfer_length_known\":%s,"
                    "\"transfer_length\":%u,\"captured_length\":%u,"
                    "\"payload_crc32\":\"0x%08lX\","
                    "\"capture_status\":",
                    event->speed, event->has_setup ? "true" : "false",
                    event->setup_packet.bm_request_type,
                    event->setup_packet.b_request, event->setup_packet.w_value,
                    event->setup_packet.w_index, event->setup_packet.w_length,
                    (long)event->return_status,
                    event->transfer_length_known ? "true" : "false",
                    event->transfer_length, event->captured_length,
                    (unsigned long)event->payload_crc32);
        json_append_string(&cursor, &remaining,
                           capture_name(event->capture_status));
        json_append(&cursor, &remaining, ",\"data_direction\":");
        json_append_string(&cursor, &remaining,
                           data_direction_name(event->data_direction));
        json_append(&cursor, &remaining, ",\"payload_bytes\":%u",
                    event->captured_length);
        json_append(&cursor, &remaining, "}");
    }

    json_append(&cursor, &remaining, "],\"descriptor_nodes\":[");
    for (index = 0U; index < g_evidence.descriptor_node_count; index++)
    {
        const usb_evidence_descriptor_node_t *node =
            &g_evidence.descriptor_nodes[index];
        rt_uint8_t field_index;

        if (index != 0U) json_append(&cursor, &remaining, ",");
        json_append(&cursor, &remaining, "{\"id\":%u,\"parent_id\":",
                    node->node_id);
        if (node->parent_id == USB_EVIDENCE_NODE_NONE)
        {
            json_append(&cursor, &remaining, "null");
        }
        else
        {
            json_append(&cursor, &remaining, "%u", node->parent_id);
        }
        json_append(&cursor, &remaining, ",\"type\":");
        json_append_string(&cursor, &remaining, node->type_name);
        json_append(&cursor, &remaining, ",\"title\":");
        json_append_string(&cursor, &remaining, node->summary);
        json_append(&cursor, &remaining,
                    ",\"event_sequence\":%lu,\"byte_offset\":%u,"
                    "\"byte_length\":%u,\"fields\":[",
                    (unsigned long)node->source_sequence, node->source_offset,
                    node->byte_length);
        for (field_index = 0U; field_index < node->field_count; field_index++)
        {
            const usb_evidence_descriptor_field_t *field =
                &g_evidence.descriptor_fields[node->field_start + field_index];

            if (field_index != 0U) json_append(&cursor, &remaining, ",");
            json_append(&cursor, &remaining, "{\"name\":");
            json_append_string(&cursor, &remaining, field->name);
            json_append(&cursor, &remaining, ",\"value\":");
            json_append_string(&cursor, &remaining, field->value);
            json_append(&cursor, &remaining, ",\"byte_offset\":%u,"
                        "\"byte_length\":%u}", field->byte_offset,
                        field->byte_length);
        }
        json_append(&cursor, &remaining, "]}");
    }

    json_append(&cursor, &remaining, "],\"timeline\":[");
    for (index = 0U; index < g_evidence.timeline_count; index++)
    {
        const usb_evidence_timeline_item_t *item = &g_evidence.timeline[index];

        if (index != 0U) json_append(&cursor, &remaining, ",");
        json_append(&cursor, &remaining,
                    "{\"event_sequence\":%lu,\"timestamp_us\":%lu,"
                    "\"kind\":",
                    (unsigned long)item->event_sequence,
                    (unsigned long)item->timestamp_us);
        json_append_string(&cursor, &remaining, timeline_stage_name(item->stage));
        json_append(&cursor, &remaining, ",\"status\":");
        json_append_string(&cursor, &remaining,
                           timeline_status_name(item->status));
        json_append(&cursor, &remaining, ",\"label\":");
        json_append_string(&cursor, &remaining, item->label);
        json_append(&cursor, &remaining, "}");
    }
    json_append(&cursor, &remaining, "]}");

    if (g_json_overflow)
    {
        buffer[0] = '\0';
        return -RT_ENOSPC;
    }
    return (int)(WEB_SNAPSHOT_JSON_SIZE - remaining);
#undef g_evidence
}
#define g_evidence (*g_evidence_published)

static rt_uint32_t web_snapshot_crc32(const void *data, rt_size_t length)
{
    return mg_crc32(0U, (const char *)data, (size_t)length);
}

static void web_snapshot_copy_active(usb_evidence_snapshot_t *destination,
                                     const usb_evidence_snapshot_t *source)
{
    rt_size_t index;
    rt_size_t raw_count = source->raw_event_count;
    rt_size_t node_count = source->descriptor_node_count;
    rt_size_t field_count = source->descriptor_field_count;
    rt_size_t timeline_count = source->timeline_count;
    rt_size_t event_prefix = offsetof(usb_evidence_raw_event_t, raw_response);

    if (raw_count > USB_EVIDENCE_SNAPSHOT_EVENT_COUNT)
        raw_count = USB_EVIDENCE_SNAPSHOT_EVENT_COUNT;
    if (node_count > USB_EVIDENCE_DESCRIPTOR_NODE_COUNT)
        node_count = USB_EVIDENCE_DESCRIPTOR_NODE_COUNT;
    if (field_count > USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT)
        field_count = USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT;
    if (timeline_count > USB_EVIDENCE_TIMELINE_COUNT)
        timeline_count = USB_EVIDENCE_TIMELINE_COUNT;

    rt_memcpy(destination, source,
              offsetof(usb_evidence_snapshot_t, raw_events));
    destination->raw_event_count = (rt_uint8_t)raw_count;
    destination->descriptor_node_count = (rt_uint8_t)node_count;
    destination->descriptor_field_count = (rt_uint8_t)field_count;
    destination->timeline_count = (rt_uint8_t)timeline_count;

    for (index = 0U; index < raw_count; index++)
    {
        const usb_evidence_raw_event_t *source_event =
            &source->raw_events[index];
        usb_evidence_raw_event_t *destination_event =
            &destination->raw_events[index];
        rt_size_t captured = source_event->captured_length;

        if (captured > USB_EVIDENCE_CAPTURE_MAX)
            captured = USB_EVIDENCE_CAPTURE_MAX;
        rt_memcpy(destination_event, source_event, event_prefix + captured);
        if (source_event->captured_length > USB_EVIDENCE_CAPTURE_MAX)
        {
            destination_event->captured_length = USB_EVIDENCE_CAPTURE_MAX;
            destination_event->capture_status =
                USB_EVIDENCE_CAPTURE_LOCAL_TRUNCATED;
        }
    }
    rt_memcpy(destination->descriptor_nodes, source->descriptor_nodes,
              node_count * sizeof(source->descriptor_nodes[0]));
    rt_memcpy(destination->descriptor_fields, source->descriptor_fields,
              field_count * sizeof(source->descriptor_fields[0]));
    rt_memcpy(destination->timeline, source->timeline,
              timeline_count * sizeof(source->timeline[0]));
    rt_memcpy(destination->rules, source->rules,
              sizeof(destination->rules));
}

static void web_snapshot_sanitize(usb_evidence_snapshot_t *snapshot)
{
    rt_size_t index;

    snapshot->manufacturer[sizeof(snapshot->manufacturer) - 1U] = '\0';
    snapshot->product[sizeof(snapshot->product) - 1U] = '\0';
    snapshot->serial[sizeof(snapshot->serial) - 1U] = '\0';
    snapshot->class_name[sizeof(snapshot->class_name) - 1U] = '\0';
    snapshot->phase[sizeof(snapshot->phase) - 1U] = '\0';
    if (snapshot->raw_event_count > USB_EVIDENCE_SNAPSHOT_EVENT_COUNT)
    {
        snapshot->raw_event_count = USB_EVIDENCE_SNAPSHOT_EVENT_COUNT;
    }
    if (snapshot->descriptor_node_count > USB_EVIDENCE_DESCRIPTOR_NODE_COUNT)
    {
        snapshot->descriptor_node_count = USB_EVIDENCE_DESCRIPTOR_NODE_COUNT;
    }
    if (snapshot->descriptor_field_count > USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT)
    {
        snapshot->descriptor_field_count = USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT;
    }
    if (snapshot->timeline_count > USB_EVIDENCE_TIMELINE_COUNT)
    {
        snapshot->timeline_count = USB_EVIDENCE_TIMELINE_COUNT;
    }
    for (index = 0U; index < snapshot->raw_event_count; index++)
    {
        usb_evidence_raw_event_t *event = &snapshot->raw_events[index];
        if (event->captured_length > USB_EVIDENCE_CAPTURE_MAX)
        {
            event->captured_length = USB_EVIDENCE_CAPTURE_MAX;
            event->capture_status = USB_EVIDENCE_CAPTURE_LOCAL_TRUNCATED;
        }
        event->payload_crc32 = web_snapshot_crc32(event->raw_response,
                                                   event->captured_length);
    }
    for (index = 0U; index < USB_EVIDENCE_RULE_COUNT; index++)
    {
        usb_evidence_rule_result_t *rule = &snapshot->rules[index];
        rule->expected[sizeof(rule->expected) - 1U] = '\0';
        rule->observed[sizeof(rule->observed) - 1U] = '\0';
        rule->reason_code[sizeof(rule->reason_code) - 1U] = '\0';
        if (rule->evidence_ref_count > USB_EVIDENCE_REFERENCE_COUNT)
        {
            rule->evidence_ref_count = USB_EVIDENCE_REFERENCE_COUNT;
        }
    }
    snapshot->content_crc32 = 0U;
    snapshot->content_crc32 = usb_evidence_snapshot_crc32(snapshot);
}

static int web_snapshot_publish_candidate(void)
{
    rt_uint32_t length;
    rt_uint32_t json_crc32;
    rt_uint8_t history_index;
    char *old_json;
    usb_evidence_snapshot_t *old_snapshot;
    int build_length;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    rt_uint32_t json_lock_start;
    rt_uint32_t json_lock_acquired;
    rt_uint32_t json_crc_start;
    rt_uint32_t json_crc_end;
    rt_uint32_t publish_acquired;
    rt_uint32_t publish_hold_end;
    rt_uint32_t publish_end;
#endif
    rt_uint32_t build_start;
    rt_uint32_t publish_start;

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    json_lock_start = usb_perf_now();
#endif
    if (!g_json_build_lock_ready ||
        rt_mutex_take(&g_json_build_lock, RT_WAITING_FOREVER) != RT_EOK)
    {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        usb_perf_record(USB_PERF_JSON_LOCK_WAIT,
                        json_lock_start, usb_perf_now());
#endif
        return -RT_ERROR;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    json_lock_acquired = usb_perf_now();
#endif

    build_start = usb_perf_now();
    web_snapshot_sanitize(&g_evidence_build);
    build_length = web_snapshot_rebuild_schema(&g_evidence_build, g_json_build);
    usb_perf_record(USB_PERF_JSON_BUILD, build_start, usb_perf_now());
    if (build_length <= 0 || (rt_uint32_t)build_length >= WEB_SNAPSHOT_JSON_SIZE)
    {
        rt_mutex_release(&g_json_build_lock);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        usb_perf_record(USB_PERF_JSON_LOCK_WAIT,
                        json_lock_start, json_lock_acquired);
#endif
        return build_length < 0 ? build_length : -RT_ENOSPC;
    }
    length = (rt_uint32_t)build_length;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    json_crc_start = usb_perf_now();
#endif
    json_crc32 = web_snapshot_crc32(g_json_build, length);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    json_crc_end = usb_perf_now();
#endif

    publish_start = usb_perf_now();
    if (!web_snapshot_begin())
    {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        publish_end = usb_perf_now();
        usb_perf_record(USB_PERF_PUBLISH_LOCK, publish_start, publish_end);
#else
        usb_perf_record(USB_PERF_PUBLISH_LOCK,
                        publish_start, usb_perf_now());
#endif
        rt_mutex_release(&g_json_build_lock);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        usb_perf_record(USB_PERF_JSON_LOCK_WAIT,
                        json_lock_start, json_lock_acquired);
        usb_perf_record(USB_PERF_JSON_CRC, json_crc_start, json_crc_end);
        usb_perf_record(USB_PERF_WEB_PUBLISH_LOCK_WAIT,
                        publish_start, publish_end);
#endif
        return -RT_EBUSY;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    publish_acquired = usb_perf_now();
#endif

    old_snapshot = g_evidence_published;
    g_evidence_published = g_evidence_candidate;
    g_evidence_candidate = old_snapshot;
    old_json = g_json_current;
    g_json_current = g_json_build;
    g_json_build = old_json;
    g_json_length = length;
    g_json_crc32 = json_crc32;
    rt_snprintf(g_generation_token, sizeof(g_generation_token),
                "%08lX-%08lX-%08lX",
                (unsigned long)g_evidence.boot_id,
                (unsigned long)g_evidence.snapshot_id,
                (unsigned long)g_evidence.content_crc32);

    if (g_evidence.is_final && g_evidence.snapshot_id != 0U)
    {
        if (g_history_count < (rt_uint8_t)(sizeof(g_history) /
                                           sizeof(g_history[0])))
        {
            history_index = g_history_count++;
        }
        else
        {
            rt_memmove(&g_history[0], &g_history[1],
                       sizeof(g_history) - sizeof(g_history[0]));
            history_index = (rt_uint8_t)(sizeof(g_history) /
                                         sizeof(g_history[0]) - 1U);
        }
        g_history[history_index].snapshot_id = g_evidence.snapshot_id;
        g_history[history_index].session_id = g_evidence.session_id;
        g_history[history_index].revision = g_evidence.revision;
        g_history[history_index].is_final = g_evidence.is_final;
        g_history[history_index].session_state = g_evidence.session_state;
        g_history[history_index].capture_integrity =
            g_evidence.capture_integrity;
        g_history[history_index].content_crc32 = g_evidence.content_crc32;
        g_history[history_index].conclusion =
            web_snapshot_conclusion(&g_evidence);
    }

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    publish_hold_end = usb_perf_now();
#endif
    web_snapshot_end();
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    publish_end = usb_perf_now();
    rt_mutex_release(&g_json_build_lock);
    usb_perf_record(USB_PERF_JSON_LOCK_WAIT,
                    json_lock_start, json_lock_acquired);
    usb_perf_record(USB_PERF_JSON_CRC, json_crc_start, json_crc_end);
    usb_perf_record(USB_PERF_WEB_PUBLISH_LOCK_WAIT,
                    publish_start, publish_acquired);
    usb_perf_record(USB_PERF_WEB_PUBLISH_LOCK_HOLD,
                    publish_acquired, publish_hold_end);
    usb_perf_record(USB_PERF_PUBLISH_LOCK, publish_start, publish_end);
#else
    usb_perf_record(USB_PERF_PUBLISH_LOCK, publish_start, usb_perf_now());
    rt_mutex_release(&g_json_build_lock);
#endif
    return RT_EOK;
}

int web_snapshot_update_evidence(const usb_evidence_snapshot_t *snapshot)
{
    rt_uint32_t copy_start;

    if (!snapshot)
    {
        return -RT_EINVAL;
    }
    copy_start = usb_perf_now();
    web_snapshot_copy_active(&g_evidence_build, snapshot);
    usb_perf_record(USB_PERF_SNAPSHOT_COPY, copy_start, usb_perf_now());
    return web_snapshot_publish_candidate();
}

void web_snapshot_reset(void)
{
    rt_memset(&g_evidence_build, 0, sizeof(g_evidence_build));
    g_evidence_build.schema_version = USB_EVIDENCE_SCHEMA_VERSION;
    rt_strncpy(g_evidence_build.phase, "等待设备",
               sizeof(g_evidence_build.phase) - 1U);
    (void)web_snapshot_publish_candidate();
}

rt_bool_t web_snapshot_get_copy(usb_evidence_snapshot_t *snapshot)
{
    rt_uint32_t copy_start;

    if (!snapshot || !g_snapshot_lock_ready ||
        rt_mutex_take(&g_snapshot_lock, RT_WAITING_NO) != RT_EOK)
    {
        return RT_FALSE;
    }
    copy_start = usb_perf_now();
    web_snapshot_copy_active(snapshot, &g_evidence);
    rt_mutex_release(&g_snapshot_lock);
    usb_perf_record(USB_PERF_SNAPSHOT_COPY, copy_start, usb_perf_now());
    return RT_TRUE;
}

int web_snapshot_get_pdf_view(const char *token,
                              web_snapshot_pdf_view_t *view)
{
    rt_size_t index;

    if (!token || !view)
    {
        return -RT_EINVAL;
    }
    if (!web_snapshot_begin())
    {
        return -RT_EBUSY;
    }
    if (rt_strcmp(token, g_generation_token) != 0)
    {
        web_snapshot_end();
        return -WEB_SNAPSHOT_ERR_STALE;
    }

    rt_memset(view, 0, sizeof(*view));
    rt_strncpy(view->token, g_generation_token, sizeof(view->token) - 1U);
    view->boot_id = g_evidence.boot_id;
    view->snapshot_id = g_evidence.snapshot_id;
    view->session_id = g_evidence.session_id;
    view->attempt_id = g_evidence.attempt_id;
    view->revision = g_evidence.revision;
    view->last_sequence = g_evidence.last_sequence;
    view->content_crc32 = g_evidence.content_crc32;
    view->json_crc32 = g_json_crc32;
    view->dropped_event_count = g_evidence.dropped_event_count;
    view->dropped_payload_bytes = g_evidence.dropped_payload_bytes;
    view->session_dropped_event_count =
        g_evidence.session_dropped_event_count;
    view->session_dropped_payload_bytes =
        g_evidence.session_dropped_payload_bytes;
    view->raw_event_overflow_count = g_evidence.raw_event_overflow_count;
    view->schema_version = g_evidence.schema_version;
    view->parser_version = g_evidence.parser_version;
    view->ruleset_version = g_evidence.ruleset_version;
    view->session_state = g_evidence.session_state;
    view->capture_integrity = g_evidence.capture_integrity;
    view->is_final = g_evidence.is_final;
    view->device_present = g_evidence.device_present;
    view->speed = g_evidence.speed;
    view->vendor_id = g_evidence.vendor_id;
    view->product_id = g_evidence.product_id;
    view->bcd_usb = g_evidence.bcd_usb;
    view->bcd_device = g_evidence.bcd_device;
    rt_strncpy(view->manufacturer, g_evidence.manufacturer,
               sizeof(view->manufacturer) - 1U);
    rt_strncpy(view->product, g_evidence.product,
               sizeof(view->product) - 1U);
    rt_strncpy(view->class_name, g_evidence.class_name,
               sizeof(view->class_name) - 1U);
    rt_strncpy(view->phase, g_evidence.phase, sizeof(view->phase) - 1U);

    view->timeline_count = g_evidence.timeline_count;
    if (view->timeline_count > USB_EVIDENCE_TIMELINE_COUNT)
    {
        view->timeline_count = USB_EVIDENCE_TIMELINE_COUNT;
    }
    for (index = 0U; index < view->timeline_count; index++)
    {
        view->timeline[index] = g_evidence.timeline[index];
    }
    for (index = 0U; index < USB_EVIDENCE_RULE_COUNT; index++)
    {
        view->rules[index] = g_evidence.rules[index];
    }

    view->event_count = g_evidence.raw_event_count;
    if (view->event_count > USB_EVIDENCE_SNAPSHOT_EVENT_COUNT)
    {
        view->event_count = USB_EVIDENCE_SNAPSHOT_EVENT_COUNT;
    }
    for (index = 0U; index < view->event_count; index++)
    {
        const usb_evidence_raw_event_t *source = &g_evidence.raw_events[index];
        web_snapshot_pdf_event_t *destination = &view->events[index];

        destination->sequence = source->sequence;
        destination->timestamp_us = source->timestamp_us;
        destination->duration_us = source->duration_us;
        destination->event_type = source->event_type;
        destination->port = source->port;
        destination->port_valid = source->port_valid;
        destination->timing_valid = source->timing_valid;
        destination->speed = source->speed;
        destination->has_setup = source->has_setup;
        destination->setup_packet = source->setup_packet;
        destination->return_status = source->return_status;
        destination->transfer_length_known = source->transfer_length_known;
        destination->transfer_length = source->transfer_length;
        destination->captured_length = source->captured_length;
        destination->capture_status = source->capture_status;
        destination->payload_crc32 = source->payload_crc32;
    }

    web_snapshot_end();
    return RT_EOK;
}

rt_bool_t web_snapshot_get_meta(rt_uint32_t *session_id,
                                rt_uint32_t *revision)
{
    if (!g_snapshot_lock_ready ||
        rt_mutex_take(&g_snapshot_lock, RT_WAITING_NO) != RT_EOK)
    {
        return RT_FALSE;
    }
    if (session_id) *session_id = g_evidence.session_id;
    if (revision) *revision = g_evidence.revision;
    rt_mutex_release(&g_snapshot_lock);
    return RT_TRUE;
}

int web_snapshot_get_manifest(web_snapshot_manifest_t *manifest)
{
    if (!manifest)
    {
        return -RT_EINVAL;
    }
    if (!web_snapshot_begin())
    {
        return -RT_EBUSY;
    }
    rt_memset(manifest, 0, sizeof(*manifest));
    rt_strncpy(manifest->token, g_generation_token,
               sizeof(manifest->token) - 1U);
    manifest->boot_id = g_evidence.boot_id;
    manifest->snapshot_id = g_evidence.snapshot_id;
    manifest->revision = g_evidence.revision;
    manifest->last_sequence = g_evidence.last_sequence;
    manifest->content_crc32 = g_evidence.content_crc32;
    manifest->json_crc32 = g_json_crc32;
    manifest->total_bytes = g_json_length;
    manifest->raw_event_count = g_evidence.raw_event_count;
    manifest->is_final = g_evidence.is_final;
    web_snapshot_end();
    return RT_EOK;
}

int web_snapshot_get_range(const char *token, rt_uint32_t offset,
                           rt_uint8_t *buffer, rt_size_t buffer_size,
                           rt_uint32_t *total_length)
{
    rt_size_t length;

    if (!token || !buffer || buffer_size == 0U)
    {
        return -RT_EINVAL;
    }
    if (!web_snapshot_begin())
    {
        return -RT_EBUSY;
    }
    if (rt_strcmp(token, g_generation_token) != 0)
    {
        web_snapshot_end();
        return -WEB_SNAPSHOT_ERR_STALE;
    }
    if (total_length)
    {
        *total_length = g_json_length;
    }
    if (offset >= g_json_length)
    {
        web_snapshot_end();
        return -WEB_SNAPSHOT_ERR_RANGE;
    }
    length = g_json_length - offset;
    if (length > buffer_size)
    {
        length = buffer_size;
    }
    rt_memcpy(buffer, (const rt_uint8_t *)g_json_current + offset, length);
    web_snapshot_end();
    return (int)length;
}

int web_snapshot_get_event_payload(const char *token,
                                   rt_uint32_t sequence,
                                   rt_uint32_t offset,
                                   rt_uint8_t *buffer,
                                   rt_size_t buffer_size,
                                   rt_uint32_t *total_length,
                                   usb_evidence_capture_status_t *status,
                                   rt_uint32_t *payload_crc32)
{
    rt_uint8_t index;

    if (!token || !buffer || buffer_size == 0U)
    {
        return -RT_EINVAL;
    }
    if (!web_snapshot_begin())
    {
        return -RT_EBUSY;
    }
    if (rt_strcmp(token, g_generation_token) != 0)
    {
        web_snapshot_end();
        return -WEB_SNAPSHOT_ERR_STALE;
    }
    for (index = 0U; index < g_evidence.raw_event_count; index++)
    {
        const usb_evidence_raw_event_t *event = &g_evidence.raw_events[index];
        rt_size_t length;
        if (event->sequence != sequence)
        {
            continue;
        }
        if (total_length) *total_length = event->captured_length;
        if (status) *status = event->capture_status;
        if (payload_crc32) *payload_crc32 = event->payload_crc32;
        if (offset >= event->captured_length)
        {
            web_snapshot_end();
            return -WEB_SNAPSHOT_ERR_RANGE;
        }
        length = event->captured_length - offset;
        if (length > buffer_size) length = buffer_size;
        rt_memcpy(buffer, event->raw_response + offset, length);
        web_snapshot_end();
        return (int)length;
    }
    web_snapshot_end();
    return -RT_ENOENT;
}

int web_snapshot_get_history(char *buffer, rt_size_t buffer_size)
{
    char *cursor = buffer;
    rt_size_t remaining = buffer_size;
    struct web_snapshot_summary history[3];
    rt_uint8_t history_count;
    rt_uint8_t index;

    if (!buffer || buffer_size == 0U || !web_snapshot_begin())
    {
        return buffer == RT_NULL || buffer_size == 0U ? -RT_EINVAL : -RT_EBUSY;
    }
    history_count = g_history_count;
    rt_memcpy(history, g_history,
              history_count * sizeof(struct web_snapshot_summary));
    web_snapshot_end();
    if (!g_json_build_lock_ready ||
        rt_mutex_take(&g_json_build_lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        return -RT_EBUSY;
    }
    g_json_overflow = RT_FALSE;
    json_append(&cursor, &remaining,
                "{\"history_type\":\"summary_only\",\"items\":[");
    for (index = 0U; index < history_count; index++)
    {
        const struct web_snapshot_summary *item = &history[index];

        if (index != 0U) json_append(&cursor, &remaining, ",");
        json_append(&cursor, &remaining,
                    "{\"snapshot_id\":%lu,\"session_id\":%lu,"
                    "\"revision\":%lu,\"session_state\":",
                    (unsigned long)item->snapshot_id,
                    (unsigned long)item->session_id,
                    (unsigned long)item->revision);
        json_append_string(&cursor, &remaining,
                           usb_evidence_session_state_name(item->session_state));
        json_append(&cursor, &remaining, ",\"is_final\":%s,"
                    "\"capture_integrity\":",
                    item->is_final ? "true" : "false");
        json_append_string(&cursor, &remaining,
                           usb_evidence_integrity_name(item->capture_integrity));
        json_append(&cursor, &remaining, ",\"content_crc32\":\"0x%08lX\","
                    "\"conclusion\":",
                    (unsigned long)item->content_crc32);
        json_append_string(&cursor, &remaining, verdict_name(item->conclusion));
        json_append(&cursor, &remaining, "}");
    }
    json_append(&cursor, &remaining, "]}");
    {
        int result = g_json_overflow ? -RT_ENOSPC :
                      (int)(buffer_size - remaining);

        rt_mutex_release(&g_json_build_lock);
        return result;
    }
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static rt_uint32_t web_snapshot_crc32_bitwise_oracle(const void *data,
                                                      rt_size_t length)
    __attribute__((noinline));

static rt_uint32_t web_snapshot_crc32_bitwise_oracle(const void *data,
                                                      rt_size_t length)
{
    const rt_uint8_t *cursor = (const rt_uint8_t *)data;
    rt_uint32_t crc = 0xFFFFFFFFU;
    rt_size_t index;

    for (index = 0U; index < length; index++)
    {
        rt_uint8_t bit;
        crc ^= cursor[index];
        for (bit = 0U; bit < 8U; bit++)
        {
            crc = (crc >> 1U) ^
                  ((crc & 1U) ? 0xEDB88320U : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static rt_uint32_t web_snapshot_crc32_median(rt_uint32_t samples[7])
{
    rt_uint32_t i;

    for (i = 1U; i < 7U; i++)
    {
        rt_uint32_t value = samples[i];
        rt_uint32_t position = i;
        while (position > 0U && samples[position - 1U] > value)
        {
            samples[position] = samples[position - 1U];
            position--;
        }
        samples[position] = value;
    }
    return samples[3];
}

static void web_snapshot_crc_bench_cmd(void)
{
    rt_uint32_t bitwise_cycles[7];
    rt_uint32_t mongoose_cycles[7];
    rt_uint32_t bitwise_crc = 0U;
    rt_uint32_t mongoose_crc = 0U;
    rt_uint32_t bitwise_median;
    rt_uint32_t mongoose_median;
    rt_uint32_t saved_permille;
    rt_uint32_t index;
    rt_size_t length;
    const void *data;

    if (!g_snapshot_lock_ready ||
        rt_mutex_take(&g_snapshot_lock, RT_WAITING_NO) != RT_EOK)
    {
        rt_kprintf("[web_crc_bench] FAIL snapshot busy\r\n");
        return;
    }
    data = g_json_current;
    length = g_json_length;
    if (data == RT_NULL || length == 0U)
    {
        rt_mutex_release(&g_snapshot_lock);
        rt_kprintf("[web_crc_bench] FAIL no published JSON\r\n");
        return;
    }

    /* Both implementations consume the same live snapshot bytes. Alternate
     * order to reduce systematic cache and temperature bias. */
    for (index = 0U; index < 7U; index++)
    {
        rt_uint32_t start_cycles;
        rt_uint32_t elapsed;
        if ((index & 1U) == 0U)
        {
            start_cycles = usb_perf_now();
            bitwise_crc = web_snapshot_crc32_bitwise_oracle(data, length);
            elapsed = usb_perf_now() - start_cycles;
            bitwise_cycles[index] = elapsed;
            start_cycles = usb_perf_now();
            mongoose_crc = web_snapshot_crc32(data, length);
            elapsed = usb_perf_now() - start_cycles;
            mongoose_cycles[index] = elapsed;
        }
        else
        {
            start_cycles = usb_perf_now();
            mongoose_crc = web_snapshot_crc32(data, length);
            elapsed = usb_perf_now() - start_cycles;
            mongoose_cycles[index] = elapsed;
            start_cycles = usb_perf_now();
            bitwise_crc = web_snapshot_crc32_bitwise_oracle(data, length);
            elapsed = usb_perf_now() - start_cycles;
            bitwise_cycles[index] = elapsed;
        }
    }
    rt_mutex_release(&g_snapshot_lock);

    if (bitwise_crc != mongoose_crc)
    {
        rt_kprintf("[web_crc_bench] FAIL crc mismatch old=%08lX new=%08lX\r\n",
                   (unsigned long)bitwise_crc,
                   (unsigned long)mongoose_crc);
        return;
    }

    bitwise_median = web_snapshot_crc32_median(bitwise_cycles);
    mongoose_median = web_snapshot_crc32_median(mongoose_cycles);
    saved_permille = bitwise_median == 0U ? 0U :
        (bitwise_median > mongoose_median ?
            (rt_uint32_t)(((rt_uint64_t)(bitwise_median - mongoose_median) *
                           1000U) / bitwise_median) : 0U);
    rt_kprintf("[web_crc_bench] PASS bytes=%lu old=%lu new=%lu save_pm=%lu crc=%08lX\r\n",
               (unsigned long)length, (unsigned long)bitwise_median,
               (unsigned long)mongoose_median, (unsigned long)saved_permille,
               (unsigned long)mongoose_crc);
}
MSH_CMD_EXPORT(web_snapshot_crc_bench_cmd,
               compare CRC implementations on current published JSON);

static void evidence_api_selftest_cmd(void)
{
    static const rt_uint8_t crc32_check[] = "123456789";
    rt_uint32_t crc_before;
    rt_uint32_t crc_after;
    rt_size_t index;
    int length;
    rt_uint8_t failures = 0U;

    if (rt_mutex_take(&g_json_build_lock, RT_WAITING_FOREVER) != RT_EOK)
    {
        rt_kprintf("[evidence_api_test] busy\r\n");
        return;
    }

    if (web_snapshot_crc32(crc32_check, sizeof(crc32_check) - 1U) !=
        0xCBF43926U)
    {
        failures++;
        rt_kprintf("[evidence_api_test] stage=crc32_vector\r\n");
    }

    {
        static const rt_uint8_t escape_input[] =
            { '"', '\\', '\n', '\r', '\t', 0x01U, 'A',
              0xC3U, 0xA9U, 0xC0U, 0xA0U, 0U };
        static const char escape_expected[] =
            "\"\\\"\\\\\\n\\r\\t\\u0001A\xC3\xA9\\u00C0\\u00A0\"";
        char *cursor = g_json_build;
        rt_size_t remaining = sizeof(g_json_build);

        g_json_overflow = RT_FALSE;
        g_json_build[0] = '\0';
        json_append_string(&cursor, &remaining,
                           (const char *)escape_input);
        if (g_json_overflow ||
            rt_strcmp(g_json_build, escape_expected) != 0)
        {
            failures++;
            rt_kprintf("[evidence_api_test] stage=json_string_escape\r\n");
        }
    }

    rt_memset(&g_evidence_build, 0, sizeof(g_evidence_build));
    g_evidence_build.schema_version = USB_EVIDENCE_SCHEMA_VERSION;
    rt_strncpy(g_evidence_build.phase, "empty fixture",
               sizeof(g_evidence_build.phase) - 1U);
    web_snapshot_sanitize(&g_evidence_build);
    length = web_snapshot_rebuild_schema(&g_evidence_build, g_json_build);
    if (length <= 0 || g_json_build[length - 1] != '}' ||
        rt_strstr(g_json_build, "\"schema_version\":2") == RT_NULL)
    {
        failures++;
        rt_kprintf("[evidence_api_test] stage=empty length=%d overflow=%d\r\n",
                   length, g_json_overflow ? 1 : 0);
    }

    crc_before = usb_evidence_snapshot_crc32(&g_evidence_build);
    g_evidence_build.reserved = 0xA55AU;
    g_evidence_build.content_crc32 = 0xDEADBEEFU;
    g_evidence_build.raw_events[USB_EVIDENCE_SNAPSHOT_EVENT_COUNT - 1U]
        .raw_response[0] = 0x5AU;
    crc_after = usb_evidence_snapshot_crc32(&g_evidence_build);
    if (crc_before != crc_after)
    {
        failures++;
        rt_kprintf("[evidence_api_test] stage=canonical_crc\r\n");
    }

    g_evidence_build.raw_event_count = 1U;
    g_evidence_build.raw_events[0].captured_length = 1U;
    g_evidence_build.raw_events[0].raw_response[0] = 0x12U;
    g_evidence_build.raw_events[0].payload_crc32 =
        web_snapshot_crc32(g_evidence_build.raw_events[0].raw_response, 1U);
    crc_before = usb_evidence_snapshot_crc32(&g_evidence_build);
    g_evidence_build.raw_events[0].raw_response[0] = 0x13U;
    g_evidence_build.raw_events[0].payload_crc32 =
        web_snapshot_crc32(g_evidence_build.raw_events[0].raw_response, 1U);
    crc_after = usb_evidence_snapshot_crc32(&g_evidence_build);
    if (crc_before == crc_after)
    {
        failures++;
        rt_kprintf("[evidence_api_test] stage=active_payload_crc\r\n");
    }

    rt_memset(&g_evidence_build, 0, sizeof(g_evidence_build));
    g_evidence_build.schema_version = USB_EVIDENCE_SCHEMA_VERSION;
    g_evidence_build.raw_event_count = USB_EVIDENCE_SNAPSHOT_EVENT_COUNT;
    g_evidence_build.descriptor_node_count =
        USB_EVIDENCE_DESCRIPTOR_NODE_COUNT;
    g_evidence_build.descriptor_field_count =
        USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT;
    g_evidence_build.timeline_count = USB_EVIDENCE_TIMELINE_COUNT;
    for (index = 0U; index < g_evidence_build.raw_event_count; index++)
    {
        usb_evidence_raw_event_t *event = &g_evidence_build.raw_events[index];
        event->sequence = (rt_uint32_t)index + 1U;
        event->event_type = USB_EVIDENCE_EVENT_CONTROL_TRANSFER;
        event->captured_length = USB_EVIDENCE_CAPTURE_MAX;
        event->transfer_length = USB_EVIDENCE_CAPTURE_MAX;
        event->capture_status = USB_EVIDENCE_CAPTURE_COMPLETE;
    }
    for (index = 0U; index < g_evidence_build.descriptor_node_count; index++)
    {
        usb_evidence_descriptor_node_t *node =
            &g_evidence_build.descriptor_nodes[index];
        node->node_id = (rt_uint16_t)index;
        node->parent_id = USB_EVIDENCE_NODE_NONE;
        node->field_start = (rt_uint16_t)(index * 2U);
        node->field_count = 2U;
        rt_strncpy(node->type_name, "Fixture Descriptor",
                   sizeof(node->type_name) - 1U);
        rt_strncpy(node->summary, "Synthetic maximum-size descriptor node",
                   sizeof(node->summary) - 1U);
    }
    for (index = 0U; index < g_evidence_build.descriptor_field_count; index++)
    {
        usb_evidence_descriptor_field_t *field =
            &g_evidence_build.descriptor_fields[index];
        field->node_id = (rt_uint16_t)(index / 2U);
        rt_strncpy(field->name, "Fixture Field",
                   sizeof(field->name) - 1U);
        rt_memset(field->value, 'X', sizeof(field->value) - 1U);
    }
    for (index = 0U; index < g_evidence_build.timeline_count; index++)
    {
        rt_strncpy(g_evidence_build.timeline[index].label,
                   "Maximum fixture timeline label",
                   sizeof(g_evidence_build.timeline[index].label) - 1U);
    }
    web_snapshot_sanitize(&g_evidence_build);
    length = web_snapshot_rebuild_schema(&g_evidence_build, g_json_build);
    if (length <= 0 || (rt_uint32_t)length >= WEB_SNAPSHOT_JSON_SIZE ||
        g_json_build[length - 1] != '}' ||
        rt_strstr(g_json_build, "\"started_at_us\":") == RT_NULL ||
        rt_strstr(g_json_build, "\"completed_at_us\":") == RT_NULL ||
        rt_strstr(g_json_build, "\"attempt_id\":") == RT_NULL ||
        rt_strstr(g_json_build, "\"status\":\"WAITING\"") == RT_NULL)
    {
        failures++;
        rt_kprintf("[evidence_api_test] stage=max_fixture length=%d overflow=%d "
                   "started=%d completed=%d attempt=%d timeline=%d\r\n",
                   length, g_json_overflow ? 1 : 0,
                   rt_strstr(g_json_build, "\"started_at_us\":") != RT_NULL,
                   rt_strstr(g_json_build, "\"completed_at_us\":") != RT_NULL,
                   rt_strstr(g_json_build, "\"attempt_id\":") != RT_NULL,
                   rt_strstr(g_json_build, "\"status\":\"WAITING\"") != RT_NULL);
    }

    /* Counts and captured lengths are clamped before serialization. */
    g_evidence_build.raw_events[0].captured_length =
        USB_EVIDENCE_CAPTURE_MAX + 1U;
    g_evidence_build.raw_events[0].capture_status =
        USB_EVIDENCE_CAPTURE_COMPLETE;
    web_snapshot_sanitize(&g_evidence_build);
    if (g_evidence_build.raw_events[0].captured_length !=
            USB_EVIDENCE_CAPTURE_MAX ||
        g_evidence_build.raw_events[0].capture_status !=
            USB_EVIDENCE_CAPTURE_LOCAL_TRUNCATED)
    {
        failures++;
        rt_kprintf("[evidence_api_test] stage=truncate captured=%u status=%u\r\n",
                   g_evidence_build.raw_events[0].captured_length,
                   (unsigned int)g_evidence_build.raw_events[0].capture_status);
    }

    rt_mutex_release(&g_json_build_lock);
    rt_kprintf("[evidence_api_test] %s failures=%u "
               "(empty/schema, canonical CRC, active-byte CRC, max fixture/fields, truncation)\r\n",
               failures == 0U ? "PASS" : "FAIL", (unsigned int)failures);
}
MSH_CMD_EXPORT(evidence_api_selftest_cmd, run isolated evidence schema and CRC checks);
#endif

static int web_snapshot_init(void)
{
    if (rt_mutex_init(&g_json_build_lock, "snapb", RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        return -RT_ERROR;
    }
    g_json_build_lock_ready = RT_TRUE;
    if (rt_mutex_init(&g_snapshot_lock, "snap", RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        return -RT_ERROR;
    }
    g_snapshot_lock_ready = RT_TRUE;
    web_snapshot_reset();
    return 0;
}
INIT_APP_EXPORT(web_snapshot_init);
