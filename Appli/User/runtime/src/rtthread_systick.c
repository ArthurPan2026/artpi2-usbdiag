#include <rtthread.h>
#include "watchdog_supervisor.h"
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#include "lvgl_isr_repro.h"
#include "ui_sample_profile.h"
#endif

/* SysTick belongs exclusively to RT-Thread.  HAL time is advanced by TIM7
 * through app_hal_timebase_tim.c. */
void SysTick_Handler(void)
{
    uint32_t exc_return;
    uint32_t psp;
    uint32_t msp;
    uint32_t ipsr;
    uint32_t primask;
    uint32_t basepri;
    uint32_t faultmask;
    uint32_t control;

    /* Capture the original exception return token before any C call can
     * replace LR. The watchdog validates PSP bounds and the hardware frame
     * before it trusts the interrupted PC. */
    __asm volatile("mov %0, lr\n"
                   "mrs %1, psp\n"
                   "mrs %2, msp\n"
                   "mrs %3, ipsr\n"
                   "mrs %4, primask\n"
                   "mrs %5, basepri\n"
                   "mrs %6, faultmask\n"
                   "mrs %7, control\n"
                   : "=r"(exc_return), "=r"(psp), "=r"(msp), "=r"(ipsr),
                     "=r"(primask), "=r"(basepri), "=r"(faultmask),
                     "=r"(control));
    watchdog_supervisor_trace_systick_capture(exc_return, psp, msp, ipsr);
    watchdog_supervisor_systick_capture(exc_return, psp, msp, ipsr,
                                        primask, basepri, faultmask, control);
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    ui_sample_profile_systick();
#endif
    rt_interrupt_enter();
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    lvgl_isr_repro_systick();
#endif
    rt_tick_increase();
    rt_interrupt_leave();
}
