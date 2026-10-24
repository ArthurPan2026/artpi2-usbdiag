/*
 * lv_port_disp.h - LVGL display port (LTDC, ATK-MD0430R 800x480 RGB565)
 */
#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#include <rtthread.h>

#define LVGL_FRAMEBUFFER_CAPTURE_WIDTH  800U
#define LVGL_FRAMEBUFFER_CAPTURE_HEIGHT 480U
#define LVGL_FRAMEBUFFER_CAPTURE_BYTES  \
    (LVGL_FRAMEBUFFER_CAPTURE_WIDTH * LVGL_FRAMEBUFFER_CAPTURE_HEIGHT * 2U)

typedef struct
{
    rt_uint32_t generation;
    rt_uint32_t source_address;
    rt_uint32_t byte_count;
    rt_uint32_t crc32;
} lvgl_framebuffer_capture_info_t;

int lvgl_disp_init(void);

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
/* These functions are used only by the Debug LVGL owner thread
 * and read-only HTTP capture endpoint. */
rt_err_t lvgl_framebuffer_capture_publish(
    lvgl_framebuffer_capture_info_t *info);
rt_err_t lvgl_framebuffer_capture_get_info(
    lvgl_framebuffer_capture_info_t *info);
int lvgl_framebuffer_capture_copy_page(
    rt_uint32_t generation, rt_uint32_t offset, rt_uint8_t *destination,
    rt_size_t capacity, rt_uint32_t *total_bytes);
#endif

#endif /* LV_PORT_DISP_H */
