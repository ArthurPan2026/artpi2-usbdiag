/* SPDX-License-Identifier: Apache-2.0 */
#include "watchdog_h7r7_port.h"

#include <stdio.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

int main(void)
{
    const uint32_t por = WATCHDOG_H7R7_RESET_FLAG_POR;
    const uint32_t fresh_por =
        por | WATCHDOG_H7R7_RESET_FLAG_PIN | WATCHDOG_H7R7_RESET_FLAG_BOR;

    CHECK(watchdog_h7r7_backup_erase_wait_required(fresh_por, 0, 0) == 1);
    CHECK(watchdog_h7r7_backup_erase_wait_required(fresh_por, 0, 1) == 0);
    CHECK(fresh_por == 0x00E00000UL);
    CHECK(watchdog_h7r7_backup_erase_wait_required(0x00E00000UL, 0, 0) == 1);
    CHECK(watchdog_h7r7_backup_erase_wait_required(
              por | WATCHDOG_H7R7_RESET_FLAG_SOFTWARE, 0, 0) == 0);
    CHECK(watchdog_h7r7_backup_erase_wait_required(
              fresh_por | WATCHDOG_H7R7_RESET_FLAG_SOFTWARE, 0, 0) == 0);
    CHECK(watchdog_h7r7_backup_erase_wait_required(0x01E00000UL, 0, 0) == 0);
    CHECK(watchdog_h7r7_backup_erase_wait_required(
              fresh_por | WATCHDOG_H7R7_RESET_FLAG_IWDG, 0, 0) == 0);
    CHECK(watchdog_h7r7_backup_erase_wait_required(0U, 0, 0) == 0);
    CHECK(watchdog_h7r7_backup_erase_wait_required(0U, 1, 0) == 1);
    CHECK(watchdog_h7r7_backup_erase_wait_required(0U, 1, 1) == 0);

    (void)puts("PASS H7R7 backup erase gate distinguishes a fresh POR from sticky reset flags");
    return 0;
}
