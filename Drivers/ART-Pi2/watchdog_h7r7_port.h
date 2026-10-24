/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ARTPI2_WATCHDOG_H7R7_PORT_H
#define ARTPI2_WATCHDOG_H7R7_PORT_H

#include <stdint.h>

#include "watchdog_record.h"

/* ART-Pi2 / STM32H7R7 retained-memory and external-XIP log geometry. */
#define WATCHDOG_BKPSRAM_BASE              0x38800000UL
#define WATCHDOG_BKPSRAM_SIZE              WATCHDOG_RECORD_SIZE
#define WATCHDOG_FLASH_OFFSET              0x03FBF000UL
#define WATCHDOG_FLASH_MAPPED_BASE         0x73FBF000UL
#define WATCHDOG_FLASH_SIZE                0x00041000UL
#define WATCHDOG_FLASH_SLOT_SIZE           WATCHDOG_RECORD_SIZE
#define WATCHDOG_FLASH_SECTOR_COUNT        65U
#define WATCHDOG_FLASH_SLOT_COUNT          64U

/* Two transactional trace banks sit immediately before the unchanged
 * watchdog journal. Each bank uses eight 4-KiB sectors. */
#define WATCHDOG_TRACE_FLASH_OFFSET        0x03FAF000UL
#define WATCHDOG_TRACE_FLASH_MAPPED_BASE   0x73FAF000UL
#define WATCHDOG_TRACE_FLASH_BANK_SIZE     0x00008000UL
#define WATCHDOG_TRACE_FLASH_BANK_COUNT    2U
#define WATCHDOG_TRACE_FLASH_SIZE          0x00010000UL
#define WATCHDOG_TRACE_FLASH_SECTOR_SIZE   0x00001000UL

#define WATCHDOG_RESET_STREAK_LIMIT        3U
#define WATCHDOG_RESET_WINDOW_SECONDS      300U

/* STM32H7R7 RCC_RSR flags used by the portable reset/erase gate below. Keep
 * these values aligned with stm32h7r7xx.h; compile-time checks live in the
 * hardware adapter. RCC reset-source flags are sticky until RMVF is written,
 * while SBS_MESR.MEF is reset by a later system reset. */
#define WATCHDOG_H7R7_RESET_FLAG_OBL        0x00020000UL
#define WATCHDOG_H7R7_RESET_FLAG_BOR        0x00200000UL
#define WATCHDOG_H7R7_RESET_FLAG_PIN        0x00400000UL
#define WATCHDOG_H7R7_RESET_FLAG_POR        0x00800000UL
#define WATCHDOG_H7R7_RESET_FLAG_SOFTWARE   0x01000000UL
#define WATCHDOG_H7R7_RESET_FLAG_IWDG       0x04000000UL
#define WATCHDOG_H7R7_RESET_FLAG_WWDG       0x10000000UL
#define WATCHDOG_H7R7_RESET_FLAG_LOW_POWER  0x40000000UL

typedef enum watchdog_fault_kind
{
    WATCHDOG_FAULT_MEMMANAGE = 1U,
    WATCHDOG_FAULT_BUS = 2U,
    WATCHDOG_FAULT_USAGE = 3U,
    WATCHDOG_FAULT_HARD = 4U,
    WATCHDOG_FAULT_ASSERT = 5U,
    WATCHDOG_FAULT_ERROR_HANDLER = 6U,
    WATCHDOG_FAULT_BOOT_ERROR = 7U,
    WATCHDOG_FAULT_STACK_OVERFLOW = 8U,
    WATCHDOG_FAULT_NMI = 9U,
    WATCHDOG_FAULT_UNEXPECTED = 10U
} watchdog_fault_kind_t;

int watchdog_h7r7_iwdg_is_running(void);
void watchdog_h7r7_iwdg_refresh(void);
int watchdog_h7r7_iwdg_start_or_refresh(void);
void watchdog_h7r7_record_fault(volatile watchdog_retained_record_t *record,
                                uint32_t kind,
                                uint32_t exc_return, uint32_t cfsr,
                                uint32_t hfsr, uint32_t mmfar, uint32_t bfar,
                                uint32_t frame_address,
                                uint32_t basic_frame_address,
                                uint32_t psp, uint32_t msp,
                                uint32_t source_id, uint32_t tick,
                                uint32_t cycles, uint32_t primask,
                                uint32_t basepri, uint32_t control,
                                const uint32_t *frame,
                                uint32_t file_address,
                                uint32_t function_address,
                                uint32_t expression_address,
                                uint32_t caller_address, uint32_t line);

uint32_t watchdog_h7r7_reset_causes(uint32_t reset_flags);
int watchdog_h7r7_tamper_source_requests_erase(int source_enabled,
                                               int tamper_flag_set,
                                               int potential_mode,
                                               int iwdg_reset);

/*
 * Decide whether SBS_MESR.MEF=0 still means a backup-RAM erase is pending.
 * RCC_RSR flags accumulate until RMVF is written; after POR, a later software,
 * software/watchdog, option-byte, or low-power reset also leaves PORRSTF set,
 * but resets MEF to zero. PINRSTF and BORRSTF are excluded because RM0477
 * reports them together with a fresh POR. This inline policy is host-testable
 * without STM32 HAL.
 */
static inline int watchdog_h7r7_backup_erase_wait_required(
    uint32_t reset_flags, int tamper_erase_triggered, int erase_complete)
{
    const uint32_t secondary_reset_flags =
        WATCHDOG_H7R7_RESET_FLAG_OBL |
        WATCHDOG_H7R7_RESET_FLAG_SOFTWARE |
        WATCHDOG_H7R7_RESET_FLAG_IWDG |
        WATCHDOG_H7R7_RESET_FLAG_WWDG |
        WATCHDOG_H7R7_RESET_FLAG_LOW_POWER;
    const int unambiguous_por =
        (reset_flags & WATCHDOG_H7R7_RESET_FLAG_POR) != 0U &&
        (reset_flags & secondary_reset_flags) == 0U;
    const int erase_requested = unambiguous_por || tamper_erase_triggered != 0;

    return erase_requested != 0 && erase_complete == 0 ? 1 : 0;
}

#endif
