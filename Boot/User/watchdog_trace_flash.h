/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ARTPI2_BOOT_WATCHDOG_TRACE_FLASH_H
#define ARTPI2_BOOT_WATCHDOG_TRACE_FLASH_H

#include <stdint.h>

#define BOOT_WATCHDOG_TRACE_RAM_ADDRESS 0x2406C9A0UL

/* WDF1: a fixed 64-word, little-endian bank header followed by the raw
 * watchdog_trace_ring_t payload. Keep this layout in sync with the host
 * watchdog trace decoder. */
#define BOOT_WATCHDOG_TRACE_FLASH_MAGIC          0x57444631UL
#define BOOT_WATCHDOG_TRACE_FLASH_VERSION         1U
#define BOOT_WATCHDOG_TRACE_FLASH_HEADER_SIZE    256U
#define BOOT_WATCHDOG_TRACE_FLASH_HEADER_COMMIT  0x54524631UL
#define BOOT_WATCHDOG_TRACE_FLASH_HEADER_WORDS   64U

enum
{
    BOOT_WATCHDOG_TRACE_WORD_MAGIC = 0U,
    BOOT_WATCHDOG_TRACE_WORD_VERSION = 1U,
    BOOT_WATCHDOG_TRACE_WORD_HEADER_SIZE = 2U,
    BOOT_WATCHDOG_TRACE_WORD_PAYLOAD_SIZE = 3U,
    BOOT_WATCHDOG_TRACE_WORD_BANK_SEQUENCE = 4U,
    BOOT_WATCHDOG_TRACE_WORD_APP_BOOT_SEQUENCE = 5U,
    BOOT_WATCHDOG_TRACE_WORD_RESET_BOOT_SEQUENCE = 6U,
    BOOT_WATCHDOG_TRACE_WORD_RESET_FLAGS = 7U,
    BOOT_WATCHDOG_TRACE_WORD_JOURNAL_FLASH_SEQUENCE = 8U,
    BOOT_WATCHDOG_TRACE_WORD_FINGERPRINT_CRC32 = 9U,
    BOOT_WATCHDOG_TRACE_WORD_VALID_COUNT = 10U,
    BOOT_WATCHDOG_TRACE_WORD_REJECTED_COUNT = 11U,
    BOOT_WATCHDOG_TRACE_WORD_OLDEST_SEQUENCE = 12U,
    BOOT_WATCHDOG_TRACE_WORD_NEWEST_SEQUENCE = 13U,
    BOOT_WATCHDOG_TRACE_WORD_SAMPLES_CRC32 = 14U,
    BOOT_WATCHDOG_TRACE_WORD_PAYLOAD_CRC32 = 15U,
    BOOT_WATCHDOG_TRACE_WORD_HEADER_CRC32 = 16U,
    BOOT_WATCHDOG_TRACE_WORD_HEADER_COMMIT = 17U,
    BOOT_WATCHDOG_TRACE_WORD_BANK_INDEX = 18U,
    BOOT_WATCHDOG_TRACE_WORD_RESERVED0 = 19U
};

/* Snapshot reset eligibility before the journal commit can clear the fault
 * record. This is RAM-only and performs no Flash access. */
int boot_watchdog_trace_flash_prepare(uint32_t reset_flags);

/* Preserve a synchronous fault trace request before watchdog_record_boot_begin
 * consumes its one-shot fault-reset marker. This only arms the SRAM retry
 * marker; the trace is scanned and snapshotted later by prepare(). */
int boot_watchdog_trace_flash_preserve_fault(uint32_t reset_flags);

/* Returns zero for an ineligible/invalid capture or a successful flush;
 * returns -1 only when an eligible capture could not be committed. */
int boot_watchdog_trace_flash_flush(uint32_t reset_flags,
                                    int journal_flush_result);

#endif /* ARTPI2_BOOT_WATCHDOG_TRACE_FLASH_H */
