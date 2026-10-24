/* SPDX-License-Identifier: Apache-2.0 */
#include "usbdiag_session.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t device_descriptor[18] = {
    18, 1, 0x10, 0x02, 0, 0, 0, 64,
    0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 1, 2, 3, 1
};

static const uint8_t configuration_descriptor[25] = {
    9, 2, 25, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 3, 1, 1, 0,
    7, 5, 0x81, 3, 8, 0, 10
};

static usbdiag_control_event_t event_make(uint32_t session_id,
                                          uint32_t attempt_id,
                                          uint32_t sequence,
                                          uint8_t speed,
                                          uint8_t request,
                                          uint16_t value,
                                          uint16_t requested_length,
                                          const uint8_t *payload,
                                          uint16_t length)
{
    usbdiag_control_event_t event;
    memset(&event, 0, sizeof(event));
    event.session_id = session_id;
    event.attempt_id = attempt_id;
    event.sequence = sequence;
    event.speed = speed;
    event.has_setup = true;
    event.setup.bm_request_type = 0x80U;
    event.setup.request = request;
    event.setup.value = value;
    event.setup.length = requested_length;
    event.transfer_status = USBDIAG_TRANSFER_SUCCESS;
    event.capture_status = USBDIAG_CAPTURE_COMPLETE;
    event.transfer_length_known = true;
    event.transfer_length = length;
    event.captured_length = length;
    event.payload = payload;
    event.payload_size = length;
    return event;
}

static usbdiag_session_status_t feed_event(usbdiag_session_t *session,
                                           usbdiag_control_event_t *event)
{
    return usbdiag_session_feed(session, event);
}

static usbdiag_rule_result_t *rule(usbdiag_session_t *session,
                                   usbdiag_rule_id_t id)
{
    return &session->rules[id];
}

static void required_no_data(usbdiag_session_t *session,
                             uint32_t sequence,
                             uint8_t request,
                             uint16_t value,
                             usbdiag_transfer_status_t status)
{
    usbdiag_control_event_t event = event_make(session->session_id,
                                               session->attempt_id,
                                               sequence, USBDIAG_SPEED_FULL,
                                               request, value, 0U, NULL, 0U);
    event.setup.bm_request_type = request == 6U ? 0x80U : 0U;
    event.transfer_status = status;
    event.capture_status = USBDIAG_CAPTURE_NO_DATA;
    (void)feed_event(session, &event);
}

static void session_with_valid_enumeration(usbdiag_session_t *session,
                                           uint32_t session_id,
                                           uint32_t sequence)
{
    usbdiag_control_event_t event;
    usbdiag_session_status_t final_status;
    uint8_t rule_index;
    assert(usbdiag_session_begin(session, session_id, 1U,
                                 USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);

    event = event_make(session_id, 1U, sequence++, USBDIAG_SPEED_FULL,
                       6U, 0x0100U, 8U, device_descriptor, 8U);
    assert(feed_event(session, &event) == USBDIAG_SESSION_NOT_FINAL);

    event = event_make(session_id, 1U, sequence++, USBDIAG_SPEED_FULL,
                       6U, 0x0100U, 18U, device_descriptor,
                       sizeof(device_descriptor));
    (void)feed_event(session, &event);

    event = event_make(session_id, 1U, sequence++, USBDIAG_SPEED_FULL,
                       6U, 0x0200U, 9U, configuration_descriptor, 9U);
    (void)feed_event(session, &event);

    event = event_make(session_id, 1U, sequence++, USBDIAG_SPEED_FULL,
                       6U, 0x0200U, sizeof(configuration_descriptor),
                       configuration_descriptor,
                       sizeof(configuration_descriptor));
    (void)feed_event(session, &event);

    required_no_data(session, sequence++, 5U, 1U,
                     USBDIAG_TRANSFER_SUCCESS);
    required_no_data(session, sequence++, 9U, 1U,
                     USBDIAG_TRANSFER_SUCCESS);
    final_status = usbdiag_session_end(session, true);
    if (final_status != USBDIAG_SESSION_COMPLETE)
    {
        fprintf(stderr, "session %u final status %u, pending configurations %u\n",
                (unsigned)session_id, (unsigned)final_status,
                (unsigned)session->pending_configuration_count);
        for (rule_index = 0U; rule_index < USBDIAG_RULE_COUNT; rule_index++)
        {
            const usbdiag_rule_result_t *result = &session->rules[rule_index];
            fprintf(stderr, "rule %u verdict %u reason %u refs %u omitted %u\n",
                    (unsigned)rule_index, (unsigned)result->verdict,
                    (unsigned)result->reason, (unsigned)result->reference_count,
                    (unsigned)result->omitted_failure_reference_count);
        }
    }
    assert(final_status == USBDIAG_SESSION_COMPLETE);
}

static void test_complete_twelve_rule_session(void)
{
    usbdiag_session_t session;
    uint8_t index;

    session_with_valid_enumeration(&session, 1U, 100U);
    for (index = 0U; index < USBDIAG_RULE_COUNT; index++)
    {
        assert(rule(&session, (usbdiag_rule_id_t)index)->verdict ==
               USBDIAG_PASS ||
               rule(&session, (usbdiag_rule_id_t)index)->verdict ==
               USBDIAG_NOT_APPLICABLE);
        assert(rule(&session, (usbdiag_rule_id_t)index)->reference_count > 0U);
        assert(usbdiag_rule_definition((usbdiag_rule_id_t)index) != NULL);
    }
    assert(rule(&session, USBDIAG_RULE_DEV_B_LENGTH)->references[0].byte_offset == 0U);
    assert(rule(&session, USBDIAG_RULE_DEV_TYPE)->references[0].byte_offset == 1U);
    assert(rule(&session, USBDIAG_RULE_DEV_FIELDS)->references[0].byte_offset == 7U);
    assert(rule(&session, USBDIAG_RULE_CFG_INTERFACE_COUNT)->references[0].byte_offset == 4U);
    assert(usbdiag_reason_name(USBDIAG_REASON_MATCH)[0] == 'M');
}

static void test_each_individual_rule_failure(void)
{
    usbdiag_session_t session;
    usbdiag_control_event_t event;
    uint8_t bad_device[sizeof(device_descriptor)];
    uint8_t bad_config[sizeof(configuration_descriptor)];

    memcpy(bad_device, device_descriptor, sizeof(bad_device));
    bad_device[0] = 12U;
    assert(usbdiag_session_begin(&session, 2U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(2U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0100U, 18U, bad_device, sizeof(bad_device));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_DEV_B_LENGTH)->verdict == USBDIAG_FAIL);
    assert(rule(&session, USBDIAG_RULE_DEV_B_LENGTH)->references[0].byte_offset == 0U);

    memcpy(bad_device, device_descriptor, sizeof(bad_device));
    bad_device[1] = 2U;
    assert(usbdiag_session_begin(&session, 3U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(3U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0100U, 18U, bad_device, sizeof(bad_device));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_DEV_TYPE)->verdict == USBDIAG_FAIL);
    assert(rule(&session, USBDIAG_RULE_DEV_TYPE)->references[0].byte_offset == 1U);

    memcpy(bad_device, device_descriptor, sizeof(bad_device));
    bad_device[7] = 128U;
    assert(usbdiag_session_begin(&session, 4U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(4U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0100U, 18U, bad_device, sizeof(bad_device));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_DEV_FIELDS)->verdict == USBDIAG_FAIL);

    memcpy(bad_config, configuration_descriptor, sizeof(bad_config));
    bad_config[0] = 8U;
    assert(usbdiag_session_begin(&session, 5U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(5U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0200U, sizeof(bad_config), bad_config, sizeof(bad_config));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_CFG_B_LENGTH)->verdict == USBDIAG_FAIL);

    memcpy(bad_config, configuration_descriptor, sizeof(bad_config));
    event = event_make(5U, 1U, 2U, USBDIAG_SPEED_FULL, 6U,
                       0x0200U, sizeof(configuration_descriptor), bad_config,
                       sizeof(configuration_descriptor) - 1U);
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_CFG_TOTAL_LENGTH)->verdict == USBDIAG_FAIL);
    assert(rule(&session, USBDIAG_RULE_CFG_TOTAL_LENGTH)->references[0].byte_offset == 2U);

    memcpy(bad_config, configuration_descriptor, sizeof(bad_config));
    bad_config[9] = 0U;
    assert(usbdiag_session_begin(&session, 6U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(6U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0200U, sizeof(bad_config), bad_config, sizeof(bad_config));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_CFG_TREE_WALK)->verdict == USBDIAG_FAIL);

    memcpy(bad_config, configuration_descriptor, sizeof(bad_config));
    bad_config[4] = 2U;
    assert(usbdiag_session_begin(&session, 7U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(7U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0200U, sizeof(bad_config), bad_config, sizeof(bad_config));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_CFG_INTERFACE_COUNT)->verdict == USBDIAG_FAIL);
    assert(rule(&session, USBDIAG_RULE_CFG_INTERFACE_COUNT)->references[0].byte_offset == 4U);

    memcpy(bad_config, configuration_descriptor, sizeof(bad_config));
    bad_config[13] = 2U;
    assert(usbdiag_session_begin(&session, 8U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(8U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0200U, sizeof(bad_config), bad_config, sizeof(bad_config));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_IF_ENDPOINT_COUNT)->verdict == USBDIAG_FAIL);

    memcpy(bad_config, configuration_descriptor, sizeof(bad_config));
    bad_config[22] = 65U;
    assert(usbdiag_session_begin(&session, 9U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(9U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0200U, sizeof(bad_config), bad_config, sizeof(bad_config));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_EP_PACKET_SIZE)->verdict == USBDIAG_FAIL);

    memcpy(bad_config, configuration_descriptor, sizeof(bad_config));
    bad_config[24] = 0U;
    assert(usbdiag_session_begin(&session, 10U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(10U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0200U, sizeof(bad_config), bad_config, sizeof(bad_config));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_EP_INTERVAL)->verdict == USBDIAG_FAIL);

    assert(usbdiag_session_begin(&session, 11U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    required_no_data(&session, 1U, 6U, 0x0100U, USBDIAG_TRANSFER_STALL);
    assert(rule(&session, USBDIAG_RULE_EP0_STALL)->verdict == USBDIAG_FAIL);
}

static void test_prefix_witness_is_sticky(void)
{
    usbdiag_session_t session;
    uint8_t mismatched[sizeof(device_descriptor)];
    usbdiag_control_event_t event;
    uint32_t first_sequence;

    assert(usbdiag_session_begin(&session, 12U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(12U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0100U, 8U, device_descriptor, 8U);
    (void)feed_event(&session, &event);
    first_sequence = rule(&session, USBDIAG_RULE_DEV_B_LENGTH)->references[0].event_sequence;

    memcpy(mismatched, device_descriptor, sizeof(mismatched));
    mismatched[2] ^= 1U;
    event = event_make(12U, 1U, 2U, USBDIAG_SPEED_FULL, 6U,
                       0x0100U, 18U, mismatched, sizeof(mismatched));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_DEV_8B_PREFIX)->verdict == USBDIAG_FAIL);
    assert(rule(&session, USBDIAG_RULE_DEV_8B_PREFIX)->reference_count == 2U);
    assert(rule(&session, USBDIAG_RULE_DEV_8B_PREFIX)->references[0].event_sequence == first_sequence);
    assert(rule(&session, USBDIAG_RULE_DEV_8B_PREFIX)->references[1].event_sequence == 2U);

    event = event_make(12U, 1U, 3U, USBDIAG_SPEED_FULL, 6U,
                       0x0100U, 18U, device_descriptor, sizeof(device_descriptor));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_DEV_8B_PREFIX)->verdict == USBDIAG_FAIL);
    assert(rule(&session, USBDIAG_RULE_DEV_8B_PREFIX)->references[1].event_sequence == 2U);
}

static void test_missing_and_dropped_evidence_never_passes(void)
{
    usbdiag_session_t session;
    usbdiag_control_event_t event;
    uint32_t pins[USBDIAG_SESSION_EVENT_CAPACITY];
    uint16_t pin_count;

    assert(usbdiag_session_begin(&session, 13U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(13U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0100U, 8U, device_descriptor, 8U);
    (void)feed_event(&session, &event);
    pin_count = usbdiag_session_get_pins(&session, pins,
                                         USBDIAG_SESSION_EVENT_CAPACITY);
    assert(pin_count == 1U && pins[0] == 1U);
    assert(usbdiag_session_forget_event(&session, 1U) ==
           USBDIAG_SESSION_DEGRADED);
    event = event_make(13U, 1U, 2U, USBDIAG_SPEED_FULL, 6U,
                       0x0100U, 18U, device_descriptor, sizeof(device_descriptor));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_DEV_8B_PREFIX)->verdict ==
           USBDIAG_NOT_OBSERVED);
    assert(rule(&session, USBDIAG_RULE_DEV_8B_PREFIX)->reason ==
           USBDIAG_REASON_EVIDENCE_MISSING);
    assert(usbdiag_session_end(&session, true) == USBDIAG_SESSION_DEGRADED);

    assert(usbdiag_session_begin(&session, 14U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(14U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0100U, 8U, device_descriptor, 8U);
    event.capture_status = USBDIAG_CAPTURE_TRUNCATED;
    (void)feed_event(&session, &event);
    event = event_make(14U, 1U, 2U, USBDIAG_SPEED_FULL, 6U,
                       0x0100U, 18U, device_descriptor, sizeof(device_descriptor));
    (void)feed_event(&session, &event);
    assert(usbdiag_session_end(&session, true) == USBDIAG_SESSION_DEGRADED);

    assert(usbdiag_session_begin(&session, 15U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(15U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0100U, 18U, device_descriptor, sizeof(device_descriptor));
    (void)feed_event(&session, &event);
    assert(usbdiag_session_mark_gap(&session) == USBDIAG_SESSION_DEGRADED);
    assert(usbdiag_session_end(&session, true) == USBDIAG_SESSION_DEGRADED);
}

static void test_ep0_rules_and_optional_stall(void)
{
    usbdiag_session_t session;
    usbdiag_control_event_t event;

    assert(usbdiag_session_begin(&session, 16U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    required_no_data(&session, 1U, 6U, 0x0301U, USBDIAG_TRANSFER_STALL);
    assert(rule(&session, USBDIAG_RULE_EP0_STALL)->verdict ==
           USBDIAG_NOT_OBSERVED);
    assert(usbdiag_session_end(&session, true) == USBDIAG_SESSION_INCOMPLETE);

    assert(usbdiag_session_begin(&session, 17U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(17U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0301U, 0U, NULL, 0U);
    event.setup.bm_request_type = 0x80U;
    event.transfer_status = USBDIAG_TRANSFER_STALL;
    event.capture_status = USBDIAG_CAPTURE_NO_DATA;
    (void)feed_event(&session, &event);
    required_no_data(&session, 2U, 5U, 1U, USBDIAG_TRANSFER_SUCCESS);
    required_no_data(&session, 3U, 9U, 1U, USBDIAG_TRANSFER_SUCCESS);
    assert(usbdiag_session_end(&session, true) == USBDIAG_SESSION_INCOMPLETE);
    assert(rule(&session, USBDIAG_RULE_EP0_STALL)->verdict ==
           USBDIAG_PASS);

    assert(usbdiag_session_begin(&session, 18U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    required_no_data(&session, 1U, 6U, 0x0100U, USBDIAG_TRANSFER_SUCCESS);
    required_no_data(&session, 2U, 6U, 0x0200U, USBDIAG_TRANSFER_SUCCESS);
    required_no_data(&session, 3U, 5U, 1U, USBDIAG_TRANSFER_SUCCESS);
    required_no_data(&session, 4U, 9U, 1U, USBDIAG_TRANSFER_SUCCESS);
    assert(usbdiag_session_end(&session, true) == USBDIAG_SESSION_INCOMPLETE);
    assert(rule(&session, USBDIAG_RULE_EP0_STALL)->verdict == USBDIAG_PASS);
}

static void test_configuration_counts_and_applicability(void)
{
    usbdiag_session_t session;
    uint8_t no_endpoint_config[18] = {
        9, 2, 18, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 0, 3, 1, 1, 0
    };
    usbdiag_control_event_t event;

    assert(usbdiag_session_begin(&session, 19U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(19U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0200U, sizeof(no_endpoint_config), no_endpoint_config,
                       sizeof(no_endpoint_config));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_EP_PACKET_SIZE)->verdict ==
           USBDIAG_NOT_APPLICABLE);
    assert(rule(&session, USBDIAG_RULE_EP_INTERVAL)->verdict ==
           USBDIAG_NOT_APPLICABLE);
    assert(usbdiag_session_end(&session, true) == USBDIAG_SESSION_INCOMPLETE);

    assert(usbdiag_session_begin(&session, 20U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(20U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0200U, 9U, no_endpoint_config, 9U);
    (void)feed_event(&session, &event);
    assert(session.pending_configuration_count == 1U);
    assert(usbdiag_session_end(&session, true) == USBDIAG_SESSION_INCOMPLETE);
    assert(rule(&session, USBDIAG_RULE_CFG_TOTAL_LENGTH)->verdict ==
           USBDIAG_NOT_OBSERVED);
}

static void test_fifth_failure_and_wrapped_clock(void)
{
    usbdiag_session_t session;
    uint8_t configuration[9U + 9U + 5U * 7U] = {
        9, 2, sizeof(configuration), 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 5, 3, 1, 1, 0
    };
    usbdiag_control_event_t event;
    uint8_t index;

    for (index = 0U; index < 5U; index++)
    {
        uint8_t offset = (uint8_t)(18U + index * 7U);
        configuration[offset] = 7U;
        configuration[offset + 1U] = 5U;
        configuration[offset + 2U] = (uint8_t)(0x81U + index);
        configuration[offset + 3U] = 2U;
        configuration[offset + 4U] = 65U;
        configuration[offset + 5U] = 0U;
        configuration[offset + 6U] = 0U;
    }
    assert(usbdiag_session_begin(&session, 21U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    event = event_make(21U, 1U, 1U, USBDIAG_SPEED_FULL, 6U,
                       0x0200U, sizeof(configuration), configuration,
                       sizeof(configuration));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_EP_PACKET_SIZE)->verdict == USBDIAG_FAIL);
    assert(rule(&session, USBDIAG_RULE_EP_PACKET_SIZE)->reference_count == 4U);
    assert(rule(&session, USBDIAG_RULE_EP_PACKET_SIZE)->omitted_failure_reference_count == 1U);

    event = event_make(21U, 1U, 2U, USBDIAG_SPEED_FULL, 6U,
                       0x0200U, sizeof(configuration_descriptor),
                       configuration_descriptor, sizeof(configuration_descriptor));
    (void)feed_event(&session, &event);
    assert(rule(&session, USBDIAG_RULE_EP_PACKET_SIZE)->verdict == USBDIAG_FAIL);
    assert(rule(&session, USBDIAG_RULE_EP_PACKET_SIZE)->omitted_failure_reference_count == 1U);
    assert(usbdiag_elapsed_us(0xFFFFFFF0U, 0x00000010U) == 32U);
}

static void test_context_isolation_and_input_errors(void)
{
    usbdiag_session_t first;
    usbdiag_session_t second;
    usbdiag_control_event_t first_event;
    usbdiag_control_event_t second_event;

    assert(usbdiag_session_begin(&first, 22U, 1U, USBDIAG_SPEED_FULL) ==
           USBDIAG_SESSION_NOT_FINAL);
    assert(usbdiag_session_begin(&second, 23U, 1U, USBDIAG_SPEED_HIGH) ==
           USBDIAG_SESSION_NOT_FINAL);
    first_event = event_make(22U, 1U, 100U, USBDIAG_SPEED_FULL, 6U,
                             0x0100U, 8U, device_descriptor, 8U);
    second_event = event_make(23U, 1U, 200U, USBDIAG_SPEED_HIGH, 6U,
                              0x0100U, 18U, device_descriptor,
                              sizeof(device_descriptor));
    (void)feed_event(&first, &first_event);
    (void)feed_event(&second, &second_event);
    assert(first.session_id == 22U && second.session_id == 23U);
    assert(first.rules[USBDIAG_RULE_DEV_B_LENGTH].references[0].event_sequence == 100U);
    assert(second.rules[USBDIAG_RULE_DEV_B_LENGTH].references[0].event_sequence == 200U);

    first_event.sequence = 100U;
    assert(usbdiag_session_feed(&first, &first_event) == USBDIAG_SESSION_INVALID);
    assert(usbdiag_session_get_result(&first, NULL) == USBDIAG_SESSION_INVALID);
    assert(usbdiag_session_end(&first, true) == USBDIAG_SESSION_DEGRADED);
}

static uint32_t next_boundary_value(uint32_t *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static bool reference_was_captured(const usbdiag_reference_t *reference,
                                   const uint32_t *sequences,
                                   const uint16_t *lengths,
                                   uint8_t event_count)
{
    uint8_t index;
    for (index = 0U; index < event_count; index++)
    {
        if (sequences[index] == reference->event_sequence)
            return (uint32_t)reference->byte_offset + reference->byte_length <=
                   lengths[index];
    }
    return false;
}

static void test_fixed_seed_session_boundaries(void)
{
    uint32_t prng = 0x8C6E37F1U;
    uint32_t iteration;

    for (iteration = 0U; iteration < 10000U; iteration++)
    {
        uint32_t state = next_boundary_value(&prng);
        usbdiag_session_t session;
        uint8_t device[sizeof(device_descriptor)];
        uint8_t config[sizeof(configuration_descriptor)];
        uint8_t prefix[8];
        uint32_t sequences[8];
        uint16_t lengths[8];
        uint8_t event_count = 0U;
        uint32_t sequence = iteration * 8U + 1U;
        uint32_t session_id = iteration + 1U;
        uint8_t speed = (uint8_t)(1U + ((state >> 8U) % 3U));
        usbdiag_session_status_t status;
        usbdiag_control_event_t event;
        uint8_t rule_index;

        memcpy(device, device_descriptor, sizeof(device));
        memcpy(config, configuration_descriptor, sizeof(config));
        memcpy(prefix, device, sizeof(prefix));
        switch (state % 12U)
        {
        case 0U: device[0] = (uint8_t)state; break;
        case 1U: device[1] = (uint8_t)state; break;
        case 2U: device[7] = (uint8_t)(65U + (state % 128U)); break;
        case 3U: config[0] = (uint8_t)(state & 7U); break;
        case 4U: config[2] = 24U; break;
        case 5U: config[4] = (uint8_t)(1U + ((state >> 12U) % 7U)); break;
        case 6U: config[13] = (uint8_t)(state & 3U); break;
        case 7U: config[22] = (uint8_t)(65U + (state % 128U)); break;
        case 8U: config[24] = (uint8_t)state; break;
        case 9U: prefix[2] ^= 1U; break;
        default: break;
        }

        status = usbdiag_session_begin(&session, session_id, 1U, speed);
        assert(status == USBDIAG_SESSION_NOT_FINAL);
        event = event_make(session_id, 1U, sequence++, speed, 6U,
                           0x0100U, sizeof(prefix), prefix, sizeof(prefix));
        sequences[event_count] = event.sequence;
        lengths[event_count++] = event.captured_length;
        (void)feed_event(&session, &event);

        event = event_make(session_id, 1U, sequence++, speed, 6U,
                           0x0100U, sizeof(device), device, sizeof(device));
        sequences[event_count] = event.sequence;
        lengths[event_count++] = event.captured_length;
        (void)feed_event(&session, &event);

        event = event_make(session_id, 1U, sequence++, speed, 6U,
                           0x0200U, 9U, config, 9U);
        sequences[event_count] = event.sequence;
        lengths[event_count++] = event.captured_length;
        (void)feed_event(&session, &event);

        event = event_make(session_id, 1U, sequence++, speed, 6U,
                           0x0200U, sizeof(config), config, sizeof(config));
        if ((state & 0x40000000U) != 0U)
        {
            event.transfer_length = (uint16_t)(sizeof(config) - 1U);
            event.captured_length = (uint16_t)(sizeof(config) - 1U);
            event.payload_size = event.captured_length;
        }
        sequences[event_count] = event.sequence;
        lengths[event_count++] = event.captured_length;
        (void)feed_event(&session, &event);

        sequences[event_count] = sequence;
        lengths[event_count++] = 0U;
        required_no_data(&session, sequence++, 5U, 1U,
                         (state & 0x10000000U) != 0U ?
                         USBDIAG_TRANSFER_ERROR : USBDIAG_TRANSFER_SUCCESS);
        sequences[event_count] = sequence;
        lengths[event_count++] = 0U;
        required_no_data(&session, sequence++, 9U, 1U,
                         USBDIAG_TRANSFER_SUCCESS);
        if ((state & 0x08000000U) != 0U)
            (void)usbdiag_session_mark_gap(&session);
        status = usbdiag_session_end(&session, true);

        for (rule_index = 0U; rule_index < USBDIAG_RULE_COUNT; rule_index++)
        {
            const usbdiag_rule_result_t *result = &session.rules[rule_index];
            uint8_t ref_index;
            assert(result->reference_count <= USBDIAG_REFERENCE_CAPACITY);
            if (result->omitted_failure_reference_count > 0U)
                assert(result->verdict == USBDIAG_FAIL);
            for (ref_index = 0U; ref_index < result->reference_count; ref_index++)
            {
                if (!reference_was_captured(&result->references[ref_index],
                                            sequences, lengths, event_count))
                {
                    fprintf(stderr,
                            "seed iteration %lu rule %u ref seq %lu offset %u length %u\n",
                            (unsigned long)iteration, (unsigned)rule_index,
                            (unsigned long)result->references[ref_index].event_sequence,
                            (unsigned)result->references[ref_index].byte_offset,
                            (unsigned)result->references[ref_index].byte_length);
                    assert(false);
                }
            }
            if (result->verdict == USBDIAG_PASS)
                assert(result->reference_count > 0U);
        }
        if (status == USBDIAG_SESSION_COMPLETE)
        {
            assert(session.pending_configuration_count == 0U);
            assert(session.rules[USBDIAG_RULE_DEV_B_LENGTH].verdict == USBDIAG_PASS);
        }
    }
}

int usbdiag_run_session_tests(void)
{
    test_complete_twelve_rule_session();
    test_each_individual_rule_failure();
    test_prefix_witness_is_sticky();
    test_missing_and_dropped_evidence_never_passes();
    test_ep0_rules_and_optional_stall();
    test_configuration_counts_and_applicability();
    test_fifth_failure_and_wrapped_clock();
    test_context_isolation_and_input_errors();
    test_fixed_seed_session_boundaries();
    return 0;
}

#ifndef USBDIAG_LIBRARY_TESTS_NO_MAIN
int main(void)
{
    int result = usbdiag_run_session_tests();
    puts("usbdiag_session: PASS (12 rules, witness retention, gaps, sessions, 10000 fixed-seed inputs)");
    return result;
}
#endif
