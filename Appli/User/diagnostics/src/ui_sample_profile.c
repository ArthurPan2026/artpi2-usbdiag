#include "ui_sample_profile.h"
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS && !defined(WORK_UI_HOST)
#include <rtthread.h>
#include "stm32h7rsxx_hal.h"
#include <string.h>
static volatile unsigned s_enabled, s_samples, s_missed;
static volatile uint32_t s_pcs[128], s_counts[128];
void ui_sample_profile_systick(void)
{
    if (!s_enabled || rt_interrupt_get_nest()) return;
    rt_thread_t thread = rt_thread_self();
    if (!thread || strncmp(thread->parent.name, "lvgl", 4)) return;
    uintptr_t sp = __get_PSP();
    if ((sp & 3) || sp < 0x24000000U || sp + 104 > 0x24060000U) { s_missed++; return; }
    const uint32_t *stack = (const uint32_t *)sp;
    uint32_t pc = stack[6], psr = stack[7];
    if (pc < 0x70000000U || pc >= 0x74000000U || !(psr & (1U << 24))) {
        pc = stack[24]; psr = stack[25];
    }
    if (pc < 0x70000000U || pc >= 0x74000000U || !(psr & (1U << 24))) { s_missed++; return; }
    pc &= ~63U;
    s_samples++;
    for (unsigned i = 0; i < 128; i++) {
        if (!s_pcs[i] || s_pcs[i] == pc) { s_pcs[i] = pc; s_counts[i]++; return; }
    }
    s_missed++;
}
static void ui_sample_profile_cmd(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "start")) {
        rt_base_t level = rt_hw_interrupt_disable();
        s_enabled = 0; s_samples = s_missed = 0;
        memset((void *)s_pcs, 0, sizeof(s_pcs));
        memset((void *)s_counts, 0, sizeof(s_counts));
        s_enabled = 1;
        rt_hw_interrupt_enable(level);
        rt_kprintf("[UI_PC] started thread=lvgl method=SysTick_exception_PSP_frame\r\n");
        return;
    }
    s_enabled = 0;
    rt_kprintf("[UI_PC] samples=%u missed=%u buckets=64byte\r\n", s_samples, s_missed);
    for (unsigned i = 0; i < 128; i++)
        if (s_counts[i]) rt_kprintf("[UI_PC] pc=0x%08lX n=%lu\r\n", (unsigned long)s_pcs[i], (unsigned long)s_counts[i]);
}
MSH_CMD_EXPORT(ui_sample_profile_cmd, Debug-only bounded sampling of UI interrupted PCs);
#else
void ui_sample_profile_systick(void) {}
#endif
