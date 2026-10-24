#ifndef LVGL_H7RS_CACHE_H
#define LVGL_H7RS_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include <lvgl/draw/lv_draw_buf.h>

#ifdef __cplusplus
extern "C" {
#endif

void lvgl_h7rs_cache_clean_range(const void *address, size_t length);
void lvgl_h7rs_cache_invalidate_range(const void *address, size_t length);
void lvgl_h7rs_cache_flush_draw_buf(const lv_draw_buf_t *draw_buf,
                                    const lv_area_t *area);
void lvgl_h7rs_cache_invalidate_draw_buf(const lv_draw_buf_t *draw_buf,
                                         const lv_area_t *area);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_H7RS_CACHE_H */
