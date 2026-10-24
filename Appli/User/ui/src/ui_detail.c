#include "ui_detail.h"
#include "ui_deferred.h"
#include "build_identity.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_overlay, *s_panel, *s_title, *s_body, *s_primary;
static lv_obj_t *s_refs[4], *s_bytes, *s_reveal, *s_hex, *s_zoom;
static ui_detail_t s_last_detail;
static uint32_t s_last_revision;
static bool s_body_scrollable = true;

static void set_hidden_if_changed(lv_obj_t *object, bool hidden)
{
    if (object && lv_obj_is_hidden(object) != hidden)
        lv_obj_set_hidden(object, hidden);
}

static const char *field_meaning(const char *name)
{
    if (!name) return "按字段来源核对原始字节。";
    if (!strcmp(name, "bLength")) return "当前描述符自身的字节长度，不是整个配置的长度。";
    if (!strcmp(name, "bDescriptorType")) return "描述符类型，用于区分设备、配置、接口和端点。";
    if (!strcmp(name, "wTotalLength")) return "配置及其下属接口、端点等描述符的总字节长度。";
    if (!strcmp(name, "bNumInterfaces")) return "配置内唯一接口的数量，备用设置不重复计数。";
    if (!strcmp(name, "bMaxPacketSize0")) return "默认控制端点 EP0 的最大包大小，合法值与速度相关。";
    if (!strcmp(name, "bEndpointAddress")) return "端点编号及数据方向；最高位表示 IN 或 OUT。";
    if (!strcmp(name, "bmAttributes")) return "属性位的含义由当前描述符类型决定。";
    if (!strcmp(name, "wMaxPacketSize")) return "端点最大包大小及相关事务位，由规则核验合法性。";
    if (!strcmp(name, "bInterval")) return "轮询间隔的解释与设备速度及端点传输类型相关。";
    if (!strcmp(name, "idVendor") || !strcmp(name, "idProduct")) return "设备身份标识；显示值由当前捕获的设备描述符解析。";
    if (!strcmp(name, "iManufacturer") || !strcmp(name, "iProduct") || !strcmp(name, "iSerialNumber"))
        return "字符串描述符索引。索引为零时表示设备未提供该字符串。";
    return "字段值来自当前描述符。结合偏移、长度及规则结果核对证据。";
}
static void close_outside(lv_event_t *event)
{
    if (lv_event_get_target(event) == s_overlay && ui_event_is_release_click(event))
        ui_model_post(UI_ACTION_CLOSE_DETAIL, 0);
}
static void button_text(lv_obj_t *button, const char *text)
{
    lv_obj_t *label = lv_obj_get_child(button, 0);
    char safe[1024];
    if (!label || !text) return;
    ui_font_sanitize_dynamic_text(text, safe, sizeof(safe));
    bool changed = strcmp(lv_label_get_text(label), safe) != 0;
    if (changed) ui_label_set_safe(label, text);
    int32_t width = lv_obj_get_width(button) - 12;
    if (changed || lv_obj_get_width(label) != width ||
        lv_label_get_long_mode(label) != LV_LABEL_LONG_MODE_DOTS) {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_center(label);
    }
}
lv_obj_t *ui_detail_object(void) { return s_overlay; }
void ui_detail_create(lv_obj_t *screen)
{
    if (!screen || s_overlay) return;
    s_overlay = lv_obj_create(screen);
    lv_obj_set_pos(s_overlay, 0, UI_TOP_BAR_HEIGHT);
    lv_obj_set_size(s_overlay, 800, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, C_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, 140, 0);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_add_event_cb(s_overlay, close_outside, LV_EVENT_CLICKED, NULL);
    s_panel = ui_panel(s_overlay, 280, 0, 520, UI_CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(s_panel, C_RAISED, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    s_title = ui_text(s_panel, "证据详情", F_TITLE, C_TEXT);
    lv_obj_set_pos(s_title, 0, 8);
    lv_obj_set_width(s_title, 312);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    s_zoom = ui_button(s_panel, "放大", 80, UI_ACTION_BYTES, 1);
    lv_obj_set_pos(s_zoom, 324, 0);
    lv_obj_t *close = ui_button(s_panel, "关闭", 80, UI_ACTION_CLOSE_DETAIL, 0);
    lv_obj_set_pos(close, 412, 0);
    s_body = lv_obj_create(s_panel);
    lv_obj_add_style(s_body, &style_transp, 0);
    lv_obj_set_pos(s_body, 0, 56);
    lv_obj_set_size(s_body, 496, 288);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    lv_obj_set_scrollable(s_body, true);
    s_body_scrollable = true;
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_AUTO);
    s_primary = ui_text(s_body, "", F_BODY, C_TEXT);
    lv_obj_set_width(s_primary, 492);
    lv_label_set_long_mode(s_primary, LV_LABEL_LONG_MODE_WRAP);
    for (int i = 0; i < 4; i++) s_refs[i] = ui_button(s_body, "", 488, UI_ACTION_RULE_REFERENCE, i);
    s_bytes = ui_button(s_body, "定位原始字节", 200, UI_ACTION_BYTES, 0);
    s_reveal = ui_button(s_body, "查看序列号", 200, UI_ACTION_REVEAL_SERIAL, 0);
    s_hex = lv_obj_create(s_body);
    lv_obj_add_style(s_hex, &style_transp, 0);
    lv_obj_set_size(s_hex, 492, 288);
    lv_obj_set_scrollable(s_hex, false);
    s_last_detail = UI_DETAIL_NONE;
    s_last_revision = 0;
    lv_obj_set_hidden(s_overlay, true);
}
void ui_detail_refresh(void)
{
    if (!s_overlay || !s_panel || !s_body) return;
    const ui_view_state_t *v = ui_model_get();
    const usb_evidence_snapshot_t *s = v->snapshot;
    const usb_evidence_raw_event_t *e = ui_model_selected_event();
    bool opened = s_last_detail == UI_DETAIL_NONE && v->detail != UI_DETAIL_NONE;
    if (v->detail == UI_DETAIL_NONE) {
        if (s_last_detail != UI_DETAIL_NONE) {
            lv_anim_delete(s_panel, NULL);
            lv_obj_set_style_translate_x(s_panel, 0, 0);
            set_hidden_if_changed(s_overlay, true);
        }
        s_last_detail = UI_DETAIL_NONE;
        return;
    }
    if (s_last_revision == v->selection_revision && s_last_detail == v->detail) return;
    s_last_revision = v->selection_revision;
    if (s_last_detail != v->detail) lv_obj_scroll_to_y(s_body, 0, LV_ANIM_OFF);
    set_hidden_if_changed(s_overlay, false);
    for (int i = 0; i < 4; i++) set_hidden_if_changed(s_refs[i], true);
    set_hidden_if_changed(s_bytes, true);
    set_hidden_if_changed(s_reveal, true);
    set_hidden_if_changed(s_hex, v->detail != UI_DETAIL_BYTES);
    set_hidden_if_changed(s_primary, v->detail == UI_DETAIL_BYTES);
    set_hidden_if_changed(s_zoom, v->detail != UI_DETAIL_BYTES);
    bool body_scrollable = v->detail != UI_DETAIL_BYTES;
    if (s_body_scrollable != body_scrollable) {
        lv_obj_set_scrollable(s_body, body_scrollable);
        s_body_scrollable = body_scrollable;
    }
    char body[1536], title[96];
    body[0] = '\0';
    snprintf(title, sizeof(title), "证据详情");
    unsigned ref_count = 0;
    if (v->detail == UI_DETAIL_RULE && s && v->rule_index >= 0) {
        const usb_evidence_rule_result_t *r = &s->rules[v->rule_index];
        snprintf(title, sizeof(title), "%s · %s", ui_model_rule_title(v->rule_index), ui_model_verdict_name(r->verdict));
        snprintf(body, sizeof(body), "规则  %s\n\n期望值\n%s\n\n实测值\n%s\n\n判定依据\n%s\n\nUSB 规范条款\n%s\n\n证据引用%s\n其余失败位置未展开：%u",
            r->rule_id ? r->rule_id : "未知", r->expected, r->observed, r->reason_code,
            r->spec_clause ? r->spec_clause : "未提供", r->evidence_ref_count ? "" : "：不可用",
            r->omitted_failure_reference_count);
        ref_count = r->evidence_ref_count > 4 ? 4 : r->evidence_ref_count;
        for (unsigned i = 0; i < ref_count; i++) {
            const usb_evidence_reference_t *ref = &r->evidence_refs[i];
            char caption[160];
            snprintf(caption, sizeof(caption), "事件 #%lu  +%u:%u  %s  %s",
                (unsigned long)ref->event_sequence, ref->byte_offset, ref->byte_length,
                ref->role, ui_model_range_valid(ref->event_sequence, ref->byte_offset, ref->byte_length) ?
                "定位字节" : "引用不可用");
            button_text(s_refs[i], caption);
        }
    } else if (v->detail == UI_DETAIL_FIELD && s && v->field_index >= 0 && v->node_index >= 0) {
        const usb_evidence_descriptor_field_t *f = &s->descriptor_fields[v->field_index];
        const usb_evidence_descriptor_node_t *n = &s->descriptor_nodes[v->node_index];
        char value[128];
        ui_font_sanitize_dynamic_text(f->value, value, sizeof(value));
        snprintf(title, sizeof(title), "%s", f->name);
        snprintf(body, sizeof(body), "字段含义\n%s\n\n完整字段值\n%s\n\n字段内偏移  +%u\n字段长度  %u B\n\n来源事件  #%lu\n事件内字节  +%u:%u\n引用状态  %s",
            field_meaning(f->name), value, f->byte_offset, f->byte_length,
            (unsigned long)n->source_sequence, v->byte_offset, v->byte_length,
            ui_model_range_valid(n->source_sequence, v->byte_offset, v->byte_length) ?
            "有效，可定位" : "字节未完整捕获");
        set_hidden_if_changed(s_bytes, false);
    } else if (v->detail == UI_DETAIL_EVENT) {
        snprintf(title, sizeof(title), "%s", ui_model_event_name(e));
        if (!e) snprintf(body, sizeof(body), "来源事件不可用。当前快照不能提供这条证据。");
        else {
            snprintf(body, sizeof(body), "事件  #%lu   尝试  %lu\n端口  %s\n\n方向  %s\n返回状态  %ld\n实传长度  %s\n捕获长度  %u B · %s\n\nSETUP\nbmRequestType %02X   bRequest %02X\nwValue %04X  wIndex %04X\nwLength %u\n\nHost 软件观测时间\n%s\n",
                (unsigned long)e->sequence, (unsigned long)e->attempt_id, e->port_valid ? "已采集" : "未采集",
                e->data_direction == USB_EVIDENCE_DATA_DEVICE_TO_HOST ? "Device > Host" :
                e->data_direction == USB_EVIDENCE_DATA_HOST_TO_DEVICE ? "Host > Device" : "无数据阶段",
                (long)e->return_status, e->transfer_length_known ? "已采集" : "未知",
                e->captured_length, ui_model_capture_name(e->capture_status),
                e->setup_packet.bm_request_type, e->setup_packet.b_request, e->setup_packet.w_value,
                e->setup_packet.w_index, e->setup_packet.w_length, e->timing_valid ? "已采集，见下方数值" : "未采集");
            size_t used = strlen(body);
            if (e->transfer_length_known) snprintf(body + used, sizeof(body) - used, "实际传输  %u B\n", e->transfer_length);
            used = strlen(body);
            if (e->timing_valid) snprintf(body + used, sizeof(body) - used, "开始 %lu us\n完成 %lu us\n耗时 %lu us\n",
                (unsigned long)e->started_at_us, (unsigned long)e->completed_at_us, (unsigned long)e->duration_us);
            used = strlen(body);
            if (e->port_valid) snprintf(body + used, sizeof(body) - used, "真实端口  %u\n", e->port);
            set_hidden_if_changed(s_bytes, false);
        }
    } else if (v->detail == UI_DETAIL_DEVICE && s) {
        char manufacturer[64], product[64], serial[256], class_name[64];
        ui_font_sanitize_dynamic_text(s->manufacturer, manufacturer, sizeof(manufacturer));
        ui_font_sanitize_dynamic_text(s->product, product, sizeof(product));
        ui_font_sanitize_dynamic_text(s->serial, serial, sizeof(serial));
        ui_font_sanitize_dynamic_text(s->class_name, class_name, sizeof(class_name));
        snprintf(title, sizeof(title), "设备身份");
        snprintf(body, sizeof(body), "厂商\n%s\n\n产品\n%s\n\nVID:PID  %04X:%04X\nUSB 版本  %04X\n设备版本  %04X\n\n设备类\n%s\n\n序列号\n%s",
            manufacturer, product, s->vendor_id, s->product_id, s->bcd_usb, s->bcd_device,
            class_name, v->reveal_serial ? serial : "已遮盖，点选下方按钮查看");
        button_text(s_reveal, v->reveal_serial ? "遮盖序列号" : "查看序列号");
        set_hidden_if_changed(s_reveal, false);
    } else if (v->detail == UI_DETAIL_SESSION) {
        snprintf(title, sizeof(title), "会话与构建身份");
        snprintf(body, sizeof(body), "当前查看会话  #%lu · r%lu\n完整性  %s\n证据 CRC32  %08lX\n原始事件  %u\n会话丢弃  %lu\n\n解析器  v%u\n规则集  v%u\n\nRT-Thread 5.3.0 · LVGL 9.6.0\n源码指纹\n%s\n\n冻结查看只暂停界面跟随，后台采集继续。",
            s ? (unsigned long)s->session_id : 0, s ? (unsigned long)s->revision : 0,
            s && s->capture_integrity == USB_EVIDENCE_INTEGRITY_COMPLETE ? "完整" : "未完整",
            s ? (unsigned long)s->content_crc32 : 0, s ? s->raw_event_count : 0,
            s ? (unsigned long)s->session_dropped_event_count : 0,
            s ? s->parser_version : 0, s ? s->ruleset_version : 0, WORK_SOURCE_FINGERPRINT);
    } else if (v->detail == UI_DETAIL_BYTES) {
        snprintf(title, sizeof(title), "原始字节 · 来源与偏移");
        button_text(s_zoom, v->hex_enlarged ? "恢复" : "放大");
        ui_hex_set(s_hex, e, v->byte_offset, v->byte_length, v->hex_enlarged);
    } else snprintf(body, sizeof(body), "证据尚未观察到，等待设备与完整会话。");
    ui_label_set_safe(s_title, title);
    if (v->detail != UI_DETAIL_BYTES) {
        ui_label_set_formatted(s_primary, body);
        lv_obj_update_layout(s_primary);
        int y = lv_obj_get_height(s_primary) + 12;
        for (unsigned i = 0; i < ref_count; i++) {
            lv_obj_set_pos(s_refs[i], 0, y); y += 56;
            set_hidden_if_changed(s_refs[i], false);
        }
        if (!lv_obj_is_hidden(s_bytes)) { lv_obj_set_pos(s_bytes, 0, y); y += 56; }
        if (!lv_obj_is_hidden(s_reveal)) lv_obj_set_pos(s_reveal, 0, y);
    }
    if (opened) ui_animate_enter(s_panel, UI_DETAIL_MS, 24);
    s_last_detail = v->detail;
}
