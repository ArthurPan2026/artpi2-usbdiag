#ifndef LVGL_NEMA_PORT_H
#define LVGL_NEMA_PORT_H

#include <stdint.h>

typedef struct {
    uint32_t frames;
    uint32_t active_frames;
    uint32_t submits;
    uint32_t wait_failures;
    uint32_t max_frame_cycles;
    uint64_t total_wait_cycles;
    uint64_t total_bytes;
} lvgl_nema_stats_t;

void lvgl_nema_frame_begin(void);
void lvgl_nema_frame_end(void);
void lvgl_nema_stats_reset(void);
void lvgl_nema_stats_get(lvgl_nema_stats_t *stats);
void lvgl_nema_gpu2d_error_isr(uint32_t error_code);
uint32_t lvgl_nema_completion_count(void);
int32_t lvgl_nema_last_command_list(void);
int lvgl_nema_command_list_state(int32_t *offset, int32_t *submission_id,
                                 uint32_t *flags, int32_t *internal_submitted_id,
                                 int32_t *internal_executed_id, uintptr_t *buffer,
                                 int32_t *buffer_size);
int lvgl_nema_task_state(int32_t *task_type, int32_t *x1, int32_t *y1,
                         int32_t *x2, int32_t *y2, uint32_t *submit_attempts,
                         uint32_t *submit_returns, uint32_t *wait_failures);

#endif /* LVGL_NEMA_PORT_H */
