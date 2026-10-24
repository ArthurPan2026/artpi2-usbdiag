/* SPDX-License-Identifier: Apache-2.0 */
#include "watchdog_h7r7_port.h"

#include <string.h>

#include "stm32h7rsxx.h"

#if RCC_RSR_OBLRSTF != WATCHDOG_H7R7_RESET_FLAG_OBL || \
    RCC_RSR_BORRSTF != WATCHDOG_H7R7_RESET_FLAG_BOR || \
    RCC_RSR_PINRSTF != WATCHDOG_H7R7_RESET_FLAG_PIN || \
    RCC_RSR_PORRSTF != WATCHDOG_H7R7_RESET_FLAG_POR || \
    RCC_RSR_SFTRSTF != WATCHDOG_H7R7_RESET_FLAG_SOFTWARE || \
    RCC_RSR_IWDGRSTF != WATCHDOG_H7R7_RESET_FLAG_IWDG || \
    RCC_RSR_WWDGRSTF != WATCHDOG_H7R7_RESET_FLAG_WWDG || \
    RCC_RSR_LPWRRSTF != WATCHDOG_H7R7_RESET_FLAG_LOW_POWER
#error "Portable watchdog reset policy flags must match STM32H7R7 RCC_RSR."
#endif

#define WATCHDOG_IWDG_START_KEY   0xCCCCU
#define WATCHDOG_IWDG_RELOAD_KEY  0xAAAAU
#define WATCHDOG_IWDG_UNLOCK_KEY  0x5555U
#define WATCHDOG_IWDG_PRESCALER   6U
#define WATCHDOG_IWDG_RELOAD      0x0FFFU
#define WATCHDOG_IWDG_UPDATE_MASK (IWDG_SR_PVU | IWDG_SR_RVU)

int watchdog_h7r7_iwdg_is_running(void)
{
    return (IWDG->SR & IWDG_SR_ONF) != 0U;
}

void watchdog_h7r7_iwdg_refresh(void)
{
    IWDG->KR = WATCHDOG_IWDG_RELOAD_KEY;
    __DSB();
}

int watchdog_h7r7_iwdg_start_or_refresh(void)
{
    uint32_t spin = 0U;

    /* If a previous IWDG reset left the peripheral running, first give Boot
     * the full current interval before it performs any external-memory work. */
    if (watchdog_h7r7_iwdg_is_running())
    {
        watchdog_h7r7_iwdg_refresh();
    }

    IWDG->KR = WATCHDOG_IWDG_START_KEY;
    IWDG->KR = WATCHDOG_IWDG_UNLOCK_KEY;
    IWDG->PR = WATCHDOG_IWDG_PRESCALER;
    IWDG->RLR = WATCHDOG_IWDG_RELOAD;
    while ((IWDG->SR & WATCHDOG_IWDG_UPDATE_MASK) != 0U &&
           spin < 1000000U)
    {
        ++spin;
    }
    if ((IWDG->SR & WATCHDOG_IWDG_UPDATE_MASK) != 0U)
    {
        return -1;
    }
    IWDG->KR = WATCHDOG_IWDG_RELOAD_KEY;
    __DSB();

#if defined(DBGMCU_APB4FZR_IWDG)
    /* Keep fault-injection and production timing identical while a debugger
     * is attached. A long breakpoint intentionally expires the watchdog. */
    DBGMCU->APB4FZR &= ~DBGMCU_APB4FZR_IWDG;
#endif
    __DSB();
    return 0;
}

uint32_t watchdog_h7r7_reset_causes(uint32_t reset_flags)
{
    uint32_t causes = 0U;
    const uint32_t watchdog_flags = RCC_RSR_IWDGRSTF | RCC_RSR_WWDGRSTF;
    const uint32_t non_pin_other_flags = RCC_RSR_OBLRSTF | RCC_RSR_BORRSTF |
                                         RCC_RSR_LPWRRSTF;

    if ((reset_flags & watchdog_flags) != 0U)
    {
        causes |= WATCHDOG_RESET_CAUSE_WATCHDOG;
    }
    if ((reset_flags & RCC_RSR_SFTRSTF) != 0U)
    {
        causes |= WATCHDOG_RESET_CAUSE_SOFTWARE;
    }
    if ((reset_flags & RCC_RSR_PORRSTF) != 0U)
    {
        causes |= WATCHDOG_RESET_CAUSE_POWER_ON;
    }
    if ((reset_flags & non_pin_other_flags) != 0U ||
        (((reset_flags & RCC_RSR_PINRSTF) != 0U) &&
         ((reset_flags & RCC_RSR_SFTRSTF) == 0U)))
    {
        /* PINRSTF is ignored only when paired with SFTRSTF: that pairing is
         * observed on the ART-Pi2 reset path and must not mask a committed
         * software fault-reset request. */
        causes |= WATCHDOG_RESET_CAUSE_OTHER;
    }
    return causes;
}

int watchdog_h7r7_tamper_source_requests_erase(int source_enabled,
                                               int tamper_flag_set,
                                               int potential_mode,
                                               int iwdg_reset)
{
    if (source_enabled == 0 || tamper_flag_set == 0)
    {
        return 0;
    }
    /* Confirmed-mode tampers erase immediately. In potential mode the
     * hardware erases if software fails to clear the flag before an IWDG
     * reset. */
    return (potential_mode == 0 || iwdg_reset != 0) ? 1 : 0;
}

void watchdog_h7r7_record_fault(volatile watchdog_retained_record_t *record,
                                uint32_t kind,
                                uint32_t exc_return, uint32_t cfsr,
                                uint32_t hfsr, uint32_t mmfar, uint32_t bfar,
                                uint32_t frame_address,
                                uint32_t basic_frame_address,
                                uint32_t psp, uint32_t msp,
                                uint32_t source_id, uint32_t tick,
                                uint32_t cycles, uint32_t primask,
                                uint32_t basepri, uint32_t control,
                                const uint32_t *frame,
                                uint32_t file_address,
                                uint32_t function_address,
                                uint32_t expression_address,
                                uint32_t caller_address, uint32_t line)
{
    watchdog_fault_context_t context;
    uint32_t index;

    memset(&context, 0, sizeof(context));
    context.return_token = exc_return;
    context.status[0] = cfsr;
    context.status[1] = hfsr;
    context.status[2] = mmfar;
    context.status[3] = bfar;
    context.frame_address = frame_address;
    context.basic_frame_address = basic_frame_address;
    context.stack_pointer[0] = psp;
    context.stack_pointer[1] = msp;
    context.source_id = source_id;
    context.tick = tick;
    context.cycle_counter = cycles;
    context.control[0] = primask;
    context.control[1] = basepri;
    context.control[2] = control;
    context.source_location[0] = file_address;
    context.source_location[1] = function_address;
    context.source_location[2] = expression_address;
    context.source_location[3] = caller_address;
    context.source_location[4] = line;
    if (frame != NULL)
    {
        for (index = 0U; index < 8U; ++index)
        {
            context.frame[index] = frame[index];
        }
    }
    watchdog_record_fault(record, kind, &context);
}
