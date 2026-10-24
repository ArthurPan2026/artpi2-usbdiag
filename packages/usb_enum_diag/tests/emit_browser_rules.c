/* SPDX-License-Identifier: Apache-2.0 */
#include "usbdiag_core.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct browser_rule
{
    const char *id;
    const char *clause;
    const char *expected;
    const char *observed;
    const char *reason;
    usbdiag_verdict_t verdict;
    uint32_t event_sequence;
    uint16_t offsets[USBDIAG_REFERENCE_CAPACITY];
    uint16_t lengths[USBDIAG_REFERENCE_CAPACITY];
    uint8_t reference_count;
    uint16_t omitted;
    uint32_t reference_sequences[USBDIAG_REFERENCE_CAPACITY];
};

static const char *verdict_name(usbdiag_verdict_t verdict)
{
    switch (verdict)
    {
    case USBDIAG_PASS: return "PASS";
    case USBDIAG_FAIL: return "FAIL";
    case USBDIAG_NOT_APPLICABLE: return "NOT_APPLICABLE";
    default: return "NOT_OBSERVED";
    }
}

static void reference(struct browser_rule *rule, uint32_t sequence,
                      uint16_t offset, uint16_t length)
{
    if (rule->reference_count < USBDIAG_REFERENCE_CAPACITY)
    {
        uint8_t index = rule->reference_count++;
        rule->event_sequence = sequence;
        rule->reference_sequences[index] = sequence;
        rule->offsets[index] = offset;
        rule->lengths[index] = length;
    }
}

static void emit_rule(const struct browser_rule *rule, bool first)
{
    uint8_t index;

    printf("%s{\"id\":\"%s\",\"clause\":\"%s\",\"severity\":2,"
           "\"verdict\":\"%s\",\"event_sequence\":%lu,"
           "\"expected\":\"%s\",\"observed\":\"%s\","
           "\"reason_code\":\"%s\","
           "\"omitted_failure_reference_count\":%u,\"evidence_refs\":[",
           first ? "" : ",", rule->id, rule->clause,
           verdict_name(rule->verdict), (unsigned long)rule->event_sequence,
           rule->expected, rule->observed, rule->reason,
           (unsigned int)rule->omitted);
    for (index = 0U; index < rule->reference_count; index++)
    {
        printf("%s{\"event_sequence\":%lu,\"byte_offset\":%u,"
               "\"byte_length\":%u,\"role\":\"core_fixture\"}",
               index == 0U ? "" : ",",
               (unsigned long)rule->reference_sequences[index],
               (unsigned int)rule->offsets[index],
               (unsigned int)rule->lengths[index]);
    }
    printf("]}");
}

int main(int argc, char **argv)
{
    uint8_t device[18] =
    {
        18, 1, 0, 2, 0, 0, 0, 64, 0x17, 0x27, 0x91, 0x50, 0x14, 3, 1, 2, 3, 1,
    };
    uint8_t prefix[8];
    uint8_t config[25] =
    {
        9, 2, 25, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 1, 3, 1, 1, 0,
        7, 5, 0x81, 3, 8, 0, 10,
    };
    struct browser_rule rules[12] =
    {
        { "DEV_B_LENGTH", "USB 2.0 9.6.1", "bLength=18", "bLength=18", "MATCH", USBDIAG_PASS, 4, {0}, {0}, 0, 0 },
        { "DEV_TYPE", "USB 2.0 9.4.3, 9.6.1", "type=1", "type=1", "MATCH", USBDIAG_PASS, 4, {0}, {0}, 0, 0 },
        { "DEV_8B_PREFIX", "USB 2.0 9.4.3, 9.6.1", "prefix matches", "prefix matches", "MATCH", USBDIAG_PASS, 4, {0}, {0}, 0, 0 },
        { "DEV_FIELDS", "USB 2.0 9.6.1", "valid EP0 MPS", "bMaxPacketSize0=64", "MATCH", USBDIAG_PASS, 4, {0}, {0}, 0, 0 },
        { "CFG_B_LENGTH", "USB 2.0 9.6.3", "length=9 type=2", "length=9 type=2", "MATCH", USBDIAG_PASS, 6, {0}, {0}, 0, 0 },
        { "CFG_TOTAL_LENGTH", "USB 2.0 9.4.3, 9.6.3", "transfer length matches total", "wTotalLength=25 transfer=25", "MATCH", USBDIAG_PASS, 6, {0}, {0}, 0, 0 },
        { "CFG_TREE_WALK", "USB 2.0 9.6.3-9.6.6", "bounded descriptors", "tree valid", "MATCH", USBDIAG_PASS, 6, {0}, {0}, 0, 0 },
        { "CFG_INTERFACE_COUNT", "USB 2.0 9.6.3", "declared unique interface count", "unique interfaces=1", "MATCH", USBDIAG_PASS, 6, {0}, {0}, 0, 0 },
        { "IF_ENDPOINT_COUNT", "USB 2.0 9.6.5", "endpoints match each alternate", "all endpoint counts match", "MATCH", USBDIAG_PASS, 6, {0}, {0}, 0, 0 },
        { "EP_PACKET_SIZE", "USB 2.0 9.6.6, Table 9-13", "valid for speed/type", "wMaxPacketSize=8", "MATCH", USBDIAG_PASS, 6, {0}, {0}, 0, 0 },
        { "EP_INTERVAL", "USB 2.0 9.6.6", "valid periodic interval", "bInterval=10", "MATCH", USBDIAG_PASS, 6, {0}, {0}, 0, 0 },
        { "EP0_STALL", "USB 2.0 9.4.3, 9.4.7", "required requests do not STALL", "required requests completed", "MATCH", USBDIAG_PASS, 9, {0}, {0}, 0, 0 },
    };
    usbdiag_config_result_t config_result;
    const char *scenario = argc > 1 ? argv[1] : "valid";
    bool capture_complete = true;
    uint16_t config_capture_length = sizeof(config);
    uint8_t index;

    if (strcmp(scenario, "empty") == 0)
    {
        for (index = 0U; index < 12U; index++)
        {
            rules[index].verdict = USBDIAG_NOT_OBSERVED;
            rules[index].event_sequence = 0U;
        }
    }
    else
    {
        if (strcmp(scenario, "device-fail") == 0)
        {
            device[0] = 9;
        }
        if (strcmp(scenario, "prefix-fail") == 0)
        {
            device[0] ^= 1U;
        }
        memcpy(prefix, device, sizeof(prefix));
        if (strcmp(scenario, "prefix-fail") == 0)
        {
            prefix[0] ^= 1U;
        }

        rules[0].verdict = usbdiag_device_b_length_valid(device[0]) ?
                           USBDIAG_PASS : USBDIAG_FAIL;
        rules[0].observed = device[0] == 18U ? "bLength=18" : "bLength=9";
        rules[0].reason = rules[0].verdict == USBDIAG_PASS ? "MATCH" : "MISMATCH";
        reference(&rules[0], 4U, 0U, 1U);

        rules[1].verdict = usbdiag_device_descriptor_type_valid(device[1]) ?
                           USBDIAG_PASS : USBDIAG_FAIL;
        reference(&rules[1], 4U, 1U, 1U);

        rules[2].verdict = usbdiag_device_prefix_equal(prefix, device, sizeof(device)) ?
                           USBDIAG_PASS : USBDIAG_FAIL;
        rules[2].reason = rules[2].verdict == USBDIAG_PASS ? "MATCH" : "MISMATCH";
        rules[2].reference_count = 0U;
        reference(&rules[2], 2U, 0U, 8U);
        reference(&rules[2], 4U, 0U, 8U);

        rules[3].verdict = usbdiag_device_ep0_mps_valid(
            USBDIAG_SPEED_FULL, device[7]) ? USBDIAG_PASS : USBDIAG_FAIL;
        rules[3].observed = "bMaxPacketSize0=64";
        reference(&rules[3], 4U, 7U, 1U);

        if (strcmp(scenario, "config-fail") == 0)
        {
            config[22] = 65U;
            rules[9].observed = "wMaxPacketSize=65";
        }
        if (strcmp(scenario, "truncated") == 0)
        {
            capture_complete = false;
            config_capture_length = 16U;
        }
        usbdiag_analyze_configuration(config, config_capture_length,
                                      capture_complete,
                                      sizeof(config), true, sizeof(config),
                                      USBDIAG_SPEED_FULL, &config_result);
        for (index = 0U; index < USBDIAG_CFG_RULE_COUNT; index++)
        {
            uint8_t output_index = (uint8_t)(index + 4U);
            const usbdiag_config_rule_result_t *source =
                &config_result.rules[index];
            uint8_t ref_index;

            rules[output_index].verdict = source->verdict;
            rules[output_index].reference_count = 0U;
            rules[output_index].event_sequence = 6U;
            if (source->verdict == USBDIAG_FAIL)
            {
                uint16_t length = index == USBDIAG_CFG_TOTAL_LENGTH ||
                                  index == USBDIAG_CFG_ENDPOINT_PACKET_SIZE ?
                                  2U : 1U;
                for (ref_index = 0U; ref_index < source->failures.count; ref_index++)
                {
                    reference(&rules[output_index], 6U,
                              source->failures.offsets[ref_index], length);
                }
                rules[output_index].omitted =
                    source->failures.total_failures > source->failures.count ?
                    (uint16_t)(source->failures.total_failures - source->failures.count) : 0U;
                rules[output_index].reason = "MISMATCH";
            }
            else if (source->verdict == USBDIAG_PASS)
            {
                uint16_t offset = index == USBDIAG_CFG_TOTAL_LENGTH ? 2U : 0U;
                uint16_t length = index == USBDIAG_CFG_HEADER ? 2U :
                                  (index == USBDIAG_CFG_TOTAL_LENGTH ? 2U :
                                   sizeof(config));
                reference(&rules[output_index], 6U, offset, length);
            }
        }
    }

    printf("{\"rule_results\":[");
    for (index = 0U; index < 12U; index++)
    {
        emit_rule(&rules[index], index == 0U);
    }
    puts("]}");
    return 0;
}
