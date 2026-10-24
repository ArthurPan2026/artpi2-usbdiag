/*
 * GPU-readable copies of the UI's A4 font bitmaps.
 *
 * The H7RS NeoChrom GPU can use XIP resources, but keeping mutable/derived
 * graphics inputs in the non-cacheable PSRAM arena removes the XIP texture
 * cache and external-bus timing from the command-list critical path.
 */
#include <rtthread.h>

#include "ui_fonts.h"

/* These capacities are generated from the bitmap arrays, audited before every
 * normal build, and checked against sizeof(glyph_bitmap) in each font TU. */
static uint8_t s_font14_gpu[UI_FONT_14_GPU_CAPACITY]
    __attribute__((section(".lvgl_ram"), aligned(UI_FONT_GPU_ALIGNMENT), used));
static uint8_t s_font16_gpu[UI_FONT_16_GPU_CAPACITY]
    __attribute__((section(".lvgl_ram"), aligned(UI_FONT_GPU_ALIGNMENT), used));
static uint8_t s_font20_gpu[UI_FONT_20_GPU_CAPACITY]
    __attribute__((section(".lvgl_ram"), aligned(UI_FONT_GPU_ALIGNMENT), used));
static uint8_t s_font24_gpu[UI_FONT_24_GPU_CAPACITY]
    __attribute__((section(".lvgl_ram"), aligned(UI_FONT_GPU_ALIGNMENT), used));

_Static_assert(sizeof(s_font14_gpu) + sizeof(s_font16_gpu) +
               sizeof(s_font20_gpu) + sizeof(s_font24_gpu) == UI_FONT_GPU_STAGING_BYTES,
               "UI font staging metadata is stale; regenerate all fonts");

void ui_font_gpu_init(void)
{
    size_t font14_size = lv_font_notosans_14_gpu_bitmap_size();
    size_t font16_size = lv_font_notosans_16_gpu_bitmap_size();
    size_t font20_size = lv_font_notosans_20_gpu_bitmap_size();
    size_t font24_size = lv_font_notosans_24_gpu_bitmap_size();

    if(font14_size != UI_FONT_14_BITMAP_BYTES || font16_size != UI_FONT_16_BITMAP_BYTES ||
       font20_size != UI_FONT_20_BITMAP_BYTES || font24_size != UI_FONT_24_BITMAP_BYTES ||
       lv_font_notosans_14_gpu_prepare(s_font14_gpu, sizeof(s_font14_gpu)) != 0 ||
       lv_font_notosans_16_gpu_prepare(s_font16_gpu, sizeof(s_font16_gpu)) != 0 ||
       lv_font_notosans_20_gpu_prepare(s_font20_gpu, sizeof(s_font20_gpu)) != 0 ||
       lv_font_notosans_24_gpu_prepare(s_font24_gpu, sizeof(s_font24_gpu)) != 0)
    {
        rt_kprintf("[LVGL] GPU font staging failed: 14=%lu 16=%lu 20=%lu 24=%lu\r\n",
                   (unsigned long)font14_size, (unsigned long)font16_size,
                   (unsigned long)font20_size, (unsigned long)font24_size);
        return;
    }

    rt_kprintf("[LVGL] GPU A4 fonts staged in PSRAM: 14=%lu 16=%lu 20=%lu 24=%lu\r\n",
               (unsigned long)font14_size, (unsigned long)font16_size,
               (unsigned long)font20_size, (unsigned long)font24_size);
}
