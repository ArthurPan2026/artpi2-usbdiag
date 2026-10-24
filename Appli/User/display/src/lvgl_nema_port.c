#include "lvgl_nema_port.h"

#include "draw/nema_gfx/lv_draw_nema_gfx.h"

extern void lv_nema_gfx_gpu2d_error_isr(uint32_t error_code);
extern uint32_t lv_nema_gfx_completion_count(void);
extern int32_t lv_nema_gfx_last_command_list(void);
extern int lv_nema_gfx_command_list_state(int32_t *offset, int32_t *submission_id,
                                          uint32_t *flags, int32_t *internal_submitted_id,
                                          int32_t *internal_executed_id, uintptr_t *buffer,
                                          int32_t *buffer_size);
extern int lv_nema_gfx_task_state(int32_t *task_type, int32_t *x1, int32_t *y1,
                                  int32_t *x2, int32_t *y2, uint32_t *submit_attempts,
                                  uint32_t *submit_returns, uint32_t *wait_failures);

void lvgl_nema_frame_begin(void)
{
    lv_nema_gfx_frame_begin();
}

void lvgl_nema_frame_end(void)
{
    lv_nema_gfx_frame_end();
}

void lvgl_nema_stats_reset(void)
{
    lv_nema_gfx_stats_reset();
}

void lvgl_nema_stats_get(lvgl_nema_stats_t *stats)
{
    lv_nema_gfx_stats_get((lv_nema_gfx_stats_t *)stats);
}

void lvgl_nema_gpu2d_error_isr(uint32_t error_code)
{
    lv_nema_gfx_gpu2d_error_isr(error_code);
}

uint32_t lvgl_nema_completion_count(void)
{
    return lv_nema_gfx_completion_count();
}

int32_t lvgl_nema_last_command_list(void)
{
    return lv_nema_gfx_last_command_list();
}

int lvgl_nema_command_list_state(int32_t *offset, int32_t *submission_id,
                                 uint32_t *flags, int32_t *internal_submitted_id,
                                 int32_t *internal_executed_id, uintptr_t *buffer,
                                 int32_t *buffer_size)
{
    return lv_nema_gfx_command_list_state(offset, submission_id, flags,
                                          internal_submitted_id,
                                          internal_executed_id, buffer,
                                          buffer_size);
}

int lvgl_nema_task_state(int32_t *task_type, int32_t *x1, int32_t *y1,
                         int32_t *x2, int32_t *y2, uint32_t *submit_attempts,
                         uint32_t *submit_returns, uint32_t *wait_failures)
{
    return lv_nema_gfx_task_state(task_type, x1, y1, x2, y2, submit_attempts,
                                  submit_returns, wait_failures);
}
