#ifndef ARTPI2_WATCHDOG_SUPERVISOR_H
#define ARTPI2_WATCHDOG_SUPERVISOR_H

#include <stdint.h>
#include "watchdog_record.h"

#define WATCHDOG_BOOT_OPERATION_COMPONENTS  1U
#define WATCHDOG_BOOT_OPERATION_USB_START  2U
#define WATCHDOG_COMPONENT_INIT_TIMEOUT_MS  120000U

/* Call once after the MPU/cache map is installed and before peripheral
 * initialization begins. It opens BKPSRAM and records the application build. */
int watchdog_supervisor_early_init(void);

/* Create the sole RT-Thread IWDG feeder before starting application workers. */
int watchdog_supervisor_start(void);

void watchdog_supervisor_boot_phase_begin(uint32_t operation_id,
                                          uint32_t timeout_ms);
void watchdog_supervisor_operation_progress(uint32_t stage,
                                            uint32_t value);
void watchdog_supervisor_operation_end(uint32_t result);
void watchdog_supervisor_core_ready(void);
void watchdog_supervisor_core_progress(uint32_t progress_id);

/* The component API records producer progress in volatile staging and commits
 * a bounded snapshot from SysTick. Only explicitly required components take
 * part in the reset decision; optional/idle workers remain forensic context. */
void watchdog_supervisor_component_expect(watchdog_component_id_t component,
                                          int required);
void watchdog_supervisor_component_progress(
    watchdog_component_id_t component, uint32_t stage, uint32_t progress);
void watchdog_supervisor_display_expect(void);

/* Called at the application-owned SysTick entry with the original exception
 * token and stack pointers, before RT-Thread advances its tick. */
void watchdog_supervisor_systick_capture(uint32_t exc_return,
                                         uint32_t psp, uint32_t msp,
                                         uint32_t ipsr, uint32_t primask,
                                         uint32_t basepri, uint32_t faultmask,
                                         uint32_t control);

/* Lightweight 1-kHz PC/LR recorder. It must run before any RTOS or logging
 * work and only trusts a validated thread-mode PSP frame. */
void watchdog_supervisor_trace_systick_capture(uint32_t exc_return,
                                              uint32_t psp, uint32_t msp,
                                              uint32_t ipsr);

/* Used by fault handlers which cannot depend on RT-Thread or console output. */
void watchdog_supervisor_record_fault(uint32_t kind,
                                      uint32_t exc_return,
                                      const uint32_t *frame,
                                      uint32_t thread_id,
                                      uint32_t frame_address,
                                      uint32_t basic_frame_address,
                                      uint32_t file_address,
                                      uint32_t function_address,
                                      uint32_t expression_address,
                                      uint32_t caller_address,
                                      uint32_t line);

/* Assertion path: persist the source metadata plus a bounded stack snapshot
 * before requesting reset. The first eight words are stored in fault.frame;
 * the remaining words use a CRC/commit-protected sidecar. */
void watchdog_supervisor_record_panic_fault(
    uint32_t thread_id, const uint32_t *stack, uint32_t stack_words,
    uint32_t captured_sp, uint32_t file_address, uint32_t function_address,
    uint32_t expression_address, uint32_t caller_address, uint32_t line);

#endif
