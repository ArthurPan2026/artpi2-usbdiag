/* SPDX-License-Identifier: Apache-2.0 */
#include "usbdiag_session.h"

#include <stddef.h>
#include <string.h>

#define USBDIAG_RULE_MASK(id) (1UL << (unsigned)(id))
#define USBDIAG_MAX_CONFIGS USBDIAG_SESSION_CONFIG_CAPACITY

typedef struct
{
    uint32_t sequence;
    uint16_t captured_length;
    uint8_t capture_status;
    uint8_t used;
} usbdiag_event_record_t;

typedef struct
{
    uint32_t header_sequence;
    uint16_t declared_length;
    uint8_t descriptor_index;
    uint8_t used;
    uint8_t header_known;
    uint8_t header_missing;
    uint8_t pending;
    uint8_t full_seen;
} usbdiag_config_record_t;

typedef struct
{
    uint32_t magic;
    uint32_t last_sequence;
    uint32_t blocked_pass_mask;
    uint32_t prefix_sequence;
    uint16_t event_count;
    uint16_t config_count;
    uint8_t active;
    uint8_t ended;
    uint8_t enumeration_succeeded;
    uint8_t quality_gap;
    uint8_t has_sequence;
    uint8_t prefix_seen;
    uint8_t required_request_seen;
    uint8_t required_request_error;
    uint8_t speed;
    uint8_t prefix[8];
    usbdiag_event_record_t events[USBDIAG_SESSION_EVENT_CAPACITY];
    usbdiag_config_record_t configurations[USBDIAG_MAX_CONFIGS];
} usbdiag_session_state_t;

#define USBDIAG_SESSION_MAGIC 0x55445331UL

static const usbdiag_rule_definition_t rule_definitions[USBDIAG_RULE_COUNT] =
{
    { "DEV_B_LENGTH", "USB 2.0 9.6.1", "bLength is 18" },
    { "DEV_TYPE", "USB 2.0 9.4.3, 9.6.1", "response type is Device" },
    { "DEV_8B_PREFIX", "USB 2.0 9.4.3, 9.6.1", "initial 8 bytes match the full Device descriptor" },
    { "DEV_FIELDS", "USB 2.0 9.6.1", "bMaxPacketSize0 is valid for the observed speed" },
    { "CFG_B_LENGTH", "USB 2.0 9.6.3", "bLength is 9 and descriptor type is 2" },
    { "CFG_TOTAL_LENGTH", "USB 2.0 9.4.3, 9.6.3", "full response and wTotalLength agree" },
    { "CFG_TREE_WALK", "USB 2.0 9.6.3-9.6.6", "every descriptor is bounded and walkable" },
    { "CFG_INTERFACE_COUNT", "USB 2.0 9.6.3", "unique interface count matches bNumInterfaces" },
    { "IF_ENDPOINT_COUNT", "USB 2.0 9.6.5", "endpoint count matches each alternate setting" },
    { "EP_PACKET_SIZE", "USB 2.0 9.6.6, Table 9-13", "wMaxPacketSize is valid for speed and transfer type" },
    { "EP_INTERVAL", "USB 2.0 9.6.6", "periodic endpoint bInterval is valid" },
    { "EP0_STALL", "USB 2.0 9.4.3, 9.4.7", "required enumeration requests do not STALL" }
};

static const char *const reason_names[] =
{
    "NONE", "MATCH", "MISMATCH", "NOT_OBSERVED", "NOT_APPLICABLE",
    "EVIDENCE_MISSING", "TRANSFER_ERROR", "UNSUPPORTED_SPEED",
    "INVALID_INPUT", "CAPACITY_EXCEEDED"
};

static const char *const reference_role_names[] =
{
    "observation", "device_prefix_first", "device_prefix_current",
    "descriptor_field", "configuration_header", "wTotalLength",
    "configuration_tree", "interface_count", "endpoint_count",
    "endpoint_packet_size", "endpoint_interval", "control_status"
};

const char *usbdiag_version(void)
{
    return USB_ENUM_DIAG_VERSION;
}

static usbdiag_session_state_t *session_state(usbdiag_session_t *session)
{
    return (usbdiag_session_state_t *)(void *)session->opaque_state;
}

static const usbdiag_session_state_t *session_state_const(
    const usbdiag_session_t *session)
{
    return (const usbdiag_session_state_t *)(const void *)session->opaque_state;
}

typedef char usbdiag_session_state_must_fit[
    sizeof(usbdiag_session_state_t) <= 4096U ? 1 : -1];

static void result_set(usbdiag_session_t *session,
                       usbdiag_rule_id_t id,
                       usbdiag_verdict_t verdict,
                       usbdiag_reason_t reason,
                       const uint32_t *expected,
                       uint8_t expected_count,
                       const uint32_t *observed,
                       uint8_t observed_count,
                       const usbdiag_reference_t *references,
                       uint8_t reference_count)
{
    usbdiag_session_state_t *state = session_state(session);
    usbdiag_rule_result_t *current = &session->rules[id];
    usbdiag_rule_result_t candidate;
    uint8_t index;

    memset(&candidate, 0, sizeof(candidate));
    candidate.verdict = verdict;
    candidate.reason = reason;
    candidate.expected_count = expected_count > 3U ? 3U : expected_count;
    candidate.observed_count = observed_count > 3U ? 3U : observed_count;
    for (index = 0U; index < candidate.expected_count; index++)
        candidate.expected_values[index] = expected[index];
    for (index = 0U; index < candidate.observed_count; index++)
        candidate.observed_values[index] = observed[index];

    for (index = 0U; index < reference_count; index++)
    {
        uint16_t event_index;
        uint32_t span_end = (uint32_t)references[index].byte_offset +
                            references[index].byte_length;
        const usbdiag_event_record_t *record = NULL;

        for (event_index = 0U; event_index < state->event_count; event_index++)
        {
            if (state->events[event_index].used &&
                state->events[event_index].sequence ==
                    references[index].event_sequence)
            {
                record = &state->events[event_index];
                break;
            }
        }
        if (record == NULL ||
            (references[index].byte_length > 0U &&
             span_end > record->captured_length))
        {
            state->quality_gap = 1U;
            verdict = USBDIAG_NOT_OBSERVED;
            reason = USBDIAG_REASON_EVIDENCE_MISSING;
            state->blocked_pass_mask |= USBDIAG_RULE_MASK(id);
            memset(&candidate, 0, sizeof(candidate));
            candidate.verdict = verdict;
            candidate.reason = reason;
            reference_count = 0U;
            break;
        }
        if (candidate.reference_count < USBDIAG_REFERENCE_CAPACITY)
        {
            candidate.references[candidate.reference_count++] = references[index];
        }
        else if (verdict == USBDIAG_FAIL &&
                 candidate.omitted_failure_reference_count != UINT16_MAX)
        {
            candidate.omitted_failure_reference_count++;
        }
    }

    if ((state->blocked_pass_mask & USBDIAG_RULE_MASK(id)) != 0U &&
        verdict == USBDIAG_PASS)
    {
        verdict = USBDIAG_NOT_OBSERVED;
        reason = USBDIAG_REASON_EVIDENCE_MISSING;
        candidate.verdict = verdict;
        candidate.reason = reason;
        candidate.reference_count = 0U;
    }

    /* A failure is a durable witness. Later passing observations never
     * replace its values, source bytes, or reason. Additional failures may
     * contribute references while the four-reference limit permits. */
    if (current->verdict == USBDIAG_FAIL)
    {
        if (verdict == USBDIAG_FAIL)
        {
            for (index = 0U; index < candidate.reference_count; index++)
            {
                uint8_t old_index;
                bool duplicate = false;
                for (old_index = 0U; old_index < current->reference_count; old_index++)
                {
                    const usbdiag_reference_t *old =
                        &current->references[old_index];
                    const usbdiag_reference_t *next =
                        &candidate.references[index];
                    if (old->event_sequence == next->event_sequence &&
                        old->byte_offset == next->byte_offset &&
                        old->byte_length == next->byte_length &&
                        old->role == next->role)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                {
                    if (current->reference_count < USBDIAG_REFERENCE_CAPACITY)
                        current->references[current->reference_count++] =
                            candidate.references[index];
                    else if (current->omitted_failure_reference_count != UINT16_MAX)
                        current->omitted_failure_reference_count++;
                }
            }
        }
        return;
    }

    /* NA is provisional until a relevant object appears. NOT_OBSERVED is
     * provisional until enough bytes arrive, except after its source was lost. */
    if (current->verdict == USBDIAG_NOT_APPLICABLE &&
        verdict == USBDIAG_NOT_OBSERVED)
        return;
    if (current->verdict == USBDIAG_PASS &&
        verdict == USBDIAG_NOT_APPLICABLE)
        return;
    if (current->verdict == USBDIAG_NOT_OBSERVED &&
        current->reason == USBDIAG_REASON_EVIDENCE_MISSING &&
        verdict != USBDIAG_FAIL)
        return;

    candidate.verdict = verdict;
    candidate.reason = reason;
    if (verdict != USBDIAG_FAIL)
        candidate.omitted_failure_reference_count = 0U;
    *current = candidate;
}

static usbdiag_reference_t reference_make(uint32_t sequence,
                                          uint16_t offset,
                                          uint16_t length,
                                          usbdiag_reference_role_t role)
{
    usbdiag_reference_t reference;
    reference.event_sequence = sequence;
    reference.byte_offset = offset;
    reference.byte_length = length;
    reference.role = role;
    return reference;
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static usbdiag_config_record_t *find_config(usbdiag_session_state_t *state,
                                            uint8_t descriptor_index,
                                            bool create)
{
    uint16_t index;
    usbdiag_config_record_t *available = NULL;
    for (index = 0U; index < USBDIAG_MAX_CONFIGS; index++)
    {
        usbdiag_config_record_t *record = &state->configurations[index];
        if (record->used && record->descriptor_index == descriptor_index)
            return record;
        if (!record->used && available == NULL)
            available = record;
    }
    if (!create || available == NULL)
        return NULL;
    memset(available, 0, sizeof(*available));
    available->used = 1U;
    available->descriptor_index = descriptor_index;
    state->config_count++;
    return available;
}

static void refresh_pending_configuration_count(usbdiag_session_t *session)
{
    const usbdiag_session_state_t *state = session_state_const(session);
    uint16_t index;
    session->pending_configuration_count = 0U;
    for (index = 0U; index < USBDIAG_MAX_CONFIGS; index++)
    {
        if (state->configurations[index].used &&
            state->configurations[index].pending)
            session->pending_configuration_count++;
    }
}

static bool event_is_device_descriptor(const usbdiag_control_event_t *event)
{
    return event->has_setup && event->setup.request == 6U &&
           event->setup.bm_request_type == 0x80U &&
           (event->setup.value >> 8U) == 1U;
}

static bool event_is_configuration_descriptor(const usbdiag_control_event_t *event)
{
    return event->has_setup && event->setup.request == 6U &&
           event->setup.bm_request_type == 0x80U &&
           (event->setup.value >> 8U) == 2U;
}

static bool event_is_required_request(const usbdiag_control_event_t *event)
{
    if (!event->has_setup)
        return false;
    if (event->setup.request == 6U && event->setup.bm_request_type == 0x80U)
    {
        uint8_t descriptor_type = (uint8_t)(event->setup.value >> 8U);
        return descriptor_type == 1U || descriptor_type == 2U;
    }
    if ((event->setup.request == 5U || event->setup.request == 9U) &&
        event->setup.bm_request_type == 0U && event->setup.length == 0U)
        return true;
    return false;
}

static void set_not_observed(usbdiag_session_t *session,
                             usbdiag_rule_id_t id,
                             usbdiag_reason_t reason,
                             uint32_t sequence)
{
    usbdiag_reference_t ref = reference_make(sequence, 0U, 0U,
                                              USBDIAG_REF_CONTROL_STATUS);
    result_set(session, id, USBDIAG_NOT_OBSERVED, reason, NULL, 0U,
               NULL, 0U, sequence == 0U ? NULL : &ref,
               sequence == 0U ? 0U : 1U);
}

static void analyze_device_descriptor(usbdiag_session_t *session,
                                      const usbdiag_control_event_t *event)
{
    usbdiag_session_state_t *state = session_state(session);
    const uint8_t *data = event->payload;
    uint16_t length = event->captured_length;
    uint32_t expected[1];
    uint32_t observed[1];
    usbdiag_reference_t refs[2];
    bool full_response = event->setup.length >= 18U &&
                         event->transfer_status == USBDIAG_TRANSFER_SUCCESS &&
                         event->capture_status == USBDIAG_CAPTURE_COMPLETE &&
                         length >= 18U;

    if (length >= 1U)
    {
        expected[0] = 18U;
        observed[0] = data[0];
        refs[0] = reference_make(event->sequence, 0U, 1U,
                                 USBDIAG_REF_DESCRIPTOR_FIELD);
        result_set(session, USBDIAG_RULE_DEV_B_LENGTH,
                   usbdiag_device_b_length_valid(data[0]) ?
                       USBDIAG_PASS : USBDIAG_FAIL,
                   usbdiag_device_b_length_valid(data[0]) ?
                       USBDIAG_REASON_MATCH : USBDIAG_REASON_MISMATCH,
                   expected, 1U, observed, 1U, refs, 1U);
    }
    else
        set_not_observed(session, USBDIAG_RULE_DEV_B_LENGTH,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);

    if (length >= 2U)
    {
        expected[0] = 1U;
        observed[0] = data[1];
        refs[0] = reference_make(event->sequence, 1U, 1U,
                                 USBDIAG_REF_DESCRIPTOR_FIELD);
        result_set(session, USBDIAG_RULE_DEV_TYPE,
                   usbdiag_device_descriptor_type_valid(data[1]) ?
                       USBDIAG_PASS : USBDIAG_FAIL,
                   usbdiag_device_descriptor_type_valid(data[1]) ?
                       USBDIAG_REASON_MATCH : USBDIAG_REASON_MISMATCH,
                   expected, 1U, observed, 1U, refs, 1U);
    }
    else
        set_not_observed(session, USBDIAG_RULE_DEV_TYPE,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);

    if (event->setup.length == 8U && length >= 8U &&
        event->capture_status == USBDIAG_CAPTURE_COMPLETE &&
        event->transfer_status == USBDIAG_TRANSFER_SUCCESS)
    {
        if (!state->prefix_seen)
        {
            memcpy(state->prefix, data, sizeof(state->prefix));
            state->prefix_sequence = event->sequence;
            state->prefix_seen = 1U;
        }
        else if (memcmp(state->prefix, data, sizeof(state->prefix)) != 0)
        {
            refs[0] = reference_make(state->prefix_sequence, 0U, 8U,
                                     USBDIAG_REF_DEVICE_PREFIX_FIRST);
            refs[1] = reference_make(event->sequence, 0U, 8U,
                                     USBDIAG_REF_DEVICE_PREFIX_CURRENT);
            expected[0] = 1U;
            observed[0] = 0U;
            result_set(session, USBDIAG_RULE_DEV_8B_PREFIX, USBDIAG_FAIL,
                       USBDIAG_REASON_MISMATCH, expected, 1U,
                       observed, 1U, refs, 2U);
        }
    }
    else if (event->setup.length == 8U)
    {
        state->quality_gap = 1U;
    }

    if (length >= 8U && usbdiag_speed_known(event->speed))
    {
        expected[0] = event->speed;
        observed[0] = data[7];
        refs[0] = reference_make(event->sequence, 7U, 1U,
                                 USBDIAG_REF_DESCRIPTOR_FIELD);
        result_set(session, USBDIAG_RULE_DEV_FIELDS,
                   usbdiag_device_ep0_mps_valid(event->speed, data[7]) ?
                       USBDIAG_PASS : USBDIAG_FAIL,
                   usbdiag_device_ep0_mps_valid(event->speed, data[7]) ?
                       USBDIAG_REASON_MATCH : USBDIAG_REASON_MISMATCH,
                   expected, 1U, observed, 1U, refs, 1U);
    }
    else if (length >= 8U)
    {
        set_not_observed(session, USBDIAG_RULE_DEV_FIELDS,
                         USBDIAG_REASON_UNSUPPORTED_SPEED, event->sequence);
    }

    if (full_response && state->prefix_seen)
    {
        expected[0] = 1U;
        observed[0] = memcmp(state->prefix, data, sizeof(state->prefix)) == 0;
        refs[0] = reference_make(state->prefix_sequence, 0U, 8U,
                                 USBDIAG_REF_DEVICE_PREFIX_FIRST);
        refs[1] = reference_make(event->sequence, 0U, 8U,
                                 USBDIAG_REF_DEVICE_PREFIX_CURRENT);
        result_set(session, USBDIAG_RULE_DEV_8B_PREFIX,
                   observed[0] != 0U ? USBDIAG_PASS : USBDIAG_FAIL,
                   observed[0] != 0U ? USBDIAG_REASON_MATCH :
                                       USBDIAG_REASON_MISMATCH,
                   expected, 1U, observed, 1U, refs, 2U);
    }
    else if (full_response)
    {
        set_not_observed(session, USBDIAG_RULE_DEV_8B_PREFIX,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);
    }
}

static void add_config_refs(usbdiag_reference_t refs[USBDIAG_REFERENCE_CAPACITY],
                            uint8_t *count,
                            uint32_t sequence,
                            const usbdiag_failure_refs_t *failures,
                            uint8_t field_length,
                            usbdiag_reference_role_t role)
{
    uint8_t index;
    for (index = 0U; index < failures->count &&
         *count < USBDIAG_REFERENCE_CAPACITY; index++)
    {
        refs[*count] = reference_make(sequence, failures->offsets[index],
                                      field_length, role);
        (*count)++;
    }
}

static void commit_configuration_analysis(usbdiag_session_t *session,
                                          const usbdiag_control_event_t *event,
                                          const usbdiag_config_result_t *analysis)
{
    static const usbdiag_rule_id_t destination[USBDIAG_CFG_RULE_COUNT] =
    {
        USBDIAG_RULE_CFG_B_LENGTH,
        USBDIAG_RULE_CFG_TOTAL_LENGTH,
        USBDIAG_RULE_CFG_TREE_WALK,
        USBDIAG_RULE_CFG_INTERFACE_COUNT,
        USBDIAG_RULE_IF_ENDPOINT_COUNT,
        USBDIAG_RULE_EP_PACKET_SIZE,
        USBDIAG_RULE_EP_INTERVAL
    };
    uint8_t rule_index;
    for (rule_index = 0U; rule_index < USBDIAG_CFG_RULE_COUNT; rule_index++)
    {
        const usbdiag_config_rule_result_t *source =
            &analysis->rules[rule_index];
        usbdiag_reference_t refs[USBDIAG_REFERENCE_CAPACITY];
        uint8_t ref_count = 0U;
        uint8_t field_length = 1U;
        uint32_t observed[3] = { 0U, 0U, 0U };
        uint32_t expected[3] = { 0U, 0U, 0U };
        uint8_t value_count = 0U;
        uint16_t index;

        switch (rule_index)
        {
        case USBDIAG_CFG_HEADER:
            field_length = 2U;
            refs[ref_count++] = reference_make(event->sequence, 0U, 2U,
                                                USBDIAG_REF_CONFIG_HEADER);
            expected[0] = 9U;
            expected[1] = 2U;
            observed[0] = event->payload[0];
            observed[1] = event->payload[1];
            value_count = 2U;
            break;
        case USBDIAG_CFG_TOTAL_LENGTH:
            field_length = 2U;
            refs[ref_count++] = reference_make(event->sequence, 2U, 2U,
                                                USBDIAG_REF_CONFIG_TOTAL_LENGTH);
            expected[0] = analysis->declared_total_length;
            observed[0] = event->transfer_length;
            observed[1] = event->setup.length;
            value_count = 2U;
            break;
        case USBDIAG_CFG_TREE_WALK:
            refs[ref_count++] = reference_make(event->sequence, 0U,
                                                analysis->declared_total_length,
                                                USBDIAG_REF_CONFIG_DESCRIPTOR);
            expected[0] = analysis->declared_total_length;
            observed[0] = analysis->declared_total_length;
            value_count = 1U;
            break;
        case USBDIAG_CFG_INTERFACE_COUNT:
            refs[ref_count++] = reference_make(event->sequence, 4U, 1U,
                                                USBDIAG_REF_INTERFACE_COUNT);
            expected[0] = event->payload[4];
            observed[0] = analysis->unique_interface_count;
            value_count = 1U;
            break;
        case USBDIAG_CFG_ENDPOINT_COUNT:
            add_config_refs(refs, &ref_count, event->sequence,
                            &source->failures, 1U,
                            USBDIAG_REF_ENDPOINT_COUNT);
            if (source->verdict == USBDIAG_PASS && ref_count == 0U)
            {
                uint16_t offset = 0U;
                while (offset + 1U < analysis->declared_total_length)
                {
                    uint8_t descriptor_length = event->payload[offset];
                    if (descriptor_length < 2U ||
                        (uint32_t)offset + descriptor_length >
                            analysis->declared_total_length)
                        break;
                    if (event->payload[offset + 1U] == 4U &&
                        descriptor_length >= 9U)
                    {
                        refs[ref_count++] = reference_make(
                            event->sequence, (uint16_t)(offset + 4U), 1U,
                            USBDIAG_REF_ENDPOINT_COUNT);
                        break;
                    }
                    offset = (uint16_t)(offset + descriptor_length);
                }
            }
            expected[0] = source->failures.total_failures == 0U ?
                          analysis->interface_setting_count :
                          source->failures.total_failures;
            observed[0] = analysis->endpoint_count;
            value_count = 1U;
            break;
        case USBDIAG_CFG_ENDPOINT_PACKET_SIZE:
            add_config_refs(refs, &ref_count, event->sequence,
                            &source->failures, 2U,
                            USBDIAG_REF_ENDPOINT_PACKET_SIZE);
            expected[0] = analysis->endpoint_count;
            observed[0] = source->failures.total_failures;
            value_count = 1U;
            field_length = 2U;
            break;
        case USBDIAG_CFG_ENDPOINT_INTERVAL:
            add_config_refs(refs, &ref_count, event->sequence,
                            &source->failures, 1U,
                            USBDIAG_REF_ENDPOINT_INTERVAL);
            expected[0] = analysis->endpoint_count;
            observed[0] = source->failures.total_failures;
            value_count = 1U;
            break;
        default:
            break;
        }

        if (source->verdict == USBDIAG_PASS &&
            (rule_index == USBDIAG_CFG_ENDPOINT_PACKET_SIZE ||
             rule_index == USBDIAG_CFG_ENDPOINT_INTERVAL))
        {
            /* Point at the first complete configuration field. The observed
             * count and analysis result cover every applicable descriptor. */
            for (index = 0U; index + 1U < analysis->declared_total_length &&
                 ref_count == 0U; )
            {
                uint8_t length = event->payload[index];
                uint8_t type = event->payload[index + 1U];
                if (length < 2U ||
                    (uint32_t)index + length > analysis->declared_total_length)
                    break;
                if (type == 5U && length >= 7U)
                {
                    uint8_t transfer_type = event->payload[index + 3U] & 3U;
                    if (rule_index == USBDIAG_CFG_ENDPOINT_PACKET_SIZE ||
                        transfer_type == 1U || transfer_type == 3U)
                    {
                        uint16_t offset = (uint16_t)(index +
                            (rule_index == USBDIAG_CFG_ENDPOINT_INTERVAL ? 6U : 4U));
                        refs[ref_count++] = reference_make(
                            event->sequence, offset, field_length,
                            (rule_index == USBDIAG_CFG_ENDPOINT_INTERVAL) ?
                            USBDIAG_REF_ENDPOINT_INTERVAL :
                            USBDIAG_REF_ENDPOINT_PACKET_SIZE);
                    }
                }
                index = (uint16_t)(index + length);
            }
        }

        result_set(session, destination[rule_index], source->verdict,
                   source->failures.total_failures != 0U ?
                       USBDIAG_REASON_MISMATCH :
                   source->verdict == USBDIAG_PASS ? USBDIAG_REASON_MATCH :
                   source->verdict == USBDIAG_NOT_APPLICABLE ?
                       USBDIAG_REASON_NOT_APPLICABLE :
                       USBDIAG_REASON_NOT_OBSERVED,
                   expected, value_count, observed, value_count,
                   refs, ref_count);
        if (source->failures.total_failures != 0U)
        {
            session->rules[destination[rule_index]].omitted_failure_reference_count =
            source->failures.total_failures > ref_count ?
            (uint16_t)(source->failures.total_failures - ref_count) : 0U;
        }
    }
}

static void analyze_configuration(usbdiag_session_t *session,
                                  const usbdiag_control_event_t *event)
{
    usbdiag_session_state_t *state = session_state(session);
    usbdiag_config_record_t *record;
    uint8_t descriptor_index = (uint8_t)event->setup.value;
    const uint8_t *data = event->payload;
    uint16_t length = event->captured_length;
    bool header_available = length >= 2U;
    bool total_available = length >= 4U;
    uint16_t declared_total = total_available ? read_le16(data + 2U) : 0U;
    bool complete = event->transfer_status == USBDIAG_TRANSFER_SUCCESS &&
                    event->capture_status == USBDIAG_CAPTURE_COMPLETE &&
                    event->transfer_length_known &&
                    event->transfer_length == length;

    record = find_config(state, descriptor_index, true);
    if (record == NULL)
    {
        state->quality_gap = 1U;
        session->status = USBDIAG_SESSION_CAPACITY_EXCEEDED;
        set_not_observed(session, USBDIAG_RULE_CFG_TOTAL_LENGTH,
                         USBDIAG_REASON_CAPACITY_EXCEEDED, event->sequence);
        return;
    }
    session->configuration_count = state->config_count;
    if (!header_available)
    {
        set_not_observed(session, USBDIAG_RULE_CFG_B_LENGTH,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);
        set_not_observed(session, USBDIAG_RULE_CFG_TOTAL_LENGTH,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);
        record->pending = 1U;
        refresh_pending_configuration_count(session);
        return;
    }

    {
        uint32_t expected[2] = { 9U, 2U };
        uint32_t observed[2] = { data[0], data[1] };
        usbdiag_reference_t ref = reference_make(
            event->sequence, 0U, 2U, USBDIAG_REF_CONFIG_HEADER);
        bool valid = usbdiag_config_header_valid(data, length);
        /* A two-byte header is enough to prove these two fields; the full
         * descriptor length remains a separate session-level requirement. */
        if (length >= 2U)
            valid = data[0] == 9U && data[1] == 2U;
        result_set(session, USBDIAG_RULE_CFG_B_LENGTH,
                   valid ? USBDIAG_PASS : USBDIAG_FAIL,
                   valid ? USBDIAG_REASON_MATCH : USBDIAG_REASON_MISMATCH,
                   expected, 2U, observed, 2U, &ref, 1U);
    }

    if (!total_available)
    {
        set_not_observed(session, USBDIAG_RULE_CFG_TOTAL_LENGTH,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);
        record->pending = 1U;
        record->header_sequence = event->sequence;
        record->header_missing = 1U;
        refresh_pending_configuration_count(session);
        return;
    }

    if (record->header_known && record->pending &&
        (record->declared_length != declared_total ||
         (data[0] != 9U || data[1] != 2U)))
    {
        usbdiag_reference_t refs[2] =
        {
            reference_make(record->header_sequence, 2U, 2U,
                           USBDIAG_REF_CONFIG_TOTAL_LENGTH),
            reference_make(event->sequence, 2U, 2U,
                           USBDIAG_REF_CONFIG_TOTAL_LENGTH)
        };
        uint32_t expected[2] = { record->declared_length, declared_total };
        uint32_t observed[2] = { record->declared_length, declared_total };
        result_set(session, USBDIAG_RULE_CFG_TOTAL_LENGTH,
                   USBDIAG_FAIL, USBDIAG_REASON_MISMATCH,
                   expected, 2U, observed, 2U, refs, 2U);
    }

    record->header_known = 1U;
    record->header_sequence = event->sequence;
    record->declared_length = declared_total;
    record->header_missing = 0U;
    record->pending = (event->setup.length < declared_total || !complete) ? 1U : 0U;
    record->full_seen = record->pending ? record->full_seen : 1U;
    refresh_pending_configuration_count(session);

    if (declared_total < 9U)
    {
        record->pending = 0U;
        refresh_pending_configuration_count(session);
        uint32_t expected = 9U;
        uint32_t observed = declared_total;
        usbdiag_reference_t ref = reference_make(
            event->sequence, 2U, 2U, USBDIAG_REF_CONFIG_TOTAL_LENGTH);
        result_set(session, USBDIAG_RULE_CFG_TOTAL_LENGTH, USBDIAG_FAIL,
                   USBDIAG_REASON_MISMATCH, &expected, 1U, &observed, 1U,
                   &ref, 1U);
        return;
    }

    if (event->setup.length < declared_total)
    {
        uint32_t expected = declared_total;
        uint32_t observed[2] = { event->transfer_length, event->setup.length };
        usbdiag_reference_t ref = reference_make(
            event->sequence, 2U, 2U, USBDIAG_REF_CONFIG_TOTAL_LENGTH);
        set_not_observed(session, USBDIAG_RULE_CFG_TOTAL_LENGTH,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);
        session->rules[USBDIAG_RULE_CFG_TOTAL_LENGTH].expected_values[0] = expected;
        session->rules[USBDIAG_RULE_CFG_TOTAL_LENGTH].expected_count = 1U;
        session->rules[USBDIAG_RULE_CFG_TOTAL_LENGTH].observed_values[0] = observed[0];
        session->rules[USBDIAG_RULE_CFG_TOTAL_LENGTH].observed_values[1] = observed[1];
        session->rules[USBDIAG_RULE_CFG_TOTAL_LENGTH].observed_count = 2U;
        /* wTotalLength is a useful location even while the full request is pending. */
        if (session->rules[USBDIAG_RULE_CFG_TOTAL_LENGTH].reference_count == 0U)
            session->rules[USBDIAG_RULE_CFG_TOTAL_LENGTH].references[0] = ref,
            session->rules[USBDIAG_RULE_CFG_TOTAL_LENGTH].reference_count = 1U;
        return;
    }

    if (!event->transfer_length_known ||
        event->transfer_length != declared_total)
    {
        uint32_t expected = declared_total;
        uint32_t observed[2] = { event->transfer_length, event->setup.length };
        usbdiag_reference_t ref = reference_make(
            event->sequence, 2U, 2U, USBDIAG_REF_CONFIG_TOTAL_LENGTH);
        if (event->transfer_status == USBDIAG_TRANSFER_SUCCESS &&
            event->transfer_length_known)
            result_set(session, USBDIAG_RULE_CFG_TOTAL_LENGTH,
                       USBDIAG_FAIL, USBDIAG_REASON_MISMATCH,
                       &expected, 1U, observed, 2U, &ref, 1U);
        else
            set_not_observed(session, USBDIAG_RULE_CFG_TOTAL_LENGTH,
                             USBDIAG_REASON_NOT_OBSERVED, event->sequence);
        return;
    }

    if (!complete || length < declared_total)
    {
        state->quality_gap = 1U;
        set_not_observed(session, USBDIAG_RULE_CFG_TOTAL_LENGTH,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);
        set_not_observed(session, USBDIAG_RULE_CFG_TREE_WALK,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);
        set_not_observed(session, USBDIAG_RULE_CFG_INTERFACE_COUNT,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);
        set_not_observed(session, USBDIAG_RULE_IF_ENDPOINT_COUNT,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);
        set_not_observed(session, USBDIAG_RULE_EP_PACKET_SIZE,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);
        set_not_observed(session, USBDIAG_RULE_EP_INTERVAL,
                         USBDIAG_REASON_NOT_OBSERVED, event->sequence);
        return;
    }

    {
        uint32_t expected = declared_total;
        uint32_t observed[2] = { event->transfer_length, event->setup.length };
        usbdiag_reference_t ref = reference_make(
            event->sequence, 2U, 2U, USBDIAG_REF_CONFIG_TOTAL_LENGTH);
        result_set(session, USBDIAG_RULE_CFG_TOTAL_LENGTH, USBDIAG_PASS,
                   USBDIAG_REASON_MATCH, &expected, 1U, observed, 2U,
                   &ref, 1U);
    }
    record->pending = 0U;
    record->full_seen = 1U;
    refresh_pending_configuration_count(session);
    {
        usbdiag_config_result_t analysis;
        usbdiag_analyze_configuration(data, length, true,
                                      event->setup.length,
                                      event->transfer_length_known,
                                      event->transfer_length, event->speed,
                                      &analysis);
        commit_configuration_analysis(session, event, &analysis);
    }
}

const usbdiag_rule_definition_t *usbdiag_rule_definition(usbdiag_rule_id_t rule)
{
    return (unsigned)rule < USBDIAG_RULE_COUNT ? &rule_definitions[rule] : NULL;
}

const char *usbdiag_reason_name(usbdiag_reason_t reason)
{
    return (unsigned)reason <
           (sizeof(reason_names) / sizeof(reason_names[0])) ?
           reason_names[reason] : "INVALID_REASON";
}

const char *usbdiag_reference_role_name(usbdiag_reference_role_t role)
{
    return (unsigned)role <
           (sizeof(reference_role_names) / sizeof(reference_role_names[0])) ?
           reference_role_names[role] : "unknown";
}

usbdiag_session_status_t usbdiag_session_begin(usbdiag_session_t *session,
                                               uint32_t session_id,
                                               uint32_t attempt_id,
                                               uint8_t speed)
{
    usbdiag_session_state_t *state;
    uint8_t index;
    if (session == NULL || session_id == 0U || attempt_id == 0U)
        return USBDIAG_SESSION_INVALID;

    memset(session, 0, sizeof(*session));
    state = session_state(session);
    state->magic = USBDIAG_SESSION_MAGIC;
    state->active = 1U;
    state->speed = speed;
    session->session_id = session_id;
    session->attempt_id = attempt_id;
    session->speed = speed;
    session->status = USBDIAG_SESSION_NOT_FINAL;
    for (index = 0U; index < USBDIAG_RULE_COUNT; index++)
    {
        session->rules[index].verdict = USBDIAG_NOT_OBSERVED;
        session->rules[index].reason = USBDIAG_REASON_NOT_OBSERVED;
    }
    return session->status;
}

usbdiag_session_status_t usbdiag_session_feed(usbdiag_session_t *session,
                                              const usbdiag_control_event_t *event)
{
    usbdiag_session_state_t *state;
    usbdiag_event_record_t *record;
    uint32_t sequence_delta;
    bool relevant;

    if (session == NULL || event == NULL)
        return USBDIAG_SESSION_INVALID;
    state = session_state(session);
    if (state->magic != USBDIAG_SESSION_MAGIC || !state->active || state->ended)
        return USBDIAG_SESSION_INVALID;
    if (event->session_id != session->session_id ||
        event->attempt_id != session->attempt_id)
        return USBDIAG_SESSION_INVALID;

    relevant = event_is_device_descriptor(event) ||
               event_is_configuration_descriptor(event) ||
               event_is_required_request(event);
    if (!relevant)
        return session->status;

    if (event->captured_length > event->payload_size ||
        (event->captured_length > 0U && event->payload == NULL) ||
        (event->payload_size > 0U && event->payload == NULL) ||
        (event->transfer_length_known &&
         event->captured_length > event->transfer_length) ||
        (event->capture_status == USBDIAG_CAPTURE_COMPLETE &&
         event->captured_length != event->payload_size))
    {
        (void)usbdiag_session_mark_gap(session);
        return USBDIAG_SESSION_INVALID;
    }

    if (state->has_sequence)
    {
        sequence_delta = event->sequence - state->last_sequence;
        if (sequence_delta == 0U || sequence_delta >= 0x80000000UL)
        {
            (void)usbdiag_session_mark_gap(session);
            return USBDIAG_SESSION_INVALID;
        }
    }
    if (state->event_count >= USBDIAG_SESSION_EVENT_CAPACITY)
    {
        state->quality_gap = 1U;
        session->status = USBDIAG_SESSION_CAPACITY_EXCEEDED;
        return session->status;
    }

    record = &state->events[state->event_count++];
    record->sequence = event->sequence;
    record->captured_length = event->captured_length;
    record->capture_status = (uint8_t)event->capture_status;
    record->used = 1U;
    state->last_sequence = event->sequence;
    state->has_sequence = 1U;
    session->last_sequence = event->sequence;
    session->event_count = state->event_count;

    if (event->capture_status == USBDIAG_CAPTURE_TRUNCATED)
        state->quality_gap = 1U;

    if (event_is_device_descriptor(event))
        analyze_device_descriptor(session, event);
    if (event_is_configuration_descriptor(event))
        analyze_configuration(session, event);

    if (event_is_required_request(event))
    {
        state->required_request_seen = 1U;
        if (event->transfer_status == USBDIAG_TRANSFER_STALL)
        {
            uint32_t expected = 0U;
            uint32_t observed = 1U;
            usbdiag_reference_t ref = reference_make(
                event->sequence, 0U, 0U, USBDIAG_REF_CONTROL_STATUS);
            result_set(session, USBDIAG_RULE_EP0_STALL, USBDIAG_FAIL,
                       USBDIAG_REASON_MISMATCH, &expected, 1U,
                       &observed, 1U, &ref, 1U);
        }
        else if (event->transfer_status == USBDIAG_TRANSFER_SUCCESS)
        {
            uint32_t expected = 0U;
            uint32_t observed = 0U;
            usbdiag_reference_t ref = reference_make(
                event->sequence, 0U, 0U, USBDIAG_REF_CONTROL_STATUS);
            result_set(session, USBDIAG_RULE_EP0_STALL, USBDIAG_PASS,
                       USBDIAG_REASON_MATCH, &expected, 1U,
                       &observed, 1U, &ref, 1U);
        }
        else
        {
            state->required_request_error = 1U;
            set_not_observed(session, USBDIAG_RULE_EP0_STALL,
                             USBDIAG_REASON_TRANSFER_ERROR, event->sequence);
        }
    }

    return session->status;
}

usbdiag_session_status_t usbdiag_session_mark_gap(usbdiag_session_t *session)
{
    usbdiag_session_state_t *state;
    if (session == NULL)
        return USBDIAG_SESSION_INVALID;
    state = session_state(session);
    if (state->magic != USBDIAG_SESSION_MAGIC || !state->active)
        return USBDIAG_SESSION_INVALID;
    state->quality_gap = 1U;
    session->status = USBDIAG_SESSION_DEGRADED;
    return session->status;
}

usbdiag_session_status_t usbdiag_session_forget_event(usbdiag_session_t *session,
                                                      uint32_t event_sequence)
{
    usbdiag_session_state_t *state;
    uint16_t event_index;
    uint8_t rule_index;
    bool found = false;
    if (session == NULL)
        return USBDIAG_SESSION_INVALID;
    state = session_state(session);
    if (state->magic != USBDIAG_SESSION_MAGIC || !state->active)
        return USBDIAG_SESSION_INVALID;
    for (event_index = 0U; event_index < state->event_count; event_index++)
    {
        if (state->events[event_index].used &&
            state->events[event_index].sequence == event_sequence)
        {
            state->events[event_index].used = 0U;
            found = true;
            break;
        }
    }
    if (!found)
        return USBDIAG_SESSION_INVALID;

    if (state->prefix_seen && state->prefix_sequence == event_sequence)
    {
        usbdiag_rule_result_t *prefix =
            &session->rules[USBDIAG_RULE_DEV_8B_PREFIX];
        state->prefix_seen = 0U;
        prefix->verdict = USBDIAG_NOT_OBSERVED;
        prefix->reason = USBDIAG_REASON_EVIDENCE_MISSING;
        prefix->reference_count = 0U;
        prefix->omitted_failure_reference_count = 0U;
        state->blocked_pass_mask |=
            USBDIAG_RULE_MASK(USBDIAG_RULE_DEV_8B_PREFIX);
    }
    for (event_index = 0U; event_index < USBDIAG_MAX_CONFIGS; event_index++)
    {
        usbdiag_config_record_t *record = &state->configurations[event_index];
        if (record->used && record->header_sequence == event_sequence)
        {
            record->header_missing = 1U;
            record->header_known = 0U;
            record->pending = 1U;
        }
    }
    for (rule_index = 0U; rule_index < USBDIAG_RULE_COUNT; rule_index++)
    {
        usbdiag_rule_result_t *result = &session->rules[rule_index];
        uint8_t ref_index;
        bool uses_event = false;
        for (ref_index = 0U; ref_index < result->reference_count; ref_index++)
            if (result->references[ref_index].event_sequence == event_sequence)
                uses_event = true;
        if (uses_event)
        {
            result->verdict = USBDIAG_NOT_OBSERVED;
            result->reason = USBDIAG_REASON_EVIDENCE_MISSING;
            result->reference_count = 0U;
            result->omitted_failure_reference_count = 0U;
            state->blocked_pass_mask |= USBDIAG_RULE_MASK(rule_index);
        }
    }
    state->quality_gap = 1U;
    refresh_pending_configuration_count(session);
    session->status = USBDIAG_SESSION_DEGRADED;
    return session->status;
}

usbdiag_session_status_t usbdiag_session_end(usbdiag_session_t *session,
                                             bool enumeration_succeeded)
{
    usbdiag_session_state_t *state;
    uint8_t index;
    bool failed_rule = false;
    bool unobserved_rule = false;

    if (session == NULL)
        return USBDIAG_SESSION_INVALID;
    state = session_state(session);
    if (state->magic != USBDIAG_SESSION_MAGIC || !state->active)
        return USBDIAG_SESSION_INVALID;
    if (state->ended)
        return session->status;

    state->ended = 1U;
    state->enumeration_succeeded = enumeration_succeeded ? 1U : 0U;
    if (session->rules[USBDIAG_RULE_EP0_STALL].verdict ==
            USBDIAG_NOT_OBSERVED && !state->required_request_seen)
        set_not_observed(session, USBDIAG_RULE_EP0_STALL,
                         USBDIAG_REASON_NOT_OBSERVED, 0U);
    else if (state->required_request_error &&
             session->rules[USBDIAG_RULE_EP0_STALL].verdict != USBDIAG_FAIL)
        set_not_observed(session, USBDIAG_RULE_EP0_STALL,
                         USBDIAG_REASON_TRANSFER_ERROR, state->last_sequence);

    if (session->pending_configuration_count > 0U)
    {
        set_not_observed(session, USBDIAG_RULE_CFG_TOTAL_LENGTH,
                         USBDIAG_REASON_NOT_OBSERVED, state->last_sequence);
        set_not_observed(session, USBDIAG_RULE_CFG_TREE_WALK,
                         USBDIAG_REASON_NOT_OBSERVED, state->last_sequence);
        set_not_observed(session, USBDIAG_RULE_CFG_INTERFACE_COUNT,
                         USBDIAG_REASON_NOT_OBSERVED, state->last_sequence);
        set_not_observed(session, USBDIAG_RULE_IF_ENDPOINT_COUNT,
                         USBDIAG_REASON_NOT_OBSERVED, state->last_sequence);
        set_not_observed(session, USBDIAG_RULE_EP_PACKET_SIZE,
                         USBDIAG_REASON_NOT_OBSERVED, state->last_sequence);
        set_not_observed(session, USBDIAG_RULE_EP_INTERVAL,
                         USBDIAG_REASON_NOT_OBSERVED, state->last_sequence);
    }

    for (index = 0U; index < USBDIAG_RULE_COUNT; index++)
    {
        if (session->rules[index].verdict == USBDIAG_FAIL)
            failed_rule = true;
        if (session->rules[index].verdict == USBDIAG_NOT_OBSERVED)
            unobserved_rule = true;
    }

    if (state->quality_gap)
        session->status = USBDIAG_SESSION_DEGRADED;
    else if (failed_rule)
        session->status = USBDIAG_SESSION_RULE_FAILURE;
    else if (!enumeration_succeeded)
        session->status = USBDIAG_SESSION_ENUMERATION_FAILED;
    else if (unobserved_rule)
        session->status = USBDIAG_SESSION_INCOMPLETE;
    else
        session->status = USBDIAG_SESSION_COMPLETE;
    return session->status;
}

usbdiag_session_status_t usbdiag_session_get_result(
    const usbdiag_session_t *session,
    usbdiag_rule_result_t results[USBDIAG_RULE_COUNT])
{
    const usbdiag_session_state_t *state;
    if (session == NULL || results == NULL)
        return USBDIAG_SESSION_INVALID;
    state = session_state_const(session);
    if (state->magic != USBDIAG_SESSION_MAGIC || !state->active)
        return USBDIAG_SESSION_INVALID;
    memcpy(results, session->rules, sizeof(session->rules));
    return session->status;
}

uint16_t usbdiag_session_get_pins(const usbdiag_session_t *session,
                                  uint32_t *event_sequences,
                                  uint16_t capacity)
{
    const usbdiag_session_state_t *state;
    uint32_t unique[USBDIAG_SESSION_EVENT_CAPACITY];
    uint16_t count = 0U;
    uint8_t rule_index;
    uint16_t config_index;
    if (session == NULL)
        return 0U;
    state = session_state_const(session);
    if (state->magic != USBDIAG_SESSION_MAGIC || !state->active)
        return 0U;

#define USBDIAG_ADD_PIN(sequence_value) do { \
    uint16_t _i; \
    bool _found = false; \
    for (_i = 0U; _i < count; _i++) \
        if (unique[_i] == (sequence_value)) \
            _found = true; \
    if (!_found) { \
        if (event_sequences != NULL && count < capacity) \
            event_sequences[count] = (sequence_value); \
        unique[count] = (sequence_value); \
        if (count < UINT16_MAX) count++; \
    } \
} while (0)

    for (rule_index = 0U; rule_index < USBDIAG_RULE_COUNT; rule_index++)
    {
        const usbdiag_rule_result_t *rule = &session->rules[rule_index];
        uint8_t ref_index;
        for (ref_index = 0U; ref_index < rule->reference_count; ref_index++)
            USBDIAG_ADD_PIN(rule->references[ref_index].event_sequence);
    }
    if (state->prefix_seen)
        USBDIAG_ADD_PIN(state->prefix_sequence);
    for (config_index = 0U; config_index < USBDIAG_MAX_CONFIGS; config_index++)
    {
        const usbdiag_config_record_t *record =
            &state->configurations[config_index];
        if (record->used && record->pending && record->header_sequence != 0U)
            USBDIAG_ADD_PIN(record->header_sequence);
    }
#undef USBDIAG_ADD_PIN
    return count;
}

uint32_t usbdiag_elapsed_us(uint32_t started_at_us, uint32_t completed_at_us)
{
    return (uint32_t)(completed_at_us - started_at_us);
}
