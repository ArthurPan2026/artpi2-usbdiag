/* SPDX-License-Identifier: Apache-2.0 */
#ifndef USB_DIAG_SESSION_H
#define USB_DIAG_SESSION_H

#include "usbdiag_core.h"
#include "usbdiag_version.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USBDIAG_RULE_COUNT             12U
#define USBDIAG_SESSION_EVENT_CAPACITY 128U
#define USBDIAG_SESSION_CONFIG_CAPACITY 128U

typedef enum
{
    USBDIAG_RULE_DEV_B_LENGTH = 0,
    USBDIAG_RULE_DEV_TYPE,
    USBDIAG_RULE_DEV_8B_PREFIX,
    USBDIAG_RULE_DEV_FIELDS,
    USBDIAG_RULE_CFG_B_LENGTH,
    USBDIAG_RULE_CFG_TOTAL_LENGTH,
    USBDIAG_RULE_CFG_TREE_WALK,
    USBDIAG_RULE_CFG_INTERFACE_COUNT,
    USBDIAG_RULE_IF_ENDPOINT_COUNT,
    USBDIAG_RULE_EP_PACKET_SIZE,
    USBDIAG_RULE_EP_INTERVAL,
    USBDIAG_RULE_EP0_STALL
} usbdiag_rule_id_t;

typedef enum
{
    USBDIAG_TRANSFER_SUCCESS = 0,
    USBDIAG_TRANSFER_STALL,
    USBDIAG_TRANSFER_ERROR
} usbdiag_transfer_status_t;

typedef enum
{
    USBDIAG_CAPTURE_COMPLETE = 0,
    USBDIAG_CAPTURE_TRUNCATED,
    USBDIAG_CAPTURE_NO_DATA,
    USBDIAG_CAPTURE_UNAVAILABLE
} usbdiag_capture_status_t;

typedef enum
{
    USBDIAG_SESSION_NOT_FINAL = 0,
    USBDIAG_SESSION_COMPLETE,
    USBDIAG_SESSION_RULE_FAILURE,
    USBDIAG_SESSION_ENUMERATION_FAILED,
    USBDIAG_SESSION_INCOMPLETE,
    USBDIAG_SESSION_DEGRADED,
    USBDIAG_SESSION_CAPACITY_EXCEEDED,
    USBDIAG_SESSION_INVALID
} usbdiag_session_status_t;

typedef enum
{
    USBDIAG_REASON_NONE = 0,
    USBDIAG_REASON_MATCH,
    USBDIAG_REASON_MISMATCH,
    USBDIAG_REASON_NOT_OBSERVED,
    USBDIAG_REASON_NOT_APPLICABLE,
    USBDIAG_REASON_EVIDENCE_MISSING,
    USBDIAG_REASON_TRANSFER_ERROR,
    USBDIAG_REASON_UNSUPPORTED_SPEED,
    USBDIAG_REASON_INVALID_INPUT,
    USBDIAG_REASON_CAPACITY_EXCEEDED
} usbdiag_reason_t;

typedef enum
{
    USBDIAG_REF_OBSERVATION = 0,
    USBDIAG_REF_DEVICE_PREFIX_FIRST,
    USBDIAG_REF_DEVICE_PREFIX_CURRENT,
    USBDIAG_REF_DESCRIPTOR_FIELD,
    USBDIAG_REF_CONFIG_HEADER,
    USBDIAG_REF_CONFIG_TOTAL_LENGTH,
    USBDIAG_REF_CONFIG_DESCRIPTOR,
    USBDIAG_REF_INTERFACE_COUNT,
    USBDIAG_REF_ENDPOINT_COUNT,
    USBDIAG_REF_ENDPOINT_PACKET_SIZE,
    USBDIAG_REF_ENDPOINT_INTERVAL,
    USBDIAG_REF_CONTROL_STATUS
} usbdiag_reference_role_t;

typedef struct
{
    uint8_t bm_request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} usbdiag_setup_packet_t;

typedef struct
{
    uint32_t session_id;
    uint32_t attempt_id;
    uint32_t sequence;
    uint8_t speed;
    bool has_setup;
    usbdiag_setup_packet_t setup;
    usbdiag_transfer_status_t transfer_status;
    usbdiag_capture_status_t capture_status;
    bool transfer_length_known;
    uint16_t transfer_length;
    uint16_t captured_length;
    const uint8_t *payload;
    uint16_t payload_size;
} usbdiag_control_event_t;

typedef struct
{
    uint32_t event_sequence;
    uint16_t byte_offset;
    uint16_t byte_length;
    usbdiag_reference_role_t role;
} usbdiag_reference_t;

typedef struct
{
    const char *id;
    const char *spec_clause;
    const char *expected_text;
} usbdiag_rule_definition_t;

typedef struct
{
    usbdiag_verdict_t verdict;
    usbdiag_reason_t reason;
    uint32_t event_sequence;
    uint8_t expected_count;
    uint8_t observed_count;
    uint32_t expected_values[3];
    uint32_t observed_values[3];
    uint8_t reference_count;
    uint16_t omitted_failure_reference_count;
    usbdiag_reference_t references[USBDIAG_REFERENCE_CAPACITY];
} usbdiag_rule_result_t;

typedef struct
{
    uint32_t session_id;
    uint32_t attempt_id;
    uint8_t speed;
    uint32_t last_sequence;
    uint16_t event_count;
    uint16_t configuration_count;
    uint16_t pending_configuration_count;
    usbdiag_session_status_t status;
    usbdiag_rule_result_t rules[USBDIAG_RULE_COUNT];
    uint64_t opaque_alignment;
    uint8_t opaque_state[4096];
} usbdiag_session_t;

const usbdiag_rule_definition_t *usbdiag_rule_definition(usbdiag_rule_id_t rule);
const char *usbdiag_version(void);
const char *usbdiag_reason_name(usbdiag_reason_t reason);
const char *usbdiag_reference_role_name(usbdiag_reference_role_t role);

usbdiag_session_status_t usbdiag_session_begin(usbdiag_session_t *session,
                                               uint32_t session_id,
                                               uint32_t attempt_id,
                                               uint8_t speed);
usbdiag_session_status_t usbdiag_session_feed(usbdiag_session_t *session,
                                              const usbdiag_control_event_t *event);
usbdiag_session_status_t usbdiag_session_mark_gap(usbdiag_session_t *session);
usbdiag_session_status_t usbdiag_session_forget_event(usbdiag_session_t *session,
                                                      uint32_t event_sequence);
usbdiag_session_status_t usbdiag_session_end(usbdiag_session_t *session,
                                             bool enumeration_succeeded);
usbdiag_session_status_t usbdiag_session_get_result(const usbdiag_session_t *session,
                                                    usbdiag_rule_result_t results[USBDIAG_RULE_COUNT]);
uint16_t usbdiag_session_get_pins(const usbdiag_session_t *session,
                                  uint32_t *event_sequences,
                                  uint16_t capacity);
uint32_t usbdiag_elapsed_us(uint32_t started_at_us, uint32_t completed_at_us);

#ifdef __cplusplus
}
#endif

#endif
