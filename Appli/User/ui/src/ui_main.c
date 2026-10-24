#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>
#include <lvgl/font/lv_text.h>
#include "lv_port_disp.h"
#include "ui_deferred.h"
#include "text_preview.h"
#include "ui_main.h"
#include "ui_widgets.h"
#include "ui_detail.h"
#include "screens/scr_home.h"
#include "screens/scr_descriptors.h"
#include "screens/scr_events.h"
#include "screens/scr_wifi.h"
#if defined(WORK_UI_HOST) && WORK_UI_HOST
static void ui_main_wake_owner(void) {}
#else
#include "startup_coordinator.h"
#include "stm32h7rsxx_hal.h"
static void ui_main_wake_owner(void) { startup_notify_ui(); }
#endif
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#include "ui_debug_request.h"
#endif
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS && !defined(WORK_UI_HOST)
#include "ltdc_irq_adapter.h"
#endif

/* One bounded reader copy supports stable UI inspection. A frozen view never
 * overwrites it, even when the producer advances its published generation. */
static usb_evidence_snapshot_t s_evidence USB_EVIDENCE_RAM_SECTION;
static uint32_t s_session = UINT32_MAX, s_revision = UINT32_MAX;
static lv_obj_t *s_pages[UI_PAGE_COUNT], *s_nav[3], *s_mode, *s_session_button, *s_status;
static lv_obj_t *s_screen;
static ui_page_t s_visible_page = UI_PAGE_COUNT;
static bool s_nav_icons_pending;
static volatile int s_scroll_request = -1;
static volatile int s_startup_limited_request = -1;
static bool s_startup_limited;
static bool s_skip_initial_home_refresh;
static bool s_ready, s_force_refresh;
static bool s_commit_success;
static uint32_t s_page_create_ms[UI_PAGE_COUNT];
static uint32_t s_home_complete_ms;
static bool s_page_ready[UI_PAGE_COUNT], s_views_pending;
static lv_obj_t *s_loading, *s_loading_label;
static uint32_t s_init_tick, s_last_action_tick, s_loading_tick;
static ui_preload_stats_t s_preload;
static uint32_t s_feedback_samples, s_feedback_max;
static unsigned s_feedback_p95;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS && !defined(WORK_UI_HOST)
static bool s_feedback_pending;
static bool s_feedback_flushed;
static uint32_t s_feedback_started, s_feedback_reload;
static uint32_t s_feedback_hist[8];
static uint32_t s_visible_invalidations;
static void feedback_invalidated(lv_event_t *event)
{(void)event;s_visible_invalidations++;}
static uint32_t feedback_visible_revision(void){return s_visible_invalidations;}
static const unsigned s_feedback_bounds[8] = {2,4,8,16,32,64,100,1000};
static bool feedback_note_at(uint32_t requested_tick)
{
    if (s_feedback_pending) return false;
    s_feedback_pending = true;
    s_feedback_flushed = false;
    s_feedback_started = requested_tick;
    s_feedback_reload = ltdc_reload_count();
    return true;
}
static void feedback_cancel(bool owned){if(owned)s_feedback_pending=false;}
static void feedback_flush_started(lv_event_t *event)
{
    (void)event;
    if (s_feedback_pending && !s_feedback_flushed) {
        s_feedback_reload = ltdc_reload_count();
        s_feedback_flushed = true;
    }
}
static void feedback_poll(void)
{
    if (!s_feedback_pending || !s_feedback_flushed || s_feedback_reload == ltdc_reload_count()) return;
    uint32_t presented;
    if (!ltdc_reload_tick(s_feedback_reload + 1U, &presented)) {
        s_feedback_pending = false; return;
    }
    uint32_t elapsed = (uint32_t)(((uint64_t)(presented - s_feedback_started) * 1000U) / RT_TICK_PER_SECOND);
    unsigned bucket = 0;
    while (bucket < 7 && elapsed > s_feedback_bounds[bucket]) bucket++;
    s_feedback_hist[bucket]++; s_feedback_samples++;
    if (elapsed > s_feedback_max) s_feedback_max = elapsed;
    unsigned accumulated = 0;
    for (unsigned i = 0; i < 8; i++) {
        accumulated += s_feedback_hist[i];
        if ((uint64_t)accumulated * 100 >= (uint64_t)s_feedback_samples * 95) {
            /* The final bin includes >1000 ms. Its actual observed maximum,
             * not 1000, is a valid upper bound for this overflow bin. */
            s_feedback_p95 = i==7 && s_feedback_max>1000 ? s_feedback_max : s_feedback_bounds[i]; break;
        }
    }
    s_feedback_pending = false;
}
#else
static bool feedback_note_at(uint32_t tick) {(void)tick;return false;}
static void feedback_cancel(bool owned) {(void)owned;}
static uint32_t feedback_visible_revision(void){return 0;}
static void feedback_poll(void) {}
#endif
typedef struct {
    ui_action_t action;
    int32_t argument;
    uint32_t snapshot_epoch;
    uint32_t requested_tick;
    bool occupied;
} pending_action_t;
static pending_action_t s_actions[24];

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static rt_sem_t s_page_sem, s_frame_sem, s_inspect_sem, s_action_sem;
static volatile int s_page_request = -1, s_inspect_page = -1;
static volatile bool s_frame_request;
static ui_debug_request_t s_action_request;
static volatile ui_action_t s_debug_action;
static volatile int32_t s_debug_argument;
static volatile unsigned s_inspect_first, s_inspect_total, s_inspect_returned;
static ui_inspect_item_t s_inspect_reply[8] USB_EVIDENCE_RAM_SECTION;
static char s_debug_state[384], s_action_state[384];
static char s_page_labels[UI_MAIN_DEBUG_LABEL_MAX][UI_MAIN_DEBUG_LABEL_TEXT_MAX];
static uint8_t s_page_label_count, s_page_timeline;
static uint32_t s_page_children;
static int s_page_active;
static uint32_t s_probe_started, s_probe_tick;
static lvgl_framebuffer_capture_info_t s_frame_info;
static rt_err_t s_frame_result, s_action_result;
#endif

static void text_copy(char *out, size_t capacity, const char *text)
{
    (void)work_text_preview_copy(out, capacity, text, 0);
}
static void set_button_label(lv_obj_t *button, const char *text)
{
    if (!button || !text) return;
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (!label) return;
    bool changed = strcmp(lv_label_get_text(label), text) != 0;
    if (changed) lv_label_set_text(label, text);
    int32_t width = lv_obj_get_width(button) - 12;
    if (changed || lv_obj_get_width(label) != width ||
        lv_label_get_long_mode(label) != LV_LABEL_LONG_MODE_DOTS) {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_center(label);
    }
}
static bool ensure_page_created(ui_page_t page, bool *created_now)
{
    lv_obj_t *created = NULL;
    int32_t child_index = 1;

    if (created_now) *created_now = false;
    if ((unsigned)page >= UI_PAGE_COUNT || !s_screen) return false;
    if (s_pages[page]) return s_page_ready[page];
    /* Non-home construction belongs to the cooperative builder. A request
     * merely selects/prioritizes the page; it never falls back to a long,
     * synchronous constructor in a touch/Observer callback. */
    if (page != UI_PAGE_LIVE) return false;
    rt_tick_t create_begin=rt_tick_get();
    /* Intermediate coordinates/styles of a newly constructed subtree are
     * never visible. Avoid repeatedly walking/invalidation of that growing
     * tree; keep style and layout events enabled, then invalidate once. */
    lv_display_t *display=lv_display_get_default();
    bool invalidation=display && lv_display_is_invalidation_enabled(display);
    if(invalidation)lv_display_enable_invalidation(display,false);

    switch (page) {
    case UI_PAGE_LIVE:        created = scr_home_create(s_screen); break;
    default: break;
    }
    if(invalidation)lv_display_enable_invalidation(display,true);
    if (!created) return false;
    s_page_create_ms[page]=(uint32_t)((uint64_t)(rt_tick_get()-create_begin)*1000U/RT_TICK_PER_SECOND);

    s_pages[page] = created;
    s_page_ready[page] = true;
    /* The live page shell is visible and useful immediately, but the home
     * page is not complete until its deferred eight-stage timeline exists. */
    if (page != UI_PAGE_LIVE || scr_home_deferred_is_complete())
        s_preload.ready_mask |= 1U << page;
    if (page == UI_PAGE_LIVE && scr_home_deferred_is_complete())
        s_home_complete_ms = (uint32_t)((uint64_t)(rt_tick_get() - s_init_tick) *
                                         1000U / RT_TICK_PER_SECOND);
    lv_obj_set_pos(created, 0, UI_TOP_BAR_HEIGHT);
    lv_obj_set_size(created, 800, UI_CONTENT_HEIGHT);
    lv_obj_set_hidden(created, page != ui_model_get()->page);
    for (unsigned i = 0; i < (unsigned)page; i++)
        if (s_pages[i]) child_index++;
    /* Keep pages in enum order and in front of navigation/detail overlays,
     * even when the first visit is to a later page. */
    lv_obj_move_to_index(created, child_index);
    if(invalidation)lv_obj_invalidate(created);
    if (created_now) *created_now = true;
    return true;
}

bool ui_main_page_ready(unsigned page)
{
    if (page >= UI_PAGE_COUNT || !s_page_ready[page]) return false;
    return page != UI_PAGE_LIVE || scr_home_deferred_is_complete();
}

bool ui_main_preload_pending(void)
{ return s_ready && s_preload.ready_mask != (1U << UI_PAGE_COUNT) - 1U; }

void ui_main_preload_stats(ui_preload_stats_t *out)
{
    if (!out) return;
    rt_enter_critical(); *out = s_preload; rt_exit_critical();
}

static void refresh_views(void);

static void loading_view(ui_page_t page, bool show)
{
    if (show && !s_loading) {
        s_loading = ui_panel(s_screen, 0, UI_TOP_BAR_HEIGHT, 800, UI_CONTENT_HEIGHT);
        lv_obj_set_style_bg_color(s_loading, C_BG, 0);
        s_loading_label = ui_text(s_loading, "初始化中", F_TITLE, C_TEXT);
        lv_obj_set_pos(s_loading_label, 24, 100);
        lv_obj_t *hint = ui_text(s_loading, "可以随时返回首页", F_BODY, C_GRAY);
        lv_obj_set_pos(hint, 24, 144);
    }
    if (!s_loading) return;
    if (show) {
        static const char *const labels[] = {
            "实时", "描述符 · 初始化中", "诊断 · 初始化中", "设置 · 初始化中"
        };
        if (s_preload.loading_page != (uint32_t)page) s_loading_tick = lv_tick_get();
        lv_label_set_text(s_loading_label, labels[page]);
        lv_obj_move_to_index(s_loading, -1);
    }
    lv_obj_set_hidden(s_loading, !show);
    s_preload.loading_page = show ? (uint32_t)page : UI_PAGE_COUNT;
}

bool ui_main_preload_step(bool allow_background, uint32_t idle_budget_ms)
{
    if (!ui_main_preload_pending() || !s_screen) return false;
    unsigned page = ui_model_get()->page;
    bool foreground = page < UI_PAGE_COUNT && !s_page_ready[page];
    if (foreground) {
        /* Give the loading state a real render opportunity before any work.
         * It remains cancelable by navigation; it is not a content-ready claim. */
        if (lv_tick_elaps(s_loading_tick) < 34U) return false;
    } else {
        if (!allow_background || idle_budget_ms < 4U ||
            lv_tick_elaps(s_last_action_tick) < 200U ||
            lv_display_get_inactive_time(lv_display_get_default()) < 200U ||
            ui_model_get()->detail != UI_DETAIL_NONE || lv_anim_count_running()) return false;
        for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input))
            if (lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) return false;
        /* Settings is the largest first-use cost; prepare it first. */
        static const unsigned priority[] = {UI_PAGE_SETTINGS, UI_PAGE_DESCRIPTORS, UI_PAGE_DIAGNOSTICS};
        for (unsigned i = 0; i < 3; ++i)
            if (!s_page_ready[priority[i]]) { page = priority[i]; break; }
    }
    if (page >= UI_PAGE_COUNT || s_page_ready[page]) return false;
    lv_display_t *display = lv_display_get_default();
    bool invalidation = display && lv_display_is_invalidation_enabled(display);
    uint32_t begin_tick = (uint32_t)rt_tick_get();
#if !defined(WORK_UI_HOST)
    uint32_t begin_cycle = DWT->CYCCNT;
#endif
    /* LVGL 9.6 uses a nesting counter, NOT an absolute Boolean setter.
     * Only balance the disable that this slice actually owns. */
    if (invalidation) lv_display_enable_invalidation(display, false);
    bool complete = false;
    if (!s_pages[page]) {
        switch (page) {
        case UI_PAGE_DESCRIPTORS: s_pages[page] = scr_descriptors_create_begin(s_screen); break;
        case UI_PAGE_DIAGNOSTICS: s_pages[page] = scr_events_create_begin(s_screen); break;
        case UI_PAGE_SETTINGS: s_pages[page] = scr_wifi_create_begin(s_screen); break;
        default: break;
        }
        if (s_pages[page]) {
            lv_obj_set_hidden(s_pages[page], true);
            lv_obj_set_pos(s_pages[page], 0, UI_TOP_BAR_HEIGHT);
            lv_obj_set_size(s_pages[page], 800, UI_CONTENT_HEIGHT);
            int index = 1;
            for (unsigned i = 0; i < page; ++i) if (s_pages[i]) ++index;
            lv_obj_move_to_index(s_pages[page], index);
        }
    } else {
        switch (page) {
        case UI_PAGE_DESCRIPTORS: complete = scr_descriptors_create_step(); break;
        case UI_PAGE_DIAGNOSTICS: complete = scr_events_create_step(); break;
        case UI_PAGE_SETTINGS: complete = scr_wifi_create_step(); break;
        default: break;
        }
    }
    if (invalidation) lv_display_enable_invalidation(display, true);
    uint32_t elapsed_us = (uint32_t)((uint64_t)((uint32_t)rt_tick_get() - begin_tick) * 1000000U / RT_TICK_PER_SECOND);
#if !defined(WORK_UI_HOST)
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) && SystemCoreClock >= 1000000U)
        elapsed_us = (DWT->CYCCNT - begin_cycle) / (SystemCoreClock / 1000000U);
#endif
    ++s_preload.steps[page];
    if (foreground) ++s_preload.foreground_steps; else ++s_preload.background_steps;
    s_preload.work_us[page] += elapsed_us;
    if (elapsed_us > s_preload.max_step_us[page]) s_preload.max_step_us[page] = elapsed_us;
    s_page_create_ms[page] = (s_preload.work_us[page] + 999U) / 1000U;
    if (complete) {
        s_page_ready[page] = true;
        s_preload.ready_mask |= 1U << page;
        s_preload.ready_ms[page] = (uint32_t)((uint64_t)((uint32_t)rt_tick_get() - s_init_tick) * 1000U / RT_TICK_PER_SECOND);
        /* Bind current (possibly frozen) evidence in the next bracketed owner
         * iteration, never draw canvases outside the GPU frame contract. */
        if (ui_model_get()->page == page) s_views_pending = true;
    }
    return true;
}

bool ui_main_home_timeline_step(bool allow_start)
{
    const unsigned page = UI_PAGE_LIVE;
    const ui_view_state_t *view = ui_model_get();
    lv_display_t *display;
    bool invalidation;
    bool complete;
    uint32_t begin_tick;
    uint32_t elapsed_us;
#if !defined(WORK_UI_HOST)
    uint32_t begin_cycle;
#endif

    if (!allow_start || !s_pages[page] || !s_page_ready[page] ||
        view->page != UI_PAGE_LIVE || view->detail != UI_DETAIL_NONE ||
        lv_tick_elaps(s_last_action_tick) < 200U ||
        lv_anim_count_running() != 0U ||
        scr_home_deferred_is_complete()) return false;
    for (lv_indev_t *input = lv_indev_get_next(NULL); input;
         input = lv_indev_get_next(input))
        if (lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) return false;

    display = lv_display_get_default();
    invalidation = display && lv_display_is_invalidation_enabled(display);
    begin_tick = (uint32_t)rt_tick_get();
#if !defined(WORK_UI_HOST)
    begin_cycle = DWT->CYCCNT;
#endif
    /* Build timeline controls and then the lower detail panels in bounded
     * LVGL-owner slices after the first frame has already been presented. */
    if (invalidation) lv_display_enable_invalidation(display, false);
    if (!scr_home_timeline_is_complete()) {
        complete = scr_home_create_timeline_step();
        if (complete) scr_home_show_timeline();
    } else {
        complete = scr_home_create_details_step();
    }
    if (invalidation) lv_display_enable_invalidation(display, true);
    complete = scr_home_deferred_is_complete();

    elapsed_us = (uint32_t)((uint64_t)((uint32_t)rt_tick_get() - begin_tick) *
                            1000000U / RT_TICK_PER_SECOND);
#if !defined(WORK_UI_HOST)
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) && SystemCoreClock >= 1000000U)
        elapsed_us = (DWT->CYCCNT - begin_cycle) /
                     (SystemCoreClock / 1000000U);
#endif
    ++s_preload.steps[page];
    ++s_preload.background_steps;
    s_preload.work_us[page] += elapsed_us;
    if (elapsed_us > s_preload.max_step_us[page])
        s_preload.max_step_us[page] = elapsed_us;
    if (complete) {
        /* Placeholder visibility changes were made with invalidation paused.
         * Repaint the completed home page once so its old pixels cannot remain
         * in the active framebuffer after the deferred build is marked ready. */
        lv_obj_invalidate(s_pages[page]);
        s_preload.ready_mask |= 1U << page;
        s_preload.ready_ms[page] = (uint32_t)(
            (uint64_t)((uint32_t)rt_tick_get() - s_init_tick) *
            1000U / RT_TICK_PER_SECOND);
        s_home_complete_ms = s_preload.ready_ms[page];
    }
    return true;
}

static void apply_startup_limited_request(void)
{
    int request;
    if (s_startup_limited_request < 0) return;
    rt_enter_critical();
    request = s_startup_limited_request;
    s_startup_limited_request = -1;
    rt_exit_critical();
    if (request >= 0) s_startup_limited = request != 0;
    ui_widgets_set_startup_limited(s_startup_limited);
}

void ui_main_set_startup_limited(bool limited)
{
    rt_enter_critical();
    s_startup_limited_request = limited ? 1 : 0;
    rt_exit_critical();
    ui_main_wake_owner();
}

unsigned ui_main_created_page_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < UI_PAGE_COUNT; i++)
        if (ui_main_page_ready(i)) count++;
    return count;
}

#if defined(WORK_UI_HOST) && WORK_UI_HOST
lv_obj_t *ui_main_host_page_object(unsigned page_index)
{
    return page_index < UI_PAGE_COUNT ? s_pages[page_index] : NULL;
}
#endif

static void refresh_views(void)
{
    if (!s_ready) return;
    const ui_view_state_t *v = ui_model_get();
    const usb_evidence_snapshot_t *s = v->snapshot;
    ui_widgets_set_reduced_motion(v->reduced_motion);
    bool page_created_now = false;
    bool page_ready = ensure_page_created(v->page, &page_created_now);
    if (s_visible_page != v->page) {
        if (s_visible_page == UI_PAGE_SETTINGS && s_page_ready[UI_PAGE_SETTINGS])
            scr_wifi_close_editor();
        for (unsigned i = 0; i < UI_PAGE_COUNT; i++) {
            lv_obj_t *page = s_pages[i];
            if (!page) continue;
            lv_anim_delete(page, NULL);
            lv_obj_set_style_translate_x(page, 0, 0);
            lv_obj_set_hidden(page, i != (unsigned)v->page || !s_page_ready[i]);
        }
        s_visible_page = v->page;
        for (unsigned i = 0; i < 3; i++)
            if (s_nav[i]) ui_button_set_selected(s_nav[i], (unsigned)v->page == i);
        if (page_ready) ui_animate_enter(s_pages[v->page], UI_NAV_MS, 10);
    }
    loading_view(v->page, !page_ready);
    char text[128];
    snprintf(text, sizeof(text), "#%lu · r%lu", s ? (unsigned long)s->session_id : 0,
        s ? (unsigned long)s->revision : 0);
    set_button_label(s_session_button, text);
    set_button_label(s_mode, v->frozen ?
        (v->updates_available ? "冻结 · 有更新" : "冻结查看") : "跟随最新");
    ui_button_set_selected(s_mode, v->frozen);
    const char *state = "等待设备";
    lv_color_t color = C_GRAY;
    if (s && s->session_id) {
        if (s->capture_integrity != USB_EVIDENCE_INTEGRITY_COMPLETE) {
            state = "证据不足"; color = C_ORANGE;
        } else if (s->session_state == USB_EVIDENCE_SESSION_FAILED) {
            state = "枚举失败"; color = C_RED;
        } else if (s->is_final) { state = "证据完整"; color = C_GREEN; }
        else { state = "枚举进行中"; color = C_BLUE; }
    }
    if (s_status && strcmp(lv_label_get_text(s_status), state) != 0)
        lv_label_set_text(s_status, state);
    if (s_status && !lv_color_eq(
            lv_obj_get_style_text_color(s_status, LV_PART_MAIN), color))
        lv_obj_set_style_text_color(s_status, color, 0);
    if (!page_ready) { ui_detail_refresh(); return; }
    /* Construction may have completed without a page change. */
    lv_obj_set_hidden(s_pages[v->page], false);
    if (v->page == UI_PAGE_LIVE) {
        if (s_skip_initial_home_refresh) s_skip_initial_home_refresh = false;
        else if (!page_created_now) scr_home_refresh();
    }
    else if (v->page == UI_PAGE_DESCRIPTORS) {
        if (!page_created_now) scr_descriptors_refresh();
    }
    else if (v->page == UI_PAGE_DIAGNOSTICS) {
        if (!page_created_now) scr_events_refresh();
    }
    else if (v->page == UI_PAGE_SETTINGS) scr_wifi_refresh_preferences();
    if (v->detail != UI_DETAIL_NONE) ui_detail_create(s_screen);
    ui_detail_refresh();
}
static void selection_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer; (void)subject;
    refresh_views();
}
static void action_commit(uintptr_t index)
{
    s_commit_success = false;
    if (index >= 24 || !s_actions[index].occupied) return;
    pending_action_t action = s_actions[index];
    s_actions[index].occupied = false;
    if (action.action != UI_ACTION_PAGE && action.action != UI_ACTION_FOLLOW &&
        action.action != UI_ACTION_CLOSE_DETAIL &&
        action.snapshot_epoch != ui_model_get()->snapshot_epoch) return;
    /* Arm only when the business action is about to commit. A flush of the
     * preceding pressed/released frame must not masquerade as its result. */
    bool feedback_owned=feedback_note_at(action.requested_tick);
    uint32_t visible_revision=feedback_visible_revision();
    uint32_t revision=ui_model_get()->selection_revision;
    s_commit_success = ui_model_apply_action(action.action, action.argument);
    if(!s_commit_success || revision==ui_model_get()->selection_revision ||
        visible_revision==feedback_visible_revision())
        feedback_cancel(feedback_owned);
    if (s_commit_success && revision != ui_model_get()->selection_revision) {
        /* The action committed AFTER a possible released-button frame. Ask
         * the next owner iteration to render dirty content immediately rather
         * than paying another refresh-period delay. Hidden/no-op changes do
         * not manufacture invalid areas or framebuffer swaps. */
        lv_timer_t *refresh = lv_display_get_refr_timer(lv_display_get_default());
        if (refresh) lv_timer_ready(refresh);
    }
    if (s_commit_success && action.action == UI_ACTION_FOLLOW) s_force_refresh = true;
}
static void action_post(ui_action_t action, int32_t argument)
{
    s_last_action_tick = lv_tick_get();
    for (unsigned i = 0; i < 24; i++) if (!s_actions[i].occupied) {
        s_actions[i].action = action;
        s_actions[i].argument = argument;
        s_actions[i].snapshot_epoch = ui_model_get()->snapshot_epoch;
        s_actions[i].requested_tick=(uint32_t)rt_tick_get();
        s_actions[i].occupied = true;
        if (ui_deferred_post(action_commit, i) != 0) s_actions[i].occupied = false;
        else ui_main_wake_owner();
        return;
    }
}
static void refresh_evidence(void)
{
    rt_uint32_t session, revision;
    if (!usb_evidence_get_snapshot_meta(&session, &revision)) return;
    if (ui_model_get()->frozen) {
        ui_model_set_update_available(session != s_session || revision != s_revision);
        return;
    }
    if (!s_force_refresh && session == s_session && revision == s_revision) return;
    if (!usb_evidence_get_snapshot(&s_evidence)) return;
    s_session = s_evidence.session_id;
    s_revision = s_evidence.revision;
    s_force_refresh = false;
    ui_model_set_snapshot(&s_evidence);
}

/* Small geometric navigation marks, made from opaque rectangles rather than
 * icon-font glyphs. They share the same project color/size tokens. */
static void nav_icon(lv_obj_t *button, unsigned kind)
{
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *mark = lv_obj_create(button);
        lv_obj_set_style_bg_color(mark, C_BLUE, 0);
        lv_obj_set_style_border_width(mark, 0, 0);
        lv_obj_set_style_radius(mark, 2, 0);
        lv_obj_set_scrollable(mark, false);
        lv_obj_set_clickable(mark, false);
        if (kind == 0) {
            lv_obj_set_pos(mark, 56 + i * 6, 22 - i * 4);
            lv_obj_set_size(mark, 4, 6 + i * 4);
        } else {
            lv_obj_set_pos(mark, 54 + (kind == 1 ? i * 3 : 0), 12 + i * 6);
            lv_obj_set_size(mark, kind == 1 ? 12 : 18, 3);
        }
    }
}

bool ui_main_nav_icons_step(bool allow_start)
{
    if (!allow_start || !s_nav_icons_pending)
    {
        return false;
    }
    for (unsigned i = 0U; i < 3U; i++)
    {
        if (s_nav[i])
        {
            nav_icon(s_nav[i], i);
        }
    }
    s_nav_icons_pending = false;
    return true;
}

void ui_main_init(void)
{
    s_ready = false;
    s_visible_page = UI_PAGE_COUNT;
    memset(s_pages, 0, sizeof(s_pages));
    memset(s_nav, 0, sizeof(s_nav));
    s_nav_icons_pending = false;
    memset(s_actions, 0, sizeof(s_actions));
    memset(s_page_ready, 0, sizeof(s_page_ready));
    memset(&s_preload, 0, sizeof(s_preload));
    s_home_complete_ms = 0U;
    s_preload.loading_page = UI_PAGE_COUNT;
    s_init_tick = (uint32_t)rt_tick_get(); s_last_action_tick = lv_tick_get();
    s_loading = s_loading_label = NULL; s_views_pending = false;
    s_skip_initial_home_refresh = false;
    ui_styles_init();
    ui_model_init(action_post);
    ui_widgets_set_reduced_motion(false);
    apply_startup_limited_request();
    ui_widgets_set_startup_limited(s_startup_limited);
    scr_wifi_init();
    lv_obj_t *screen = lv_screen_active();
    s_screen = screen;
    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_set_style_text_font(screen, F_BODY, 0);
    lv_obj_set_style_text_color(screen, C_TEXT, 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_t *top = ui_panel(screen, 0, 0, 800, UI_TOP_BAR_HEIGHT);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_pad_left(top, 12, 0);
    lv_obj_set_style_pad_right(top, 12, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(top, 8, 0);
    lv_obj_t *title = ui_text(top, "USB 枚举诊断仪", F_TITLE, C_TEXT);
    lv_obj_set_width(title, 196);
    s_status = ui_text(top, "等待设备", F_META, C_GRAY);
    lv_obj_set_width(s_status, 100);
    s_session_button = ui_button(top, "#0 · r0", 108, UI_ACTION_SESSION, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(s_session_button, 0), F_META, 0);
    s_mode = ui_button(top, "跟随最新", 176, UI_ACTION_FOLLOW, 0);
    lv_obj_t *spacer = lv_obj_create(top);
    lv_obj_add_style(spacer, &style_transp, 0);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_size(spacer, 1, 1);
    ui_button(top, "设置", 72, UI_ACTION_PAGE, UI_PAGE_SETTINGS);
    bool home_created = false;
    (void)ensure_page_created(UI_PAGE_LIVE, &home_created);
    s_skip_initial_home_refresh = home_created;
    lv_obj_t *nav = ui_panel(screen, 0, 424, 800, UI_NAV_HEIGHT);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_style_pad_all(nav, 4, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *const names[] = {"实时", "描述符", "诊断"};
    for (unsigned i = 0; i < 3; i++) {
        s_nav[i] = ui_button(nav, names[i], 232, UI_ACTION_PAGE, i);
#if defined(WORK_UI_HOST) && WORK_UI_HOST
        nav_icon(s_nav[i], i);
#else
        s_nav_icons_pending = true;
#endif
    }
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    s_page_sem = rt_sem_create("ui_pg", 0, RT_IPC_FLAG_FIFO);
    s_frame_sem = rt_sem_create("fb_cap", 0, RT_IPC_FLAG_FIFO);
    s_inspect_sem = rt_sem_create("ui_ins", 0, RT_IPC_FLAG_FIFO);
    s_action_sem = rt_sem_create("ui_act", 0, RT_IPC_FLAG_FIFO);
    unsigned short limit = 1;
    if (s_page_sem) rt_sem_control(s_page_sem, RT_IPC_CMD_SET_VLIMIT, &limit);
    if (s_frame_sem) rt_sem_control(s_frame_sem, RT_IPC_CMD_SET_VLIMIT, &limit);
    if (s_inspect_sem) rt_sem_control(s_inspect_sem, RT_IPC_CMD_SET_VLIMIT, &limit);
    if (s_action_sem) rt_sem_control(s_action_sem, RT_IPC_CMD_SET_VLIMIT, &limit);
#endif
    s_ready = true;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS && !defined(WORK_UI_HOST)
    lv_display_add_event_cb(lv_display_get_default(), feedback_flush_started, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(lv_display_get_default(), feedback_invalidated, LV_EVENT_INVALIDATE_AREA, NULL);
#endif
    if (ui_model_selection_subject())
        lv_subject_add_observer(ui_model_selection_subject(), selection_changed, NULL);
    refresh_views();
    refresh_evidence();
}
void ui_main_request_descriptor_scroll_test(int start)
{
    s_scroll_request = start ? 1 : 0;
    ui_main_wake_owner();
}

static bool visible_box(lv_obj_t *obj, lv_area_t *box, bool *clipped)
{
    lv_obj_get_coords(obj, box);
    lv_area_t intersection = *box;
    for (lv_obj_t *p = obj; p; p = lv_obj_get_parent(p)) {
        if (lv_obj_is_hidden(p)) return false;
        lv_area_t a;
        lv_obj_get_coords(p, &a);
        if (intersection.x1 < a.x1) intersection.x1 = a.x1;
        if (intersection.y1 < a.y1) intersection.y1 = a.y1;
        if (intersection.x2 > a.x2) intersection.x2 = a.x2;
        if (intersection.y2 > a.y2) intersection.y2 = a.y2;
    }
    if (intersection.x1 < 0) intersection.x1 = 0;
    if (intersection.y1 < 0) intersection.y1 = 0;
    if (intersection.x2 > 799) intersection.x2 = 799;
    if (intersection.y2 > 479) intersection.y2 = 479;
    if (intersection.x1 > intersection.x2 || intersection.y1 > intersection.y2) return false;
    *clipped = intersection.x1 != box->x1 || intersection.y1 != box->y1 ||
        intersection.x2 != box->x2 || intersection.y2 != box->y2;
    return true;
}
typedef struct { size_t first, max; ui_inspect_item_t *out; unsigned count; } inspect_walk_t;
static void inspect_object(lv_obj_t *obj, inspect_walk_t *walk, unsigned depth)
{
    if (!obj || depth > 18 || lv_obj_is_hidden(obj)) return;
    lv_area_t box;
    bool clipped = false;
    if (!visible_box(obj, &box, &clipped)) return;
    bool label = lv_obj_check_type(obj, &lv_label_class);
    bool touch = lv_obj_check_type(obj, &lv_button_class) ||
        lv_obj_check_type(obj, &lv_roller_class) || lv_obj_check_type(obj, &lv_textarea_class);
    if (label || touch) {
        const char *text = label ? lv_label_get_text(obj) : "";
        if (touch && lv_obj_get_child_count(obj) && lv_obj_check_type(lv_obj_get_child(obj, 0), &lv_label_class))
            text = lv_label_get_text(lv_obj_get_child(obj, 0));
        bool password=false;
        for(lv_obj_t *parent=obj;parent;parent=lv_obj_get_parent(parent))
            if(lv_obj_check_type(parent,&lv_textarea_class)){password=true;break;}
        /* Independent of the on-screen Show toggle; this text is exported
         * over UART/API and into regression reports. */
        if(password)text="[password hidden]";
        if (walk->count >= walk->first && walk->count - walk->first < walk->max) {
            ui_inspect_item_t *item = &walk->out[walk->count - walk->first];
            memset(item, 0, sizeof(*item));
            text_copy(item->text, sizeof(item->text), text);
            item->x = (int16_t)box.x1; item->y = (int16_t)box.y1;
            item->w = (int16_t)(box.x2 - box.x1 + 1); item->h = (int16_t)(box.y2 - box.y1 + 1);
            item->touch = touch; item->clipped = clipped;
            item->disabled = lv_obj_has_state(obj, LV_STATE_DISABLED);
            const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
            item->font_px = font == UI_FONT_14 ? 14 : font == UI_FONT_16 ? 16 :
                font == UI_FONT_20 ? 20 : font == UI_FONT_24 ? 24 : (font ? font->line_height : 0);
            if (label && !password && font && lv_label_get_long_mode(obj) != LV_LABEL_LONG_MODE_DOTS) {
                lv_point_t needed;
                lv_text_get_size(&needed, text, font, lv_obj_get_style_text_letter_space(obj, 0),
                    lv_obj_get_style_text_line_space(obj, 0), lv_obj_get_content_width(obj), LV_TEXT_FLAG_NONE);
                if (needed.y > lv_obj_get_content_height(obj) + 1) item->clipped |= 2;
            }
        }
        walk->count++;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++)
        inspect_object(lv_obj_get_child(obj, i), walk, depth + 1);
}
unsigned ui_main_inspect(size_t first, size_t max, ui_inspect_item_t *out)
{
    lv_obj_update_layout(lv_screen_active());
    inspect_walk_t walk = {first, max, out, 0};
    inspect_object(lv_screen_active(), &walk, 0);
    char semantic[2048];
    semantic[0] = '\0';
    const ui_view_state_t *v = ui_model_get();
    if (v->page == UI_PAGE_DESCRIPTORS) scr_descriptors_debug_semantic(semantic, sizeof(semantic));
    else if (v->page == UI_PAGE_DIAGNOSTICS) scr_events_debug_semantic(semantic, sizeof(semantic));
    if (v->detail == UI_DETAIL_BYTES) ui_hex_semantic(semantic, sizeof(semantic));
    /* Split canvas/model semantics into bounded pages just like widget text. */
    const char *p = semantic;
    while (*p) {
        char piece[384];
        text_copy(piece, sizeof(piece), p);
        size_t length = strlen(piece);
        if (!length) break;
        if (walk.count >= first && walk.count - first < max) {
            ui_inspect_item_t *item = &out[walk.count - first];
            memset(item, 0, sizeof(*item));
            memcpy(item->text, piece, length + 1);
            item->canvas = 1;
        }
        walk.count++; p += length;
    }
    return walk.count;
}
int ui_main_state_json(char *buffer, size_t capacity)
{
    const ui_view_state_t *v = ui_model_get();
    const usb_evidence_snapshot_t *s = v->snapshot;
    const usb_evidence_raw_event_t *e = ui_model_selected_event();
    /* page_create_ms[0] is the initially visible home shell constructor;
     * home_complete_ms separately records when its deferred timeline finished. */
    return snprintf(buffer, capacity,
        "{\"session\":%lu,\"revision\":%lu,\"epoch\":%lu,\"page\":%u,\"detail\":%u,\"frozen\":%u,\"updates\":%u,\"stage\":%d,\"event\":%lu,\"node\":%d,\"field\":%d,\"rule\":%d,\"offset\":%u,\"length\":%u,\"range_valid\":%u,\"reduced_motion\":%u,\"feedback_n\":%lu,\"feedback_p95_ms\":%u,\"feedback_max_ms\":%lu,\"page_create_ms\":[%lu,%lu,%lu,%lu],\"home_complete_ms\":%lu}",
        s ? (unsigned long)s->session_id : 0, s ? (unsigned long)s->revision : 0,
        (unsigned long)v->snapshot_epoch, v->page, v->detail, v->frozen, v->updates_available,
        v->stage, e ? (unsigned long)e->sequence : 0, v->node_index, v->field_index,
        v->rule_index, v->byte_offset, v->byte_length,
        e ? (unsigned)ui_model_range_valid(e->sequence, v->byte_offset, v->byte_length) : 0,
        v->reduced_motion, (unsigned long)s_feedback_samples, s_feedback_p95,
        (unsigned long)s_feedback_max,(unsigned long)s_page_create_ms[0],
        (unsigned long)s_page_create_ms[1],(unsigned long)s_page_create_ms[2],
        (unsigned long)s_page_create_ms[3],(unsigned long)s_home_complete_ms);
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
rt_err_t ui_main_debug_navigate(int page, rt_uint32_t timeout, rt_uint32_t *children,
    char labels[UI_MAIN_DEBUG_LABEL_MAX][UI_MAIN_DEBUG_LABEL_TEXT_MAX],
    rt_uint8_t *count, rt_uint8_t *timeline)
{
    if (page < 0 || page >= UI_PAGE_COUNT || !s_page_sem) return -RT_EINVAL;
    rt_sem_take(s_page_sem, 0); s_page_request = page;
    ui_main_wake_owner();
    if (rt_sem_take(s_page_sem, rt_tick_from_millisecond(timeout)) != RT_EOK) return -RT_ETIMEOUT;
    if (s_page_active != page) return -RT_ERROR;
    if (children) *children = s_page_children;
    if (count) *count = s_page_label_count;
    if (timeline) *timeline = s_page_timeline;
    if (labels) memcpy(labels, s_page_labels, sizeof(s_page_labels));
    return RT_EOK;
}
rt_err_t ui_main_debug_capture_framebuffer(rt_uint32_t timeout, rt_uint32_t *generation,
    rt_uint32_t *address, rt_uint32_t *bytes, rt_uint32_t *crc)
{
    if (!s_frame_sem) return -RT_EINVAL;
    rt_sem_take(s_frame_sem, 0); s_frame_request = true;
    ui_main_wake_owner();
    if (rt_sem_take(s_frame_sem, rt_tick_from_millisecond(timeout)) != RT_EOK) return -RT_ETIMEOUT;
    if (s_frame_result != RT_EOK) return s_frame_result;
    if (generation) *generation = s_frame_info.generation;
    if (address) *address = s_frame_info.source_address;
    if (bytes) *bytes = s_frame_info.byte_count;
    if (crc) *crc = s_frame_info.crc32;
    return RT_EOK;
}
rt_err_t ui_main_debug_inspect(int page, unsigned first, rt_uint32_t timeout,
    ui_inspect_item_t out[8], unsigned *total, unsigned *returned, char state[384])
{
    if (!s_inspect_sem || page < 0 || page >= UI_PAGE_COUNT) return -RT_EINVAL;
    rt_sem_take(s_inspect_sem, 0); s_inspect_first = first; s_inspect_page = page;
    ui_main_wake_owner();
    if (rt_sem_take(s_inspect_sem, rt_tick_from_millisecond(timeout)) != RT_EOK) return -RT_ETIMEOUT;
    if (total) *total = s_inspect_total;
    if (returned) *returned = s_inspect_returned;
    if (out) memcpy(out, s_inspect_reply, sizeof(s_inspect_reply));
    if (state) memcpy(state, s_debug_state, sizeof(s_debug_state));
    return RT_EOK;
}
rt_err_t ui_main_debug_action(ui_action_t action, int32_t arg, rt_uint32_t timeout, char state[384])
{
    if (!s_action_sem) return -RT_EINVAL;
    rt_tick_t ticks=rt_tick_from_millisecond(timeout);
    rt_enter_critical();
    if (!ui_debug_request_submit(&s_action_request,(uint32_t)rt_tick_get(),(uint32_t)ticks)) {
        rt_exit_critical();return -RT_EBUSY;
    }
    (void)rt_sem_take(s_action_sem,0);
    s_debug_action = action; s_debug_argument = arg;
    rt_exit_critical();
    ui_main_wake_owner();
    rt_err_t wait=rt_sem_take(s_action_sem,ticks);
    rt_enter_critical();
    if (wait != RT_EOK) {
        ui_request_timeout_t outcome=ui_debug_request_timeout(&s_action_request);
        if (outcome != UI_REQUEST_COMPLETED) {
            rt_exit_critical();return outcome==UI_REQUEST_IN_FLIGHT?-RT_EBUSY:-RT_ETIMEOUT;
        }
        (void)rt_sem_take(s_action_sem,0);
    }
    rt_err_t result=s_action_result;
    if (state) memcpy(state, s_action_state, sizeof(s_action_state));
    if (!ui_debug_request_consume(&s_action_request)) result=-RT_ERROR;
    rt_exit_critical();return result;
}
#endif
void ui_main_update(void)
{
    if (!s_ready) return;
    apply_startup_limited_request();
    feedback_poll();
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    /* Run the early-navigation test in the real owner, independent of slow
     * startup UART output. No fake input/data and no external LVGL thread. */
    if (s_preload.probe > 0 && s_preload.probe < 5) {
        if (lv_tick_elaps(s_probe_started) > 6000U) s_preload.probe = 6;
        else if (s_preload.probe == 1 && lv_tick_elaps(s_probe_tick) >= 100U) {
            s_preload.probe_cancel_steps = s_preload.steps[UI_PAGE_SETTINGS];
            s_preload.probe_fg_before = s_preload.foreground_steps;
            if (!s_page_ready[UI_PAGE_SETTINGS] && s_preload.probe_cancel_steps > 0) {
                ui_model_apply_action(UI_ACTION_PAGE, UI_PAGE_LIVE);
                s_preload.probe = 2; s_probe_tick = lv_tick_get();
            } else s_preload.probe = 6;
        } else if (s_preload.probe == 2 && lv_tick_elaps(s_probe_tick) >= 80U) {
            ui_model_apply_action(UI_ACTION_PAGE, UI_PAGE_DIAGNOSTICS);
            s_preload.probe = 3;
        } else if (s_preload.probe == 3 && s_page_ready[UI_PAGE_DIAGNOSTICS]) {
            s_preload.probe_fg_after = s_preload.foreground_steps;
            s_preload.probe = 4; s_probe_tick = lv_tick_get();
        } else if (s_preload.probe == 4 && lv_tick_elaps(s_probe_tick) >= 80U) {
            ui_model_apply_action(UI_ACTION_PAGE, UI_PAGE_LIVE);
            s_preload.probe = s_preload.probe_fg_after > s_preload.probe_fg_before ? 5 : 6;
        }
        if (s_preload.probe == 6) ui_model_apply_action(UI_ACTION_PAGE, UI_PAGE_LIVE);
    }
#endif
    if (s_views_pending) { s_views_pending = false; refresh_views(); }
    refresh_evidence();
    int request = s_scroll_request;
    if (request >= 0) {
        if (request) ui_model_apply_action(UI_ACTION_PAGE, UI_PAGE_DESCRIPTORS);
        if (!request || s_page_ready[UI_PAGE_DESCRIPTORS]) {
            s_scroll_request = -1;
            scr_descriptors_set_scroll_test(request != 0);
        }
    }
    if (s_page_ready[UI_PAGE_DESCRIPTORS] &&
        ui_model_get()->page == UI_PAGE_DESCRIPTORS)
        scr_descriptors_poll_scroll_test();
    scr_wifi_poll();
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    rt_enter_critical();
    ui_request_claim_t claim=s_action_sem?
        ui_debug_request_claim(&s_action_request,(uint32_t)rt_tick_get()):UI_REQUEST_NONE;
    if (claim==UI_REQUEST_EXPIRED) {
        s_action_result=-RT_ETIMEOUT;rt_sem_release(s_action_sem);
    }
    rt_exit_critical();
    if (claim==UI_REQUEST_EXECUTE) {
        s_commit_success = false;
        if (s_debug_action == UI_ACTION_PRELOAD_PROBE) {
            if (s_preload.probe == 0 && !s_page_ready[UI_PAGE_SETTINGS] &&
                !s_page_ready[UI_PAGE_DIAGNOSTICS]) {
                s_probe_started = s_probe_tick = lv_tick_get(); s_preload.probe = 1;
                s_commit_success = ui_model_apply_action(UI_ACTION_PAGE, UI_PAGE_SETTINGS);
            }
        }
        if (s_debug_action == UI_ACTION_FEEDBACK_RESET) {
            /* Explicit phase boundary: discard any unfinished old sample,
             * not error counters or cold-path evidence kept by the host. */
            s_feedback_samples = s_feedback_max = s_feedback_p95 = 0;
#if !defined(WORK_UI_HOST)
            memset(s_feedback_hist, 0, sizeof(s_feedback_hist));
            s_feedback_pending = s_feedback_flushed = false;
#endif
            s_commit_success = true;
        }
        if (s_debug_action >= UI_ACTION_WIFI_EDITOR &&
            s_debug_action <= UI_ACTION_WIFI_CONNECT_SAVED_SELECTED)
            (void)ensure_page_created(UI_PAGE_SETTINGS, NULL);
        if (s_debug_action == UI_ACTION_FEEDBACK_RESET || s_debug_action == UI_ACTION_PRELOAD_PROBE) { /* handled above */ }
        else if (s_debug_action == UI_ACTION_WIFI_EDITOR)
            s_commit_success = scr_wifi_debug_editor((unsigned)s_debug_argument);
        else if(s_debug_action==UI_ACTION_WIFI_SELECT_NETWORK)
            s_commit_success=scr_wifi_debug_select_network((unsigned)s_debug_argument);
        else if(s_debug_action==UI_ACTION_WIFI_CONFIRM_SCREEN)
            s_commit_success=scr_wifi_debug_confirm_screen((unsigned)s_debug_argument);
        else if(s_debug_action==UI_ACTION_WIFI_VIEW)
            s_commit_success=scr_wifi_debug_view((unsigned)s_debug_argument);
        else if(s_debug_action==UI_ACTION_WIFI_SELECT_SAVED)
            s_commit_success=scr_wifi_debug_select_saved((uint32_t)s_debug_argument);
        else if(s_debug_action==UI_ACTION_WIFI_CONNECT_SAVED_SELECTED)
            s_commit_success=s_debug_argument==0 && scr_wifi_debug_connect_saved_selected();
        else {
            ui_model_post(s_debug_action, s_debug_argument);
            ui_deferred_process();
        }
        if (s_debug_action == UI_ACTION_FOLLOW) refresh_evidence();
        ui_main_state_json(s_action_state, sizeof(s_action_state));
        s_action_result = s_commit_success ? RT_EOK : -RT_EINVAL;
        rt_enter_critical();
        if (ui_debug_request_finish(&s_action_request)) rt_sem_release(s_action_sem);
        rt_exit_critical();
    }
    request = s_page_request;
    if (request >= 0 && request < UI_PAGE_COUNT && s_page_sem) {
        ui_model_apply_action(UI_ACTION_PAGE, request);
        /* Debug navigation promises a complete page, unlike an ordinary
         * asynchronous navigation action. Do not acknowledge a loading shell. */
        if (ui_main_page_ready((unsigned)request)) {
        s_page_request = -1;
        s_page_active = ui_model_get()->page;
        s_page_children = s_pages[request] ?
            lv_obj_get_child_count(s_pages[request]) : 0U;
        s_page_timeline = request == 0 ? scr_home_debug_timeline_visible_count() : 0;
        ui_inspect_item_t items[2];
        unsigned total = ui_main_inspect(0, 0, NULL);
        s_page_label_count = 0;
        memset(s_page_labels, 0, sizeof(s_page_labels));
        for (unsigned i = 0; i < total && s_page_label_count < UI_MAIN_DEBUG_LABEL_MAX; i += 2) {
            ui_main_inspect(i, 2, items);
            for (unsigned j = 0; j < 2 && i + j < total && s_page_label_count < UI_MAIN_DEBUG_LABEL_MAX; j++)
                if (!items[j].touch && items[j].text[0])
                    text_copy(s_page_labels[s_page_label_count++], UI_MAIN_DEBUG_LABEL_TEXT_MAX, items[j].text);
        }
        rt_sem_release(s_page_sem);
        }
    }
    request = s_inspect_page;
    if (request >= 0 && request < UI_PAGE_COUNT && s_inspect_sem) {
        if ((int)ui_model_get()->page != request) ui_model_apply_action(UI_ACTION_PAGE, request);
        if (ui_main_page_ready((unsigned)request)) {
        s_inspect_page = -1;
        memset(s_inspect_reply, 0, sizeof(s_inspect_reply));
        s_inspect_total = ui_main_inspect(s_inspect_first, 8, s_inspect_reply);
        s_inspect_returned = s_inspect_total > s_inspect_first ? s_inspect_total - s_inspect_first : 0;
        if (s_inspect_returned > 8) s_inspect_returned = 8;
        ui_main_state_json(s_debug_state, sizeof(s_debug_state));
        rt_sem_release(s_inspect_sem);
        }
    }
    if (s_frame_request && s_frame_sem) {
        s_frame_request = false;
        s_frame_result = scr_wifi_capture_allowed()?
            lvgl_framebuffer_capture_publish(&s_frame_info):-RT_EBUSY;
        rt_sem_release(s_frame_sem);
    }
#endif
}
