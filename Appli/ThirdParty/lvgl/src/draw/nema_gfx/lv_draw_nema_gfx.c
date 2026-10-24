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
 * @file lv_draw_nema_gfx.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "../../lvgl_public.h"

#if LV_USE_NEMA_GFX

#include "lv_draw_nema_gfx.h"
#include <lvgl/font/lv_font.h>
#include <lvgl/font/lv_font_fmt_txt.h>
#include "stm32h7rsxx_hal.h"
#include "stm32h7rsxx_hal_gpu2d.h"

/*********************
 *      DEFINES
 *********************/

#define DRAW_UNIT_ID_NEMA_GFX 7

/**********************
 *      TYPEDEFS
 **********************/

#if LV_USE_OS
/**
 * Structure of pending nema_gfx draw task
 */
typedef struct _nema_gfx_draw_task_t {
    lv_draw_task_t * task;
    bool flushed;
} nema_gfx_draw_task_t;
#endif

/**********************
 *  STATIC PROTOTYPES
 **********************/

#if LV_USE_OS && LV_NEMA_GFX_USE_THREAD
    static void nema_gfx_render_thread_cb(void * ptr);
#endif

static void nema_gfx_execute_drawing(lv_draw_nema_gfx_unit_t * u);

static int32_t nema_gfx_dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer);

static int32_t nema_gfx_evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task);

static int32_t nema_gfx_delete(lv_draw_unit_t * draw_unit);

static int32_t nema_gfx_wait_for_finish(lv_draw_unit_t * draw_unit);

static void nema_gfx_init_draw_buf_handlers(void);

/**********************
 *  STATIC VARIABLES
 **********************/

static lv_draw_nema_gfx_unit_t * s_nema_gfx_unit;
static volatile int32_t s_nema_last_task_type = -1;
static volatile int32_t s_nema_last_x1;
static volatile int32_t s_nema_last_y1;
static volatile int32_t s_nema_last_x2;
static volatile int32_t s_nema_last_y2;
static volatile uint32_t s_nema_submit_attempts;
static volatile uint32_t s_nema_submit_returns;
static volatile uint32_t s_nema_wait_failures;

/* The profiling counters are intentionally kept next to the draw-unit
 * diagnostics.  The application uses one synchronous Nema command list, so
 * a frame can be measured without adding another synchronization primitive to
 * the render path. */
static volatile uint32_t s_nema_frame_open;
static volatile uint32_t s_nema_frame_submit_count;
static volatile uint32_t s_nema_frame_cycles;
static volatile uint32_t s_nema_frame_bytes;
static volatile uint32_t s_nema_stats_frames;
static volatile uint32_t s_nema_stats_active_frames;
static volatile uint32_t s_nema_stats_submits;
static volatile uint32_t s_nema_stats_wait_failures;
static volatile uint32_t s_nema_stats_max_frame_cycles;
static volatile uint64_t s_nema_stats_wait_cycles;
static volatile uint64_t s_nema_stats_bytes;

static uint64_t nema_gfx_estimate_task_bytes(const lv_draw_task_t * task)
{
    int32_t width;
    int32_t height;
    uint64_t pixels;
    uint64_t bytes;

    if(task == NULL)
    {
        return 0U;
    }

    width = task->area.x2 - task->area.x1 + 1;
    height = task->area.y2 - task->area.y1 + 1;
    if(width <= 0 || height <= 0)
    {
        return 0U;
    }

    pixels = (uint64_t)(uint32_t)width * (uint64_t)(uint32_t)height;

    /* For an RGB565 destination, blending is conservatively accounted as a
     * read plus a write.  This is a useful upper-bound estimate for the
     * external-memory pressure of a draw task, not a hardware bus counter. */
    bytes = pixels * 2U * 2U;
    if(task->type == LV_DRAW_TASK_TYPE_IMAGE ||
       task->type == LV_DRAW_TASK_TYPE_LABEL)
    {
        /* Image sources are RGB565 in this port.  Labels use an A4/A8-like
         * glyph source; one byte per covered pixel is a safe estimate. */
        bytes += pixels * 2U;
    }

    return bytes;
}

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_nema_gfx_init(void)
{
    lv_draw_nema_gfx_unit_t * draw_nema_gfx_unit = lv_draw_create_unit(sizeof(lv_draw_nema_gfx_unit_t));
    s_nema_gfx_unit = draw_nema_gfx_unit;
    /*Initialize NemaGFX*/
    nema_init();
    nema_gfx_init_draw_buf_handlers();

    /*
     * H7RS requires the GPU2D system/bus error sources to be enabled by the
     * Nema layer.  CubeMX enables the NVIC lines, but HAL_GPU2D_Init() does
     * not program this Nema-specific register.  Without it an external
     * memory fault can leave a command list pending forever.
     */
    nema_reg_write(GPU2D_SYS_INTERRUPT, 0x7EU);
    nema_ext_hold_enable(2U);
    nema_ext_hold_irq_enable(2U);
    nema_ext_hold_enable(3U);
    nema_ext_hold_irq_enable(3U);

    draw_nema_gfx_unit->base_unit.dispatch_cb = nema_gfx_dispatch;
    draw_nema_gfx_unit->base_unit.evaluate_cb = nema_gfx_evaluate;
    draw_nema_gfx_unit->base_unit.delete_cb = nema_gfx_delete;
    draw_nema_gfx_unit->base_unit.wait_for_finish_cb = nema_gfx_wait_for_finish;
    draw_nema_gfx_unit->base_unit.name = "NEMA_GFX";

#if LV_USE_NEMA_VG
    /*Initialize NemaVG */
    nema_vg_init(LV_NEMA_GFX_MAX_RESX, LV_NEMA_GFX_MAX_RESY);
    /* Allocate VG Buffers*/
    draw_nema_gfx_unit->paint = nema_vg_paint_create();
    draw_nema_gfx_unit->gradient = nema_vg_grad_create();
    draw_nema_gfx_unit->path = nema_vg_path_create();
    /*Initialize Freetype Support*/
    lv_draw_nema_gfx_label_init(&(draw_nema_gfx_unit->base_unit));
#endif
    /*Create GPU Command List*/
    draw_nema_gfx_unit->cl = nema_cl_create_sized(LV_NEMA_GFX_COMMAND_LIST_SIZE);
    /*
     * Use a normal command list.  The LVGL dispatcher is synchronous on this
     * port, so each task submits and waits before the list is rewound.  This
     * avoids reusing a circular list while GPU2D is still reading it.
     */
    nema_cl_bind(&(draw_nema_gfx_unit->cl));
    nema_cl_rewind(&(draw_nema_gfx_unit->cl));


#if LV_USE_OS && LV_NEMA_GFX_USE_THREAD
    lv_thread_init(&draw_nema_gfx_unit->thread, "nemagfx", LV_DRAW_THREAD_PRIO, nema_gfx_render_thread_cb, 2 * 1024,
                   draw_nema_gfx_unit);
#endif
}

void lv_draw_nema_gfx_deinit(void)
{
    return;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static int32_t nema_gfx_wait_for_finish(lv_draw_unit_t * draw_unit)
{
    lv_draw_nema_gfx_unit_t * draw_nema_gfx_unit = (lv_draw_nema_gfx_unit_t *)draw_unit;
    return lv_nema_gfx_wait_and_rewind(draw_nema_gfx_unit) == 0 ? 1 : 0;
}

int lv_nema_gfx_submit_wait(lv_draw_nema_gfx_unit_t * unit)
{
    uint32_t start_cycles;
    uint32_t elapsed_cycles;
    uint64_t estimated_bytes;

    if(unit == NULL)
        return -1;

    if(unit->task_act != NULL) {
        s_nema_last_task_type = (int32_t)unit->task_act->type;
        s_nema_last_x1 = unit->task_act->area.x1;
        s_nema_last_y1 = unit->task_act->area.y1;
        s_nema_last_x2 = unit->task_act->area.x2;
        s_nema_last_y2 = unit->task_act->area.y2;
    }
    s_nema_submit_attempts++;
    estimated_bytes = nema_gfx_estimate_task_bytes(unit->task_act);
    start_cycles = DWT->CYCCNT;

    /* The H7RS NemaGFX examples unbind before handing a normal CL to the
     * ring-buffer submitter.  Bind it again only after the GPU is done. */
    nema_cl_unbind();
    nema_cl_submit(&unit->cl);
    int result = nema_cl_wait(&unit->cl);
    if(result == 0) {
        nema_cl_rewind(&unit->cl);
    }
    nema_cl_bind(&unit->cl);
    s_nema_submit_returns++;
    if(result != 0)
        s_nema_wait_failures++;

    elapsed_cycles = DWT->CYCCNT - start_cycles;
    s_nema_stats_submits++;
    if(result != 0)
        s_nema_stats_wait_failures++;
    if(s_nema_frame_open)
    {
        s_nema_frame_submit_count++;
        s_nema_frame_cycles += elapsed_cycles;
        /* A single frame cannot approach 4 GiB of GPU traffic on this
         * 800x480 UI. Keep the hot-path accumulator 32-bit and widen it once
         * at frame end. */
        s_nema_frame_bytes += (uint32_t)estimated_bytes;
    }
    return result;
}

int lv_nema_gfx_wait_and_rewind(lv_draw_nema_gfx_unit_t * unit)
{
    if(unit == NULL)
        return -1;

    int result = nema_cl_wait(&unit->cl);
    if(result == 0) {
        nema_cl_rewind(&unit->cl);
    }
    nema_cl_bind(&unit->cl);
    return result;
}

int lv_nema_gfx_command_list_state(int32_t * offset, int32_t * submission_id,
                                   uint32_t * flags, int32_t * internal_submitted_id,
                                   int32_t * internal_executed_id, uintptr_t * buffer,
                                   int32_t * buffer_size)
{
    if(s_nema_gfx_unit == NULL)
        return -1;

    if(offset) *offset = s_nema_gfx_unit->cl.offset;
    if(submission_id) *submission_id = s_nema_gfx_unit->cl.submission_id;
    if(flags) *flags = s_nema_gfx_unit->cl.flags;
    if(internal_submitted_id) *internal_submitted_id = s_nema_gfx_unit->cl.internal_submitted_id;
    if(internal_executed_id) *internal_executed_id = s_nema_gfx_unit->cl.internal_executed_id;
    if(buffer) *buffer = (uintptr_t)s_nema_gfx_unit->cl.bo.base_virt;
    if(buffer_size) *buffer_size = s_nema_gfx_unit->cl.bo.size;
    return 0;
}

int lv_nema_gfx_task_state(int32_t * task_type, int32_t * x1, int32_t * y1,
                           int32_t * x2, int32_t * y2, uint32_t * submit_attempts,
                           uint32_t * submit_returns, uint32_t * wait_failures)
{
    if(task_type) *task_type = s_nema_last_task_type;
    if(x1) *x1 = s_nema_last_x1;
    if(y1) *y1 = s_nema_last_y1;
    if(x2) *x2 = s_nema_last_x2;
    if(y2) *y2 = s_nema_last_y2;
    if(submit_attempts) *submit_attempts = s_nema_submit_attempts;
    if(submit_returns) *submit_returns = s_nema_submit_returns;
    if(wait_failures) *wait_failures = s_nema_wait_failures;
    return s_nema_gfx_unit == NULL ? -1 : 0;
}

void lv_nema_gfx_frame_begin(void)
{
    s_nema_frame_submit_count = 0U;
    s_nema_frame_cycles = 0U;
    s_nema_frame_bytes = 0U;
    s_nema_frame_open = 1U;
}

void lv_nema_gfx_frame_end(void)
{
    if(!s_nema_frame_open)
    {
        return;
    }

    s_nema_stats_frames++;
    if(s_nema_frame_submit_count != 0U)
    {
        s_nema_stats_active_frames++;
    }
    s_nema_stats_wait_cycles += s_nema_frame_cycles;
    s_nema_stats_bytes += s_nema_frame_bytes;
    if(s_nema_frame_cycles > s_nema_stats_max_frame_cycles)
    {
        s_nema_stats_max_frame_cycles = s_nema_frame_cycles;
    }
    s_nema_frame_open = 0U;
}

void lv_nema_gfx_stats_reset(void)
{
    s_nema_stats_frames = 0U;
    s_nema_stats_active_frames = 0U;
    s_nema_stats_submits = 0U;
    s_nema_stats_wait_failures = 0U;
    s_nema_stats_max_frame_cycles = 0U;
    s_nema_stats_wait_cycles = 0U;
    s_nema_stats_bytes = 0U;
    s_nema_frame_open = 0U;
    s_nema_frame_submit_count = 0U;
    s_nema_frame_cycles = 0U;
    s_nema_frame_bytes = 0U;
}

void lv_nema_gfx_stats_get(lv_nema_gfx_stats_t * stats)
{
    if(stats == NULL)
    {
        return;
    }

    stats->frames = s_nema_stats_frames;
    stats->active_frames = s_nema_stats_active_frames;
    stats->submits = s_nema_stats_submits;
    stats->wait_failures = s_nema_stats_wait_failures;
    stats->max_frame_cycles = s_nema_stats_max_frame_cycles;
    stats->total_wait_cycles = s_nema_stats_wait_cycles;
    stats->total_bytes = s_nema_stats_bytes;
}

static int32_t nema_gfx_evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task)
{
    lv_draw_nema_gfx_unit_t * draw_nema_gfx_unit = (lv_draw_nema_gfx_unit_t *)draw_unit;

    switch(task->type) {
        case LV_DRAW_TASK_TYPE_LAYER: {
                if(task->preference_score > 80) {
                    task->preference_score = 80;
                    task->preferred_draw_unit_id = DRAW_UNIT_ID_NEMA_GFX;
                }
                return 1;
            }
#if LV_USE_NEMA_VG
        case LV_DRAW_TASK_TYPE_TRIANGLE:
        case LV_DRAW_TASK_TYPE_ARC:
        case LV_DRAW_TASK_TYPE_FILL: {
                if(task->preference_score > 80) {
                    task->preference_score = 80;
                    task->preferred_draw_unit_id = DRAW_UNIT_ID_NEMA_GFX;
                }
                return 1;
            }
#else
        case LV_DRAW_TASK_TYPE_FILL: {
                lv_draw_fill_dsc_t * draw_fill_dsc = (lv_draw_fill_dsc_t *) task->draw_dsc;
                if((draw_fill_dsc->grad.dir == (lv_grad_dir_t)LV_GRAD_DIR_NONE)) {
                    if(task->preference_score > 80) {
                        task->preference_score = 80;
                        task->preferred_draw_unit_id = DRAW_UNIT_ID_NEMA_GFX;
                    }
                    return 1;
                }
                break;
            }
        case LV_DRAW_TASK_TYPE_TRIANGLE: {
                lv_draw_triangle_dsc_t * draw_triangle_dsc = (lv_draw_triangle_dsc_t *) task->draw_dsc;
                if((draw_triangle_dsc->grad.dir == (lv_grad_dir_t)LV_GRAD_DIR_NONE)) {
                    if(task->preference_score > 80) {
                        task->preference_score = 80;
                        task->preferred_draw_unit_id = DRAW_UNIT_ID_NEMA_GFX;
                    }
                    return 1;
                }
                break;
            }
#endif
        case LV_DRAW_TASK_TYPE_IMAGE: {
                lv_draw_image_dsc_t * draw_image_dsc = (lv_draw_image_dsc_t *) task->draw_dsc;
                /*Guard for previous NemaGFX Version*/
#ifndef NEMA_BLOP_RECOLOR
                if(draw_image_dsc->recolor_opa > LV_OPA_MIN)
                    break;
#endif
                const lv_image_dsc_t * img_dsc = draw_image_dsc->src;
                if(!lv_nemagfx_is_cf_supported(img_dsc->header.cf))
                    break;

                if(draw_image_dsc->blend_mode != LV_BLEND_MODE_SUBTRACTIVE) {
                    if(task->preference_score > 80) {
                        task->preference_score = 80;
                        task->preferred_draw_unit_id = DRAW_UNIT_ID_NEMA_GFX;
                    }
                    return 1;
                }
                break;
            }
        case LV_DRAW_TASK_TYPE_LABEL: {
#if LV_NEMA_GFX_ACCELERATE_LABEL
                lv_draw_label_dsc_t * draw_label_dsc = (lv_draw_label_dsc_t *) task->draw_dsc;
                lv_font_fmt_txt_dsc_t * fdsc = (lv_font_fmt_txt_dsc_t *)(draw_label_dsc->font->dsc);
                if(fdsc->bitmap_format != LV_FONT_FMT_TXT_COMPRESSED) {
                    if(task->preference_score > 80) {
                        task->preference_score = 80;
                        task->preferred_draw_unit_id = DRAW_UNIT_ID_NEMA_GFX;
                    }
                    return 1;
                }
#endif
                break;
            }
        case LV_DRAW_TASK_TYPE_LINE: {
                lv_draw_line_dsc_t * draw_line_dsc = (lv_draw_line_dsc_t *) task->draw_dsc;
                bool is_dashed = (draw_line_dsc->dash_width && draw_line_dsc->dash_gap);
                if(!is_dashed && !(draw_line_dsc->round_end || draw_line_dsc->round_start)) {
                    if(task->preference_score > 80) {
                        task->preference_score = 80;
                        task->preferred_draw_unit_id = DRAW_UNIT_ID_NEMA_GFX;
                    }
                    return 1;
                }
                break;
            }
        case LV_DRAW_TASK_TYPE_BORDER: {
                const lv_draw_border_dsc_t * draw_dsc = (lv_draw_border_dsc_t *) task->draw_dsc;
                if((!(draw_dsc->side != (lv_border_side_t)LV_BORDER_SIDE_FULL && draw_dsc->radius > 0)) &&
                   (draw_dsc->radius > draw_dsc->width)) {
                    if(task->preference_score > 80) {
                        task->preference_score = 80;
                        task->preferred_draw_unit_id = DRAW_UNIT_ID_NEMA_GFX;
                    }
                    return 1;
                }
                break;
            }
#if LV_USE_VECTOR_GRAPHIC && LV_USE_NEMA_VG
        case LV_DRAW_TASK_TYPE_VECTOR: {
                if(task->preference_score > 80) {
                    task->preference_score = 80;
                    task->preferred_draw_unit_id = DRAW_UNIT_ID_NEMA_GFX;
                }
                return 1;
            }
#endif
        case LV_DRAW_TASK_TYPE_BOX_SHADOW:
        case LV_DRAW_TASK_TYPE_MASK_RECTANGLE:
        case LV_DRAW_TASK_TYPE_MASK_BITMAP:
        default:
            break;
    }
    nema_cl_wait(&(draw_nema_gfx_unit->cl));
    return 0;
}

static int32_t nema_gfx_dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer)
{
    lv_draw_nema_gfx_unit_t * draw_nema_gfx_unit = (lv_draw_nema_gfx_unit_t *) draw_unit;

    /* Return immediately if it's busy with draw task. */
    if(draw_nema_gfx_unit->task_act)
        return 0;

    /* Try to get an ready to draw. */
    lv_draw_task_t * t = lv_draw_get_available_task(layer, NULL, DRAW_UNIT_ID_NEMA_GFX);

    /* Return 0 is no selection, some tasks can be supported by other units. */
    if(t == NULL || t->preferred_draw_unit_id != DRAW_UNIT_ID_NEMA_GFX)
        return LV_DRAW_UNIT_IDLE;

    void * buf = lv_draw_layer_alloc_buf(layer);
    if(buf == NULL)
        return LV_DRAW_UNIT_IDLE;

    t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    draw_nema_gfx_unit->task_act = t;

#if LV_USE_OS && LV_NEMA_GFX_USE_THREAD
    /* Let the render thread work. */
    if(draw_nema_gfx_unit->inited)
        lv_thread_sync_signal(&draw_nema_gfx_unit->sync);
#else
    nema_gfx_execute_drawing(draw_nema_gfx_unit);

    /* A normal command list is reusable only after GPU2D is idle. */
    (void)lv_nema_gfx_wait_and_rewind(draw_nema_gfx_unit);

    draw_nema_gfx_unit->task_act->state = LV_DRAW_TASK_STATE_FINISHED;
    draw_nema_gfx_unit->task_act = NULL;

    /* The draw unit is free now. Request a new dispatching as it can get a new task. */
    lv_draw_dispatch_request();
#endif

    return 1;
}

static void nema_gfx_execute_drawing(lv_draw_nema_gfx_unit_t * u)
{
    lv_draw_task_t * t = u->task_act;
    lv_layer_t * layer = t->target_layer;
    lv_area_t cache_area;
    bool cache_area_valid = lv_area_intersect(&cache_area, &t->area, &t->clip_area);

    /* remember draw unit for access to unit's context */
    t->draw_unit = (lv_draw_unit_t *)u;

    if(cache_area_valid) {
        lv_area_move(&cache_area, -layer->buf_area.x1, -layer->buf_area.y1);
        lv_draw_buf_flush_cache(layer->draw_buf, &cache_area);
    }

    switch(t->type) {
        case LV_DRAW_TASK_TYPE_FILL:
            lv_draw_nema_gfx_fill(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_IMAGE:
            lv_draw_nema_gfx_img(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_TRIANGLE:
            lv_draw_nema_gfx_triangle(t, t->draw_dsc);
            break;
        case LV_DRAW_TASK_TYPE_LABEL:
            lv_draw_nema_gfx_label(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_LAYER:
            lv_draw_nema_gfx_layer(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_LINE:
            lv_draw_line_iterate(t, t->draw_dsc, lv_draw_nema_gfx_line);
            break;
#if LV_USE_NEMA_VG
        case LV_DRAW_TASK_TYPE_ARC:
            lv_draw_nema_gfx_arc(t, t->draw_dsc, &t->area);
            break;
#endif
        case LV_DRAW_TASK_TYPE_BORDER:
            lv_draw_nema_gfx_border(t, t->draw_dsc, &t->area);
            break;
#if LV_USE_VECTOR_GRAPHIC && LV_USE_NEMA_VG
        case LV_DRAW_TASK_TYPE_VECTOR:
            lv_draw_nema_gfx_vector(t, t->draw_dsc, &t->area);
            break;
#endif
        default:
            break;
    }

    /*
     * Most primitives submit through lv_nema_gfx_submit_wait().  This final
     * wait also covers fully clipped/empty tasks and keeps the threaded build
     * variant safe if it is enabled later.
     */
    (void)lv_nema_gfx_wait_and_rewind(u);

    if(cache_area_valid) {
        lv_draw_buf_invalidate_cache(layer->draw_buf, &cache_area);
    }
}

static void nema_gfx_init_draw_buf_handlers(void)
{
#if LV_NEMA_USE_CACHE
    lv_draw_buf_handlers_t * handlers = lv_draw_buf_get_handlers();
    lv_draw_buf_handlers_t * font_handlers = lv_draw_buf_get_font_handlers();
    lv_draw_buf_handlers_t * image_handlers = lv_draw_buf_get_image_handlers();

    handlers->invalidate_cache_cb = lv_draw_nema_gfx_invalidate_cache;
    handlers->flush_cache_cb = lv_draw_nema_gfx_flush_cache;
    font_handlers->invalidate_cache_cb = lv_draw_nema_gfx_invalidate_cache;
    font_handlers->flush_cache_cb = lv_draw_nema_gfx_flush_cache;
    image_handlers->invalidate_cache_cb = lv_draw_nema_gfx_invalidate_cache;
    image_handlers->flush_cache_cb = lv_draw_nema_gfx_flush_cache;
#endif
}

static int32_t nema_gfx_delete(lv_draw_unit_t * draw_unit)
{
#if LV_USE_NEMA_VG
    /*Free VG Buffers*/
    lv_draw_nema_gfx_unit_t * draw_nema_gfx_unit = (lv_draw_nema_gfx_unit_t *) draw_unit;
    nema_vg_paint_destroy(draw_nema_gfx_unit->paint);
    nema_vg_path_destroy(draw_nema_gfx_unit->path);
    nema_vg_grad_destroy(draw_nema_gfx_unit->gradient);
#endif

#if LV_USE_OS && LV_NEMA_GFX_USE_THREAD
    lv_draw_nema_gfx_unit_t * _draw_nema_gfx_unit = (lv_draw_nema_gfx_unit_t *) draw_unit;
    LV_LOG_INFO("Cancel NemaGFX draw thread.");
    _draw_nema_gfx_unit->exit_status = true;

    if(_draw_nema_gfx_unit->inited)
        lv_thread_sync_signal(&_draw_nema_gfx_unit->sync);

    lv_result_t res = lv_thread_delete(&_draw_nema_gfx_unit->thread);

    return res;
#endif

#if LV_USE_NEMA_VG == 0 && LV_USE_OS == LV_OS_NONE
    LV_UNUSED(draw_unit);
#endif
    return 0;
}

#if LV_USE_OS && LV_NEMA_GFX_USE_THREAD
static void nema_gfx_render_thread_cb(void * ptr)
{
    lv_draw_nema_gfx_unit_t * u = ptr;

    lv_thread_sync_init(&u->sync);
    u->inited = true;

    while(1) {
        /* Wait for sync if there is no task set. */
        while(u->task_act == NULL) {
            if(u->exit_status)
                break;

            lv_thread_sync_wait(&u->sync);
        }

        if(u->exit_status) {
            LV_LOG_INFO("Ready to exit NemaGFX draw thread.");
            break;
        }

        if(u->task_act) {
            nema_gfx_execute_drawing(u);
        }
        /* Signal the ready state to dispatcher. */
        u->task_act->state = LV_DRAW_TASK_STATE_FINISHED;
        /* Cleanup. */
        u->task_act = NULL;

        /* The draw unit is free now. Request a new dispatching as it can get a new task. */
        lv_draw_dispatch_request();
    }

    u->inited = false;
    lv_thread_sync_delete(&u->sync);
    LV_LOG_INFO("Exit NemaGFX draw thread.");
}
#endif

#endif
