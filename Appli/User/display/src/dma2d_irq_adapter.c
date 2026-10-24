#include <rtthread.h>
#include "dma2d.h"
#include "dma2d_broker.h"
#include "stm32h7rsxx_hal.h"

void DMA2D_IRQHandler(void)
{
    rt_interrupt_enter();
    dma2d_broker_irq();
    rt_interrupt_leave();
}

/* The regenerated CubeMX ISR still contains a private legacy dispatch call.
 * LVGL owns DMA2D completion through the ISR notification above. */
void work_cubemx_legacy_dma2d_dispatch(void)
{
}
