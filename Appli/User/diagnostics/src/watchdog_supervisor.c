#include "watchdog_supervisor.h"

#include <stdint.h>
#include <string.h>
#include <rtthread.h>
#include <rthw.h>
#include <rtsched.h>
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#include <finsh.h>
#endif

#include "build_identity.h"
#include "stm32h7rsxx_hal.h"
#include "watchdog_h7r7_port.h"
#include "watchdog_record.h"
#include "watchdog_rtt_supervisor.h"
#include "watchdog_trace.h"
#include "ltdc_irq_adapter.h"
#include "gpu2d_irq_adapter.h"
#include "dma2d_service.h"

#define WATCHDOG_THREAD_PRIORITY             3U
#define WATCHDOG_THREAD_STACK_SIZE           1024U
#define WATCHDOG_THREAD_TIMESLICE            5U
#define WATCHDOG_POLL_MS                     100U
#define WATCHDOG_CORE_PROGRESS_MAX_AGE_MS    5000U
#define WATCHDOG_COMPONENT_MAX_AGE_MS        5000U
#define WATCHDOG_CONTEXT_SAMPLE_PERIOD_MS    100U
#define WATCHDOG_EVENT_SAMPLE_PERIOD_MS     1000U
#define WATCHDOG_LTDC_MAX_AGE_MS             1200U
#define WATCHDOG_STABLE_CLEAR_MS             300000U
#define WATCHDOG_OPERATION_MAX_TIMEOUT_MS    120000U
#define WATCHDOG_BKPSRAM_READY_SPIN_LIMIT     2000000U
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#define WATCHDOG_FEED_EVENT_INTERVAL_POLLS    1U
#else
#define WATCHDOG_FEED_EVENT_INTERVAL_POLLS    20U
#endif

#define WATCHDOG_FIRST_FAILURE_BASE_WORD       15U
#define WATCHDOG_FIRST_FAILURE_WORDS           41U
#define WATCHDOG_FIRST_FAILURE_MAGIC           0x46465331UL /* FFS1 */
#define WATCHDOG_FIRST_FAILURE_COMMIT          0x46464331UL /* FFC1 */
#define WATCHDOG_PANIC_STACK_MAGIC             0x504E5331UL /* PNS1 */
#define WATCHDOG_PANIC_STACK_COMMIT            0x504E4331UL /* PNC1 */

#define WATCHDOG_HEALTH_TICK_MOVING          (1UL << 0)
#define WATCHDOG_HEALTH_CORE_PROGRESS        (1UL << 1)
#define WATCHDOG_HEALTH_OPERATION_IDLE       (1UL << 2)
#define WATCHDOG_HEALTH_DISPLAY_FRAMES       (1UL << 3)
#define WATCHDOG_CONTEXT_PC_VALID            (1UL << 24)
#define WATCHDOG_CONTEXT_STACK_FROM_PSP      (1UL << 25)
#define WATCHDOG_CONTEXT_EXTENDED_FP_FRAME   (1UL << 26)
#define WATCHDOG_CONTEXT_FRAME_VALID         (1UL << 27)
#define WATCHDOG_CONTEXT_SUPERVISOR_STALE    (1UL << 28)
#define WATCHDOG_CONTEXT_MMFAR_VALID         (1UL << 29)
#define WATCHDOG_CONTEXT_BFAR_VALID          (1UL << 30)
#define WATCHDOG_CONTEXT_FAULT_REGS_VALID     (1UL << 31)

#define WATCHDOG_HEALTH_REQUIRED_COMPONENTS  (1UL << 4)
#define WATCHDOG_HEALTH_FAILURE_LATCHED      (1UL << 5)

enum
{
    WATCHDOG_FAILURE_OPERATION_TIMEOUT = 1U,
    WATCHDOG_FAILURE_INIT_TIMEOUT,
    WATCHDOG_FAILURE_CORE_PROGRESS_STALE,
    WATCHDOG_FAILURE_REQUIRED_COMPONENT_STALE,
    WATCHDOG_FAILURE_DISPLAY_FRAMES_STALE,
    WATCHDOG_FAILURE_SYNCHRONOUS_FAULT,
    WATCHDOG_FAILURE_ASSERTION
};

enum
{
    WATCHDOG_REJECT_NONE = 0U,
    WATCHDOG_REJECT_HEADER_INVALID,
    WATCHDOG_REJECT_OPERATION_TIMEOUT,
    WATCHDOG_REJECT_INITIALIZATION_TIMEOUT,
    WATCHDOG_REJECT_CORE_PROGRESS_STALE,
    WATCHDOG_REJECT_REQUIRED_COMPONENT_STALE,
    WATCHDOG_REJECT_DISPLAY_FRAMES_STALE,
    WATCHDOG_REJECT_FATAL_FAULT,
    WATCHDOG_REJECT_OTHER
};

/* Match the application linker limit exactly; the remaining XIP tail holds
 * the Boot-owned trace sidecar followed by the watchdog journal. */
#define WATCHDOG_APP_XIP_BEGIN               0x70000000UL
#define WATCHDOG_APP_XIP_END                 0x73FAF000UL
#define WATCHDOG_INTERNAL_FLASH_BEGIN        0x08000000UL
#define WATCHDOG_INTERNAL_FLASH_END          0x08200000UL
#define WATCHDOG_AXI_RAM_BEGIN               0x24000000UL
#define WATCHDOG_AXI_RAM_END                 0x24080000UL
#define WATCHDOG_AXI_FAULT_FRAME_END         0x24072000UL
#define WATCHDOG_DTCM_BEGIN                  0x20000000UL
#define WATCHDOG_DTCM_END                    0x20010000UL
#define WATCHDOG_TRACE_AXI_BEGIN             0x24000000UL
#define WATCHDOG_TRACE_AXI_END               0x24072000UL
#define WATCHDOG_TRACE_PSRAM_BEGIN           0x90000000UL
#define WATCHDOG_TRACE_PSRAM_END             0x92000000UL
#define WATCHDOG_TRACE_PC_VALID              (1UL << 31)
#define WATCHDOG_TRACE_STACK_FROM_PSP         (1UL << 30)
#define WATCHDOG_TRACE_EXTENDED_FRAME         (1UL << 29)
#define WATCHDOG_TRACE_XPSR_T                 (1UL << 24)
#define WATCHDOG_TRACE_FP_FRAME_WORDS         18U

/* Boot reads this fixed NOLOAD ring before it initializes the application.
 * 912 x 24-byte samples plus the 32-byte header fit at 0x2406C9A0. */
__attribute__((section(".watchdog_trace"), aligned(32), used))
static volatile watchdog_trace_ring_t s_watchdog_trace_ring;
static volatile uint32_t s_watchdog_trace_sequence;
static volatile uint32_t s_watchdog_trace_enabled;

#if defined(__GNUC__)
#define WATCHDOG_NOINLINE __attribute__((noinline))
#define WATCHDOG_CALLER_PC() \
    ((uint32_t)(uintptr_t)__builtin_return_address(0))
#else
#define WATCHDOG_NOINLINE
#define WATCHDOG_CALLER_PC() 0U
#endif

typedef struct watchdog_component_live
{
    volatile uint32_t tick;
    volatile uint32_t thread;
    volatile uint32_t stage;
    volatile uint32_t progress;
    volatile uint32_t pc;
    volatile uint32_t sp;
} watchdog_component_live_t;

/* Frozen at the first failed health observation. The snapshot is kept apart
 * from the rolling context so the later IWDG wait cannot replace the cause. */
typedef struct watchdog_first_failure_snapshot
{
    uint32_t magic;
    uint32_t sequence;
    uint32_t tick;
    uint32_t reason;
    uint32_t subject;
    uint32_t age_ms;
    uint32_t context_sequence;
    uint32_t context_tick;
    uint32_t context_cycles;
    uint32_t context_thread;
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
    uint32_t gpu2d_errors;
    uint32_t dma2d_errors;
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t xpsr;
    uint32_t crc32;
    uint32_t commit;
} watchdog_first_failure_snapshot_t;

typedef char watchdog_first_failure_snapshot_size_must_be_164[
    (sizeof(watchdog_first_failure_snapshot_t) ==
     WATCHDOG_FIRST_FAILURE_WORDS * sizeof(uint32_t)) ? 1 : -1];

static watchdog_component_live_t s_component_live[WATCHDOG_COMPONENT_COUNT];
static volatile uint32_t s_component_expected_mask;
static volatile uint32_t s_component_required_mask;
static volatile uint32_t s_component_checkpoint_sequence[WATCHDOG_COMPONENT_COUNT];
static volatile uint32_t s_stale_component_mask;
static volatile uint32_t s_capture_enabled;
static volatile uint32_t s_display_expected;
static volatile uint32_t s_context_sequence;
static volatile uint32_t s_last_context_sample_tick;
static volatile uint32_t s_last_event_sample_tick;
static volatile uint32_t s_last_ltdc_frames;
static volatile uint32_t s_last_ltdc_progress_tick;
static volatile uint32_t s_systick_ltdc_frames;
static volatile uint32_t s_systick_ltdc_progress_tick;
static volatile uint32_t s_capture_last_cycles;
static volatile uint32_t s_capture_max_cycles;
static volatile uint32_t s_feed_event_enabled;
static volatile uint32_t s_supervisor_wake_sequence;
static volatile uint32_t s_feed_attempt_sequence;
static volatile uint32_t s_feed_return_sequence;
static volatile uint32_t s_health_reject_reason;
static volatile uint32_t s_health_reject_logged;

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#define WATCHDOG_TEST_NOFEED                 1U
#define WATCHDOG_TEST_SUPERVISOR_SPIN        2U
#define WATCHDOG_TEST_CORE_STALL             3U
#define WATCHDOG_TEST_ASSERT                 6U
#define WATCHDOG_TEST_ERROR_HANDLER          7U
#define WATCHDOG_TEST_HAL_ASSERT             8U
#define WATCHDOG_TEST_STACK_OVERFLOW         9U
#define WATCHDOG_TEST_HARDFAULT              10U
#define WATCHDOG_TEST_UNEXPECTED_IRQ         11U
#define WATCHDOG_TEST_COMPONENT_STALE        12U
extern void Error_Handler(void);
static volatile uint32_t s_test_mode;
static volatile uint32_t s_test_component_mask;
#endif

__attribute__((section(".watchdog_bkpsram"), aligned(32), used))
volatile watchdog_retained_record_t g_watchdog_retained_record;

static volatile uint32_t s_early_init_done;
static volatile rt_tick_t s_supervisor_start_tick;
static volatile rt_tick_t s_stable_start_tick;
static volatile uint32_t s_stale_logged;
static volatile uint32_t s_fatal_fault_latched;
static volatile uint32_t s_last_health_flags;

/* BKPSRAM record operations update two-copy headers and event slots as one
 * transaction. Serialize the app and supervisor writers with a short IRQ
 * critical section; every operation is bounded SRAM-only work. */
static rt_base_t watchdog_record_lock(void)
{
    return rt_hw_interrupt_disable();
}

static void watchdog_record_unlock(rt_base_t level)
{
    rt_hw_interrupt_enable(level);
}

static rt_tick_t watchdog_ticks_from_ms(uint32_t milliseconds)
{
    uint64_t ticks = ((uint64_t)milliseconds * RT_TICK_PER_SECOND + 999U) /
                     1000U;
    if (ticks == 0U)
    {
        ticks = 1U;
    }
    return (rt_tick_t)ticks;
}

static uint32_t watchdog_now_ms(void)
{
    return (uint32_t)(((uint64_t)rt_tick_get() * 1000U) /
                      RT_TICK_PER_SECOND);
}

static uint32_t watchdog_current_sp(void)
{
    uint32_t stack_pointer;
#if defined(__arm__) || defined(__thumb__)
    __asm volatile("mov %0, sp" : "=r"(stack_pointer));
#else
    stack_pointer = 0U;
#endif
    return stack_pointer;
}

static rt_thread_t watchdog_self(void);
static int watchdog_header_snapshot(watchdog_record_header_t *header,
                                    unsigned *active_slot);

static int watchdog_pc_is_executable(uint32_t pc)
{
    uint32_t address = pc & ~1UL;
    return (address >= WATCHDOG_APP_XIP_BEGIN &&
            address < WATCHDOG_APP_XIP_END) ||
           (address >= WATCHDOG_INTERNAL_FLASH_BEGIN &&
            address < WATCHDOG_INTERNAL_FLASH_END) ||
           (address >= WATCHDOG_AXI_RAM_BEGIN &&
            address < WATCHDOG_AXI_RAM_END) ||
           (address >= WATCHDOG_DTCM_BEGIN &&
            address < WATCHDOG_DTCM_END);
}

static int watchdog_first_failure_valid(
    watchdog_first_failure_snapshot_t *snapshot)
{
    const volatile uint32_t *source =
        &g_watchdog_retained_record.footer.reserved[
            WATCHDOG_FIRST_FAILURE_BASE_WORD];
    uint32_t words[WATCHDOG_FIRST_FAILURE_WORDS];
    uint32_t index;

    for (index = 0U; index < WATCHDOG_FIRST_FAILURE_WORDS; ++index)
    {
        words[index] = source[index];
    }
    if (words[0] != WATCHDOG_FIRST_FAILURE_MAGIC ||
        words[WATCHDOG_FIRST_FAILURE_WORDS - 1U] !=
            WATCHDOG_FIRST_FAILURE_COMMIT ||
        watchdog_crc32(words,
                       offsetof(watchdog_first_failure_snapshot_t, crc32)) !=
            words[WATCHDOG_FIRST_FAILURE_WORDS - 2U])
    {
        return 0;
    }
    if (snapshot != RT_NULL)
    {
        uint32_t *destination = (uint32_t *)snapshot;
        for (index = 0U; index < WATCHDOG_FIRST_FAILURE_WORDS; ++index)
        {
            destination[index] = words[index];
        }
    }
    return 1;
}

static void watchdog_first_failure_clear(void)
{
    volatile uint32_t *destination =
        &g_watchdog_retained_record.footer.reserved[
            WATCHDOG_FIRST_FAILURE_BASE_WORD];
    uint32_t index;

    destination[WATCHDOG_FIRST_FAILURE_WORDS - 1U] = 0U;
    __DMB();
    for (index = 0U; index < WATCHDOG_FIRST_FAILURE_WORDS; ++index)
    {
        destination[index] = 0U;
    }
    __DSB();
}

static int watchdog_first_failure_commit(
    uint32_t reason, uint32_t subject, uint32_t age_ms, uint32_t tick,
    const watchdog_runtime_context_t *context_arg,
    const watchdog_interrupted_frame_t *frame_arg)
{
    watchdog_first_failure_snapshot_t snapshot;
    watchdog_runtime_context_t context;
    watchdog_interrupted_frame_t frame;
    const watchdog_runtime_context_t *context_source = context_arg;
    const watchdog_interrupted_frame_t *frame_source = frame_arg;
    volatile uint32_t *destination =
        &g_watchdog_retained_record.footer.reserved[
            WATCHDOG_FIRST_FAILURE_BASE_WORD];
    uint32_t *source_words = (uint32_t *)&snapshot;
    uint32_t index;

    if (watchdog_first_failure_valid(RT_NULL))
    {
        return 0;
    }
    if (context_source == RT_NULL &&
        watchdog_record_context_valid(&g_watchdog_retained_record, &context))
    {
        context_source = &context;
    }
    if (frame_source == RT_NULL &&
        watchdog_record_frame_latest(&g_watchdog_retained_record, &frame))
    {
        frame_source = &frame;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.magic = WATCHDOG_FIRST_FAILURE_MAGIC;
    snapshot.sequence = context_source != RT_NULL
                            ? context_source->sequence : s_context_sequence;
    snapshot.tick = tick;
    snapshot.reason = reason;
    snapshot.subject = subject;
    snapshot.age_ms = age_ms;
    if (context_source != RT_NULL)
    {
        snapshot.context_sequence = context_source->sequence;
        snapshot.context_tick = context_source->tick;
        snapshot.context_cycles = context_source->cycles;
        snapshot.context_thread = context_source->thread;
        memcpy(snapshot.thread_name, context_source->thread_name,
               sizeof(snapshot.thread_name));
        snapshot.pc = context_source->pc;
        snapshot.lr = context_source->lr;
        snapshot.psp = context_source->psp;
        snapshot.msp = context_source->msp;
        snapshot.exc_return = context_source->exc_return;
        snapshot.ipsr = context_source->ipsr;
        snapshot.primask = context_source->primask;
        snapshot.basepri = context_source->basepri;
        snapshot.faultmask = context_source->faultmask;
        snapshot.control = context_source->control;
        snapshot.cfsr = context_source->cfsr;
        snapshot.hfsr = context_source->hfsr;
        snapshot.mmfar = context_source->mmfar;
        snapshot.bfar = context_source->bfar;
        snapshot.shcsr = context_source->shcsr;
        snapshot.health_flags = context_source->health_flags |
                                WATCHDOG_HEALTH_FAILURE_LATCHED;
        snapshot.ltdc_frames = context_source->ltdc_frames;
        snapshot.gpu2d_errors = context_source->gpu2d_errors;
        snapshot.dma2d_errors = context_source->dma2d_errors;
        if ((context_source->cfsr & (1UL << 7U)) != 0U)
        {
            snapshot.health_flags |= WATCHDOG_CONTEXT_MMFAR_VALID;
        }
        if ((context_source->cfsr & (1UL << 15U)) != 0U)
        {
            snapshot.health_flags |= WATCHDOG_CONTEXT_BFAR_VALID;
        }
        if (frame_source != RT_NULL &&
            frame_source->sequence == context_source->sequence)
        {
            snapshot.health_flags |= WATCHDOG_CONTEXT_FRAME_VALID;
            snapshot.r0 = frame_source->r0;
            snapshot.r1 = frame_source->r1;
            snapshot.r2 = frame_source->r2;
            snapshot.r3 = frame_source->r3;
            snapshot.r12 = frame_source->r12;
            snapshot.xpsr = frame_source->xpsr;
        }
    }

    snapshot.crc32 = watchdog_crc32(
        &snapshot, offsetof(watchdog_first_failure_snapshot_t, crc32));
    snapshot.commit = 0U;
    destination[WATCHDOG_FIRST_FAILURE_WORDS - 1U] = 0U;
    __DMB();
    for (index = 0U; index < WATCHDOG_FIRST_FAILURE_WORDS - 1U; ++index)
    {
        destination[index] = source_words[index];
    }
    __DMB();
    destination[WATCHDOG_FIRST_FAILURE_WORDS - 1U] =
        WATCHDOG_FIRST_FAILURE_COMMIT;
    __DSB();
    if (watchdog_first_failure_valid(RT_NULL))
    {
        s_last_health_flags |= WATCHDOG_HEALTH_FAILURE_LATCHED;
        return 1;
    }
    return 0;
}

static void watchdog_panic_stack_commit(uint32_t captured_sp,
                                        const uint32_t *stack,
                                        uint32_t stack_words)
{
    uint32_t words[95];
    volatile uint32_t *destination =
        (volatile uint32_t *)g_watchdog_retained_record.fault.reserved;
    uint32_t count = stack_words > 96U ? 96U : stack_words;
    uint32_t tail_count = count > 8U ? count - 8U : 0U;
    uint32_t index;

    if (stack == RT_NULL)
    {
        return;
    }
    memset(words, 0, sizeof(words));
    words[0] = WATCHDOG_PANIC_STACK_MAGIC;
    words[1] = 1U;
    words[2] = captured_sp;
    words[3] = count;
    if (tail_count > 88U)
    {
        tail_count = 88U;
    }
    for (index = 0U; index < tail_count; ++index)
    {
        words[5U + index] = stack[8U + index];
    }
    /* The fixed assertion record stores stack[0..7] in fault.frame. This
     * sidecar stores the remaining words with CRC and commit-last semantics. */
    words[4] = 0U;
    words[94] = 0U;
    words[4] = watchdog_crc32(words, 94U * sizeof(uint32_t));

    destination[94] = 0U;
    __DMB();
    for (index = 0U; index < 94U; ++index)
    {
        destination[index] = words[index];
    }
    __DMB();
    destination[94] = WATCHDOG_PANIC_STACK_COMMIT;
    __DSB();
}

static void watchdog_capture_interrupted_frame(uint32_t exc_return,
                                               uint32_t psp,
                                               rt_thread_t thread,
                                               uint32_t *pc,
                                               uint32_t *lr,
                                               uint32_t *xpsr,
                                               uint32_t *context_flags)
{
    uintptr_t stack_begin;
    uintptr_t stack_end;
    uintptr_t frame_address;
    const uint32_t *basic_frame;
    uint32_t frame_words;
    uint32_t fp_offset;

    if (pc != RT_NULL) *pc = 0U;
    if (lr != RT_NULL) *lr = 0U;
    if (xpsr != RT_NULL) *xpsr = 0U;
    if (context_flags != RT_NULL) *context_flags = 0U;

    /* Only trust a Thread-mode PSP frame. A nested handler frame lives on
     * MSP and may belong to an unrelated interrupt with different bounds. */
    if (thread == RT_NULL || (exc_return & 0xFFFFFFE0UL) != 0xFFFFFFE0UL ||
        (exc_return & (1UL << 2)) == 0U || (psp & 3U) != 0U ||
        thread->stack_addr == RT_NULL || thread->stack_size < 32U)
    {
        return;
    }

    stack_begin = (uintptr_t)thread->stack_addr;
    stack_end = stack_begin + (uintptr_t)thread->stack_size;
    if (stack_end < stack_begin)
    {
        return;
    }
    fp_offset = (exc_return & (1UL << 4)) == 0U ? 18U : 0U;
    frame_words = fp_offset != 0U ? 26U : 8U;
    frame_address = (uintptr_t)psp;
    if (frame_address < stack_begin ||
        frame_address > stack_end - frame_words * sizeof(uint32_t))
    {
        return;
    }

    basic_frame = (const uint32_t *)frame_address + fp_offset;
    if ((basic_frame[7] & (1UL << 24)) == 0U ||
        !watchdog_pc_is_executable(basic_frame[6]))
    {
        return;
    }

    if (pc != RT_NULL) *pc = basic_frame[6];
    if (lr != RT_NULL) *lr = basic_frame[5];
    if (xpsr != RT_NULL) *xpsr = basic_frame[7];
    if (context_flags != RT_NULL)
    {
        *context_flags = WATCHDOG_CONTEXT_PC_VALID |
                         WATCHDOG_CONTEXT_STACK_FROM_PSP |
                         (fp_offset != 0U
                              ? WATCHDOG_CONTEXT_EXTENDED_FP_FRAME : 0U);
    }
}

void watchdog_supervisor_component_expect(watchdog_component_id_t component,
                                          int required)
{
    uint32_t mask;
    rt_base_t irq_level;
    uint32_t progress = 0U;
    uint32_t index;
    const char *name;

    if ((uint32_t)component >= WATCHDOG_COMPONENT_COUNT)
    {
        return;
    }
    mask = 1UL << (uint32_t)component;
    irq_level = watchdog_record_lock();
    s_component_expected_mask |= mask;
    if (required != 0)
    {
        s_component_required_mask |= mask;
    }
    else
    {
        s_component_required_mask &= ~mask;
    }
    watchdog_record_unlock(irq_level);

    switch (component)
    {
    case WATCHDOG_COMPONENT_LVGL: name = "lvgl"; break;
    case WATCHDOG_COMPONENT_MONGOOSE: name = "mongoose"; break;
    case WATCHDOG_COMPONENT_WIFI_WORKER: name = "wifi_worker"; break;
    case WATCHDOG_COMPONENT_USB_ANALYZER: name = "usb_diag"; break;
    case WATCHDOG_COMPONENT_PDF_WORKER: name = "pdf"; break;
    default: name = "unknown"; break;
    }
    for (index = 0U; index < RT_NAME_MAX && name[index] != '\0'; ++index)
    {
        progress = (progress << 5U) ^ (uint8_t)name[index];
    }
    watchdog_supervisor_component_progress(
        component, WATCHDOG_COMPONENT_STAGE_STARTING, progress);
}

WATCHDOG_NOINLINE void watchdog_supervisor_component_progress(
    watchdog_component_id_t component, uint32_t stage, uint32_t progress)
{
    watchdog_component_live_t *live;
    rt_thread_t thread;
    rt_base_t irq_level;
    if ((uint32_t)component >= WATCHDOG_COMPONENT_COUNT)
    {
        return;
    }

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (s_test_mode == WATCHDOG_TEST_COMPONENT_STALE &&
        (s_test_component_mask & (1UL << (uint32_t)component)) != 0U)
    {
        return;
    }
#endif

    thread = watchdog_self();
    irq_level = watchdog_record_lock();
    live = &s_component_live[(uint32_t)component];
    live->tick = (uint32_t)rt_tick_get();
    live->thread = (uint32_t)(uintptr_t)thread;
    live->stage = stage & 0xFFU;
    if (thread != RT_NULL)
    {
        live->stage |= ((uint32_t)thread->sched_thread_ctx.stat & 0xFFU) << 8U;
        live->stage |= ((uint32_t)RT_SCHED_PRIV(thread).current_priority &
                        0xFFU) << 16U;
        live->stage |= ((uint32_t)RT_SCHED_PRIV(thread).init_priority &
                        0xFFU) << 24U;
    }
    live->progress = progress;
    live->pc = WATCHDOG_CALLER_PC();
    live->sp = watchdog_current_sp();
    watchdog_record_unlock(irq_level);
}

void watchdog_supervisor_display_expect(void)
{
    rt_base_t irq_level = watchdog_record_lock();
    s_display_expected = 1U;
    s_last_ltdc_frames = ltdc_frame_count();
    s_last_ltdc_progress_tick = (uint32_t)rt_tick_get();
    s_systick_ltdc_frames = s_last_ltdc_frames;
    s_systick_ltdc_progress_tick = s_last_ltdc_progress_tick;
    watchdog_record_unlock(irq_level);
}

static uint32_t watchdog_elapsed_ms(rt_tick_t age)
{
    return (uint32_t)(((uint64_t)age * 1000U) / RT_TICK_PER_SECOND);
}

/* This bounded probe runs from the independently ticking SysTick at the
 * context-sample cadence. It can latch core/component loss even when the RTOS
 * supervisor thread is starved, provided SysTick itself is still deliverable. */
static void watchdog_systick_health_probe(
    rt_tick_t now, const watchdog_runtime_context_t *context,
    const watchdog_interrupted_frame_t *frame)
{
    watchdog_record_header_t header;
    uint32_t now_ms = (uint32_t)(((uint64_t)now * 1000U) /
                                 RT_TICK_PER_SECOND);
    uint32_t index;

    if (!watchdog_header_snapshot(&header, RT_NULL))
    {
        return;
    }

    if (header.active_operation != 0U)
    {
        uint32_t timeout_ms = header.active_timeout_ms;
        uint32_t elapsed = now_ms - header.active_started_tick;
        if (timeout_ms == 0U || timeout_ms > WATCHDOG_OPERATION_MAX_TIMEOUT_MS)
        {
            timeout_ms = WATCHDOG_OPERATION_MAX_TIMEOUT_MS;
        }
        if (elapsed > timeout_ms && s_stale_logged == 0U)
        {
            s_stale_logged = 1U;
            (void)watchdog_first_failure_commit(
                WATCHDOG_FAILURE_OPERATION_TIMEOUT,
                header.active_operation, elapsed, (uint32_t)now,
                context, frame);
            watchdog_record_append_event(
                &g_watchdog_retained_record, (uint32_t)now,
                WATCHDOG_EVENT_HEALTH_STALE, header.active_operation,
                header.active_stage, header.last_core_pc,
                header.last_core_sp);
        }
    }

    if (header.core_ready == 0U)
    {
        if ((rt_tick_t)(now - s_supervisor_start_tick) >
                watchdog_ticks_from_ms(WATCHDOG_COMPONENT_INIT_TIMEOUT_MS) &&
            s_stale_logged == 0U)
        {
            s_stale_logged = 1U;
            (void)watchdog_first_failure_commit(
                WATCHDOG_FAILURE_INIT_TIMEOUT, 0U,
                watchdog_elapsed_ms(now - s_supervisor_start_tick),
                (uint32_t)now, context, frame);
            watchdog_record_append_event(
                &g_watchdog_retained_record, (uint32_t)now,
                WATCHDOG_EVENT_HEALTH_STALE, 0U,
                WATCHDOG_COMPONENT_INIT_TIMEOUT_MS,
                header.last_core_pc, header.last_core_sp);
        }
        return;
    }

    if ((rt_tick_t)(now - (rt_tick_t)header.last_core_tick) >
        watchdog_ticks_from_ms(WATCHDOG_CORE_PROGRESS_MAX_AGE_MS))
    {
        if (s_stale_logged == 0U)
        {
            rt_tick_t age = now - (rt_tick_t)header.last_core_tick;
            s_stale_logged = 1U;
            (void)watchdog_first_failure_commit(
                WATCHDOG_FAILURE_CORE_PROGRESS_STALE,
                header.last_core_progress, watchdog_elapsed_ms(age),
                (uint32_t)now, context, frame);
            watchdog_record_append_event(
                &g_watchdog_retained_record, (uint32_t)now,
                WATCHDOG_EVENT_HEALTH_STALE, header.last_core_progress,
                (uint32_t)age, header.last_core_pc, header.last_core_sp);
        }
    }

    for (index = 0U; index < WATCHDOG_COMPONENT_COUNT; ++index)
    {
        uint32_t mask = 1UL << index;
        watchdog_component_live_t *live = &s_component_live[index];
        rt_tick_t age;
        if ((s_component_expected_mask & s_component_required_mask & mask) == 0U)
        {
            continue;
        }
        age = now - (rt_tick_t)live->tick;
        if (age <= watchdog_ticks_from_ms(WATCHDOG_COMPONENT_MAX_AGE_MS) ||
            (s_stale_component_mask & mask) != 0U)
        {
            continue;
        }
        s_stale_component_mask |= mask;
        (void)watchdog_first_failure_commit(
            WATCHDOG_FAILURE_REQUIRED_COMPONENT_STALE, index,
            watchdog_elapsed_ms(age), (uint32_t)now, context, frame);
        watchdog_record_append_event(
            &g_watchdog_retained_record, (uint32_t)now,
            WATCHDOG_EVENT_HEALTH_STALE, 0x434D0000UL | (index + 1U),
            watchdog_elapsed_ms(age), live->pc, live->sp);
    }

    if (s_display_expected != 0U)
    {
        uint32_t frames = ltdc_frame_count();
        if (frames != s_systick_ltdc_frames)
        {
            s_systick_ltdc_frames = frames;
            s_systick_ltdc_progress_tick = (uint32_t)now;
        }
        else if ((uint32_t)(now - s_systick_ltdc_progress_tick) >
                 watchdog_ticks_from_ms(WATCHDOG_LTDC_MAX_AGE_MS) &&
                 (s_stale_component_mask & (1UL << 31)) == 0U)
        {
            uint32_t age = (uint32_t)(now - s_systick_ltdc_progress_tick);
            s_stale_component_mask |= (1UL << 31);
            (void)watchdog_first_failure_commit(
                WATCHDOG_FAILURE_DISPLAY_FRAMES_STALE, 0x4C544443UL,
                watchdog_elapsed_ms((rt_tick_t)age), (uint32_t)now,
                context, frame);
            watchdog_record_append_event(
                &g_watchdog_retained_record, (uint32_t)now,
                WATCHDOG_EVENT_HEALTH_STALE, 0x4C544443UL, age,
                header.last_core_pc, header.last_core_sp);
        }
    }
}

static int watchdog_trace_stack_contains(uint32_t address, uint32_t size)
{
    uint32_t end = address + size;

    if ((address & 3U) != 0U || end < address)
    {
        return 0;
    }
    return (address >= WATCHDOG_DTCM_BEGIN &&
            end <= WATCHDOG_DTCM_END) ||
           (address >= WATCHDOG_TRACE_AXI_BEGIN &&
            end <= WATCHDOG_TRACE_AXI_END) ||
           (address >= WATCHDOG_TRACE_PSRAM_BEGIN &&
            end <= WATCHDOG_TRACE_PSRAM_END);
}

static int watchdog_trace_pc_executable(uint32_t pc)
{
    uint32_t address = pc & ~1U;

    return (address >= WATCHDOG_APP_XIP_BEGIN &&
            address < WATCHDOG_APP_XIP_END) ||
           (address >= WATCHDOG_INTERNAL_FLASH_BEGIN &&
            address < WATCHDOG_INTERNAL_FLASH_END) ||
           address < 0x00010000UL;
}

/* The independent 1-kHz flight recorder deliberately avoids RT-Thread,
 * locks, logging, allocation, and the richer 10-Hz context path below. On the
 * Cortex-M7 thread path the original PSP is unaffected by the C handler
 * prologue, so only a validated PSP frame is trusted for PC/LR extraction. */
void watchdog_supervisor_trace_systick_capture(uint32_t exc_return,
                                               uint32_t psp, uint32_t msp,
                                               uint32_t ipsr)
{
    uint32_t sequence;
    uint32_t stack_pointer;
    uint32_t frame_words = 8U;
    uint32_t frame_offset = 0U;
    uint32_t pc = 0U;
    uint32_t lr = 0U;
    uint32_t xpsr = 0U;
    uint32_t packed = ipsr & 0x1FFU;
    uint32_t cycles = DWT->CYCCNT;
    int frame_valid = 0;

    if (s_watchdog_trace_enabled == 0U)
    {
        return;
    }

    if ((exc_return & (1UL << 2U)) != 0U)
    {
        packed |= (1UL << 30U);
        stack_pointer = psp;
        if ((exc_return & (1UL << 4U)) == 0U)
        {
            packed |= (1UL << 29U);
            frame_offset = WATCHDOG_TRACE_FP_FRAME_WORDS * 4U;
            frame_words += WATCHDOG_TRACE_FP_FRAME_WORDS;
        }
        if (watchdog_trace_stack_contains(stack_pointer,
                                           frame_words * 4U))
        {
            const volatile uint32_t *frame =
                (const volatile uint32_t *)(uintptr_t)(stack_pointer +
                                                       frame_offset);
            xpsr = frame[7];
            pc = frame[6];
            lr = frame[5];
            if ((xpsr & (1UL << 24U)) != 0U &&
                watchdog_trace_pc_executable(pc))
            {
                frame_valid = 1;
            }
        }
    }
    else
    {
        /* SysTick's C prologue may already have changed MSP. Keep the raw
         * stack hint but never dereference a potentially stale handler frame. */
        stack_pointer = msp;
    }

    if (frame_valid != 0)
    {
        packed = (packed & 0x1FFU) |
                 (xpsr & (1UL << 24U)) |
                 (1UL << 31U) | (1UL << 30U);
        if ((exc_return & (1UL << 4U)) == 0U)
        {
            packed |= (1UL << 29U);
        }
        if ((exc_return & (1UL << 2U)) != 0U)
        {
            packed |= (1UL << 25U);
        }
        if ((exc_return & (1UL << 4U)) != 0U)
        {
            packed |= (1UL << 26U);
        }
    }
    sequence = watchdog_trace_next_sequence(s_watchdog_trace_sequence);
    s_watchdog_trace_sequence = sequence;
    (void)watchdog_trace_append(&s_watchdog_trace_ring, sequence, cycles,
                                pc, lr, stack_pointer, packed);
}

void watchdog_supervisor_systick_capture(uint32_t exc_return,
                                         uint32_t psp, uint32_t msp,
                                         uint32_t ipsr, uint32_t primask,
                                         uint32_t basepri, uint32_t faultmask,
                                         uint32_t control)
{
    watchdog_runtime_context_t context;
    watchdog_interrupted_frame_t frame;
    watchdog_record_header_t header;
    rt_thread_t thread;
    rt_tick_t now;
    uint32_t pc = 0U;
    uint32_t lr = 0U;
    uint32_t stacked_xpsr = 0U;
    uint32_t context_flags = 0U;
    uint32_t sample_period_ticks;
    uint32_t sample_start_cycles;
    uint32_t frame_valid = 0U;
    uint32_t index;

    if (s_capture_enabled == 0U)
    {
        return;
    }
    now = rt_tick_get();
    sample_period_ticks =
        (WATCHDOG_CONTEXT_SAMPLE_PERIOD_MS * RT_TICK_PER_SECOND + 999U) /
        1000U;
    if (sample_period_ticks == 0U ||
        (uint32_t)(now - s_last_context_sample_tick) < sample_period_ticks)
    {
        return;
    }
    s_last_context_sample_tick = (uint32_t)now;
    sample_start_cycles = DWT->CYCCNT;

    thread = watchdog_self();
    watchdog_capture_interrupted_frame(exc_return, psp, thread, &pc, &lr,
                                       &stacked_xpsr, &context_flags);

    for (index = 0U; index < WATCHDOG_CONTEXT_WORDS; ++index)
    {
        ((uint32_t *)&context)[index] = 0U;
    }
    context.sequence = ++s_context_sequence;
    if (context.sequence == 0U)
    {
        context.sequence = ++s_context_sequence;
    }
    context.tick = (uint32_t)now;
    context.cycles = DWT->CYCCNT;
    context.thread = (uint32_t)(uintptr_t)thread;
    if (thread != RT_NULL)
    {
        const char *thread_name = thread->parent.name;
        uint8_t *name_bytes = (uint8_t *)context.thread_name;
        for (index = 0U; index < sizeof(context.thread_name); ++index)
        {
            name_bytes[index] = (uint8_t)thread_name[index];
        }
    }
    context.pc = pc;
    context.lr = lr;
    context.psp = psp;
    context.msp = msp;
    context.exc_return = exc_return;
    context.ipsr = (context_flags & WATCHDOG_CONTEXT_PC_VALID) != 0U
                       ? (stacked_xpsr & 0x1FFU) : ipsr;
    context.primask = primask;
    context.basepri = basepri;
    context.faultmask = faultmask;
    context.control = control;
    if (thread != RT_NULL)
    {
        context.thread_state =
            ((uint32_t)thread->sched_thread_ctx.stat & 0xFFU) |
            (((uint32_t)RT_SCHED_PRIV(thread).current_priority & 0xFFU) << 8U) |
            (((uint32_t)RT_SCHED_PRIV(thread).init_priority & 0xFFU) << 16U);
    }
    context.cfsr = SCB->CFSR;
    context.hfsr = SCB->HFSR;
    context.mmfar = SCB->MMFAR;
    context.bfar = SCB->BFAR;
    context.shcsr = SCB->SHCSR;
    context.health_flags = s_last_health_flags |
        ((s_component_expected_mask & 0x7FUL) << 8U) |
        ((s_component_required_mask & 0x7FUL) << 16U) |
        context_flags;
    if ((context.cfsr & (1UL << 7U)) != 0U)
    {
        context.health_flags |= WATCHDOG_CONTEXT_MMFAR_VALID;
    }
    if ((context.cfsr & (1UL << 15U)) != 0U)
    {
        context.health_flags |= WATCHDOG_CONTEXT_BFAR_VALID;
    }
    if (watchdog_header_snapshot(&header, RT_NULL) && header.core_ready != 0U &&
        (rt_tick_t)(now - (rt_tick_t)header.supervisor_tick) >
            watchdog_ticks_from_ms(500U))
    {
        context.health_flags |= WATCHDOG_CONTEXT_SUPERVISOR_STALE;
    }
    context.ltdc_frames = ltdc_frame_count();
    context.ltdc_reloads = ltdc_reload_count();
    context.gpu2d_errors = gpu2d_error_count();
    context.dma2d_errors = dma2d_service_error_count();
    memset(&frame, 0, sizeof(frame));
    if ((context_flags & WATCHDOG_CONTEXT_PC_VALID) != 0U)
    {
        const uint32_t *basic_frame = (const uint32_t *)(uintptr_t)psp;
        uint32_t fp_offset =
            (exc_return & (1UL << 4)) == 0U ? 18U : 0U;
        basic_frame += fp_offset;
        frame.sequence = context.sequence;
        frame.r0 = basic_frame[0];
        frame.r1 = basic_frame[1];
        frame.r2 = basic_frame[2];
        frame.r3 = basic_frame[3];
        frame.r12 = basic_frame[4];
        frame.xpsr = basic_frame[7];
        frame_valid = 1U;
        context.health_flags |= WATCHDOG_CONTEXT_FRAME_VALID;
        watchdog_record_frame_commit(&g_watchdog_retained_record, &frame);
    }
    /* Commit each owner-thread heartbeat into its own CRC/commit slot. The
     * source loop remains in ordinary SRAM; this bounded checkpoint survives
     * an asynchronous IWDG reset. */
    for (index = 0U; index < WATCHDOG_COMPONENT_COUNT; ++index)
    {
        uint32_t mask = 1UL << index;
        watchdog_component_breadcrumb_t breadcrumb;
        if ((s_component_expected_mask & mask) == 0U)
        {
            continue;
        }
        breadcrumb.sequence = ++s_component_checkpoint_sequence[index];
        if (breadcrumb.sequence == 0U)
        {
            breadcrumb.sequence = ++s_component_checkpoint_sequence[index];
        }
        breadcrumb.tick = s_component_live[index].tick;
        breadcrumb.thread = s_component_live[index].thread;
        breadcrumb.stage = s_component_live[index].stage;
        breadcrumb.progress = s_component_live[index].progress;
        breadcrumb.pc = s_component_live[index].pc;
        breadcrumb.sp = s_component_live[index].sp;
        breadcrumb.crc32 = 0U;
        breadcrumb.commit = 0U;
        watchdog_record_component_commit(&g_watchdog_retained_record,
                                         (watchdog_component_id_t)index,
                                         &breadcrumb);
    }

    watchdog_record_context_commit(&g_watchdog_retained_record, &context);
    watchdog_systick_health_probe(
        now, &context, frame_valid != 0U ? &frame : RT_NULL);

    if ((uint32_t)(now - s_last_event_sample_tick) >=
        watchdog_ticks_from_ms(WATCHDOG_EVENT_SAMPLE_PERIOD_MS))
    {
        s_last_event_sample_tick = (uint32_t)now;
        watchdog_record_append_event(&g_watchdog_retained_record,
                                     (uint32_t)now,
                                     WATCHDOG_EVENT_CONTEXT_SAMPLE,
                                     context.thread, psp, pc, msp);
    }
    s_capture_last_cycles = DWT->CYCCNT - sample_start_cycles;
    if (s_capture_last_cycles > s_capture_max_cycles)
    {
        s_capture_max_cycles = s_capture_last_cycles;
    }
}

static rt_thread_t watchdog_self(void)
{
    return rt_thread_self();
}

static int watchdog_header_snapshot(watchdog_record_header_t *header,
                                    unsigned *active_slot)
{
    return watchdog_record_active_header(&g_watchdog_retained_record,
                                         header, active_slot);
}

static void watchdog_header_publish(watchdog_record_header_t *header,
                                    unsigned active_slot)
{
    watchdog_record_header_commit(&g_watchdog_retained_record, header,
                                  active_slot);
}

static void watchdog_supervisor_event_append(uint32_t code,
                                            uint32_t arg0,
                                            uint32_t arg1,
                                            uint32_t callsite_pc,
                                            uint32_t stack_pointer)
{
    rt_base_t irq_level;

    if (s_feed_event_enabled == 0U)
    {
        return;
    }
    irq_level = watchdog_record_lock();
    watchdog_record_append_event(&g_watchdog_retained_record,
                                 (uint32_t)rt_tick_get(), code, arg0, arg1,
                                 callsite_pc, stack_pointer);
    watchdog_record_unlock(irq_level);
}

int watchdog_supervisor_early_init(void)
{
    watchdog_record_header_t header;
    uint32_t reset_flags = RCC->RSR;
    rt_base_t irq_level;

    if (s_early_init_done != 0U)
    {
        return RT_EOK;
    }

    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_BKPRAM_CLK_ENABLE();
    /* Boot owns the POR/tamper erase wait and only jumps to Appli once the
     * retained region is safe. MEF=0 after a normal reset is not a global
     * BKPSRAM failure, so the App must not repeat that Boot-only gate. */
    __DSB();

    irq_level = watchdog_record_lock();
    if (s_early_init_done != 0U)
    {
        watchdog_record_unlock(irq_level);
        return RT_EOK;
    }

    if (!watchdog_header_snapshot(&header, RT_NULL))
    {
        watchdog_record_initialize(&g_watchdog_retained_record,
                                  WORK_SOURCE_FINGERPRINT, reset_flags);
    }
    watchdog_record_set_build_fingerprint(&g_watchdog_retained_record,
                                          WORK_SOURCE_FINGERPRINT);
    s_watchdog_trace_enabled = 0U;
    /* Keep reset metadata for consecutive-IWDG recovery, but do not append a
     * normal boot/reset event to the crash log. Boot writes only abnormal
     * records to external Flash. */
    s_fatal_fault_latched = 0U;
    if (g_watchdog_retained_record.footer.reserved[0] !=
            WATCHDOG_PENDING_FLASH_MAGIC &&
        !watchdog_fault_valid(&g_watchdog_retained_record.fault))
    {
        watchdog_record_clear_diagnostics(&g_watchdog_retained_record);
        watchdog_first_failure_clear();
        memset((void *)s_component_live, 0, sizeof(s_component_live));
        memset((void *)s_component_checkpoint_sequence, 0,
               sizeof(s_component_checkpoint_sequence));
        s_component_expected_mask = 0U;
        s_component_required_mask = 0U;
        s_stale_component_mask = 0U;
        s_context_sequence = 0U;
        s_last_context_sample_tick = (uint32_t)rt_tick_get();
        s_last_event_sample_tick = s_last_context_sample_tick;
        s_supervisor_start_tick = s_last_context_sample_tick;
        s_systick_ltdc_frames = ltdc_frame_count();
        s_systick_ltdc_progress_tick = s_last_context_sample_tick;
        s_capture_last_cycles = 0U;
        s_capture_max_cycles = 0U;
        s_feed_event_enabled = 1U;
        s_supervisor_wake_sequence = 0U;
        s_feed_attempt_sequence = 0U;
        s_feed_return_sequence = 0U;
        s_health_reject_reason = WATCHDOG_REJECT_NONE;
        s_health_reject_logged = 0U;
        s_capture_enabled = 1U;
        if (g_watchdog_retained_record.footer.reserved[
                WATCHDOG_TRACE_PENDING_WORD] ==
            WATCHDOG_TRACE_PENDING_MAGIC)
        {
            /* Boot has not durably copied the previous ring yet. Preserve it
             * for a retry rather than replacing its samples with this run. */
            s_watchdog_trace_enabled = 0U;
        }
        else if (watchdog_header_snapshot(&header, RT_NULL) &&
                 watchdog_trace_initialize(&s_watchdog_trace_ring,
                                           header.boot_sequence,
                                           header.source_fingerprint_crc32) ==
                     WATCHDOG_TRACE_OK)
        {
            s_watchdog_trace_sequence = 0U;
            s_watchdog_trace_enabled = 1U;
        }
    }
    else
    {
        /* Do not overwrite a crash that Boot has not durably copied yet. */
        s_feed_event_enabled = 0U;
        s_capture_enabled = 0U;
        s_watchdog_trace_enabled = 0U;
    }
    __DSB();
    s_early_init_done = 1U;
    watchdog_record_unlock(irq_level);
    return RT_EOK;
}

WATCHDOG_NOINLINE void watchdog_supervisor_boot_phase_begin(
    uint32_t operation_id, uint32_t timeout_ms)
{
    watchdog_record_header_t header;
    unsigned active_slot;
    rt_base_t irq_level = watchdog_record_lock();
    if (!watchdog_header_snapshot(&header, &active_slot))
    {
        watchdog_record_unlock(irq_level);
        return;
    }
    if (timeout_ms == 0U || timeout_ms > WATCHDOG_OPERATION_MAX_TIMEOUT_MS)
    {
        timeout_ms = WATCHDOG_OPERATION_MAX_TIMEOUT_MS;
    }
    header.active_operation = operation_id;
    header.active_stage = 1U;
    header.active_started_tick = watchdog_now_ms();
    header.active_timeout_ms = timeout_ms;
    header.active_progress = 0U;
    watchdog_header_publish(&header, active_slot);
    watchdog_record_append_event(&g_watchdog_retained_record, rt_tick_get(),
                                 WATCHDOG_EVENT_OPERATION_BEGIN,
                                 operation_id, timeout_ms,
                                 WATCHDOG_CALLER_PC(), watchdog_current_sp());
    watchdog_record_unlock(irq_level);
}

WATCHDOG_NOINLINE void watchdog_supervisor_operation_progress(
    uint32_t stage, uint32_t value)
{
    watchdog_record_header_t header;
    unsigned active_slot;
    rt_base_t irq_level = watchdog_record_lock();
    if (!watchdog_header_snapshot(&header, &active_slot) ||
        header.active_operation == 0U)
    {
        watchdog_record_unlock(irq_level);
        return;
    }
    if (header.active_stage == stage && header.active_progress == value)
    {
        watchdog_record_unlock(irq_level);
        return;
    }
    header.active_stage = stage;
    header.active_progress = value;
    header.active_started_tick = watchdog_now_ms();
    watchdog_header_publish(&header, active_slot);
    watchdog_record_append_event(&g_watchdog_retained_record, rt_tick_get(),
                                 WATCHDOG_EVENT_OPERATION_PROGRESS,
                                 ((header.active_operation & 0xFFFFU) << 16U) |
                                     (stage & 0xFFFFU),
                                 value,
                                 WATCHDOG_CALLER_PC(), watchdog_current_sp());
    watchdog_record_unlock(irq_level);
}

WATCHDOG_NOINLINE void watchdog_supervisor_operation_end(uint32_t result)
{
    watchdog_record_header_t header;
    unsigned active_slot;
    uint32_t operation_id;
    rt_base_t irq_level = watchdog_record_lock();
    if (!watchdog_header_snapshot(&header, &active_slot) ||
        header.active_operation == 0U)
    {
        watchdog_record_unlock(irq_level);
        return;
    }
    operation_id = header.active_operation;
    header.active_operation = 0U;
    header.active_stage = 0U;
    header.active_started_tick = watchdog_now_ms();
    header.active_progress = result;
    watchdog_header_publish(&header, active_slot);
    watchdog_record_append_event(&g_watchdog_retained_record, rt_tick_get(),
                                 WATCHDOG_EVENT_OPERATION_END,
                                 operation_id, result,
                                 WATCHDOG_CALLER_PC(), watchdog_current_sp());
    watchdog_record_unlock(irq_level);
}

WATCHDOG_NOINLINE void watchdog_supervisor_core_ready(void)
{
    watchdog_record_header_t header;
    unsigned active_slot;
    rt_base_t irq_level = watchdog_record_lock();
    if (!watchdog_header_snapshot(&header, &active_slot))
    {
        watchdog_record_unlock(irq_level);
        return;
    }
    s_stable_start_tick = rt_tick_get();
    header.core_ready = 1U;
    header.last_core_tick = rt_tick_get();
    header.last_core_progress += 1U;
    header.last_core_pc = WATCHDOG_CALLER_PC();
    header.last_core_sp = watchdog_current_sp();
    header.last_core_thread = (uint32_t)(uintptr_t)watchdog_self();
    watchdog_header_publish(&header, active_slot);
    watchdog_record_append_event(&g_watchdog_retained_record, rt_tick_get(),
                                 WATCHDOG_EVENT_CORE_READY,
                                 header.last_core_progress, 0U,
                                 header.last_core_pc, header.last_core_sp);
    watchdog_record_unlock(irq_level);
}

WATCHDOG_NOINLINE void watchdog_supervisor_core_progress(uint32_t progress_id)
{
    watchdog_record_header_t header;
    unsigned active_slot;
    uint32_t pc;
    uint32_t sp;
    rt_base_t irq_level;

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (s_test_mode == WATCHDOG_TEST_CORE_STALL)
    {
        return;
    }
#endif

    irq_level = watchdog_record_lock();
    if (!watchdog_header_snapshot(&header, &active_slot))
    {
        watchdog_record_unlock(irq_level);
        return;
    }
    pc = WATCHDOG_CALLER_PC();
    sp = watchdog_current_sp();
    header.last_core_tick = rt_tick_get();
    header.last_core_progress = progress_id;
    header.last_core_pc = pc;
    header.last_core_sp = sp;
    header.last_core_thread = (uint32_t)(uintptr_t)watchdog_self();
    watchdog_header_publish(&header, active_slot);
    /* Keep the compact event ring available for the independent 1 Hz CPU
     * context samples. The full coordinator tick/progress remains in the
     * dual-copy header, while ten-second checkpoints preserve milestones. */
    if (progress_id == 1U || (progress_id % 10U) == 0U)
    {
        watchdog_record_append_event(&g_watchdog_retained_record,
                                     rt_tick_get(),
                                     WATCHDOG_EVENT_CORE_PROGRESS,
                                     progress_id, header.core_ready,
                                     pc, sp);
    }
    watchdog_record_unlock(irq_level);
}

static void watchdog_supervisor_record_fault_internal(
    uint32_t kind, uint32_t exc_return, const uint32_t *frame,
    uint32_t thread_id, uint32_t frame_address,
    uint32_t basic_frame_address, uint32_t file_address,
    uint32_t function_address, uint32_t expression_address,
    uint32_t caller_address, uint32_t line,
    const uint32_t *panic_stack, uint32_t panic_stack_words,
    uint32_t panic_stack_sp)
{
    uint32_t tick = 0U;
    uint32_t ipsr = __get_IPSR();
    watchdog_runtime_context_t prior_context;
    watchdog_runtime_context_t failure_context;
    watchdog_interrupted_frame_t failure_frame;
    const watchdog_runtime_context_t *failure_context_ptr = RT_NULL;
    const watchdog_interrupted_frame_t *failure_frame_ptr = RT_NULL;
    int prior_context_valid;
    uint32_t sample_age_ms = 0U;
    rt_base_t irq_level = 0;
    int fault_committed;
    int frame_readable = 0;
    int stacked_frame_valid = 0;

    /* Exception context already excludes thread writers. Do not call the
     * interrupt-disable primitive again from its own fault handler: the
     * latest incident faulted while executing rt_hw_interrupt_disable(). */
    if (ipsr == 0U)
    {
        irq_level = watchdog_record_lock();
    }

    if (s_early_init_done == 0U)
    {
        if (ipsr == 0U)
        {
            watchdog_record_unlock(irq_level);
        }
        return;
    }

    prior_context_valid = watchdog_record_context_valid(
        &g_watchdog_retained_record, &prior_context);
    /* rt_tick_get is a single atomic read of the global tick in this RT-Thread
     * port; it takes no scheduler lock and is safe in exception context. */
    tick = (uint32_t)rt_tick_get();
    if (prior_context_valid)
    {
        sample_age_ms = watchdog_elapsed_ms(
            (rt_tick_t)(tick - prior_context.tick));
        failure_context_ptr = &prior_context;
    }

    /* `frame` is only dereferenced after fault_diagnostics has checked that
     * the basic frame is aligned and wholly inside internal SRAM. Keep the
     * raw stacked PC/LR even when xPSR makes the frame unusable for symbolizing. */
    if (kind != WATCHDOG_FAULT_ASSERT && frame != RT_NULL &&
        (((uintptr_t)frame & (sizeof(uint32_t) - 1U)) == 0U) &&
        (uintptr_t)frame == (uintptr_t)basic_frame_address &&
        (((uintptr_t)frame >= WATCHDOG_DTCM_BEGIN &&
          (uintptr_t)frame <= WATCHDOG_DTCM_END -
              (8U * sizeof(uint32_t))) ||
         ((uintptr_t)frame >= WATCHDOG_AXI_RAM_BEGIN &&
          (uintptr_t)frame <= WATCHDOG_AXI_FAULT_FRAME_END -
              (8U * sizeof(uint32_t)))))
    {
        frame_readable = 1;
        stacked_frame_valid =
            ((frame[7] & (1UL << 24U)) != 0U) &&
            watchdog_pc_is_executable(frame[6]);
    }

    memset(&failure_context, 0, sizeof(failure_context));
    memset(&failure_frame, 0, sizeof(failure_frame));
    failure_context.sequence = prior_context_valid
        ? prior_context.sequence + 1U : s_context_sequence + 1U;
    if (failure_context.sequence == 0U)
    {
        failure_context.sequence = 1U;
    }
    failure_context.tick = tick;
    failure_context.cycles = DWT->CYCCNT;
    failure_context.thread = SCB->VTOR;
    if (frame_readable != 0)
    {
        failure_context.pc = frame[6];
        failure_context.lr = frame[5];
    }
    failure_context.psp = __get_PSP();
    failure_context.msp = __get_MSP();
    failure_context.exc_return = exc_return;
    failure_context.ipsr = ipsr;
    failure_context.primask = __get_PRIMASK();
    failure_context.basepri = __get_BASEPRI();
    failure_context.faultmask = __get_FAULTMASK();
    failure_context.control = __get_CONTROL();
    failure_context.cfsr = SCB->CFSR;
    failure_context.hfsr = SCB->HFSR;
    failure_context.mmfar = SCB->MMFAR;
    failure_context.bfar = SCB->BFAR;
    failure_context.shcsr = SCB->SHCSR;
    failure_context.health_flags = WATCHDOG_CONTEXT_FAULT_REGS_VALID |
                                   WATCHDOG_HEALTH_FAILURE_LATCHED;

    /* The exception path avoids querying RT-Thread for a TCB. Use the
     * otherwise-unused context fields for fault-time debug and XIP state.
     * The high-bit tag prevents these values being decoded as a thread name
     * or normal peripheral counters. */
    failure_context.thread_name[0] = SCB->DFSR;
    failure_context.thread_name[1] = CoreDebug->DEMCR;
    failure_context.thread_name[2] = CoreDebug->DHCSR;
    failure_context.thread_name[3] = SCB->ICSR;
    failure_context.ltdc_frames = ICACHE->CR;
    failure_context.ltdc_reloads = ICACHE->SR;
    failure_context.gpu2d_errors = XSPI2->CR;
    failure_context.dma2d_errors = XSPI2->DCR2;
    failure_context.thread_state = XSPI2->SR;

    if ((exc_return & (1UL << 2U)) != 0U)
    {
        failure_context.health_flags |= WATCHDOG_CONTEXT_STACK_FROM_PSP;
    }
    if ((exc_return & (1UL << 4U)) == 0U)
    {
        failure_context.health_flags |= WATCHDOG_CONTEXT_EXTENDED_FP_FRAME;
    }
    if ((failure_context.cfsr & (1UL << 7U)) != 0U)
    {
        failure_context.health_flags |= WATCHDOG_CONTEXT_MMFAR_VALID;
    }
    if ((failure_context.cfsr & (1UL << 15U)) != 0U)
    {
        failure_context.health_flags |= WATCHDOG_CONTEXT_BFAR_VALID;
    }

    if (stacked_frame_valid != 0)
    {
        failure_context.health_flags |= WATCHDOG_CONTEXT_PC_VALID |
                                        WATCHDOG_CONTEXT_FRAME_VALID;
        failure_frame.sequence = failure_context.sequence;
        failure_frame.r0 = frame[0];
        failure_frame.r1 = frame[1];
        failure_frame.r2 = frame[2];
        failure_frame.r3 = frame[3];
        failure_frame.r12 = frame[4];
        failure_frame.xpsr = frame[7];
        failure_context_ptr = &failure_context;
        failure_frame_ptr = &failure_frame;
        sample_age_ms = 0U;
    }
    else if ((kind == WATCHDOG_FAULT_STACK_OVERFLOW ||
              kind == WATCHDOG_FAULT_ERROR_HANDLER) &&
             watchdog_pc_is_executable(caller_address))
    {
        failure_context.pc = caller_address;
        failure_context.health_flags |= WATCHDOG_CONTEXT_PC_VALID;
        failure_context_ptr = &failure_context;
        sample_age_ms = 0U;
    }
    else if (prior_context_valid)
    {
        /* Keep first-failure's pre-fault sample separate from the latest
         * fault-time snapshot committed below. */
        failure_context_ptr = &prior_context;
    }
    (void)watchdog_first_failure_commit(
        kind == WATCHDOG_FAULT_ASSERT ? WATCHDOG_FAILURE_ASSERTION
                                      : WATCHDOG_FAILURE_SYNCHRONOUS_FAULT,
        kind, sample_age_ms, tick, failure_context_ptr, failure_frame_ptr);

    /* The fault handler is intentionally limited to SRAM register writes. It
     * does not call RT-Thread, Flash, UART, or any subsystem lock. */
    watchdog_h7r7_record_fault(&g_watchdog_retained_record,
                          (watchdog_fault_kind_t)kind, exc_return,
                          SCB->CFSR, SCB->HFSR, SCB->MMFAR, SCB->BFAR,
                          frame_address, basic_frame_address,
                          __get_PSP(), __get_MSP(), thread_id, tick,
                          DWT->CYCCNT, __get_PRIMASK(), __get_BASEPRI(),
                          __get_CONTROL(), frame, file_address,
                          function_address, expression_address,
                          caller_address, line);
    /* Always commit the latest hardware snapshot independently from the
     * first-failure sidecar. The latter is intentionally write-once and may
     * describe an older incident. */
    watchdog_record_context_commit(&g_watchdog_retained_record,
                                   &failure_context);
    if (stacked_frame_valid != 0)
    {
        watchdog_record_frame_commit(&g_watchdog_retained_record,
                                     &failure_frame);
    }
    __DSB();
    fault_committed = watchdog_fault_valid(&g_watchdog_retained_record.fault);
    if (fault_committed && panic_stack != RT_NULL)
    {
        watchdog_panic_stack_commit(panic_stack_sp, panic_stack,
                                    panic_stack_words);
        __DSB();
        fault_committed = watchdog_fault_valid(
            &g_watchdog_retained_record.fault);
    }
    if (fault_committed)
    {
        watchdog_record_header_t active_header;
        if (watchdog_record_active_header(&g_watchdog_retained_record,
                                          &active_header, RT_NULL))
        {
            watchdog_record_stage_commit(&g_watchdog_retained_record,
                                         active_header.boot_sequence);
            fault_committed = watchdog_record_mark_fault_reset_requested(
                &g_watchdog_retained_record);
        }
        else
        {
            fault_committed = 0;
        }
        __DSB();
    }
    /* RT_ASSERT and Error_Handler may fail-stop a single task while the
     * coordinator still runs. Latch the fault and request a prompt system
     * reset after the retained record is fully committed. */
    s_fatal_fault_latched = 1U;
    __DSB();
    /* Exception handlers intentionally remain masked while fail-stopped.
     * A thread-mode Error_Handler restores the prior IRQ state for its caller. */
    if (ipsr == 0U)
    {
        watchdog_record_unlock(irq_level);
    }
    if (fault_committed)
    {
        /* If this request cannot complete (for example because the core is
         * already in lockup), the independent IWDG remains armed as fallback. */
        NVIC_SystemReset();
    }
}

void watchdog_supervisor_record_fault(uint32_t kind,
                                      uint32_t exc_return,
                                      const uint32_t *frame,
                                      uint32_t thread_id,
                                      uint32_t frame_address,
                                      uint32_t basic_frame_address,
                                      uint32_t file_address,
                                      uint32_t function_address,
                                      uint32_t expression_address,
                                      uint32_t caller_address,
                                      uint32_t line)
{
    watchdog_supervisor_record_fault_internal(
        kind, exc_return, frame, thread_id, frame_address,
        basic_frame_address, file_address, function_address,
        expression_address, caller_address, line, RT_NULL, 0U, 0U);
}

void watchdog_supervisor_record_panic_fault(
    uint32_t thread_id, const uint32_t *stack, uint32_t stack_words,
    uint32_t captured_sp, uint32_t file_address, uint32_t function_address,
    uint32_t expression_address, uint32_t caller_address, uint32_t line)
{
    watchdog_supervisor_record_fault_internal(
        WATCHDOG_FAULT_ASSERT, 0U, stack, thread_id, captured_sp,
        captured_sp, file_address, function_address, expression_address,
        caller_address, line, stack, stack_words, captured_sp);
}

static int watchdog_health_pass(rt_tick_t now, rt_tick_t boot_deadline,
                                uint32_t *health_flags)
{
    watchdog_record_header_t header;
    rt_tick_t core_age;
    uint32_t now_ms;
    unsigned active_slot;
    int all_components_healthy = 1;
    uint32_t component_index;

    if (health_flags != NULL)
    {
        *health_flags = 0U;
    }
    s_health_reject_reason = WATCHDOG_REJECT_NONE;
    if (!watchdog_header_snapshot(&header, &active_slot))
    {
        s_health_reject_reason = WATCHDOG_REJECT_HEADER_INVALID;
        return 0;
    }
    if (health_flags != NULL)
    {
        *health_flags |= WATCHDOG_HEALTH_TICK_MOVING;
    }

    now_ms = watchdog_now_ms();
    if (header.active_operation != 0U)
    {
        uint32_t timeout_ms = header.active_timeout_ms;
        if (timeout_ms == 0U || timeout_ms > WATCHDOG_OPERATION_MAX_TIMEOUT_MS)
        {
            timeout_ms = WATCHDOG_OPERATION_MAX_TIMEOUT_MS;
        }
        if ((uint32_t)(now_ms - header.active_started_tick) > timeout_ms)
        {
            s_health_reject_reason = WATCHDOG_REJECT_OPERATION_TIMEOUT;
            if (s_stale_logged == 0U)
            {
                uint32_t elapsed = now_ms - header.active_started_tick;
                watchdog_record_append_event(&g_watchdog_retained_record, now,
                                             WATCHDOG_EVENT_HEALTH_STALE,
                                             header.active_operation,
                                             header.active_stage,
                                             header.last_core_pc,
                                             header.last_core_sp);
                (void)watchdog_first_failure_commit(
                    WATCHDOG_FAILURE_OPERATION_TIMEOUT,
                    header.active_operation, elapsed, (uint32_t)now,
                    RT_NULL, RT_NULL);
                s_stale_logged = 1U;
            }
            if (health_flags != NULL)
            {
                *health_flags |= WATCHDOG_HEALTH_FAILURE_LATCHED;
            }
            return 0;
        }
    }
    else if (health_flags != NULL)
    {
        *health_flags |= WATCHDOG_HEALTH_OPERATION_IDLE;
    }

    if (header.core_ready == 0U)
    {
        if ((rt_tick_t)(now - s_supervisor_start_tick) > boot_deadline)
        {
            s_health_reject_reason =
                WATCHDOG_REJECT_INITIALIZATION_TIMEOUT;
            if (s_stale_logged == 0U)
            {
                watchdog_record_append_event(&g_watchdog_retained_record, now,
                                             WATCHDOG_EVENT_HEALTH_STALE,
                                             0U, WATCHDOG_COMPONENT_INIT_TIMEOUT_MS,
                                             header.last_core_pc,
                                             header.last_core_sp);
                (void)watchdog_first_failure_commit(
                    WATCHDOG_FAILURE_INIT_TIMEOUT, 0U,
                    watchdog_elapsed_ms(now - s_supervisor_start_tick),
                    (uint32_t)now, RT_NULL, RT_NULL);
                s_stale_logged = 1U;
            }
            if (health_flags != NULL)
            {
                *health_flags |= WATCHDOG_HEALTH_FAILURE_LATCHED;
            }
            return 0;
        }
        return 1;
    }

    core_age = now - (rt_tick_t)header.last_core_tick;
    if (core_age > watchdog_ticks_from_ms(WATCHDOG_CORE_PROGRESS_MAX_AGE_MS))
    {
        s_health_reject_reason = WATCHDOG_REJECT_CORE_PROGRESS_STALE;
        if (s_stale_logged == 0U)
        {
            rt_tick_t age = now - (rt_tick_t)header.last_core_tick;
            watchdog_record_append_event(&g_watchdog_retained_record, now,
                                         WATCHDOG_EVENT_HEALTH_STALE,
                                         header.last_core_progress,
                                         (uint32_t)age,
                                         header.last_core_pc,
                                         header.last_core_sp);
            (void)watchdog_first_failure_commit(
                WATCHDOG_FAILURE_CORE_PROGRESS_STALE,
                header.last_core_progress, watchdog_elapsed_ms(age),
                (uint32_t)now, RT_NULL, RT_NULL);
            s_stale_logged = 1U;
        }
        if (health_flags != NULL)
        {
            *health_flags |= WATCHDOG_HEALTH_FAILURE_LATCHED;
        }
        return 0;
    }
    s_stale_logged = 0U;
    if (health_flags != NULL)
    {
        *health_flags |= WATCHDOG_HEALTH_CORE_PROGRESS;
    }

    for (component_index = 0U;
         component_index < WATCHDOG_COMPONENT_COUNT;
         ++component_index)
    {
        uint32_t mask = 1UL << component_index;
        rt_tick_t age;
        uint32_t age_ms;
        watchdog_component_live_t *live = &s_component_live[component_index];

        if ((s_component_expected_mask & s_component_required_mask & mask) == 0U)
        {
            continue;
        }
        age = now - (rt_tick_t)live->tick;
        if (age <= watchdog_ticks_from_ms(WATCHDOG_COMPONENT_MAX_AGE_MS))
        {
            continue;
        }

        all_components_healthy = 0;
        if (s_health_reject_reason == WATCHDOG_REJECT_NONE)
        {
            s_health_reject_reason =
                WATCHDOG_REJECT_REQUIRED_COMPONENT_STALE;
        }
        if ((s_stale_component_mask & mask) == 0U)
        {
            age_ms = (uint32_t)(((uint64_t)age * 1000U) /
                                RT_TICK_PER_SECOND);
            watchdog_record_append_event(
                &g_watchdog_retained_record, now,
                WATCHDOG_EVENT_HEALTH_STALE,
                0x434D0000UL | (component_index + 1U), age_ms,
                live->pc, live->sp);
            (void)watchdog_first_failure_commit(
                WATCHDOG_FAILURE_REQUIRED_COMPONENT_STALE,
                component_index, age_ms, (uint32_t)now, RT_NULL, RT_NULL);
            s_stale_component_mask |= mask;
        }
    }
    if (all_components_healthy && health_flags != NULL)
    {
        *health_flags |= WATCHDOG_HEALTH_REQUIRED_COMPONENTS;
    }

    if (s_display_expected != 0U)
    {
        uint32_t frames = ltdc_frame_count();
        if (frames != s_last_ltdc_frames)
        {
            s_last_ltdc_frames = frames;
            s_last_ltdc_progress_tick = (uint32_t)now;
            if (health_flags != NULL)
            {
                *health_flags |= WATCHDOG_HEALTH_DISPLAY_FRAMES;
            }
        }
        else if ((uint32_t)(now - s_last_ltdc_progress_tick) >
                 watchdog_ticks_from_ms(WATCHDOG_LTDC_MAX_AGE_MS))
        {
            all_components_healthy = 0;
            if (s_health_reject_reason == WATCHDOG_REJECT_NONE)
            {
                s_health_reject_reason =
                    WATCHDOG_REJECT_DISPLAY_FRAMES_STALE;
            }
            if ((s_stale_component_mask & (1UL << 31)) == 0U)
            {
                watchdog_record_append_event(
                    &g_watchdog_retained_record, now,
                    WATCHDOG_EVENT_HEALTH_STALE,
                    0x4C544443UL,
                    (uint32_t)(now - s_last_ltdc_progress_tick),
                    header.last_core_pc, header.last_core_sp);
                (void)watchdog_first_failure_commit(
                    WATCHDOG_FAILURE_DISPLAY_FRAMES_STALE, 0x4C544443UL,
                    watchdog_elapsed_ms((rt_tick_t)(now -
                        s_last_ltdc_progress_tick)), (uint32_t)now,
                    RT_NULL, RT_NULL);
                s_stale_component_mask |= (1UL << 31);
            }
        }
        else if (health_flags != NULL)
        {
            *health_flags |= WATCHDOG_HEALTH_DISPLAY_FRAMES;
        }
    }

    if (!all_components_healthy)
    {
        if (health_flags != NULL)
        {
            *health_flags |= WATCHDOG_HEALTH_FAILURE_LATCHED;
        }
        s_last_health_flags = health_flags != NULL ? *health_flags : 0U;
        return 0;
    }
    s_stale_component_mask = 0U;
    s_last_health_flags = health_flags != NULL ? *health_flags : 0U;

    if (s_stable_start_tick != 0U &&
        (rt_tick_t)(now - s_stable_start_tick) >=
            watchdog_ticks_from_ms(WATCHDOG_STABLE_CLEAR_MS) &&
        header.reset_count != 0U)
    {
        watchdog_record_clear_recovery(&g_watchdog_retained_record);
        s_stable_start_tick = now;
    }
    return 1;
}

WATCHDOG_NOINLINE rt_err_t
work_watchdog_stack_overflow_hook(struct rt_thread *thread)
{
    uint32_t thread_address = (uint32_t)(uintptr_t)thread;
    uint32_t stack_pointer = 0U;
    uint32_t stack_address = 0U;
    uint32_t stack_size = 0U;

    if (thread != RT_NULL)
    {
        stack_pointer = (uint32_t)(uintptr_t)thread->sp;
        stack_address = (uint32_t)(uintptr_t)thread->stack_addr;
        stack_size = (uint32_t)thread->stack_size;
    }
    watchdog_supervisor_record_fault(
        WATCHDOG_FAULT_STACK_OVERFLOW, 0U, RT_NULL, thread_address,
        stack_pointer, stack_address, stack_size, 0U, 0U,
        WATCHDOG_CALLER_PC(), 0U);

    /* RT-Thread treats non-RT_EOK as a fail-stop. The captured fault requests
     * a software reset immediately; the independent IWDG remains fallback. */
    return -RT_ERROR;
}

static int watchdog_supervisor_health_check(void *context,
                                           rt_tick_t now,
                                           uint32_t *health_flags)
{
    rt_tick_t boot_deadline = watchdog_ticks_from_ms(WATCHDOG_COMPONENT_INIT_TIMEOUT_MS);
    int healthy;
    rt_base_t irq_level;
    RT_UNUSED(context);

    s_supervisor_wake_sequence++;
    s_health_reject_reason = WATCHDOG_REJECT_NONE;

    if (health_flags != RT_NULL)
    {
        *health_flags = 0U;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (s_test_mode == WATCHDOG_TEST_SUPERVISOR_SPIN)
    {
        /* Keep SysTick running while preventing task switches. This
         * isolates scheduler failure from the separate IRQ-off test. */
        rt_enter_critical();
        for (;;) { __NOP(); }
    }
#endif

    irq_level = watchdog_record_lock();
    if (s_fatal_fault_latched != 0U)
    {
        healthy = 0;
        s_health_reject_reason = WATCHDOG_REJECT_FATAL_FAULT;
        if (health_flags != RT_NULL)
        {
            *health_flags |= WATCHDOG_HEALTH_FAILURE_LATCHED;
        }
    }
    else
    {
        healthy = watchdog_health_pass(now, boot_deadline, health_flags);
    }
    if (healthy != 0)
    {
        watchdog_record_header_t header;
        unsigned active_slot;
        if (watchdog_header_snapshot(&header, &active_slot))
        {
            header.supervisor_tick = now;
            header.health_flags = health_flags != RT_NULL
                                      ? *health_flags : 0U;
            watchdog_header_publish(&header, active_slot);
        }
    }
    watchdog_record_unlock(irq_level);
    s_last_health_flags = health_flags != RT_NULL ? *health_flags : 0U;
    if (healthy != 0)
    {
        s_health_reject_logged = 0U;
    }
    else if (s_health_reject_logged == 0U)
    {
        uint32_t reason = s_health_reject_reason != WATCHDOG_REJECT_NONE
                              ? s_health_reject_reason
                              : WATCHDOG_REJECT_OTHER;
        watchdog_supervisor_event_append(
            WATCHDOG_EVENT_HEALTH_REJECTED, reason, s_last_health_flags,
            WATCHDOG_CALLER_PC(), watchdog_current_sp());
        s_health_reject_logged = 1U;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (healthy == 0 && s_test_mode == WATCHDOG_TEST_COMPONENT_STALE)
    {
        /* The stale-reason records are committed above. Keep SysTick alive
         * while preventing task scheduling to prove the final context sample
         * and hardware IWDG path in one controlled board test. */
        rt_enter_critical();
        for (;;) { __NOP(); }
    }
#endif
    return healthy;
}

static void watchdog_supervisor_feed(void *context)
{
    uint32_t checkpoint = 0U;
    RT_UNUSED(context);
    s_feed_attempt_sequence++;
    if (s_supervisor_wake_sequence == 1U ||
        (s_supervisor_wake_sequence % WATCHDOG_FEED_EVENT_INTERVAL_POLLS) == 0U)
    {
        checkpoint = 1U;
        watchdog_supervisor_event_append(
            WATCHDOG_EVENT_FEED_BEGIN, s_feed_attempt_sequence,
            s_last_health_flags, WATCHDOG_CALLER_PC(), watchdog_current_sp());
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    if (s_test_mode == WATCHDOG_TEST_NOFEED)
    {
        if (checkpoint != 0U)
        {
            watchdog_supervisor_event_append(
                WATCHDOG_EVENT_FEED_SUPPRESSED, s_feed_attempt_sequence,
                s_last_health_flags, WATCHDOG_CALLER_PC(),
                watchdog_current_sp());
        }
        return;
    }
#endif
    watchdog_h7r7_iwdg_refresh();
    s_feed_return_sequence++;
    if (checkpoint != 0U)
    {
        watchdog_supervisor_event_append(
            WATCHDOG_EVENT_FEED_RETURNED, s_feed_attempt_sequence,
            s_feed_return_sequence, WATCHDOG_CALLER_PC(),
            watchdog_current_sp());
    }
}

int watchdog_supervisor_start(void)
{
    const watchdog_rtt_supervisor_config_t config = {
        "wdog",
        WATCHDOG_POLL_MS,
        WATCHDOG_THREAD_STACK_SIZE,
        WATCHDOG_THREAD_PRIORITY,
        WATCHDOG_THREAD_TIMESLICE,
        watchdog_supervisor_health_check,
        watchdog_supervisor_feed,
        RT_NULL
    };

    if (s_early_init_done == 0U)
    {
        if (watchdog_supervisor_early_init() != RT_EOK)
        {
            return -RT_ERROR;
        }
    }
    if (!watchdog_h7r7_iwdg_is_running())
    {
        return -RT_ERROR;
    }

    s_supervisor_start_tick = rt_tick_get();
    return watchdog_rtt_supervisor_start(&config);
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
WATCHDOG_NOINLINE static void watchdog_test_record(uint32_t test_id)
{
    rt_base_t irq_level = watchdog_record_lock();
    watchdog_record_append_event(&g_watchdog_retained_record, rt_tick_get(),
                                 WATCHDOG_EVENT_HEALTH_STALE,
                                 0x54455354U, test_id,
                                 WATCHDOG_CALLER_PC(), watchdog_current_sp());
    watchdog_record_unlock(irq_level);
}

static void watchdog_test_status(void)
{
    watchdog_record_header_t header;
    watchdog_runtime_context_t runtime_context;
    watchdog_first_failure_snapshot_t first_failure;
    watchdog_component_breadcrumb_t component;
    watchdog_interrupted_frame_t interrupted_frame;
    unsigned active_slot = 0U;
    uint32_t trace[8];
    uint32_t fault_valid;
    uint32_t fault_kind;
    uint32_t request_marker;
    uint32_t stage_sequence;
    uint32_t component_index;
    uint32_t context_valid;
    uint32_t frame_valid;
    uint32_t first_failure_valid;
    rt_base_t irq_level = watchdog_record_lock();
    int header_valid = watchdog_header_snapshot(&header, &active_slot);

    fault_valid = (uint32_t)watchdog_fault_valid(
        &g_watchdog_retained_record.fault);
    fault_kind = fault_valid != 0U
        ? g_watchdog_retained_record.fault.kind : 0U;
    request_marker = g_watchdog_retained_record.footer.reserved[1];
    stage_sequence = g_watchdog_retained_record.footer.stage_sequence;
    for (uint32_t index = 0U; index < 8U; ++index)
    {
        trace[index] = g_watchdog_retained_record.footer.reserved[index + 2U];
    }
    context_valid = (uint32_t)watchdog_record_context_valid(
        &g_watchdog_retained_record, &runtime_context);
    frame_valid = (uint32_t)watchdog_record_frame_latest(
        &g_watchdog_retained_record, &interrupted_frame);
    first_failure_valid = (uint32_t)watchdog_first_failure_valid(
        &first_failure);
    watchdog_record_unlock(irq_level);

    if (!header_valid)
    {
        rt_kprintf("[WDT_STATUS] invalid header\r\n");
        return;
    }
    RT_UNUSED(active_slot);
    rt_kprintf("[WDT_SAMPLE] context=10Hz feed_checkpoint_every=%lu_polls "
               "event_history=variable last_cycles=%lu max_cycles=%lu\r\n",
               (unsigned long)WATCHDOG_FEED_EVENT_INTERVAL_POLLS,
               (unsigned long)s_capture_last_cycles,
               (unsigned long)s_capture_max_cycles);
    rt_kprintf("[WDT_STATUS] boot=%lu reset_flags=0x%08lx reset_count=%lu "
               "fault_valid=%lu fault_kind=%lu stage=%lu request=0x%08lx "
               "vtor=0x%08lx shcsr=0x%08lx cfsr=0x%08lx hfsr=0x%08lx\r\n",
               (unsigned long)header.boot_sequence,
               (unsigned long)header.reset_flags,
               (unsigned long)header.reset_count,
               (unsigned long)fault_valid,
               (unsigned long)fault_kind,
               (unsigned long)stage_sequence,
               (unsigned long)request_marker,
               (unsigned long)SCB->VTOR,
               (unsigned long)SCB->SHCSR,
               (unsigned long)SCB->CFSR,
               (unsigned long)SCB->HFSR);
    rt_kprintf("[WDT_TRACE] %08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx\r\n",
               (unsigned long)trace[0], (unsigned long)trace[1],
               (unsigned long)trace[2], (unsigned long)trace[3],
               (unsigned long)trace[4], (unsigned long)trace[5],
               (unsigned long)trace[6], (unsigned long)trace[7]);
    if (context_valid != 0U)
    {
        if ((runtime_context.health_flags &
             WATCHDOG_CONTEXT_FAULT_REGS_VALID) != 0U)
        {
            rt_kprintf("[WDT_FAULT_CONTEXT] seq=%lu tick=%lu raw_pc=0x%08lx "
                       "lr=0x%08lx pc_valid=%lu psp=0x%08lx msp=0x%08lx "
                       "exc=0x%08lx ipsr=%lu flags=0x%08lx\r\n",
                       (unsigned long)runtime_context.sequence,
                       (unsigned long)runtime_context.tick,
                       (unsigned long)runtime_context.pc,
                       (unsigned long)runtime_context.lr,
                       (unsigned long)((runtime_context.health_flags &
                           WATCHDOG_CONTEXT_PC_VALID) != 0U),
                       (unsigned long)runtime_context.psp,
                       (unsigned long)runtime_context.msp,
                       (unsigned long)runtime_context.exc_return,
                       (unsigned long)runtime_context.ipsr,
                       (unsigned long)runtime_context.health_flags);
            rt_kprintf("[WDT_FAULT_STATUS] CFSR=0x%08lx HFSR=0x%08lx "
                       "MMFAR=0x%08lx BFAR=0x%08lx SHCSR=0x%08lx\r\n",
                       (unsigned long)runtime_context.cfsr,
                       (unsigned long)runtime_context.hfsr,
                       (unsigned long)runtime_context.mmfar,
                       (unsigned long)runtime_context.bfar,
                       (unsigned long)runtime_context.shcsr);
            rt_kprintf("[WDT_FAULT_HW] VTOR=0x%08lx DFSR=0x%08lx "
                       "DEMCR=0x%08lx DHCSR=0x%08lx ICSR=0x%08lx "
                       "ICACHE_CR=0x%08lx ICACHE_SR=0x%08lx "
                       "XSPI2_CR=0x%08lx XSPI2_DCR2=0x%08lx "
                       "XSPI2_SR=0x%08lx\r\n",
                       (unsigned long)runtime_context.thread,
                       (unsigned long)runtime_context.thread_name[0],
                       (unsigned long)runtime_context.thread_name[1],
                       (unsigned long)runtime_context.thread_name[2],
                       (unsigned long)runtime_context.thread_name[3],
                       (unsigned long)runtime_context.ltdc_frames,
                       (unsigned long)runtime_context.ltdc_reloads,
                       (unsigned long)runtime_context.gpu2d_errors,
                       (unsigned long)runtime_context.dma2d_errors,
                       (unsigned long)runtime_context.thread_state);
        }
        else
        {
            const char *name = (const char *)runtime_context.thread_name;
            rt_kprintf("[WDT_CONTEXT] seq=%lu tick=%lu thread=0x%08lx name=%.16s "
                       "pc=0x%08lx lr=0x%08lx psp=0x%08lx msp=0x%08lx "
                       "exc=0x%08lx ipsr=%lu flags=0x%08lx\r\n",
                       (unsigned long)runtime_context.sequence,
                       (unsigned long)runtime_context.tick,
                       (unsigned long)runtime_context.thread, name,
                       (unsigned long)runtime_context.pc,
                       (unsigned long)runtime_context.lr,
                       (unsigned long)runtime_context.psp,
                       (unsigned long)runtime_context.msp,
                       (unsigned long)runtime_context.exc_return,
                       (unsigned long)runtime_context.ipsr,
                       (unsigned long)runtime_context.health_flags);
            rt_kprintf("[WDT_CONTEXT] CFSR=0x%08lx HFSR=0x%08lx MMFAR=0x%08lx "
                       "BFAR=0x%08lx LTDC=%lu/%lu GPU=%lu DMA=%lu\r\n",
                       (unsigned long)runtime_context.cfsr,
                       (unsigned long)runtime_context.hfsr,
                       (unsigned long)runtime_context.mmfar,
                       (unsigned long)runtime_context.bfar,
                       (unsigned long)runtime_context.ltdc_frames,
                       (unsigned long)runtime_context.ltdc_reloads,
                       (unsigned long)runtime_context.gpu2d_errors,
                       (unsigned long)runtime_context.dma2d_errors);
        }
    }
    if (first_failure_valid != 0U)
    {
        if ((first_failure.health_flags &
             WATCHDOG_CONTEXT_FAULT_REGS_VALID) != 0U)
        {
            rt_kprintf("[WDT_FIRST_FAILURE] reason=%lu subject=%lu tick=%lu "
                       "pc=0x%08lx lr=0x%08lx psp=0x%08lx msp=0x%08lx "
                       "exc=0x%08lx ipsr=%lu flags=0x%08lx\r\n",
                       (unsigned long)first_failure.reason,
                       (unsigned long)first_failure.subject,
                       (unsigned long)first_failure.tick,
                       (unsigned long)first_failure.pc,
                       (unsigned long)first_failure.lr,
                       (unsigned long)first_failure.psp,
                       (unsigned long)first_failure.msp,
                       (unsigned long)first_failure.exc_return,
                       (unsigned long)first_failure.ipsr,
                       (unsigned long)first_failure.health_flags);
            rt_kprintf("[WDT_FAULT_HW] VTOR=0x%08lx DFSR=0x%08lx "
                       "DEMCR=0x%08lx DHCSR=0x%08lx ICSR=0x%08lx "
                       "ICACHE_CR=0x%08lx XSPI2_CR=0x%08lx "
                       "XSPI2_DCR2=0x%08lx\r\n",
                       (unsigned long)first_failure.context_thread,
                       (unsigned long)first_failure.thread_name[0],
                       (unsigned long)first_failure.thread_name[1],
                       (unsigned long)first_failure.thread_name[2],
                       (unsigned long)first_failure.thread_name[3],
                       (unsigned long)first_failure.ltdc_frames,
                       (unsigned long)first_failure.gpu2d_errors,
                       (unsigned long)first_failure.dma2d_errors);
        }
        else
        {
            rt_kprintf("[WDT_FIRST_FAILURE] reason=%lu subject=%lu tick=%lu "
                       "age_ms=%lu context_seq=%lu context_tick=%lu "
                       "thread=0x%08lx pc=0x%08lx ipsr=%lu flags=0x%08lx\r\n",
                       (unsigned long)first_failure.reason,
                       (unsigned long)first_failure.subject,
                       (unsigned long)first_failure.tick,
                       (unsigned long)first_failure.age_ms,
                       (unsigned long)first_failure.context_sequence,
                       (unsigned long)first_failure.context_tick,
                       (unsigned long)first_failure.context_thread,
                       (unsigned long)first_failure.pc,
                       (unsigned long)first_failure.ipsr,
                       (unsigned long)first_failure.health_flags);
        }
    }
    if (frame_valid != 0U)
    {
        rt_kprintf("[WDT_FRAME] seq=%lu r0=%08lx r1=%08lx r2=%08lx r3=%08lx "
                   "r12=%08lx xpsr=%08lx matches=%lu\r\n",
                   (unsigned long)interrupted_frame.sequence,
                   (unsigned long)interrupted_frame.r0,
                   (unsigned long)interrupted_frame.r1,
                   (unsigned long)interrupted_frame.r2,
                   (unsigned long)interrupted_frame.r3,
                   (unsigned long)interrupted_frame.r12,
                   (unsigned long)interrupted_frame.xpsr,
                   (unsigned long)(context_valid != 0U &&
                       interrupted_frame.sequence == runtime_context.sequence));
    }
    for (component_index = 0U;
         component_index < WATCHDOG_COMPONENT_COUNT;
         ++component_index)
    {
        if (watchdog_record_component_valid(
                &g_watchdog_retained_record,
                (watchdog_component_id_t)component_index, &component))
        {
            rt_kprintf("[WDT_COMPONENT] id=%lu seq=%lu tick=%lu thread=0x%08lx "
                       "stage=%lu progress=%lu pc=0x%08lx sp=0x%08lx\r\n",
                       (unsigned long)component_index,
                       (unsigned long)component.sequence,
                       (unsigned long)component.tick,
                       (unsigned long)component.thread,
                       (unsigned long)component.stage,
                       (unsigned long)component.progress,
                       (unsigned long)component.pc,
                       (unsigned long)component.sp);
        }
    }
}

static void watchdog_test_cmd(int argc, char **argv)
{
    if (argc != 2)
    {
        rt_kprintf("usage: watchdog_test status|core|nofeed|supervisor|services|irqoff|fault|vtor0|hardfault|assert|halassert|stack|unexpected|error\r\n");
        return;
    }
    if (rt_strcmp(argv[1], "status") == 0)
    {
        watchdog_test_status();
    }
    else if (rt_strcmp(argv[1], "core") == 0)
    {
        s_test_mode = WATCHDOG_TEST_CORE_STALL;
        watchdog_test_record(WATCHDOG_TEST_CORE_STALL);
        rt_kprintf("[WDT_TEST] app progress suppressed; expect IWDG reset\r\n");
    }
    else if (rt_strcmp(argv[1], "nofeed") == 0)
    {
        s_test_mode = WATCHDOG_TEST_NOFEED;
        watchdog_test_record(WATCHDOG_TEST_NOFEED);
        rt_kprintf("[WDT_TEST] supervisor will stop refreshing IWDG\r\n");
    }
    else if (rt_strcmp(argv[1], "supervisor") == 0)
    {
        s_test_mode = WATCHDOG_TEST_SUPERVISOR_SPIN;
        watchdog_test_record(WATCHDOG_TEST_SUPERVISOR_SPIN);
        rt_kprintf("[WDT_TEST] supervisor will stall\r\n");
    }
    else if (rt_strcmp(argv[1], "services") == 0)
    {
        s_test_component_mask = s_component_expected_mask &
                                s_component_required_mask;
        if (s_test_component_mask == 0U)
        {
            rt_kprintf("[WDT_TEST] no required component is active\r\n");
            return;
        }
        s_test_mode = WATCHDOG_TEST_COMPONENT_STALE;
        watchdog_test_record(WATCHDOG_TEST_COMPONENT_STALE);
        rt_kprintf("[WDT_TEST] suppressing required component heartbeats; "
                   "expect per-component stale records and IWDG reset\r\n");
    }
    else if (rt_strcmp(argv[1], "irqoff") == 0)
    {
        watchdog_test_record(4U);
        rt_kprintf("[WDT_TEST] interrupts disabled; expect hardware reset\r\n");
        rt_hw_interrupt_disable();
        for (;;) { __NOP(); }
    }
    else if (rt_strcmp(argv[1], "fault") == 0)
    {
        watchdog_test_record(5U);
        rt_kprintf("[WDT_TEST] triggering UsageFault\r\n");
        __asm volatile("udf #0");
    }
    else if (rt_strcmp(argv[1], "vtor0") == 0)
    {
        watchdog_test_record(5U);
        rt_kprintf("[WDT_TEST] set VTOR=0, then trigger UsageFault; "
                   "ITCM fallback should capture it\r\n");
        SCB->VTOR = 0U;
        __DSB();
        __ISB();
        __asm volatile("udf #0");
    }
    else if (rt_strcmp(argv[1], "hardfault") == 0)
    {
        uint32_t shcsr;
        uint32_t demcr_before;
        uint32_t demcr_after;
        uint32_t dhcsr;
        uint32_t faultmask_before;
        watchdog_test_record(WATCHDOG_TEST_HARDFAULT);
        /* Clear sticky fault status so this controlled escalation has a
         * clean, attributable CFSR/HFSR record. */
        SCB->CFSR = 0xFFFFFFFFU;
        SCB->HFSR = 0xFFFFFFFFU;
        /* A debugger can set DEMCR.VC_HARDERR to halt before the HardFault
         * vector's first instruction. Disable only that vector-catch bit for
         * this controlled test so it exercises the firmware capture path. */
        demcr_before = CoreDebug->DEMCR;
        CoreDebug->DEMCR = demcr_before & ~CoreDebug_DEMCR_VC_HARDERR_Msk;
        faultmask_before = __get_FAULTMASK();
        __set_FAULTMASK(0U);
        SCB->SHCSR &= ~SCB_SHCSR_USGFAULTENA_Msk;
        __DSB();
        __ISB();
        shcsr = SCB->SHCSR;
        demcr_after = CoreDebug->DEMCR;
        dhcsr = CoreDebug->DHCSR;
        g_watchdog_retained_record.footer.reserved[2] = 0x48465453U;
        g_watchdog_retained_record.footer.reserved[3] = faultmask_before;
        g_watchdog_retained_record.footer.reserved[4] =
            __get_FAULTMASK();
        g_watchdog_retained_record.footer.reserved[5] = demcr_before;
        g_watchdog_retained_record.footer.reserved[6] = demcr_after;
        g_watchdog_retained_record.footer.reserved[7] = dhcsr;
        g_watchdog_retained_record.footer.reserved[8] = SCB->VTOR;
        g_watchdog_retained_record.footer.reserved[9] =
            *(volatile const uint32_t *)(uintptr_t)(SCB->VTOR + 12U);
        g_watchdog_retained_record.footer.reserved[10] = shcsr;
        __DSB();
        rt_kprintf("[WDT_TEST] hardfault probe vtor=0x%08lx shcsr=0x%08lx "
                   "demcr_before=0x%08lx demcr=0x%08lx dhcsr=0x%08lx "
                   "faultmask_before=%lu faultmask=%lu vector=0x%08lx\r\n",
                   (unsigned long)SCB->VTOR, (unsigned long)shcsr,
                   (unsigned long)demcr_before, (unsigned long)demcr_after,
                   (unsigned long)dhcsr,
                   (unsigned long)faultmask_before,
                   (unsigned long)__get_FAULTMASK(),
                   (unsigned long)*(volatile const uint32_t *)(uintptr_t)(
                       SCB->VTOR + 12U));
        if ((shcsr & SCB_SHCSR_USGFAULTENA_Msk) != 0U)
        {
            rt_kprintf("[WDT_TEST] UsageFault could not be disabled; probe aborted\r\n");
            return;
        }
        __asm volatile("udf #0");
    }
    else if (rt_strcmp(argv[1], "assert") == 0)
    {
        watchdog_test_record(WATCHDOG_TEST_ASSERT);
        rt_kprintf("[WDT_TEST] triggering RT-Thread assertion capture\r\n");
        RT_ASSERT(0);
    }
    else if (rt_strcmp(argv[1], "halassert") == 0)
    {
        watchdog_test_record(WATCHDOG_TEST_HAL_ASSERT);
        rt_kprintf("[WDT_TEST] triggering HAL assert_param\r\n");
        assert_param(0);
    }
    else if (rt_strcmp(argv[1], "stack") == 0)
    {
#ifdef RT_USING_OVERFLOW_CHECK
        rt_thread_t self = rt_thread_self();
        watchdog_test_record(WATCHDOG_TEST_STACK_OVERFLOW);
        if (self == RT_NULL || self->stack_addr == RT_NULL)
        {
            rt_kprintf("[WDT_TEST] no current thread stack\r\n");
            return;
        }
        rt_kprintf("[WDT_TEST] corrupting only current stack guard byte\r\n");
        *((rt_uint8_t *)self->stack_addr) = (rt_uint8_t)'!';
        rt_scheduler_stack_check(self);
#else
        rt_kprintf("[WDT_TEST] stack overflow check is disabled\r\n");
#endif
    }
    else if (rt_strcmp(argv[1], "unexpected") == 0)
    {
        watchdog_test_record(WATCHDOG_TEST_UNEXPECTED_IRQ);
        rt_kprintf("[WDT_TEST] pending the unused PVD/PVM IRQ vector\r\n");
        NVIC_ClearPendingIRQ(PVD_PVM_IRQn);
        NVIC_EnableIRQ(PVD_PVM_IRQn);
        NVIC_SetPendingIRQ(PVD_PVM_IRQn);
        for (;;) { __NOP(); }
    }
    else if (rt_strcmp(argv[1], "error") == 0)
    {
        watchdog_test_record(WATCHDOG_TEST_ERROR_HANDLER);
        rt_kprintf("[WDT_TEST] triggering Error_Handler capture\r\n");
        Error_Handler();
    }
    else
    {
        rt_kprintf("unknown watchdog test mode\r\n");
    }
}
MSH_CMD_EXPORT(watchdog_test_cmd, controlled Debug-only watchdog fault injection);
#endif
