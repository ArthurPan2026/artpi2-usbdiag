#include <stdint.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <rtthread.h>

#include "stm32h7rsxx_hal.h"
#include "wifi_nv_layout.h"
#include "watchdog_h7r7_port.h"
#include "watchdog_supervisor.h"
#include "watchdog_record.h"
#include "work_assert_capture.h"

/*
 * Keep a tiny fault record in internal SRAM.  This handler is project-owned
 * through the CubeMX ISR symbol remap in CMake; the generated Core file is
 * left untouched.  It is intentionally independent of RT-Thread and the
 * console so that a fault can be inspected with SWD even when the scheduler
 * or the UART is no longer usable.
 */
typedef struct
{
    uint32_t valid;
    uint32_t exc_return;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t frame_address;
    uint32_t basic_frame_address;
    uint32_t psp;
    uint32_t msp;
    uint32_t cpu_current_thread;
    uint32_t frame[8];
} work_fault_record_t;

__attribute__((section(".fault_capture"), aligned(32), used))
volatile work_fault_record_t g_work_memmanage_fault;

__attribute__((section(".fault_capture"), aligned(32), used))
volatile work_fault_record_t g_work_exception_fault;

typedef struct work_wifi_nv_read_guard
{
    volatile uint32_t active;
    volatile uint32_t expected_address;
    volatile uint32_t fault_pc;
    volatile uint32_t fixup_pc;
    volatile int32_t result;
} work_wifi_nv_read_guard_t;

static work_wifi_nv_read_guard_t g_work_wifi_nv_read_guard;
static uint32_t s_fault_handlers_installed;

#define WORK_FAULT_DTCM_BEGIN       0x20000000UL
#define WORK_FAULT_DTCM_END         0x20010000UL
#define WORK_FAULT_AXI_BEGIN        0x24000000UL
#define WORK_FAULT_AXI_END          0x24072000UL
#define WORK_CFSR_STACK_ERROR_MASK  ((1UL << 3) | (1UL << 4) | (1UL << 5) | \
                                     (1UL << 11) | (1UL << 12) | (1UL << 13))

extern int work_wifi_nv_flash_load_word(const volatile uint32_t *source,
                                        uint32_t *value);
extern void work_wifi_nv_flash_faulting_load(void);
extern void work_wifi_nv_flash_load_fixup(void);

__asm__(
    ".syntax unified\n"
    ".thumb\n"
    ".align 2\n"
    ".global work_wifi_nv_flash_load_word\n"
    ".type work_wifi_nv_flash_load_word, %function\n"
    ".thumb_func\n"
    "work_wifi_nv_flash_load_word:\n"
    ".global work_wifi_nv_flash_faulting_load\n"
    ".type work_wifi_nv_flash_faulting_load, %function\n"
    ".thumb_func\n"
    "work_wifi_nv_flash_faulting_load:\n"
    "ldr r2, [r0]\n"
    "str r2, [r1]\n"
    "movs r0, #0\n"
    "bx lr\n"
    ".size work_wifi_nv_flash_load_word, .-work_wifi_nv_flash_load_word\n"
    ".align 2\n"
    ".global work_wifi_nv_flash_load_fixup\n"
    ".type work_wifi_nv_flash_load_fixup, %function\n"
    ".thumb_func\n"
    "work_wifi_nv_flash_load_fixup:\n"
    "bx lr\n"
    ".size work_wifi_nv_flash_load_fixup, .-work_wifi_nv_flash_load_fixup\n");

static void work_fault_store(volatile work_fault_record_t *record,
                             const uint32_t *frame, uint32_t exc_return,
                             uint32_t valid)
{
    const uint32_t *basic_frame = NULL;
    uintptr_t frame_start = (uintptr_t)frame;
    uintptr_t frame_end;
    uintptr_t ram_end;
    size_t frame_size = 0U;
    uint32_t i;

    /* Never dereference a possibly damaged exception frame. On an invalid
     * frame retain the fault registers and raw SP values, but omit its words. */
    if (frame != NULL && (frame_start & 3U) == 0U &&
        (exc_return & 0xFFFFFFE0U) == 0xFFFFFFE0U &&
        (SCB->CFSR & WORK_CFSR_STACK_ERROR_MASK) == 0U)
    {
        frame_size = (exc_return & (1UL << 4)) != 0U ? 32U : 104U;
        if (frame_start >= WORK_FAULT_AXI_BEGIN &&
            frame_start <= WORK_FAULT_AXI_END - frame_size)
        {
            ram_end = WORK_FAULT_AXI_END;
        }
        else if (frame_start >= WORK_FAULT_DTCM_BEGIN &&
                 frame_start <= WORK_FAULT_DTCM_END - frame_size)
        {
            ram_end = WORK_FAULT_DTCM_END;
        }
        else
        {
            ram_end = 0U;
        }
        frame_end = frame_start + frame_size;
        if (ram_end != 0U && frame_end <= ram_end)
        {
            basic_frame = frame +
                (((exc_return & (1UL << 4)) == 0U) ? 18U : 0U);
        }
    }
    else if (frame != NULL && (frame_start & 3U) == 0U &&
             (exc_return & 0xFFFFFFE0U) != 0xFFFFFFE0U &&
             (SCB->CFSR & WORK_CFSR_STACK_ERROR_MASK) == 0U)
    {
        /* RT-Thread's HardFault hook receives its already-unwrapped basic
         * frame, rather than EXC_RETURN. Accept only a complete internal-RAM
         * basic frame. */
        if ((frame_start >= WORK_FAULT_AXI_BEGIN &&
             frame_start <= WORK_FAULT_AXI_END - 32U) ||
            (frame_start >= WORK_FAULT_DTCM_BEGIN &&
             frame_start <= WORK_FAULT_DTCM_END - 32U))
        {
            basic_frame = frame;
        }
    }

    record->valid = valid;
    record->exc_return = exc_return;
    record->cfsr = SCB->CFSR;
    record->hfsr = SCB->HFSR;
    record->mmfar = SCB->MMFAR;
    record->bfar = SCB->BFAR;
    record->frame_address = (uint32_t)frame;
    record->basic_frame_address = (uint32_t)(uintptr_t)basic_frame;
    record->psp = __get_PSP();
    record->msp = __get_MSP();
    /* Avoid touching RT-Thread scheduler state in a fault handler. */
    record->cpu_current_thread = 0U;

    for (i = 0U; basic_frame != NULL && i < 8U; i++)
    {
        record->frame[i] = basic_frame[i];
    }

    __DSB();
}

static __attribute__((noreturn)) void
work_fault_capture(volatile work_fault_record_t *record,
                   const uint32_t *frame, uint32_t exc_return,
                   uint32_t valid)
{
    const uint32_t *basic_frame;
    watchdog_fault_kind_t fault_kind = WATCHDOG_FAULT_HARD;

    if (valid == 0x4D4D4631U)
    {
        fault_kind = WATCHDOG_FAULT_MEMMANAGE;
    }
    else if (valid == 0x42464631U)
    {
        fault_kind = WATCHDOG_FAULT_BUS;
    }
    else if (valid == 0x55464631U)
    {
        fault_kind = WATCHDOG_FAULT_USAGE;
    }
    else if (valid == 0x4E4D4931U)
    {
        fault_kind = WATCHDOG_FAULT_NMI;
    }
    else if (valid == 0x554E4531U)
    {
        fault_kind = WATCHDOG_FAULT_UNEXPECTED;
    }
    work_fault_store(record, frame, exc_return, valid);
    basic_frame = (const uint32_t *)(uintptr_t)record->basic_frame_address;
    watchdog_supervisor_record_fault(
        (uint32_t)fault_kind, exc_return, basic_frame,
        record->cpu_current_thread,
        (uint32_t)(uintptr_t)frame, (uint32_t)(uintptr_t)basic_frame,
        0U, 0U, 0U, (uint32_t)(uintptr_t)__builtin_return_address(0), 0U);
    while (1)
    {
        __NOP();
    }
}

void work_memmanage_capture(const uint32_t *frame, uint32_t exc_return)
{
    work_fault_capture(&g_work_memmanage_fault, frame, exc_return,
                       0x4D4D4631U); /* "MMF1" */
}

static uint32_t *work_fault_basic_frame(const uint32_t *frame,
                                        uint32_t exc_return)
{
    uint32_t *basic_frame = (uint32_t *)frame;

    if ((exc_return & 0xFFFFFFE0U) == 0xFFFFFFE0U &&
        (exc_return & (1UL << 4)) == 0U)
    {
        basic_frame += 18U;
    }
    return basic_frame;
}

static int work_wifi_nv_try_busfault_recovery(const uint32_t *frame,
                                             uint32_t exc_return)
{
    const uint32_t busfault_mask = 0xFF00U;
    const uint32_t precise_read_fault =
        SCB_CFSR_BFARVALID_Msk | SCB_CFSR_PRECISERR_Msk;
    uint32_t *basic_frame;
    uint32_t cfsr;
    uint32_t bfar;
    uint32_t stacked_pc;
    uint32_t end_address = WIFI_NV_FLASH_BASE_ADDRESS +
                           (WIFI_NV_FLASH_SECTOR_COUNT * WIFI_NV_FLASH_SECTOR_SIZE);

    if (frame == NULL || g_work_wifi_nv_read_guard.active == 0U)
    {
        return 0;
    }
    uintptr_t frame_start = (uintptr_t)frame;
    size_t frame_bytes = (exc_return & (1U << 4)) ? 32U : 104U;
    if ((frame_start & 3U) || (exc_return & 0xFFFFFFE0U) != 0xFFFFFFE0U ||
        !((frame_start >= 0x24000000U && frame_start + frame_bytes <= 0x24072000U) ||
          (frame_start >= 0x20000000U && frame_start + frame_bytes <= 0x20010000U)))
        return 0;

    cfsr = SCB->CFSR;
    bfar = SCB->BFAR;
    basic_frame = work_fault_basic_frame(frame, exc_return);
    stacked_pc = basic_frame[6];

    if ((cfsr & busfault_mask) != precise_read_fault ||
        (cfsr & ~busfault_mask) != 0U ||
        bfar != g_work_wifi_nv_read_guard.expected_address ||
        bfar < WIFI_NV_FLASH_BASE_ADDRESS || bfar >= end_address ||
        (stacked_pc & ~1UL) != (g_work_wifi_nv_read_guard.fault_pc & ~1UL) ||
        (FLASH->ISR & FLASH_FLAG_DBECCERR) == 0U ||
        (FLASH->ECCDFADDR & ~15UL) != (bfar & ~15UL) ||
        (basic_frame[7] & (1UL << 24)) == 0U)
    {
        return 0;
    }

    g_work_wifi_nv_read_guard.active = 0U;
    g_work_wifi_nv_read_guard.result = -EIO;
    basic_frame[0] = (uint32_t)-EIO;
    basic_frame[6] = g_work_wifi_nv_read_guard.fixup_pc & ~1UL;

    /* H7RS reports flash double-bit ECC through BusFault; the HAL handler
     * clears the ECC record and preserves the normal interrupt configuration. */
    HAL_FLASHEx_ECCD_IRQHandler();
    SCB->CFSR = precise_read_fault;
    __DSB();
    __ISB();
    return 1;
}

__attribute__((noinline))
int work_wifi_nv_guarded_flash_read(uint32_t address, uint8_t *data,
                                    size_t length)
{
    uint32_t offset;
    uint32_t value;
    int read_result;
    uint32_t end_address = WIFI_NV_FLASH_BASE_ADDRESS +
                           (WIFI_NV_FLASH_SECTOR_COUNT * WIFI_NV_FLASH_SECTOR_SIZE);

    if (data == NULL || length == 0U || (address & 3U) != 0U ||
        (length & 3U) != 0U || address < WIFI_NV_FLASH_BASE_ADDRESS ||
        address > end_address || length > (size_t)(end_address - address) ||
        g_work_wifi_nv_read_guard.active != 0U)
    {
        return -EIO;
    }

    for (offset = 0U; offset < length; offset += sizeof(value))
    {
        const volatile uint32_t *source =
            (const volatile uint32_t *)(uintptr_t)(address + offset);

        if ((FLASH->ISR & FLASH_FLAG_DBECCERR) != 0U)
        {
            return -EIO;
        }

        g_work_wifi_nv_read_guard.expected_address = address + offset;
        g_work_wifi_nv_read_guard.fault_pc =
            (uint32_t)(uintptr_t)work_wifi_nv_flash_faulting_load;
        g_work_wifi_nv_read_guard.fixup_pc =
            (uint32_t)(uintptr_t)work_wifi_nv_flash_load_fixup;
        g_work_wifi_nv_read_guard.result = 0;
        __DMB();
        g_work_wifi_nv_read_guard.active = 1U;
        __DSB();

        read_result = work_wifi_nv_flash_load_word(source, &value);

        g_work_wifi_nv_read_guard.active = 0U;
        __DMB();
        if (read_result != 0 || g_work_wifi_nv_read_guard.result != 0)
        {
            return -EIO;
        }
        memcpy(data + offset, &value, sizeof(value));
    }
    return 0;
}

void work_busfault_capture(const uint32_t *frame, uint32_t exc_return)
{
    if (work_wifi_nv_try_busfault_recovery(frame, exc_return))
    {
        return;
    }
    work_fault_capture(&g_work_exception_fault, frame, exc_return,
                       0x42464631U); /* "BFF1" */
}

void work_usagefault_capture(const uint32_t *frame, uint32_t exc_return)
{
    work_fault_capture(&g_work_exception_fault, frame, exc_return,
                       0x55464631U); /* "UFF1" */
}

__attribute__((noreturn)) void
work_hardfault_capture(const uint32_t *frame, uint32_t exc_return)
{
    work_fault_capture(&g_work_exception_fault, frame, exc_return,
                       0x48464631U); /* "HFF1" */
}

/* Keep HardFault capture independent of RT-Thread's fault hook. The vector
 * veneer passes the architectural exception frame and EXC_RETURN directly to
 * the SRAM-only recorder, which commits BKPSRAM before requesting reset. */
__attribute__((naked, noreturn, used)) void work_hardfault_entry(void)
{
    __asm volatile(
        /* Leave a first-instruction breadcrumb in non-cacheable SRAM. This
         * distinguishes failure to vector into this handler from a later
         * capture/retained-record failure, even if the C path cannot run. */
        "ldr r2, =g_work_exception_fault\n"
        "ldr r3, =0x48464631\n"
        "str r3, [r2]\n"
        "dsb sy\n"
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b work_hardfault_capture\n");
}

__attribute__((noreturn)) void
work_unexpected_exception_capture(const uint32_t *frame,
                                 uint32_t exc_return, uint32_t ipsr)
{
    work_fault_capture(&g_work_exception_fault, frame, exc_return,
                       ipsr == 2U ? 0x4E4D4931U : 0x554E4531U);
}

__attribute__((naked, noinline)) void MemManage_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b work_memmanage_capture\n");
}

__attribute__((naked, noinline)) void BusFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b work_busfault_capture\n");
}

__attribute__((naked, noinline)) void UsageFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b work_usagefault_capture\n");
}

void work_fault_install(void)
{
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    uint32_t index;
#endif

    if (s_fault_handlers_installed != 0U)
    {
        return;
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    /* This NOLOAD scratch survives resets. Boot has already persisted the
     * previous abnormal record, so clear it before the diagnostic run to keep
     * an old frame from looking like a new capture. */
    for (index = 0U;
         index < (uint32_t)(sizeof(work_fault_record_t) / sizeof(uint32_t));
         ++index)
    {
        ((volatile uint32_t *)&g_work_memmanage_fault)[index] = 0U;
        ((volatile uint32_t *)&g_work_exception_fault)[index] = 0U;
    }
    __DSB();
#endif
    work_assert_capture_init();
    s_fault_handlers_installed = 1U;
}
