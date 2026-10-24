/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ARTPI2_WATCHDOG_TRACE_H
#define ARTPI2_WATCHDOG_TRACE_H

#include <stddef.h>
#include <stdint.h>

/* Independent trace sidecar format. It does not change watchdog_record V2. */
#define WATCHDOG_TRACE_MAGIC          0x57545243UL /* WTRC */
#define WATCHDOG_TRACE_VERSION        1U
#define WATCHDOG_TRACE_CAPACITY       912U
#define WATCHDOG_TRACE_HEADER_COMMIT  0x54524331UL /* TRC1 */
#define WATCHDOG_TRACE_SEQUENCE_NONE  0U
#define WATCHDOG_TRACE_INDEX_NONE     UINT32_MAX

typedef enum watchdog_trace_status
{
    WATCHDOG_TRACE_OK = 0,
    WATCHDOG_TRACE_ERROR_ARGUMENT = -1,
    WATCHDOG_TRACE_ERROR_ALIGNMENT = -2,
    WATCHDOG_TRACE_ERROR_HEADER = -3,
    WATCHDOG_TRACE_ERROR_SEQUENCE = -4,
    WATCHDOG_TRACE_ERROR_NOT_FOUND = -5
} watchdog_trace_status_t;

/* All fields are 32-bit so the platform can place the structure in retained
 * RAM and perform aligned word stores. `next_index` is mutable and excluded
 * from header_crc32; the remaining fields are CRC-protected. */
typedef struct watchdog_trace_header
{
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t boot_sequence;
    uint32_t fingerprint_crc32;
    uint32_t header_crc32;
    uint32_t next_index;
    uint32_t commit;
} watchdog_trace_header_t;

/* `sequence_commit` is invalidated before a slot is replaced and written last.
 * Sequence zero is reserved to mean an uncommitted/empty slot. */
typedef struct watchdog_trace_sample
{
    uint32_t cycle_count;
    uint32_t pc;
    uint32_t lr;
    uint32_t sp;
    uint32_t packed_exc_xpsr;
    uint32_t sequence_commit;
} watchdog_trace_sample_t;

typedef struct watchdog_trace_ring
{
    watchdog_trace_header_t header;
    watchdog_trace_sample_t samples[WATCHDOG_TRACE_CAPACITY];
} watchdog_trace_ring_t;

typedef struct watchdog_trace_scan_result
{
    uint32_t boot_sequence;
    uint32_t fingerprint_crc32;
    uint32_t next_index;
    uint32_t valid_count;
    uint32_t rejected_count;
    uint32_t oldest_sequence;
    uint32_t oldest_index;
    uint32_t newest_sequence;
    uint32_t newest_index;
    uint32_t samples_crc32;
} watchdog_trace_scan_result_t;

/* Standard reflected CRC-32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF).
 * The input may point to volatile retained memory. */
uint32_t watchdog_trace_crc32(const volatile void *data, size_t length);

/* Initialize metadata and invalidate all sample commit words. This is a
 * startup operation, not part of the 1 kHz append path. */
int watchdog_trace_initialize(volatile watchdog_trace_ring_t *ring,
                              uint32_t boot_sequence,
                              uint32_t fingerprint_crc32);

int watchdog_trace_header_valid(
    const volatile watchdog_trace_header_t *header);

/* Append is constant-work and single-writer only. The caller supplies a
 * non-zero monotonically incrementing sequence (skip zero at wrap). Slots are
 * selected by the retained next_index cursor, independent of sequence wrap. */
int watchdog_trace_append(volatile watchdog_trace_ring_t *ring,
                          uint32_t sequence,
                          uint32_t cycle_count,
                          uint32_t pc,
                          uint32_t lr,
                          uint32_t sp,
                          uint32_t packed_exc_xpsr);

/* Scan validates the header CRC and keeps committed samples within the
 * capacity-sized sequence window ending at the newest sample. The returned
 * samples_crc32 covers the entire sample array in physical slot order in
 * canonical little-endian word order, including invalid slots, for
 * Boot/Flash transfer verification. */
int watchdog_trace_scan(const volatile watchdog_trace_ring_t *ring,
                        watchdog_trace_scan_result_t *result);

/* Scan all slots for the exact non-zero sequence, returning NOT_FOUND if it
 * has been overwritten, is outside the latest window, or is not committed. */
int watchdog_trace_get_sequence(
    const volatile watchdog_trace_ring_t *ring,
    uint32_t sequence,
    watchdog_trace_sample_t *sample);

/* Sequence helper for the producer. Zero remains reserved across wrap. */
uint32_t watchdog_trace_next_sequence(uint32_t sequence);

typedef char watchdog_trace_header_size_must_be_32[
    (sizeof(watchdog_trace_header_t) == 32U) ? 1 : -1];
typedef char watchdog_trace_sample_size_must_be_24[
    (sizeof(watchdog_trace_sample_t) == 24U) ? 1 : -1];
typedef char watchdog_trace_sample_commit_offset_must_be_20[
    (offsetof(watchdog_trace_sample_t, sequence_commit) == 20U) ? 1 : -1];
typedef char watchdog_trace_capacity_must_be_912[
    (WATCHDOG_TRACE_CAPACITY == 912U) ? 1 : -1];
typedef char watchdog_trace_ring_size_must_be_0x55a0[
    (sizeof(watchdog_trace_ring_t) == 0x55A0U) ? 1 : -1];

#endif /* ARTPI2_WATCHDOG_TRACE_H */
