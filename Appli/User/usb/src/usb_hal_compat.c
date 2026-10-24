#include "stm32h7rsxx_hal.h"

/* CubeMX emits MX_USB_OTG_HS_HCD_Init() for the HAL HCD stack.  The
 * application uses CherryUSB's direct DWC2 host driver instead, so the
 * generated wrapper must not perform a second USB core initialization before
 * RT-Thread starts.  Its generated MSP routine remains the authoritative
 * clock/NVIC setup and is invoked by usb_glue_st.c when CherryUSB starts. */
HAL_StatusTypeDef work_hal_hcd_init_noop(HCD_HandleTypeDef *hcd)
{
    (void)hcd;
    return HAL_OK;
}
