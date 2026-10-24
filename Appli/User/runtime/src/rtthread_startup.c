#include <rtthread.h>
#include <rtdevice.h>

#include <drv_gpio.h>

#include "stm32h7rsxx.h"

#include "rtthread_app.h"
#include "rtthread_uart4_device.h"
#include "startup_coordinator.h"
#include "watchdog_supervisor.h"

extern char __rt_heap_start;
extern char __rt_heap_end;
extern uint32_t SystemCoreClock;
extern void work_fault_install(void);

/* DWT CYCCNT is a shared application time source. Initialize it once before
 * components and worker threads start; all consumers must only read it. */
static void rt_hw_cycle_counter_init(void)
{
    rt_uint32_t first_cycle;
    volatile rt_uint32_t nop_count;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
    {
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
    __DSB();
    __ISB();

    first_cycle = DWT->CYCCNT;
    for (nop_count = 0U; nop_count < 8U; nop_count++)
    {
        __NOP();
    }
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U ||
        DWT->CYCCNT == first_cycle)
    {
        rt_hw_console_output(
            "[DWT] cycle counter did not advance; cycle timing unavailable\r\n");
    }
}

static void application_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);
    startup_init();startup_mark(BOOT_COMPONENTS_BEGIN);
    /* Component initializers legitimately delay or create threads.  Run them
     * from a real RT-Thread context after the scheduler starts instead of
     * invoking them from the pre-scheduler C caller. */
    rt_kprintf("[RTT] components initializing\r\n");
    watchdog_supervisor_boot_phase_begin(WATCHDOG_BOOT_OPERATION_COMPONENTS,
                                         WATCHDOG_COMPONENT_INIT_TIMEOUT_MS);
    rt_components_init();
    watchdog_supervisor_operation_end(0U);
    startup_mark(BOOT_COMPONENTS_READY);
    rt_kprintf("[RTT] components ready\r\n");
    rtthread_app_main();
}

void rt_hw_board_init(void)
{
    rt_err_t uart_result;

    rt_hw_console_output("[APP] board init\r\n");
    /* The final H7RS MPU/cache map is installed from the CubeMX-preserved
     * HAL_MspInit() user section, before any peripheral initialization.  Do
     * not rebuild it here: LTDC, SDMMC, GPU2D or WHD may already be active. */
    rt_hw_pin_init();
    SystemCoreClockUpdate();
    rt_hw_cycle_counter_init();
    SysTick_Config(SystemCoreClock / RT_TICK_PER_SECOND);

    /* Keep MemManage faults separate from the RT-Thread HardFault fallback.
     * fault_diagnostics.c records the stacked PC/LR in internal SRAM without
     * touching the scheduler, which makes SDIO/PSRAM corruption diagnosable
     * even when the RT-Thread hard-fault path is already compromised. */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk |
                  SCB_SHCSR_BUSFAULTENA_Msk |
                  SCB_SHCSR_USGFAULTENA_Msk;
    work_fault_install();

#ifdef RT_USING_HEAP
    rt_system_heap_init(&__rt_heap_start, &__rt_heap_end);
#endif

    uart_result = rt_hw_uart4_device_init();
    if (uart_result != RT_EOK)
    {
        rt_hw_console_output("[RTT] uart4 device register failed\r\n");
        return;
    }

    (void)rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
    if (rt_console_get_device() == RT_NULL)
    {
        rt_hw_console_output("[RTT] uart4 console open failed\r\n");
    }
}

int rtthread_startup(void)
{
    rt_hw_interrupt_disable();

    rt_hw_console_output("[RTT] startup\r\n");

    rt_hw_board_init();
    rt_show_version();
    rt_hw_console_output("[RTT] board ready\r\n");
    rt_system_timer_init();
    rt_system_scheduler_init();

    RT_ASSERT(watchdog_supervisor_start() == RT_EOK);

    /* Keep the core coordinator above optional device/service workers so a
     * single busy optional thread cannot manufacture a stale-core reset. */
    rt_thread_t thread = rt_thread_create("app", application_thread_entry,
                                          RT_NULL, 4096U, 3U, 20U);
    RT_ASSERT(thread != RT_NULL);
    rt_thread_startup(thread);
    rt_hw_console_output("[RTT] scheduler ready, app thread ready\r\n");

    rt_system_timer_thread_init();
    rt_thread_idle_init();
    rt_thread_defunct_init();
    rt_system_scheduler_start();
    return 0;
}
