/* Project-owned LTDC diagnostics shared with the LVGL profiler. */
#ifndef LTDC_IRQ_ADAPTER_H
#define LTDC_IRQ_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t ltdc_reload_count(void);
uint32_t ltdc_frame_count(void);
/* Debug presentation timing: timestamp the actual reload interrupt, not a
 * later UI poll delayed by framebuffer export or network work. */
int ltdc_reload_tick(uint32_t reload, uint32_t *tick);
int ltdc_frame_monitor_start(void);

#ifdef __cplusplus
}
#endif

#endif /* LTDC_IRQ_ADAPTER_H */
