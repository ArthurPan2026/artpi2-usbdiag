/*
 * Immutable USB enumeration evidence and rule-analysis interface.
 *
 * The transfer observer is a producer only: it copies a bounded record and
 * never parses descriptors, renders the UI, or waits for network activity.
 */
#ifndef USB_EVIDENCE_H
#define USB_EVIDENCE_H

#include <rtthread.h>
#include "usb_config.h"

#define USB_EVIDENCE_SCHEMA_VERSION          2U
#define USB_EVIDENCE_PARSER_VERSION          2U
#define USB_EVIDENCE_RULESET_VERSION         3U
#define USB_EVIDENCE_EVENT_CAPACITY          128U
/* A successful USB control transfer is retained in full up to the same
 * bounded size used by CherryUSB's non-cacheable EP0 request buffer.  The
 * complete event records live in PSRAM; this is no longer a 512-byte preview
 * masquerading as the authoritative evidence. */
#define USB_EVIDENCE_CAPTURE_MAX             CONFIG_USBHOST_REQUEST_BUFFER_LEN
#define USB_EVIDENCE_RULE_COUNT              12U
#define USB_EVIDENCE_SNAPSHOT_EVENT_COUNT    64U
#define USB_EVIDENCE_RULE_NOTE_MAX           112U
#define USB_EVIDENCE_EXPECTED_MAX            64U
#define USB_EVIDENCE_REASON_CODE_MAX         32U
#define USB_EVIDENCE_REFERENCE_COUNT         4U
#define USB_EVIDENCE_REFERENCE_ROLE_MAX      32U
#define USB_EVIDENCE_DESCRIPTOR_NODE_COUNT   64U
#define USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT  128U
#define USB_EVIDENCE_TIMELINE_COUNT          8U
#define USB_EVIDENCE_NODE_NONE               0xFFFFU

typedef enum
{
    USB_EVIDENCE_EVENT_PORT_RESET,
    USB_EVIDENCE_EVENT_CONTROL_TRANSFER,
    USB_EVIDENCE_EVENT_ENUMERATION_COMPLETE,
    USB_EVIDENCE_EVENT_DISCONNECT,
} usb_evidence_event_type_t;

typedef enum
{
    USB_EVIDENCE_CAPTURE_NONE,
    USB_EVIDENCE_CAPTURE_COMPLETE,
    USB_EVIDENCE_CAPTURE_LOCAL_TRUNCATED,
    USB_EVIDENCE_CAPTURE_NO_DATA_STAGE,
    USB_EVIDENCE_CAPTURE_UNAVAILABLE,
} usb_evidence_capture_status_t;

/* Compatibility name retained while in-tree consumers move to schema v1. */
#define USB_EVIDENCE_CAPTURE_TRUNCATED USB_EVIDENCE_CAPTURE_LOCAL_TRUNCATED

typedef enum
{
    USB_EVIDENCE_DATA_NONE,
    USB_EVIDENCE_DATA_DEVICE_TO_HOST,
    USB_EVIDENCE_DATA_HOST_TO_DEVICE,
} usb_evidence_data_direction_t;

typedef enum
{
    USB_RULE_PASS,
    USB_RULE_FAIL,
    USB_RULE_NOT_OBSERVED,
    USB_RULE_NOT_APPLICABLE,
} usb_rule_verdict_t;

typedef enum
{
    USB_RULE_SEVERITY_INFO,
    USB_RULE_SEVERITY_WARNING,
    USB_RULE_SEVERITY_ERROR,
} usb_rule_severity_t;

typedef enum
{
    USB_EVIDENCE_SESSION_WAITING,
    USB_EVIDENCE_SESSION_RECORDING,
    USB_EVIDENCE_SESSION_ANALYZING,
    USB_EVIDENCE_SESSION_FINAL,
    USB_EVIDENCE_SESSION_DEGRADED,
    USB_EVIDENCE_SESSION_FAILED,
} usb_evidence_session_state_t;

typedef enum
{
    USB_EVIDENCE_INTEGRITY_COMPLETE,
    USB_EVIDENCE_INTEGRITY_DEGRADED,
    USB_EVIDENCE_INTEGRITY_UNAVAILABLE,
} usb_evidence_integrity_t;

typedef enum
{
    USB_EVIDENCE_TIMELINE_WAITING,
    USB_EVIDENCE_TIMELINE_OBSERVED,
    USB_EVIDENCE_TIMELINE_FAILED,
} usb_evidence_timeline_status_t;

typedef enum
{
    USB_EVIDENCE_STAGE_PORT_RESET,
    USB_EVIDENCE_STAGE_DEVICE_8B,
    USB_EVIDENCE_STAGE_SET_ADDRESS,
    USB_EVIDENCE_STAGE_DEVICE_18B,
    USB_EVIDENCE_STAGE_CONFIGURATION,
    USB_EVIDENCE_STAGE_STRINGS,
    USB_EVIDENCE_STAGE_SET_CONFIGURATION,
    USB_EVIDENCE_STAGE_ENUM_COMPLETE,
} usb_evidence_timeline_stage_t;

typedef struct
{
    rt_uint8_t bm_request_type;
    rt_uint8_t b_request;
    rt_uint16_t w_value;
    rt_uint16_t w_index;
    rt_uint16_t w_length;
} usb_evidence_setup_t;

typedef struct
{
    rt_uint32_t session_id;
    rt_uint32_t attempt_id;
    rt_uint32_t sequence;
    /* Monotonic Host-observation time relative to evidence start. */
    rt_uint32_t timestamp_us;
    rt_uint32_t started_at_us;
    rt_uint32_t completed_at_us;
    rt_uint32_t duration_us;
    usb_evidence_event_type_t event_type;
    rt_uint8_t port;
    rt_bool_t port_valid;
    rt_bool_t timing_valid;
    rt_uint8_t speed;
    rt_bool_t has_setup;
    usb_evidence_setup_t setup_packet;
    rt_int32_t return_status;
    rt_bool_t transfer_length_known;
    /* Actual bytes returned by the Host transfer, independent of capture. */
    rt_uint16_t transfer_length;
    usb_evidence_data_direction_t data_direction;
    /* Bytes copied into raw_response; never exceeds CAPTURE_MAX. */
    rt_uint16_t captured_length;
    rt_uint32_t payload_crc32;
    /* Per-session values sampled before the control event is queued. */
    rt_uint32_t session_dropped_event_count;
    rt_uint32_t session_dropped_payload_bytes;
    usb_evidence_capture_status_t capture_status;
    rt_uint8_t raw_response[USB_EVIDENCE_CAPTURE_MAX];
} usb_evidence_raw_event_t;

typedef struct
{
    rt_uint32_t event_sequence;
    rt_uint16_t byte_offset;
    rt_uint16_t byte_length;
    char role[USB_EVIDENCE_REFERENCE_ROLE_MAX];
} usb_evidence_reference_t;

typedef struct
{
    const char *rule_id;
    const char *spec_clause;
    usb_rule_severity_t severity;
    usb_rule_verdict_t verdict;
    /* Kept as the primary reference for compact legacy projections. */
    rt_uint32_t event_sequence;
    char expected[USB_EVIDENCE_EXPECTED_MAX];
    char observed[USB_EVIDENCE_RULE_NOTE_MAX];
    char reason_code[USB_EVIDENCE_REASON_CODE_MAX];
    rt_uint8_t evidence_ref_count;
    rt_uint16_t omitted_failure_reference_count;
    usb_evidence_reference_t evidence_refs[USB_EVIDENCE_REFERENCE_COUNT];
} usb_evidence_rule_result_t;

typedef struct
{
    rt_uint16_t node_id;
    rt_uint16_t parent_id;
    rt_uint32_t source_sequence;
    rt_uint16_t source_offset;
    rt_uint16_t byte_length;
    rt_uint16_t field_start;
    rt_uint8_t field_count;
    rt_uint8_t depth;
    rt_uint8_t descriptor_type;
    rt_bool_t truncated;
    char type_name[16];
    char summary[40];
} usb_evidence_descriptor_node_t;

typedef struct
{
    rt_uint16_t node_id;
    rt_uint8_t byte_offset;
    rt_uint8_t byte_length;
    char name[32];
    char value[128];
} usb_evidence_descriptor_field_t;

typedef struct
{
    usb_evidence_timeline_stage_t stage;
    usb_evidence_timeline_status_t status;
    rt_uint32_t event_sequence;
    rt_uint32_t timestamp_us;
    char label[24];
} usb_evidence_timeline_item_t;

typedef struct
{
    rt_uint16_t schema_version;
    rt_uint16_t parser_version;
    rt_uint16_t ruleset_version;
    rt_uint16_t reserved;
    rt_uint32_t boot_id;
    rt_uint32_t snapshot_id;
    rt_uint32_t session_id;
    rt_uint32_t attempt_id;
    rt_uint32_t revision;
    rt_uint32_t last_sequence;
    rt_uint32_t last_raw_sequence;
    rt_uint32_t analyzed_through_sequence;
    rt_uint32_t published_at_us;
    rt_uint32_t content_crc32;
    rt_uint32_t dropped_event_count;
    rt_uint32_t dropped_payload_bytes;
    /* Session-local losses are distinct from the boot-total counters above. */
    rt_uint32_t session_dropped_event_count;
    rt_uint32_t session_dropped_payload_bytes;
    rt_uint32_t raw_event_overflow_count;
    usb_evidence_session_state_t session_state;
    usb_evidence_integrity_t capture_integrity;
    rt_bool_t is_final;
    rt_uint8_t speed;
    rt_bool_t enumeration_complete;
    rt_bool_t device_present;
    rt_uint16_t vendor_id;
    rt_uint16_t product_id;
    rt_uint16_t bcd_usb;
    rt_uint16_t bcd_device;
    char manufacturer[32];
    char product[32];
    char serial[128];  /* full serial strings can be 120+ chars (SanDisk) */
    char class_name[32];
    char phase[24];
    rt_uint8_t raw_event_count;
    rt_uint8_t descriptor_node_count;
    rt_uint8_t descriptor_field_count;
    rt_uint8_t timeline_count;
    usb_evidence_raw_event_t raw_events[USB_EVIDENCE_SNAPSHOT_EVENT_COUNT];
    usb_evidence_descriptor_node_t
        descriptor_nodes[USB_EVIDENCE_DESCRIPTOR_NODE_COUNT];
    usb_evidence_descriptor_field_t
        descriptor_fields[USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT];
    usb_evidence_timeline_item_t timeline[USB_EVIDENCE_TIMELINE_COUNT];
    usb_evidence_rule_result_t rules[USB_EVIDENCE_RULE_COUNT];
} usb_evidence_snapshot_t;

#ifdef __cplusplus
extern "C" {
#endif

int usb_evidence_start(void);

#if defined(__arm__) && defined(WORK_EVIDENCE_DIAGNOSTICS) && \
    WORK_EVIDENCE_DIAGNOSTICS
/* Debug-only scheduler accounting. The startup coordinator owns the single
 * RT-Thread scheduler hook and forwards its switches here. */
void usb_evidence_scheduler_profile_enable(rt_bool_t enabled);
void usb_evidence_scheduler_profile_switch(rt_thread_t from,
                                           rt_thread_t to,
                                           rt_uint32_t cycle);
#endif

void usb_evidence_record_port_reset(rt_uint8_t port, rt_uint8_t speed);
void usb_evidence_record_control(rt_uint8_t speed,
                                 rt_uint8_t port,
                                 rt_uint32_t started_at_us,
                                 rt_uint32_t completed_at_us,
                                 rt_bool_t timing_valid,
                                 const usb_evidence_setup_t *setup,
                                 rt_int32_t return_status,
                                 const rt_uint8_t *response);
rt_bool_t usb_evidence_clock_us(rt_uint32_t *timestamp_us);
void usb_evidence_record_enumeration_complete(rt_int32_t return_status);
void usb_evidence_record_disconnect(void);
void usb_evidence_set_attempt_id(rt_uint32_t attempt_id);

/* Returns false when the analysis thread is publishing a snapshot. The read
 * side is deliberately non-blocking so the LVGL display loop never waits on
 * USB analysis; callers retry on their next loop. */
rt_bool_t usb_evidence_get_snapshot(usb_evidence_snapshot_t *snapshot);
rt_bool_t usb_evidence_get_snapshot_meta(rt_uint32_t *session_id,
                                         rt_uint32_t *revision);
rt_uint32_t usb_evidence_snapshot_crc32(
    const usb_evidence_snapshot_t *snapshot);
rt_uint32_t usb_evidence_bytes_crc32(const void *data, rt_size_t length);

const char *usb_evidence_session_state_name(usb_evidence_session_state_t state);
const char *usb_evidence_integrity_name(usb_evidence_integrity_t integrity);
const char *usb_evidence_capture_status_name(
    usb_evidence_capture_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* USB_EVIDENCE_H */
