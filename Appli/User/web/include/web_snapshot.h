#ifndef WEB_SNAPSHOT_H
#define WEB_SNAPSHOT_H

#include <rtthread.h>
#include "usb_evidence.h"

#define WEB_SNAPSHOT_JSON_SIZE 131072U
#define WEB_SNAPSHOT_PAGE_SIZE 4096U
#define WEB_SNAPSHOT_TOKEN_SIZE 32U
#define WEB_SNAPSHOT_ERR_STALE 1001
#define WEB_SNAPSHOT_ERR_RANGE  1002

typedef struct
{
    char token[WEB_SNAPSHOT_TOKEN_SIZE];
    rt_uint32_t boot_id;
    rt_uint32_t snapshot_id;
    rt_uint32_t revision;
    rt_uint32_t last_sequence;
    rt_uint32_t content_crc32;
    rt_uint32_t json_crc32;
    rt_uint32_t total_bytes;
    rt_uint8_t raw_event_count;
    rt_bool_t is_final;
} web_snapshot_manifest_t;

/*
 * Small immutable view used by the board-side PDF worker. It deliberately
 * excludes USB raw payloads and the device serial string; the full snapshot
 * remains available through the token/CRC-checked evidence APIs.
 */
typedef struct
{
    rt_uint32_t sequence;
    rt_uint32_t timestamp_us;
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
    rt_uint16_t transfer_length;
    rt_uint16_t captured_length;
    usb_evidence_capture_status_t capture_status;
    rt_uint32_t payload_crc32;
} web_snapshot_pdf_event_t;

typedef struct
{
    char token[WEB_SNAPSHOT_TOKEN_SIZE];
    rt_uint32_t boot_id;
    rt_uint32_t snapshot_id;
    rt_uint32_t session_id;
    rt_uint32_t attempt_id;
    rt_uint32_t revision;
    rt_uint32_t last_sequence;
    rt_uint32_t content_crc32;
    rt_uint32_t json_crc32;
    rt_uint32_t dropped_event_count;
    rt_uint32_t dropped_payload_bytes;
    rt_uint32_t session_dropped_event_count;
    rt_uint32_t session_dropped_payload_bytes;
    rt_uint32_t raw_event_overflow_count;
    rt_uint16_t schema_version;
    rt_uint16_t parser_version;
    rt_uint16_t ruleset_version;
    usb_evidence_session_state_t session_state;
    usb_evidence_integrity_t capture_integrity;
    rt_bool_t is_final;
    rt_bool_t device_present;
    rt_uint8_t speed;
    rt_uint16_t vendor_id;
    rt_uint16_t product_id;
    rt_uint16_t bcd_usb;
    rt_uint16_t bcd_device;
    char manufacturer[32];
    char product[32];
    char class_name[32];
    char phase[24];
    rt_uint8_t timeline_count;
    usb_evidence_timeline_item_t timeline[USB_EVIDENCE_TIMELINE_COUNT];
    usb_evidence_rule_result_t rules[USB_EVIDENCE_RULE_COUNT];
    rt_uint8_t event_count;
    web_snapshot_pdf_event_t events[USB_EVIDENCE_SNAPSHOT_EVENT_COUNT];
} web_snapshot_pdf_view_t;

#ifdef __cplusplus
extern "C" {
#endif

int web_snapshot_update_evidence(const usb_evidence_snapshot_t *snapshot);
void web_snapshot_reset(void);
rt_bool_t web_snapshot_get_copy(usb_evidence_snapshot_t *snapshot);
rt_bool_t web_snapshot_get_meta(rt_uint32_t *session_id,
                                rt_uint32_t *revision);
int web_snapshot_get_manifest(web_snapshot_manifest_t *manifest);
int web_snapshot_get_pdf_view(const char *token,
                              web_snapshot_pdf_view_t *view);
int web_snapshot_get_range(const char *token, rt_uint32_t offset,
                           rt_uint8_t *buffer, rt_size_t buffer_size,
                           rt_uint32_t *total_length);
int web_snapshot_get_event_payload(const char *token,
                                   rt_uint32_t sequence,
                                   rt_uint32_t offset,
                                   rt_uint8_t *buffer,
                                   rt_size_t buffer_size,
                                   rt_uint32_t *total_length,
                                   usb_evidence_capture_status_t *status,
                                   rt_uint32_t *payload_crc32);
int web_snapshot_get_history(char *buffer, rt_size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif
