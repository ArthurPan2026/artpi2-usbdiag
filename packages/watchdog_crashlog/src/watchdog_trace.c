/* SPDX-License-Identifier: Apache-2.0 */
#include "watchdog_trace.h"

#include <limits.h>

#define WATCHDOG_TRACE_CRC32_POLYNOMIAL 0xEDB88320UL

/* Ports may override this macro if their compiler needs a different DSB
 * spelling. Host builds use a compiler/full memory fence. */
#ifndef WATCHDOG_TRACE_MEMORY_BARRIER
#if (defined(__arm__) || defined(__thumb__)) && defined(__GNUC__)
#define WATCHDOG_TRACE_MEMORY_BARRIER() \
    __asm volatile("dsb sy" ::: "memory")
#elif defined(__GNUC__)
#define WATCHDOG_TRACE_MEMORY_BARRIER() __sync_synchronize()
#else
#define WATCHDOG_TRACE_MEMORY_BARRIER() \
    do { volatile uint32_t watchdog_trace_barrier = 0U; \
         (void)watchdog_trace_barrier; } while (0)
#endif
#endif

static uint32_t watchdog_trace_crc32_update_byte(uint32_t crc,
                                                  uint8_t byte)
{
    unsigned bit;

    crc ^= byte;
    for (bit = 0U; bit < 8U; ++bit)
    {
        crc = (crc >> 1U) ^
              ((crc & 1U) != 0U ? WATCHDOG_TRACE_CRC32_POLYNOMIAL : 0U);
    }
    return crc;
}

static uint32_t watchdog_trace_crc32_update_word(uint32_t crc,
                                                 uint32_t word)
{
    unsigned byte_index;

    for (byte_index = 0U; byte_index < 4U; ++byte_index)
    {
        crc = watchdog_trace_crc32_update_byte(
            crc, (uint8_t)(word >> (byte_index * 8U)));
    }
    return crc;
}

static uint32_t watchdog_trace_header_crc(
    const volatile watchdog_trace_header_t *header)
{
    uint32_t crc = 0xFFFFFFFFUL;

    crc = watchdog_trace_crc32_update_word(crc, header->magic);
    crc = watchdog_trace_crc32_update_word(crc, header->version);
    crc = watchdog_trace_crc32_update_word(crc, header->capacity);
    crc = watchdog_trace_crc32_update_word(crc, header->boot_sequence);
    crc = watchdog_trace_crc32_update_word(crc,
                                            header->fingerprint_crc32);
    return crc ^ 0xFFFFFFFFUL;
}

static uint32_t watchdog_trace_samples_crc(
    const volatile watchdog_trace_sample_t *samples)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t index;

    for (index = 0U; index < WATCHDOG_TRACE_CAPACITY; ++index)
    {
        const volatile watchdog_trace_sample_t *sample = &samples[index];

        crc = watchdog_trace_crc32_update_word(crc, sample->cycle_count);
        crc = watchdog_trace_crc32_update_word(crc, sample->pc);
        crc = watchdog_trace_crc32_update_word(crc, sample->lr);
        crc = watchdog_trace_crc32_update_word(crc, sample->sp);
        crc = watchdog_trace_crc32_update_word(crc,
                                                sample->packed_exc_xpsr);
        crc = watchdog_trace_crc32_update_word(crc,
                                                sample->sequence_commit);
    }
    return crc ^ 0xFFFFFFFFUL;
}

static int watchdog_trace_is_aligned(const volatile void *pointer)
{
    return pointer != NULL &&
           (((uintptr_t)pointer & (uintptr_t)(sizeof(uint32_t) - 1U)) == 0U);
}

static int watchdog_trace_header_quick_valid(
    const volatile watchdog_trace_header_t *header)
{
    return watchdog_trace_is_aligned(header) &&
           header->magic == WATCHDOG_TRACE_MAGIC &&
           header->version == WATCHDOG_TRACE_VERSION &&
           header->capacity == WATCHDOG_TRACE_CAPACITY &&
           header->next_index < WATCHDOG_TRACE_CAPACITY &&
           header->commit == WATCHDOG_TRACE_HEADER_COMMIT;
}

static int watchdog_trace_sequence_newer(uint32_t left, uint32_t right)
{
    uint32_t difference = left - right;

    return difference != 0U && difference < 0x80000000UL;
}

static int watchdog_trace_sequence_age(uint32_t newest, uint32_t sequence,
                                       uint32_t *age)
{
    if (newest == WATCHDOG_TRACE_SEQUENCE_NONE ||
        sequence == WATCHDOG_TRACE_SEQUENCE_NONE || age == NULL)
    {
        return 0;
    }
    if (sequence == newest)
    {
        *age = 0U;
        return 1;
    }
    if (!watchdog_trace_sequence_newer(newest, sequence))
    {
        return 0;
    }

    /* Sequence zero is skipped. Correct the modular delta by one when the
     * chronological window crosses UINT32_MAX -> 1. */
    if (newest < sequence)
    {
        *age = (UINT32_MAX - sequence) + newest;
    }
    else
    {
        *age = newest - sequence;
    }
    return 1;
}

uint32_t watchdog_trace_crc32(const volatile void *data, size_t length)
{
    const volatile uint8_t *bytes = (const volatile uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFUL;
    size_t index;

    if (bytes == NULL)
    {
        return 0U;
    }
    for (index = 0U; index < length; ++index)
    {
        crc = watchdog_trace_crc32_update_byte(crc, bytes[index]);
    }
    return crc ^ 0xFFFFFFFFUL;
}

int watchdog_trace_header_valid(
    const volatile watchdog_trace_header_t *header)
{
    if (!watchdog_trace_is_aligned(header) ||
        header->magic != WATCHDOG_TRACE_MAGIC ||
        header->version != WATCHDOG_TRACE_VERSION ||
        header->capacity != WATCHDOG_TRACE_CAPACITY ||
        header->commit != WATCHDOG_TRACE_HEADER_COMMIT ||
        header->next_index >= WATCHDOG_TRACE_CAPACITY)
    {
        return 0;
    }
    return header->header_crc32 == watchdog_trace_header_crc(header);
}

int watchdog_trace_initialize(volatile watchdog_trace_ring_t *ring,
                              uint32_t boot_sequence,
                              uint32_t fingerprint_crc32)
{
    uint32_t index;
    volatile watchdog_trace_header_t *header;

    if (!watchdog_trace_is_aligned(ring))
    {
        return ring == NULL ? WATCHDOG_TRACE_ERROR_ARGUMENT
                            : WATCHDOG_TRACE_ERROR_ALIGNMENT;
    }

    header = &ring->header;
    header->commit = 0U;
    WATCHDOG_TRACE_MEMORY_BARRIER();
    for (index = 0U; index < WATCHDOG_TRACE_CAPACITY; ++index)
    {
        ring->samples[index].sequence_commit = WATCHDOG_TRACE_SEQUENCE_NONE;
    }

    header->magic = WATCHDOG_TRACE_MAGIC;
    header->version = WATCHDOG_TRACE_VERSION;
    header->capacity = WATCHDOG_TRACE_CAPACITY;
    header->boot_sequence = boot_sequence;
    header->fingerprint_crc32 = fingerprint_crc32;
    header->next_index = 0U;
    header->header_crc32 = watchdog_trace_header_crc(header);
    WATCHDOG_TRACE_MEMORY_BARRIER();
    header->commit = WATCHDOG_TRACE_HEADER_COMMIT;
    WATCHDOG_TRACE_MEMORY_BARRIER();
    return WATCHDOG_TRACE_OK;
}

int watchdog_trace_append(volatile watchdog_trace_ring_t *ring,
                          uint32_t sequence,
                          uint32_t cycle_count,
                          uint32_t pc,
                          uint32_t lr,
                          uint32_t sp,
                          uint32_t packed_exc_xpsr)
{
    uint32_t index;
    volatile watchdog_trace_sample_t *sample;

    if (!watchdog_trace_is_aligned(ring))
    {
        return ring == NULL ? WATCHDOG_TRACE_ERROR_ARGUMENT
                            : WATCHDOG_TRACE_ERROR_ALIGNMENT;
    }
    if (sequence == WATCHDOG_TRACE_SEQUENCE_NONE)
    {
        return WATCHDOG_TRACE_ERROR_SEQUENCE;
    }
    /* Header CRC is checked by the reset-time scanner. Keep the 1 kHz path
     * bounded to fixed metadata comparisons plus aligned word stores. */
    if (!watchdog_trace_header_quick_valid(&ring->header))
    {
        return WATCHDOG_TRACE_ERROR_HEADER;
    }

    index = ring->header.next_index;
    sample = &ring->samples[index];
    sample->sequence_commit = WATCHDOG_TRACE_SEQUENCE_NONE;
    WATCHDOG_TRACE_MEMORY_BARRIER();
    sample->cycle_count = cycle_count;
    sample->pc = pc;
    sample->lr = lr;
    sample->sp = sp;
    sample->packed_exc_xpsr = packed_exc_xpsr;
    WATCHDOG_TRACE_MEMORY_BARRIER();
    sample->sequence_commit = sequence;
    WATCHDOG_TRACE_MEMORY_BARRIER();
    ++index;
    if (index == WATCHDOG_TRACE_CAPACITY)
    {
        index = 0U;
    }
    ring->header.next_index = index;
    WATCHDOG_TRACE_MEMORY_BARRIER();
    return WATCHDOG_TRACE_OK;
}

int watchdog_trace_scan(const volatile watchdog_trace_ring_t *ring,
                        watchdog_trace_scan_result_t *result)
{
    uint32_t index;
    uint32_t newest_sequence = WATCHDOG_TRACE_SEQUENCE_NONE;
    uint32_t oldest_age = 0U;

    if (ring == NULL || result == NULL)
    {
        return WATCHDOG_TRACE_ERROR_ARGUMENT;
    }
    if (!watchdog_trace_is_aligned(ring) ||
        !watchdog_trace_is_aligned(result))
    {
        return WATCHDOG_TRACE_ERROR_ALIGNMENT;
    }
    if (!watchdog_trace_header_valid(&ring->header))
    {
        return WATCHDOG_TRACE_ERROR_HEADER;
    }

    result->boot_sequence = ring->header.boot_sequence;
    result->fingerprint_crc32 = ring->header.fingerprint_crc32;
    result->next_index = ring->header.next_index;
    result->valid_count = 0U;
    result->rejected_count = 0U;
    result->oldest_sequence = WATCHDOG_TRACE_SEQUENCE_NONE;
    result->oldest_index = WATCHDOG_TRACE_INDEX_NONE;
    result->newest_sequence = WATCHDOG_TRACE_SEQUENCE_NONE;
    result->newest_index = WATCHDOG_TRACE_INDEX_NONE;

    for (index = 0U; index < WATCHDOG_TRACE_CAPACITY; ++index)
    {
        uint32_t sequence = ring->samples[index].sequence_commit;

        if (sequence != WATCHDOG_TRACE_SEQUENCE_NONE &&
            (newest_sequence == WATCHDOG_TRACE_SEQUENCE_NONE ||
             watchdog_trace_sequence_newer(sequence, newest_sequence)))
        {
            newest_sequence = sequence;
        }
    }

    result->newest_sequence = newest_sequence;
    for (index = 0U; index < WATCHDOG_TRACE_CAPACITY; ++index)
    {
        uint32_t sequence = ring->samples[index].sequence_commit;
        uint32_t age;

        if (sequence == WATCHDOG_TRACE_SEQUENCE_NONE)
        {
            continue;
        }
        if (!watchdog_trace_sequence_age(newest_sequence, sequence, &age) ||
            age >= WATCHDOG_TRACE_CAPACITY)
        {
            ++result->rejected_count;
            continue;
        }
        if (result->valid_count == 0U || age > oldest_age)
        {
            oldest_age = age;
            result->oldest_sequence = sequence;
            result->oldest_index = index;
        }
        if (sequence == newest_sequence)
        {
            result->newest_index = index;
        }
        ++result->valid_count;
    }

    result->samples_crc32 = watchdog_trace_samples_crc(ring->samples);
    return WATCHDOG_TRACE_OK;
}

int watchdog_trace_get_sequence(
    const volatile watchdog_trace_ring_t *ring,
    uint32_t sequence,
    watchdog_trace_sample_t *sample_out)
{
    uint32_t index;
    uint32_t age;
    uint32_t found_index = WATCHDOG_TRACE_INDEX_NONE;
    watchdog_trace_scan_result_t scan;
    const volatile watchdog_trace_sample_t *source;

    if (ring == NULL || sample_out == NULL)
    {
        return WATCHDOG_TRACE_ERROR_ARGUMENT;
    }
    if (!watchdog_trace_is_aligned(ring) ||
        !watchdog_trace_is_aligned(sample_out))
    {
        return WATCHDOG_TRACE_ERROR_ALIGNMENT;
    }
    if (sequence == WATCHDOG_TRACE_SEQUENCE_NONE)
    {
        return WATCHDOG_TRACE_ERROR_SEQUENCE;
    }
    if (!watchdog_trace_header_valid(&ring->header))
    {
        return WATCHDOG_TRACE_ERROR_HEADER;
    }
    if (watchdog_trace_scan(ring, &scan) != WATCHDOG_TRACE_OK ||
        scan.valid_count == 0U ||
        !watchdog_trace_sequence_age(scan.newest_sequence, sequence, &age) ||
        age >= WATCHDOG_TRACE_CAPACITY)
    {
        return WATCHDOG_TRACE_ERROR_NOT_FOUND;
    }

    for (index = 0U; index < WATCHDOG_TRACE_CAPACITY; ++index)
    {
        if (ring->samples[index].sequence_commit == sequence)
        {
            if (found_index != WATCHDOG_TRACE_INDEX_NONE)
            {
                return WATCHDOG_TRACE_ERROR_SEQUENCE;
            }
            found_index = index;
        }
    }
    if (found_index == WATCHDOG_TRACE_INDEX_NONE)
    {
        return WATCHDOG_TRACE_ERROR_NOT_FOUND;
    }
    source = &ring->samples[found_index];
    sample_out->cycle_count = source->cycle_count;
    sample_out->pc = source->pc;
    sample_out->lr = source->lr;
    sample_out->sp = source->sp;
    sample_out->packed_exc_xpsr = source->packed_exc_xpsr;
    sample_out->sequence_commit = source->sequence_commit;
    return WATCHDOG_TRACE_OK;
}

uint32_t watchdog_trace_next_sequence(uint32_t sequence)
{
    if (sequence == UINT32_MAX)
    {
        return 1U;
    }
    ++sequence;
    return sequence == WATCHDOG_TRACE_SEQUENCE_NONE ? 1U : sequence;
}
