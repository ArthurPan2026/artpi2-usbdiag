#include "ui_model.h"
#include <string.h>

static ui_view_state_t s_view;
static ui_action_post_fn s_post;
static lv_subject_t *s_page, *s_selection, *s_detail;
static const char *const s_stages[8] = {
    "端口复位", "设备前 8 字节", "设置设备地址", "完整设备描述符",
    "读取配置描述符", "读取字符串描述符", "设置设备配置", "枚举完成"
};
static const char *const s_short_stages[8] = {
    "端口复位", "设备前8B", "设置地址", "完整设备",
    "配置描述符", "字符串", "设置配置", "枚举完成"
};
static const char *const s_explanations[8] = {
    "主机复位端口，开始建立本次枚举会话。",
    "先读取设备头部，确定 EP0 最大包大小。",
    "主机分配地址，后续请求使用该地址。",
    "读取设备身份、USB 版本及字符串索引。",
    "先读配置头，再读取接口与端点的完整结构。",
    "读取厂商、产品和序列号等设备字符串。",
    "选择配置，让设备进入可使用的配置状态。",
    "主机完成枚举；结合完整性与十二规则查看结论。"
};
static const char *const s_rule_titles[12] = {
    "设备描述符长度", "设备描述符类型", "前 8 字节一致性", "EP0 最大包大小",
    "配置头长度与类型", "配置总长度", "配置结构可遍历性", "接口数量",
    "接口的端点数量", "端点包大小", "端点轮询间隔", "必要请求 STALL"
};
static const char *const s_rule_ids[12] = {
    "DEV_B_LENGTH", "DEV_TYPE", "DEV_8B_PREFIX", "DEV_FIELDS",
    "CFG_B_LENGTH", "CFG_TOTAL_LENGTH", "CFG_TREE_WALK", "CFG_INTERFACE_COUNT",
    "IF_ENDPOINT_COUNT", "EP_PACKET_SIZE", "EP_INTERVAL", "EP0_STALL"
};

const char *ui_model_stage_name(int s) { return s >= 0 && s < 8 ? s_stages[s] : "尚未观察"; }
const char *ui_model_stage_short_name(int s) { return s >= 0 && s < 8 ? s_short_stages[s] : "未知"; }
const char *ui_model_stage_explanation(int s) { return s >= 0 && s < 8 ? s_explanations[s] : "连接 USB 设备后，可点选阶段探索枚举证据。"; }
const char *ui_model_rule_title(unsigned i)
{
    if (i >= 12) return "未知规则";
    if (s_view.snapshot) {
        const char *id = s_view.snapshot->rules[i].rule_id;
        if (s_view.snapshot->ruleset_version != USB_EVIDENCE_RULESET_VERSION ||
            !id || strcmp(id, s_rule_ids[i])) return "未知规则";
    }
    return s_rule_titles[i];
}
const char *ui_model_verdict_name(usb_rule_verdict_t v)
{
    switch (v) {
    case USB_RULE_PASS: return "通过";
    case USB_RULE_FAIL: return "失败";
    case USB_RULE_NOT_APPLICABLE: return "不适用";
    default: return "未观察";
    }
}
const char *ui_model_capture_name(usb_evidence_capture_status_t v)
{
    switch (v) {
    case USB_EVIDENCE_CAPTURE_COMPLETE: return "捕获完整";
    case USB_EVIDENCE_CAPTURE_LOCAL_TRUNCATED: return "捕获截断";
    case USB_EVIDENCE_CAPTURE_NO_DATA_STAGE: return "无数据阶段";
    case USB_EVIDENCE_CAPTURE_UNAVAILABLE: return "字节不可用";
    default: return "未捕获";
    }
}
int ui_model_event_stage(const usb_evidence_raw_event_t *e)
{
    if (!e) return -1;
    if (e->event_type == USB_EVIDENCE_EVENT_PORT_RESET) return 0;
    if (e->event_type == USB_EVIDENCE_EVENT_ENUMERATION_COMPLETE) return 7;
    if (!e->has_setup || e->event_type != USB_EVIDENCE_EVENT_CONTROL_TRANSFER) return -1;
    if (e->setup_packet.b_request == 5) return 2;
    if (e->setup_packet.b_request == 9) return 6;
    if (e->setup_packet.b_request == 6) {
        switch (e->setup_packet.w_value >> 8) {
        case 1: return e->setup_packet.w_length <= 8 ? 1 : 3;
        case 2: return 4;
        case 3: return 5;
        default: break;
        }
    }
    return -1;
}
const char *ui_model_event_name(const usb_evidence_raw_event_t *e)
{
    if (!e) return "等待 USB 证据";
    if (e->event_type == USB_EVIDENCE_EVENT_DISCONNECT) return "设备断开";
    if (e->event_type == USB_EVIDENCE_EVENT_PORT_RESET) return "端口复位";
    if (e->event_type == USB_EVIDENCE_EVENT_ENUMERATION_COMPLETE) return "枚举完成";
    if (!e->has_setup) return "控制传输";
    if (e->setup_packet.b_request == 6) {
        switch (e->setup_packet.w_value >> 8) {
        case 1: return "GET_DESCRIPTOR / Device";
        case 2: return "GET_DESCRIPTOR / Configuration";
        case 3: return "GET_DESCRIPTOR / String";
        default: return "GET_DESCRIPTOR";
        }
    }
    if (e->setup_packet.b_request == 5) return "SET_ADDRESS";
    if (e->setup_packet.b_request == 9) return "SET_CONFIGURATION";
    return "控制请求";
}

void ui_model_reset(void)
{
    if (s_page) lv_subject_delete(s_page);
    if (s_selection) lv_subject_delete(s_selection);
    if (s_detail) lv_subject_delete(s_detail);
    s_page = s_selection = s_detail = NULL;
    s_post = NULL;
    memset(&s_view, 0, sizeof(s_view));
}
void ui_model_init(ui_action_post_fn post)
{
    ui_model_reset();
    s_post = post;
    s_view.stage = s_view.event_index = s_view.node_index = -1;
    s_view.field_index = s_view.rule_index = -1;
    s_page = lv_subject_create(LV_SUBJECT_TYPE_INT);
    s_selection = lv_subject_create(LV_SUBJECT_TYPE_INT);
    s_detail = lv_subject_create(LV_SUBJECT_TYPE_INT);
    if (s_page) lv_subject_set_int(s_page, UI_PAGE_LIVE);
    if (s_selection) lv_subject_set_int(s_selection, 0);
    if (s_detail) lv_subject_set_int(s_detail, UI_DETAIL_NONE);
}
const ui_view_state_t *ui_model_get(void) { return &s_view; }
const usb_evidence_snapshot_t *ui_model_snapshot(void) { return s_view.snapshot; }
lv_subject_t *ui_model_page_subject(void) { return s_page; }
lv_subject_t *ui_model_selection_subject(void) { return s_selection; }
lv_subject_t *ui_model_detail_subject(void) { return s_detail; }
void ui_model_post(ui_action_t a, int32_t arg) { if (s_post) s_post(a, arg); }

int ui_model_event_index(uint32_t seq)
{
    const usb_evidence_snapshot_t *s = s_view.snapshot;
    if (s) for (unsigned i = 0; i < s->raw_event_count; i++)
        if (s->raw_events[i].sequence == seq) return (int)i;
    return -1;
}
const usb_evidence_raw_event_t *ui_model_event_by_sequence(uint32_t seq)
{
    int i = ui_model_event_index(seq);
    return i >= 0 ? &s_view.snapshot->raw_events[i] : NULL;
}
const usb_evidence_raw_event_t *ui_model_selected_event(void)
{
    const usb_evidence_snapshot_t *s = s_view.snapshot;
    return s && s_view.event_index >= 0 && s_view.event_index < s->raw_event_count ?
        &s->raw_events[s_view.event_index] : NULL;
}
bool ui_model_range_valid(uint32_t seq, uint16_t off, uint16_t len)
{
    const usb_evidence_raw_event_t *e = ui_model_event_by_sequence(seq);
    return e && e->captured_length <= USB_EVIDENCE_CAPTURE_MAX &&
        off <= e->captured_length && len <= e->captured_length - off;
}
unsigned ui_model_stage_event_count(int stage)
{
    const usb_evidence_snapshot_t *s = s_view.snapshot;
    unsigned n = 0;
    if (s) for (unsigned i = 0; i < s->raw_event_count; i++)
        if (ui_model_event_stage(&s->raw_events[i]) == stage) n++;
    return n;
}
unsigned ui_model_stage_event_ordinal(void)
{
    const usb_evidence_snapshot_t *s = s_view.snapshot;
    unsigned n = 0;
    if (s) for (int i = 0; i <= s_view.event_index && i < s->raw_event_count; i++)
        if (ui_model_event_stage(&s->raw_events[i]) == s_view.stage) n++;
    return n;
}
bool ui_model_node_visible(unsigned index)
{
    const usb_evidence_snapshot_t *s = s_view.snapshot;
    if (!s || index >= s->descriptor_node_count) return false;
    uint16_t parent = s->descriptor_nodes[index].parent_id;
    for (unsigned guard = 0; guard < 64 && parent != USB_EVIDENCE_NODE_NONE; guard++) {
        int p = -1;
        for (unsigned i = 0; i < s->descriptor_node_count; i++)
            if (s->descriptor_nodes[i].node_id == parent) { p = (int)i; break; }
        if (p < 0 || (unsigned)p == index) break;
        if (s_view.collapsed_nodes & ((uint64_t)1 << p)) return false;
        parent = s->descriptor_nodes[p].parent_id;
    }
    return true;
}
static void notify(void)
{
    s_view.selection_revision++;
    if (s_page) lv_subject_set_int(s_page, s_view.page);
    if (s_detail) lv_subject_set_int(s_detail, s_view.detail);
    if (s_selection) lv_subject_set_int(s_selection, (int32_t)s_view.selection_revision);
}
static void select_stage(int stage, bool last)
{
    s_view.stage = (int16_t)stage;
    s_view.event_index = -1;
    if (s_view.snapshot) for (unsigned i = 0; i < s_view.snapshot->raw_event_count; i++)
        if (ui_model_event_stage(&s_view.snapshot->raw_events[i]) == stage) {
            s_view.event_index = (int16_t)i;
            if (!last) break;
        }
    s_view.byte_offset = s_view.byte_length = 0;
}
static void select_node(int index)
{
    const usb_evidence_descriptor_node_t *n = &s_view.snapshot->descriptor_nodes[index];
    s_view.node_index = (int16_t)index;
    s_view.field_index = -1;
    s_view.event_index = (int16_t)ui_model_event_index(n->source_sequence);
    s_view.byte_offset = n->source_offset;
    s_view.byte_length = n->byte_length;
}
static void select_reference(const usb_evidence_reference_t *ref)
{
    s_view.event_index = (int16_t)ui_model_event_index(ref->event_sequence);
    s_view.byte_offset = ref->byte_offset;
    s_view.byte_length = ref->byte_length;
}
void ui_model_set_snapshot(const usb_evidence_snapshot_t *s)
{
    /* Counts are checked before any dereference, including host fixtures. */
    if (s && (s->raw_event_count > 64 || s->descriptor_node_count > 64 ||
              s->descriptor_field_count > 128 || s->timeline_count > 8)) s = NULL;
    s_view.snapshot = s;
    s_view.snapshot_epoch++;
    s_view.detail = UI_DETAIL_NONE;
    s_view.reveal_serial = false;
    s_view.collapsed_nodes = 0;
    s_view.field_index = s_view.rule_index = s_view.node_index = -1;
    s_view.updates_available = false;
    int stage = -1;
    if (s) {
        for (unsigned i = 0; i < s->timeline_count; i++)
            if (s->timeline[i].status == USB_EVIDENCE_TIMELINE_FAILED) { stage = s->timeline[i].stage; break; }
        if (stage < 0) for (unsigned i = 0; i < s->timeline_count; i++)
            if (s->timeline[i].status == USB_EVIDENCE_TIMELINE_OBSERVED) stage = s->timeline[i].stage;
        for (unsigned i = 0; i < s->descriptor_node_count; i++) {
            if (s_view.node_index < 0) s_view.node_index = (int16_t)i;
            if (s->descriptor_nodes[i].descriptor_type == 1) { s_view.node_index = (int16_t)i; break; }
        }
        for (unsigned i = 0; i < 12; i++) if (s->rules[i].verdict == USB_RULE_FAIL) {
            s_view.rule_index = (int16_t)i;
            if (s->rules[i].evidence_ref_count) {
                int failed_stage = ui_model_event_stage(ui_model_event_by_sequence(s->rules[i].evidence_refs[0].event_sequence));
                if (failed_stage >= 0) stage = failed_stage;
            }
            break;
        }
    }
    select_stage(stage, true);
    notify();
}
void ui_model_set_update_available(bool value)
{
    if (s_view.updates_available != value) { s_view.updates_available = value; notify(); }
}
bool ui_model_apply_action(ui_action_t a, int32_t arg)
{
    const ui_view_state_t before=s_view;
    const usb_evidence_snapshot_t *s = s_view.snapshot;
    switch (a) {
    case UI_ACTION_PAGE:
        if (arg < 0 || arg >= UI_PAGE_COUNT) return false;
        s_view.page = (ui_page_t)arg; s_view.detail = UI_DETAIL_NONE; break;
    case UI_ACTION_FOLLOW:
        s_view.frozen = false; s_view.detail = UI_DETAIL_NONE; s_view.reveal_serial = false; break;
    case UI_ACTION_CLOSE_DETAIL: s_view.detail = UI_DETAIL_NONE; break;
    case UI_ACTION_REDUCED_MOTION: s_view.reduced_motion = !s_view.reduced_motion; break;
    case UI_ACTION_SESSION: s_view.detail = UI_DETAIL_SESSION; break;
    case UI_ACTION_DEVICE: s_view.detail = UI_DETAIL_DEVICE; s_view.frozen = s != NULL; break;
    case UI_ACTION_REVEAL_SERIAL: s_view.reveal_serial = !s_view.reveal_serial; break;
    case UI_ACTION_STAGE:
        if (!s || arg < 0 || arg >= 8) return false;
        select_stage((int)arg, false); s_view.frozen = true; s_view.detail = UI_DETAIL_NONE; break;
    case UI_ACTION_STAGE_PREV:
    case UI_ACTION_STAGE_NEXT: {
        if (!s) return false;
        int step = a == UI_ACTION_STAGE_NEXT ? 1 : -1;
        for (int i = s_view.event_index + step; i >= 0 && i < s->raw_event_count; i += step)
            if (ui_model_event_stage(&s->raw_events[i]) == s_view.stage) { s_view.event_index = (int16_t)i; break; }
        s_view.frozen = true; break;
    }
    case UI_ACTION_NODE:
    case UI_ACTION_NODE_TOGGLE:
        if (!s || arg < 0 || arg >= s->descriptor_node_count) return false;
        select_node((int)arg);
        if (a == UI_ACTION_NODE_TOGGLE) s_view.collapsed_nodes ^= (uint64_t)1 << arg;
        s_view.frozen = true; s_view.detail = UI_DETAIL_NONE; break;
    case UI_ACTION_FIELD: {
        if (!s || arg < 0 || arg >= s->descriptor_field_count) return false;
        const usb_evidence_descriptor_field_t *f = &s->descriptor_fields[arg];
        int node = -1;
        for (unsigned i = 0; i < s->descriptor_node_count; i++)
            if (s->descriptor_nodes[i].node_id == f->node_id) { node = (int)i; break; }
        if (node < 0) return false;
        select_node(node); s_view.field_index = (int16_t)arg;
        s_view.byte_offset = (uint16_t)(s->descriptor_nodes[node].source_offset + f->byte_offset);
        s_view.byte_length = f->byte_length;
        s_view.frozen = true; s_view.detail = UI_DETAIL_FIELD; break;
    }
    case UI_ACTION_RULE:
        if (!s || arg < 0 || arg >= 12) return false;
        s_view.rule_index = (int16_t)arg; s_view.byte_offset = s_view.byte_length = 0;
        s_view.event_index = -1;
        if (s->rules[arg].evidence_ref_count) select_reference(&s->rules[arg].evidence_refs[0]);
        s_view.frozen = true; s_view.detail = UI_DETAIL_RULE; break;
    case UI_ACTION_RULE_REFERENCE:
        if (!s || s_view.rule_index < 0 || arg < 0 ||
            arg >= s->rules[s_view.rule_index].evidence_ref_count || arg >= 4) return false;
        select_reference(&s->rules[s_view.rule_index].evidence_refs[arg]);
        s_view.frozen = true; s_view.detail = UI_DETAIL_BYTES; break;
    case UI_ACTION_EVENT:
        if (!s || arg < 0 || arg >= s->raw_event_count) return false;
        s_view.event_index = (int16_t)arg; s_view.byte_offset = s_view.byte_length = 0;
        s_view.frozen = true; s_view.detail = UI_DETAIL_EVENT; break;
    case UI_ACTION_BYTES:
        if (arg == 1) s_view.hex_enlarged = !s_view.hex_enlarged;
        s_view.frozen = s != NULL; s_view.detail = UI_DETAIL_BYTES; break;
    case UI_ACTION_RELATED_RULES:
        s_view.page = UI_PAGE_DIAGNOSTICS; s_view.diagnostic_tab = 0; s_view.frozen = s != NULL;
        if (s) {
            int best = -1, score = -1;
            for (unsigned i = 0; i < 12; i++) {
                const usb_evidence_rule_result_t *r = &s->rules[i];
                for (unsigned j = 0; j < r->evidence_ref_count && j < 4; j++) {
                    const usb_evidence_raw_event_t *source = ui_model_event_by_sequence(r->evidence_refs[j].event_sequence);
                    if (ui_model_event_stage(source) == s_view.stage) {
                        int candidate = r->verdict == USB_RULE_FAIL ? 2 : 1;
                        if (candidate > score) { best = (int)i; score = candidate; }
                    }
                }
            }
            if (best >= 0) {
                s_view.rule_index = (int16_t)best;
                if (s->rules[best].evidence_ref_count) select_reference(&s->rules[best].evidence_refs[0]);
                s_view.detail = UI_DETAIL_RULE;
            } else { s_view.diagnostic_tab = 1; s_view.event_stage_filter = true; }
        }
        break;
    case UI_ACTION_DESCRIPTOR_TAB:
        if (arg < 0 || arg > 1) return false;
        s_view.descriptor_tab = (uint8_t)arg;
        if (arg == 1 && s && s_view.node_index >= 0 && s_view.field_index < 0) select_node(s_view.node_index);
        s_view.frozen = s != NULL; break;
    case UI_ACTION_DIAGNOSTIC_TAB:
        if (arg < 0 || arg > 1) return false;
        s_view.diagnostic_tab = (uint8_t)arg; break;
    case UI_ACTION_RULE_FILTER:
        if (arg < 0 || arg > 3) return false;
        s_view.rule_filter = (uint8_t)arg; break;
    case UI_ACTION_EVENT_FILTER: s_view.event_stage_filter = !s_view.event_stage_filter; break;
    default: return false;
    }
    /* Compare logical fields, not struct padding. Repeating a selection must
     * not redraw an entire canvas or notify observers as if it changed. */
#define SAME(member) (before.member == s_view.member)
    if (!(SAME(snapshot) && SAME(snapshot_epoch) && SAME(page) && SAME(detail) &&
        SAME(stage) && SAME(event_index) && SAME(node_index) && SAME(field_index) &&
        SAME(rule_index) && SAME(byte_offset) && SAME(byte_length) &&
        SAME(collapsed_nodes) && SAME(descriptor_tab) && SAME(diagnostic_tab) &&
        SAME(rule_filter) && SAME(event_stage_filter) && SAME(frozen) &&
        SAME(updates_available) && SAME(reduced_motion) && SAME(reveal_serial) &&
        SAME(hex_enlarged))) notify();
#undef SAME
    return true;
}
