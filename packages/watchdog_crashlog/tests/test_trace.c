/* SPDX-License-Identifier: Apache-2.0 */
#include "watchdog_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static watchdog_trace_ring_t ring;

static void append_sample(uint32_t sequence)
{
    CHECK(watchdog_trace_append(&ring, sequence,
                                sequence * 10U,
                                0x70000000U + sequence * 2U,
                                0x70001000U + sequence * 2U,
                                0x24001000U + sequence * 4U,
                                0x01000000U | (sequence & 0x1FFU)) ==
          WATCHDOG_TRACE_OK);
}

static void test_crc_vector_and_layout(void)
{
    static const uint8_t vector[] = "123456789";

    CHECK(sizeof(watchdog_trace_header_t) == 32U);
    CHECK(sizeof(watchdog_trace_sample_t) == 24U);
    CHECK(offsetof(watchdog_trace_sample_t, sequence_commit) == 20U);
    CHECK(sizeof(ring) == 32U +
          (WATCHDOG_TRACE_CAPACITY * sizeof(watchdog_trace_sample_t)));
    CHECK(watchdog_trace_crc32(vector, sizeof(vector) - 1U) ==
          0xCBF43926UL);
}

static void test_initialize_and_append(void)
{
    watchdog_trace_sample_t sample;
    watchdog_trace_scan_result_t scan;
    uint32_t index;

    memset(&ring, 0xA5, sizeof(ring));
    CHECK(watchdog_trace_initialize(&ring, 17U, 0x12345678U) ==
          WATCHDOG_TRACE_OK);
    CHECK(watchdog_trace_header_valid(&ring.header));
    CHECK(ring.header.boot_sequence == 17U);
    CHECK(ring.header.fingerprint_crc32 == 0x12345678U);
    CHECK(ring.header.next_index == 0U);
    for (index = 0U; index < WATCHDOG_TRACE_CAPACITY; ++index)
    {
        CHECK(ring.samples[index].sequence_commit == 0U);
    }
    CHECK(watchdog_trace_scan(&ring, &scan) == WATCHDOG_TRACE_OK);
    CHECK(scan.valid_count == 0U);
    CHECK(scan.oldest_sequence == 0U);
    CHECK(scan.newest_sequence == 0U);
    CHECK(scan.oldest_index == WATCHDOG_TRACE_INDEX_NONE);
    CHECK(scan.newest_index == WATCHDOG_TRACE_INDEX_NONE);

    CHECK(watchdog_trace_append(&ring, 0U, 1U, 2U, 3U, 4U, 5U) ==
          WATCHDOG_TRACE_ERROR_SEQUENCE);
    CHECK(watchdog_trace_append(&ring, 1U, 100U, 0x70000100U,
                                0x70000200U, 0x24001000U,
                                0x01000003U) == WATCHDOG_TRACE_OK);
    CHECK(watchdog_trace_get_sequence(&ring, 1U, &sample) ==
          WATCHDOG_TRACE_OK);
    CHECK(sample.cycle_count == 100U);
    CHECK(sample.pc == 0x70000100U);
    CHECK(sample.lr == 0x70000200U);
    CHECK(sample.sp == 0x24001000U);
    CHECK(sample.packed_exc_xpsr == 0x01000003U);
    CHECK(sample.sequence_commit == 1U);
    CHECK(ring.header.next_index == 1U);
    CHECK(watchdog_trace_header_valid(&ring.header));

    CHECK(watchdog_trace_scan(&ring, &scan) == WATCHDOG_TRACE_OK);
    CHECK(scan.valid_count == 1U);
    CHECK(scan.rejected_count == 0U);
    CHECK(scan.oldest_sequence == 1U && scan.oldest_index == 0U);
    CHECK(scan.newest_sequence == 1U && scan.newest_index == 0U);
    CHECK(scan.samples_crc32 != 0U);
}

static void test_capacity_wrap_and_overwrite(void)
{
    watchdog_trace_scan_result_t scan;
    watchdog_trace_sample_t sample;
    uint32_t sequence;

    CHECK(watchdog_trace_initialize(&ring, 18U, 0x87654321U) ==
          WATCHDOG_TRACE_OK);
    for (sequence = 1U; sequence <= WATCHDOG_TRACE_CAPACITY + 1U;
         ++sequence)
    {
        append_sample(sequence);
    }

    CHECK(watchdog_trace_scan(&ring, &scan) == WATCHDOG_TRACE_OK);
    CHECK(scan.valid_count == WATCHDOG_TRACE_CAPACITY);
    CHECK(scan.rejected_count == 0U);
    CHECK(scan.oldest_sequence == 2U);
    CHECK(scan.oldest_index == 1U);
    CHECK(scan.newest_sequence == WATCHDOG_TRACE_CAPACITY + 1U);
    CHECK(scan.newest_index == 0U);
    CHECK(scan.next_index == 1U);
    CHECK(watchdog_trace_get_sequence(&ring, 1U, &sample) ==
          WATCHDOG_TRACE_ERROR_NOT_FOUND);
    CHECK(watchdog_trace_get_sequence(
              &ring, WATCHDOG_TRACE_CAPACITY + 1U, &sample) ==
          WATCHDOG_TRACE_OK);
    CHECK(sample.sequence_commit == WATCHDOG_TRACE_CAPACITY + 1U);
}

static void test_sequence_wraparound(void)
{
    watchdog_trace_scan_result_t scan;
    uint32_t sequence = UINT32_MAX - 3U;
    uint32_t sample_index;

    CHECK(watchdog_trace_initialize(&ring, 19U, 0xAABBCCDDU) ==
          WATCHDOG_TRACE_OK);
    for (sample_index = 0U;
         sample_index < WATCHDOG_TRACE_CAPACITY + 1U;
         ++sample_index)
    {
        append_sample(sequence);
        sequence = watchdog_trace_next_sequence(sequence);
    }

    CHECK(watchdog_trace_next_sequence(0U) == 1U);
    CHECK(watchdog_trace_next_sequence(UINT32_MAX - 1U) == UINT32_MAX);
    CHECK(watchdog_trace_next_sequence(UINT32_MAX) == 1U);
    CHECK(watchdog_trace_scan(&ring, &scan) == WATCHDOG_TRACE_OK);
    CHECK(scan.valid_count == WATCHDOG_TRACE_CAPACITY);
    CHECK(scan.oldest_sequence == UINT32_MAX - 2U);
    CHECK(scan.oldest_index == 1U);
    CHECK(scan.newest_sequence == WATCHDOG_TRACE_CAPACITY - 3U);
    CHECK(scan.newest_index == 0U);
    CHECK(scan.next_index == 1U);
}

static void test_invalid_slots_and_commit_marker(void)
{
    watchdog_trace_scan_result_t scan;
    watchdog_trace_sample_t sample;

    CHECK(watchdog_trace_initialize(&ring, 20U, 0xCAFEBABEU) ==
          WATCHDOG_TRACE_OK);
    append_sample(1U);
    append_sample(2U);

    /* A slot interrupted before commit remains invisible even with payload. */
    ring.samples[2U].cycle_count = 444U;
    ring.samples[2U].pc = 0x7000ABCDU;
    ring.samples[2U].sequence_commit = 0U;

    CHECK(watchdog_trace_scan(&ring, &scan) == WATCHDOG_TRACE_OK);
    CHECK(scan.valid_count == 2U);
    CHECK(scan.rejected_count == 0U);
    CHECK(watchdog_trace_get_sequence(&ring, 3U, &sample) ==
          WATCHDOG_TRACE_ERROR_NOT_FOUND);
}

static void test_scan_bounds_sequence_window(void)
{
    watchdog_trace_scan_result_t scan;
    watchdog_trace_sample_t sample;
    uint32_t sequence;

    CHECK(watchdog_trace_initialize(&ring, 24U, 0x51515151U) ==
          WATCHDOG_TRACE_OK);
    for (sequence = 1U; sequence <= WATCHDOG_TRACE_CAPACITY + 1U;
         ++sequence)
    {
        append_sample(sequence);
    }

    /* Inject a committed stale sequence into one slot. It is outside the
     * capacity-sized window and must not be exposed by scan/get. */
    ring.samples[7U].sequence_commit = 1U;
    CHECK(watchdog_trace_scan(&ring, &scan) == WATCHDOG_TRACE_OK);
    CHECK(scan.newest_sequence == WATCHDOG_TRACE_CAPACITY + 1U);
    CHECK(scan.valid_count == WATCHDOG_TRACE_CAPACITY - 1U);
    CHECK(scan.rejected_count == 1U);
    CHECK(watchdog_trace_get_sequence(&ring, 1U, &sample) ==
          WATCHDOG_TRACE_ERROR_NOT_FOUND);
}

static void test_header_cursor_is_excluded_from_crc(void)
{
    CHECK(watchdog_trace_initialize(&ring, 23U, 0x42424242U) ==
          WATCHDOG_TRACE_OK);
    CHECK(watchdog_trace_header_valid(&ring.header));
    ring.header.next_index = 7U;
    CHECK(watchdog_trace_header_valid(&ring.header));
    append_sample(77U);
    CHECK(ring.samples[7U].sequence_commit == 77U);
    CHECK(ring.header.next_index == 8U);
    CHECK(watchdog_trace_header_valid(&ring.header));
    ring.header.next_index = WATCHDOG_TRACE_CAPACITY;
    CHECK(!watchdog_trace_header_valid(&ring.header));
}

static void test_corrupt_header_and_alignment_errors(void)
{
    uint8_t unaligned_storage[sizeof(watchdog_trace_ring_t) + 4U];
    volatile watchdog_trace_ring_t *unaligned =
        (volatile watchdog_trace_ring_t *)(void *)(unaligned_storage + 1U);
    watchdog_trace_scan_result_t scan;

    CHECK(watchdog_trace_initialize(&ring, 21U, 0x10203040U) ==
          WATCHDOG_TRACE_OK);
    ring.header.fingerprint_crc32 ^= 1U;
    CHECK(!watchdog_trace_header_valid(&ring.header));
    ring.header.commit = 0U;
    CHECK(watchdog_trace_append(&ring, 1U, 0U, 0U, 0U, 0U, 0U) ==
          WATCHDOG_TRACE_ERROR_HEADER);
    CHECK(watchdog_trace_scan(&ring, &scan) ==
          WATCHDOG_TRACE_ERROR_HEADER);

    CHECK(watchdog_trace_initialize(unaligned, 22U, 0U) ==
          WATCHDOG_TRACE_ERROR_ALIGNMENT);
    CHECK(watchdog_trace_scan(unaligned, &scan) ==
          WATCHDOG_TRACE_ERROR_ALIGNMENT);
}

int main(void)
{
    test_crc_vector_and_layout();
    test_initialize_and_append();
    test_capacity_wrap_and_overwrite();
    test_sequence_wraparound();
    test_invalid_slots_and_commit_marker();
    test_scan_bounds_sequence_window();
    test_header_cursor_is_excluded_from_crc();
    test_corrupt_header_and_alignment_errors();
    puts("watchdog_crashlog trace tests passed");
    return EXIT_SUCCESS;
}
