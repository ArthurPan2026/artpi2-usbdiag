/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ARTPI2_WATCHDOG_RECORD_H
#define ARTPI2_WATCHDOG_RECORD_H

#include <stddef.h>
#include <stdint.h>

/* V2 wire image size. Storage addresses and erase geometry belong to ports. */
#define WATCHDOG_RECORD_SIZE             0x00001000UL

#define WATCHDOG_RECORD_MAGIC            0x57444752UL /* WDGR */
#define WATCHDOG_RECORD_VERSION          2U
#define WATCHDOG_HEADER_COMMIT           0x48445231UL /* HDR1 */
#define WATCHDOG_EVENT_COMMIT            0x45564E31UL /* EVN1 */
#define WATCHDOG_FAULT_COMMIT            0x464C5431UL /* FLT1 */
#define WATCHDOG_STAGE_COMMIT            0x53544731UL /* STG1 */
#define WATCHDOG_FLASH_COMMIT            0x464C5331UL /* FLS1 */
#define WATCHDOG_RECOVERY_CLEAR_MAGIC    0x434C5231UL /* CLR1 */
#define WATCHDOG_PENDING_FLASH_MAGIC     0x504E4431UL /* PND1 */
#define WATCHDOG_TRACE_PENDING_MAGIC     0x54525031UL /* TRP1 */
#define WATCHDOG_TRACE_PENDING_FLAGS_WORD 9U
#define WATCHDOG_TRACE_PENDING_WORD       10U
/* Retained-header-only marker: this reset followed a committed fault record
 * and was requested in software. It is not an RCC reset-source bit. */
#define WATCHDOG_RESET_FLAG_CAPTURED_FAULT 0x80000000UL
#define WATCHDOG_FAULT_RESET_REQUESTED    0x46525331UL /* FRS1 */

#define WATCHDOG_EVENT_CAPACITY          96U
#define WATCHDOG_COMPONENT_COUNT         5U
#define WATCHDOG_COMPONENT_WORDS         9U
#define WATCHDOG_CONTEXT_WORDS           32U
#define WATCHDOG_CONTEXT_COMPONENT_WORDS \
    (WATCHDOG_COMPONENT_COUNT * WATCHDOG_COMPONENT_WORDS)
#define WATCHDOG_CONTEXT_FRAME_WORDS     9U
#define WATCHDOG_CONTEXT_FRAME_COPIES    2U
#define WATCHDOG_CONTEXT_FRAME_OFFSET \
    (WATCHDOG_CONTEXT_COMPONENT_WORDS + WATCHDOG_CONTEXT_WORDS)
#define WATCHDOG_COMPONENT_COMMIT_BASE   0x434D0000UL /* CM */
#define WATCHDOG_CONTEXT_MAGIC           0x43545831UL /* CTX1 */
#define WATCHDOG_CONTEXT_COMMIT           0x43545843UL /* CTXC */
#define WATCHDOG_FRAME_COMMIT_BASE       0x46520000UL /* FR */

/* Reset classes normalized by a platform adapter; raw source bits remain
 * opaque in the persisted V2 header for diagnostics. */
#define WATCHDOG_RESET_CAUSE_WATCHDOG    (1UL << 0)
#define WATCHDOG_RESET_CAUSE_SOFTWARE    (1UL << 1)
#define WATCHDOG_RESET_CAUSE_POWER_ON    (1UL << 2)
#define WATCHDOG_RESET_CAUSE_OTHER       (1UL << 3)

typedef enum watchdog_event_code
{
    WATCHDOG_EVENT_BOOT = 1U,
    WATCHDOG_EVENT_RESET_CAUSE,
    WATCHDOG_EVENT_CORE_READY,
    WATCHDOG_EVENT_CORE_PROGRESS,
    WATCHDOG_EVENT_OPERATION_BEGIN,
    WATCHDOG_EVENT_OPERATION_PROGRESS,
    WATCHDOG_EVENT_OPERATION_END,
    WATCHDOG_EVENT_HEALTH_STALE,
    WATCHDOG_EVENT_FAULT,
    WATCHDOG_EVENT_FLASH_COMMIT,
    WATCHDOG_EVENT_FLASH_FAILURE,
    WATCHDOG_EVENT_RECOVERY,
    WATCHDOG_EVENT_BOOT_FAILURE,
    WATCHDOG_EVENT_CONTEXT_SAMPLE,
    WATCHDOG_EVENT_HEALTH_REJECTED,
    WATCHDOG_EVENT_FEED_BEGIN,
    WATCHDOG_EVENT_FEED_RETURNED,
    WATCHDOG_EVENT_FEED_SUPPRESSED
} watchdog_event_code_t;

typedef enum watchdog_component_id
{
    WATCHDOG_COMPONENT_LVGL = 0U,
    WATCHDOG_COMPONENT_MONGOOSE,
    WATCHDOG_COMPONENT_WIFI_WORKER,
    WATCHDOG_COMPONENT_USB_ANALYZER,
    WATCHDOG_COMPONENT_PDF_WORKER
} watchdog_component_id_t;

typedef enum watchdog_component_stage
{
    WATCHDOG_COMPONENT_STAGE_STARTING = 1U,
    WATCHDOG_COMPONENT_STAGE_ACTIVE,
    WATCHDOG_COMPONENT_STAGE_IDLE,
    WATCHDOG_COMPONENT_STAGE_ERROR,
    WATCHDOG_COMPONENT_STAGE_STOPPED
} watchdog_component_stage_t;

/* A component checkpoint lives in fault.reserved[] and remains independent
 * from the architecture fault frame. The fixed slot is selected by ID. */
typedef struct watchdog_component_breadcrumb
{
    uint32_t sequence;
    uint32_t tick;
    uint32_t thread;
    uint32_t stage;
    uint32_t progress;
    uint32_t pc;
    uint32_t sp;
    uint32_t crc32;
    uint32_t commit;
} watchdog_component_breadcrumb_t;

/* Fixed 32-word most-recent CPU/peripheral snapshot. A rolling event
 * sample separately preserves the preceding PCs and stack pointers. */
typedef struct watchdog_runtime_context
{
    uint32_t magic;
    uint32_t sequence;
    uint32_t tick;
    uint32_t cycles;
    uint32_t thread;
    uint32_t thread_name[4];
    uint32_t pc;
    uint32_t lr;
    uint32_t psp;
    uint32_t msp;
    uint32_t exc_return;
    uint32_t ipsr;
    uint32_t primask;
    uint32_t basepri;
    uint32_t faultmask;
    uint32_t control;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t shcsr;
    uint32_t health_flags;
    uint32_t ltdc_frames;
    uint32_t ltdc_reloads;
    uint32_t gpu2d_errors;
    uint32_t dma2d_errors;
    uint32_t thread_state;
    uint32_t crc32;
    uint32_t commit;
} watchdog_runtime_context_t;

/* Cortex-M hardware-stacked registers that do not fit into the concise event
 * record. Two rotating slots preserve the preceding valid frame if a commit
 * is interrupted. PC/LR and the stack pointers live in runtime_context. */
typedef struct watchdog_interrupted_frame
{
    uint32_t sequence;
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t xpsr;
    uint32_t crc32;
    uint32_t commit;
} watchdog_interrupted_frame_t;

typedef struct watchdog_record_header
{
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t boot_sequence;
    uint32_t reset_flags; /* Opaque platform reset-source bits. */
    uint32_t reset_count;
    uint32_t reset_window_second;
    uint32_t reset_window_valid;
    uint32_t event_next_sequence;
    uint32_t event_count;
    uint32_t core_ready;
    uint32_t last_core_tick;
    uint32_t last_core_progress;
    uint32_t last_core_pc;
    uint32_t last_core_sp;
    uint32_t last_core_thread;
    uint32_t active_operation;
    uint32_t active_stage;
    uint32_t active_started_tick;
    uint32_t active_timeout_ms;
    uint32_t active_progress;
    uint32_t supervisor_tick;
    uint32_t health_flags;
    uint32_t source_fingerprint_crc32;
    char source_fingerprint_prefix[24];
    uint32_t crc32;
    uint32_t commit;
} watchdog_record_header_t;

typedef struct watchdog_event_record
{
    uint32_t sequence;
    uint32_t tick;
    uint32_t code;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t pc;
    uint32_t sp;
    uint32_t crc32;
} watchdog_event_record_t;

typedef struct watchdog_fault_record
{
    uint32_t magic;
    uint32_t version;
    uint32_t kind;
    uint32_t platform_context[20];
    uint32_t frame[8];
    uint32_t crc32;
    uint32_t reserved[95];
    uint32_t commit;
} watchdog_fault_record_t;

/* The V2 payload keeps a fixed 20-word platform context and an 8-word frame.
 * Meaning is supplied by a CPU/BSP port; the generic core stores and checks it. */
typedef struct watchdog_fault_context
{
    uint32_t return_token;
    uint32_t status[4];
    uint32_t frame_address;
    uint32_t basic_frame_address;
    uint32_t stack_pointer[2];
    uint32_t source_id;
    uint32_t tick;
    uint32_t cycle_counter;
    uint32_t control[3];
    uint32_t source_location[5];
    uint32_t frame[8];
} watchdog_fault_context_t;

typedef struct watchdog_record_boot_context
{
    uint32_t raw_reset_flags;
    uint32_t reset_causes;
    uint32_t previous_reset_causes;
    uint32_t rtc_second;
    uint32_t rtc_valid;
    uint32_t tick;
} watchdog_record_boot_context_t;

typedef struct watchdog_record_policy
{
    uint32_t reset_window_seconds;
} watchdog_record_policy_t;

typedef struct watchdog_record_footer
{
    uint32_t magic;
    uint32_t version;
    uint32_t stage_sequence;
    uint32_t stage_commit;
    uint32_t flash_sequence;
    uint32_t flash_crc32;
    uint32_t recovery_clear_magic;
    uint32_t reserved[56];
    uint32_t flash_commit;
} watchdog_record_footer_t;

typedef struct watchdog_retained_record
{
    watchdog_record_header_t headers[2];
    watchdog_fault_record_t fault;
    watchdog_event_record_t events[WATCHDOG_EVENT_CAPACITY];
    watchdog_record_footer_t footer;
} watchdog_retained_record_t;

typedef char watchdog_header_size_must_be_128[(sizeof(watchdog_record_header_t) == 128U) ? 1 : -1];
typedef char watchdog_event_size_must_be_32[(sizeof(watchdog_event_record_t) == 32U) ? 1 : -1];
typedef char watchdog_fault_size_must_be_512[(sizeof(watchdog_fault_record_t) == 512U) ? 1 : -1];
typedef char watchdog_footer_size_must_be_256[(sizeof(watchdog_record_footer_t) == 256U) ? 1 : -1];
typedef char watchdog_record_size_must_be_4096[(sizeof(watchdog_retained_record_t) == WATCHDOG_RECORD_SIZE) ? 1 : -1];
typedef char watchdog_component_size_must_be_36[(sizeof(watchdog_component_breadcrumb_t) == 36U) ? 1 : -1];
typedef char watchdog_context_size_must_be_128[(sizeof(watchdog_runtime_context_t) == 128U) ? 1 : -1];
typedef char watchdog_frame_size_must_be_36[(sizeof(watchdog_interrupted_frame_t) == 36U) ? 1 : -1];
typedef char watchdog_reserved_sidecar_fits[(WATCHDOG_CONTEXT_FRAME_OFFSET + (WATCHDOG_CONTEXT_FRAME_WORDS * WATCHDOG_CONTEXT_FRAME_COPIES) <= 95U) ? 1 : -1];
typedef char watchdog_header1_offset_must_be_128[(offsetof(watchdog_retained_record_t, headers[1]) == 0x0080U) ? 1 : -1];
typedef char watchdog_fault_offset_must_be_256[(offsetof(watchdog_retained_record_t, fault) == 0x0100U) ? 1 : -1];
typedef char watchdog_events_offset_must_be_768[(offsetof(watchdog_retained_record_t, events) == 0x0300U) ? 1 : -1];
typedef char watchdog_footer_offset_must_be_3840[(offsetof(watchdog_retained_record_t, footer) == 0x0F00U) ? 1 : -1];

uint32_t watchdog_crc32(const void *data, size_t length);
uint32_t watchdog_crc32_volatile_words(const volatile uint32_t *words,
                                       size_t word_count);
int watchdog_header_valid(const volatile watchdog_record_header_t *header);
int watchdog_event_valid(const volatile watchdog_event_record_t *event);
int watchdog_fault_valid(const volatile watchdog_fault_record_t *fault);
int watchdog_flash_record_valid(const volatile watchdog_retained_record_t *record);
uint32_t watchdog_flash_record_crc32(const volatile watchdog_retained_record_t *record);
void watchdog_record_boot_begin(volatile watchdog_retained_record_t *record,
                                const char *source_fingerprint,
                                const watchdog_record_boot_context_t *context,
                                const watchdog_record_policy_t *policy,
                                uint32_t *reset_count);
void watchdog_record_set_build_fingerprint(volatile watchdog_retained_record_t *record,
                                           const char *source_fingerprint);
int watchdog_record_active_header(const volatile watchdog_retained_record_t *record,
                                  watchdog_record_header_t *header,
                                  unsigned *slot);
void watchdog_record_header_commit(volatile watchdog_retained_record_t *record,
                                   watchdog_record_header_t *header,
                                   unsigned active_slot);
void watchdog_record_initialize(volatile watchdog_retained_record_t *record,
                                const char *source_fingerprint,
                                uint32_t reset_flags);
void watchdog_record_append_event(volatile watchdog_retained_record_t *record,
                                  uint32_t tick, uint32_t code,
                                  uint32_t arg0, uint32_t arg1,
                                  uint32_t pc, uint32_t sp);
void watchdog_record_fault(volatile watchdog_retained_record_t *record,
                           uint32_t kind,
                           const watchdog_fault_context_t *context);
int watchdog_record_mark_fault_reset_requested(
    volatile watchdog_retained_record_t *record);
void watchdog_record_stage_commit(volatile watchdog_retained_record_t *record,
                                  uint32_t stage_sequence);
void watchdog_record_mark_flash_committed(volatile watchdog_retained_record_t *record,
                                          uint32_t flash_sequence);
void watchdog_record_mark_flash_image(volatile watchdog_retained_record_t *record,
                                      uint32_t flash_sequence);
void watchdog_record_clear_fault(volatile watchdog_retained_record_t *record);
void watchdog_record_clear_recovery(volatile watchdog_retained_record_t *record);
void watchdog_record_component_commit(
    volatile watchdog_retained_record_t *record, watchdog_component_id_t id,
    const watchdog_component_breadcrumb_t *breadcrumb);
int watchdog_record_component_valid(
    const volatile watchdog_retained_record_t *record,
    watchdog_component_id_t id, watchdog_component_breadcrumb_t *breadcrumb);
void watchdog_record_context_commit(
    volatile watchdog_retained_record_t *record,
    const watchdog_runtime_context_t *context);
int watchdog_record_context_valid(
    const volatile watchdog_retained_record_t *record,
    watchdog_runtime_context_t *context);
void watchdog_record_frame_commit(
    volatile watchdog_retained_record_t *record,
    const watchdog_interrupted_frame_t *frame);
int watchdog_record_frame_latest(
    const volatile watchdog_retained_record_t *record,
    watchdog_interrupted_frame_t *frame);
void watchdog_record_clear_diagnostics(
    volatile watchdog_retained_record_t *record);

#endif
