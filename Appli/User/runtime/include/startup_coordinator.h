#ifndef WORK_STARTUP_COORDINATOR_H
#define WORK_STARTUP_COORDINATOR_H
#include <stdint.h>

typedef enum {
    BOOT_APP_ENTRY, BOOT_COMPONENTS_BEGIN, BOOT_COMPONENTS_READY,
    BOOT_DMA_BEGIN, BOOT_DMA_READY, BOOT_FONT_BEGIN, BOOT_FONT_READY,
    BOOT_DISPLAY_BEGIN, BOOT_DISPLAY_READY, BOOT_TOUCH_BEGIN,
    BOOT_TOUCH_READY, BOOT_TOUCH_FAILED, BOOT_UI_BEGIN, BOOT_UI_READY,
    BOOT_FIRST_SUBMIT, BOOT_FIRST_RELOAD, BOOT_FIRST_PRESENTED,
    BOOT_BACKLIGHT, BOOT_RADIO_BEGIN, BOOT_FIRMWARE_BEGIN,
    BOOT_FIRMWARE_READY, BOOT_RADIO_READY, BOOT_RADIO_FAILED,
    BOOT_NETWORK_READY, BOOT_USB_READY, BOOT_CONFIG_BEGIN, BOOT_CONFIG_READY, BOOT_STAGE_COUNT
} boot_stage_t;
void startup_init(void);
void startup_mark(boot_stage_t stage);
void startup_frame_submit(uint32_t address);
void startup_frame_render_begin(uint32_t cycle);
void startup_frame_handler_return(uint32_t cycle);
void startup_frame_nema_metrics(uint32_t submits, uint64_t wait_cycles,
                                uint32_t wait_failures,
                                uint32_t max_frame_cycles);
void startup_frame_render_done(void);
void startup_ltdc_reload(uint32_t address);
int startup_first_presented(void);
void startup_notify_ui(void);
void startup_wait_ui(uint32_t timeout_ms);
void startup_ui_iteration(void);
int startup_radio_active(void);
int startup_radio_allowed(void);
/* True once first presentation succeeds or the display path has failed. */
int startup_display_gate_complete(void);
/* No elapsed-time guess: the home is presented, touch reached a terminal
 * state, and radio initialization ended (IP acquisition is independent). */
int startup_preload_allowed(void);
void startup_display_failed(void);
#endif
