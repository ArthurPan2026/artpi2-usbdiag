/*
 * lvgl_mem_pool.h - LVGL builtin memory pool (defined in Src/lvgl_mem_pool.c)
 *
 * The pool lives in the dedicated PSRAM .lvgl_ram section so LVGL widget
 * memory does not compete with the internal RT-Thread heap or USB DMA area.
 */
#ifndef LVGL_MEM_POOL_H
#define LVGL_MEM_POOL_H

#include <stdint.h>

#define LVGL_MEM_POOL_SIZE (512u * 1024u)

extern uint8_t g_lvgl_mem_pool[LVGL_MEM_POOL_SIZE];

#endif /* LVGL_MEM_POOL_H */
