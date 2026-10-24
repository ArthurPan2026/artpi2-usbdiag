/*
 * lvgl_mem_pool.c - LVGL builtin memory pool in dedicated PSRAM
 *
 * LVGL 9.5's LV_STDLIB_BUILTIN allocator gets its pool from
 * LV_MEM_POOL_ALLOC (lv_conf.h). The pool is CPU-owned and therefore lives
 * in the dedicated PSRAM LVGL arena, leaving internal AXI SRAM for RT-Thread
 * context storage and USB/LVGL DMA buffers.
 */
#include "lvgl_mem_pool.h"

__attribute__((section(".lvgl_ram"), aligned(32)))
uint8_t g_lvgl_mem_pool[LVGL_MEM_POOL_SIZE];
