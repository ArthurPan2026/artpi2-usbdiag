/* SPDX-License-Identifier: Apache-2.0 */
#include <usbdiag_core.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_device_rules(void)
{
    const uint8_t prefix[8] = { 18, 1, 0, 2, 0, 0, 0, 64 };
    uint8_t descriptor[18] = { 18, 1, 0, 2, 0, 0, 0, 64 };

    assert(usbdiag_device_b_length_valid(18));
    assert(!usbdiag_device_b_length_valid(9));
    assert(usbdiag_device_descriptor_type_valid(1));
    assert(!usbdiag_device_descriptor_type_valid(2));
    assert(usbdiag_device_prefix_equal(prefix, descriptor, sizeof(descriptor)));
    descriptor[7] ^= 1U;
    assert(!usbdiag_device_prefix_equal(prefix, descriptor, sizeof(descriptor)));
    assert(!usbdiag_device_prefix_equal(prefix, descriptor, 7));
    assert(usbdiag_device_ep0_mps_valid(1, 8));
    assert(!usbdiag_device_ep0_mps_valid(1, 64));
    assert(usbdiag_device_ep0_mps_valid(2, 64));
    assert(!usbdiag_device_ep0_mps_valid(3, 8));
    assert(usbdiag_device_ep0_mps_valid(3, 64));
}

static void test_config_rules(void)
{
    const uint8_t header[9] = { 9, 2, 9, 0, 0, 1, 0, 0x80, 50 };
    uint8_t changed[9];

    assert(usbdiag_config_header_valid(header, sizeof(header)));
    assert(!usbdiag_config_header_valid(header, 1));
    memcpy(changed, header, sizeof(changed));
    changed[1] = 3;
    assert(!usbdiag_config_header_valid(changed, sizeof(changed)));
    assert(usbdiag_config_transfer_verdict(9, true, 9, 9) == USBDIAG_PASS);
    assert(usbdiag_config_transfer_verdict(9, true, 8, 9) == USBDIAG_FAIL);
    assert(usbdiag_config_transfer_verdict(64, true, 8, 9) == USBDIAG_FAIL);
    assert(usbdiag_config_transfer_verdict(64, false, 0, 9) == USBDIAG_NOT_OBSERVED);
    assert(usbdiag_config_transfer_verdict(64, true, 64, 8) == USBDIAG_FAIL);
    assert(usbdiag_ep0_required_request_verdict(true, true, false) == USBDIAG_PASS);
    assert(usbdiag_ep0_required_request_verdict(true, false, true) == USBDIAG_FAIL);
    assert(usbdiag_ep0_required_request_verdict(false, false, true) == USBDIAG_NOT_OBSERVED);
    assert(usbdiag_ep0_required_request_verdict(true, false, false) == USBDIAG_NOT_OBSERVED);
}

static void test_endpoint_rules(void)
{
    uint8_t endpoint[7] = { 7, 5, 0x81, 3, 8, 0, 10 };

    assert(usbdiag_endpoint_packet_size_valid(1, endpoint));
    assert(usbdiag_endpoint_packet_size_valid(2, endpoint));
    assert(usbdiag_endpoint_interval_valid(1, 3, 10));
    assert(usbdiag_endpoint_interval_valid(3, 3, 16));
    assert(!usbdiag_endpoint_interval_valid(3, 3, 17));
    assert(!usbdiag_endpoint_interval_valid(1, 1, 1));
    assert(!usbdiag_endpoint_interval_valid(4, 3, 1));
    endpoint[4] = 65;
    assert(!usbdiag_endpoint_packet_size_valid(2, endpoint));
    endpoint[3] = 2;
    endpoint[4] = 0;
    endpoint[5] = 2;
    assert(usbdiag_endpoint_packet_size_valid(3, endpoint));
    endpoint[5] = 0;
    assert(!usbdiag_endpoint_packet_size_valid(3, endpoint));
}

static void test_failure_reference_cap(void)
{
    usbdiag_failure_refs_t refs = { { 0 }, 0, 0 };
    uint16_t index;

    for (index = 0; index < 7; index++)
    {
        usbdiag_failure_ref_add(&refs, (uint16_t)(10U + index));
    }
    assert(refs.count == 4);
    assert(refs.total_failures == 7);
    assert(refs.offsets[0] == 10 && refs.offsets[3] == 13);
}

static void test_configuration_analysis(void)
{
    const uint8_t valid[] = {
        9, 2, 25, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 1, 3, 1, 1, 0,
        7, 5, 0x81, 3, 8, 0, 10,
    };
    uint8_t invalid_many[9 + 9 + 5 * 7] = {
        9, 2, sizeof(invalid_many), 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 5, 3, 1, 1, 0,
    };
    uint8_t malformed[sizeof(valid)];
    usbdiag_config_result_t result;
    uint8_t index;

    usbdiag_analyze_configuration(valid, sizeof(valid), true, sizeof(valid),
                                  true, sizeof(valid), USBDIAG_SPEED_FULL,
                                  &result);
    assert(result.rules[USBDIAG_CFG_HEADER].verdict == USBDIAG_PASS);
    assert(result.rules[USBDIAG_CFG_TOTAL_LENGTH].verdict == USBDIAG_PASS);
    assert(result.rules[USBDIAG_CFG_TREE_WALK].verdict == USBDIAG_PASS);
    assert(result.rules[USBDIAG_CFG_INTERFACE_COUNT].verdict == USBDIAG_PASS);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_COUNT].verdict == USBDIAG_PASS);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_PACKET_SIZE].verdict == USBDIAG_PASS);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_INTERVAL].verdict == USBDIAG_PASS);

    memcpy(malformed, valid, sizeof(valid));
    malformed[13] = 0U;
    usbdiag_analyze_configuration(malformed, sizeof(malformed), true,
                                  sizeof(malformed), true, sizeof(malformed),
                                  USBDIAG_SPEED_FULL, &result);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_COUNT].verdict == USBDIAG_FAIL);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_COUNT].failures.count == 1);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_COUNT].failures.offsets[0] == 13);

    usbdiag_analyze_configuration(valid, 9, true, 9, true, 9,
                                  USBDIAG_SPEED_FULL, &result);
    assert(result.rules[USBDIAG_CFG_TOTAL_LENGTH].verdict == USBDIAG_NOT_OBSERVED);
    assert(result.rules[USBDIAG_CFG_TREE_WALK].verdict == USBDIAG_NOT_OBSERVED);

    memcpy(malformed, valid, sizeof(valid));
    malformed[9] = 1;
    usbdiag_analyze_configuration(malformed, sizeof(malformed), true,
                                  sizeof(malformed), true, sizeof(malformed),
                                  USBDIAG_SPEED_FULL, &result);
    assert(result.rules[USBDIAG_CFG_TREE_WALK].verdict == USBDIAG_FAIL);
    assert(result.rules[USBDIAG_CFG_TREE_WALK].failures.count == 1);
    assert(result.rules[USBDIAG_CFG_TREE_WALK].failures.offsets[0] == 9);

    for (index = 0; index < 5; index++)
    {
        uint16_t offset = (uint16_t)(18U + 7U * index);
        invalid_many[offset] = 7;
        invalid_many[offset + 1U] = 5;
        invalid_many[offset + 2U] = (uint8_t)(0x81U + index);
        invalid_many[offset + 3U] = 2;
        invalid_many[offset + 4U] = 65;
        invalid_many[offset + 5U] = 0;
        invalid_many[offset + 6U] = 0;
    }
    usbdiag_analyze_configuration(invalid_many, sizeof(invalid_many), true,
                                  sizeof(invalid_many), true,
                                  sizeof(invalid_many), USBDIAG_SPEED_FULL,
                                  &result);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_PACKET_SIZE].verdict == USBDIAG_FAIL);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_PACKET_SIZE].failures.count == 4);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_PACKET_SIZE].failures.total_failures == 5);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_PACKET_SIZE].failures.offsets[0] == 22);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_PACKET_SIZE].failures.offsets[3] == 43);
    assert(result.rules[USBDIAG_CFG_ENDPOINT_COUNT].verdict == USBDIAG_PASS);
}

static void test_fixed_seed_boundaries(void)
{
    uint32_t state = 0x51A7C0DEU;
    uint32_t index;
    uint8_t endpoint[7] = { 7, 5, 0x81, 3, 8, 0, 10 };
    uint8_t candidate[64];
    usbdiag_config_result_t result;

    for (index = 0; index < 10000U; index++)
    {
        uint8_t speed;
        uint8_t type;
        uint8_t interval;
        uint16_t request;
        uint16_t transfer;
        uint16_t total;

        state = state * 1664525U + 1013904223U;
        speed = (uint8_t)(state >> 24U);
        type = (uint8_t)((state >> 16U) & 3U);
        interval = (uint8_t)(state >> 8U);
        request = (uint16_t)state;
        state = state * 1664525U + 1013904223U;
        transfer = (uint16_t)state;
        total = (uint16_t)(state >> 16U);
        endpoint[3] = type;
        endpoint[4] = (uint8_t)state;
        endpoint[5] = (uint8_t)(state >> 8U);
        endpoint[6] = interval;
        for (uint8_t byte = 0U; byte < sizeof(candidate); byte++)
        {
            state = state * 1664525U + 1013904223U;
            candidate[byte] = (uint8_t)(state >> 24U);
        }

        (void)usbdiag_speed_known(speed);
        (void)usbdiag_device_ep0_mps_valid(speed, endpoint[4]);
        (void)usbdiag_endpoint_packet_size_valid(speed, endpoint);
        (void)usbdiag_endpoint_interval_valid(speed, type, interval);
        (void)usbdiag_config_transfer_verdict(request, (state & 1U) != 0U,
                                              transfer, total);
        usbdiag_analyze_configuration(
            candidate, (uint16_t)(state % (sizeof(candidate) + 1U)),
            (state & 2U) != 0U, request, (state & 4U) != 0U,
            transfer, speed, &result);
    }
}

int usbdiag_run_core_tests(void)
{
    test_device_rules();
    test_config_rules();
    test_endpoint_rules();
    test_failure_reference_cap();
    test_configuration_analysis();
    test_fixed_seed_boundaries();
    return 0;
}

#ifndef USBDIAG_LIBRARY_TESTS_NO_MAIN
int main(void)
{
    int result = usbdiag_run_core_tests();
    puts("usbdiag_core: PASS (rules, reference cap, fixed-seed 10000 inputs)");
    return result;
}
#endif
