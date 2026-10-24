#include "dma2d_broker.h"

#include "dma2d.h"
#include "stm32h7rsxx_hal.h"
#include "usb_perf.h"

#include <lvgl/config/lv_conf_internal.h>

#if LV_USE_DRAW_DMA2D_INTERRUPT
extern void lv_draw_dma2d_transfer_complete_interrupt_handler(void);
#endif

extern DMA2D_HandleTypeDef hdma2d;

static struct rt_mutex s_dma2d_broker_mutex;
static volatile rt_bool_t s_dma2d_broker_initialized;
static volatile dma2d_broker_owner_t s_dma2d_owner;
static volatile uint32_t s_dma2d_generation;
static volatile uint32_t s_dma2d_service_error;
static volatile uint32_t s_dma2d_lvgl_error;

int dma2d_broker_init(void)
{
    rt_err_t result;

    if(s_dma2d_broker_initialized)
        return RT_EOK;

    result = rt_mutex_init(&s_dma2d_broker_mutex, "d2dbkr", RT_IPC_FLAG_PRIO);
    if(result != RT_EOK)
        return result;

    s_dma2d_owner = DMA2D_BROKER_OWNER_NONE;
    s_dma2d_generation = 0U;
    s_dma2d_service_error = 0U;
    s_dma2d_lvgl_error = 0U;
    s_dma2d_broker_initialized = RT_TRUE;
    return RT_EOK;
}

int dma2d_broker_acquire(dma2d_broker_owner_t owner, rt_int32_t timeout)
{
    rt_err_t result;
    rt_uint32_t wait_start_cycles;

    if(owner == DMA2D_BROKER_OWNER_NONE)
        return -RT_EINVAL;
    if(!s_dma2d_broker_initialized && dma2d_broker_init() != RT_EOK)
        return -RT_ERROR;

    wait_start_cycles = usb_perf_now();
    result = rt_mutex_take(&s_dma2d_broker_mutex, timeout);
    usb_perf_record(USB_PERF_DMA2D_BROKER_WAIT, wait_start_cycles,
                    usb_perf_now());
    if(result != RT_EOK)
        return result;

    s_dma2d_owner = owner;
    s_dma2d_generation++;
    if(owner == DMA2D_BROKER_OWNER_SERVICE)
        s_dma2d_service_error = 0U;
    else
        s_dma2d_lvgl_error = 0U;
    dma2d_broker_clear_flags();
    return RT_EOK;
}

void dma2d_broker_release(dma2d_broker_owner_t owner)
{
    if(!s_dma2d_broker_initialized || s_dma2d_owner != owner)
        return;

    s_dma2d_owner = DMA2D_BROKER_OWNER_NONE;
    rt_mutex_release(&s_dma2d_broker_mutex);
}

void dma2d_broker_clear_flags(void)
{
    /* CTCIF/CTEIF/CAEIF/TWIF/TCIF/TEIF/CEIF are all cleared by this mask on
     * STM32H7RS.  Clearing before every owner starts removes stale wakes. */
    DMA2D->IFCR = 0x3FU;
}

void dma2d_broker_set_error(dma2d_broker_owner_t owner, uint32_t error)
{
    if(owner == DMA2D_BROKER_OWNER_SERVICE)
        s_dma2d_service_error = error;
    else if(owner == DMA2D_BROKER_OWNER_LVGL)
        s_dma2d_lvgl_error = error;
}

uint32_t dma2d_broker_last_error(dma2d_broker_owner_t owner)
{
    if(owner == DMA2D_BROKER_OWNER_SERVICE)
        return s_dma2d_service_error;
    if(owner == DMA2D_BROKER_OWNER_LVGL)
        return s_dma2d_lvgl_error;
    return 0U;
}

uint32_t dma2d_broker_generation(void)
{
    return s_dma2d_generation;
}

dma2d_broker_owner_t dma2d_broker_owner(void)
{
    return s_dma2d_owner;
}

void dma2d_broker_irq(void)
{
    dma2d_broker_owner_t owner = s_dma2d_owner;
    uint32_t flags = DMA2D->ISR;

    if(owner == DMA2D_BROKER_OWNER_SERVICE) {
        HAL_DMA2D_IRQHandler(&hdma2d);
        return;
    }

    if(owner == DMA2D_BROKER_OWNER_LVGL) {
        if((flags & (DMA2D_ISR_CEIF | DMA2D_ISR_TEIF)) != 0U)
            s_dma2d_lvgl_error = flags & (DMA2D_ISR_CEIF | DMA2D_ISR_TEIF);
        dma2d_broker_clear_flags();
#if LV_USE_DRAW_DMA2D_INTERRUPT
        lv_draw_dma2d_transfer_complete_interrupt_handler();
#endif
        return;
    }

    dma2d_broker_clear_flags();
}
