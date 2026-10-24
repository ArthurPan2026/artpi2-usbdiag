#include <rthw.h>

#include "stm32h7rsxx_hal.h"

/* RT-Thread's default implementation is an empty weak function.  Use the
 * UART4 registers directly so fault output also works before CubeMX has
 * initialized the Application huart4 handle.  Boot leaves UART4 configured
 * at 115200 baud and the same peripheral/pins are shared with Application. */
void rt_hw_console_output(const char *string)
{
    if (string == RT_NULL || UART4 == NULL)
    {
        return;
    }

    while (*string != '\0')
    {
        uint32_t guard = 1000000U;
        while ((UART4->ISR & USART_ISR_TXE_TXFNF) == 0U && guard-- != 0U)
        {
        }
        if (guard == 0U)
        {
            return;
        }

        UART4->TDR = (uint8_t)*string++;
    }
}
