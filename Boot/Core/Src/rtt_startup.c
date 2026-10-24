#include <rtthread.h>
#include "main.h"
#include "rtt_console.h"

extern void SystemCoreClockUpdate(void);
extern uint32_t SystemCoreClock;

#define STM32_RAM_BEGIN        0x24000000UL
#define STM32_RAM_SIZE         0x4FC00UL   /* __RAM_SIZE (AXI SRAM) */
#define STM32_MSP_RESERVE_SIZE 0x2000U

static void app_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);
    rt_kprintf("app thread starting...\r\n");
    while (1)
    {
        rt_kprintf("app tick\r\n");
        rt_thread_mdelay(1000);
    }
}

void rt_hw_board_init(void)
{
    extern int _end;

    SystemCoreClockUpdate();
    SysTick_Config(SystemCoreClock / RT_TICK_PER_SECOND);

#ifdef RT_USING_HEAP
    rt_system_heap_init((void *)&_end,
                        (void *)(STM32_RAM_BEGIN + STM32_RAM_SIZE - STM32_MSP_RESERVE_SIZE));
#endif
}

int rtthread_startup(void)
{
    rt_hw_interrupt_disable();

    rt_hw_board_init();
    rt_system_timer_init();
    rt_system_scheduler_init();

    RT_ASSERT(rtt_console_init() == RT_EOK);
    rt_show_version();

    /* finsh/msh thread is created by components init (INIT_APP_EXPORT) */
    rt_components_init();

    rt_thread_t tid = rt_thread_create("app",
                                       app_thread_entry,
                                       RT_NULL,
                                       2048,
                                       10,
                                       20);
    RT_ASSERT(tid != RT_NULL);
    rt_thread_startup(tid);

    rt_thread_idle_init();
    rt_thread_defunct_init();

    rt_system_scheduler_start();

    return 0;
}
