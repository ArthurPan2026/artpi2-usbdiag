/* SPDX-License-Identifier: Apache-2.0 */
#include "watchdog_record.h"
#include "watchdog_incident_id.h"

#include <stdint.h>
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

#define TEST_RAW_SOFTWARE    (1UL << 0)
#define TEST_RAW_PIN         (1UL << 1)
#define TEST_RAW_IWDG        (1UL << 2)
#define TEST_RAW_POWER       (1UL << 3)
#define TEST_RAW_BROWNOUT    (1UL << 4)
#define TEST_RESET_STREAK_LIMIT 3U

#define TEST_FAULT_HARD      4U
#define TEST_FAULT_ASSERT    5U

static watchdog_retained_record_t record;

static uint32_t test_reset_causes(uint32_t raw_flags)
{
    uint32_t causes = 0U;
    if ((raw_flags & TEST_RAW_IWDG) != 0U)
    {
        causes |= WATCHDOG_RESET_CAUSE_WATCHDOG;
    }
    if ((raw_flags & TEST_RAW_SOFTWARE) != 0U)
    {
        causes |= WATCHDOG_RESET_CAUSE_SOFTWARE;
    }
    if ((raw_flags & TEST_RAW_POWER) != 0U)
    {
        causes |= WATCHDOG_RESET_CAUSE_POWER_ON;
    }
    if ((raw_flags & TEST_RAW_BROWNOUT) != 0U ||
        (((raw_flags & TEST_RAW_PIN) != 0U) &&
         ((raw_flags & TEST_RAW_SOFTWARE) == 0U)))
    {
        causes |= WATCHDOG_RESET_CAUSE_OTHER;
    }
    return causes;
}

static void test_record_boot_begin(
    volatile watchdog_retained_record_t *target,
    const char *source_fingerprint,
    uint32_t raw_reset_flags, int watchdog_reset,
    uint32_t rtc_second, int rtc_valid, uint32_t tick,
    uint32_t *reset_count)
{
    watchdog_record_boot_context_t context;
    watchdog_record_policy_t policy;
    watchdog_record_header_t previous_header;

    memset(&context, 0, sizeof(context));
    context.raw_reset_flags = raw_reset_flags;
    context.reset_causes = test_reset_causes(raw_reset_flags);
    if (watchdog_reset != 0)
    {
        context.reset_causes |= WATCHDOG_RESET_CAUSE_WATCHDOG;
    }
    if (watchdog_record_active_header(target, &previous_header, NULL))
    {
        context.previous_reset_causes =
            test_reset_causes(previous_header.reset_flags);
    }
    context.rtc_second = rtc_second;
    context.rtc_valid = rtc_valid != 0 ? 1U : 0U;
    context.tick = tick;
    policy.reset_window_seconds = 300U;
    watchdog_record_boot_begin(target, source_fingerprint, &context, &policy,
                               reset_count);
}

static void capture_assert_fault(void)
{
    watchdog_fault_context_t context;

    memset(&context, 0, sizeof(context));
    context.stack_pointer[0] = 0x24001000U;
    context.stack_pointer[1] = 0x24002000U;
    context.source_id = 0x24000100U;
    context.tick = 10U;
    context.cycle_counter = 1234U;
    context.source_location[0] = 0x70001000U;
    context.source_location[1] = 0x70002000U;
    context.source_location[2] = 0x70003000U;
    context.source_location[3] = 0x70004000U;
    context.source_location[4] = 77U;
    watchdog_record_fault(&record, TEST_FAULT_ASSERT, &context);
}

static void test_layout_and_headers(void)
{
    watchdog_record_header_t header;
    unsigned slot = 0U;

    watchdog_record_initialize(&record, "0123456789abcdef0123456789abcdef", 0U);
    CHECK(sizeof(record) == WATCHDOG_RECORD_SIZE);
    CHECK(watchdog_record_active_header(&record, &header, &slot));
    CHECK(header.boot_sequence == 1U);
    CHECK(header.commit == WATCHDOG_HEADER_COMMIT);
    CHECK(header.reset_flags == 0U);

    header.health_flags = 0xA55AU;
    watchdog_record_header_commit(&record, &header, slot);
    CHECK(watchdog_record_active_header(&record, &header, &slot));
    CHECK(header.health_flags == 0xA55AU);
    CHECK(watchdog_header_valid(&record.headers[slot]));

    /* An interrupted header update must leave the other copy usable. */
    record.headers[slot].commit = 0U;
    CHECK(watchdog_record_active_header(&record, &header, &slot));
    CHECK(header.health_flags == 0U);
}

static void test_rejects_legacy_v1_records(void)
{
    watchdog_record_header_t header;

    watchdog_record_initialize(&record, "v2-only", 0U);
    CHECK(watchdog_record_active_header(&record, &header, NULL));
    header.version = 1U;
    header.crc32 = watchdog_crc32(
        &header, offsetof(watchdog_record_header_t, crc32));
    CHECK(!watchdog_header_valid(&header));

    capture_assert_fault();
    CHECK(watchdog_fault_valid(&record.fault));
    record.fault.version = 1U;
    record.fault.crc32 = watchdog_crc32(
        &record.fault, offsetof(watchdog_fault_record_t, crc32));
    CHECK(!watchdog_fault_valid(&record.fault));

    watchdog_record_initialize(&record, "v2-only", 0U);
    watchdog_record_incident_id_commit(&record, 1U);
    watchdog_record_mark_flash_image(&record, 1U);
    record.footer.flash_commit = WATCHDOG_FLASH_COMMIT;
    CHECK(watchdog_flash_record_valid(&record));
    record.footer.version = 1U;
    record.footer.flash_crc32 = watchdog_flash_record_crc32(&record);
    CHECK(!watchdog_flash_record_valid(&record));
    CHECK(!watchdog_record_incident_id_valid(&record, NULL));
}

static void test_event_ring(void)
{
    watchdog_record_header_t header;
    unsigned slot;
    uint32_t i;

    watchdog_record_initialize(&record, "fingerprint", 0U);
    for (i = 0U; i < WATCHDOG_EVENT_CAPACITY + 4U; ++i)
    {
        watchdog_record_append_event(&record, i * 1000U,
                                    WATCHDOG_EVENT_CORE_PROGRESS,
                                    i, i + 1U, 0x70001000U + i, 0x24001000U);
    }
    CHECK(watchdog_record_active_header(&record, &header, &slot));
    CHECK(header.event_count == WATCHDOG_EVENT_CAPACITY);
    CHECK(header.event_next_sequence == WATCHDOG_EVENT_CAPACITY + 4U);
    CHECK(watchdog_event_valid(&record.events[0]));
    CHECK(record.events[0].sequence == WATCHDOG_EVENT_CAPACITY + 1U);
    record.events[0].crc32 ^= 1U;
    CHECK(!watchdog_event_valid(&record.events[0]));
}

static void test_fault_and_flash_commit(void)
{
    uint32_t frame[8] = {
        1U, 2U, 3U, 4U, 12U, 0x24000100U, 0x70001235U, 0x01000000U
    };
    watchdog_fault_context_t context;
    watchdog_retained_record_t flash_image;
    uint32_t flash_crc;

    watchdog_record_initialize(&record, "abcdef0123456789", 0U);
    memset(&context, 0, sizeof(context));
    context.return_token = 0xFFFFFFFDU;
    context.status[0] = 0x100U;
    context.status[1] = 0x40000000U;
    context.status[2] = 0x24001000U;
    context.frame_address = 0x24002000U;
    context.basic_frame_address = 0x24002000U;
    context.stack_pointer[0] = 0x24002000U;
    context.stack_pointer[1] = 0x24001000U;
    context.source_id = 0x24000100U;
    context.tick = 200U;
    context.cycle_counter = 123456U;
    memcpy(context.frame, frame, sizeof(context.frame));
    watchdog_record_fault(&record, TEST_FAULT_HARD, &context);
    CHECK(watchdog_fault_valid(&record.fault));
    CHECK(record.fault.platform_context[0] == 0xFFFFFFFDU);
    CHECK(record.fault.platform_context[1] == 0x100U);
    CHECK(record.fault.platform_context[5] == 0x24002000U);
    CHECK(record.fault.platform_context[9] == 0x24000100U);
    CHECK(record.fault.frame[6] == 0x70001235U);

    memcpy(&flash_image, &record, sizeof(flash_image));
    watchdog_record_mark_flash_image(&flash_image, 7U);
    flash_crc = flash_image.footer.flash_crc32;
    CHECK(flash_crc != 0U);
    flash_image.footer.flash_commit = WATCHDOG_FLASH_COMMIT;
    CHECK(watchdog_flash_record_valid(&flash_image));
    flash_image.events[0].arg0 ^= 1U;
    CHECK(!watchdog_flash_record_valid(&flash_image));
    flash_image.events[0].arg0 ^= 1U;
    flash_image.footer.flash_commit = 0xFFFFFFFFUL;
    CHECK(!watchdog_flash_record_valid(&flash_image));
}

static void test_incident_id_transaction_and_por_monotonicity(void)
{
    watchdog_retained_record_t flash_image;
    watchdog_record_header_t header;
    uint64_t incident_id = 0U;
    uint64_t next_id = 0U;
    const uint64_t first_id = UINT64_C(0x0000000200000009);

    watchdog_record_initialize(&record, "fingerprint", 0U);
    CHECK(watchdog_record_active_header(&record, &header, NULL));
    CHECK(header.boot_sequence == 1U);
    CHECK(!watchdog_record_incident_id_valid(&record, &incident_id));

    watchdog_record_incident_id_commit(&record, first_id);
    CHECK(watchdog_record_incident_id_valid(&record, &incident_id));
    CHECK(incident_id == first_id);

    /* A torn ID payload is rejected until the commit word is written last. */
    record.footer.reserved[11] = 0U;
    CHECK(!watchdog_record_incident_id_valid(&record, &incident_id));
    watchdog_record_incident_id_commit(&record, first_id);
    record.footer.reserved[14] ^= 1U;
    CHECK(!watchdog_record_incident_id_valid(&record, &incident_id));
    watchdog_record_incident_id_commit(&record, first_id);

    watchdog_record_mark_flash_image(&record, 1U);
    record.footer.flash_commit = WATCHDOG_FLASH_COMMIT;
    CHECK(watchdog_flash_record_valid(&record));
    CHECK(watchdog_record_incident_id_valid(&record, &incident_id));
    CHECK(incident_id == first_id);
    memcpy(&flash_image, &record, sizeof(flash_image));

    /* Simulate POR: BKPSRAM is reinitialized and boot_sequence returns to one,
     * while the committed Flash incident ID supplies the next high-water ID. */
    watchdog_record_initialize(&record, "fingerprint", 0U);
    CHECK(watchdog_record_active_header(&record, &header, NULL));
    CHECK(header.boot_sequence == 1U);
    CHECK(!watchdog_record_incident_id_valid(&record, &incident_id));
    CHECK(watchdog_record_incident_id_valid(&flash_image, &incident_id));
    CHECK(incident_id == first_id);
    CHECK(watchdog_record_incident_id_next(incident_id, &next_id));
    CHECK(next_id == first_id + 1U);
    watchdog_record_incident_id_commit(&record, next_id);
    CHECK(watchdog_record_incident_id_valid(&record, &incident_id));
    CHECK(incident_id == first_id + 1U);

    CHECK(!watchdog_record_incident_id_next(UINT64_MAX, &next_id));
    CHECK(!watchdog_record_incident_id_next(1U, NULL));
}

static void test_runtime_sidecar_commit_and_fault_preservation(void)
{
    watchdog_component_breadcrumb_t component;
    watchdog_component_breadcrumb_t component_read;
    watchdog_runtime_context_t context;
    watchdog_runtime_context_t context_read;
    watchdog_interrupted_frame_t frame;
    watchdog_interrupted_frame_t frame_read;
    watchdog_fault_context_t fault_context;
    watchdog_retained_record_t flash_image;

    watchdog_record_initialize(&record, "runtime-sidecar", 0U);
    memset(&component, 0, sizeof(component));
    component.sequence = 17U;
    component.tick = 9000U;
    component.thread = 0x24001234U;
    component.stage = WATCHDOG_COMPONENT_STAGE_ACTIVE;
    component.progress = 99U;
    component.pc = 0x70012345U;
    component.sp = 0x24004321U;
    watchdog_record_component_commit(&record, WATCHDOG_COMPONENT_MONGOOSE,
                                     &component);
    CHECK(watchdog_record_component_valid(&record,
                                           WATCHDOG_COMPONENT_MONGOOSE,
                                           &component_read));
    CHECK(component_read.sequence == 17U);
    CHECK(component_read.tick == 9000U);
    CHECK(component_read.stage == WATCHDOG_COMPONENT_STAGE_ACTIVE);
    CHECK(component_read.pc == 0x70012345U);
    CHECK(!watchdog_record_component_valid(&record,
                                            WATCHDOG_COMPONENT_LVGL,
                                            &component_read));

    memset(&context, 0, sizeof(context));
    context.sequence = 4U;
    context.tick = 12000U;
    context.cycles = 0x12345678U;
    context.thread = 0x24001000U;
    context.thread_name[0] = 0x726f6f77U;
    context.thread_name[1] = 0x0000006dU;
    context.pc = 0x70023457U;
    context.lr = 0x70023401U;
    context.psp = 0x24002000U;
    context.msp = 0x20001000U;
    context.exc_return = 0xFFFFFFFDU;
    context.ipsr = 0U;
    context.primask = 0U;
    context.basepri = 0U;
    context.control = 2U;
    context.cfsr = 0x10U;
    context.ltdc_frames = 0x10203040U;
    context.gpu2d_errors = 3U;
    context.dma2d_errors = 2U;
    context.thread_state = 0x00030203U;
    watchdog_record_context_commit(&record, &context);
    CHECK(watchdog_record_context_valid(&record, &context_read));
    CHECK(context_read.sequence == 4U);
    CHECK(context_read.thread == 0x24001000U);
    CHECK(context_read.pc == 0x70023457U);
    CHECK(context_read.exc_return == 0xFFFFFFFDU);
    CHECK(context_read.ltdc_frames == 0x10203040U);

    memset(&frame, 0, sizeof(frame));
    frame.sequence = 1U;
    frame.r0 = 1U;
    frame.r1 = 2U;
    frame.r2 = 3U;
    frame.r3 = 4U;
    frame.r12 = 12U;
    frame.xpsr = 0x01000000U;
    watchdog_record_frame_commit(&record, &frame);
    CHECK(watchdog_record_frame_latest(&record, &frame_read));
    CHECK(frame_read.sequence == 1U);
    CHECK(frame_read.r3 == 4U);

    frame.sequence = 2U;
    frame.r0 = 20U;
    watchdog_record_frame_commit(&record, &frame);
    CHECK(watchdog_record_frame_latest(&record, &frame_read));
    CHECK(frame_read.sequence == 2U);
    CHECK(frame_read.r0 == 20U);

    /* A torn context update is rejected while a previous event sample remains
     * independently protected by its own CRC. */
    record.fault.reserved[WATCHDOG_CONTEXT_COMPONENT_WORDS +
                          WATCHDOG_CONTEXT_WORDS - 1U] = 0U;
    CHECK(!watchdog_record_context_valid(&record, &context_read));
    record.fault.reserved[WATCHDOG_CONTEXT_FRAME_OFFSET +
                          WATCHDOG_CONTEXT_FRAME_WORDS +
                          WATCHDOG_CONTEXT_FRAME_WORDS - 1U] = 0U;
    CHECK(watchdog_record_frame_latest(&record, &frame_read));
    CHECK(frame_read.sequence == 2U); /* Latest intact CRC/commit slot survives. */
    watchdog_record_context_commit(&record, &context);

    memset(&fault_context, 0, sizeof(fault_context));
    fault_context.source_id = 0x24000100U;
    fault_context.frame[6] = 0x70001001U;
    watchdog_record_fault(&record, TEST_FAULT_HARD, &fault_context);
    CHECK(watchdog_fault_valid(&record.fault));
    CHECK(watchdog_record_component_valid(&record,
                                           WATCHDOG_COMPONENT_MONGOOSE,
                                           &component_read));
    CHECK(watchdog_record_context_valid(&record, &context_read));
    CHECK(context_read.pc == 0x70023457U);
    CHECK(watchdog_record_frame_latest(&record, &frame_read));
    CHECK(frame_read.sequence == 2U);

    memcpy(&flash_image, &record, sizeof(flash_image));
    watchdog_record_mark_flash_image(&flash_image, 8U);
    flash_image.footer.flash_commit = WATCHDOG_FLASH_COMMIT;
    CHECK(watchdog_flash_record_valid(&flash_image));
    flash_image.fault.reserved[WATCHDOG_CONTEXT_FRAME_OFFSET] ^= 1U;
    CHECK(!watchdog_flash_record_valid(&flash_image));

    watchdog_record_clear_diagnostics(&record);
    CHECK(!watchdog_record_component_valid(&record,
                                            WATCHDOG_COMPONENT_MONGOOSE,
                                            &component_read));
    CHECK(!watchdog_record_context_valid(&record, &context_read));
    CHECK(!watchdog_record_frame_latest(&record, &frame_read));
}

static void test_reset_streak_window(void)
{
    watchdog_record_header_t header;
    unsigned slot;
    uint32_t count = 0U;

    watchdog_record_initialize(&record, "fingerprint", 0U);
    test_record_boot_begin(&record, NULL, TEST_RAW_IWDG, 1, 100U, 1, 0U,
                               &count);
    CHECK(count == 1U);
    test_record_boot_begin(&record, NULL, TEST_RAW_IWDG, 1, 200U, 1, 0U,
                               &count);
    CHECK(count == 2U);
    test_record_boot_begin(&record, NULL, TEST_RAW_IWDG, 1, 300U, 1, 0U,
                               &count);
    CHECK(count == 3U);

    test_record_boot_begin(&record, NULL, TEST_RAW_IWDG, 1, 600U, 1, 0U,
                               &count);
    CHECK(count == 1U);

    CHECK(watchdog_record_active_header(&record, &header, &slot));
    CHECK(header.reset_count == 1U);
    CHECK(header.reset_window_second == 600U);
    watchdog_record_clear_recovery(&record);
    CHECK(watchdog_record_active_header(&record, &header, &slot));
    CHECK(header.reset_count == 0U);
    CHECK(header.reset_window_valid == 0U);

    watchdog_record_initialize(&record, "fingerprint", 0U);
    test_record_boot_begin(&record, NULL, TEST_RAW_IWDG, 1, 86390U, 1, 0U,
                               &count);
    CHECK(count == 1U);
    test_record_boot_begin(&record, NULL, TEST_RAW_IWDG, 1, 60U, 1, 0U,
                               &count);
    CHECK(count == 2U); /* RTC midnight rollover remains a 70-second interval. */

    watchdog_record_initialize(&record, "fingerprint", 0U);
    test_record_boot_begin(&record, NULL, TEST_RAW_IWDG, 1, 100U, 0, 0U,
                               &count);
    CHECK(count == 1U);
    test_record_boot_begin(&record, NULL, TEST_RAW_IWDG, 1, 200U, 0, 0U,
                               &count);
    CHECK(count == 2U);
    test_record_boot_begin(&record, NULL, TEST_RAW_IWDG, 1, 300U, 0, 0U,
                               &count);
    CHECK(count == 3U); /* Consecutive IWDG resets reach recovery without RTC. */
    test_record_boot_begin(&record, NULL, TEST_RAW_SOFTWARE, 0, 210U, 0, 0U,
                               &count);
    CHECK(count == 0U); /* A deliberate reset breaks the IWDG streak. */
}

static void test_clean_reset_is_not_staged_as_a_crash(void)
{
    watchdog_record_header_t before;
    watchdog_record_header_t after;
    uint32_t count = 99U;

    watchdog_record_initialize(&record, "fingerprint", 0U);
    watchdog_record_append_event(&record, 5U, WATCHDOG_EVENT_OPERATION_BEGIN,
                                 3U, 1000U, 0x70001001U, 0x24001000U);
    CHECK(watchdog_record_active_header(&record, &before, NULL));

    /* A clean software reset during an operation is not a crash. It must not
     * append reset/health events, create a pending Flash record, or preserve a
     * watchdog recovery streak. */
    test_record_boot_begin(&record, NULL, TEST_RAW_SOFTWARE, 0, 10U, 1, 0U,
                               &count);
    CHECK(watchdog_record_active_header(&record, &after, NULL));
    CHECK(after.event_next_sequence == before.event_next_sequence);
    CHECK(after.event_count == before.event_count);
    CHECK(after.reset_count == 0U);
    CHECK(after.reset_flags == TEST_RAW_SOFTWARE);
    CHECK(record.footer.stage_commit == 0U);
    CHECK(record.footer.reserved[0] == 0U);
}

static void test_clean_reset_after_committed_crash_does_not_duplicate_it(void)
{
    watchdog_record_header_t header;
    uint32_t count = 0U;
    uint32_t event_sequence;

    watchdog_record_initialize(&record, "fingerprint", 0U);
    test_record_boot_begin(&record, NULL, TEST_RAW_IWDG, 1,
                               100U, 1, 0U, &count);
    CHECK(record.footer.reserved[0] == WATCHDOG_PENDING_FLASH_MAGIC);
    watchdog_record_incident_id_commit(&record, 5U);
    record.footer.reserved[10] = 0xA5A5A5A5UL;
    record.footer.reserved[15] = 0x15151515UL;
    record.footer.reserved[55] = 0x55555555UL;
    watchdog_record_mark_flash_committed(&record, 1U);
    CHECK(record.footer.reserved[10] == 0xA5A5A5A5UL);
    CHECK(record.footer.reserved[15] == 0U);
    CHECK(record.footer.reserved[55] == 0U);
    CHECK(!watchdog_record_incident_id_valid(&record, NULL));

    /* Normal runtime breadcrumbs after the committed crash must not make the
     * old IWDG cause look pending and cause a duplicate Flash entry. */
    watchdog_record_append_event(&record, 50U, WATCHDOG_EVENT_CORE_PROGRESS,
                                 2U, 1U, 0x70001001U, 0x24001000U);
    CHECK(watchdog_record_active_header(&record, &header, NULL));
    event_sequence = header.event_next_sequence;
    CHECK(header.reset_flags == TEST_RAW_IWDG);
    CHECK(record.footer.reserved[0] == 0U);

    test_record_boot_begin(&record, NULL, TEST_RAW_SOFTWARE, 0, 110U, 1, 0U,
                               &count);
    CHECK(watchdog_record_active_header(&record, &header, NULL));
    CHECK(header.event_next_sequence == event_sequence);
    CHECK(header.reset_flags == TEST_RAW_SOFTWARE);
    CHECK(header.reset_count == 0U);
    CHECK(record.footer.stage_commit == 0U);
    CHECK(record.footer.reserved[0] == 0U);
}

static void test_pending_flash_survives_normal_reset(void)
{
    uint32_t count = 0U;
    uint32_t original_stage;

    watchdog_record_initialize(&record, "fingerprint", 0U);
    test_record_boot_begin(&record, NULL, TEST_RAW_IWDG, 1, 100U, 1, 0U,
                               &count);
    CHECK(count == 1U);
    CHECK(record.footer.reserved[0] == WATCHDOG_PENDING_FLASH_MAGIC);
    original_stage = record.footer.stage_sequence;

    /* Simulate a later software reset after a failed external Flash write. */
    test_record_boot_begin(&record, NULL, TEST_RAW_SOFTWARE, 0, 110U, 1, 0U,
                               &count);
    CHECK(record.footer.reserved[0] == WATCHDOG_PENDING_FLASH_MAGIC);
    CHECK(record.footer.stage_sequence == original_stage);
    CHECK(count == 0U); /* The clean restart breaks the reset streak. */
    {
        watchdog_record_header_t header;
        CHECK(watchdog_record_active_header(&record, &header, NULL));
        CHECK(header.reset_flags == TEST_RAW_IWDG);
    }

    watchdog_record_mark_flash_committed(&record, 3U);
    CHECK(record.footer.reserved[0] == 0U);
    CHECK(record.footer.stage_commit == 0U);
}

static void test_captured_fault_software_reset_streak(void)
{
    watchdog_record_header_t header;
    uint32_t count = 0U;

    watchdog_record_initialize(&record, "fingerprint", 0U);
    capture_assert_fault();
    CHECK(watchdog_fault_valid(&record.fault));
    CHECK(watchdog_record_mark_fault_reset_requested(&record));
    CHECK(record.footer.reserved[1] == WATCHDOG_FAULT_RESET_REQUESTED);

    test_record_boot_begin(&record, NULL, TEST_RAW_SOFTWARE,
                               0, 100U, 1, 0U, &count);
    CHECK(count == 1U);
    CHECK(watchdog_record_active_header(&record, &header, NULL));
    CHECK(header.reset_flags ==
           (TEST_RAW_SOFTWARE | WATCHDOG_RESET_FLAG_CAPTURED_FAULT));
    CHECK(header.reset_count == 1U);
    CHECK(record.footer.reserved[1] == 0U);
    CHECK(record.footer.reserved[0] == WATCHDOG_PENDING_FLASH_MAGIC);

    watchdog_record_mark_flash_committed(&record, 1U);
    capture_assert_fault();
    CHECK(watchdog_record_mark_fault_reset_requested(&record));
    test_record_boot_begin(&record, NULL, TEST_RAW_SOFTWARE,
                               0, 200U, 1, 0U, &count);
    CHECK(count == 2U);
    CHECK(watchdog_record_active_header(&record, &header, NULL));
    CHECK((header.reset_flags & WATCHDOG_RESET_FLAG_CAPTURED_FAULT) != 0U);

    watchdog_record_mark_flash_committed(&record, 2U);
    capture_assert_fault();
    CHECK(watchdog_record_mark_fault_reset_requested(&record));
    test_record_boot_begin(&record, NULL, TEST_RAW_SOFTWARE,
                               0, 300U, 1, 0U, &count);
    CHECK(count == TEST_RESET_STREAK_LIMIT);

    watchdog_record_mark_flash_committed(&record, 3U);
    test_record_boot_begin(&record, NULL, TEST_RAW_SOFTWARE,
                               0, 310U, 1, 0U, &count);
    CHECK(count == 0U); /* A clean restart breaks the streak. */
}

static void test_fault_reset_marker_allows_software_plus_pin_flag(void)
{
    watchdog_record_header_t header;
    uint32_t count = 99U;

    watchdog_record_initialize(&record, "fingerprint", 0U);
    capture_assert_fault();
    CHECK(watchdog_record_mark_fault_reset_requested(&record));
    test_record_boot_begin(&record, NULL,
                               TEST_RAW_SOFTWARE | TEST_RAW_PIN,
                               0, 100U, 1, 0U, &count);
    CHECK(count == 1U);
    CHECK(watchdog_record_active_header(&record, &header, NULL));
    CHECK((header.reset_flags & WATCHDOG_RESET_FLAG_CAPTURED_FAULT) != 0U);
    CHECK(record.footer.reserved[1] == 0U);
    CHECK(watchdog_fault_valid(&record.fault));

    watchdog_record_initialize(&record, "fingerprint", 0U);
    capture_assert_fault();
    CHECK(watchdog_record_mark_fault_reset_requested(&record));
    test_record_boot_begin(&record, NULL, TEST_RAW_PIN,
                               0, 100U, 1, 0U, &count);
    CHECK(count == 0U); /* PIN reset without SFTRSTF is not a fault reset. */
    CHECK(watchdog_record_active_header(&record, &header, NULL));
    CHECK((header.reset_flags & WATCHDOG_RESET_FLAG_CAPTURED_FAULT) == 0U);
    CHECK(record.footer.reserved[1] == 0U);
    CHECK(watchdog_fault_valid(&record.fault));

    watchdog_record_initialize(&record, "fingerprint", 0U);
    capture_assert_fault();
    CHECK(watchdog_record_mark_fault_reset_requested(&record));
    test_record_boot_begin(&record, NULL,
                               TEST_RAW_SOFTWARE | TEST_RAW_IWDG,
                               1, 100U, 1, 0U, &count);
    CHECK(count == 1U); /* Count IWDG once, never double-count the assert. */
    CHECK(watchdog_record_active_header(&record, &header, NULL));
    CHECK((header.reset_flags & WATCHDOG_RESET_FLAG_CAPTURED_FAULT) == 0U);
    CHECK((header.reset_flags & TEST_RAW_IWDG) != 0U);
}

static void test_fault_without_request_is_not_a_fault_reset(void)
{
    watchdog_record_header_t header;
    uint32_t count = 99U;

    watchdog_record_initialize(&record, "fingerprint", 0U);
    capture_assert_fault();
    CHECK(watchdog_fault_valid(&record.fault));
    CHECK(record.footer.reserved[1] == 0U);
    test_record_boot_begin(&record, NULL, TEST_RAW_SOFTWARE,
                               0, 100U, 1, 0U, &count);
    CHECK(count == 0U);
    CHECK(watchdog_record_active_header(&record, &header, NULL));
    CHECK((header.reset_flags & WATCHDOG_RESET_FLAG_CAPTURED_FAULT) == 0U);
    CHECK(record.footer.reserved[1] == 0U);
    CHECK(watchdog_fault_valid(&record.fault)); /* Kept for later Flash write. */
}

int main(void)
{
    test_layout_and_headers();
    test_rejects_legacy_v1_records();
    test_event_ring();
    test_fault_and_flash_commit();
    test_incident_id_transaction_and_por_monotonicity();
    test_runtime_sidecar_commit_and_fault_preservation();
    test_reset_streak_window();
    test_clean_reset_is_not_staged_as_a_crash();
    test_clean_reset_after_committed_crash_does_not_duplicate_it();
    test_pending_flash_survives_normal_reset();
    test_captured_fault_software_reset_streak();
    test_fault_reset_marker_allows_software_plus_pin_flag();
    test_fault_without_request_is_not_a_fault_reset();
    puts("watchdog_record_host: PASS");
    return 0;
}
