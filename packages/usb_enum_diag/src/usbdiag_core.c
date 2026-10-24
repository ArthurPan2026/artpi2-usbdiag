/* SPDX-License-Identifier: Apache-2.0 */
#include "usbdiag_core.h"

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

bool usbdiag_device_b_length_valid(uint8_t value)
{
    return value == 18U;
}

bool usbdiag_device_descriptor_type_valid(uint8_t value)
{
    return value == 0x01U;
}

bool usbdiag_device_prefix_equal(const uint8_t *prefix,
                                 const uint8_t *descriptor,
                                 uint16_t length)
{
    uint8_t index;

    if (prefix == 0 || descriptor == 0 || length < 8U)
    {
        return false;
    }
    for (index = 0U; index < 8U; index++)
    {
        if (prefix[index] != descriptor[index])
        {
            return false;
        }
    }
    return true;
}

bool usbdiag_device_ep0_mps_valid(uint8_t speed, uint8_t mps0)
{
    if (speed == USBDIAG_SPEED_LOW)
    {
        return mps0 == 8U;
    }
    if (speed == USBDIAG_SPEED_FULL)
    {
        return mps0 == 8U || mps0 == 16U || mps0 == 32U || mps0 == 64U;
    }
    if (speed == USBDIAG_SPEED_HIGH)
    {
        return mps0 == 64U;
    }
    return false;
}

bool usbdiag_config_header_valid(const uint8_t *data, uint16_t length)
{
    return data != 0 && length >= 2U && data[0] == 9U && data[1] == 0x02U;
}

usbdiag_verdict_t usbdiag_config_transfer_verdict(uint16_t requested_length,
                                                  bool transfer_length_known,
                                                  uint16_t transfer_length,
                                                  uint16_t declared_length)
{
    if (declared_length < 9U)
    {
        return USBDIAG_FAIL;
    }
    if (!transfer_length_known || requested_length < declared_length)
    {
        return USBDIAG_NOT_OBSERVED;
    }
    return transfer_length == declared_length ? USBDIAG_PASS : USBDIAG_FAIL;
}

usbdiag_verdict_t usbdiag_ep0_required_request_verdict(bool required_request,
                                                       bool completed,
                                                       bool stalled)
{
    if (!required_request || (!completed && !stalled))
    {
        return USBDIAG_NOT_OBSERVED;
    }
    return stalled ? USBDIAG_FAIL : USBDIAG_PASS;
}

bool usbdiag_speed_known(uint8_t speed)
{
    return speed == USBDIAG_SPEED_LOW || speed == USBDIAG_SPEED_FULL ||
           speed == USBDIAG_SPEED_HIGH;
}

bool usbdiag_endpoint_packet_size_valid(uint8_t speed,
                                        const uint8_t endpoint_descriptor[7])
{
    uint8_t transfer_type;
    uint16_t encoded;
    uint16_t packet_size;
    uint8_t transactions;

    if (endpoint_descriptor == 0)
    {
        return false;
    }
    transfer_type = endpoint_descriptor[3] & 0x03U;
    encoded = read_le16(endpoint_descriptor + 4U);
    packet_size = encoded & 0x07FFU;
    transactions = (uint8_t)((encoded >> 11U) & 0x03U);

    if (speed == USBDIAG_SPEED_LOW)
    {
        return transfer_type == 0x03U && packet_size == 8U &&
               transactions == 0U;
    }
    if (speed == USBDIAG_SPEED_FULL)
    {
        if (transactions != 0U)
        {
            return false;
        }
        if (transfer_type == 0x02U)
        {
            return packet_size == 8U || packet_size == 16U ||
                   packet_size == 32U || packet_size == 64U;
        }
        if (transfer_type == 0x03U)
        {
            return packet_size > 0U && packet_size <= 64U;
        }
        if (transfer_type == 0x01U)
        {
            return packet_size > 0U && packet_size <= 1023U;
        }
        return false;
    }
    if (speed == USBDIAG_SPEED_HIGH)
    {
        if (transfer_type == 0x02U)
        {
            return packet_size == 512U && transactions == 0U;
        }
        if (transfer_type == 0x01U || transfer_type == 0x03U)
        {
            return packet_size > 0U && packet_size <= 1024U &&
                   transactions <= 2U;
        }
    }
    return false;
}

bool usbdiag_endpoint_interval_valid(uint8_t speed,
                                     uint8_t transfer_type,
                                     uint8_t interval)
{
    if (!usbdiag_speed_known(speed) || interval == 0U)
    {
        return false;
    }
    if (transfer_type == 0x01U)
    {
        return speed != USBDIAG_SPEED_LOW && interval <= 16U;
    }
    if (transfer_type == 0x03U)
    {
        return speed != USBDIAG_SPEED_HIGH || interval <= 16U;
    }
    return true;
}

void usbdiag_failure_ref_add(usbdiag_failure_refs_t *refs, uint16_t offset)
{
    if (refs == 0)
    {
        return;
    }
    if (refs->count < USBDIAG_REFERENCE_CAPACITY)
    {
        refs->offsets[refs->count++] = offset;
    }
    if (refs->total_failures != UINT16_MAX)
    {
        refs->total_failures++;
    }
}

struct interface_group
{
    uint8_t number;
    uint8_t alternate_setting;
    uint8_t declared_endpoints;
    uint16_t observed_endpoints;
    uint16_t declared_offset;
};

static void config_result_init(usbdiag_config_result_t *result)
{
    uint8_t index;

    for (index = 0U; index < USBDIAG_CFG_RULE_COUNT; index++)
    {
        result->rules[index].verdict = USBDIAG_NOT_OBSERVED;
        result->rules[index].failures.count = 0U;
        result->rules[index].failures.total_failures = 0U;
    }
    result->declared_total_length = 0U;
    result->unique_interface_count = 0U;
    result->interface_setting_count = 0U;
    result->endpoint_count = 0U;
}

void usbdiag_analyze_configuration(const uint8_t *data,
                                   uint16_t captured_length,
                                   bool capture_complete,
                                   uint16_t requested_length,
                                   bool transfer_length_known,
                                   uint16_t transfer_length,
                                   uint8_t speed,
                                   usbdiag_config_result_t *result)
{
    struct interface_group groups[32];
    uint8_t interface_seen[256] = { 0U };
    uint8_t group_count = 0U;
    int16_t current_group = -1;
    uint16_t unique_count = 0U;
    uint16_t offset = 0U;
    uint16_t tree_failure_offset = 0U;
    uint16_t tree_failure_length = 1U;
    bool tree_ok = true;
    bool group_overflow = false;
    bool endpoint_orphaned = false;
    bool packet_seen = false;
    bool interval_seen = false;
    uint16_t endpoint_count = 0U;
    uint8_t index;
    uint16_t total_length;

    if (result == 0)
    {
        return;
    }
    config_result_init(result);
    if (data == 0 || captured_length < 2U)
    {
        return;
    }

    if (data[0] == 9U && data[1] == 0x02U)
    {
        result->rules[USBDIAG_CFG_HEADER].verdict = USBDIAG_PASS;
    }
    else
    {
        result->rules[USBDIAG_CFG_HEADER].verdict = USBDIAG_FAIL;
        if (data[0] != 9U)
        {
            usbdiag_failure_ref_add(
                &result->rules[USBDIAG_CFG_HEADER].failures, 0U);
        }
        if (data[1] != 0x02U)
        {
            usbdiag_failure_ref_add(
                &result->rules[USBDIAG_CFG_HEADER].failures, 1U);
        }
    }
    if (transfer_length_known && requested_length >= 9U &&
        transfer_length < 9U)
    {
        result->rules[USBDIAG_CFG_HEADER].verdict = USBDIAG_FAIL;
    }

    if (captured_length < 4U)
    {
        return;
    }
    total_length = read_le16(data + 2U);
    result->declared_total_length = total_length;
    result->rules[USBDIAG_CFG_TOTAL_LENGTH].verdict =
        usbdiag_config_transfer_verdict(requested_length,
                                        transfer_length_known,
                                        transfer_length, total_length);
    if (result->rules[USBDIAG_CFG_TOTAL_LENGTH].verdict == USBDIAG_FAIL)
    {
        usbdiag_failure_ref_add(
            &result->rules[USBDIAG_CFG_TOTAL_LENGTH].failures, 2U);
    }
    if (total_length < 9U)
    {
        return;
    }
    if (!capture_complete || captured_length < total_length)
    {
        return;
    }

    while (offset < total_length)
    {
        uint8_t descriptor_length;
        uint8_t descriptor_type;

        if ((uint16_t)(total_length - offset) < 2U)
        {
            tree_ok = false;
            tree_failure_offset = offset;
            tree_failure_length = (uint16_t)(total_length - offset);
            break;
        }
        descriptor_length = data[offset];
        descriptor_type = data[offset + 1U];
        if (descriptor_length < 2U ||
            (uint16_t)descriptor_length > (uint16_t)(total_length - offset))
        {
            tree_ok = false;
            tree_failure_offset = offset;
            tree_failure_length = 2U;
            break;
        }
        if ((offset == 0U &&
             (descriptor_type != 0x02U || descriptor_length != 9U)) ||
            (descriptor_type == 0x04U && descriptor_length != 9U) ||
            (descriptor_type == 0x05U && descriptor_length < 7U) ||
            (descriptor_type == 0x21U && descriptor_length < 9U))
        {
            tree_ok = false;
            tree_failure_offset = offset;
            tree_failure_length = 2U;
            break;
        }

        if (descriptor_type == 0x04U)
        {
            uint8_t number = data[offset + 2U];

            if (!interface_seen[number])
            {
                interface_seen[number] = 1U;
                unique_count++;
            }
            if (group_count < (uint8_t)(sizeof(groups) / sizeof(groups[0])))
            {
                groups[group_count].number = number;
                groups[group_count].alternate_setting = data[offset + 3U];
                groups[group_count].declared_endpoints = data[offset + 4U];
                groups[group_count].observed_endpoints = 0U;
                groups[group_count].declared_offset = (uint16_t)(offset + 4U);
                current_group = (int16_t)group_count;
                group_count++;
            }
            else
            {
                group_overflow = true;
                current_group = -1;
            }
        }
        else if (descriptor_type == 0x05U && descriptor_length >= 7U)
        {
            uint8_t transfer_type = data[offset + 3U] & 0x03U;

            endpoint_count++;
            if (current_group >= 0)
            {
                groups[current_group].observed_endpoints++;
            }
            else
            {
                endpoint_orphaned = true;
                usbdiag_failure_ref_add(
                    &result->rules[USBDIAG_CFG_ENDPOINT_COUNT].failures,
                    (uint16_t)(offset + 2U));
            }

            if (usbdiag_speed_known(speed))
            {
                packet_seen = true;
                if (!usbdiag_endpoint_packet_size_valid(speed, data + offset))
                {
                    usbdiag_failure_ref_add(
                        &result->rules[USBDIAG_CFG_ENDPOINT_PACKET_SIZE].failures,
                        (uint16_t)(offset + 4U));
                }
            }
            if (transfer_type == 0x01U || transfer_type == 0x03U)
            {
                interval_seen = true;
                if (usbdiag_speed_known(speed) &&
                    !usbdiag_endpoint_interval_valid(
                        speed, transfer_type, data[offset + 6U]))
                {
                    usbdiag_failure_ref_add(
                        &result->rules[USBDIAG_CFG_ENDPOINT_INTERVAL].failures,
                        (uint16_t)(offset + 6U));
                }
            }
        }
        offset = (uint16_t)(offset + descriptor_length);
    }

    result->unique_interface_count = unique_count;
    result->interface_setting_count = group_count;
    result->endpoint_count = endpoint_count;
    if (!tree_ok || offset != total_length)
    {
        result->rules[USBDIAG_CFG_TREE_WALK].verdict = USBDIAG_FAIL;
        if (tree_failure_length != 0U &&
            (uint32_t)tree_failure_offset + tree_failure_length <= captured_length)
        {
            usbdiag_failure_ref_add(
                &result->rules[USBDIAG_CFG_TREE_WALK].failures,
                tree_failure_offset);
        }
        return;
    }
    result->rules[USBDIAG_CFG_TREE_WALK].verdict = USBDIAG_PASS;

    result->rules[USBDIAG_CFG_INTERFACE_COUNT].verdict =
        unique_count == data[4] ? USBDIAG_PASS : USBDIAG_FAIL;
    if (unique_count != data[4])
    {
        usbdiag_failure_ref_add(
            &result->rules[USBDIAG_CFG_INTERFACE_COUNT].failures, 4U);
    }

    if (group_overflow)
    {
        result->rules[USBDIAG_CFG_ENDPOINT_COUNT].verdict =
            USBDIAG_NOT_OBSERVED;
    }
    else
    {
        for (index = 0U; index < group_count; index++)
        {
            if (groups[index].observed_endpoints !=
                groups[index].declared_endpoints)
            {
                usbdiag_failure_ref_add(
                    &result->rules[USBDIAG_CFG_ENDPOINT_COUNT].failures,
                    groups[index].declared_offset);
            }
        }
        if (endpoint_orphaned ||
            result->rules[USBDIAG_CFG_ENDPOINT_COUNT].failures.total_failures != 0U)
        {
            result->rules[USBDIAG_CFG_ENDPOINT_COUNT].verdict = USBDIAG_FAIL;
        }
        else
        {
            result->rules[USBDIAG_CFG_ENDPOINT_COUNT].verdict = USBDIAG_PASS;
        }
    }

    if (!usbdiag_speed_known(speed))
    {
        result->rules[USBDIAG_CFG_ENDPOINT_PACKET_SIZE].verdict =
            USBDIAG_NOT_APPLICABLE;
    }
    else if (!packet_seen)
    {
        result->rules[USBDIAG_CFG_ENDPOINT_PACKET_SIZE].verdict =
            USBDIAG_NOT_APPLICABLE;
    }
    else
    {
        result->rules[USBDIAG_CFG_ENDPOINT_PACKET_SIZE].verdict =
            result->rules[USBDIAG_CFG_ENDPOINT_PACKET_SIZE].failures.total_failures ?
            USBDIAG_FAIL : USBDIAG_PASS;
    }

    if (!usbdiag_speed_known(speed) || !interval_seen)
    {
        result->rules[USBDIAG_CFG_ENDPOINT_INTERVAL].verdict =
            USBDIAG_NOT_APPLICABLE;
    }
    else
    {
        result->rules[USBDIAG_CFG_ENDPOINT_INTERVAL].verdict =
            result->rules[USBDIAG_CFG_ENDPOINT_INTERVAL].failures.total_failures ?
            USBDIAG_FAIL : USBDIAG_PASS;
    }
}
