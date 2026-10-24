/**
 * MIT License
 *
 * -----------------------------------------------------------------------------
 * Copyright (c) 2008-24 Think Silicon Single Member PC
 * -----------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next paragraph)
 * shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/**
 * @file lv_draw_nema_gfx.h
 *
 */

#ifndef LV_DRAW_NEMA_GFX_H
#define LV_DRAW_NEMA_GFX_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "../../lvgl_public.h"

#if LV_USE_NEMA_GFX

#include "lv_draw_nema_gfx_utils.h"
#include "lv_draw_nema_gfx_cache.h"

#include "../lv_draw_private.h"
#include "../lv_draw_buf_private.h"
#include "../lv_draw_image_private.h"
#include "../../image/lv_image_decoder_private.h"
#include "../lv_draw_label_private.h"
#include <lvgl/draw/lv_draw_mask.h>
#include <lvgl/draw/lv_draw_rect.h>
#include <lvgl/draw/lv_draw_triangle.h>
#include "../lv_draw_vector_private.h"
#include <lvgl/draw/lv_draw_line.h>
#include <lvgl/draw/lv_draw_arc.h>

#include "../../misc/lv_area_private.h"

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    lv_draw_unit_t base_unit;
    lv_draw_task_t * task_act;
#if LV_USE_OS
    lv_thread_sync_t sync;
    lv_thread_t thread;
    volatile bool inited;
    volatile bool exit_status;
#endif
    uint32_t idx;
    nema_cmdlist_t cl;
#if LV_USE_NEMA_VG
    NEMA_VG_PAINT_HANDLE paint;
    NEMA_VG_GRAD_HANDLE gradient;
    NEMA_VG_PATH_HANDLE path;
#endif
} lv_draw_nema_gfx_unit_t;

/**
 * Board-side NemaGFX profiling counters.
 *
 * `total_bytes` is an estimated PSRAM traffic counter.  It accounts for the
 * RGB565 destination read/write and the source read for image/label tasks;
 * it is deliberately labelled as an estimate because the GPU2D block has no
 * portable per-master byte counter exposed through the LVGL API.
 */
typedef struct {
    uint32_t frames;
    uint32_t active_frames;
    uint32_t submits;
    uint32_t wait_failures;
    uint32_t max_frame_cycles;
    uint64_t total_wait_cycles;
    uint64_t total_bytes;
} lv_nema_gfx_stats_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

void lv_draw_nema_gfx_init(void);

void lv_draw_nema_gfx_deinit(void);

/**
 * Submit the current command list, wait for GPU2D completion and make the
 * list reusable for the next LVGL draw task.
 *
 * The H7RS port deliberately uses a normal (non-circular) command list.  A
 * command list must not be rewound while GPU2D is still consuming it.
 */
int lv_nema_gfx_submit_wait(lv_draw_nema_gfx_unit_t * unit);

/** Wait for a previously submitted list and rewind it when it is idle. */
int lv_nema_gfx_wait_and_rewind(lv_draw_nema_gfx_unit_t * unit);

/** Return the command-list state for the board-side diagnostic shell. */
int lv_nema_gfx_command_list_state(int32_t * offset, int32_t * submission_id,
                                   uint32_t * flags, int32_t * internal_submitted_id,
                                   int32_t * internal_executed_id, uintptr_t * buffer,
                                   int32_t * buffer_size);

/** Return the last Nema task/submission state for the board-side shell. */
int lv_nema_gfx_task_state(int32_t * task_type, int32_t * x1, int32_t * y1,
                           int32_t * x2, int32_t * y2, uint32_t * submit_attempts,
                           uint32_t * submit_returns, uint32_t * wait_failures);

/** Mark the beginning/end of one LVGL application frame for profiling. */
void lv_nema_gfx_frame_begin(void);
void lv_nema_gfx_frame_end(void);

/** Reset/read the profiling counters without changing the draw pipeline. */
void lv_nema_gfx_stats_reset(void);
void lv_nema_gfx_stats_get(lv_nema_gfx_stats_t * stats);

void lv_draw_nema_gfx_fill(lv_draw_task_t * t,
                           const lv_draw_fill_dsc_t * dsc, const lv_area_t * coords);

void lv_draw_nema_gfx_triangle(lv_draw_task_t * t, const lv_draw_triangle_dsc_t * dsc);

void lv_draw_nema_gfx_img(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                          const lv_area_t * coords);

void lv_draw_nema_gfx_label(lv_draw_task_t * t, const lv_draw_label_dsc_t * dsc,
                            const lv_area_t * coords);

void lv_draw_nema_gfx_label_init(lv_draw_unit_t * draw_unit);

void lv_draw_nema_gfx_layer(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc,
                            const lv_area_t * coords);

void lv_draw_nema_gfx_line(lv_draw_task_t * t, const lv_draw_line_dsc_t * dsc);

void lv_draw_nema_gfx_border(lv_draw_task_t * t, const lv_draw_border_dsc_t * dsc,
                             const lv_area_t * coords);

void lv_draw_nema_gfx_arc(lv_draw_task_t * t, const lv_draw_arc_dsc_t * dsc,
                          const lv_area_t * coords);

#if LV_USE_VECTOR_GRAPHIC && LV_USE_NEMA_VG
void lv_draw_nema_gfx_vector(lv_draw_task_t * t, const lv_draw_vector_dsc_t * dsc,
                             const lv_area_t * coords);
#endif


/**********************
 *      MACROS
 **********************/

#endif  /*LV_USE_NEMA_GFX*/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_DRAW_NEMA_GFX_H*/
