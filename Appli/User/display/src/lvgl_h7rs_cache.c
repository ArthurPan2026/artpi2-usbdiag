#include "lvgl_h7rs_cache.h"

#include "stm32h7rsxx_hal.h"
#include <lvgl/core/lv_area.h>

#define H7RS_CACHE_LINE_SIZE 32U
#define H7RS_PSRAM_BEGIN     ((uintptr_t)0x90000000U)
#define H7RS_PSRAM_END       ((uintptr_t)0x92000000U)
#define H7RS_AXI_NC_BEGIN    ((uintptr_t)0x24060000U)
#define H7RS_AXI_NC_END      ((uintptr_t)0x24070000U)

static int cacheable_address(uintptr_t address)
{
    if(address >= H7RS_PSRAM_BEGIN && address < H7RS_PSRAM_END)
        return 0;
    if(address >= H7RS_AXI_NC_BEGIN && address < H7RS_AXI_NC_END)
        return 0;
    return 1;
}

static void cache_range(const void *address, size_t length, int clean)
{
    uintptr_t start;
    uintptr_t end;

    if(address == NULL || length == 0U || !cacheable_address((uintptr_t)address))
        return;
    if((SCB->CCR & SCB_CCR_DC_Msk) == 0U)
        return;

    start = (uintptr_t)address & ~(uintptr_t)(H7RS_CACHE_LINE_SIZE - 1U);
    end = ((uintptr_t)address + length + H7RS_CACHE_LINE_SIZE - 1U) &
          ~(uintptr_t)(H7RS_CACHE_LINE_SIZE - 1U);
    if(end <= start)
        return;

    if(clean)
        SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
    else
        SCB_InvalidateDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
}

void lvgl_h7rs_cache_clean_range(const void *address, size_t length)
{
    cache_range(address, length, 1);
}

void lvgl_h7rs_cache_invalidate_range(const void *address, size_t length)
{
    cache_range(address, length, 0);
}

static void cache_draw_buf(const lv_draw_buf_t *draw_buf,
                           const lv_area_t *area, int clean)
{
    lv_area_t clipped;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    uint32_t bpp;
    uint64_t start_bit;
    uint64_t end_bit;
    uint32_t start_byte;
    uint32_t end_byte;
    uint32_t bytes_per_line;
    uint8_t *address;
    int32_t line;

    if(draw_buf == NULL || draw_buf->data == NULL)
        return;

    clipped.x1 = 0;
    clipped.y1 = 0;
    clipped.x2 = (int32_t)draw_buf->header.w - 1;
    clipped.y2 = (int32_t)draw_buf->header.h - 1;
    if(area != NULL) {
        x1 = clipped.x1 > area->x1 ? clipped.x1 : area->x1;
        y1 = clipped.y1 > area->y1 ? clipped.y1 : area->y1;
        x2 = clipped.x2 < area->x2 ? clipped.x2 : area->x2;
        y2 = clipped.y2 < area->y2 ? clipped.y2 : area->y2;
        if(x1 > x2 || y1 > y2)
            return;
        clipped.x1 = x1;
        clipped.y1 = y1;
        clipped.x2 = x2;
        clipped.y2 = y2;
    }

    bpp = lv_color_format_get_bpp((lv_color_format_t)draw_buf->header.cf);
    if(bpp == 0U || draw_buf->header.stride == 0U ||
       clipped.x2 < clipped.x1 || clipped.y2 < clipped.y1)
        return;

    start_bit = (uint64_t)(uint32_t)clipped.x1 * bpp;
    end_bit = (uint64_t)((uint32_t)clipped.x2 + 1U) * bpp;
    start_byte = (uint32_t)(start_bit >> 3);
    end_byte = (uint32_t)((end_bit + 7U) >> 3);
    bytes_per_line = end_byte - start_byte;
    address = draw_buf->data + start_byte +
              draw_buf->header.stride * (uint32_t)clipped.y1;

    for(line = clipped.y1; line <= clipped.y2; line++) {
        cache_range(address, bytes_per_line, clean);
        address += draw_buf->header.stride;
    }
}

void lvgl_h7rs_cache_flush_draw_buf(const lv_draw_buf_t *draw_buf,
                                    const lv_area_t *area)
{
    cache_draw_buf(draw_buf, area, 1);
}

void lvgl_h7rs_cache_invalidate_draw_buf(const lv_draw_buf_t *draw_buf,
                                         const lv_area_t *area)
{
    cache_draw_buf(draw_buf, area, 0);
}
