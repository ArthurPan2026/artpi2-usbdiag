/**
 * UI Manager — screen switching, data routing, teaching state machine
 */
#ifndef UI_MAIN_H
#define UI_MAIN_H

#include <rtthread.h>
#include <stddef.h>
#include <stdint.h>
#include "ui_model.h"

void ui_main_init(void);
void ui_main_update(void);
/* Creates decorative navigation marks only after the first frame is shown. */
bool ui_main_nav_icons_step(bool allow_start);
/* Temporarily reduce transitions during startup without changing the user's
 * persistent reduced-motion preference. Safe to request from another thread. */
void ui_main_set_startup_limited(bool limited);
/* Cooperative construction: one hidden, bounded step after foreground work.
 * allow_background is a real startup readiness gate, not a timeout. */
bool ui_main_preload_step(bool allow_background, uint32_t idle_budget_ms);
/* Builds one hidden home-timeline stage after the real first presentation. */
bool ui_main_home_timeline_step(bool allow_start);
bool ui_main_page_ready(unsigned page);
bool ui_main_preload_pending(void);
typedef struct {
    uint32_t ready_mask, loading_page, background_steps, foreground_steps;
    uint32_t steps[4], max_step_us[4], work_us[4], ready_ms[4];
    uint32_t probe, probe_cancel_steps, probe_fg_before, probe_fg_after;
} ui_preload_stats_t;
/* Copies RAM only; safe for non-LVGL diagnostics, contains no credentials. */
void ui_main_preload_stats(ui_preload_stats_t *out);
/* Owner-thread diagnostic count; counts complete pages, not partial shells. */
unsigned ui_main_created_page_count(void);
#if defined(WORK_UI_HOST) && WORK_UI_HOST
/* Host-only lookup for render/animation checks; never creates a page. */
lv_obj_t *ui_main_host_page_object(unsigned page_index);
#endif

/* Request a descriptor scrolling benchmark from a non-LVGL thread. */
void ui_main_request_descriptor_scroll_test(int start);

typedef struct {
    char text[384];
    int16_t x, y, w, h;
    uint8_t clipped, touch, canvas, disabled;
    uint16_t font_px;
} ui_inspect_item_t;
/* Owner-thread synchronous inspection. Return the total visible item count;
 * copy at most max items beginning at first. Canvas text is included. */
unsigned ui_main_inspect(size_t first, size_t max, ui_inspect_item_t *out);
int ui_main_state_json(char *buffer, size_t capacity);

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#define UI_MAIN_DEBUG_LABEL_MAX 16U
#define UI_MAIN_DEBUG_LABEL_TEXT_MAX 128U
rt_err_t ui_main_debug_navigate(int page_index, rt_uint32_t timeout_ms,
                                rt_uint32_t *child_count,
                                char labels[UI_MAIN_DEBUG_LABEL_MAX]
                                            [UI_MAIN_DEBUG_LABEL_TEXT_MAX],
                                rt_uint8_t *label_count,
                                rt_uint8_t *timeline_visible_count);
rt_err_t ui_main_debug_capture_framebuffer(
    rt_uint32_t timeout_ms, rt_uint32_t *generation,
    rt_uint32_t *source_address, rt_uint32_t *byte_count,
    rt_uint32_t *crc32);
rt_err_t ui_main_debug_inspect(int page_index, unsigned first,
    rt_uint32_t timeout_ms, ui_inspect_item_t out[8], unsigned *total,
    unsigned *returned, char state[384]);
rt_err_t ui_main_debug_action(ui_action_t action, int32_t argument,
    rt_uint32_t timeout_ms, char state[384]);
#endif

#endif
