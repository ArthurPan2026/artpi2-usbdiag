#include "stm32h7rsxx_hal.h"
#include "ltdc.h"
#include "ltdc_irq_adapter.h"

#include <finsh.h>
#include <rtthread.h>
#include "startup_coordinator.h"

static volatile uint32_t s_ltdc_irq_count;
static volatile uint32_t s_ltdc_reload_irq_count;
static volatile uint32_t s_ltdc_frame_count;
static volatile uint32_t s_ltdc_last_isr;
static volatile uint32_t s_ltdc_last_ier;
static volatile uint32_t s_ltdc_last_srcr;
static volatile uint32_t s_ltdc_line_phase;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static volatile uint32_t s_reload_tick[16], s_reload_ticket[16];
#endif

/* LIPCR=0 is unsafe on this LTDC revision when it is re-armed from inside
 * the line-0 ISR: the current-line comparator can remain true and immediately
 * pend LTDC_IRQn again.  Use two well-separated scan lines and alternate
 * between them.  Counting only the first marker gives one sample per panel
 * frame while the second marker ensures that the next line event is never
 * re-armed while the comparator is still active. */
#define LTDC_FRAME_LINE_A 1U
#define LTDC_FRAME_LINE_B 525U

static void ltdc_arm_line_event(uint32_t line)
{
    /* HAL_LTDC_IRQHandler has already cleared the flag for an ISR request.
     * Clear once more before enabling the next comparator window so a stale
     * LIF cannot turn into a level-triggered interrupt. */
    __HAL_LTDC_DISABLE_IT(&hltdc, LTDC_IT_LI);
    __HAL_LTDC_CLEAR_FLAG(&hltdc, LTDC_FLAG_LI);
    hltdc.Instance->LIPCR = line;
    __HAL_LTDC_ENABLE_IT(&hltdc, LTDC_IT_LI);
}

/* The generated handler remains in stm32h7rsxx_it.c, but is compiled under
 * a private name from CMake. Keep the wrapper in the RT-Thread ISR boundary
 * for any configured LTDC line event. */
void LTDC_IRQHandler(void)
{
    uint32_t isr = hltdc.Instance->ISR;
    uint32_t ier = hltdc.Instance->IER;
    rt_bool_t line_event = ((isr & LTDC_ISR_LIF) != 0U &&
                            (ier & LTDC_IER_LIE) != 0U);

    s_ltdc_irq_count++;
    s_ltdc_last_isr = isr;
    s_ltdc_last_ier = ier;
    s_ltdc_last_srcr = hltdc.Instance->SRCR;
    if ((isr & LTDC_ISR_RRIF) != 0U && (ier & LTDC_IER_RRIE) != 0U)
    {
        uint32_t reload = s_ltdc_reload_irq_count + 1U;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        s_reload_tick[reload & 15U] = (uint32_t)rt_tick_get();
        s_reload_ticket[reload & 15U] = reload;
        __DMB();
#endif
        s_ltdc_reload_irq_count = reload;
    }
    if (line_event && s_ltdc_line_phase == 0U)
    {
        s_ltdc_frame_count++;
    }

    rt_interrupt_enter();
    if((isr&LTDC_ISR_RRIF)&&(ier&LTDC_IER_RRIE))
        startup_ltdc_reload(LTDC_Layer1->CFBAR);
    HAL_LTDC_IRQHandler(&hltdc);
    if (line_event)
    {
        /* Do not re-arm line 0 from its own ISR.  Alternate between a line
         * near the top and one near the bottom of the frame. */
        s_ltdc_line_phase ^= 1U;
        ltdc_arm_line_event(s_ltdc_line_phase == 0U ?
                            LTDC_FRAME_LINE_A : LTDC_FRAME_LINE_B);
    }
    rt_interrupt_leave();
}

uint32_t ltdc_reload_count(void)
{
    return s_ltdc_reload_irq_count;
}

uint32_t ltdc_frame_count(void)
{
    return s_ltdc_frame_count;
}
int ltdc_reload_tick(uint32_t reload, uint32_t *tick)
{
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    rt_base_t level = rt_hw_interrupt_disable();
    int found = s_reload_ticket[reload & 15U] == reload;
    if (found && tick) *tick = s_reload_tick[reload & 15U];
    rt_hw_interrupt_enable(level);
    return found;
#else
    (void)reload; (void)tick;
    return 0;
#endif
}

int ltdc_frame_monitor_start(void)
{
    s_ltdc_frame_count = 0U;
    s_ltdc_line_phase = 0U;
    ltdc_arm_line_event(LTDC_FRAME_LINE_A);
    return 0;
}

static void ltdc_status_cmd(void)
{
    rt_kprintf("[LTDC] irq=%lu frames=%lu reload=%lu line=%lu\n",
               (unsigned long)s_ltdc_irq_count,
               (unsigned long)s_ltdc_frame_count,
               (unsigned long)s_ltdc_reload_irq_count,
               (unsigned long)hltdc.Instance->LIPCR);
    rt_kprintf("[LTDC] ISR=0x%08lx IER=0x%08lx SRCR=0x%08lx CPSR=0x%08lx\n",
               (unsigned long)s_ltdc_last_isr,
               (unsigned long)s_ltdc_last_ier,
               (unsigned long)s_ltdc_last_srcr,
               (unsigned long)hltdc.Instance->CPSR);
    rt_kprintf("[LTDC] state=%lu error=0x%08lx\n",
               (unsigned long)HAL_LTDC_GetState(&hltdc),
               (unsigned long)hltdc.ErrorCode);
}
MSH_CMD_EXPORT(ltdc_status_cmd, show LTDC interrupt and reload status);
