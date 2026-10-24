/*
 * Project-owned DMA2D service.
 *
 * The LVGL direct framebuffer path does not use DMA2D for its flush.  This
 * service owns DMA2D for non-LVGL blit/fill/conversion jobs and serializes all
 * callers through one RT-Thread mutex.
 */
#ifndef DMA2D_SERVICE_H
#define DMA2D_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int dma2d_service_init(void);

int dma2d_service_fill_rgb565(uint16_t *dst, uint32_t dst_stride_pixels,
                              uint16_t color, uint32_t width, uint32_t height);

int dma2d_service_blit_rgb565(const uint16_t *src, uint32_t src_stride_pixels,
                              uint16_t *dst, uint32_t dst_stride_pixels,
                              uint32_t width, uint32_t height);

/**
 * DMA2D profiling counters.  total_bytes is the estimated traffic generated
 * by the service (R2M: destination write; M2M: source read plus destination
 * write), not a silicon bus-monitor reading.
 */
typedef struct {
    uint32_t frames;
    uint32_t active_frames;
    uint32_t jobs;
    uint32_t errors;
    uint32_t max_frame_cycles;
    uint32_t last_job_cycles;
    uint64_t total_cycles;
    uint64_t total_bytes;
} dma2d_service_stats_t;

void dma2d_service_frame_begin(void);
void dma2d_service_frame_end(void);
void dma2d_service_stats_reset(void);
void dma2d_service_stats_get(dma2d_service_stats_t *stats);

uint32_t dma2d_service_error_count(void);
uint32_t dma2d_service_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* DMA2D_SERVICE_H */
