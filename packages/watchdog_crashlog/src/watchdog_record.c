/* SPDX-License-Identifier: Apache-2.0 */
#include "watchdog_record.h"
#include "watchdog_incident_id.h"

#include <string.h>

#define WATCHDOG_CRC32_POLYNOMIAL 0xEDB88320UL
#define WATCHDOG_INCIDENT_ID_MAGIC       0x49494431UL /* IID1 */
#define WATCHDOG_INCIDENT_ID_COMMIT_WORD 11U
#define WATCHDOG_INCIDENT_ID_LOW_WORD    12U
#define WATCHDOG_INCIDENT_ID_HIGH_WORD   13U
#define WATCHDOG_INCIDENT_ID_CRC_WORD    14U

typedef char watchdog_incident_id_reserved_fits[
    (WATCHDOG_INCIDENT_ID_CRC_WORD < 16U) ? 1 : -1];

static void watchdog_memory_barrier(void)
{
#if defined(__arm__) || defined(__thumb__)
    __asm volatile("dmb sy" ::: "memory");
#elif defined(__GNUC__)
    __sync_synchronize();
#else
    volatile uint32_t barrier = 0U;
    (void)barrier;
#endif
}

uint32_t watchdog_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFUL;
    size_t i;
    unsigned bit;

    if (bytes == NULL)
    {
        return 0U;
    }
    for (i = 0U; i < length; ++i)
    {
        crc ^= bytes[i];
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc >> 1U) ^
                  ((crc & 1U) ? WATCHDOG_CRC32_POLYNOMIAL : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

uint32_t watchdog_crc32_volatile_words(const volatile uint32_t *words,
                                      size_t word_count)
{
    uint32_t crc = 0xFFFFFFFFUL;
    size_t i;
    unsigned byte_index;
    unsigned bit;

    if (words == NULL)
    {
        return 0U;
    }
    for (i = 0U; i < word_count; ++i)
    {
        uint32_t word = words[i];
        for (byte_index = 0U; byte_index < 4U; ++byte_index)
        {
            crc ^= (uint8_t)(word >> (byte_index * 8U));
            for (bit = 0U; bit < 8U; ++bit)
            {
                crc = (crc >> 1U) ^
                      ((crc & 1U) ? WATCHDOG_CRC32_POLYNOMIAL : 0U);
            }
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

int watchdog_header_valid(const volatile watchdog_record_header_t *header)
{
    uint32_t crc;
    if (header == NULL || header->magic != WATCHDOG_RECORD_MAGIC ||
        header->version != WATCHDOG_RECORD_VERSION ||
        header->commit != WATCHDOG_HEADER_COMMIT)
    {
        return 0;
    }
    crc = watchdog_crc32_volatile_words((const volatile uint32_t *)header,
                                        offsetof(watchdog_record_header_t,
                                                 crc32) / sizeof(uint32_t));
    return crc == header->crc32;
}

int watchdog_event_valid(const volatile watchdog_event_record_t *event)
{
    uint32_t crc;
    if (event == NULL || event->sequence == 0U || event->code == 0U)
    {
        return 0;
    }
    crc = watchdog_crc32_volatile_words((const volatile uint32_t *)event, 7U);
    return crc == event->crc32;
}

int watchdog_fault_valid(const volatile watchdog_fault_record_t *fault)
{
    uint32_t crc;
    if (fault == NULL || fault->magic != WATCHDOG_RECORD_MAGIC ||
        fault->version != WATCHDOG_RECORD_VERSION ||
        fault->commit != WATCHDOG_FAULT_COMMIT)
    {
        return 0;
    }
    crc = watchdog_crc32_volatile_words((const volatile uint32_t *)fault,
                                        offsetof(watchdog_fault_record_t,
                                                 crc32) / sizeof(uint32_t));
    return crc == fault->crc32;
}

uint32_t watchdog_flash_record_crc32(const volatile watchdog_retained_record_t *record)
{
    const volatile uint32_t *words;
    uint32_t crc = 0xFFFFFFFFUL;
    size_t word_count = sizeof(*record) / sizeof(uint32_t);
    size_t crc_word = offsetof(watchdog_record_footer_t, flash_crc32) /
                      sizeof(uint32_t) +
                      (offsetof(watchdog_retained_record_t, footer) /
                       sizeof(uint32_t));
    size_t commit_word = offsetof(watchdog_record_footer_t, flash_commit) /
                         sizeof(uint32_t) +
                         (offsetof(watchdog_retained_record_t, footer) /
                          sizeof(uint32_t));
    size_t i;
    unsigned byte_index;
    unsigned bit;

    if (record == NULL)
    {
        return 0U;
    }
    words = (const volatile uint32_t *)record;
    for (i = 0U; i < word_count; ++i)
    {
        uint32_t word = (i == crc_word || i == commit_word) ? 0U : words[i];
        for (byte_index = 0U; byte_index < 4U; ++byte_index)
        {
            crc ^= (uint8_t)(word >> (byte_index * 8U));
            for (bit = 0U; bit < 8U; ++bit)
            {
                crc = (crc >> 1U) ^
                      ((crc & 1U) ? WATCHDOG_CRC32_POLYNOMIAL : 0U);
            }
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

int watchdog_flash_record_valid(const volatile watchdog_retained_record_t *record)
{
    if (record == NULL || record->footer.magic != WATCHDOG_RECORD_MAGIC ||
        record->footer.version != WATCHDOG_RECORD_VERSION ||
        record->footer.flash_commit != WATCHDOG_FLASH_COMMIT ||
        record->footer.flash_sequence == 0U)
    {
        return 0;
    }
    return watchdog_flash_record_crc32(record) == record->footer.flash_crc32;
}

static uint32_t watchdog_incident_id_crc(uint32_t low_word,
                                         uint32_t high_word)
{
    const uint32_t words[3] = {
        WATCHDOG_INCIDENT_ID_MAGIC, low_word, high_word
    };
    return watchdog_crc32_volatile_words(words, 3U);
}

int watchdog_record_incident_id_valid(
    const volatile watchdog_retained_record_t *record, uint64_t *incident_id)
{
    uint32_t low_word;
    uint32_t high_word;
    uint32_t stored_crc;

    if (incident_id != NULL)
    {
        *incident_id = 0U;
    }
    if (record == NULL ||
        record->footer.magic != WATCHDOG_RECORD_MAGIC ||
        record->footer.version != WATCHDOG_RECORD_VERSION ||
        record->footer.reserved[WATCHDOG_INCIDENT_ID_COMMIT_WORD] !=
            WATCHDOG_INCIDENT_ID_MAGIC)
    {
        return 0;
    }

    low_word = record->footer.reserved[WATCHDOG_INCIDENT_ID_LOW_WORD];
    high_word = record->footer.reserved[WATCHDOG_INCIDENT_ID_HIGH_WORD];
    stored_crc = record->footer.reserved[WATCHDOG_INCIDENT_ID_CRC_WORD];
    if ((low_word == 0U && high_word == 0U) ||
        stored_crc != watchdog_incident_id_crc(low_word, high_word))
    {
        return 0;
    }

    if (incident_id != NULL)
    {
        *incident_id = ((uint64_t)high_word << 32U) | low_word;
    }
    return 1;
}

void watchdog_record_incident_id_clear(
    volatile watchdog_retained_record_t *record)
{
    if (record == NULL)
    {
        return;
    }

    /* Invalidate first. A reset during cleanup must never expose a partially
     * updated ID as a committed transaction. */
    record->footer.reserved[WATCHDOG_INCIDENT_ID_COMMIT_WORD] = 0U;
    watchdog_memory_barrier();
    record->footer.reserved[WATCHDOG_INCIDENT_ID_LOW_WORD] = 0U;
    record->footer.reserved[WATCHDOG_INCIDENT_ID_HIGH_WORD] = 0U;
    record->footer.reserved[WATCHDOG_INCIDENT_ID_CRC_WORD] = 0U;
    watchdog_memory_barrier();
}

void watchdog_record_incident_id_commit(
    volatile watchdog_retained_record_t *record, uint64_t incident_id)
{
    uint32_t low_word;
    uint32_t high_word;

    if (record == NULL)
    {
        return;
    }
    if (incident_id == 0U)
    {
        watchdog_record_incident_id_clear(record);
        return;
    }

    low_word = (uint32_t)incident_id;
    high_word = (uint32_t)(incident_id >> 32U);
    record->footer.reserved[WATCHDOG_INCIDENT_ID_COMMIT_WORD] = 0U;
    watchdog_memory_barrier();
    record->footer.reserved[WATCHDOG_INCIDENT_ID_LOW_WORD] = low_word;
    record->footer.reserved[WATCHDOG_INCIDENT_ID_HIGH_WORD] = high_word;
    record->footer.reserved[WATCHDOG_INCIDENT_ID_CRC_WORD] =
        watchdog_incident_id_crc(low_word, high_word);
    watchdog_memory_barrier();
    record->footer.reserved[WATCHDOG_INCIDENT_ID_COMMIT_WORD] =
        WATCHDOG_INCIDENT_ID_MAGIC;
    watchdog_memory_barrier();
}

int watchdog_record_incident_id_next(uint64_t latest_id, uint64_t *next_id)
{
    if (next_id == NULL || latest_id == UINT64_MAX)
    {
        return 0;
    }
    *next_id = latest_id + 1U;
    return *next_id != 0U;
}

int watchdog_record_active_header(const volatile watchdog_retained_record_t *record,
                                  watchdog_record_header_t *header,
                                  unsigned *slot)
{
    int valid0;
    int valid1;
    unsigned selected;
    const volatile watchdog_record_header_t *source;
    uint32_t *destination;
    const volatile uint32_t *source_words;
    size_t i;

    if (record == NULL || header == NULL)
    {
        return 0;
    }
    valid0 = watchdog_header_valid(&record->headers[0]);
    valid1 = watchdog_header_valid(&record->headers[1]);
    if (!valid0 && !valid1)
    {
        return 0;
    }
    selected = (!valid0 || (valid1 &&
                 ((int32_t)(record->headers[1].sequence -
                            record->headers[0].sequence) > 0))) ? 1U : 0U;
    source = &record->headers[selected];
    destination = (uint32_t *)header;
    source_words = (const volatile uint32_t *)source;
    for (i = 0U; i < sizeof(*header) / sizeof(uint32_t); ++i)
    {
        destination[i] = source_words[i];
    }
    if (slot != NULL)
    {
        *slot = selected;
    }
    return 1;
}

void watchdog_record_header_commit(volatile watchdog_retained_record_t *record,
                                  watchdog_record_header_t *header,
                                  unsigned active_slot)
{
    volatile watchdog_record_header_t *destination;
    uint32_t *source_words;
    volatile uint32_t *destination_words;
    size_t i;
    unsigned target_slot;

    if (record == NULL || header == NULL)
    {
        return;
    }
    target_slot = (active_slot & 1U) ^ 1U;
    destination = &record->headers[target_slot];
    header->magic = WATCHDOG_RECORD_MAGIC;
    header->version = WATCHDOG_RECORD_VERSION;
    header->sequence += 1U;
    header->commit = 0U;
    header->crc32 = 0U;
    header->crc32 = watchdog_crc32(header,
                                   offsetof(watchdog_record_header_t, crc32));
    source_words = (uint32_t *)header;
    destination_words = (volatile uint32_t *)destination;
    for (i = 0U; i < (sizeof(*header) / sizeof(uint32_t)) - 1U; ++i)
    {
        destination_words[i] = source_words[i];
    }
    watchdog_memory_barrier();
    destination->commit = WATCHDOG_HEADER_COMMIT;
    watchdog_memory_barrier();
}

static void watchdog_fingerprint_copy(uint32_t destination[6],
                                     const char *source)
{
    uint32_t i;
    const uint8_t *bytes = (const uint8_t *)source;
    for (i = 0U; i < 6U; ++i)
    {
        uint32_t value = 0x20202020UL;
        unsigned byte_index;
        for (byte_index = 0U; byte_index < 4U; ++byte_index)
        {
            uint32_t source_index = i * 4U + byte_index;
            uint8_t byte = (bytes != NULL && bytes[source_index] != 0U)
                               ? bytes[source_index]
                               : (uint8_t)' ';
            value &= ~(0xFFUL << (byte_index * 8U));
            value |= ((uint32_t)byte << (byte_index * 8U));
        }
        destination[i] = value;
    }
}

void watchdog_record_set_build_fingerprint(volatile watchdog_retained_record_t *record,
                                           const char *source_fingerprint)
{
    watchdog_record_header_t header;
    unsigned active_slot;
    uint32_t fingerprint_words[6];

    if (!watchdog_record_active_header(record, &header, &active_slot))
    {
        return;
    }
    header.source_fingerprint_crc32 =
        watchdog_crc32(source_fingerprint,
                       source_fingerprint != NULL ? strlen(source_fingerprint) : 0U);
    watchdog_fingerprint_copy(fingerprint_words, source_fingerprint);
    memcpy(header.source_fingerprint_prefix, fingerprint_words,
           sizeof(fingerprint_words));
    watchdog_record_header_commit(record, &header, active_slot);
}

void watchdog_record_initialize(volatile watchdog_retained_record_t *record,
                                const char *source_fingerprint,
                                uint32_t reset_flags)
{
    volatile uint32_t *words;
    watchdog_record_header_t header;
    uint32_t fingerprint_words[6];
    size_t i;

    if (record == NULL)
    {
        return;
    }
    words = (volatile uint32_t *)record;
    for (i = 0U; i < sizeof(*record) / sizeof(uint32_t); ++i)
    {
        words[i] = 0U;
    }
    watchdog_memory_barrier();

    memset(&header, 0, sizeof(header));
    header.sequence = 0U;
    header.boot_sequence = 1U;
    header.reset_flags = reset_flags;
    header.source_fingerprint_crc32 =
        watchdog_crc32(source_fingerprint,
                       source_fingerprint != NULL ? strlen(source_fingerprint) : 0U);
    watchdog_fingerprint_copy(fingerprint_words, source_fingerprint);
    memcpy(header.source_fingerprint_prefix, fingerprint_words,
           sizeof(fingerprint_words));
    watchdog_record_header_commit(record, &header, 1U);
    record->footer.magic = WATCHDOG_RECORD_MAGIC;
    record->footer.version = WATCHDOG_RECORD_VERSION;
    record->footer.stage_sequence = header.boot_sequence;
    watchdog_memory_barrier();
    /* A clean power-on/reset is not a crash record. The staging marker is
     * committed only after an abnormal event or fault is captured. */
    record->footer.stage_commit = 0U;
    watchdog_memory_barrier();
}

void watchdog_record_boot_begin(volatile watchdog_retained_record_t *record,
                                const char *source_fingerprint,
                                const watchdog_record_boot_context_t *context,
                                const watchdog_record_policy_t *policy,
                                uint32_t *reset_count)
{
    watchdog_record_header_t header;
    unsigned active_slot = 1U;
    uint32_t previous_count = 0U;
    uint32_t raw_reset_flags = context != NULL
                                   ? context->raw_reset_flags : 0U;
    uint32_t reset_causes = context != NULL ? context->reset_causes : 0U;
    uint32_t previous_reset_causes = context != NULL
                                         ? context->previous_reset_causes : 0U;
    uint32_t rtc_second = context != NULL ? context->rtc_second : 0U;
    uint32_t tick = context != NULL ? context->tick : 0U;
    uint32_t reset_window_seconds = policy != NULL
                                        ? policy->reset_window_seconds : 0U;
    uint32_t elapsed = UINT32_MAX;
    int have_header;
    int pending_previous_crash;
    int fault_pending;
    int previous_captured_fault;
    int fault_reset;
    int watchdog_reset;
    int rtc_valid = context != NULL && context->rtc_valid != 0U;
    int abnormal_reset;

    if (record == NULL)
    {
        if (reset_count != NULL) *reset_count = 0U;
        return;
    }

    have_header = watchdog_record_active_header(record, &header, &active_slot);
    if (!have_header)
    {
        watchdog_record_initialize(record, source_fingerprint,
                                   raw_reset_flags);
        (void)watchdog_record_active_header(record, &header, &active_slot);
    }

    previous_count = header.reset_count;
    previous_captured_fault =
        (header.reset_flags & WATCHDOG_RESET_FLAG_CAPTURED_FAULT) != 0U;
    fault_pending = watchdog_fault_valid(&record->fault);
    pending_previous_crash = fault_pending ||
        (record->footer.magic == WATCHDOG_RECORD_MAGIC &&
         record->footer.version == WATCHDOG_RECORD_VERSION &&
         record->footer.stage_commit == WATCHDOG_STAGE_COMMIT &&
         record->footer.reserved[0] == WATCHDOG_PENDING_FLASH_MAGIC);
    fault_reset = fault_pending &&
        record->footer.magic == WATCHDOG_RECORD_MAGIC &&
        record->footer.version == WATCHDOG_RECORD_VERSION &&
        record->footer.stage_commit == WATCHDOG_STAGE_COMMIT &&
        record->footer.stage_sequence == header.boot_sequence &&
        record->footer.reserved[1] == WATCHDOG_FAULT_RESET_REQUESTED &&
        (reset_causes & WATCHDOG_RESET_CAUSE_SOFTWARE) != 0U &&
        (reset_causes & (WATCHDOG_RESET_CAUSE_WATCHDOG |
                         WATCHDOG_RESET_CAUSE_POWER_ON |
                         WATCHDOG_RESET_CAUSE_OTHER)) == 0U;
    watchdog_reset = (reset_causes & WATCHDOG_RESET_CAUSE_WATCHDOG) != 0U;
    abnormal_reset = watchdog_reset || fault_reset;
    if (rtc_valid && header.reset_window_valid != 0U &&
        reset_window_seconds != 0U)
    {
        elapsed = (rtc_second + 86400U - header.reset_window_second) % 86400U;
    }

    if (abnormal_reset && header.active_operation != 0U)
    {
        watchdog_record_append_event(record, tick,
                                     WATCHDOG_EVENT_OPERATION_BEGIN,
                                     header.active_operation,
                                     header.active_started_tick,
                                     header.last_core_pc,
                                     header.last_core_sp);
        watchdog_record_append_event(record, tick,
                                     WATCHDOG_EVENT_HEALTH_STALE,
                                     header.active_stage,
                                     header.active_progress,
                                     header.last_core_pc,
                                     header.last_core_sp);
        /* Preserve the prior operation in the event ring, then begin this
         * boot with no stale ACTIVE lease from the previous tick epoch. */
        (void)watchdog_record_active_header(record, &header, &active_slot);
    }

    header.boot_sequence += 1U;
    if (header.boot_sequence == 0U)
    {
        header.boot_sequence = 1U;
    }
    /* If an earlier crash could not yet be written to Flash, preserve its
     * reset cause across a clean user restart. Do not turn that restart into
     * a new log entry or overwrite the pending abnormal cause. */
    if (abnormal_reset || !pending_previous_crash)
    {
        header.reset_flags = raw_reset_flags;
        if (fault_reset)
        {
            header.reset_flags |= WATCHDOG_RESET_FLAG_CAPTURED_FAULT;
        }
    }
    header.core_ready = 0U;
    header.last_core_tick = 0U;
    header.last_core_progress = 0U;
    header.active_operation = 0U;
    header.active_stage = 0U;
    header.active_started_tick = 0U;
    header.active_timeout_ms = 0U;
    header.active_progress = 0U;

    if (abnormal_reset)
    {
        /* Count consecutive watchdog or captured-fault resets. A deliberate
         * reset/power cycle below breaks the streak. RTC bounds the window
         * when available; without it, consecutive abnormal resets still need
         * to reach recovery rather than restarting the count at one. */
        if (((previous_reset_causes & WATCHDOG_RESET_CAUSE_WATCHDOG) != 0U ||
             previous_captured_fault != 0) &&
            previous_count != 0U &&
            (!rtc_valid ||
             (header.reset_window_valid != 0U &&
              (reset_window_seconds == 0U ||
               elapsed <= reset_window_seconds))))
        {
            header.reset_count = previous_count + 1U;
        }
        else
        {
            header.reset_count = 1U;
            header.reset_window_second = rtc_valid ? rtc_second : 0U;
        }
        header.reset_window_valid = rtc_valid ? 1U : 0U;
    }
    else
    {
        /* A deliberate software/external reset or a power cycle breaks an
         * abnormal reset streak. Its cause is not appended to the journal. */
        header.reset_count = 0U;
        header.reset_window_second = 0U;
        header.reset_window_valid = 0U;
    }

    if (source_fingerprint != NULL)
    {
        header.source_fingerprint_crc32 =
            watchdog_crc32(source_fingerprint, strlen(source_fingerprint));
        {
            uint32_t fingerprint_words[6];
            watchdog_fingerprint_copy(fingerprint_words, source_fingerprint);
            memcpy(header.source_fingerprint_prefix, fingerprint_words,
                   sizeof(fingerprint_words));
        }
    }

    watchdog_record_header_commit(record, &header, active_slot);
    if (abnormal_reset)
    {
        watchdog_record_append_event(record, tick, WATCHDOG_EVENT_RESET_CAUSE,
                                     header.reset_flags, header.reset_count,
                                     0U, 0U);
    }
    if (watchdog_reset || pending_previous_crash || fault_pending)
    {
        /* This marker survives a later non-IWDG reset if Boot could not
         * append the previous crash to external Flash. */
        if (!pending_previous_crash)
        {
            /* A fresh abnormal reset starts a new transaction. Do not let a
             * stale, already committed ID survive into this incident. */
            watchdog_record_incident_id_clear(record);
            watchdog_record_stage_commit(record, header.boot_sequence);
        }
        record->footer.reserved[0] = WATCHDOG_PENDING_FLASH_MAGIC;
        watchdog_memory_barrier();
    }
    else
    {
        /* Drop staging left by normal-operation breadcrumbs. They are useful
         * only as context if a later abnormal event occurs, never as a log on
         * their own. */
        record->footer.stage_commit = 0U;
        record->footer.reserved[0] = 0U;
        watchdog_record_incident_id_clear(record);
        watchdog_memory_barrier();
    }
    /* Consume the one-shot request only after the new header containing the
     * classified reset and streak count has been committed. */
    record->footer.reserved[1] = 0U;
    watchdog_memory_barrier();
    if (reset_count != NULL)
    {
        *reset_count = header.reset_count;
    }
}

void watchdog_record_append_event(volatile watchdog_retained_record_t *record,
                                  uint32_t tick, uint32_t code,
                                  uint32_t arg0, uint32_t arg1,
                                  uint32_t pc, uint32_t sp)
{
    watchdog_record_header_t header;
    watchdog_event_record_t event;
    volatile watchdog_event_record_t *destination;
    uint32_t *source_words;
    volatile uint32_t *destination_words;
    unsigned active_slot;
    size_t i;

    if (!watchdog_record_active_header(record, &header, &active_slot))
    {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.sequence = header.event_next_sequence + 1U;
    if (event.sequence == 0U)
    {
        event.sequence = 1U;
    }
    event.tick = tick;
    event.code = code;
    event.arg0 = arg0;
    event.arg1 = arg1;
    event.pc = pc;
    event.sp = sp;
    event.crc32 = watchdog_crc32(&event,
                                 offsetof(watchdog_event_record_t, crc32));
    destination = &record->events[header.event_next_sequence %
                                  WATCHDOG_EVENT_CAPACITY];
    source_words = (uint32_t *)&event;
    destination_words = (volatile uint32_t *)destination;
    for (i = 0U; i < 7U; ++i)
    {
        destination_words[i] = source_words[i];
    }
    watchdog_memory_barrier();
    destination->crc32 = event.crc32;
    watchdog_memory_barrier();

    header.event_next_sequence = event.sequence;
    if (header.event_count < WATCHDOG_EVENT_CAPACITY)
    {
        header.event_count += 1U;
    }
    watchdog_record_header_commit(record, &header, active_slot);
    if (record->footer.stage_commit != WATCHDOG_STAGE_COMMIT)
    {
        record->footer.stage_sequence = header.boot_sequence;
    }
    watchdog_memory_barrier();
    record->footer.stage_commit = WATCHDOG_STAGE_COMMIT;
    watchdog_memory_barrier();
}

void watchdog_record_fault(volatile watchdog_retained_record_t *record,
                           uint32_t kind,
                           const watchdog_fault_context_t *context)
{
    volatile watchdog_fault_record_t *destination;
    volatile uint32_t *destination_words;
    uint32_t source_words[32];
    uint32_t context_words[20];
    uint32_t crc;
    uint32_t i;

    if (record == NULL)
    {
        return;
    }
    destination = &record->fault;
    destination_words = (volatile uint32_t *)destination;
    destination->commit = 0U;
    /* Keep the runtime breadcrumbs in reserved[95]. A synchronous fault is
     * precisely when those pre-fault checkpoints matter most. */
    for (i = 0U; i < 32U; ++i)
    {
        destination_words[i] = 0U;
    }

    memset(source_words, 0, sizeof(source_words));
    memset(context_words, 0, sizeof(context_words));
    source_words[0] = WATCHDOG_RECORD_MAGIC;
    source_words[1] = WATCHDOG_RECORD_VERSION;
    source_words[2] = kind;
    if (context != NULL)
    {
        uint32_t index = 0U;
        uint32_t status_index;
        uint32_t stack_index;
        uint32_t control_index;
        uint32_t source_index;

        context_words[index++] = context->return_token;
        for (status_index = 0U; status_index < 4U; ++status_index)
        {
            context_words[index++] = context->status[status_index];
        }
        context_words[index++] = context->frame_address;
        context_words[index++] = context->basic_frame_address;
        for (stack_index = 0U; stack_index < 2U; ++stack_index)
        {
            context_words[index++] = context->stack_pointer[stack_index];
        }
        context_words[index++] = context->source_id;
        context_words[index++] = context->tick;
        context_words[index++] = context->cycle_counter;
        for (control_index = 0U; control_index < 3U; ++control_index)
        {
            context_words[index++] = context->control[control_index];
        }
        for (source_index = 0U; source_index < 5U; ++source_index)
        {
            context_words[index++] = context->source_location[source_index];
        }
        for (i = 0U; i < 8U; ++i)
        {
            source_words[23U + i] = context->frame[i];
        }
    }
    for (i = 0U; i < 20U; ++i)
    {
        source_words[3U + i] = context_words[i];
    }
    for (i = 0U; i < 31U; ++i)
    {
        destination_words[i] = source_words[i];
    }
    crc = watchdog_crc32_volatile_words(destination_words, 31U);
    destination_words[31] = crc;
    watchdog_memory_barrier();
    destination->commit = WATCHDOG_FAULT_COMMIT;
    watchdog_memory_barrier();

    {
        watchdog_record_header_t header;
        if (watchdog_record_active_header(record, &header, NULL))
        {
            /* Make the newest committed fault attributable to this boot even
             * if normal-operation breadcrumbs had already staged the slot. */
            record->footer.stage_sequence = header.boot_sequence;
            record->footer.stage_commit = WATCHDOG_STAGE_COMMIT;
        }
    }
    watchdog_memory_barrier();
}

int watchdog_record_mark_fault_reset_requested(
    volatile watchdog_retained_record_t *record)
{
    watchdog_record_header_t header;
    if (record == NULL || !watchdog_fault_valid(&record->fault) ||
        !watchdog_record_active_header(record, &header, NULL) ||
        record->footer.magic != WATCHDOG_RECORD_MAGIC ||
        record->footer.version != WATCHDOG_RECORD_VERSION ||
        record->footer.stage_sequence != header.boot_sequence ||
        record->footer.stage_commit != WATCHDOG_STAGE_COMMIT)
    {
        return 0;
    }
    record->footer.reserved[1] = WATCHDOG_FAULT_RESET_REQUESTED;
    watchdog_memory_barrier();
    return record->footer.reserved[1] == WATCHDOG_FAULT_RESET_REQUESTED;
}

void watchdog_record_stage_commit(volatile watchdog_retained_record_t *record,
                                  uint32_t stage_sequence)
{
    if (record == NULL)
    {
        return;
    }
    record->footer.magic = WATCHDOG_RECORD_MAGIC;
    record->footer.version = WATCHDOG_RECORD_VERSION;
    record->footer.stage_sequence = stage_sequence;
    watchdog_memory_barrier();
    record->footer.stage_commit = WATCHDOG_STAGE_COMMIT;
    watchdog_memory_barrier();
}

void watchdog_record_mark_flash_committed(volatile watchdog_retained_record_t *record,
                                          uint32_t flash_sequence)
{
    size_t i;
    if (record == NULL)
    {
        return;
    }
    record->footer.flash_sequence = flash_sequence;
    record->footer.stage_commit = 0U;
    watchdog_record_clear_fault(record);
    watchdog_memory_barrier();
    record->footer.reserved[0] = 0U;
    record->footer.reserved[1] = 0U;
    for (i = 15U; i <= 55U; ++i)
    {
        record->footer.reserved[i] = 0U;
    }
    watchdog_memory_barrier();
    watchdog_record_incident_id_clear(record);
}

void watchdog_record_mark_flash_image(volatile watchdog_retained_record_t *record,
                                      uint32_t flash_sequence)
{
    if (record == NULL)
    {
        return;
    }
    record->footer.magic = WATCHDOG_RECORD_MAGIC;
    record->footer.version = WATCHDOG_RECORD_VERSION;
    record->footer.flash_sequence = flash_sequence;
    record->footer.flash_commit = 0xFFFFFFFFUL;
    record->footer.flash_crc32 = 0U;
    record->footer.flash_crc32 = watchdog_flash_record_crc32(record);
    watchdog_memory_barrier();
}

void watchdog_record_clear_fault(volatile watchdog_retained_record_t *record)
{
    volatile uint32_t *words;
    size_t i;
    if (record == NULL)
    {
        return;
    }
    words = (volatile uint32_t *)&record->fault;
    words[127] = 0U;
    watchdog_memory_barrier();
    for (i = 0U; i < 127U; ++i)
    {
        words[i] = 0U;
    }
    watchdog_memory_barrier();
}

void watchdog_record_component_commit(
    volatile watchdog_retained_record_t *record, watchdog_component_id_t id,
    const watchdog_component_breadcrumb_t *breadcrumb)
{
    volatile uint32_t *destination;
    const uint32_t *source;
    uint32_t slot[WATCHDOG_COMPONENT_WORDS];
    size_t index;
    size_t base;

    if (record == NULL || breadcrumb == NULL ||
        (uint32_t)id >= WATCHDOG_COMPONENT_COUNT)
    {
        return;
    }

    base = (size_t)id * WATCHDOG_COMPONENT_WORDS;
    destination = &record->fault.reserved[base];
    destination[8] = 0U;
    watchdog_memory_barrier();

    slot[0] = breadcrumb->sequence;
    slot[1] = breadcrumb->tick;
    slot[2] = breadcrumb->thread;
    slot[3] = breadcrumb->stage;
    slot[4] = breadcrumb->progress;
    slot[5] = breadcrumb->pc;
    slot[6] = breadcrumb->sp;
    slot[7] = watchdog_crc32(slot, 7U * sizeof(uint32_t));
    slot[8] = 0U;
    source = slot;
    for (index = 0U; index < 8U; ++index)
    {
        destination[index] = source[index];
    }
    watchdog_memory_barrier();
    destination[8] = WATCHDOG_COMPONENT_COMMIT_BASE | ((uint32_t)id + 1U);
    watchdog_memory_barrier();
}

int watchdog_record_component_valid(
    const volatile watchdog_retained_record_t *record,
    watchdog_component_id_t id, watchdog_component_breadcrumb_t *breadcrumb)
{
    const volatile uint32_t *source;
    uint32_t *destination;
    size_t base;
    size_t index;

    if (record == NULL || breadcrumb == NULL ||
        (uint32_t)id >= WATCHDOG_COMPONENT_COUNT)
    {
        return 0;
    }

    base = (size_t)id * WATCHDOG_COMPONENT_WORDS;
    source = &record->fault.reserved[base];
    if (source[0] == 0U || source[8] !=
            (WATCHDOG_COMPONENT_COMMIT_BASE | ((uint32_t)id + 1U)) ||
        watchdog_crc32_volatile_words(source, 7U) != source[7])
    {
        return 0;
    }

    destination = (uint32_t *)breadcrumb;
    for (index = 0U; index < WATCHDOG_COMPONENT_WORDS; ++index)
    {
        destination[index] = source[index];
    }
    return 1;
}

void watchdog_record_context_commit(
    volatile watchdog_retained_record_t *record,
    const watchdog_runtime_context_t *context)
{
    volatile uint32_t *destination;
    watchdog_runtime_context_t snapshot;
    uint32_t *source;
    size_t index;
    const size_t base = WATCHDOG_CONTEXT_COMPONENT_WORDS;

    if (record == NULL || context == NULL)
    {
        return;
    }

    destination = &record->fault.reserved[base];
    snapshot = *context;
    snapshot.magic = WATCHDOG_CONTEXT_MAGIC;
    snapshot.commit = 0U;
    snapshot.crc32 = watchdog_crc32(
        &snapshot, offsetof(watchdog_runtime_context_t, crc32));
    source = (uint32_t *)&snapshot;
    destination[WATCHDOG_CONTEXT_WORDS - 1U] = 0U;
    watchdog_memory_barrier();
    for (index = 0U; index < WATCHDOG_CONTEXT_WORDS - 1U; ++index)
    {
        destination[index] = source[index];
    }
    watchdog_memory_barrier();
    destination[WATCHDOG_CONTEXT_WORDS - 1U] = WATCHDOG_CONTEXT_COMMIT;
    watchdog_memory_barrier();
}

int watchdog_record_context_valid(
    const volatile watchdog_retained_record_t *record,
    watchdog_runtime_context_t *context)
{
    const volatile uint32_t *source;
    uint32_t *destination;
    size_t index;
    const size_t base = WATCHDOG_CONTEXT_COMPONENT_WORDS;

    if (record == NULL || context == NULL)
    {
        return 0;
    }

    source = &record->fault.reserved[base];
    if (source[0] != WATCHDOG_CONTEXT_MAGIC ||
        source[WATCHDOG_CONTEXT_WORDS - 1U] != WATCHDOG_CONTEXT_COMMIT ||
        watchdog_crc32_volatile_words(source,
                                     WATCHDOG_CONTEXT_WORDS - 2U) !=
            source[WATCHDOG_CONTEXT_WORDS - 2U])
    {
        return 0;
    }

    destination = (uint32_t *)context;
    for (index = 0U; index < WATCHDOG_CONTEXT_WORDS; ++index)
    {
        destination[index] = source[index];
    }
    return 1;
}

void watchdog_record_frame_commit(
    volatile watchdog_retained_record_t *record,
    const watchdog_interrupted_frame_t *frame)
{
    watchdog_interrupted_frame_t snapshot;
    volatile uint32_t *destination;
    uint32_t *source;
    uint32_t slot;
    size_t index;

    if (record == NULL || frame == NULL || frame->sequence == 0U)
    {
        return;
    }
    slot = frame->sequence % WATCHDOG_CONTEXT_FRAME_COPIES;
    destination = &record->fault.reserved[
        WATCHDOG_CONTEXT_FRAME_OFFSET + slot * WATCHDOG_CONTEXT_FRAME_WORDS];
    destination[WATCHDOG_CONTEXT_FRAME_WORDS - 1U] = 0U;
    watchdog_memory_barrier();

    snapshot = *frame;
    snapshot.crc32 = watchdog_crc32(
        &snapshot, offsetof(watchdog_interrupted_frame_t, crc32));
    snapshot.commit = 0U;
    source = (uint32_t *)&snapshot;
    for (index = 0U; index < WATCHDOG_CONTEXT_FRAME_WORDS - 1U; ++index)
    {
        destination[index] = source[index];
    }
    watchdog_memory_barrier();
    destination[WATCHDOG_CONTEXT_FRAME_WORDS - 1U] =
        WATCHDOG_FRAME_COMMIT_BASE | (slot + 1U);
    watchdog_memory_barrier();
}

int watchdog_record_frame_latest(
    const volatile watchdog_retained_record_t *record,
    watchdog_interrupted_frame_t *frame)
{
    watchdog_interrupted_frame_t candidate;
    watchdog_interrupted_frame_t latest;
    uint32_t valid_count = 0U;
    uint32_t slot;
    uint32_t *destination;
    const volatile uint32_t *source;
    size_t index;

    if (record == NULL || frame == NULL)
    {
        return 0;
    }
    for (slot = 0U; slot < WATCHDOG_CONTEXT_FRAME_COPIES; ++slot)
    {
        source = &record->fault.reserved[
            WATCHDOG_CONTEXT_FRAME_OFFSET +
            slot * WATCHDOG_CONTEXT_FRAME_WORDS];
        if (source[0] == 0U ||
            source[WATCHDOG_CONTEXT_FRAME_WORDS - 1U] !=
                (WATCHDOG_FRAME_COMMIT_BASE | (slot + 1U)) ||
            watchdog_crc32_volatile_words(
                source, WATCHDOG_CONTEXT_FRAME_WORDS - 2U) !=
                source[WATCHDOG_CONTEXT_FRAME_WORDS - 2U])
        {
            continue;
        }
        destination = (uint32_t *)&candidate;
        for (index = 0U; index < WATCHDOG_CONTEXT_FRAME_WORDS; ++index)
        {
            destination[index] = source[index];
        }
        if (valid_count == 0U ||
            (int32_t)(candidate.sequence - latest.sequence) > 0)
        {
            latest = candidate;
        }
        valid_count++;
    }
    if (valid_count == 0U)
    {
        return 0;
    }
    *frame = latest;
    return 1;
}

void watchdog_record_clear_diagnostics(
    volatile watchdog_retained_record_t *record)
{
    volatile uint32_t *words;
    size_t index;

    if (record == NULL)
    {
        return;
    }
    words = (volatile uint32_t *)record->fault.reserved;
    for (index = 0U; index < 95U; ++index)
    {
        words[index] = 0U;
    }
    watchdog_memory_barrier();
}

void watchdog_record_clear_recovery(volatile watchdog_retained_record_t *record)
{
    watchdog_record_header_t header;
    unsigned active_slot;
    if (!watchdog_record_active_header(record, &header, &active_slot))
    {
        return;
    }
    header.reset_count = 0U;
    header.reset_window_second = 0U;
    header.reset_window_valid = 0U;
    watchdog_record_header_commit(record, &header, active_slot);
    record->footer.recovery_clear_magic = WATCHDOG_RECOVERY_CLEAR_MAGIC;
    watchdog_memory_barrier();
}

