#include <stdint.h>
#include <stddef.h>
#include <rtthread.h>
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#include <finsh.h>
#endif
#include "stm32h7rsxx_hal.h"
#include "build_identity.h"
#include "work_assert_capture.h"
#include "watchdog_h7r7_port.h"
#include "watchdog_supervisor.h"
#include "watchdog_record.h"

#define WORK_PANIC_MAGIC 0x50414E31U
#define WORK_PANIC_VERSION 1U
#define WORK_PANIC_STACK_WORDS 96U

typedef struct
{
    uint32_t valid;
    uint32_t version;
    uint32_t kind; /* 1=LVGL, 2=RT-Thread */
    uint32_t line;
    uint32_t file;
    uint32_t function;
    uint32_t expression;
    uint32_t caller;
    uint32_t thread;
    uint32_t tick;
    uint32_t cycles;
    uint32_t ipsr;
    uint32_t primask;
    uint32_t basepri;
    uint32_t control;
    uint32_t msp;
    uint32_t psp;
    uint32_t captured_sp;
    uint32_t stack_words;
    char source_fingerprint[65];
    char thread_name[RT_NAME_MAX + 1];
    uint32_t stack[WORK_PANIC_STACK_WORDS];
    uint32_t checksum;
} work_panic_record_t;

__attribute__((section(".fault_capture"), aligned(32), used))
volatile work_panic_record_t g_work_panic_current;
__attribute__((section(".fault_capture"), aligned(32), used))
volatile work_panic_record_t g_work_panic_previous;

static uint32_t work_panic_checksum(const volatile work_panic_record_t *record)
{
    const volatile uint8_t *bytes = (const volatile uint8_t *)record;
    uint32_t hash = 2166136261U;
    size_t index;
    for (index = offsetof(work_panic_record_t, version);
         index < offsetof(work_panic_record_t, checksum); ++index)
    {
        hash = (hash ^ bytes[index]) * 16777619U;
    }
    return hash;
}

static uint32_t s_assert_hook_installed;

static int work_panic_valid(const volatile work_panic_record_t *record)
{
    return record->valid == WORK_PANIC_MAGIC &&
           record->version == WORK_PANIC_VERSION &&
           record->stack_words <= WORK_PANIC_STACK_WORDS &&
           record->checksum == work_panic_checksum(record);
}

static __attribute__((noreturn, noinline)) void
work_panic_capture(uint32_t kind, const char *file, const char *function,
                   const char *expression, uint32_t line, uint32_t caller)
{
    volatile work_panic_record_t *record = &g_work_panic_current;
    rt_thread_t thread = rt_thread_self();
    uint32_t sp;
    uint32_t end = 0U;
    uint32_t index;
    uint32_t saved_primask = __get_PRIMASK();

    /* Make the first failing location durable before nested interrupts or
     * console output can provoke a second assertion. Restore the caller's
     * interrupt state after the record has committed. */
    __disable_irq();
    if (work_panic_valid(record))
    {
        __set_PRIMASK(saved_primask);
        while (1) { __NOP(); }
    }
    __asm volatile("mov %0, sp" : "=r"(sp));
    record->valid = 0U;
    rt_memset((void *)record, 0, sizeof(*record));
    record->version = WORK_PANIC_VERSION;
    record->kind = kind;
    record->line = line;
    record->file = (uint32_t)(uintptr_t)file;
    record->function = (uint32_t)(uintptr_t)function;
    record->expression = (uint32_t)(uintptr_t)expression;
    record->caller = caller;
    record->thread = (uint32_t)(uintptr_t)thread;
    record->tick = rt_tick_get();
    record->cycles = DWT->CYCCNT;
    record->ipsr = __get_IPSR();
    record->primask = saved_primask;
    record->basepri = __get_BASEPRI();
    record->control = __get_CONTROL();
    record->msp = __get_MSP();
    record->psp = __get_PSP();
    record->captured_sp = sp;
    rt_memcpy((void *)record->source_fingerprint, WORK_SOURCE_FINGERPRINT, 64U);

    if ((uintptr_t)thread >= 0x24000000U &&
        (uintptr_t)thread + sizeof(*thread) <= 0x24072000U)
    {
        rt_memcpy((void *)record->thread_name, thread->parent.name, RT_NAME_MAX);
    }
    if (sp >= 0x24000000U && sp < 0x24072000U)
    {
        end = 0x24072000U;
    }
    else if (sp >= 0x20000000U && sp < 0x20010000U)
    {
        end = 0x20010000U;
    }
    if (end > sp && (sp & 3U) == 0U)
    {
        uint32_t count = (end - sp) / sizeof(uint32_t);
        const volatile uint32_t *stack = (const volatile uint32_t *)(uintptr_t)sp;
        if (count > WORK_PANIC_STACK_WORDS) count = WORK_PANIC_STACK_WORDS;
        for (index = 0U; index < count; ++index) record->stack[index] = stack[index];
        record->stack_words = count;
    }
    record->checksum = work_panic_checksum(record);
    __DMB();
    record->valid = WORK_PANIC_MAGIC;
    __DSB();
    watchdog_supervisor_record_panic_fault(
        (uint32_t)(uintptr_t)thread, (const uint32_t *)record->stack,
        record->stack_words, sp, record->file, record->function,
        record->expression, record->caller, line);
    /* The panic recorder is fail-stop and requests reset after its retained
     * evidence has been committed. This line is unreachable on success. */
    __set_PRIMASK(saved_primask);

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    rt_kprintf("[PANIC] kind=%lu line=%lu caller=0x%08lx\n",
               (unsigned long)kind, (unsigned long)line, (unsigned long)caller);
    rt_kprintf("[PANIC] thread=%s ipsr=%lu tick=%lu\n",
               record->thread_name, (unsigned long)record->ipsr,
               (unsigned long)record->tick);
    rt_kprintf("[PANIC] recorded in noncacheable SRAM; fail-stop\n");
#else
    RT_UNUSED(kind);
    RT_UNUSED(caller);
#endif
    while (1) { __NOP(); }
}

__attribute__((noreturn, noinline)) void
work_lvgl_assert_capture(const char *file, const char *function, uint32_t line)
{
    work_panic_capture(1U, file, function, RT_NULL, line,
                       (uint32_t)(uintptr_t)__builtin_return_address(0));
}

static __attribute__((noreturn, noinline)) void
work_rt_assert_capture(const char *expression, const char *function, rt_size_t line)
{
    work_panic_capture(2U, RT_NULL, function, expression, (uint32_t)line,
                       (uint32_t)(uintptr_t)__builtin_return_address(0));
}

void work_assert_capture_init(void)
{
    if (s_assert_hook_installed != 0U)
    {
        return;
    }
    if (work_panic_valid(&g_work_panic_current))
    {
        rt_memcpy((void *)&g_work_panic_previous,
                  (const void *)&g_work_panic_current, sizeof(g_work_panic_current));
        __DSB();
    }
    g_work_panic_current.valid = 0U;
    __DSB();
    rt_assert_set_hook(work_rt_assert_capture);
    s_assert_hook_installed = 1U;
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static void work_panic_print(const char *label,
                             const volatile work_panic_record_t *record)
{
    if (!work_panic_valid(record))
    {
        rt_kprintf("[PANIC] %s: no valid captured assertion\n", label);
        return;
    }
    rt_kprintf("[PANIC] %s kind=%lu line=%lu checksum=0x%08lx\n", label,
               (unsigned long)record->kind, (unsigned long)record->line,
               (unsigned long)record->checksum);
    rt_kprintf("[PANIC] thread=%s tick=%lu ipsr=%lu\n", record->thread_name,
               (unsigned long)record->tick, (unsigned long)record->ipsr);
    rt_kprintf("[PANIC] caller=0x%08lx file=0x%08lx func=0x%08lx\n",
               (unsigned long)record->caller, (unsigned long)record->file,
               (unsigned long)record->function);
    rt_kprintf("[PANIC] expr=0x%08lx SP=0x%08lx words=%lu\n",
               (unsigned long)record->expression, (unsigned long)record->captured_sp,
               (unsigned long)record->stack_words);
    rt_kprintf("[PANIC] PRIMASK=%lu BASEPRI=%lu CONTROL=%lu\n",
               (unsigned long)record->primask, (unsigned long)record->basepri,
               (unsigned long)record->control);
    rt_kprintf("[PANIC] source=%.64s\n", record->source_fingerprint);
}

static void panic_status_cmd(void)
{
    work_panic_print("current", &g_work_panic_current);
    work_panic_print("previous", &g_work_panic_previous);
}
MSH_CMD_EXPORT(panic_status_cmd, Show retained Debug assertion records);
#endif

