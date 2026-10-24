/**
 * @file lv_draw_nema_gfx_cache.c
 * Copyright (c) 2026 STMicroelectronics.
 */

#include "lv_draw_nema_gfx.h"

#if LV_NEMA_USE_CACHE

/*********************
 *      INCLUDES
 *********************/
#include "lvgl_h7rs_cache.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void __invalidate_flush_cache(const lv_draw_buf_t * draw_buf, const lv_area_t * area,
                                     bool flush);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_nema_gfx_invalidate_cache(const lv_draw_buf_t * draw_buf, const lv_area_t * area)
{
    __invalidate_flush_cache(draw_buf, area, false);
}

void lv_draw_nema_gfx_flush_cache(const lv_draw_buf_t * draw_buf, const lv_area_t * area)
{
    __invalidate_flush_cache(draw_buf, area, true);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void __invalidate_flush_cache(const lv_draw_buf_t * draw_buf, const lv_area_t * area,
                                     bool flush)
{
    if(flush)
        lvgl_h7rs_cache_flush_draw_buf(draw_buf, area);
    else
        lvgl_h7rs_cache_invalidate_draw_buf(draw_buf, area);
}

#endif
