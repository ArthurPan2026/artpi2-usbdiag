/* Project-owned GPU2D diagnostics shared with the LVGL profiler. */
#ifndef GPU2D_IRQ_ADAPTER_H
#define GPU2D_IRQ_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t gpu2d_error_count(void);
uint32_t gpu2d_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* GPU2D_IRQ_ADAPTER_H */
