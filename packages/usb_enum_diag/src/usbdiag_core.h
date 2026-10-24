/* SPDX-License-Identifier: Apache-2.0 */
#ifndef USB_DIAG_CORE_H
#define USB_DIAG_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USBDIAG_REFERENCE_CAPACITY 4U

typedef enum
{
    USBDIAG_SPEED_LOW = 1,
    USBDIAG_SPEED_FULL = 2,
    USBDIAG_SPEED_HIGH = 3
} usbdiag_speed_t;

typedef enum
{
    USBDIAG_NOT_OBSERVED = 0,
    USBDIAG_PASS,
    USBDIAG_FAIL,
    USBDIAG_NOT_APPLICABLE
} usbdiag_verdict_t;

typedef struct
{
    uint16_t offsets[USBDIAG_REFERENCE_CAPACITY];
    uint8_t count;
    uint16_t total_failures;
} usbdiag_failure_refs_t;

typedef enum
{
    USBDIAG_CFG_HEADER = 0,
    USBDIAG_CFG_TOTAL_LENGTH,
    USBDIAG_CFG_TREE_WALK,
    USBDIAG_CFG_INTERFACE_COUNT,
    USBDIAG_CFG_ENDPOINT_COUNT,
    USBDIAG_CFG_ENDPOINT_PACKET_SIZE,
    USBDIAG_CFG_ENDPOINT_INTERVAL,
    USBDIAG_CFG_RULE_COUNT
} usbdiag_config_rule_id_t;

typedef struct
{
    usbdiag_verdict_t verdict;
    usbdiag_failure_refs_t failures;
} usbdiag_config_rule_result_t;

typedef struct
{
    usbdiag_config_rule_result_t rules[USBDIAG_CFG_RULE_COUNT];
    uint16_t declared_total_length;
    uint16_t unique_interface_count;
    uint16_t interface_setting_count;
    uint16_t endpoint_count;
} usbdiag_config_result_t;

bool usbdiag_device_b_length_valid(uint8_t value);
bool usbdiag_device_descriptor_type_valid(uint8_t value);
bool usbdiag_device_prefix_equal(const uint8_t *prefix,
                                 const uint8_t *descriptor,
                                 uint16_t length);
bool usbdiag_device_ep0_mps_valid(uint8_t speed, uint8_t mps0);
bool usbdiag_config_header_valid(const uint8_t *data, uint16_t length);
usbdiag_verdict_t usbdiag_config_transfer_verdict(uint16_t requested_length,
                                                  bool transfer_length_known,
                                                  uint16_t transfer_length,
                                                  uint16_t declared_length);
usbdiag_verdict_t usbdiag_ep0_required_request_verdict(bool required_request,
                                                       bool completed,
                                                       bool stalled);
bool usbdiag_speed_known(uint8_t speed);
bool usbdiag_endpoint_packet_size_valid(uint8_t speed,
                                        const uint8_t endpoint_descriptor[7]);
bool usbdiag_endpoint_interval_valid(uint8_t speed,
                                     uint8_t transfer_type,
                                     uint8_t interval);
void usbdiag_failure_ref_add(usbdiag_failure_refs_t *refs, uint16_t offset);
void usbdiag_analyze_configuration(const uint8_t *data,
                                   uint16_t captured_length,
                                   bool capture_complete,
                                   uint16_t requested_length,
                                   bool transfer_length_known,
                                   uint16_t transfer_length,
                                   uint8_t speed,
                                   usbdiag_config_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
