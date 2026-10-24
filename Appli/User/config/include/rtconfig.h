#ifndef WORK_APPLI_RTCONFIG_H
#define WORK_APPLI_RTCONFIG_H

/* Official SDIO/MMCSD object names are longer than the RT default 12-byte
 * limit; 16 keeps those names intact and improves msh diagnostics. */
#define RT_NAME_MAX 16
#define RT_CPUS_NR 1
#define RT_ALIGN_SIZE 8
#define RT_THREAD_PRIORITY_32
#define RT_THREAD_PRIORITY_MAX 32
#define RT_TICK_PER_SECOND 1000
#define RT_VER_NUM 0x50300

#define RT_USING_OVERFLOW_CHECK
#define RT_BACKTRACE_LEVEL_MAX_NR 32
#define RT_USING_HOOK
#define RT_USING_IDLE_HOOK
#define RT_USING_DEBUG
/* Preserve fault assertions in Debug and Release without enabling the many
 * optional function-pointer hooks in release builds. */
#define RT_DEBUGING_ASSERT
#define WORK_WATCHDOG_STACK_OVERFLOW_HOOK
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
/* Optional profiling callbacks remain Debug-only. */
#define RT_HOOK_USING_FUNC_PTR
#endif
#define RT_USING_SEMAPHORE
#define RT_USING_MUTEX
#define RT_USING_EVENT
#define RT_USING_MAILBOX
#define RT_USING_MESSAGEQUEUE
#define RT_USING_MESSAGEQUEUE_PRIORITY
#define RT_USING_TIMER_SOFT
#define RT_TIMER_THREAD_PRIO 4
#define RT_TIMER_THREAD_STACK_SIZE 2048
#define RT_USING_DEVICE
#define RT_USING_COMPONENTS_INIT
#define RT_USING_DEVICE_IPC
#define RT_USING_SYSTEM_WORKQUEUE
#define RT_SYSTEM_WORKQUEUE_STACKSIZE 2048
#define RT_SYSTEM_WORKQUEUE_PRIORITY 23
#define RT_USING_HEAP

/* The H7RS DMA pool is itself an official RT-Thread 5.3 memheap.  Keep that
 * primitive available for every A/B profile, while selecting the allocator
 * used by rt_malloc/rt_free from CMake.  0=Small Memory, 1=Memheap,
 * 2=SLAB.  A plain non-CMake build intentionally falls back to Memheap. */
#ifndef WORK_RTTHREAD_ALLOCATOR
#define WORK_RTTHREAD_ALLOCATOR 1
#endif
#define RT_USING_MEMHEAP
#if WORK_RTTHREAD_ALLOCATOR == 0
#define RT_USING_SMALL_MEM
#define RT_USING_SMALL_MEM_AS_HEAP
#elif WORK_RTTHREAD_ALLOCATOR == 1
#define RT_USING_MEMHEAP_AS_HEAP
#elif WORK_RTTHREAD_ALLOCATOR == 2
#define RT_USING_SLAB
#define RT_USING_SLAB_AS_HEAP
#else
#error "WORK_RTTHREAD_ALLOCATOR must be 0 (smallmem), 1 (memheap), or 2 (slab)"
#endif
#define RT_USING_CACHE

/* Project-local port of RT-Thread's generic DMA memory/channel API.  The
 * classic Cortex-M build uses the H7RS fixed-region backend rather than the
 * Smart/Device-Model MMU backend. */
#define RT_USING_DMA
/*
 * This is a single-core Cortex-M7 application.  The generic RT-Thread
 * ARM hardware implementation uses an LDREX/STREX retry even for an atomic
 * load.  Under the sustained SDIO interrupt load that retry can starve
 * indefinitely because an exception clears the exclusive monitor.  Use the
 * single-core critical-section implementation instead; it is bounded and
 * preserves the required atomic semantics for the kernel.
 */
#define RT_USING_CPU_FFS

#define RT_USING_I2C
#define RT_USING_PIN

/* Mongoose owns the CYW43438 station, DHCP, TCP/IP and HTTP service. WiFi
 * credentials are supplied only by LCD or saved internal-flash profiles. */
#define WIFI_AUTO_START_WEB

/* Infineon WHD port for the CYW43438 SDIO device.  The
 * CYW43438 is a 2.4 GHz 802.11n/WiFi4 radio; these names are consumed by the
 * project-local CY HAL compatibility layer, not by CubeMX-generated code. */
#define WHD_USING_CHIP_CYW43438
#define CYBSP_USING_PIN_NAME
#define CYBSP_REG_ON_PIN_NAME "PM.2"
#define CYBSP_HOST_WAKE_IRQ_PIN_NAME "PC.5"
#define CYBSP_HOST_WAKE_IRQ_EVENT_FALL
#define CYBSP_OOB_INTR_PRIORITY 5
/* HOST_WAKE is attached to RT-Thread's EXTI5 by the project-local WHD GPIO
 * port.  The port keeps only a low-rate missed-edge watchdog as a fallback. */
#define CY_WIFI_WHD_THREAD_PRIORITY 8
#define CY_WIFI_WHD_THREAD_STACK_SIZE 8192

/* CherryUSB Host is built locally from the ART-Pi2 SDK package. */
#define PKG_USING_CHERRYUSB
#define PKG_CHERRYUSB_HOST
#define PKG_CHERRYUSB_HOST_DWC2_ST
#define PKG_CHERRYUSB_HOST_CDC_ACM
#define PKG_CHERRYUSB_HOST_HID
#define PKG_CHERRYUSB_HOST_MSC


#define RT_USING_CONSOLE
#define RT_CONSOLEBUF_SIZE 128
#define RT_CONSOLE_DEVICE_NAME "uart4"

/* FinSH/MSH console. UART4 is registered by the project-local HAL adapter. */
#define RT_USING_FINSH
#define RT_USING_MSH
#define FINSH_USING_MSH
#define FINSH_USING_SYMTAB
#define MSH_USING_BUILT_IN_COMMANDS
#define FINSH_THREAD_NAME "tshell"
#define FINSH_THREAD_PRIORITY 20
#define FINSH_THREAD_STACK_SIZE 4096
#define FINSH_USING_HISTORY
#define FINSH_HISTORY_LINES 5
#define FINSH_CMD_SIZE 80
#define FINSH_USING_DESCRIPTION
#define FINSH_ARG_MAX 10
#define FINSH_USING_OPTION_COMPLETION

#define IDLE_THREAD_STACK_SIZE 1024

#define ARCH_ARM
#define ARCH_ARM_CORTEX_M
#define ARCH_ARM_CORTEX_M7
#define SOC_FAMILY_STM32
#define SOC_SERIES_STM32H7RS
#define SOC_STM32H7R7

#endif
