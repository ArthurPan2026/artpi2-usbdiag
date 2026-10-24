/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WATCHDOG_FLASH_POLICY_H
#define WATCHDOG_FLASH_POLICY_H

#include <stddef.h>
#include <stdint.h>

/* Pure storage-selection policy shared by the Boot adapter and host tests.
 * The caller performs the actual Flash reads, erase and programming. */
typedef enum watchdog_flash_policy_state
{
    WATCHDOG_FLASH_POLICY_READ_ERROR = 0,
    WATCHDOG_FLASH_POLICY_BLANK,
    WATCHDOG_FLASH_POLICY_VALID,
    WATCHDOG_FLASH_POLICY_DAMAGED
} watchdog_flash_policy_state_t;

typedef struct watchdog_flash_policy_counts
{
    size_t blank;
    size_t valid;
    size_t damaged;
    size_t read_errors;
} watchdog_flash_policy_counts_t;

static inline int watchdog_flash_policy_is_erased(const uint8_t *bytes,
                                                   size_t length,
                                                   uint8_t erased_value)
{
    size_t index;

    if (bytes == NULL || length == 0U)
    {
        return -1;
    }
    for (index = 0U; index < length; ++index)
    {
        if (bytes[index] != erased_value)
        {
            return 0;
        }
    }
    return 1;
}

static inline watchdog_flash_policy_state_t watchdog_flash_policy_classify(
    const uint8_t *bytes, size_t length, uint8_t erased_value, int valid)
{
    int erased = watchdog_flash_policy_is_erased(bytes, length, erased_value);

    if (erased < 0)
    {
        return WATCHDOG_FLASH_POLICY_READ_ERROR;
    }
    if (erased != 0)
    {
        return WATCHDOG_FLASH_POLICY_BLANK;
    }
    return valid != 0 ? WATCHDOG_FLASH_POLICY_VALID
                      : WATCHDOG_FLASH_POLICY_DAMAGED;
}

static inline int watchdog_flash_policy_count(
    const watchdog_flash_policy_state_t *states, size_t count,
    watchdog_flash_policy_counts_t *result)
{
    size_t index;

    if (states == NULL || count == 0U || result == NULL)
    {
        return -1;
    }
    result->blank = 0U;
    result->valid = 0U;
    result->damaged = 0U;
    result->read_errors = 0U;
    for (index = 0U; index < count; ++index)
    {
        switch (states[index])
        {
        case WATCHDOG_FLASH_POLICY_BLANK:
            ++result->blank;
            break;
        case WATCHDOG_FLASH_POLICY_VALID:
            ++result->valid;
            break;
        case WATCHDOG_FLASH_POLICY_DAMAGED:
            ++result->damaged;
            break;
        case WATCHDOG_FLASH_POLICY_READ_ERROR:
        default:
            ++result->read_errors;
            break;
        }
    }
    return result->read_errors == 0U ? 0 : -1;
}

/* Choose an append slot without touching Flash. Prefer blank storage to a
 * nonblank invalid slot, and never return a slot containing a valid record. */
static inline int watchdog_flash_policy_choose_append_slot(
    const watchdog_flash_policy_state_t *states, size_t count,
    size_t start_index, size_t *target)
{
    size_t pass;
    size_t offset;

    if (states == NULL || count == 0U || target == NULL ||
        start_index >= count)
    {
        return -1;
    }
    for (pass = 0U; pass < 2U; ++pass)
    {
        watchdog_flash_policy_state_t wanted =
            pass == 0U ? WATCHDOG_FLASH_POLICY_BLANK
                       : WATCHDOG_FLASH_POLICY_DAMAGED;
        for (offset = 0U; offset < count; ++offset)
        {
            size_t index = (start_index + offset) % count;
            if (states[index] == WATCHDOG_FLASH_POLICY_READ_ERROR)
            {
                return -1;
            }
            if (states[index] == wanted)
            {
                *target = index;
                return 0;
            }
        }
    }
    return -1;
}

/* For two-bank transactional storage, retain the newest valid bank and write
 * the other one. With no valid bank, prefer blank storage over damaged data. */
static inline int watchdog_flash_policy_choose_trace_bank(
    const watchdog_flash_policy_state_t *states,
    const uint32_t *sequences, size_t count, size_t *target)
{
    size_t index;
    size_t valid_count = 0U;
    size_t only_valid = 0U;

    if (states == NULL || sequences == NULL || count < 2U || target == NULL)
    {
        return -1;
    }
    for (index = 0U; index < count; ++index)
    {
        if (states[index] == WATCHDOG_FLASH_POLICY_READ_ERROR)
        {
            return -1;
        }
        if (states[index] == WATCHDOG_FLASH_POLICY_VALID)
        {
            only_valid = index;
            ++valid_count;
        }
    }
    if (valid_count == 1U)
    {
        *target = (only_valid + 1U) % count;
        return 0;
    }
    if (valid_count == count)
    {
        /* Signed subtraction is the standard wrap-safe sequence ordering for
         * monotonically increasing 32-bit counters with a bounded gap. */
        size_t oldest = 0U;
        for (index = 1U; index < count; ++index)
        {
            if ((int32_t)(sequences[index] - sequences[oldest]) < 0)
            {
                oldest = index;
            }
        }
        *target = oldest;
        return 0;
    }
    for (index = 0U; index < count; ++index)
    {
        if (states[index] == WATCHDOG_FLASH_POLICY_BLANK)
        {
            *target = index;
            return 0;
        }
    }
    for (index = 0U; index < count; ++index)
    {
        if (states[index] == WATCHDOG_FLASH_POLICY_DAMAGED)
        {
            *target = index;
            return 0;
        }
    }
    return -1;
}

#endif /* WATCHDOG_FLASH_POLICY_H */
