#ifndef ARTPI2_BOOT_WATCHDOG_FLASH_LOG_H
#define ARTPI2_BOOT_WATCHDOG_FLASH_LOG_H

#include <stdint.h>

typedef enum boot_watchdog_stage
{
    BOOT_WATCHDOG_STAGE_MAIN_ERROR = 1U,
    BOOT_WATCHDOG_STAGE_OPTION_BYTES,
    BOOT_WATCHDOG_STAGE_XSPI1,
    BOOT_WATCHDOG_STAGE_XSPI2,
    BOOT_WATCHDOG_STAGE_FLASH_COMMAND,
    BOOT_WATCHDOG_STAGE_FLASH_MAP,
    BOOT_WATCHDOG_STAGE_PSRAM_INIT,
    BOOT_WATCHDOG_STAGE_PSRAM_TEST
} boot_watchdog_stage_t;

int boot_watchdog_prepare(uint32_t reset_flags);
int boot_watchdog_reset_streak(void);
int boot_watchdog_recovery_required(void);
int boot_watchdog_flash_log_flush(void);
void boot_watchdog_recovery_loop(void);
void boot_watchdog_record_error(uint32_t stage, int32_t status);
void boot_watchdog_record_assert(uint32_t file_address, uint32_t line);
/* Called directly from naked exception veneers; it records into BKPSRAM and
 * fail-stops so the independent IWDG performs the reset. */
void boot_watchdog_exception_capture(const uint32_t *frame,
                                     uint32_t exc_return,
                                     uint32_t ipsr)
    __attribute__((noreturn));

#endif
