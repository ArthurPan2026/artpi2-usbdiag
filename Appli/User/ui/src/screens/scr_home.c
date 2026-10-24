#include "scr_home.h"
#include "../ui_widgets.h"
#include "ui_deferred.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_page, *s_device, *s_identity, *s_conclusion, *s_counts;
static lv_obj_t *s_stage_buttons[8], *s_stage_labels[8], *s_stage_numbers[8], *s_stage_status[8];
static lv_obj_t *s_stage_placeholder;
static lv_obj_t *s_details_placeholder_panel, *s_details_placeholder;
static lv_obj_t *s_flow_panel, *s_explain_panel;
static unsigned s_stage_build_index;
static bool s_timeline_complete, s_details_complete;
static unsigned s_details_build_stage;
static lv_obj_t *s_title, *s_source, *s_request, *s_response, *s_time, *s_explanation;
static lv_obj_t *s_prev, *s_next;

/* A stage selection does not change identity, verdicts or eight observed
 * statuses. LVGL 9.6 label/style setters still refresh equal values, so
 * compare before touching them: avoid heap churn and whole-home dirty areas. */
static void home_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
}
static void home_text_color(lv_obj_t *obj, lv_color_t color)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), color))
        lv_obj_set_style_text_color(obj, color, 0);
}
static void home_bg_color(lv_obj_t *obj, lv_color_t color)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), color))
        lv_obj_set_style_bg_color(obj, color, 0);
}

static void home_append_count(char *text, size_t capacity, const char *name,
                              unsigned count)
{
    size_t used;
    if (!text || capacity == 0U || !name || count == 0U) return;
    used = strlen(text);
    if (used >= capacity) return;
    (void)snprintf(text + used, capacity - used, "%s%u %s",
                   used ? "  ·  " : "", count, name);
}

static void card_click(lv_event_t *event)
{
    if (ui_event_is_release_click(event))
        ui_model_post((ui_action_t)(uintptr_t)lv_event_get_user_data(event), 0);
}
static const char *speed_name(unsigned speed)
{
    return speed == 1 ? "低速" : speed == 2 ? "全速" : speed == 3 ? "高速" : "速度未知";
}
static void wrap(lv_obj_t *label, int x, int y, int width, int height)
{
    lv_obj_set_align(label, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
}
static void event_request(const usb_evidence_raw_event_t *e, char *text, size_t size)
{
    if (!e) { snprintf(text, size, "本阶段尚未观察到请求\n可查看其他已观察阶段"); return; }
    if (e->has_setup) {
        snprintf(text, size, "bRequest  %02X   type %02X\nwValue  %04X  index %04X\n请求长度  %u B",
            e->setup_packet.b_request, e->setup_packet.bm_request_type,
            e->setup_packet.w_value, e->setup_packet.w_index, e->setup_packet.w_length);
    } else {
        snprintf(text, size, "%s\n主机记录的会话事件", ui_model_event_name(e));
    }
}
static void event_response(const usb_evidence_raw_event_t *e, char *text, size_t size)
{
    if (!e) { snprintf(text, size, "没有对应响应证据\n不能据此判定通过"); return; }
    if (e->event_type != USB_EVIDENCE_EVENT_CONTROL_TRANSFER) {
        snprintf(text, size, "Host 报告：%s\n无控制传输字节", ui_model_event_name(e)); return;
    }
    if (e->return_status < 0) {
        snprintf(text, size, "返回错误  %ld\n%s\n已捕获  %u B",
            (long)e->return_status, ui_model_capture_name(e->capture_status), e->captured_length); return;
    }
    if (e->capture_status == USB_EVIDENCE_CAPTURE_NO_DATA_STAGE) {
        snprintf(text, size, "无数据阶段\nHost 返回成功\n不显示虚构的总线应答"); return;
    }
    if (e->transfer_length_known)
        snprintf(text, size, "实传  %u B   捕获  %u B\n%s\nHost 返回成功",
            e->transfer_length, e->captured_length, ui_model_capture_name(e->capture_status));
    else snprintf(text, size, "实际长度未采集\n捕获  %u B   %s",
        e->captured_length, ui_model_capture_name(e->capture_status));
}

static void home_create_stage_control(unsigned index, bool hidden)
{
    if (index >= 8U || !s_page) return;
    lv_obj_t *button = ui_button(s_page,
                                 ui_model_stage_short_name((int)index), 90,
                                 UI_ACTION_STAGE, (int32_t)index);
    lv_obj_set_pos(button, 12 + (int32_t)index * 98, 92);
    lv_obj_set_height(button, 80);
    s_stage_buttons[index] = button;
    s_stage_labels[index] = lv_obj_get_child(button, 0);
    wrap(s_stage_labels[index], 0, 28, 80, 20);
    lv_obj_set_style_text_align(s_stage_labels[index], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *dot = lv_obj_create(button);
    lv_obj_set_pos(dot, 28, 0);
    lv_obj_set_size(dot, 24, 24);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_scrollable(dot, false);
    lv_obj_set_clickable(dot, false);
    s_stage_numbers[index] = ui_text(dot, "", F_META, C_TEXT);
    lv_label_set_text_fmt(s_stage_numbers[index], "%u", index + 1U);
    lv_obj_center(s_stage_numbers[index]);
    s_stage_status[index] = ui_text(button, "未观察", F_META, C_GRAY);
    wrap(s_stage_status[index], 0, 50, 80, 18);
    lv_obj_set_style_text_align(s_stage_status[index], LV_TEXT_ALIGN_CENTER, 0);
    if (hidden) lv_obj_set_hidden(button, true);
}

static void home_create_flow_panel(bool hidden)
{
    lv_obj_t *flow = ui_panel(s_page, 12, 180, 480, 180);
    s_flow_panel = flow;
    if (hidden) lv_obj_set_hidden(flow, true);
    lv_obj_set_style_pad_all(flow, 10, 0);
    s_title = ui_text(flow, "请求与响应", F_TITLE, C_TEXT);
    wrap(s_title, 0, 0, 342, 25);
    s_source = ui_text(flow, "点选上方阶段，开始证据导览", F_META, C_GRAY);
    wrap(s_source, 0, 27, 342, 20);
    s_prev = ui_button(flow, "<", 48, UI_ACTION_STAGE_PREV, 0);
    s_next = ui_button(flow, ">", 48, UI_ACTION_STAGE_NEXT, 0);
    lv_obj_set_pos(s_prev, 352, 0);
    lv_obj_set_pos(s_next, 408, 0);
    lv_obj_t *request_heading = ui_text(flow, "Host 请求  >  Device", F_META, C_BLUE);
    lv_obj_set_pos(request_heading, 0, 56);
    lv_obj_t *response_heading = ui_text(flow, "Device 返回 / Host 结果", F_META, C_BLUE);
    lv_obj_set_pos(response_heading, 236, 56);
    s_request = ui_text(flow, "", F_BODY, C_TEXT);
    s_response = ui_text(flow, "", F_BODY, C_TEXT);
    wrap(s_request, 0, 78, 224, 60);
    wrap(s_response, 236, 78, 224, 60);
    s_time = ui_text(flow, "Host 软件观测时间：未采集", F_META, C_GRAY);
    wrap(s_time, 0, 142, 456, 18);
}

static void home_create_explain_panel(bool hidden)
{
    lv_obj_t *explain = ui_panel(s_page, 500, 180, 288, 180);
    s_explain_panel = explain;
    if (hidden) lv_obj_set_hidden(explain, true);
    lv_obj_set_style_pad_all(explain, 10, 0);
    ui_text(explain, "本步解读", F_TITLE, C_TEXT);
    s_explanation = ui_text(explain, "", F_BODY, C_GRAY);
    wrap(s_explanation, 0, 30, 264, 76);
    lv_obj_t *rules = ui_button(explain, "相关规则", 128, UI_ACTION_RELATED_RULES, 0);
    lv_obj_t *bytes = ui_button(explain, "查看字节", 128, UI_ACTION_BYTES, 0);
    lv_obj_set_pos(rules, 0, 112);
    lv_obj_set_pos(bytes, 136, 112);
}

lv_obj_t *scr_home_create(lv_obj_t *parent)
{
    memset(s_stage_buttons, 0, sizeof(s_stage_buttons));
    memset(s_stage_labels, 0, sizeof(s_stage_labels));
    memset(s_stage_numbers, 0, sizeof(s_stage_numbers));
    memset(s_stage_status, 0, sizeof(s_stage_status));
    s_stage_placeholder = NULL;
    s_details_placeholder_panel = NULL;
    s_details_placeholder = NULL;
    s_flow_panel = s_explain_panel = NULL;
    s_stage_build_index = 0U;
    s_timeline_complete = false;
    s_details_complete = false;
    s_details_build_stage = 0U;
    s_page = lv_obj_create(parent);
    lv_obj_add_style(s_page, &style_transp, 0);
    lv_obj_set_size(s_page, 800, UI_CONTENT_HEIGHT);
    lv_obj_set_scrollable(s_page, false);
    lv_obj_t *identity = ui_panel(s_page, 12, 12, 488, 72);
    lv_obj_set_style_pad_all(identity, 6, 0);
    lv_obj_set_style_pad_top(identity, 4, 0);
    lv_obj_set_style_pad_bottom(identity, 4, 0);
    lv_obj_set_clickable(identity, true);
    lv_obj_add_event_cb(identity, card_click, LV_EVENT_CLICKED, (void *)(uintptr_t)UI_ACTION_DEVICE);
    s_device = ui_text(identity, "连接设备，开始探索 USB 枚举", F_BODY, C_TEXT);
    wrap(s_device, 0, 0, 390, 40);
    s_identity = ui_text(identity, "设备身份 · VID:PID · 速度", F_META, C_GRAY);
    wrap(s_identity, 0, 42, 390, 18);
    lv_obj_t *more = ui_button(identity, "详情", 72, UI_ACTION_DEVICE, 0);
    lv_obj_set_pos(more, 400, 6);

    lv_obj_t *summary = ui_panel(s_page, 508, 12, 280, 72);
    lv_obj_set_style_pad_all(summary, 8, 0);
    lv_obj_set_clickable(summary, true);
    lv_obj_add_event_cb(summary, card_click, LV_EVENT_CLICKED, (void *)(uintptr_t)UI_ACTION_RELATED_RULES);
    s_conclusion = ui_text(summary, "等待证据", F_HERO, C_GRAY);
    wrap(s_conclusion, 0, 0, 260, 30);
    s_counts = ui_text(summary, "点选阶段与规则，查看判定依据", F_META, C_GRAY);
    wrap(s_counts, 0, 34, 260, 22);

#if defined(WORK_UI_HOST) && WORK_UI_HOST
    for (unsigned i = 0U; i < 8U; i++) {
        home_create_stage_control(i, false);
    }
    s_stage_build_index = 8U;
    s_timeline_complete = true;
    home_create_flow_panel(false);
    home_create_explain_panel(false);
    s_details_complete = true;
#else
    s_stage_placeholder = ui_text(s_page, "首页初始化中", F_BODY, C_GRAY);
    lv_obj_set_width(s_stage_placeholder, 776);
    lv_obj_set_style_text_align(s_stage_placeholder, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_stage_placeholder, 12, 116);

    s_details_placeholder_panel = ui_panel(s_page, 12, 180, 776, 180);
    s_details_placeholder = ui_text(s_details_placeholder_panel,
                                    "请求与响应", F_TITLE, C_GRAY);
    lv_obj_set_width(s_details_placeholder, 756);
    lv_obj_set_style_text_align(s_details_placeholder, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_details_placeholder);
#endif
    scr_home_refresh();
    return s_page;
}

bool scr_home_timeline_is_complete(void)
{
    return s_timeline_complete;
}

bool scr_home_deferred_is_complete(void)
{
    return s_timeline_complete && s_details_complete;
}

bool scr_home_create_timeline_step(void)
{
    if (!s_page) return false;
    if (s_timeline_complete) return true;

    if (s_stage_build_index < 8U) {
        home_create_stage_control(s_stage_build_index, true);
        s_stage_build_index++;
    }
    if (s_stage_build_index == 8U) {
        s_timeline_complete = true;
    }
    return s_timeline_complete;
}

bool scr_home_create_details_step(void)
{
    if (!s_page || !s_timeline_complete) return false;
    if (s_details_complete) return true;

    if (s_details_build_stage == 0U) {
        home_create_flow_panel(true);
        s_details_build_stage = 1U;
        return false;
    }

    home_create_explain_panel(true);
    if (s_flow_panel) lv_obj_set_hidden(s_flow_panel, false);
    if (s_explain_panel) lv_obj_set_hidden(s_explain_panel, false);
    if (s_details_placeholder_panel)
        lv_obj_set_hidden(s_details_placeholder_panel, true);
    s_details_complete = true;
    scr_home_refresh();
    return true;
}

void scr_home_show_timeline(void)
{
    if (!s_page || !s_timeline_complete) return;
    for (unsigned index = 0U; index < 8U; index++)
        lv_obj_set_hidden(s_stage_buttons[index], false);
    if (s_stage_placeholder) lv_obj_set_hidden(s_stage_placeholder, true);
    scr_home_refresh();
}

void scr_home_refresh(void)
{
    if (!s_page) return;
    const ui_view_state_t *v = ui_model_get();
    const usb_evidence_snapshot_t *s = v->snapshot;
    const usb_evidence_raw_event_t *e = ui_model_selected_event();
    char text[256], name[96];
    unsigned passed = 0, failed = 0, pending = 0, na = 0;
    bool refs_complete = true;
    if (s) {
        snprintf(name, sizeof(name), "%s%s%s", s->manufacturer,
            s->manufacturer[0] && s->product[0] ? " · " : "", s->product);
        ui_label_set_safe(s_device, name[0] ? name : "已观察到 USB 设备");
        snprintf(text, sizeof(text), "VID:PID %04X:%04X  ·  %s  ·  设备身份详情",
            s->vendor_id, s->product_id, speed_name(s->speed));
        home_text(s_identity, text);
        for (unsigned i = 0; i < 12; i++) {
            const usb_evidence_rule_result_t *r = &s->rules[i];
            switch (r->verdict) { case USB_RULE_PASS: passed++; break; case USB_RULE_FAIL: failed++; break;
                case USB_RULE_NOT_APPLICABLE: na++; break; default: pending++; break; }
            if (r->verdict == USB_RULE_PASS) {
                if (!r->evidence_ref_count || r->evidence_ref_count > 4) refs_complete = false;
                for (unsigned j = 0; j < r->evidence_ref_count && j < 4; j++)
                    if (!ui_model_range_valid(r->evidence_refs[j].event_sequence,
                        r->evidence_refs[j].byte_offset, r->evidence_refs[j].byte_length)) refs_complete = false;
            }
        }
    } else {
        home_text(s_device, "连接设备，开始探索 USB 枚举");
        home_text(s_identity, "设备身份 · VID:PID · 速度");
    }
    const char *conclusion = "等待证据";
    lv_color_t color = C_GRAY;
    if (s && s->session_id) {
        if (s->capture_integrity != USB_EVIDENCE_INTEGRITY_COMPLETE || !refs_complete) {
            conclusion = "证据不足"; color = C_ORANGE;
        } else if (failed) { conclusion = "规则失败"; color = C_RED; }
        else if (s->session_state == USB_EVIDENCE_SESSION_FAILED) { conclusion = "请求失败"; color = C_RED; }
        else if (!s->is_final || !s->enumeration_complete || pending || !passed) {
            conclusion = "尚待核验"; color = C_ORANGE;
        } else { conclusion = "全部通过"; color = C_GREEN; }
    }
    home_text(s_conclusion, conclusion);
    home_text_color(s_conclusion, color);
    if (!s) {
        snprintf(text, sizeof(text), "点选阶段与规则，查看判定依据");
    } else if (passed != 0U && failed == 0U && pending == 0U && na == 0U) {
        /* The conclusion already says every rule passed; retain only the total. */
        snprintf(text, sizeof(text), "%u 项规则", passed);
    } else {
        text[0] = '\0';
        home_append_count(text, sizeof(text), "通过", passed);
        home_append_count(text, sizeof(text), "失败", failed);
        home_append_count(text, sizeof(text), "待核验", pending);
        home_append_count(text, sizeof(text), "N/A", na);
        if (!text[0]) snprintf(text, sizeof(text), "等待证据");
    }
    home_text(s_counts, text);
    for (unsigned i = 0; i < 8; i++) {
        if (!s_stage_buttons[i] || !s_stage_numbers[i] || !s_stage_status[i])
            continue;
        usb_evidence_timeline_status_t status = USB_EVIDENCE_TIMELINE_WAITING;
        if (s) for (unsigned j = 0; j < s->timeline_count; j++)
            if ((unsigned)s->timeline[j].stage == i) status = s->timeline[j].status;
        lv_color_t c = status == USB_EVIDENCE_TIMELINE_OBSERVED ? C_GREEN :
            status == USB_EVIDENCE_TIMELINE_FAILED ? C_RED : C_GRAY;
        ui_button_set_selected(s_stage_buttons[i], (int)i == v->stage);
        home_bg_color(lv_obj_get_parent(s_stage_numbers[i]),
            status == USB_EVIDENCE_TIMELINE_WAITING ? C_RAISED : c);
        home_text_color(s_stage_numbers[i],
            status == USB_EVIDENCE_TIMELINE_WAITING ? C_GRAY : C_BG);
        home_text(s_stage_status[i], status == USB_EVIDENCE_TIMELINE_OBSERVED ?
            "已观察" : status == USB_EVIDENCE_TIMELINE_FAILED ? "失败" : "未观察");
        home_text_color(s_stage_status[i], c);
    }
    home_text(s_title, ui_model_stage_name(v->stage));
    if (e) snprintf(text, sizeof(text), "事件 #%lu  ·  同阶段 %u/%u%s",
        (unsigned long)e->sequence, ui_model_stage_event_ordinal(),
        ui_model_stage_event_count(v->stage),
        e->return_status < 0 ? "  ·  返回错误" : "");
    else snprintf(text, sizeof(text), "本阶段没有对应事件，证据仍待观察");
    home_text(s_source, text);
    event_request(e, text, sizeof(text)); home_text(s_request, text);
    event_response(e, text, sizeof(text)); home_text(s_response, text);
    if (e && e->timing_valid) {
        snprintf(text, sizeof(text), "Host 软件观测时间：%lu us  ·  耗时 %lu us",
            (unsigned long)e->started_at_us, (unsigned long)e->duration_us);
        home_text(s_time, text);
        lv_obj_set_hidden(s_time, false);
    } else {
        home_text(s_time, "");
        lv_obj_set_hidden(s_time, true);
    }
    home_text(s_explanation, ui_model_stage_explanation(v->stage));
    if (s_prev && s_next) {
        if (ui_model_stage_event_count(v->stage) < 2) {
            lv_obj_set_disabled(s_prev, true); lv_obj_set_disabled(s_next, true);
        } else {
            lv_obj_set_disabled(s_prev, false); lv_obj_set_disabled(s_next, false);
        }
    }
}
void scr_home_reset(void) { scr_home_refresh(); }
#if (defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS) || defined(WORK_UI_HOST)
rt_uint8_t scr_home_debug_timeline_visible_count(void)
{
    if (!s_page) return 0;
    lv_obj_update_layout(s_page);
    unsigned visible = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (!s_stage_buttons[i] || !s_stage_labels[i]) continue;
        lv_area_t a, b;
        lv_obj_get_coords(s_stage_buttons[i], &a);
        lv_obj_get_coords(s_stage_labels[i], &b);
        if (a.x1 >= 0 && a.x2 < 800 && a.y1 >= 52 && a.y2 < 424 &&
            b.x1 >= a.x1 && b.x2 <= a.x2 && b.y1 >= a.y1 && b.y2 <= a.y2) visible++;
    }
    return (rt_uint8_t)visible;
}
#endif
