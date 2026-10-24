/* SPDX-License-Identifier: Apache-2.0 */
#include "watchdog_trace_flash.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "watchdog_h7r7_port.h"
#include "watchdog_record.h"
#include "watchdog_trace.h"
#include "watchdog_flash_policy.h"
#include "w35t51nwtbie.h"
#include "xspi.h"

#define WATCHDOG_TRACE_XSPI_OPI       W35T51NWTBIE_OPI_MODE
#define WATCHDOG_TRACE_XSPI_DTR       W35T51NWTBIE_DTR_TRANSFER
#define WATCHDOG_TRACE_XSPI_ADDR_SIZE W35T51NWTBIE_4BYTES_SIZE
#define WATCHDOG_TRACE_PAGE_SIZE      W35T51NWTBIE_PAGE_SIZE

extern XSPI_HandleTypeDef hxspi2;
extern volatile watchdog_retained_record_t g_watchdog_retained_record;

extern int32_t W35T51NWTBIE_PageProgramDTR(XSPI_HandleTypeDef *Ctx,
                                           uint8_t *pData,
                                           uint32_t WriteAddr,
                                           uint32_t Size);
extern int32_t W35T51NWTBIE_ReadStatusRegister(
    XSPI_HandleTypeDef *Ctx, W35T51NWTBIE_Interface_t Mode,
    W35T51NWTBIE_Transfer_t Rate, uint8_t *Value);

typedef struct watchdog_trace_flash_bank_info
{
    uint32_t valid;
    watchdog_flash_policy_state_t state;
    uint32_t bank_sequence;
    uint32_t app_boot_sequence;
    uint32_t reset_boot_sequence;
    uint32_t fingerprint_crc32;
    uint32_t payload_crc32;
    uint32_t samples_crc32;
} watchdog_trace_flash_bank_info_t;

typedef struct watchdog_trace_prepare_snapshot
{
    uint32_t ready;
    uint32_t eligible;
    uint32_t reset_flags;
    uint32_t incident_reset_flags;
    uint32_t boot_sequence;
    uint32_t fingerprint_crc32;
    uint32_t synchronous_fault_kind;
    uint32_t pending_marker;
    uint32_t app_boot_sequence;
    uint32_t reset_boot_sequence;
    uint32_t payload_crc32;
} watchdog_trace_prepare_snapshot_t;

static watchdog_trace_prepare_snapshot_t s_trace_prepare_snapshot;
static watchdog_flash_policy_counts_t s_trace_bank_scan_counts;

static int watchdog_trace_capture_scan(
    const watchdog_record_header_t *header,
    const volatile watchdog_trace_ring_t *ring,
    watchdog_trace_scan_result_t *scan,
    int allow_older_boot);
static void watchdog_trace_pending_clear(void);
static void watchdog_trace_pending_commit(uint32_t reset_flags);

typedef char watchdog_trace_header_word_count_matches_size[
    ((BOOT_WATCHDOG_TRACE_FLASH_HEADER_WORDS * sizeof(uint32_t)) ==
     BOOT_WATCHDOG_TRACE_FLASH_HEADER_SIZE) ? 1 : -1];
typedef char watchdog_trace_payload_size_must_be_0x55a0[
    (sizeof(watchdog_trace_ring_t) == 0x55A0U) ? 1 : -1];
typedef char watchdog_trace_two_banks_must_fit_reserved_range[
    ((WATCHDOG_TRACE_FLASH_BANK_SIZE * WATCHDOG_TRACE_FLASH_BANK_COUNT) ==
     WATCHDOG_TRACE_FLASH_SIZE) ? 1 : -1];
typedef char watchdog_trace_bank_must_end_at_unchanged_journal[
    ((WATCHDOG_TRACE_FLASH_OFFSET + WATCHDOG_TRACE_FLASH_SIZE) ==
     WATCHDOG_FLASH_OFFSET) ? 1 : -1];
typedef char watchdog_trace_payload_must_fit_bank[
    ((BOOT_WATCHDOG_TRACE_FLASH_HEADER_SIZE +
      sizeof(watchdog_trace_ring_t)) <= WATCHDOG_TRACE_FLASH_BANK_SIZE)
        ? 1 : -1];

static int watchdog_trace_sequence_newer(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) > 0;
}

static uint32_t watchdog_trace_bank_offset(uint32_t bank)
{
    return WATCHDOG_TRACE_FLASH_OFFSET +
           bank * WATCHDOG_TRACE_FLASH_BANK_SIZE;
}

static uint32_t watchdog_trace_bank_mapped_address(uint32_t bank)
{
    return WATCHDOG_TRACE_FLASH_MAPPED_BASE +
           bank * WATCHDOG_TRACE_FLASH_BANK_SIZE;
}

static uint32_t watchdog_trace_read_le32(const volatile uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void watchdog_trace_write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static void watchdog_trace_invalidate_cache(uint32_t address, uint32_t size)
{
    uint32_t aligned_address = address & ~31U;
    uint32_t aligned_size =
        (size + (address - aligned_address) + 31U) & ~31U;

    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        SCB_InvalidateDCache_by_Addr((uint32_t *)(uintptr_t)aligned_address,
                                     (int32_t)aligned_size);
    }
    __DSB();
    __ISB();
}

static int watchdog_trace_flash_map_begin(void)
{
    if (W35T51NWTBIE_EnableMemoryMappedModeDTR(&hxspi2,
                                               WATCHDOG_TRACE_XSPI_OPI) !=
        W35T51NWTBIE_OK)
    {
        return -1;
    }
    watchdog_trace_invalidate_cache(WATCHDOG_TRACE_FLASH_MAPPED_BASE,
                                    WATCHDOG_TRACE_FLASH_SIZE);
    return 0;
}

static int watchdog_trace_flash_map_end(void)
{
    __DSB();
    __ISB();
    return W35T51NWTBIE_DisableMemoryMappedMode(&hxspi2) ==
                   W35T51NWTBIE_OK
               ? 0
               : -1;
}

static void watchdog_trace_invalidate_retained_ring(void)
{
    watchdog_trace_invalidate_cache(BOOT_WATCHDOG_TRACE_RAM_ADDRESS,
                                    (uint32_t)sizeof(watchdog_trace_ring_t));
}

static uint32_t watchdog_trace_header_crc(
    const volatile uint8_t *header_bytes)
{
    uint8_t crc_bytes[18U * sizeof(uint32_t)];
    uint32_t index;

    /* The decoder's canonical header CRC covers words 0..15 followed by
     * words 18..19. It skips the CRC at word 16 and commit at word 17. */
    for (index = 0U; index < 16U * sizeof(uint32_t); ++index)
    {
        crc_bytes[index] = header_bytes[index];
    }
    for (index = 0U; index < 2U * sizeof(uint32_t); ++index)
    {
        crc_bytes[16U * sizeof(uint32_t) + index] =
            header_bytes[18U * sizeof(uint32_t) + index];
    }
    return watchdog_trace_crc32(crc_bytes, sizeof(crc_bytes));
}

static int watchdog_trace_fault_kind_is_synchronous(uint32_t kind)
{
    switch (kind)
    {
    case WATCHDOG_FAULT_MEMMANAGE:
    case WATCHDOG_FAULT_BUS:
    case WATCHDOG_FAULT_USAGE:
    case WATCHDOG_FAULT_HARD:
    case WATCHDOG_FAULT_ASSERT:
    case WATCHDOG_FAULT_ERROR_HANDLER:
    case WATCHDOG_FAULT_STACK_OVERFLOW:
        return 1;
    default:
        /* NMI, Boot-stage errors, and unexpected IRQs do not qualify as a
         * synchronous App fault for this sidecar write policy. */
        return 0;
    }
}

static int watchdog_trace_reset_is_eligible(
    uint32_t reset_flags, const watchdog_record_header_t *header)
{
    const volatile watchdog_record_footer_t *footer =
        &g_watchdog_retained_record.footer;
    uint32_t fault_kind = g_watchdog_retained_record.fault.kind;

    if ((reset_flags & RCC_RSR_IWDGRSTF) != 0U)
    {
        return 1;
    }

    /* Require the current software reset to match the committed synchronous
     * fault. The adjacent boot sequence prevents an old marker from making a
     * later deliberate reset write a trace. */
    if ((reset_flags & RCC_RSR_SFTRSTF) == 0U ||
        (header->reset_flags & WATCHDOG_RESET_FLAG_CAPTURED_FAULT) == 0U ||
        !watchdog_fault_valid(&g_watchdog_retained_record.fault) ||
        !watchdog_trace_fault_kind_is_synchronous(fault_kind) ||
        footer->magic != WATCHDOG_RECORD_MAGIC ||
        footer->version != WATCHDOG_RECORD_VERSION ||
        footer->stage_commit != WATCHDOG_STAGE_COMMIT ||
        footer->reserved[1] != WATCHDOG_FAULT_RESET_REQUESTED ||
        watchdog_trace_next_sequence(footer->stage_sequence) !=
            header->boot_sequence)
    {
        return 0;
    }
    return 1;
}

int boot_watchdog_trace_flash_prepare(uint32_t reset_flags)
{
    watchdog_record_header_t record_header;
    watchdog_trace_scan_result_t scan;
    const volatile watchdog_trace_ring_t *ring =
        (const volatile watchdog_trace_ring_t *)(uintptr_t)
            BOOT_WATCHDOG_TRACE_RAM_ADDRESS;
    uint32_t pending_flags;
    int pending;

    memset(&s_trace_prepare_snapshot, 0, sizeof(s_trace_prepare_snapshot));
    if (!watchdog_record_active_header(&g_watchdog_retained_record,
                                       &record_header, NULL))
    {
        return -1;
    }

    s_trace_prepare_snapshot.reset_flags = reset_flags;
    s_trace_prepare_snapshot.boot_sequence = record_header.boot_sequence;
    s_trace_prepare_snapshot.fingerprint_crc32 =
        record_header.source_fingerprint_crc32;

    pending = g_watchdog_retained_record.footer.reserved[
                  WATCHDOG_TRACE_PENDING_WORD] ==
              WATCHDOG_TRACE_PENDING_MAGIC;
    pending_flags = g_watchdog_retained_record.footer.reserved[
        WATCHDOG_TRACE_PENDING_FLAGS_WORD];
    if (pending != 0)
    {
        /* A previously armed capture may survive multiple Boot entries. Its
         * App boot sequence remains older than this Boot sequence; preserve
         * the first reset flags and keep retrying until a bank commits. */
        if (pending_flags == 0U ||
            !watchdog_trace_capture_scan(&record_header, ring, &scan, 1))
        {
            /* Do not leave the App permanently prevented from reinitializing
             * a damaged or incompatible retained trace ring. */
            watchdog_trace_pending_clear();
            __DSB();
            s_trace_prepare_snapshot.ready = 1U;
            return 0;
        }
        s_trace_prepare_snapshot.eligible = 1U;
        s_trace_prepare_snapshot.incident_reset_flags = pending_flags;
        s_trace_prepare_snapshot.pending_marker = 1U;
        s_trace_prepare_snapshot.app_boot_sequence = scan.boot_sequence;
        s_trace_prepare_snapshot.reset_boot_sequence =
            watchdog_trace_next_sequence(scan.boot_sequence);
        s_trace_prepare_snapshot.payload_crc32 = watchdog_trace_crc32(
            ring, sizeof(watchdog_trace_ring_t));
        __DSB();
        s_trace_prepare_snapshot.ready = 1U;
        __DSB();
        return 0;
    }

    if (!watchdog_trace_reset_is_eligible(reset_flags, &record_header) ||
        !watchdog_trace_capture_scan(&record_header, ring, &scan, 0))
    {
        __DSB();
        s_trace_prepare_snapshot.ready = 1U;
        __DSB();
        return 0;
    }

    /* The marker is a commit word: persist original reset flags first and
     * publish pending only after an aligned barrier. */
    watchdog_trace_pending_commit(reset_flags);
    s_trace_prepare_snapshot.eligible = 1U;
    s_trace_prepare_snapshot.incident_reset_flags = reset_flags;
    s_trace_prepare_snapshot.synchronous_fault_kind =
        (reset_flags & RCC_RSR_IWDGRSTF) == 0U
            ? g_watchdog_retained_record.fault.kind : 0U;
    s_trace_prepare_snapshot.pending_marker = 1U;
    s_trace_prepare_snapshot.app_boot_sequence = scan.boot_sequence;
    s_trace_prepare_snapshot.reset_boot_sequence =
        watchdog_trace_next_sequence(scan.boot_sequence);
    s_trace_prepare_snapshot.payload_crc32 = watchdog_trace_crc32(
        ring, sizeof(watchdog_trace_ring_t));
    __DSB();
    s_trace_prepare_snapshot.ready = 1U;
    __DSB();
    return 0;
}

static int watchdog_trace_capture_scan(
    const watchdog_record_header_t *header,
    const volatile watchdog_trace_ring_t *ring,
    watchdog_trace_scan_result_t *scan,
    int allow_older_boot)
{
    uint32_t trace_reset_boot_sequence;

    watchdog_trace_invalidate_retained_ring();
    if (watchdog_trace_scan(ring, scan) != WATCHDOG_TRACE_OK ||
        scan->valid_count == 0U)
    {
        return 0;
    }
    trace_reset_boot_sequence =
        watchdog_trace_next_sequence(scan->boot_sequence);
    if (scan->fingerprint_crc32 != header->source_fingerprint_crc32)
    {
        return 0;
    }
    if (allow_older_boot != 0)
    {
        return !watchdog_trace_sequence_newer(trace_reset_boot_sequence,
                                              header->boot_sequence);
    }
    return trace_reset_boot_sequence == header->boot_sequence;
}

static void watchdog_trace_pending_clear(void)
{
    g_watchdog_retained_record.footer.reserved[
        WATCHDOG_TRACE_PENDING_WORD] = 0U;
    __DSB();
    g_watchdog_retained_record.footer.reserved[
        WATCHDOG_TRACE_PENDING_FLAGS_WORD] = 0U;
    __DSB();
}

static void watchdog_trace_pending_commit(uint32_t reset_flags)
{
    g_watchdog_retained_record.footer.reserved[
        WATCHDOG_TRACE_PENDING_FLAGS_WORD] = reset_flags;
    __DSB();
    g_watchdog_retained_record.footer.reserved[
        WATCHDOG_TRACE_PENDING_WORD] = WATCHDOG_TRACE_PENDING_MAGIC;
    __DSB();
}

int boot_watchdog_trace_flash_preserve_fault(uint32_t reset_flags)
{
    const volatile watchdog_record_footer_t *footer =
        &g_watchdog_retained_record.footer;
    const volatile watchdog_trace_ring_t *ring =
        (const volatile watchdog_trace_ring_t *)(uintptr_t)
            BOOT_WATCHDOG_TRACE_RAM_ADDRESS;
    watchdog_record_header_t record_header;
    watchdog_trace_scan_result_t scan;

    /* watchdog_record_boot_begin() consumes reserved[1] after it classifies
     * a captured synchronous fault. Preserve that request in the trace-owned
     * footer words first; the later external-memory phase performs the Flash
     * I/O and commits the ring. */
    if (watchdog_h7r7_reset_causes(reset_flags) !=
            WATCHDOG_RESET_CAUSE_SOFTWARE ||
        footer->reserved[WATCHDOG_TRACE_PENDING_WORD] ==
            WATCHDOG_TRACE_PENDING_MAGIC)
    {
        return 0;
    }
    if (!watchdog_record_active_header(&g_watchdog_retained_record,
                                       &record_header, NULL))
    {
        return -1;
    }
    if (!watchdog_fault_valid(&g_watchdog_retained_record.fault) ||
        !watchdog_trace_fault_kind_is_synchronous(
            g_watchdog_retained_record.fault.kind) ||
        footer->magic != WATCHDOG_RECORD_MAGIC ||
        footer->version != WATCHDOG_RECORD_VERSION ||
        footer->stage_commit != WATCHDOG_STAGE_COMMIT ||
        footer->stage_sequence != record_header.boot_sequence ||
        footer->reserved[1] != WATCHDOG_FAULT_RESET_REQUESTED)
    {
        return 0;
    }

    watchdog_trace_invalidate_retained_ring();
    if (watchdog_trace_scan(ring, &scan) != WATCHDOG_TRACE_OK ||
        scan.valid_count == 0U ||
        scan.boot_sequence != record_header.boot_sequence ||
        scan.fingerprint_crc32 != record_header.source_fingerprint_crc32)
    {
        return 0;
    }

    watchdog_trace_pending_commit(reset_flags);
    return 1;
}

static int watchdog_trace_header_validate(
    uint32_t bank, const volatile uint8_t *header_bytes,
    uint32_t expected_commit, watchdog_trace_flash_bank_info_t *info)
{
    const volatile watchdog_trace_ring_t *ring;
    watchdog_trace_scan_result_t scan;
    uint32_t bank_sequence;
    uint32_t app_boot_sequence;
    uint32_t reset_boot_sequence;
    uint32_t fingerprint_crc32;
    uint32_t payload_crc32;
    uint32_t index;

    if (bank >= WATCHDOG_TRACE_FLASH_BANK_COUNT || header_bytes == NULL ||
        info == NULL ||
        watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_MAGIC * 4U]) !=
            BOOT_WATCHDOG_TRACE_FLASH_MAGIC ||
        watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_VERSION * 4U]) !=
            BOOT_WATCHDOG_TRACE_FLASH_VERSION ||
        watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_HEADER_SIZE * 4U]) !=
            BOOT_WATCHDOG_TRACE_FLASH_HEADER_SIZE ||
        watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_PAYLOAD_SIZE * 4U]) !=
            (uint32_t)sizeof(watchdog_trace_ring_t) ||
        watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_BANK_SEQUENCE * 4U]) == 0U ||
        watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_HEADER_COMMIT * 4U]) !=
            expected_commit ||
        watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_BANK_INDEX * 4U]) != bank ||
        watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_RESERVED0 * 4U]) != 0U ||
        watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_HEADER_CRC32 * 4U]) !=
            watchdog_trace_header_crc(header_bytes))
    {
        return 0;
    }
    for (index = 20U; index < BOOT_WATCHDOG_TRACE_FLASH_HEADER_WORDS; ++index)
    {
        if (watchdog_trace_read_le32(&header_bytes[index * 4U]) != 0U)
        {
            return 0;
        }
    }

    bank_sequence = watchdog_trace_read_le32(
        &header_bytes[BOOT_WATCHDOG_TRACE_WORD_BANK_SEQUENCE * 4U]);
    app_boot_sequence = watchdog_trace_read_le32(
        &header_bytes[BOOT_WATCHDOG_TRACE_WORD_APP_BOOT_SEQUENCE * 4U]);
    reset_boot_sequence = watchdog_trace_read_le32(
        &header_bytes[BOOT_WATCHDOG_TRACE_WORD_RESET_BOOT_SEQUENCE * 4U]);
    fingerprint_crc32 = watchdog_trace_read_le32(
        &header_bytes[BOOT_WATCHDOG_TRACE_WORD_FINGERPRINT_CRC32 * 4U]);
    payload_crc32 = watchdog_trace_read_le32(
        &header_bytes[BOOT_WATCHDOG_TRACE_WORD_PAYLOAD_CRC32 * 4U]);

    if (bank_sequence != reset_boot_sequence ||
        watchdog_trace_next_sequence(app_boot_sequence) != reset_boot_sequence)
    {
        return 0;
    }

    ring = (const volatile watchdog_trace_ring_t *)(uintptr_t)
        ((uintptr_t)header_bytes + BOOT_WATCHDOG_TRACE_FLASH_HEADER_SIZE);
    if (watchdog_trace_crc32(ring, sizeof(*ring)) != payload_crc32 ||
        watchdog_trace_scan(ring, &scan) != WATCHDOG_TRACE_OK ||
        scan.valid_count == 0U ||
        scan.boot_sequence != app_boot_sequence ||
        scan.fingerprint_crc32 != fingerprint_crc32 ||
        scan.valid_count != watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_VALID_COUNT * 4U]) ||
        scan.rejected_count != watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_REJECTED_COUNT * 4U]) ||
        scan.oldest_sequence != watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_OLDEST_SEQUENCE * 4U]) ||
        scan.newest_sequence != watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_NEWEST_SEQUENCE * 4U]) ||
        scan.samples_crc32 != watchdog_trace_read_le32(
            &header_bytes[BOOT_WATCHDOG_TRACE_WORD_SAMPLES_CRC32 * 4U]))
    {
        return 0;
    }

    info->valid = 1U;
    info->bank_sequence = bank_sequence;
    info->app_boot_sequence = app_boot_sequence;
    info->reset_boot_sequence = reset_boot_sequence;
    info->fingerprint_crc32 = fingerprint_crc32;
    info->payload_crc32 = payload_crc32;
    info->samples_crc32 = scan.samples_crc32;
    return 1;
}

static int watchdog_trace_flash_scan_banks(
    watchdog_trace_flash_bank_info_t banks[WATCHDOG_TRACE_FLASH_BANK_COUNT])
{
    uint32_t bank;
    watchdog_flash_policy_state_t states[WATCHDOG_TRACE_FLASH_BANK_COUNT];
    int result = 0;

    memset(banks, 0, sizeof(watchdog_trace_flash_bank_info_t) *
                         WATCHDOG_TRACE_FLASH_BANK_COUNT);
    memset(states, WATCHDOG_FLASH_POLICY_READ_ERROR, sizeof(states));
    memset(&s_trace_bank_scan_counts, 0, sizeof(s_trace_bank_scan_counts));
    if (watchdog_trace_flash_map_begin() != 0)
    {
        return -1;
    }
    for (bank = 0U; bank < WATCHDOG_TRACE_FLASH_BANK_COUNT; ++bank)
    {
        uint32_t address = watchdog_trace_bank_mapped_address(bank);
        uint32_t offset;
        int valid = watchdog_trace_header_validate(
            bank, (const volatile uint8_t *)(uintptr_t)address,
            BOOT_WATCHDOG_TRACE_FLASH_HEADER_COMMIT, &banks[bank]);
        if (valid != 0)
        {
            states[bank] = WATCHDOG_FLASH_POLICY_VALID;
        }
        else
        {
            const volatile uint8_t *bytes =
                (const volatile uint8_t *)(uintptr_t)address;
            int erased = 1;
            for (offset = 0U; offset < WATCHDOG_TRACE_FLASH_BANK_SIZE;
                 ++offset)
            {
                if (bytes[offset] != 0xFFU)
                {
                    erased = 0;
                    break;
                }
            }
            states[bank] = erased != 0
                ? WATCHDOG_FLASH_POLICY_BLANK
                : WATCHDOG_FLASH_POLICY_DAMAGED;
        }
        banks[bank].state = states[bank];
    }
    if (watchdog_trace_flash_map_end() != 0)
    {
        result = -1;
    }
    if (result == 0 &&
        watchdog_flash_policy_count(states, WATCHDOG_TRACE_FLASH_BANK_COUNT,
                                    &s_trace_bank_scan_counts) != 0)
    {
        result = -1;
    }
    return result;
}

static int watchdog_trace_flash_write_enable(void)
{
    uint8_t status[2] = {0xFFU, 0xFFU};
    int32_t result = W35T51NWTBIE_WriteEnable(&hxspi2,
                                              WATCHDOG_TRACE_XSPI_OPI,
                                              WATCHDOG_TRACE_XSPI_DTR);

    if (result == W35T51NWTBIE_OK)
    {
        return 0;
    }
    /* OPI DTR can report an auto-poll error after WEL was set. Treat it as
     * success only when both status bytes show WEL=1 and WIP=0. */
    if (W35T51NWTBIE_ReadStatusRegister(&hxspi2,
                                        WATCHDOG_TRACE_XSPI_OPI,
                                        WATCHDOG_TRACE_XSPI_DTR,
                                        status) == W35T51NWTBIE_OK &&
        hxspi2.State == HAL_XSPI_STATE_READY &&
        (status[0] & W35T51NWTBIE_SR_WEL) != 0U &&
        (status[0] & W35T51NWTBIE_SR_WIP) == 0U &&
        (status[1] & W35T51NWTBIE_SR_WEL) != 0U &&
        (status[1] & W35T51NWTBIE_SR_WIP) == 0U)
    {
        return 0;
    }
    return -1;
}

static int watchdog_trace_flash_wait_ready(void)
{
    return W35T51NWTBIE_AutoPollingMemReady(&hxspi2,
                                             WATCHDOG_TRACE_XSPI_OPI,
                                             WATCHDOG_TRACE_XSPI_DTR) ==
                   W35T51NWTBIE_OK
               ? 0
               : -1;
}

static int watchdog_trace_flash_program(uint32_t address,
                                        const uint8_t *data,
                                        uint32_t length)
{
    uint32_t flash_end = WATCHDOG_TRACE_FLASH_OFFSET +
                         WATCHDOG_TRACE_FLASH_SIZE;

    if (data == NULL || length == 0U || length > WATCHDOG_TRACE_PAGE_SIZE ||
        (address & 0xFFU) + length > WATCHDOG_TRACE_PAGE_SIZE ||
        address < WATCHDOG_TRACE_FLASH_OFFSET || address > flash_end ||
        length > flash_end - address)
    {
        return -1;
    }
    if (watchdog_trace_flash_write_enable() != 0 ||
        W35T51NWTBIE_PageProgramDTR(&hxspi2, (uint8_t *)(uintptr_t)data,
                                    address, length) != W35T51NWTBIE_OK ||
        watchdog_trace_flash_wait_ready() != 0)
    {
        return -1;
    }
    watchdog_h7r7_iwdg_refresh();
    return 0;
}

static int watchdog_trace_flash_erase_sector(uint32_t address)
{
    uint32_t flash_end = WATCHDOG_TRACE_FLASH_OFFSET +
                         WATCHDOG_TRACE_FLASH_SIZE;

    if (address < WATCHDOG_TRACE_FLASH_OFFSET || address >= flash_end ||
        (address & (WATCHDOG_TRACE_FLASH_SECTOR_SIZE - 1U)) != 0U ||
        WATCHDOG_TRACE_FLASH_SECTOR_SIZE > flash_end - address)
    {
        return -1;
    }
    if (watchdog_trace_flash_write_enable() != 0 ||
        W35T51NWTBIE_BlockErase(&hxspi2, WATCHDOG_TRACE_XSPI_OPI,
                                WATCHDOG_TRACE_XSPI_DTR,
                                WATCHDOG_TRACE_XSPI_ADDR_SIZE, address,
                                W35T51NWTBIE_ERASE_4K) != W35T51NWTBIE_OK ||
        watchdog_trace_flash_wait_ready() != 0)
    {
        return -1;
    }
    watchdog_h7r7_iwdg_refresh();
    return 0;
}

static int watchdog_trace_flash_erase_bank(uint32_t bank)
{
    uint32_t sector;
    uint32_t first_address;

    if (bank >= WATCHDOG_TRACE_FLASH_BANK_COUNT ||
        WATCHDOG_TRACE_FLASH_BANK_SIZE % WATCHDOG_TRACE_FLASH_SECTOR_SIZE != 0U)
    {
        return -1;
    }
    first_address = watchdog_trace_bank_offset(bank);
    for (sector = 0U;
         sector < WATCHDOG_TRACE_FLASH_BANK_SIZE /
                      WATCHDOG_TRACE_FLASH_SECTOR_SIZE;
         ++sector)
    {
        if (watchdog_trace_flash_erase_sector(
                first_address + sector * WATCHDOG_TRACE_FLASH_SECTOR_SIZE) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int watchdog_trace_flash_program_bytes(uint32_t address,
                                               const uint8_t *source,
                                               uint32_t length)
{
    uint8_t page[WATCHDOG_TRACE_PAGE_SIZE] __attribute__((aligned(32)));
    uint32_t offset;

    for (offset = 0U; offset < length; offset += WATCHDOG_TRACE_PAGE_SIZE)
    {
        uint32_t remaining = length - offset;
        uint32_t page_length = remaining < WATCHDOG_TRACE_PAGE_SIZE
                                   ? remaining : WATCHDOG_TRACE_PAGE_SIZE;
        uint32_t index;
        for (index = 0U; index < page_length; ++index)
        {
            page[index] = source[offset + index];
        }
        if (watchdog_trace_flash_program(address + offset, page,
                                         page_length) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int watchdog_trace_flash_program_payload(
    uint32_t address, const volatile uint8_t *source, uint32_t length)
{
    uint8_t page[WATCHDOG_TRACE_PAGE_SIZE] __attribute__((aligned(32)));
    uint32_t offset;

    if (source == NULL || length != sizeof(watchdog_trace_ring_t))
    {
        return -1;
    }
    for (offset = 0U; offset < length; offset += WATCHDOG_TRACE_PAGE_SIZE)
    {
        uint32_t remaining = length - offset;
        uint32_t page_length = remaining < WATCHDOG_TRACE_PAGE_SIZE
                                   ? remaining : WATCHDOG_TRACE_PAGE_SIZE;
        uint32_t index;
        for (index = 0U; index < page_length; ++index)
        {
            page[index] = source[offset + index];
        }
        if (watchdog_trace_flash_program(address + offset, page,
                                         page_length) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static void watchdog_trace_header_build(
    uint8_t header[BOOT_WATCHDOG_TRACE_FLASH_HEADER_SIZE],
    uint32_t bank, uint32_t app_boot_sequence,
    uint32_t reset_boot_sequence, uint32_t reset_flags,
    uint32_t journal_flash_sequence, uint32_t fingerprint_crc32,
    const watchdog_trace_scan_result_t *scan, uint32_t payload_crc32)
{
    memset(header, 0, BOOT_WATCHDOG_TRACE_FLASH_HEADER_SIZE);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_MAGIC * 4U],
        BOOT_WATCHDOG_TRACE_FLASH_MAGIC);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_VERSION * 4U],
        BOOT_WATCHDOG_TRACE_FLASH_VERSION);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_HEADER_SIZE * 4U],
        BOOT_WATCHDOG_TRACE_FLASH_HEADER_SIZE);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_PAYLOAD_SIZE * 4U],
        (uint32_t)sizeof(watchdog_trace_ring_t));
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_BANK_SEQUENCE * 4U],
        reset_boot_sequence);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_APP_BOOT_SEQUENCE * 4U],
        app_boot_sequence);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_RESET_BOOT_SEQUENCE * 4U],
        reset_boot_sequence);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_RESET_FLAGS * 4U], reset_flags);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_JOURNAL_FLASH_SEQUENCE * 4U],
        journal_flash_sequence);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_FINGERPRINT_CRC32 * 4U],
        fingerprint_crc32);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_VALID_COUNT * 4U],
        scan->valid_count);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_REJECTED_COUNT * 4U],
        scan->rejected_count);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_OLDEST_SEQUENCE * 4U],
        scan->oldest_sequence);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_NEWEST_SEQUENCE * 4U],
        scan->newest_sequence);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_SAMPLES_CRC32 * 4U],
        scan->samples_crc32);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_PAYLOAD_CRC32 * 4U],
        payload_crc32);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_BANK_INDEX * 4U], bank);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_RESERVED0 * 4U], 0U);
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_HEADER_CRC32 * 4U],
        watchdog_trace_header_crc(header));
    watchdog_trace_write_le32(
        &header[BOOT_WATCHDOG_TRACE_WORD_HEADER_COMMIT * 4U], UINT32_MAX);
}

static int watchdog_trace_flash_verify_bank(
    uint32_t bank, uint32_t expected_commit,
    watchdog_trace_flash_bank_info_t *info)
{
    uint32_t mapped_address;
    int valid;

    if (bank >= WATCHDOG_TRACE_FLASH_BANK_COUNT || info == NULL ||
        watchdog_trace_flash_map_begin() != 0)
    {
        return -1;
    }
    mapped_address = watchdog_trace_bank_mapped_address(bank);
    valid = watchdog_trace_header_validate(
        bank, (const volatile uint8_t *)(uintptr_t)mapped_address,
        expected_commit, info);
    if (watchdog_trace_flash_map_end() != 0)
    {
        return -1;
    }
    return valid != 0 ? 0 : -1;
}

static int watchdog_trace_flash_commit_capture(
    uint32_t target_bank, uint32_t reset_boot_sequence,
    uint32_t incident_reset_flags,
    uint32_t journal_flash_sequence,
    const volatile watchdog_trace_ring_t *ring,
    const watchdog_trace_scan_result_t *scan,
    uint32_t payload_crc32)
{
    uint8_t header[BOOT_WATCHDOG_TRACE_FLASH_HEADER_SIZE]
        __attribute__((aligned(32)));
    uint32_t bank_offset;
    uint32_t payload_offset;
    uint32_t commit = BOOT_WATCHDOG_TRACE_FLASH_HEADER_COMMIT;
    watchdog_trace_flash_bank_info_t info;

    if (target_bank >= WATCHDOG_TRACE_FLASH_BANK_COUNT ||
        reset_boot_sequence == 0U || ring == NULL || scan == NULL)
    {
        return -1;
    }
    watchdog_trace_header_build(
        header, target_bank, scan->boot_sequence,
        reset_boot_sequence, incident_reset_flags,
        journal_flash_sequence, scan->fingerprint_crc32, scan,
        payload_crc32);

    bank_offset = watchdog_trace_bank_offset(target_bank);
    if (bank_offset < WATCHDOG_TRACE_FLASH_OFFSET ||
        bank_offset > WATCHDOG_TRACE_FLASH_OFFSET +
                          WATCHDOG_TRACE_FLASH_SIZE -
                          WATCHDOG_TRACE_FLASH_BANK_SIZE ||
        watchdog_trace_flash_erase_bank(target_bank) != 0 ||
        watchdog_trace_flash_program_bytes(
            bank_offset, header, BOOT_WATCHDOG_TRACE_FLASH_HEADER_SIZE) != 0)
    {
        return -1;
    }

    payload_offset = bank_offset + BOOT_WATCHDOG_TRACE_FLASH_HEADER_SIZE;
    if (payload_offset + sizeof(watchdog_trace_ring_t) >
            bank_offset + WATCHDOG_TRACE_FLASH_BANK_SIZE ||
        watchdog_trace_flash_program_payload(
            payload_offset, (const volatile uint8_t *)ring,
            (uint32_t)sizeof(watchdog_trace_ring_t)) != 0)
    {
        return -1;
    }

    /* Verify the complete uncommitted bank through the mapped window before
     * making it discoverable. The other bank remains committed throughout. */
    memset(&info, 0, sizeof(info));
    if (watchdog_trace_flash_verify_bank(
            target_bank, UINT32_MAX, &info) != 0 ||
        info.reset_boot_sequence != reset_boot_sequence ||
        info.app_boot_sequence != scan->boot_sequence ||
        info.fingerprint_crc32 != scan->fingerprint_crc32 ||
        info.payload_crc32 != payload_crc32)
    {
        return -1;
    }

    /* The single commit word is programmed last. */
    if (watchdog_trace_flash_program(
            bank_offset +
                BOOT_WATCHDOG_TRACE_WORD_HEADER_COMMIT * sizeof(uint32_t),
            (const uint8_t *)&commit, (uint32_t)sizeof(commit)) != 0)
    {
        return -1;
    }
    memset(&info, 0, sizeof(info));
    return watchdog_trace_flash_verify_bank(
        target_bank, BOOT_WATCHDOG_TRACE_FLASH_HEADER_COMMIT, &info);
}

int boot_watchdog_trace_flash_flush(uint32_t reset_flags,
                                    int journal_flush_result)
{
    watchdog_record_header_t record_header;
    watchdog_trace_scan_result_t capture_scan;
    watchdog_trace_flash_bank_info_t banks[WATCHDOG_TRACE_FLASH_BANK_COUNT];
    const volatile watchdog_trace_ring_t *ring =
        (const volatile watchdog_trace_ring_t *)(uintptr_t)
            BOOT_WATCHDOG_TRACE_RAM_ADDRESS;
    uint32_t target_bank;
    uint32_t payload_crc32;
    uint32_t journal_flash_sequence;
    uint32_t index;
    watchdog_flash_policy_state_t bank_states[WATCHDOG_TRACE_FLASH_BANK_COUNT];
    uint32_t bank_sequences[WATCHDOG_TRACE_FLASH_BANK_COUNT];
    size_t target;

    if (s_trace_prepare_snapshot.ready == 0U)
    {
        return -1;
    }
    if (reset_flags != s_trace_prepare_snapshot.reset_flags ||
        s_trace_prepare_snapshot.eligible == 0U)
    {
        return 0;
    }
    if (!watchdog_record_active_header(&g_watchdog_retained_record,
                                       &record_header, NULL))
    {
        return -1;
    }
    if (record_header.boot_sequence !=
            s_trace_prepare_snapshot.boot_sequence ||
        record_header.source_fingerprint_crc32 !=
            s_trace_prepare_snapshot.fingerprint_crc32)
    {
        return -1;
    }
    if (g_watchdog_retained_record.footer.reserved[
            WATCHDOG_TRACE_PENDING_WORD] != WATCHDOG_TRACE_PENDING_MAGIC ||
        g_watchdog_retained_record.footer.reserved[
            WATCHDOG_TRACE_PENDING_FLAGS_WORD] !=
            s_trace_prepare_snapshot.incident_reset_flags)
    {
        return -1;
    }
    if (!watchdog_trace_capture_scan(&record_header, ring, &capture_scan, 1))
    {
        /* A pending marker must not freeze App startup around a damaged ring. */
        watchdog_trace_pending_clear();
        return 0;
    }

    payload_crc32 = watchdog_trace_crc32(
        ring, sizeof(watchdog_trace_ring_t));
    if (capture_scan.boot_sequence !=
            s_trace_prepare_snapshot.app_boot_sequence ||
        watchdog_trace_next_sequence(capture_scan.boot_sequence) !=
            s_trace_prepare_snapshot.reset_boot_sequence ||
        capture_scan.fingerprint_crc32 !=
            s_trace_prepare_snapshot.fingerprint_crc32 ||
        payload_crc32 != s_trace_prepare_snapshot.payload_crc32)
    {
        /* Leave pending intact if the valid ring changed after prepare. A
         * fresh Boot will take a new stable snapshot before retrying. */
        return 0;
    }
    if (watchdog_trace_flash_scan_banks(banks) != 0)
    {
        return -1;
    }

    /* A retry of an already committed capture must not consume another erase
     * cycle. This also makes the operation idempotent within one Boot pass. */
    for (index = 0U; index < WATCHDOG_TRACE_FLASH_BANK_COUNT; ++index)
    {
        if (banks[index].valid != 0U &&
            banks[index].reset_boot_sequence ==
                s_trace_prepare_snapshot.reset_boot_sequence &&
            banks[index].app_boot_sequence == capture_scan.boot_sequence &&
            banks[index].fingerprint_crc32 == capture_scan.fingerprint_crc32 &&
            banks[index].payload_crc32 == payload_crc32)
        {
            watchdog_trace_pending_clear();
            return 0;
        }
    }

    for (index = 0U; index < WATCHDOG_TRACE_FLASH_BANK_COUNT; ++index)
    {
        bank_states[index] = banks[index].state;
        bank_sequences[index] = banks[index].bank_sequence;
    }
    if (watchdog_flash_policy_choose_trace_bank(
            bank_states, bank_sequences, WATCHDOG_TRACE_FLASH_BANK_COUNT,
            &target) != 0)
    {
        return -1;
    }
    target_bank = (uint32_t)target;

    journal_flash_sequence = journal_flush_result == 0
        ? g_watchdog_retained_record.footer.flash_sequence : 0U;
    if (watchdog_trace_flash_commit_capture(
            target_bank, s_trace_prepare_snapshot.reset_boot_sequence,
            s_trace_prepare_snapshot.incident_reset_flags,
            journal_flash_sequence, ring, &capture_scan,
            payload_crc32) != 0)
    {
        return -1;
    }
    watchdog_trace_pending_clear();
    return 0;
}
