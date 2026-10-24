/*
 * GPU2D interrupt ownership for the RT-Thread application.
 *
 * CubeMX keeps generating the HAL handlers in Core/Src/stm32h7rsxx_it.c.
 * CMake renames those generated entry points and this file owns the public
 * vectors so NemaGFX completion callbacks execute inside the RT-Thread ISR
 * boundary.  No RT-Thread API is called from the GPU2D error callback except
 * for the Nema completion notification, which is ISR-safe.
 */
#include "gpu2d.h"
#include "gpu2d_irq_adapter.h"
#include "lvgl_nema_port.h"
#include "stm32h7rsxx_hal.h"
#include <nema_graphics.h>
#include <nema_hal.h>

#include <finsh.h>
#include <rtthread.h>

static volatile uint32_t s_gpu2d_error_count;
static volatile uint32_t s_gpu2d_last_error;

void GPU2D_IRQHandler(void)
{
    rt_interrupt_enter();
    HAL_GPU2D_IRQHandler(&hgpu2d);
    rt_interrupt_leave();
}

void GPU2D_ER_IRQHandler(void)
{
    rt_interrupt_enter();
    HAL_GPU2D_ER_IRQHandler(&hgpu2d);
    rt_interrupt_leave();
}

/* The default STM32 HAL implementation intentionally loops forever.  A
 * recoverable application must wake the Nema waiter and leave diagnostics
 * for the normal thread context instead. */
void HAL_GPU2D_ErrorCallback(GPU2D_HandleTypeDef *gpu2d)
{
    uint32_t error_code = nema_reg_read(GPU2D_SYS_INTERRUPT);
    uint32_t hal_error = HAL_GPU2D_GetError(gpu2d);

    /* Clear the latched system interrupt/error source first. */
    nema_reg_write(GPU2D_SYS_INTERRUPT, error_code);

    /* Recover the external-memory/cache hold states used by H7RS. */
    if ((error_code & (1UL << 2)) != 0U)
    {
        HAL_ICACHE_Disable();
        nema_ext_hold_deassert_imm(2U);
    }
    if ((error_code & (1UL << 3)) != 0U)
    {
        HAL_ICACHE_Enable();
        HAL_ICACHE_Invalidate();
        nema_ext_hold_deassert_imm(3U);
    }

    if (error_code == 0U)
    {
        error_code = hal_error;
    }

    s_gpu2d_last_error = error_code;
    s_gpu2d_error_count++;
    lvgl_nema_gpu2d_error_isr(error_code);
}

uint32_t gpu2d_error_count(void)
{
    return s_gpu2d_error_count;
}

uint32_t gpu2d_last_error(void)
{
    return s_gpu2d_last_error;
}

static void gpu2d_status_cmd(void)
{
    int32_t offset = -1;
    int32_t submission_id = -1;
    uint32_t flags = 0U;
    int32_t internal_submitted_id = -1;
    int32_t internal_executed_id = -1;
    uintptr_t buffer = 0U;
    int32_t buffer_size = 0;
    int32_t task_type = -1;
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
    uint32_t submit_attempts = 0U;
    uint32_t submit_returns = 0U;
    uint32_t wait_failures = 0U;

    (void)lvgl_nema_command_list_state(&offset, &submission_id, &flags,
                                       &internal_submitted_id, &internal_executed_id,
                                       &buffer, &buffer_size);
    (void)lvgl_nema_task_state(&task_type, &x1, &y1, &x2, &y2,
                               &submit_attempts, &submit_returns, &wait_failures);

    rt_kprintf("[GPU2D] errors=%lu last=0x%08lx state=%lu completed=%lu cl=%ld\n",
               (unsigned long)s_gpu2d_error_count,
               (unsigned long)s_gpu2d_last_error,
               (unsigned long)HAL_GPU2D_GetState(&hgpu2d),
               (unsigned long)lvgl_nema_completion_count(),
               (long)lvgl_nema_last_command_list());
    rt_kprintf("[GPU2D] CL buf=0x%08lx size=%ld offset=%ld submit=%ld flags=0x%08lx "
               "internal=%ld/%ld\n",
               (unsigned long)buffer, (long)buffer_size, (long)offset,
               (long)submission_id, (unsigned long)flags,
               (long)internal_submitted_id, (long)internal_executed_id);
    rt_kprintf("[GPU2D] task=%ld area=%ld,%ld..%ld,%ld submits=%lu/%lu waiterr=%lu\n",
               (long)task_type, (long)x1, (long)y1, (long)x2, (long)y2,
               (unsigned long)submit_attempts, (unsigned long)submit_returns,
               (unsigned long)wait_failures);
    rt_kprintf("[GPU2D] regs ITCTRL=0x%08lx CLID=0x%08lx BREAK=0x%08lx SYS=0x%08lx\n",
               (unsigned long)HAL_GPU2D_ReadRegister(&hgpu2d, GPU2D_ITCTRL),
               (unsigned long)HAL_GPU2D_ReadRegister(&hgpu2d, GPU2D_CLID),
               (unsigned long)HAL_GPU2D_ReadRegister(&hgpu2d, GPU2D_BREAKPOINT),
               (unsigned long)HAL_GPU2D_ReadRegister(&hgpu2d, GPU2D_SYS_INTERRUPT));
    if(buffer != 0U && buffer_size >= 16 && offset > 0)
    {
        volatile const uint32_t * words = (volatile const uint32_t *)buffer;
        int32_t count = offset > 16 ? 16 : offset;
        rt_kprintf("[GPU2D] CL words");
        for(int32_t i = 0; i < count; i++)
        {
            rt_kprintf(" %08lx", (unsigned long)words[i]);
        }
        rt_kprintf("\n");
    }
}
MSH_CMD_EXPORT(gpu2d_status_cmd, show GPU2D status);
