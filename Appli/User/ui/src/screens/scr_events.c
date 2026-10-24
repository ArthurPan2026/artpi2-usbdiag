#include <stdarg.h>
#include <limits.h>
#include <string.h>

#include "scr_events.h"
#include "ui_model.h"
#include "../ui_widgets.h"

/* Keep the size names explicit on this screen; the shared font assets are
 * generated as 14/16/20/24 px Noto Sans SC subsets. */
#define F_META14  UI_FONT_14
#define F_BODY16  UI_FONT_16
#define F_TITLE20 UI_FONT_20
#define F_HERO24  UI_FONT_24

#define DIAG_PAGE_W       800
#define DIAG_PAGE_H       372
#define DIAG_PAD          12
#define DIAG_CONTENT_W    (DIAG_PAGE_W - 2 * DIAG_PAD)
#define DIAG_SUMMARY_H    28
#define DIAG_TABS_Y       44
#define DIAG_TABS_H       48
#define DIAG_FILTER_Y     96
#define DIAG_FILTER_H     48
#define DIAG_LIST_Y       152
#define DIAG_LIST_H       (DIAG_PAGE_H - DIAG_PAD - DIAG_LIST_Y)
#define DIAG_ROW_H        50
#define DIAG_VISIBLE_ROWS 4
#define DIAG_ROW_SLOTS    6
#define DIAG_FILTER_COUNT 4
#define DIAG_NONE         (-1)
#define EVENTS_CREATE_STEP_COUNT (16U + 5U * DIAG_ROW_SLOTS)

typedef struct {
    ui_action_t action;
    int32_t argument;
} diag_action_binding_t;

static lv_obj_t *s_page;
static lv_obj_t *s_summary_panel;
static lv_obj_t *s_heading;
static lv_obj_t *s_summary;
static lv_obj_t *s_tab_line;
static lv_obj_t *s_filter_line;
static lv_obj_t *s_tabs[2];
static lv_obj_t *s_rule_filters[DIAG_FILTER_COUNT];
static lv_obj_t *s_event_filters[2];
static lv_obj_t *s_list;
static lv_obj_t *s_spacer;
static lv_obj_t *s_empty;
static lv_obj_t *s_rule_rows[DIAG_ROW_SLOTS];
static lv_obj_t *s_rule_titles[DIAG_ROW_SLOTS];
static lv_obj_t *s_rule_verdicts[DIAG_ROW_SLOTS];
static lv_obj_t *s_rule_details[DIAG_ROW_SLOTS];
static lv_obj_t *s_event_rows[DIAG_ROW_SLOTS];
static lv_obj_t *s_event_titles[DIAG_ROW_SLOTS];
static lv_obj_t *s_event_details[DIAG_ROW_SLOTS];
static uint16_t s_first;
static uint16_t s_visible_count;
static int32_t s_scroll_y;
static uint32_t s_counts[5];
static uint32_t s_session_id;
static int s_last_tab = DIAG_NONE;
static int s_last_filter = DIAG_NONE;
static int s_last_stage = DIAG_NONE;
static int s_last_stage_filter = DIAG_NONE;
static bool s_rendering;
static bool s_reset_pending;

typedef enum {
    EVENTS_CREATE_SUMMARY_PANEL,
    EVENTS_CREATE_HEADING,
    EVENTS_CREATE_SUMMARY,
    EVENTS_CREATE_TAB_LINE,
    EVENTS_CREATE_TAB_RULES,
    EVENTS_CREATE_TAB_EVENTS,
    EVENTS_CREATE_FILTER_LINE,
    EVENTS_CREATE_RULE_FILTER,
    EVENTS_CREATE_EVENT_FILTER,
    EVENTS_CREATE_LIST,
    EVENTS_CREATE_SPACER,
    EVENTS_CREATE_EMPTY,
    EVENTS_CREATE_RULE_ROW,
    EVENTS_CREATE_EVENT_ROW
} events_create_phase_t;

static events_create_phase_t s_create_phase;
static uint8_t s_create_index;
static uint8_t s_create_part;
static unsigned s_create_units_done;
static bool s_create_ready;

static const diag_action_binding_t s_tab_actions[2] = {
    { UI_ACTION_DIAGNOSTIC_TAB, 0 },
    { UI_ACTION_DIAGNOSTIC_TAB, 1 }
};
static const diag_action_binding_t s_rule_filter_actions[DIAG_FILTER_COUNT] = {
    { UI_ACTION_RULE_FILTER, 0 },
    { UI_ACTION_RULE_FILTER, 1 },
    { UI_ACTION_RULE_FILTER, 2 },
    { UI_ACTION_RULE_FILTER, 3 }
};
static const diag_action_binding_t s_event_filter_actions[2] = {
    { UI_ACTION_EVENT_FILTER, 0 },
    { UI_ACTION_EVENT_FILTER, 1 }
};
static const char *const s_rule_filter_text[DIAG_FILTER_COUNT] = {
    "全部", "FAIL", "未观测", "PASS"
};
static const char *const s_event_filter_text[2] = {
    "全部阶段", "当前阶段"
};

static const usb_evidence_snapshot_t *snapshot(void)
{
    return ui_model_snapshot();
}

static const ui_view_state_t *view(void)
{
    return ui_model_get();
}

static void label_set(lv_obj_t *label, const char *text)
{
    if (label) ui_label_set_safe(label, text ? text : "");
}

static void append_text(char *buffer, size_t capacity, size_t *used,
                        const char *format, ...)
{
    int written;
    va_list arguments;

    if (!buffer || !used || !format || capacity == 0U || *used >= capacity - 1U)
        return;

    va_start(arguments, format);
    written = rt_vsnprintf(buffer + *used, capacity - *used, format, arguments);
    va_end(arguments);
    if (written < 0) {
        buffer[*used] = '\0';
        return;
    }
    if ((size_t)written >= capacity - *used) {
        *used = capacity - 1U;
        buffer[*used] = '\0';
        return;
    }
    *used += (size_t)written;
}

static void add_release_validated_cb(lv_obj_t *button, lv_event_cb_t callback,
                                     void *user_data)
{
    if (!button || !callback) return;
    (void)lv_obj_remove_event(button, 0U);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
}

static uint16_t snapshot_event_count(const usb_evidence_snapshot_t *s)
{
    if (!s) return 0U;
    return s->raw_event_count > USB_EVIDENCE_SNAPSHOT_EVENT_COUNT ?
           USB_EVIDENCE_SNAPSHOT_EVENT_COUNT : s->raw_event_count;
}

static uint32_t rule_filter(const ui_view_state_t *v)
{
    return v && v->rule_filter <= 3U ? v->rule_filter : 0U;
}

static uint8_t diagnostic_tab(const ui_view_state_t *v)
{
    return v && v->diagnostic_tab <= 1U ? v->diagnostic_tab : 0U;
}

static const char *direction_name(usb_evidence_data_direction_t direction)
{
    switch (direction) {
    case USB_EVIDENCE_DATA_DEVICE_TO_HOST: return "设备到主机";
    case USB_EVIDENCE_DATA_HOST_TO_DEVICE: return "主机到设备";
    default: return "无数据阶段";
    }
}

static lv_color_t verdict_color(usb_rule_verdict_t verdict)
{
    switch (verdict) {
    case USB_RULE_FAIL: return C_RED;
    case USB_RULE_NOT_OBSERVED: return C_ORANGE;
    case USB_RULE_PASS: return C_GREEN;
    case USB_RULE_NOT_APPLICABLE: return C_GRAY;
    default: return C_GRAY;
    }
}

static int rule_rank(usb_rule_verdict_t verdict)
{
    /* With “全部” selected, actionable findings stay above PASS/N/A. */
    switch (verdict) {
    case USB_RULE_FAIL: return 0;
    case USB_RULE_NOT_OBSERVED: return 1;
    case USB_RULE_PASS: return 2;
    case USB_RULE_NOT_APPLICABLE: return 3;
    default: return 4;
    }
}

static bool rule_matches_filter(const usb_evidence_rule_result_t *rule,
                                uint32_t filter)
{
    if (!rule || filter == 0U) return rule != NULL;
    if (filter == 1U) return rule->verdict == USB_RULE_FAIL;
    if (filter == 2U) return rule->verdict == USB_RULE_NOT_OBSERVED;
    return rule->verdict == USB_RULE_PASS;
}

static uint16_t rule_visible_count(const usb_evidence_snapshot_t *s,
                                  uint32_t filter)
{
    uint16_t count = 0U;
    unsigned i;
    if (!s) return 0U;
    for (i = 0U; i < USB_EVIDENCE_RULE_COUNT; ++i)
        if (rule_matches_filter(&s->rules[i], filter)) ++count;
    return count;
}

static int rule_visible_at(const usb_evidence_snapshot_t *s, uint32_t filter,
                           uint16_t ordinal)
{
    int rank;
    unsigned i;
    uint16_t seen = 0U;

    if (!s) return DIAG_NONE;
    for (rank = 0; rank <= 4; ++rank) {
        for (i = 0U; i < USB_EVIDENCE_RULE_COUNT; ++i) {
            const usb_evidence_rule_result_t *rule = &s->rules[i];
            if (!rule_matches_filter(rule, filter)) continue;
            if (filter != 0U && rule_rank(rule->verdict) != (int)filter - 1)
                continue;
            if (filter == 0U && rule_rank(rule->verdict) != rank) continue;
            if (seen++ == ordinal) return (int)i;
        }
        if (filter != 0U) break;
    }
    return DIAG_NONE;
}

static bool event_matches_filter(const usb_evidence_raw_event_t *event,
                                 const ui_view_state_t *v)
{
    if (!event) return false;
    if (!v || !v->event_stage_filter) return true;
    return v->stage >= 0 && ui_model_event_stage(event) == v->stage;
}

static uint16_t event_visible_count(const usb_evidence_snapshot_t *s,
                                    const ui_view_state_t *v)
{
    uint16_t i, count = 0U, raw_count = snapshot_event_count(s);
    for (i = 0U; i < raw_count; ++i)
        if (event_matches_filter(&s->raw_events[i], v)) ++count;
    return count;
}

static int event_visible_at(const usb_evidence_snapshot_t *s,
                            const ui_view_state_t *v, uint16_t ordinal)
{
    uint16_t i, seen = 0U, raw_count = snapshot_event_count(s);
    for (i = 0U; i < raw_count; ++i) {
        if (!event_matches_filter(&s->raw_events[i], v)) continue;
        if (seen++ == ordinal) return (int)i;
    }
    return DIAG_NONE;
}

static uint16_t current_list_count(const usb_evidence_snapshot_t *s,
                                   const ui_view_state_t *v)
{
    if (diagnostic_tab(v) == 0U) return rule_visible_count(s, rule_filter(v));
    return event_visible_count(s, v);
}

static int current_list_index(const usb_evidence_snapshot_t *s,
                              const ui_view_state_t *v, uint16_t ordinal)
{
    if (diagnostic_tab(v) == 0U)
        return rule_visible_at(s, rule_filter(v), ordinal);
    return event_visible_at(s, v, ordinal);
}

static void count_rules(const usb_evidence_snapshot_t *s)
{
    unsigned i;
    memset(s_counts, 0, sizeof(s_counts));
    if (!s) return;
    s_counts[0] = USB_EVIDENCE_RULE_COUNT;
    for (i = 0U; i < USB_EVIDENCE_RULE_COUNT; ++i) {
        switch (s->rules[i].verdict) {
        case USB_RULE_PASS: ++s_counts[1]; break;
        case USB_RULE_FAIL: ++s_counts[2]; break;
        case USB_RULE_NOT_OBSERVED: ++s_counts[3]; break;
        case USB_RULE_NOT_APPLICABLE: ++s_counts[4]; break;
        default: break;
        }
    }
}

static void render_summary(void)
{
    char text[144];
    size_t used = 0U;

    if (!s_create_ready) return;
    if (s_counts[0] == 0U) {
        rt_snprintf(text, sizeof(text), "等待证据");
    } else if (s_counts[1] == s_counts[0]) {
        rt_snprintf(text, sizeof(text), "规则 %lu  ·  全部通过",
                    (unsigned long)s_counts[0]);
    } else {
        text[0] = '\0';
        append_text(text, sizeof(text), &used, "规则 %lu",
                    (unsigned long)s_counts[0]);
        if (s_counts[1])
            append_text(text, sizeof(text), &used, "  ·  PASS %lu",
                        (unsigned long)s_counts[1]);
        if (s_counts[2])
            append_text(text, sizeof(text), &used, "  ·  FAIL %lu",
                        (unsigned long)s_counts[2]);
        if (s_counts[3])
            append_text(text, sizeof(text), &used, "  ·  未观测 %lu",
                        (unsigned long)s_counts[3]);
        if (s_counts[4])
            append_text(text, sizeof(text), &used, "  ·  N/A %lu",
                        (unsigned long)s_counts[4]);
    }
    label_set(s_summary, text);
}

static void post_bound_action(lv_event_t *e)
{
    const diag_action_binding_t *binding = lv_event_get_user_data(e);
    /* LV_EVENT_CLICKED is emitted only for a completed, in-bounds release;
     * ui_model_post defers the actual selection mutation to the UI owner. */
    if (!s_create_ready || lv_event_get_code(e) != LV_EVENT_CLICKED || !binding)
        return;
    ui_model_post(binding->action, binding->argument);
}

static bool validate_row_release(lv_event_t *e, bool rules, uint16_t *index_out)
{
    const usb_evidence_snapshot_t *s;
    const ui_view_state_t *v;
    lv_obj_t *target;
    uintptr_t slot;
    uintptr_t stored_index;
    int current_index;
    uint16_t ordinal;

    if (!s_create_ready || lv_event_get_code(e) != LV_EVENT_CLICKED || !index_out)
        return false;
    slot = (uintptr_t)lv_event_get_user_data(e);
    if (slot >= DIAG_ROW_SLOTS) return false;
    target = lv_event_get_current_target(e);
    if (!target || lv_obj_is_hidden(target)) return false;

    stored_index = (uintptr_t)lv_obj_get_user_data(target);
    s = snapshot();
    v = view();
    ordinal = (uint16_t)(s_first + (uint16_t)slot);
    if (!s || ordinal >= current_list_count(s, v)) return false;
    current_index = current_list_index(s, v, ordinal);
    if (current_index < 0 || (uintptr_t)current_index != stored_index) return false;
    if (rules != (diagnostic_tab(v) == 0U)) return false;
    *index_out = (uint16_t)current_index;
    return true;
}

static void post_rule_release(lv_event_t *e)
{
    uint16_t index;
    if (validate_row_release(e, true, &index))
        ui_model_post(UI_ACTION_RULE, (int32_t)index);
}

static void post_event_release(lv_event_t *e)
{
    uint16_t index;
    if (validate_row_release(e, false, &index))
        ui_model_post(UI_ACTION_EVENT, (int32_t)index);
}

static void set_row_selection(lv_obj_t *row, bool selected)
{
    lv_obj_set_style_bg_color(row, selected ? C_RAISED : C_SURFACE, 0);
    lv_obj_set_style_border_color(row, selected ? C_BLUE : C_DIVIDER, 0);
    ui_button_set_selected(row, selected);
}

static void render_rule_row(uint8_t slot, const usb_evidence_snapshot_t *s,
                            const ui_view_state_t *v)
{
    uint16_t ordinal = (uint16_t)(s_first + slot);
    int index = rule_visible_at(s, rule_filter(v), ordinal);
    lv_obj_t *row = s_rule_rows[slot];

    if (!row) return;
    if (index < 0) {
        lv_obj_set_hidden(row, true);
        return;
    }

    const usb_evidence_rule_result_t *rule = &s->rules[index];
    const char *title = ui_model_rule_title((unsigned)index);
    const char *verdict = ui_model_verdict_name(rule->verdict);
    char detail[208];
    bool selected = v && v->detail == UI_DETAIL_RULE &&
                    v->rule_index == index;

    lv_obj_set_hidden(row, false);
    lv_obj_set_pos(row, 0, (int32_t)(s_first + slot) * DIAG_ROW_H);
    lv_obj_set_size(row, DIAG_CONTENT_W - 12, DIAG_ROW_H);
    lv_obj_set_user_data(row, (void *)(uintptr_t)index);
    set_row_selection(row, selected);

    label_set(s_rule_titles[slot], title ? title : "规则");
    lv_obj_set_style_text_color(s_rule_titles[slot], C_TEXT, 0);
    label_set(s_rule_verdicts[slot], verdict ? verdict : "UNKNOWN");
    lv_obj_set_style_text_color(s_rule_verdicts[slot],
                                verdict_color(rule->verdict), 0);

    rt_snprintf(detail, sizeof(detail), "%s  ·  %s",
                rule->rule_id ? rule->rule_id : "ID?",
                rule->observed[0] ? rule->observed : "未观测到结果");
    label_set(s_rule_details[slot], detail);
}

static void render_event_row(uint8_t slot, const usb_evidence_snapshot_t *s,
                             const ui_view_state_t *v)
{
    uint16_t ordinal = (uint16_t)(s_first + slot);
    int index = event_visible_at(s, v, ordinal);
    lv_obj_t *row = s_event_rows[slot];

    if (!row) return;
    if (index < 0) {
        lv_obj_set_hidden(row, true);
        return;
    }

    const usb_evidence_raw_event_t *event = &s->raw_events[index];
    const char *helper = ui_model_event_name(event);
    const char *capture = ui_model_capture_name(event->capture_status);
    char title[128];
    char detail[192];
    bool capture_incomplete = event->data_direction != USB_EVIDENCE_DATA_NONE &&
        event->capture_status != USB_EVIDENCE_CAPTURE_COMPLETE;
    bool selected = v && v->detail == UI_DETAIL_EVENT &&
                    v->event_index == index;

    lv_obj_set_hidden(row, false);
    lv_obj_set_pos(row, 0, (int32_t)(s_first + slot) * DIAG_ROW_H);
    lv_obj_set_size(row, DIAG_CONTENT_W - 12, DIAG_ROW_H);
    lv_obj_set_user_data(row, (void *)(uintptr_t)index);
    set_row_selection(row, selected);

    rt_snprintf(title, sizeof(title), "#%lu  ·  %s  ·  %s",
                (unsigned long)event->sequence,
                helper && helper[0] ? helper : "Host事件",
                direction_name(event->data_direction));
    label_set(s_event_titles[slot], title);

    detail[0] = '\0';
    if (event->return_status < 0 && capture_incomplete)
        rt_snprintf(detail, sizeof(detail), "请求失败  ·  %s",
                    capture ? capture : "捕获状态未知");
    else if (event->return_status < 0)
        rt_snprintf(detail, sizeof(detail), "请求失败");
    else if (capture_incomplete)
        rt_snprintf(detail, sizeof(detail), "%s",
                    capture ? capture : "捕获状态未知");
    label_set(s_event_details[slot], detail);
    lv_obj_set_hidden(s_event_details[slot], detail[0] == '\0');
}

static void list_scroll(lv_event_t *e)
{
    int32_t y;
    uint16_t max_first;
    LV_UNUSED(e);
    if (!s_create_ready || s_rendering || !s_list) return;

    y = lv_obj_get_scroll_y(s_list);
    if (y < 0) y = 0;
    max_first = s_visible_count > DIAG_VISIBLE_ROWS ?
                (uint16_t)(s_visible_count - DIAG_VISIBLE_ROWS) : 0U;
    if ((uint16_t)(y / DIAG_ROW_H) > max_first) {
        y = (int32_t)max_first * DIAG_ROW_H;
        s_rendering = true;
        lv_obj_scroll_to_y(s_list, y, LV_ANIM_OFF);
        s_rendering = false;
    }
    s_first = (uint16_t)(y / DIAG_ROW_H);
    s_scroll_y = y;
    scr_events_refresh();
}

static lv_obj_t *row_primary_label(lv_obj_t *row)
{
    lv_obj_t *label = lv_obj_get_child(row, 0);
    if (!label) label = ui_text(row, "", F_BODY16, C_TEXT);
    lv_obj_set_clickable(label, false);
    return label;
}

static void configure_title_label(lv_obj_t *label, int x, int y, int width)
{
    if (!label) return;
    lv_obj_set_align(label, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, 20);
    lv_obj_set_style_text_font(label, F_BODY16, 0);
    lv_obj_set_style_text_color(label, C_TEXT, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_clickable(label, false);
}

static void configure_detail_label(lv_obj_t *label, int x, int y,
                                   int width, lv_color_t color)
{
    if (!label) return;
    lv_obj_set_align(label, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, 18);
    lv_obj_set_style_text_font(label, F_META14, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_clickable(label, false);
}

static void configure_verdict_label(lv_obj_t *label, int x, int y, int width)
{
    if (!label) return;
    lv_obj_set_align(label, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, 18);
    lv_obj_set_style_text_font(label, F_META14, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_clickable(label, false);
}

lv_obj_t *scr_events_create_begin(lv_obj_t *parent)
{
    if (s_page) return s_page;

    s_create_ready = false;
    s_create_phase = EVENTS_CREATE_SUMMARY_PANEL;
    s_create_index = 0U;
    s_create_part = 0U;
    s_create_units_done = 0U;
    s_first = 0U;
    s_scroll_y = 0;
    s_visible_count = 0U;
    s_last_tab = s_last_filter = s_last_stage = s_last_stage_filter = DIAG_NONE;
    s_reset_pending = false;

    s_page = lv_obj_create(parent);
    if (!s_page) return NULL;
    lv_obj_set_hidden(s_page, true);
    lv_obj_set_size(s_page, DIAG_PAGE_W, DIAG_PAGE_H);
    lv_obj_set_style_bg_color(s_page, C_BG, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_set_layout(s_page, LV_LAYOUT_NONE);
    return s_page;
}

bool scr_events_create_step(void)
{
    uint8_t i;

    if (s_create_ready) return true;
    if (!s_page) return false;

    switch (s_create_phase) {
    case EVENTS_CREATE_SUMMARY_PANEL:
        s_summary_panel = ui_panel(s_page, DIAG_PAD, DIAG_PAD,
                                   DIAG_CONTENT_W, DIAG_SUMMARY_H);
        lv_obj_set_size(s_summary_panel, DIAG_CONTENT_W, DIAG_SUMMARY_H);
        lv_obj_set_style_bg_color(s_summary_panel, C_SURFACE, 0);
        lv_obj_set_style_border_color(s_summary_panel, C_DIVIDER, 0);
        lv_obj_set_style_pad_all(s_summary_panel, 0, 0);
        lv_obj_set_layout(s_summary_panel, LV_LAYOUT_NONE);
        s_create_phase = EVENTS_CREATE_HEADING;
        break;
    case EVENTS_CREATE_HEADING:
        s_heading = ui_text(s_summary_panel, "诊断", F_TITLE20, C_TEXT);
        lv_obj_set_pos(s_heading, 10, 1);
        lv_obj_set_size(s_heading, 76, 26);
        s_create_phase = EVENTS_CREATE_SUMMARY;
        break;
    case EVENTS_CREATE_SUMMARY:
        s_summary = ui_text(s_summary_panel,
                            "规则 0   FAIL 0   未观测 0   PASS 0   N/A 0",
                            F_META14, C_GRAY);
        lv_obj_set_pos(s_summary, 88, 4);
        lv_obj_set_size(s_summary, DIAG_CONTENT_W - 100, 19);
        lv_label_set_long_mode(s_summary, LV_LABEL_LONG_MODE_DOTS);
        s_create_phase = EVENTS_CREATE_TAB_LINE;
        break;
    case EVENTS_CREATE_TAB_LINE:
        s_tab_line = lv_obj_create(s_page);
        lv_obj_set_pos(s_tab_line, DIAG_PAD, DIAG_TABS_Y);
        lv_obj_set_size(s_tab_line, DIAG_CONTENT_W, DIAG_TABS_H);
        lv_obj_set_style_bg_opa(s_tab_line, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_tab_line, 0, 0);
        lv_obj_set_style_pad_all(s_tab_line, 0, 0);
        lv_obj_set_layout(s_tab_line, LV_LAYOUT_NONE);
        lv_obj_set_scrollable(s_tab_line, false);
        s_create_phase = EVENTS_CREATE_TAB_RULES;
        s_create_index = 0U;
        break;
    case EVENTS_CREATE_TAB_RULES:
    case EVENTS_CREATE_TAB_EVENTS:
        {
            i = (uint8_t)(s_create_phase == EVENTS_CREATE_TAB_RULES ? 0U : 1U);
            s_tabs[i] = ui_button(s_tab_line, i == 0U ? "规则" : "事件", 116,
                                  UI_ACTION_DIAGNOSTIC_TAB, i);
            lv_obj_set_pos(s_tabs[i], i * 124, 0);
            lv_obj_set_size(s_tabs[i], 116, DIAG_TABS_H);
            add_release_validated_cb(s_tabs[i], post_bound_action,
                                     (void *)&s_tab_actions[i]);
            if (i == 0U) s_create_phase = EVENTS_CREATE_TAB_EVENTS;
            else s_create_phase = EVENTS_CREATE_FILTER_LINE;
        }
        break;
    case EVENTS_CREATE_FILTER_LINE:
        s_filter_line = lv_obj_create(s_page);
        lv_obj_set_pos(s_filter_line, DIAG_PAD, DIAG_FILTER_Y);
        lv_obj_set_size(s_filter_line, DIAG_CONTENT_W, DIAG_FILTER_H);
        lv_obj_set_style_bg_opa(s_filter_line, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_filter_line, 0, 0);
        lv_obj_set_style_pad_all(s_filter_line, 0, 0);
        lv_obj_set_layout(s_filter_line, LV_LAYOUT_NONE);
        lv_obj_set_scrollable(s_filter_line, false);
        s_create_phase = EVENTS_CREATE_RULE_FILTER;
        s_create_index = 0U;
        break;
    case EVENTS_CREATE_RULE_FILTER:
        {
            i = s_create_index;
            s_rule_filters[i] = ui_button(s_filter_line, s_rule_filter_text[i], 112,
                                          UI_ACTION_RULE_FILTER, i);
            lv_obj_set_pos(s_rule_filters[i], i * 120, 0);
            lv_obj_set_size(s_rule_filters[i], 112, DIAG_FILTER_H);
            add_release_validated_cb(s_rule_filters[i], post_bound_action,
                                     (void *)&s_rule_filter_actions[i]);
            if (++s_create_index >= DIAG_FILTER_COUNT) {
                s_create_index = 0U;
                s_create_phase = EVENTS_CREATE_EVENT_FILTER;
            }
        }
        break;
    case EVENTS_CREATE_EVENT_FILTER:
        {
            i = s_create_index;
            s_event_filters[i] = ui_button(s_filter_line, s_event_filter_text[i], 148,
                                           UI_ACTION_EVENT_FILTER, i);
            lv_obj_set_pos(s_event_filters[i], i * 156, 0);
            lv_obj_set_size(s_event_filters[i], 148, DIAG_FILTER_H);
            add_release_validated_cb(s_event_filters[i], post_bound_action,
                                     (void *)&s_event_filter_actions[i]);
            if (++s_create_index >= 2U) s_create_phase = EVENTS_CREATE_LIST;
        }
        break;
    case EVENTS_CREATE_LIST:
        s_list = ui_panel(s_page, DIAG_PAD, DIAG_LIST_Y,
                          DIAG_CONTENT_W, DIAG_LIST_H);
        lv_obj_set_size(s_list, DIAG_CONTENT_W, DIAG_LIST_H);
        lv_obj_set_style_bg_color(s_list, C_SURFACE, 0);
        lv_obj_set_style_border_color(s_list, C_DIVIDER, 0);
        lv_obj_set_style_pad_all(s_list, 0, 0);
        lv_obj_set_style_pad_gap(s_list, 0, 0);
        lv_obj_set_layout(s_list, LV_LAYOUT_NONE);
        lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
        /* ui_panel defaults to a fixed card; result lists need native scrolling. */
        lv_obj_set_scrollable(s_list, true);
        lv_obj_set_scroll_elastic(s_list, false);
        lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_add_event_cb(s_list, list_scroll, LV_EVENT_SCROLL, NULL);
        s_create_phase = EVENTS_CREATE_SPACER;
        break;
    case EVENTS_CREATE_SPACER:
        s_spacer = lv_obj_create(s_list);
        lv_obj_set_pos(s_spacer, 0, 0);
        lv_obj_set_size(s_spacer, 1, DIAG_LIST_H);
        lv_obj_set_style_bg_opa(s_spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_spacer, 0, 0);
        lv_obj_set_clickable(s_spacer, false);
        lv_obj_set_scrollable(s_spacer, false);
        s_create_phase = EVENTS_CREATE_EMPTY;
        break;
    case EVENTS_CREATE_EMPTY:
        s_empty = ui_text(s_list, "等待证据", F_HERO24, C_GRAY);
        lv_obj_set_size(s_empty, DIAG_CONTENT_W - 24, 32);
        lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_clickable(s_empty, false);
        s_create_phase = EVENTS_CREATE_RULE_ROW;
        s_create_index = 0U;
        s_create_part = 0U;
        break;
    case EVENTS_CREATE_RULE_ROW:
        i = s_create_index;
        if (s_create_part == 0U) {
            s_rule_rows[i] = ui_button(s_list, "", DIAG_CONTENT_W - 12,
                                       UI_ACTION_RULE, 0);
            lv_obj_set_layout(s_rule_rows[i], LV_LAYOUT_NONE);
            lv_obj_set_style_pad_all(s_rule_rows[i], 2, 0);
            lv_obj_set_style_border_width(s_rule_rows[i], 1, 0);
            lv_obj_set_style_radius(s_rule_rows[i], 8, 0);
            s_rule_titles[i] = row_primary_label(s_rule_rows[i]);
            configure_title_label(s_rule_titles[i], 2, 1,
                                  DIAG_CONTENT_W - 152);
            add_release_validated_cb(s_rule_rows[i], post_rule_release,
                                     (void *)(uintptr_t)i);
            s_create_part = 1U;
        } else if (s_create_part == 1U) {
            s_rule_verdicts[i] = ui_text(s_rule_rows[i], "", F_META14, C_GRAY);
            configure_verdict_label(s_rule_verdicts[i], DIAG_CONTENT_W - 134,
                                    2, 110);
            s_create_part = 2U;
        } else {
            s_rule_details[i] = ui_text(s_rule_rows[i], "", F_META14, C_GRAY);
            configure_detail_label(s_rule_details[i], 2, 24,
                                   DIAG_CONTENT_W - 24, C_GRAY);
            s_create_part = 0U;
            if (++s_create_index >= DIAG_ROW_SLOTS) {
                s_create_index = 0U;
                s_create_phase = EVENTS_CREATE_EVENT_ROW;
            }
        }
        break;
    case EVENTS_CREATE_EVENT_ROW:
        i = s_create_index;
        if (s_create_part == 0U) {
            s_event_rows[i] = ui_button(s_list, "", DIAG_CONTENT_W - 12,
                                        UI_ACTION_EVENT, 0);
            lv_obj_set_layout(s_event_rows[i], LV_LAYOUT_NONE);
            lv_obj_set_style_pad_all(s_event_rows[i], 2, 0);
            lv_obj_set_style_border_width(s_event_rows[i], 1, 0);
            lv_obj_set_style_radius(s_event_rows[i], 8, 0);
            s_event_titles[i] = row_primary_label(s_event_rows[i]);
            configure_title_label(s_event_titles[i], 2, 1,
                                  DIAG_CONTENT_W - 24);
            add_release_validated_cb(s_event_rows[i], post_event_release,
                                     (void *)(uintptr_t)i);
            s_create_part = 1U;
        } else {
            s_event_details[i] = ui_text(s_event_rows[i], "", F_META14, C_GRAY);
            configure_detail_label(s_event_details[i], 2, 24,
                                   DIAG_CONTENT_W - 24, C_GRAY);
            s_create_part = 0U;
            if (++s_create_index >= DIAG_ROW_SLOTS)
                s_create_ready = true;
        }
        break;
    }

    ++s_create_units_done;
    return s_create_ready;
}

bool scr_events_is_ready(void)
{
    return s_create_ready;
}

unsigned scr_events_create_progress(void)
{
    if (s_create_ready) return 100U;
    return (s_create_units_done * 100U) / EVENTS_CREATE_STEP_COUNT;
}

lv_obj_t *scr_events_create(lv_obj_t *parent)
{
    lv_obj_t *page = scr_events_create_begin(parent);

    if (!page) return NULL;
    while (!scr_events_create_step()) {
    }
    scr_events_refresh();
    lv_obj_set_hidden(page, false);
    return page;
}

void scr_events_refresh(void)
{
    const usb_evidence_snapshot_t *s;
    const ui_view_state_t *v;
    uint8_t tab, i;
    uint32_t filter;
    int stage;
    int stage_filter;
    uint32_t session;
    uint16_t max_first;
    uint32_t content_height;
    int32_t current_y;
    int32_t max_scroll;
    bool reset_scroll;

    if (!s_create_ready || !s_page || !s_list) return;
    s = snapshot();
    v = view();
    tab = diagnostic_tab(v);
    filter = rule_filter(v);
    stage = v ? v->stage : DIAG_NONE;
    stage_filter = v && v->event_stage_filter ? 1 : 0;
    session = s ? s->session_id : 0U;

    if (s && !s_reset_pending) count_rules(s);
    s_visible_count = s_reset_pending ? 0U : current_list_count(s, v);
    reset_scroll = tab != s_last_tab || (int)filter != s_last_filter ||
                   stage != s_last_stage || stage_filter != s_last_stage_filter ||
                   session != s_session_id;
    max_first = s_visible_count > DIAG_VISIBLE_ROWS ?
                (uint16_t)(s_visible_count - DIAG_VISIBLE_ROWS) : 0U;
    current_y = reset_scroll ? 0 : lv_obj_get_scroll_y(s_list);
    if (current_y < 0) current_y = 0;

    content_height = s_visible_count > DIAG_VISIBLE_ROWS ?
        DIAG_LIST_H + (uint32_t)(s_visible_count - DIAG_VISIBLE_ROWS) *
                      DIAG_ROW_H : DIAG_LIST_H;
    max_scroll = (int32_t)content_height - DIAG_LIST_H;
    if (current_y > max_scroll) current_y = max_scroll;
    if ((uint16_t)(current_y / DIAG_ROW_H) > max_first)
        current_y = (int32_t)max_first * DIAG_ROW_H;

    s_rendering = true;
    lv_obj_set_height(s_spacer, (int32_t)content_height);
    s_scroll_y = current_y;
    lv_obj_scroll_to_y(s_list, s_scroll_y, LV_ANIM_OFF);
    s_scroll_y = lv_obj_get_scroll_y(s_list);
    s_first = (uint16_t)(s_scroll_y / DIAG_ROW_H);

    ui_button_set_selected(s_tabs[0], tab == 0U);
    ui_button_set_selected(s_tabs[1], tab == 1U);
    for (i = 0U; i < DIAG_FILTER_COUNT; ++i) {
        lv_obj_set_hidden(s_rule_filters[i], tab != 0U);
        ui_button_set_selected(s_rule_filters[i], tab == 0U && filter == i);
    }
    for (i = 0U; i < 2U; ++i) {
        lv_obj_set_hidden(s_event_filters[i], tab != 1U);
        ui_button_set_selected(s_event_filters[i],
                               tab == 1U && stage_filter == i);
    }

    if (!s_visible_count) {
        lv_obj_set_hidden(s_empty, false);
        lv_obj_set_pos(s_empty, 12, s_scroll_y + (DIAG_LIST_H - 32) / 2);
        label_set(s_empty, s_reset_pending || !s ? "等待证据" : "无可显示项");
    } else {
        lv_obj_set_hidden(s_empty, true);
    }

    for (i = 0U; i < DIAG_ROW_SLOTS; ++i) {
        if (tab == 0U) {
            lv_obj_set_hidden(s_rule_rows[i], false);
            lv_obj_set_hidden(s_event_rows[i], true);
            if (!s_reset_pending && s) render_rule_row(i, s, v);
            else lv_obj_set_hidden(s_rule_rows[i], true);
        } else {
            lv_obj_set_hidden(s_rule_rows[i], true);
            lv_obj_set_hidden(s_event_rows[i], false);
            if (!s_reset_pending && s) render_event_row(i, s, v);
            else lv_obj_set_hidden(s_event_rows[i], true);
        }
    }
    s_last_tab = tab;
    s_last_filter = (int)filter;
    s_last_stage = stage;
    s_last_stage_filter = stage_filter;
    s_session_id = session;
    s_rendering = false;
    render_summary();
}

void scr_events_update_summary(uint32_t total, uint32_t passed,
                               uint32_t failed, uint32_t not_observed,
                               uint32_t not_applicable)
{
    const usb_evidence_snapshot_t *s;

    if (!s_create_ready) return;
    s = snapshot();
    if (s && !s_reset_pending) {
        count_rules(s);
    } else {
        s_counts[0] = total;
        s_counts[1] = passed;
        s_counts[2] = failed;
        s_counts[3] = not_observed;
        s_counts[4] = not_applicable;
    }
    render_summary();
}

void scr_events_update_evidence(const usb_evidence_snapshot_t *snapshot_arg)
{
    LV_UNUSED(snapshot_arg);
    if (!s_create_ready) return;
    s_reset_pending = false;
    scr_events_refresh();
}

void scr_events_reset(void)
{
    uint8_t i;

    if (!s_create_ready) return;
    memset(s_counts, 0, sizeof(s_counts));
    s_first = 0U;
    s_scroll_y = 0;
    s_visible_count = 0U;
    s_reset_pending = true;
    if (s_list) lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    if (s_empty) {
        lv_obj_set_hidden(s_empty, false);
        label_set(s_empty, "等待证据");
    }
    for (i = 0U; i < DIAG_ROW_SLOTS; ++i) {
        if (s_rule_rows[i]) lv_obj_set_hidden(s_rule_rows[i], true);
        if (s_event_rows[i]) lv_obj_set_hidden(s_event_rows[i], true);
    }
    render_summary();
}

int scr_events_debug_semantic(char *buffer, size_t capacity)
{
    const usb_evidence_snapshot_t *s;
    const ui_view_state_t *v;
    uint16_t count;
    uint8_t tab;
    uint32_t filter;
    uint16_t slot;
    size_t used = 0U;

    if (!buffer || capacity == 0U) return -1;
    buffer[0] = '\0';
    if (!s_create_ready) return 0;
    s = snapshot();
    v = view();
    count = s_reset_pending ? 0U : current_list_count(s, v);
    tab = diagnostic_tab(v);
    filter = rule_filter(v);
    append_text(buffer, capacity, &used,
                "canvas=diagnostics tab=%s filter=%lu first=%u visible=%u "
                "slots=%u session=%lu scrollable=%u scroll_y=%ld;",
                tab == 0U ? "rules" : "events",
                (unsigned long)(tab == 0U ? filter :
                    (v && v->event_stage_filter ? 1U : 0U)),
                s_first, DIAG_VISIBLE_ROWS, DIAG_ROW_SLOTS,
                (unsigned long)(s ? s->session_id : 0U),
                s_list && lv_obj_is_scrollable(s_list) ? 1U : 0U,
                (long)(s_list ? lv_obj_get_scroll_y(s_list) : 0));

    for (slot = 0U; s && !s_reset_pending && slot < DIAG_VISIBLE_ROWS &&
         (uint16_t)(s_first + slot) < count && used < capacity - 1U; ++slot) {
        int index = current_list_index(s, v, (uint16_t)(s_first + slot));
        if (index < 0) break;
        if (tab == 0U) {
            const usb_evidence_rule_result_t *rule = &s->rules[index];
            const char *title = ui_model_rule_title((unsigned)index);
            unsigned ref_count = rule->evidence_ref_count;
            unsigned ref;
            if (ref_count > USB_EVIDENCE_REFERENCE_COUNT)
                ref_count = USB_EVIDENCE_REFERENCE_COUNT;
            append_text(buffer, capacity, &used,
                "row%u[%d]=%s id=%s verdict=%s observed=%s refs=",
                slot, index, title ? title : "规则",
                rule->rule_id ? rule->rule_id : "ID?",
                ui_model_verdict_name(rule->verdict),
                rule->observed[0] ? rule->observed : "未观测");
            for (ref = 0U; ref < ref_count && used < capacity - 1U; ++ref) {
                const usb_evidence_reference_t *r = &rule->evidence_refs[ref];
                append_text(buffer, capacity, &used,
                    "#%lu+%u/%u:%s%s",
                    (unsigned long)r->event_sequence, r->byte_offset,
                    r->byte_length, r->role,
                    ref + 1U < ref_count ? "," : "");
            }
            if (!ref_count) append_text(buffer, capacity, &used, "none");
            append_text(buffer, capacity, &used, ";");
        } else {
            const usb_evidence_raw_event_t *event = &s->raw_events[index];
            const char *helper = ui_model_event_name(event);
            append_text(buffer, capacity, &used,
                "row%u[%d]=event#%lu helper=%s direction=%s ret=%ld "
                "result=%s host_us=%lu host_time=%s capture=%s;",
                slot, index, (unsigned long)event->sequence,
                helper ? helper : "Host事件",
                direction_name(event->data_direction),
                (long)event->return_status,
                event->return_status >= 0 ? "success" : "failure",
                (unsigned long)event->timestamp_us,
                event->timing_valid ? "collected" : "not_collected",
                ui_model_capture_name(event->capture_status) ?
                    ui_model_capture_name(event->capture_status) : "unknown");
        }
    }

    if (used > (size_t)INT_MAX) return INT_MAX;
    return (int)used;
}
