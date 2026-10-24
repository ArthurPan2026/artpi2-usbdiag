#include "usb_host_port.h"

#include <rtthread.h>

#include "stm32h7rsxx.h"
#include "stm32h7rsxx_hal.h"
#include "usb_otg.h"
#include "usbh_core.h"
#include "usb_evidence.h"
#include "usb_descriptor_store.h"

static rt_bool_t s_usb_host_started;

int usb_host_start(void)
{
    int ret;
    extern HCD_HandleTypeDef hhcd_USB_OTG_HS;

    if (s_usb_host_started)
    {
        return RT_EOK;
    }

    ret = usb_descriptor_store_init();
    if (ret != RT_EOK)
    {
        rt_kprintf("[USB] descriptor store init failed: %d\r\n", ret);
        return ret;
    }

    ret = usb_evidence_start();
    if (ret != RT_EOK)
    {
        rt_kprintf("[USB] evidence start failed: %d\r\n", ret);
        return ret;
    }

    rt_kprintf("[USB] CherryUSB HS host init, base=0x%08lx\r\n",
               (unsigned long)USB_OTG_HS_PERIPH_BASE);

    /*
     * Boot leaves interrupts disabled until RT-Thread starts.  CubeMX's
     * generated MX_USB_OTG_HS_HCD_Init() is therefore mapped to a no-op in
     * CMake and the real HAL HCD initialization is performed here, from a
     * scheduled thread, where its HAL_Delay/TIM7 wait is valid.  The generated
     * HAL_MspInit() has already enabled the H7RS USB HS regulator.
     */
    ret = HAL_HCD_Init(&hhcd_USB_OTG_HS);
    rt_kprintf("[USB] HAL HCD init status=%d state=%lu GRSTCTL=0x%08lx PWR_CSR2=0x%08lx\r\n",
               ret,
               (unsigned long)hhcd_USB_OTG_HS.State,
               (unsigned long)USB_OTG_HS->GRSTCTL,
               (unsigned long)PWR->CSR2);
    if (ret != HAL_OK)
    {
        rt_kprintf("[USB] HAL HCD initialization failed\r\n");
        return -RT_ERROR;
    }

    ret = usbh_initialize(0U, (uintptr_t)USB_OTG_HS_PERIPH_BASE);
    if (ret < 0)
    {
        rt_kprintf("[USB] host init failed: %d\r\n", ret);
        return ret;
    }

    s_usb_host_started = RT_TRUE;
    rt_kprintf("[USB] CherryUSB HS host started\r\n");
    return RT_EOK;
}
