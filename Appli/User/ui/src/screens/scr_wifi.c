/*
 * Wi-Fi manager for the Settings page.
 *
 * The screen owns presentation state only. Radio, profile, and storage state
 * always come from wifi_get_snapshot(); all requests are deferred until the
 * LVGL owner has finished the current event/render pass.
 */
#include "scr_wifi.h"
#include "ui_fonts.h"
#include "../ui_styles.h"
#include "../ui_widgets.h"
#include "ui_deferred.h"
#include "ui_model.h"
#include "wifi_service.h"
#include "wifi_state.h"
#include "web_server.h"
#include <stdio.h>
#include <string.h>

#define WIFI_UI_ROWS             WIFI_SCAN_MAX_APS
#define WIFI_UI_PAGE_ROWS        4U
#define WIFI_UI_ROW_HEIGHT       48
#define WIFI_UI_POLL_MS          200
#define WIFI_UI_TEXT_MAX         192U
#define WIFI_UI_EPOCH_MASK       0x07ffffffUL

/* WHD scan records may contain an empty SSID; the UI deliberately has no
 * hidden-network entry or AP-mode workflow. Those records are not connectable. */
typedef enum { WIFI_VIEW_MANAGER, WIFI_VIEW_DISPLAY_WEB } wifi_view_t;
typedef enum { WIFI_LIST_NEARBY, WIFI_LIST_SAVED } wifi_list_t;
typedef enum { WIFI_PASSWORD_NONE, WIFI_PASSWORD_CONNECT,
               WIFI_PASSWORD_UPDATE } wifi_password_context_t;
typedef enum { WIFI_CONFIRM_NONE, WIFI_CONFIRM_DISCONNECT,
               WIFI_CONFIRM_FORGET, WIFI_CONFIRM_UPDATE_PASSWORD,
               WIFI_CONFIRM_CLEAR_FIRST, WIFI_CONFIRM_CLEAR_FINAL }
    wifi_confirm_action_t;
typedef enum { WIFI_PENDING_NONE, WIFI_PENDING_CONNECT_AP,
               WIFI_PENDING_CONNECT_SAVED, WIFI_PENDING_UPDATE_PASSWORD, WIFI_PENDING_FORGET,
               WIFI_PENDING_FORGET_ALL, WIFI_PENDING_DISCONNECT, WIFI_PENDING_TEMP }
    wifi_pending_kind_t;

typedef struct {
    wifi_pending_kind_t kind;
    rt_uint32_t operation;
    rt_uint32_t baseline_operation;
    rt_uint32_t baseline_revision;
    rt_uint32_t profile_id;
    bool remember;
    bool valid;
    /* Owned submission context: browsing another row must not retarget a
     * completion or move its credential into that row's textarea. */
    wifi_ap_record_t target_ap;
    bool has_ap;
    uint32_t draft_revision;
    char password[WIFI_PASSWORD_BYTES_MAX + 1U];
} wifi_pending_t;

static lv_obj_t *g_page;
static lv_obj_t *g_title;
static lv_obj_t *g_mode_wifi, *g_mode_display;
static lv_obj_t *g_status_panel, *g_phase_label, *g_ip_label, *g_meta_label;
static lv_obj_t *g_status_disconnect;
static lv_obj_t *g_tab_nearby, *g_tab_saved, *g_page_prev, *g_page_next;
static lv_obj_t *g_page_label, *g_auto_button, *g_scan_button;
static lv_obj_t *g_clear_button, *g_retry_button;
static lv_obj_t *g_list_panel, *g_empty_label;
static lv_obj_t *g_rows[WIFI_UI_ROWS], *g_row_titles[WIFI_UI_ROWS];
static lv_obj_t *g_row_meta[WIFI_UI_ROWS];
static lv_obj_t *g_display_panel, *g_web_label, *g_motion_button;
static lv_obj_t *g_details, *g_detail_info_panel, *g_detail_title, *g_detail_name;
static lv_obj_t *g_detail_meta, *g_detail_note, *g_detail_feedback;
static lv_obj_t *g_password_caption, *g_pwd_area, *g_remember_button;
static lv_obj_t *g_detail_connect, *g_detail_disconnect, *g_detail_update;
static lv_obj_t *g_detail_auto, *g_detail_forget, *g_detail_back;
static lv_obj_t *g_detail_temporary;
static lv_obj_t *g_input_overlay, *g_password_network, *g_password_show;
static lv_obj_t *g_password_count, *g_kb;
static lv_obj_t *g_confirm_overlay, *g_confirm_card, *g_confirm_title, *g_confirm_body;
static lv_obj_t *g_confirm_yes, *g_confirm_no;

static wifi_snapshot_t g_snapshot;
static wifi_ap_record_t g_selected_ap;
static uint8_t g_scan_order[WIFI_SCAN_MAX_APS];
static uint8_t g_scan_visible_count;
static uint8_t g_hidden_scan_count;
static uint8_t g_row_kind[WIFI_UI_ROWS];
static uint32_t g_row_token[WIFI_UI_ROWS];
static uint32_t g_render_epoch;
static uint32_t g_render_scan_generation;
static uint32_t g_render_config_revision;
static uint32_t g_render_active_profile;
static uint32_t g_render_publication_stamp;
static wifi_list_t g_render_list;
static uint8_t g_render_page;
static uint32_t g_selected_profile_id;
static uint8_t g_list_page;
static rt_tick_t g_last_poll_tick;
static bool g_poll_started;
static bool g_snapshot_valid;
static rt_tick_t g_snapshot_observed_tick;
static bool g_snapshot_poll_error;
static bool g_details_open;
static bool g_selected_ap_valid;
static bool g_remember_password;
static bool g_editor_open, g_password_visible, g_update_password_armed;
static bool g_save_retry_requested;
static uint32_t g_save_retry_origin,g_save_retry_revision;
static wifi_view_t g_view;
static wifi_list_t g_list;
static wifi_password_context_t g_password_context;
static wifi_confirm_action_t g_confirm_action;
static wifi_pending_t g_pending;
static wifi_pending_t g_recovery;
/* One bounded alternate editor draft, used only when explicit recovery opens
 * A while the user has already edited B. This is not a credential history. */
static wifi_pending_t g_parked_draft;
static uint32_t g_draft_revision;
static char g_password_before[WIFI_PASSWORD_BYTES_MAX + 1U];
static uint8_t g_draft_ssid[WIFI_SSID_BYTES_MAX],g_draft_ssid_length;
static wifi_security_t g_draft_security;
static char g_password_chars[96];
static char g_feedback[WIFI_UI_TEXT_MAX];
static wifi_ap_record_t g_feedback_target;
static bool g_feedback_scoped;
static uint32_t g_feedback_operation;
static unsigned g_create_stage;
static bool g_create_ready;

static const char *const s_keys_lower[] = {
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","Del","\n",
    "ABC","z","x","c","v","b","n","m",".",",","\n",
    "Cancel","123","Space","Done",""
};
static const char *const s_keys_upper[] = {
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","Del","\n",
    "abc","Z","X","C","V","B","N","M",".",",","\n",
    "Cancel","123","Space","Done",""
};
static const char *const s_keys_special[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "!","@","#","$","%","^","&","*","(",")","\n",
    "abc","-","_","+","=","/","\\","?",".","Del","\n",
    "Cancel","More","Space","Done",""
};
static const char *const s_keys_more[] = {
    "[","]","{","}","<",">",":",";","'","\"","\n",
    "~","\x60","|","!","@","#","$","%","^","&","\n",
    "abc","*","(",")","-","_","+","=","?","Del","\n",
    "Cancel","123","Space","Done",""
};
static lv_buttonmatrix_ctrl_t s_key_controls[34];

enum {
    PWD_CANCEL = 256, PWD_DONE, PWD_SHOW, PWD_CLEAR, PWD_LEFT, PWD_RIGHT,
    PWD_LOWER, PWD_UPPER, PWD_SPECIAL, PWD_MORE, PWD_DELETE
};

static void password_action_commit(uintptr_t action);
static void refresh_snapshot_ui(void);
static void refresh_details(void);
static const char *password_text(void);
static void open_connection_recovery(void);

static void reset_widget_refs(void)
{
    g_page = NULL;
    g_title = NULL;
    g_mode_wifi = g_mode_display = NULL;
    g_status_panel = g_phase_label = g_ip_label = g_meta_label = NULL;
    g_status_disconnect = NULL;
    g_tab_nearby = g_tab_saved = g_page_prev = g_page_next = NULL;
    g_page_label = g_auto_button = g_scan_button = NULL;
    g_clear_button = g_retry_button = NULL;
    g_list_panel = g_empty_label = NULL;
    for (unsigned index = 0U; index < WIFI_UI_ROWS; ++index) {
        g_rows[index] = NULL;
        g_row_titles[index] = NULL;
        g_row_meta[index] = NULL;
    }
    g_display_panel = g_web_label = g_motion_button = NULL;
    g_details = g_detail_info_panel = g_detail_title = g_detail_name = NULL;
    g_detail_meta = g_detail_note = g_detail_feedback = NULL;
    g_password_caption = g_pwd_area = g_remember_button = NULL;
    g_detail_connect = g_detail_disconnect = g_detail_update = NULL;
    g_detail_auto = g_detail_forget = g_detail_back = NULL;
    g_detail_temporary = NULL;
    g_input_overlay = g_password_network = g_password_show = NULL;
    g_password_count = g_kb = NULL;
    g_confirm_overlay = g_confirm_card = g_confirm_title = g_confirm_body = NULL;
    g_confirm_yes = g_confirm_no = NULL;
    g_create_stage = 0U;
    g_create_ready = false;
}

static bool click_release(lv_event_t *event)
{
    return scr_wifi_is_ready() && event &&
           lv_event_get_code(event) == LV_EVENT_CLICKED &&
           ui_event_is_release_click(event) &&
           !lv_obj_is_hidden(lv_event_get_current_target(event));
}

static void set_label(lv_obj_t *label, const char *text)
{
    if (!label) return;
    if (!text) text = "";
    if (strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
}

static void set_label_safe(lv_obj_t *label, const char *text)
{
    char safe[WIFI_UI_TEXT_MAX];
    if (!label) return;
    (void)ui_font_sanitize_dynamic_text(text ? text : "", safe, sizeof(safe));
    set_label(label, safe);
}

static void set_button_label(lv_obj_t *button, const char *text)
{
    lv_obj_t *label;
    if (!button) return;
    label = lv_obj_get_child(button, 0);
    if (label) set_label_safe(label, text);
}

static void set_button_enabled(lv_obj_t *button, bool enabled)
{
    if (!button) return;
    if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
}

static lv_obj_t *wifi_text(lv_obj_t *parent, const char *text,
                           const lv_font_t *font, lv_color_t color,
                           int x, int y, int width, int height)
{
    lv_obj_t *label = ui_text(parent, text, font, color);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    return label;
}

static lv_obj_t *wifi_button(lv_obj_t *parent, const char *text,
                             int x, int y, int width,
                             lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_t *label;
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, UI_TOUCH_HEIGHT);
    lv_obj_set_layout(button, LV_LAYOUT_NONE);
    lv_obj_set_style_radius(button, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(button, 4, 0);
    lv_obj_set_style_bg_color(button, C_RAISED, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, C_SURFACE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, C_SURFACE, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_color_filter_opa(button, 0, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(button, C_DIVIDER, 0);
    lv_obj_set_style_border_color(button, C_BLUE, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_outline_color(button, C_BLUE, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(button, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_scrollable(button, false);
    label = ui_text(button, text, F_BODY, C_TEXT);
    lv_obj_set_width(label, width - 12);
    lv_obj_set_height(label, 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(label);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    return button;
}

static void wifi_button_selected(lv_obj_t *button, bool selected)
{
    lv_obj_t *label;
    if (!button) return;
    lv_obj_set_style_bg_color(button, selected ? C_SURFACE : C_RAISED, 0);
    lv_obj_set_style_border_color(button, selected ? C_BLUE : C_DIVIDER, 0);
    label = lv_obj_get_child(button, 0);
    if (label) lv_obj_set_style_text_color(label, selected ? C_BLUE : C_GRAY, 0);
}

static lv_obj_t *wifi_panel(lv_obj_t *parent, int x, int y,
                            int width, int height)
{
    lv_obj_t *panel = ui_panel(parent, x, y, width, height);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, C_SURFACE, 0);
    lv_obj_set_style_border_color(panel, C_DIVIDER, 0);
    return panel;
}

static void set_feedback(const char *text)
{
    if (!text) text = "";
    (void)snprintf(g_feedback, sizeof(g_feedback), "%s", text);
    g_feedback_scoped = false;
    set_label_safe(g_detail_feedback, g_feedback);
}

static const char *phase_text(wifi_phase_t phase)
{
    switch (phase) {
    case WIFI_PHASE_INITIALIZING: return "正在初始化";
    case WIFI_PHASE_IDLE: return "待命";
    case WIFI_PHASE_SCANNING: return "正在扫描";
    case WIFI_PHASE_ASSOCIATING: return "正在关联";
    case WIFI_PHASE_DHCP: return "正在获取地址";
    case WIFI_PHASE_ONLINE: return "已连接";
    case WIFI_PHASE_RETRY_WAIT: return "等待重试";
    case WIFI_PHASE_CANCELLING: return "正在断开";
    case WIFI_PHASE_DISCONNECTED: return "未连接";
    case WIFI_PHASE_FAILED: return "连接失败";
    default: return "状态未知";
    }
}

static const char *reason_text(wifi_reason_t reason)
{
    switch (reason) {
    case WIFI_REASON_NONE: return "无错误";
    case WIFI_REASON_NOT_FOUND: return "未找到网络";
    case WIFI_REASON_AUTH: return "认证失败";
    case WIFI_REASON_AUTH_INCOMPLETE: return "认证未完成";
    case WIFI_REASON_UNSUPPORTED: return "安全类型不支持";
    case WIFI_REASON_DHCP: return "地址获取失败";
    case WIFI_REASON_STORAGE: return "配置保存失败";
    case WIFI_REASON_IO: return "无线设备错误";
    case WIFI_REASON_CANCELLED: return "操作已取消";
    case WIFI_REASON_FULL: return "已保存 8 个网络";
    case WIFI_REASON_BUSY: return "服务正忙";
    case WIFI_REASON_CORRUPT: return "配置数据损坏";
    case WIFI_REASON_FOREIGN: return "配置来源不匹配";
    case WIFI_REASON_INVALID: return "请求无效";
    default: return "原因未知";
    }
}

static const char *storage_text(wifi_storage_status_t storage)
{
    switch (storage) {
    case WIFI_STORAGE_LOADING: return "正在读取配置";
    case WIFI_STORAGE_EMPTY: return "尚无已保存网络";
    case WIFI_STORAGE_OK: return "配置已保存";
    case WIFI_STORAGE_SAVING: return "正在保存配置";
    case WIFI_STORAGE_FAILED: return "保存失败，可重试";
    case WIFI_STORAGE_CORRUPT: return "配置损坏，未覆盖";
    case WIFI_STORAGE_FOREIGN: return "配置来源不匹配，未覆盖";
    default: return "存储状态未知";
    }
}

static const char *security_text(wifi_security_t security)
{
    switch (security) {
    case WIFI_SECURITY_OPEN: return "开放网络";
    case WIFI_SECURITY_WPA2: return "WPA2";
    case WIFI_SECURITY_WPA_MIXED: return "WPA / WPA2 混合";
    case WIFI_SECURITY_UNSUPPORTED: return "不支持的安全类型";
    default: return "未知安全类型";
    }
}

static wifi_security_t ap_security(const wifi_ap_record_t *ap)
{
    if (!ap) return WIFI_SECURITY_UNSUPPORTED;
    if ((unsigned)ap->security_mode > (unsigned)WIFI_SECURITY_UNSUPPORTED)
        return WIFI_SECURITY_UNSUPPORTED;
    return ap->security_mode;
}

static bool ap_security_supported(const wifi_ap_record_t *ap)
{
    wifi_security_t security = ap_security(ap);
    return security == WIFI_SECURITY_OPEN || security == WIFI_SECURITY_WPA2 ||
           security == WIFI_SECURITY_WPA_MIXED;
}

static uint8_t ap_ssid_length(const wifi_ap_record_t *ap)
{
    if (!ap) return 0U;
    return ap->ssid_length > WIFI_SSID_BYTES_MAX ?
           WIFI_SSID_BYTES_MAX : ap->ssid_length;
}

static void ap_name(const wifi_ap_record_t *ap, char *output, size_t capacity)
{
    char input[WIFI_SSID_BYTES_MAX + 1U];
    size_t length, index;
    if (!output || capacity == 0U) return;
    output[0] = '\0';
    if (!ap) return;
    length = ap_ssid_length(ap);
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)ap->ssid[index];
        input[index] = (value == 0U || value < 0x20U || value == 0x7fU) ?
                       '?' : (char)value;
    }
    input[length] = '\0';
    if (length == 0U) (void)snprintf(input, sizeof(input), "隐藏 SSID");
    (void)ui_font_sanitize_dynamic_text(input, output, capacity);
    if (!output[0]) (void)snprintf(output, capacity, "不可显示的网络名");
}

static void profile_name(const wifi_profile_summary_t *profile,
                         char *output, size_t capacity)
{
    char input[WIFI_SSID_BYTES_MAX + 1U];
    size_t length, index;
    if (!output || capacity == 0U) return;
    output[0] = '\0';
    if (!profile) return;
    length = profile->ssid_length > WIFI_SSID_BYTES_MAX ?
             WIFI_SSID_BYTES_MAX : profile->ssid_length;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)profile->ssid[index];
        input[index] = (value == 0U || value < 0x20U || value == 0x7fU) ?
                       '?' : (char)value;
    }
    input[length] = '\0';
    if (length == 0U) (void)snprintf(input, sizeof(input), "隐藏 SSID");
    (void)ui_font_sanitize_dynamic_text(input, output, capacity);
    if (!output[0]) (void)snprintf(output, capacity, "不可显示的网络名");
}

static void format_bssid(const uint8_t bssid[6], char *output, size_t capacity)
{
    (void)snprintf(output, capacity, "%02X:%02X:%02X:%02X:%02X:%02X",
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

static bool profile_matches_ap(const wifi_profile_summary_t *profile,
                               const wifi_ap_record_t *ap)
{
    uint8_t length;
    if (!profile || !ap) return false;
    length = ap_ssid_length(ap);
    return length != 0U && profile->ssid_length == length &&
           memcmp(profile->ssid, ap->ssid, length) == 0 &&
           profile->security == ap_security(ap);
}

static const wifi_profile_summary_t *profile_by_id(uint32_t id)
{
    unsigned count = g_snapshot.network.profile_count;
    if (count > WIFI_PROFILE_MAX) count = WIFI_PROFILE_MAX;
    for (unsigned index = 0U; index < count; ++index)
        if (g_snapshot.network.profiles[index].id == id)
            return &g_snapshot.network.profiles[index];
    return NULL;
}

static const wifi_profile_summary_t *profile_matching_ap(
    const wifi_ap_record_t *ap)
{
    unsigned count = g_snapshot.network.profile_count;
    if (count > WIFI_PROFILE_MAX) count = WIFI_PROFILE_MAX;
    for (unsigned index = 0U; index < count; ++index)
        if (profile_matches_ap(&g_snapshot.network.profiles[index], ap))
            return &g_snapshot.network.profiles[index];
    return NULL;
}

static bool ap_connectable(const wifi_ap_record_t *ap)
{
    return ap && ap_ssid_length(ap) != 0U &&
           ap->ssid_length <= WIFI_SSID_BYTES_MAX &&
           !(ap->flags & WIFI_AP_FLAG_SSID_TRUNCATED) &&
           ap_security_supported(ap);
}

static bool ap_same_identity(const wifi_ap_record_t *a,
                             const wifi_ap_record_t *b)
{
    uint8_t length;
    if (!a || !b) return false;
    length = ap_ssid_length(a);
    return length != 0U && length == ap_ssid_length(b) &&
           memcmp(a->ssid, b->ssid, length) == 0 &&
           a->security == b->security &&
           a->security_mode == b->security_mode &&
           memcmp(a->bssid, b->bssid, sizeof(a->bssid)) == 0;
}

static const char *scan_empty_text(void)
{
    if (!g_snapshot_valid) return "WiFi 服务状态暂不可用";
    if (g_hidden_scan_count && g_scan_visible_count == 0U)
        return "仅发现隐藏名称网络；此页面不支持连接隐藏 SSID";
    if (g_snapshot.scan_status == WIFI_SCAN_STATUS_SCANNING)
        return "正在扫描附近网络…";
    if (g_snapshot.scan_status == WIFI_SCAN_STATUS_FAILED)
        return "扫描失败，请重试";
    if (g_snapshot.scan_status == WIFI_SCAN_STATUS_CANCELLED)
        return "扫描已取消，可重新扫描";
    if (g_snapshot.scan_status == WIFI_SCAN_STATUS_TRUNCATED)
        return "扫描结果不完整，请重新扫描";
    if (g_snapshot.scan_status == WIFI_SCAN_STATUS_EMPTY)
        return "附近没有可显示的网络";
    if (g_hidden_scan_count)
        return "无可显示网络（隐藏 SSID 暂不支持）";
    return "尚无扫描结果，请点击扫描";
}

static void build_scan_order(void)
{
    unsigned count = g_snapshot.scan_count;
    g_scan_visible_count = 0U;
    g_hidden_scan_count = 0U;
    if (count > WIFI_SCAN_MAX_APS) count = WIFI_SCAN_MAX_APS;
    for (unsigned index = 0U; index < count; ++index) {
        const wifi_ap_record_t *ap = &g_snapshot.access_points[index];
        if (ap_ssid_length(ap) == 0U) {
            ++g_hidden_scan_count;
            continue;
        }
        bool duplicate=false;
        for(unsigned row=0;row<g_scan_visible_count;row++) {
            const wifi_ap_record_t *old=&g_snapshot.access_points[g_scan_order[row]];
            if(ap_security(old)==ap_security(ap) && ap_ssid_length(old)==ap_ssid_length(ap) &&
               !memcmp(old->ssid,ap->ssid,ap_ssid_length(ap))) {
                if(ap->rssi>old->rssi)g_scan_order[row]=(uint8_t)index;
                duplicate=true;break;
            }
        }
        if(!duplicate)g_scan_order[g_scan_visible_count++] = (uint8_t)index;
    }
    /* Group by the normalized public security mode, then sort strongest first
     * within each group. Rows retain their original snapshot record index. */
    for (unsigned outer = 0U; outer < g_scan_visible_count; ++outer) {
        for (unsigned inner = outer + 1U; inner < g_scan_visible_count; ++inner) {
            const wifi_ap_record_t *a = &g_snapshot.access_points[g_scan_order[outer]];
            const wifi_ap_record_t *b = &g_snapshot.access_points[g_scan_order[inner]];
            if (ap_security(b) < ap_security(a) ||
                (ap_security(b) == ap_security(a) && b->rssi > a->rssi)) {
                uint8_t temporary = g_scan_order[outer];
                g_scan_order[outer] = g_scan_order[inner];
                g_scan_order[inner] = temporary;
            }
        }
    }
}

static const char *phase_line(void)
{
    if (!g_snapshot_valid) return "WiFi 服务不可用";
    if (!g_snapshot.network.radio_ready) {
        if (g_snapshot.network.radio_init_state == WIFI_RADIO_INIT_FAILED)
            return "初始化失败";
        return "正在初始化";
    }
    return phase_text(g_snapshot.network.phase);
}

static bool is_connection_pending(wifi_pending_kind_t kind)
{
    return kind == WIFI_PENDING_CONNECT_AP || kind == WIFI_PENDING_CONNECT_SAVED;
}

static bool connection_request_waiting(void)
{
    return g_pending.valid && is_connection_pending(g_pending.kind) &&
        (int32_t)(g_snapshot.network.connection_operation_id-g_pending.operation)<0;
}

static bool connection_transition(void)
{
    if (!g_snapshot_valid) return true;
    if(!g_snapshot.network.radio_ready)return true;
    wifi_phase_t phase = g_snapshot.network.phase;
    return phase == WIFI_PHASE_ASSOCIATING || phase == WIFI_PHASE_DHCP ||
        phase == WIFI_PHASE_RETRY_WAIT || phase == WIFI_PHASE_CANCELLING ||
        connection_request_waiting() ||
        (g_pending.valid && g_pending.kind == WIFI_PENDING_UPDATE_PASSWORD &&
         (int32_t)(g_snapshot.network.connection_operation_id-g_pending.operation)<0);
}

static bool same_network(const wifi_ap_record_t *a, const wifi_ap_record_t *b)
{
    return ap_ssid_length(a) != 0U && ap_ssid_length(a) == ap_ssid_length(b) &&
        ap_security(a) == ap_security(b) &&
        !memcmp(a->ssid, b->ssid, ap_ssid_length(a));
}

static bool reason_describes_storage(wifi_reason_t reason)
{
    return reason == WIFI_REASON_STORAGE || reason == WIFI_REASON_CORRUPT ||
           reason == WIFI_REASON_FOREIGN;
}

static const char *status_feedback(const wifi_public_snapshot_t *network)
{
    if (!g_feedback[0]) return NULL;

    /* The online headline and saved-network tab already confirm these outcomes. */
    if (network->phase == WIFI_PHASE_ONLINE &&
        network->storage == WIFI_STORAGE_OK &&
        (!strcmp(g_feedback, "连接和已保存配置均由服务确认") ||
         !strcmp(g_feedback, "此网络已连接")))
        return NULL;
    if (network->temporary &&
        (!strcmp(g_feedback, "临时连接已由服务确认；配置保存状态见上方") ||
         !strcmp(g_feedback, "临时连接已由服务确认，不保存密码")))
        return NULL;
    if (!strcmp(g_feedback, "WiFi 配置保存成功")) return "保存完成";
    return g_feedback;
}

static const char *status_notice(const wifi_public_snapshot_t *network,
                                 char *formatted, size_t capacity)
{
    const char *feedback;
    if (network->cleanup_pending) return "配置已提交，清理失败";

    switch (network->storage) {
    case WIFI_STORAGE_FAILED:
    case WIFI_STORAGE_CORRUPT:
    case WIFI_STORAGE_FOREIGN:
        return storage_text(network->storage);
    default:
        break;
    }

    if (network->reason != WIFI_REASON_NONE &&
        network->reason != WIFI_REASON_CANCELLED &&
        !reason_describes_storage(network->reason) && formatted && capacity) {
        (void)snprintf(formatted, capacity, "%s：%s",
            connection_transition() ? "上次原因" : "原因",
            reason_text(network->reason));
        return formatted;
    }

    if (network->storage == WIFI_STORAGE_LOADING ||
        network->storage == WIFI_STORAGE_SAVING)
        return storage_text(network->storage);

    if (network->pending_changes &&
        !(network->phase == WIFI_PHASE_ONLINE && !network->temporary))
        return "配置待保存";

    if (network->temporary) return "临时连接 · 配置不保存";

    feedback = status_feedback(network);
    return feedback;
}

static void set_pending_feedback(const char *text)
{
    set_feedback(text);
    g_feedback_target = g_pending.target_ap;
    g_feedback_operation = g_pending.operation;
    g_feedback_scoped = ap_ssid_length(&g_feedback_target) != 0U;
}

static const char *ap_connection_mark(const wifi_ap_record_t *ap)
{
    const wifi_public_snapshot_t *network = &g_snapshot.network;
    if (g_recovery.valid && same_network(ap, &g_recovery.target_ap) &&
        network->connection_operation_id == g_recovery.operation && network->phase == WIFI_PHASE_FAILED)
        return "  ·  连接失败";
    if (is_connection_pending(g_pending.kind) && same_network(ap, &g_pending.target_ap) &&
        (network->connection_operation_id == g_pending.operation || g_pending.valid)) {
        if (network->connection_operation_id != g_pending.operation) return "  ·  请求已提交";
        if (network->phase == WIFI_PHASE_ASSOCIATING || network->phase == WIFI_PHASE_DHCP ||
            network->phase == WIFI_PHASE_RETRY_WAIT) return "  ·  连接中";
        if (network->phase == WIFI_PHASE_ONLINE) return network->temporary ? "  ·  当前 · 临时" : "  ·  当前";
        if (network->phase == WIFI_PHASE_FAILED) return "  ·  连接失败";
    }
    const wifi_profile_summary_t *profile = profile_matching_ap(ap);
    if (!profile) return "";
    return profile->id == network->active_profile_id && network->phase == WIFI_PHASE_ONLINE ?
        "  ·  当前" : (profile->persisted ? "  ·  已保存" : "  ·  待保存");
}

static void update_status(void)
{
    char ip[32], ip_label[48], meta[WIFI_UI_TEXT_MAX], headline[WIFI_UI_TEXT_MAX];
    const wifi_public_snapshot_t *network = &g_snapshot.network;
    if (!scr_wifi_is_ready()) return;
    if (!g_snapshot_valid) {
        set_label(g_phase_label, "WiFi 服务不可用");
        set_label(g_ip_label, "IP：—");
        set_label(g_meta_label, g_feedback[0] ? g_feedback :
                  "无法读取 WiFi 状态");
        lv_obj_set_hidden(g_meta_label, false);
        lv_obj_set_hidden(g_status_disconnect, true);
        return;
    }
    (void)ui_font_sanitize_dynamic_text(network->ip, ip, sizeof(ip));
    if (ip[0]) (void)snprintf(ip_label, sizeof(ip_label), "IP：%s", ip);
    else (void)snprintf(ip_label, sizeof(ip_label), "IP：—");
    wifi_ap_record_t shown = {0};
    bool submitted = connection_request_waiting();
    if (submitted) shown = g_pending.target_ap;
    else {
        shown.ssid_length = network->ssid_length > WIFI_SSID_BYTES_MAX ? WIFI_SSID_BYTES_MAX : network->ssid_length;
        memcpy(shown.ssid, network->ssid, shown.ssid_length);
    }
    if (shown.ssid_length) {
        char name[WIFI_UI_TEXT_MAX];
        ap_name(&shown, name, sizeof(name));
        const char *phase = submitted ? "请求已提交" : phase_line();
        if (!submitted && network->phase == WIFI_PHASE_ONLINE &&
            !network->temporary && network->pending_changes) phase = "已连接 · 未保存";
        (void)snprintf(headline, sizeof(headline), "%s · %.*s",
            phase, 120, name);
        set_label_safe(g_phase_label, headline);
    } else set_label(g_phase_label, phase_line());
    const char *notice = status_notice(network, meta, sizeof(meta));
    if (!notice) meta[0] = '\0';
    else if (notice != meta)
        (void)snprintf(meta, sizeof(meta), "%s", notice);
    set_label_safe(g_ip_label, ip_label);
    if (meta[0]) {
        set_label_safe(g_meta_label, meta);
        lv_obj_set_hidden(g_meta_label, false);
    } else {
        set_label(g_meta_label, "");
        lv_obj_set_hidden(g_meta_label, true);
    }
    {
        wifi_phase_t phase = network->phase;
        bool active = submitted || phase == WIFI_PHASE_ONLINE ||
            phase == WIFI_PHASE_ASSOCIATING || phase == WIFI_PHASE_DHCP ||
            phase == WIFI_PHASE_RETRY_WAIT || phase == WIFI_PHASE_CANCELLING;
        set_button_label(g_status_disconnect,
            phase == WIFI_PHASE_ONLINE && !submitted ? "断开" : "取消连接");
        lv_obj_set_hidden(g_status_disconnect, !active);
    }
    set_button_label(g_auto_button, network->auto_connect ?
                     "全局自动：开" : "全局自动：关");
    set_button_enabled(g_scan_button,
        network->initialized && g_snapshot.scan_status != WIFI_SCAN_STATUS_SCANNING && !connection_transition());
    bool storage_retry = network->storage == WIFI_STORAGE_FAILED;
    set_button_label(g_retry_button, storage_retry ? "重试保存" : "修改重试");
    set_button_enabled(g_retry_button, storage_retry || g_recovery.valid);
    lv_obj_set_hidden(g_retry_button, !storage_retry && !g_recovery.valid);
}

static unsigned list_count(void)
{
    if (!g_snapshot_valid) return 0U;
    if (g_list == WIFI_LIST_NEARBY) return g_scan_visible_count;
    return g_snapshot.network.profile_count > WIFI_PROFILE_MAX ?
           WIFI_PROFILE_MAX : g_snapshot.network.profile_count;
}

static uint32_t publication_stamp(void)
{
    const wifi_public_snapshot_t *n=&g_snapshot.network;
    return ((n->pending_changes?1U:0U)|(n->cleanup_pending?2U:0U)|
        ((uint32_t)n->saved_profile_count<<2)|((uint32_t)n->storage<<6)|((uint32_t)n->phase<<10)|
        ((uint32_t)n->reason<<16)) ^ (n->connection_operation_id * 2654435761U);
}
static void update_list_pager(void)
{
    if (!scr_wifi_is_ready()) return;
    unsigned count = list_count();
    unsigned page_count = (count + WIFI_UI_PAGE_ROWS - 1U) / WIFI_UI_PAGE_ROWS;
    if (page_count == 0U) g_list_page = 0U;
    else if (g_list_page >= page_count) g_list_page = (uint8_t)(page_count - 1U);
    unsigned first = (unsigned)g_list_page * WIFI_UI_PAGE_ROWS;
    set_button_label(g_tab_nearby, "附近网络");
    if (g_snapshot_valid) {
        char tab[48];
        (void)snprintf(tab, sizeof(tab), "%s (%u)",
            g_snapshot.network.pending_changes?(g_snapshot.network.cleanup_pending?"待清理":"待保存"):"已保存",
            (unsigned)g_snapshot.network.profile_count);
        set_button_label(g_tab_saved, tab);
    } else set_button_label(g_tab_saved, "已保存 (—)");
    wifi_button_selected(g_tab_nearby, g_list == WIFI_LIST_NEARBY);
    wifi_button_selected(g_tab_saved, g_list == WIFI_LIST_SAVED);
    if (count == 0U) set_label(g_page_label, "0 / 0");
    else {
        char page_text[40];
        unsigned last = first + WIFI_UI_PAGE_ROWS;
        if (last > count) last = count;
        (void)snprintf(page_text, sizeof(page_text), "%u–%u / %u",
            first + 1U, last, count);
        set_label(g_page_label, page_text);
    }
    set_button_enabled(g_page_prev, page_count > 1U && g_list_page > 0U);
    set_button_enabled(g_page_next, page_count > 1U &&
                       (unsigned)g_list_page + 1U < page_count);
}
static void render_rows(void)
{
    if (!scr_wifi_is_ready()) return;
    unsigned count = list_count();
    ++g_render_epoch;
    if (g_render_epoch == 0U) ++g_render_epoch;
    update_list_pager();
    lv_obj_set_hidden(g_clear_button, g_list != WIFI_LIST_SAVED);
    lv_obj_set_hidden(g_empty_label, count != 0U);
    if (count == 0U) set_label_safe(g_empty_label,
        g_list == WIFI_LIST_NEARBY ? scan_empty_text() :
        (g_snapshot_valid ? (g_snapshot.network.pending_changes?"尚有未保存修改，请重试保存":"没有已保存网络") : "WiFi 服务状态暂不可用"));

    for (unsigned slot = 0U; slot < WIFI_UI_ROWS; ++slot) {
        unsigned item = slot;
        g_row_kind[slot] = (uint8_t)g_list;
        g_row_token[slot] = UINT32_MAX;
        if (item >= count) {
            lv_obj_set_hidden(g_rows[slot], true);
            continue;
        }
        lv_obj_set_hidden(g_rows[slot], false);
        if (g_list == WIFI_LIST_NEARBY) {
            unsigned record_index = g_scan_order[item];
            const wifi_ap_record_t *ap = &g_snapshot.access_points[record_index];
            char name[WIFI_UI_TEXT_MAX], bssid[24], detail[WIFI_UI_TEXT_MAX];
            wifi_security_t security = ap_security(ap);
            ap_name(ap, name, sizeof(name));
            format_bssid(ap->bssid, bssid, sizeof(bssid));
            (void)snprintf(detail, sizeof(detail), "%s  ·  %d dBm  ·  CH %d  ·  %s%s%s",
                security_text(security), (int)ap->rssi, (int)ap->channel, bssid,
                ap_connection_mark(ap),
                ap_connectable(ap) ? "" : "  ·  不可连接");
            set_label_safe(g_row_titles[slot], name);
            set_label_safe(g_row_meta[slot], detail);
            bool failed = g_recovery.valid && same_network(ap, &g_recovery.target_ap) &&
                g_snapshot.network.phase == WIFI_PHASE_FAILED;
            lv_obj_set_style_text_color(g_row_meta[slot], failed ? C_RED : C_GRAY, 0);
            g_row_token[slot] = record_index;
            wifi_button_selected(g_rows[slot], g_selected_ap_valid &&
                ap_same_identity(&g_selected_ap, ap));
        } else {
            const wifi_profile_summary_t *profile =
                &g_snapshot.network.profiles[item];
            char name[WIFI_UI_TEXT_MAX], detail[WIFI_UI_TEXT_MAX];
            const char *mark = "";
            wifi_phase_t phase = g_snapshot.network.phase;
            if (connection_request_waiting() && g_pending.profile_id == profile->id)
                mark = "  ·  请求已提交";
            else if (profile->id == g_snapshot.network.active_profile_id) {
                if (phase == WIFI_PHASE_ONLINE) mark = "  ·  当前";
                else if (phase == WIFI_PHASE_FAILED) mark = "  ·  连接失败";
                else if (phase == WIFI_PHASE_ASSOCIATING || phase == WIFI_PHASE_DHCP ||
                    phase == WIFI_PHASE_RETRY_WAIT || phase == WIFI_PHASE_CANCELLING) mark = "  ·  连接中";
            }
            profile_name(profile, name, sizeof(name));
            (void)snprintf(detail, sizeof(detail), "%s  ·  自动连接 %s%s%s",
                security_text(profile->security),
                profile->auto_connect ? "开" : "关",
                profile->persisted ? "" : "  ·  待保存",
                mark);
            set_label_safe(g_row_titles[slot], name);
            set_label_safe(g_row_meta[slot], detail);
            bool failed = g_recovery.valid && profile_matches_ap(profile, &g_recovery.target_ap) &&
                phase == WIFI_PHASE_FAILED;
            lv_obj_set_style_text_color(g_row_meta[slot], failed ? C_RED : C_GRAY, 0);
            g_row_token[slot] = profile->id;
            wifi_button_selected(g_rows[slot], g_details_open &&
                !g_selected_ap_valid && g_selected_profile_id == profile->id);
        }
    }
    g_render_scan_generation = g_snapshot_valid ?
        g_snapshot.scan_generation : UINT32_MAX;
    g_render_config_revision = g_snapshot_valid ?
        g_snapshot.network.config_revision : UINT32_MAX;
    g_render_active_profile = g_snapshot_valid ?
        g_snapshot.network.active_profile_id : UINT32_MAX;
    g_render_publication_stamp=publication_stamp();
    g_render_list = g_list;
    g_render_page = g_list_page;
}

static void list_scroll_cb(lv_event_t *event)
{
    if (!scr_wifi_is_ready() || !event ||
        lv_event_get_code(event)!=LV_EVENT_SCROLL)return;
    int32_t y=lv_obj_get_scroll_y(g_list_panel);
    g_list_page=(uint8_t)(y>0?(y+WIFI_UI_PAGE_ROWS*WIFI_UI_ROW_HEIGHT-1)/
        (WIFI_UI_PAGE_ROWS*WIFI_UI_ROW_HEIGHT):0);
    update_list_pager();g_render_page=g_list_page;
}

static void draw_display_view(void)
{
    char address[96], ip[32];
    if (!scr_wifi_is_ready()) return;
    /* Mongoose owns web startup through WIFI_AUTO_START_WEB; this view reports
     * the listener address and keeps the independent display preference. */
    bool online = web_server_is_online() != 0;
    const wifi_public_snapshot_t *network = &g_snapshot.network;
    if (online && g_snapshot_valid && network->phase == WIFI_PHASE_ONLINE &&
        network->ip[0]) {
        (void)ui_font_sanitize_dynamic_text(network->ip, ip, sizeof(ip));
        (void)snprintf(address, sizeof(address), "证据网页：http://%s/", ip);
        set_label_safe(g_web_label, address);
        lv_obj_set_style_text_color(g_web_label, C_GREEN, 0);
    } else if (online) {
        set_label(g_web_label, "证据网页已启动，等待网络地址");
        lv_obj_set_style_text_color(g_web_label, C_BLUE, 0);
    } else {
        set_label(g_web_label, "证据网页尚未启动");
        lv_obj_set_style_text_color(g_web_label, C_GRAY, 0);
    }
    set_button_label(g_motion_button, ui_model_get()->reduced_motion ?
        "当前：即时切换" : "当前：流畅动效");
    wifi_button_selected(g_motion_button, ui_model_get()->reduced_motion);
}

static void refresh_snapshot_ui(void)
{
    bool rows_changed;
    if (!scr_wifi_is_ready() || !g_snapshot_valid) return;
    if (g_render_scan_generation != g_snapshot.scan_generation ||
        g_render_config_revision != g_snapshot.network.config_revision)
        build_scan_order();
    rows_changed = g_render_scan_generation != g_snapshot.scan_generation ||
        g_render_config_revision != g_snapshot.network.config_revision ||
        g_render_active_profile != g_snapshot.network.active_profile_id ||
        g_render_publication_stamp != publication_stamp() ||
        g_render_list != g_list || g_render_page != g_list_page;
    update_status();
    if (rows_changed) render_rows();
    if (g_details_open) refresh_details();
    if (g_view == WIFI_VIEW_DISPLAY_WEB) draw_display_view();
}

static bool profile_is_current(const wifi_profile_summary_t *profile)
{
    return profile && profile->id != 0U &&
           profile->id == g_snapshot.network.active_profile_id &&
           g_snapshot.network.phase == WIFI_PHASE_ONLINE;
}

static bool selected_network_online(void)
{
    if (!g_snapshot_valid || g_snapshot.network.phase != WIFI_PHASE_ONLINE) return false;
    if (!g_selected_ap_valid) return profile_is_current(profile_by_id(g_selected_profile_id));
    const wifi_profile_summary_t *current = profile_by_id(g_snapshot.network.active_profile_id);
    if (profile_matches_ap(current, &g_selected_ap)) return true;
    /* Temporary APs have no saved ID. The owned operation retains their
     * normalized security identity after the credential bytes are cleared. */
    return g_pending.operation == g_snapshot.network.connection_operation_id &&
        same_network(&g_selected_ap, &g_pending.target_ap);
}

static void refresh_details(void)
{
    if (!scr_wifi_is_ready()) return;
    char name[WIFI_UI_TEXT_MAX], meta[WIFI_UI_TEXT_MAX], note[WIFI_UI_TEXT_MAX];
    if (!g_details_open) {
        lv_obj_set_hidden(g_details, true);
        return;
    }
    lv_obj_set_hidden(g_details, false);
    bool feedback_matches = !g_feedback_scoped || (g_selected_ap_valid ?
        same_network(&g_selected_ap, &g_feedback_target) :
        profile_matches_ap(profile_by_id(g_selected_profile_id), &g_feedback_target));
    lv_obj_set_hidden(g_detail_feedback, !feedback_matches);
    set_label_safe(g_detail_feedback, g_feedback);
    if (g_selected_ap_valid) {
        const wifi_ap_record_t *ap = &g_selected_ap;
        char bssid[24];
        wifi_security_t security = ap_security(ap);
        ap_name(ap, name, sizeof(name));
        format_bssid(ap->bssid, bssid, sizeof(bssid));
        (void)snprintf(meta, sizeof(meta), "%s  ·  %d dBm  ·  信道 %d  ·  %s",
            security_text(security), (int)ap->rssi, (int)ap->channel, bssid);
        if (ap_ssid_length(ap) == 0U)
            (void)snprintf(note, sizeof(note), "隐藏 SSID 暂不支持连接或手动添加。");
        else if (ap->ssid_length > WIFI_SSID_BYTES_MAX ||
                 (ap->flags & WIFI_AP_FLAG_SSID_TRUNCATED))
            (void)snprintf(note, sizeof(note), "网络名称被截断，无法确保连接到原始网络身份。");
        else if (!ap_security_supported(ap))
            (void)snprintf(note, sizeof(note), "该安全类型不受当前无线连接器支持；可以查看但不能连接。");
        else
            (void)snprintf(note, sizeof(note), "扫描记录保持原始 SSID、安全类型与 BSSID。");
        set_label_safe(g_detail_title, "附近网络详情");
        set_label_safe(g_detail_name, name);
        set_label_safe(g_detail_meta, meta);
        set_label_safe(g_detail_note, note);
        lv_obj_set_hidden(g_password_caption, security == WIFI_SECURITY_OPEN);
        set_label(g_password_caption, "密码");
        lv_obj_set_hidden(g_pwd_area, security == WIFI_SECURITY_OPEN);
        lv_obj_set_hidden(g_remember_button, false);
        set_button_label(g_remember_button, g_remember_password ?
                         "保存网络：开" : "保存网络：关（临时）");
        lv_obj_set_hidden(g_detail_connect, false);
        lv_obj_set_hidden(g_detail_disconnect, true);
        lv_obj_set_hidden(g_detail_update, true);
        lv_obj_set_hidden(g_detail_auto, true);
        lv_obj_set_hidden(g_detail_forget, true);
        set_button_enabled(g_detail_connect, ap_connectable(ap));
        set_button_label(g_detail_connect, profile_matching_ap(ap)?"连接已保存":"连接此网络");
    } else {
        const wifi_profile_summary_t *profile = profile_by_id(g_selected_profile_id);
        if (!profile) {
            g_details_open = false;
            lv_obj_set_hidden(g_details, true);
            return;
        }
        profile_name(profile, name, sizeof(name));
        (void)snprintf(meta, sizeof(meta), "%s  ·  配置 ID %lu  ·  最近成功 %lu",
            security_text(profile->security), (unsigned long)profile->id,
            (unsigned long)profile->last_success);
        if (profile->ssid_length == 0U)
            (void)snprintf(note, sizeof(note), "此配置没有可用名称；不支持隐藏 SSID 连接。可忘记此配置。");
        else if (profile->security == WIFI_SECURITY_UNSUPPORTED)
            (void)snprintf(note, sizeof(note), "此配置的安全类型不受当前无线连接器支持。");
        else
            (void)snprintf(note, sizeof(note), "已保存网络摘要不含密码；密码只由 WiFi 服务管理。");
        set_label_safe(g_detail_title, "已保存网络详情");
        set_label_safe(g_detail_name, name);
        set_label_safe(g_detail_meta, meta);
        set_label_safe(g_detail_note, note);
        lv_obj_set_hidden(g_password_caption, profile->security==WIFI_SECURITY_OPEN);
        lv_obj_set_hidden(g_pwd_area, profile->security==WIFI_SECURITY_OPEN);
        set_label(g_password_caption, "新密码");
        lv_obj_set_hidden(g_remember_button, true);
        lv_obj_set_hidden(g_detail_connect, false);
        set_button_enabled(g_detail_connect,
            profile->ssid_length != 0U &&
            profile->security != WIFI_SECURITY_UNSUPPORTED);
        set_button_label(g_detail_connect, "连接");
        lv_obj_set_hidden(g_detail_disconnect,
            !profile_is_current(profile));
        lv_obj_set_hidden(g_detail_update,
            profile->security == WIFI_SECURITY_OPEN ||
            profile->security == WIFI_SECURITY_UNSUPPORTED);
        set_button_label(g_detail_update, g_update_password_armed ?
                         "确认更新" : "更新密码");
        lv_obj_set_hidden(g_detail_auto, false);
        set_button_label(g_detail_auto, profile->auto_connect ?
                         "此网络自动：开" : "此网络自动：关");
        lv_obj_set_hidden(g_detail_forget, false);
    }
    if (!g_snapshot.network.radio_ready) {
        set_button_label(g_detail_connect,g_snapshot.network.radio_init_state==WIFI_RADIO_INIT_FAILED?
            "无线不可用":"正在初始化");
        set_button_enabled(g_detail_connect,false);
    } else if (selected_network_online()) {
        set_button_label(g_detail_connect, "已连接");
        set_button_enabled(g_detail_connect, false);
    } else if (connection_transition()) {
        set_button_label(g_detail_connect, "处理中");
        set_button_enabled(g_detail_connect, false);
    }
    bool temporary_option=g_snapshot.network.phase==WIFI_PHASE_ONLINE &&
        !g_snapshot.network.temporary&&!g_snapshot.network.cleanup_pending &&
        (g_snapshot.network.storage==WIFI_STORAGE_FAILED||g_snapshot.network.storage==WIFI_STORAGE_FOREIGN||
         g_snapshot.network.storage==WIFI_STORAGE_CORRUPT);
    lv_obj_set_hidden(g_detail_temporary,!temporary_option);
    if(temporary_option)lv_obj_set_hidden(g_detail_disconnect,true);
    if (g_password_context == WIFI_PASSWORD_UPDATE && g_editor_open)
        set_label_safe(g_password_network, name);
}

static void begin_pending(wifi_pending_kind_t kind, rt_uint32_t operation,
                          rt_uint32_t profile_id, bool remember)
{
    wifi_secure_zero(&g_pending, sizeof(g_pending));
    wifi_secure_zero(&g_recovery, sizeof(g_recovery));
    g_pending.kind = kind;
    g_pending.operation = operation;
    g_pending.baseline_operation = g_snapshot_valid ?
        g_snapshot.network.operation_id : 0U;
    g_pending.profile_id = profile_id;
    g_pending.baseline_revision = g_snapshot_valid ?
        g_snapshot.network.config_revision : 0U;
    g_pending.remember = kind == WIFI_PENDING_CONNECT_SAVED ? true : remember;
    g_pending.valid = true;
    g_pending.draft_revision = g_draft_revision;
    if (g_selected_ap_valid) {
        g_pending.target_ap = g_selected_ap;
        g_pending.has_ap = true;
    } else {
        const wifi_profile_summary_t *profile = profile_by_id(profile_id ? profile_id :
            (kind == WIFI_PENDING_TEMP ? g_snapshot.network.active_profile_id : 0U));
        if (profile) {
            g_pending.target_ap.ssid_length = profile->ssid_length;
            memcpy(g_pending.target_ap.ssid, profile->ssid, profile->ssid_length);
            g_pending.target_ap.security_mode = profile->security;
        }
    }
    if (kind == WIFI_PENDING_CONNECT_AP || kind == WIFI_PENDING_UPDATE_PASSWORD)
        wifi_copy_text(g_pending.password, sizeof(g_pending.password), password_text());
}

static bool operation_matches_pending(void)
{
    uint32_t operation=(g_pending.kind==WIFI_PENDING_FORGET||g_pending.kind==WIFI_PENDING_FORGET_ALL)?
        g_snapshot.network.config_operation_id:g_snapshot.network.connection_operation_id;
    bool configuration=g_pending.kind==WIFI_PENDING_FORGET||g_pending.kind==WIFI_PENDING_FORGET_ALL;
    return g_pending.operation!=0U && (configuration?
        (int32_t)(operation-g_pending.operation)>=0:operation==g_pending.operation);
}

static void pending_fail(const char *message)
{
    if ((is_connection_pending(g_pending.kind) || g_pending.kind == WIFI_PENDING_UPDATE_PASSWORD) &&
        g_snapshot.network.reason != WIFI_REASON_CANCELLED) {
        g_recovery = g_pending;
        g_recovery.valid = true;
    }
    wifi_secure_zero(g_pending.password, sizeof(g_pending.password));
    set_pending_feedback(message);
    g_pending.valid = false;
    g_pending.kind = WIFI_PENDING_NONE;
}

static void clear_password_draft(void)
{
    if (!g_pwd_area) return;
    lv_textarea_set_text(g_pwd_area, "");
    memset(g_password_before, 0, sizeof(g_password_before));
    g_update_password_armed = false;
}

static void finish_pending_password(void)
{
    /* A delayed result must not erase a freshly edited draft, even for A. */
    if (g_pending.kind != WIFI_PENDING_CONNECT_SAVED &&
        g_draft_revision == g_pending.draft_revision &&
        g_draft_ssid_length == ap_ssid_length(&g_pending.target_ap) &&
        g_draft_security == ap_security(&g_pending.target_ap) &&
        !memcmp(g_draft_ssid, g_pending.target_ap.ssid, g_draft_ssid_length))
        clear_password_draft();
    wifi_secure_zero(g_pending.password, sizeof(g_pending.password));
    g_pending.valid = false;
}

static void forget_local_drafts(const wifi_ap_record_t *target)
{
    if (!target || (g_draft_ssid_length == ap_ssid_length(target) &&
        g_draft_security == ap_security(target) &&
        !memcmp(g_draft_ssid, target->ssid, g_draft_ssid_length)))
        clear_password_draft();
    if (!target || same_network(&g_parked_draft.target_ap, target))
        wifi_secure_zero(&g_parked_draft, sizeof(g_parked_draft));
}

static void process_pending(void)
{
    bool revised;
    if (!g_pending.valid || !g_snapshot_valid)
        return;
    if (g_pending.kind == WIFI_PENDING_CONNECT_SAVED &&
        g_snapshot.network.request_result_id == g_pending.operation &&
        g_snapshot.network.request_result == WIFI_CORE_ALREADY_CONNECTED &&
        g_snapshot.network.phase == WIFI_PHASE_ONLINE &&
        g_snapshot.network.active_profile_id == g_pending.profile_id) {
        finish_pending_password();set_pending_feedback("此网络已连接");return;
    }
    if(g_snapshot.network.request_result_id==g_pending.operation && g_snapshot.network.request_result < 0) {
        pending_fail(reason_text(g_snapshot.network.reason));return;
    }
    if(!operation_matches_pending()) {
        if (is_connection_pending(g_pending.kind) &&
            (int32_t)(g_snapshot.network.connection_operation_id-g_pending.operation)>0) {
            wifi_secure_zero(g_pending.password, sizeof(g_pending.password));
            g_pending.valid=false;
            g_pending.kind=WIFI_PENDING_NONE;
        }
        return;
    }
    revised = (int32_t)(g_snapshot.network.config_revision-g_pending.baseline_revision)>0;
    if(g_pending.kind==WIFI_PENDING_TEMP && g_snapshot.network.temporary) {
        finish_pending_password();
        set_pending_feedback("临时连接已由服务确认；配置保存状态见上方");
        g_pending.valid=false;return;
    }
    if((is_connection_pending(g_pending.kind)||g_pending.kind==WIFI_PENDING_UPDATE_PASSWORD)&&
       (g_snapshot.network.phase==WIFI_PHASE_FAILED||g_snapshot.network.phase==WIFI_PHASE_DISCONNECTED)) {
        pending_fail(reason_text(g_snapshot.network.reason));return;
    }
    if (g_snapshot.network.storage == WIFI_STORAGE_FAILED &&
        (g_pending.kind == WIFI_PENDING_UPDATE_PASSWORD ||
         g_pending.kind == WIFI_PENDING_FORGET ||
         g_pending.kind == WIFI_PENDING_FORGET_ALL ||
         (g_pending.kind == WIFI_PENDING_CONNECT_AP && g_pending.remember))) {
        if (g_pending.kind == WIFI_PENDING_CONNECT_AP ||
            g_pending.kind == WIFI_PENDING_UPDATE_PASSWORD)
            set_pending_feedback("保存失败；密码草稿保留，可重试保存");
        else set_feedback("配置保存失败；可重试保存");
        return;
    }
    if (g_pending.kind == WIFI_PENDING_CONNECT_AP) {
        if (g_snapshot.network.phase == WIFI_PHASE_FAILED) {
            pending_fail(reason_text(g_snapshot.network.reason));
            return;
        }
        if (g_snapshot.network.phase != WIFI_PHASE_ONLINE) return;
        if (!g_pending.remember && g_snapshot.network.temporary) {
            finish_pending_password();
            set_pending_feedback("临时连接已由服务确认，不保存密码");
            g_pending.valid = false;
            return;
        }
        if (g_pending.remember &&
            g_snapshot.network.active_profile_id != 0U &&
            g_snapshot.network.saved_operation_id==g_pending.operation &&
            g_snapshot.network.storage == WIFI_STORAGE_OK) {
            const wifi_profile_summary_t *profile =
                profile_by_id(g_snapshot.network.active_profile_id);
            if (profile && profile_matches_ap(profile, &g_pending.target_ap)) {
                finish_pending_password();
                set_pending_feedback("连接和已保存配置均由服务确认");
                g_pending.valid = false;
            }
        }
        return;
    }
    if (g_pending.kind == WIFI_PENDING_CONNECT_SAVED) {
        if (g_snapshot.network.phase == WIFI_PHASE_ONLINE &&
            g_snapshot.network.active_profile_id == g_pending.profile_id &&
            g_snapshot.network.saved_operation_id == g_pending.operation &&
            g_snapshot.network.storage == WIFI_STORAGE_OK) {
            finish_pending_password();
            set_pending_feedback("连接和已保存配置均由服务确认");
        }
        return;
    }
    if (g_pending.kind == WIFI_PENDING_UPDATE_PASSWORD) {
        if (g_snapshot.network.phase==WIFI_PHASE_ONLINE &&
            g_snapshot.network.active_profile_id==g_pending.profile_id &&
            g_snapshot.network.saved_operation_id==g_pending.operation &&
            g_snapshot.network.storage == WIFI_STORAGE_OK) {
            finish_pending_password();
            set_pending_feedback("密码更新已由服务保存确认");
            g_pending.valid = false;
        }
        return;
    }
    if (g_pending.kind == WIFI_PENDING_FORGET) {
        if (!profile_by_id(g_pending.profile_id) && revised &&
            (int32_t)(g_snapshot.network.config_saved_operation_id-g_pending.operation)>=0 &&
            g_snapshot.network.storage == WIFI_STORAGE_OK) {
            forget_local_drafts(&g_pending.target_ap);
            set_feedback("网络已从服务配置中删除并保存");
            g_pending.valid = false;
        }
        return;
    }
    if (g_pending.kind == WIFI_PENDING_FORGET_ALL) {
        if (g_snapshot.network.profile_count == 0U && revised &&
            (int32_t)(g_snapshot.network.config_saved_operation_id-g_pending.operation)>=0 &&
            g_snapshot.network.storage == WIFI_STORAGE_OK) {
            forget_local_drafts(NULL);
            set_feedback("WiFi 配置清除已由服务保存确认");
            g_pending.valid = false;
        }
        return;
    }
    if (g_pending.kind == WIFI_PENDING_DISCONNECT &&
        (g_snapshot.network.phase == WIFI_PHASE_DISCONNECTED ||
         g_snapshot.network.phase == WIFI_PHASE_IDLE)) {
        set_feedback("断开连接已由服务确认");
        g_pending.valid = false;
    }
}

static void request_result(int result, wifi_pending_kind_t kind,
                           rt_uint32_t operation, rt_uint32_t profile_id,
                           bool remember, const char *accepted)
{
    if (result == RT_EOK) {
        if (kind == WIFI_PENDING_NONE) {
            g_pending.valid = false;
            g_pending.kind = WIFI_PENDING_NONE;
        } else begin_pending(kind, operation, profile_id, remember);
        set_feedback(accepted ? accepted : "请求已提交，等待 WiFi 服务确认");
    } else {
        char message[WIFI_UI_TEXT_MAX];
        (void)snprintf(message, sizeof(message), "请求未提交（%d），请稍后重试", result);
        set_feedback(message);
    }
    update_status();
}

static const char *password_text(void)
{
    return g_pwd_area ? lv_textarea_get_text(g_pwd_area) : "";
}

static void password_count_update(void)
{
    if (g_password_count)
        lv_label_set_text_fmt(g_password_count, "WiFi 密码  %u/63",
            (unsigned)strlen(password_text()));
}

static void password_changed(lv_event_t *event)
{
    if (scr_wifi_is_ready() && event &&
        lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        ++g_draft_revision;
        password_count_update();
    }
}

void scr_wifi_close_editor(void)
{
    if (!scr_wifi_is_ready() || !g_editor_open || !g_pwd_area) return;
    g_editor_open = false;
    g_password_visible = false;
    lv_textarea_set_password_mode(g_pwd_area, true);
    lv_obj_set_style_opa(g_pwd_area, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_hidden(g_input_overlay, true);
    lv_keyboard_set_textarea(g_kb, NULL);
    lv_obj_set_parent(g_pwd_area, g_details);
    lv_obj_set_pos(g_pwd_area, 16, 178);
    lv_obj_set_size(g_pwd_area, 512, WIFI_UI_ROW_HEIGHT);
    g_password_context = WIFI_PASSWORD_NONE;
    for (size_t index = 0U; index < sizeof(g_password_before); ++index)
        ((volatile char *)g_password_before)[index] = 0;
}

static void password_action_commit(uintptr_t action)
{
    wifi_password_context_t context;
    if (!scr_wifi_is_ready()) return;
    if (!g_editor_open || !g_page || lv_obj_is_hidden(g_page)) return;
    if (action == PWD_CANCEL) {
        lv_textarea_set_text(g_pwd_area, g_password_before);
        g_update_password_armed = false;
        scr_wifi_close_editor();
    } else if (action == PWD_DONE) {
        context = g_password_context;
        scr_wifi_close_editor();
        if (context == WIFI_PASSWORD_UPDATE) g_update_password_armed = true;
        refresh_details();
    } else if (action == PWD_SHOW) {
        g_password_visible = !g_password_visible;
        lv_textarea_set_password_mode(g_pwd_area, !g_password_visible);
        set_button_label(g_password_show, g_password_visible ? "隐藏" : "显示");
    } else if (action == PWD_CLEAR) lv_textarea_set_text(g_pwd_area, "");
    else if (action == PWD_LEFT) lv_textarea_cursor_left(g_pwd_area);
    else if (action == PWD_RIGHT) lv_textarea_cursor_right(g_pwd_area);
    else if (action == PWD_DELETE) lv_textarea_delete_char(g_pwd_area);
    else if (action == PWD_LOWER) lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    else if (action == PWD_UPPER) lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    else if (action == PWD_SPECIAL) lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_SPECIAL);
    else if (action == PWD_MORE) lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_USER_1);
    else if (action >= 32U && action <= 126U)
        lv_textarea_add_char(g_pwd_area, (uint32_t)action);
    password_count_update();
}

static void password_button_cb(lv_event_t *event)
{
    if (click_release(event))
        (void)ui_deferred_post(password_action_commit,
            (uintptr_t)lv_event_get_user_data(event));
}

static void password_changed_cb(lv_event_t *event)
{
    password_changed(event);
}

static void kb_close_cb(lv_event_t *event)
{
    if (!scr_wifi_is_ready()) return;
    (void)ui_deferred_post(password_action_commit,
        event && lv_event_get_code(event) == LV_EVENT_CANCEL ?
        PWD_CANCEL : PWD_DONE);
}

static void kb_key_cb(lv_event_t *event)
{
    const char *key;
    uintptr_t action = 0U;
    if (!scr_wifi_is_ready() || !event ||
        lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    key = lv_keyboard_get_button_text(g_kb,
        lv_keyboard_get_selected_button(g_kb));
    if (!key) return;
    if (strcmp(key, "Cancel") == 0) action = PWD_CANCEL;
    else if (strcmp(key, "Done") == 0) action = PWD_DONE;
    else if (strcmp(key, "Del") == 0) action = PWD_DELETE;
    else if (strcmp(key, "ABC") == 0) action = PWD_UPPER;
    else if (strcmp(key, "abc") == 0) action = PWD_LOWER;
    else if (strcmp(key, "123") == 0) action = PWD_SPECIAL;
    else if (strcmp(key, "More") == 0) action = PWD_MORE;
    else if (strcmp(key, "Space") == 0) action = ' ';
    else if (key[0] && !key[1]) action = (unsigned char)key[0];
    if (action) (void)ui_deferred_post(password_action_commit, action);
}

static void pwd_click_commit(uintptr_t argument)
{
    char name[WIFI_UI_TEXT_MAX];
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    if (g_editor_open || !g_pwd_area) return;
    (void)snprintf(g_password_before, sizeof(g_password_before), "%s",
                   password_text());
    g_editor_open = true;
    g_password_visible = false;
    lv_textarea_set_password_mode(g_pwd_area, true);
    set_button_label(g_password_show, "显示");
    if (g_selected_ap_valid) ap_name(&g_selected_ap, name, sizeof(name));
    else {
        const wifi_profile_summary_t *profile = profile_by_id(g_selected_profile_id);
        if (profile) profile_name(profile, name, sizeof(name));
        else (void)snprintf(name, sizeof(name), "请先选择网络");
    }
    set_label_safe(g_password_network, name);
    lv_obj_set_parent(g_pwd_area, g_input_overlay);
    lv_obj_set_pos(g_pwd_area, 12, 54);
    lv_obj_set_size(g_pwd_area, 512, WIFI_UI_ROW_HEIGHT);
    lv_obj_set_hidden(g_input_overlay, false);
    lv_obj_set_style_opa(g_pwd_area, LV_OPA_COVER, LV_PART_CURSOR);
    lv_keyboard_set_textarea(g_kb, g_pwd_area);
    lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_textarea_set_cursor_pos(g_pwd_area, LV_TEXTAREA_CURSOR_LAST);
    password_count_update();
}

static void pwd_click_cb(lv_event_t *event)
{
    if (click_release(event))
        (void)ui_deferred_post(pwd_click_commit, 0U);
}

static void close_details_commit(uintptr_t argument)
{
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    if (g_editor_open) scr_wifi_close_editor();
    g_details_open = false;
    g_selected_ap_valid = false;
    g_update_password_armed = false;
    lv_obj_set_hidden(g_details, true);
    render_rows();
}

static void return_to_list_after_submit(void)
{
    /* No tab switch, scroll reset or snapshot refresh: keep the user's place. */
    if (!scr_wifi_is_ready()) return;
    if (g_editor_open) scr_wifi_close_editor();
    g_details_open = false;
    g_selected_ap_valid = false;
    g_update_password_armed = false;
    lv_obj_set_hidden(g_details, true);
    render_rows();
    update_status();
}

static void bind_password_draft(const uint8_t *ssid,size_t length,wifi_security_t security)
{
    if (!scr_wifi_is_ready()) return;
    if(length>WIFI_SSID_BYTES_MAX)length=0;
    if(length!=g_draft_ssid_length || security!=g_draft_security ||
       memcmp(ssid,g_draft_ssid,length)) {
        lv_textarea_set_text(g_pwd_area,"");
        wifi_secure_zero(g_password_before,sizeof(g_password_before));
    }
    wifi_secure_zero(g_draft_ssid,sizeof(g_draft_ssid));
    if(length)memcpy(g_draft_ssid,ssid,length);
    g_draft_ssid_length=(uint8_t)length;g_draft_security=security;
    if (g_parked_draft.valid && length == ap_ssid_length(&g_parked_draft.target_ap) &&
        security == ap_security(&g_parked_draft.target_ap) &&
        !memcmp(ssid, g_parked_draft.target_ap.ssid, length)) {
        lv_textarea_set_text(g_pwd_area, g_parked_draft.password);
        wifi_secure_zero(&g_parked_draft, sizeof(g_parked_draft));
    }
}

static void open_connection_recovery(void)
{
    if (!scr_wifi_is_ready()) return;
    if (!g_recovery.valid) return;
    if (g_editor_open) scr_wifi_close_editor();
    if (!g_recovery.has_ap && !profile_by_id(g_recovery.profile_id)) {
        wifi_secure_zero(&g_recovery, sizeof(g_recovery));
        set_feedback("网络配置已移除，请重新选择网络");
        update_status();
        return;
    }
    bool same_draft = g_draft_ssid_length == ap_ssid_length(&g_recovery.target_ap) &&
        g_draft_security == ap_security(&g_recovery.target_ap) &&
        !memcmp(g_draft_ssid, g_recovery.target_ap.ssid, g_draft_ssid_length);
    if (!same_draft && g_draft_ssid_length && password_text()[0]) {
        wifi_secure_zero(&g_parked_draft, sizeof(g_parked_draft));
        g_parked_draft.target_ap.ssid_length = g_draft_ssid_length;
        memcpy(g_parked_draft.target_ap.ssid, g_draft_ssid, g_draft_ssid_length);
        g_parked_draft.target_ap.security_mode = g_draft_security;
        const wifi_profile_summary_t *parked_profile = profile_matching_ap(&g_parked_draft.target_ap);
        g_parked_draft.profile_id = parked_profile ? parked_profile->id : 0U;
        wifi_copy_text(g_parked_draft.password, sizeof(g_parked_draft.password), password_text());
        g_parked_draft.valid = true;
    }
    bind_password_draft((const uint8_t *)g_recovery.target_ap.ssid,
        ap_ssid_length(&g_recovery.target_ap), ap_security(&g_recovery.target_ap));
    if (!same_draft || !password_text()[0])
        lv_textarea_set_text(g_pwd_area, g_recovery.password);
    lv_textarea_set_password_mode(g_pwd_area, true);
    g_password_visible = false;
    g_selected_ap = g_recovery.target_ap;
    g_selected_ap_valid = g_recovery.has_ap;
    g_selected_profile_id = g_recovery.profile_id;
    g_remember_password = g_recovery.remember;
    g_update_password_armed = false;
    g_details_open = true;
    refresh_details();
}

static void details_back_cb(lv_event_t *event)
{
    if (click_release(event))
        (void)ui_deferred_post(close_details_commit, 0U);
}

static void row_select_commit(uintptr_t encoded)
{
    if (!scr_wifi_is_ready()) return;
    unsigned slot = (unsigned)(encoded & 31U);
    uint32_t epoch = (uint32_t)((encoded >> 5U) & WIFI_UI_EPOCH_MASK);
    if (slot >= WIFI_UI_ROWS || epoch != (g_render_epoch & WIFI_UI_EPOCH_MASK) ||
        !g_snapshot_valid || g_row_token[slot] == UINT32_MAX ||
        lv_obj_is_hidden(g_rows[slot])) return;
    g_details_open = true;
    g_update_password_armed = false;
    if (g_row_kind[slot] == WIFI_LIST_NEARBY) {
        unsigned index = g_row_token[slot];
        if (index >= g_snapshot.scan_count || index >= WIFI_SCAN_MAX_APS) return;
        const wifi_ap_record_t *ap=&g_snapshot.access_points[index];
        bind_password_draft((const uint8_t*)ap->ssid,ap_ssid_length(ap),ap_security(ap));
        memcpy(&g_selected_ap, &g_snapshot.access_points[index], sizeof(g_selected_ap));
        g_selected_ap_valid = true;
        g_selected_profile_id = 0U;
        g_password_context = WIFI_PASSWORD_NONE;
    } else {
        g_selected_profile_id = g_row_token[slot];
        g_selected_ap_valid = false;
        if (!profile_by_id(g_selected_profile_id)) {
            g_details_open = false;
            return;
        }
        const wifi_profile_summary_t *profile=profile_by_id(g_selected_profile_id);
        bind_password_draft((const uint8_t*)profile->ssid,profile->ssid_length,profile->security);
    }
    refresh_details();
}

static void row_select_cb(lv_event_t *event)
{
    uintptr_t slot, encoded;
    if (!click_release(event)) return;
    slot = (uintptr_t)lv_event_get_user_data(event);
    if (slot >= WIFI_UI_ROWS) return;
    encoded = ((uintptr_t)(g_render_epoch & WIFI_UI_EPOCH_MASK) << 5U) | slot;
    (void)ui_deferred_post(row_select_commit, encoded);
}

static void set_list_commit(uintptr_t list)
{
    if (!scr_wifi_is_ready()) return;
    if (g_editor_open) scr_wifi_close_editor();
    g_list = list ? WIFI_LIST_SAVED : WIFI_LIST_NEARBY;
    g_list_page = 0U;
    g_details_open = false;
    g_selected_ap_valid = false;
    g_update_password_armed = false;
    lv_obj_set_hidden(g_details, true);
    render_rows();
    lv_obj_scroll_to_y(g_list_panel,0,LV_ANIM_OFF);
}

static void nearby_tab_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(set_list_commit, 0U);
}

static void saved_tab_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(set_list_commit, 1U);
}

static void page_commit(uintptr_t direction)
{
    if (!scr_wifi_is_ready()) return;
    unsigned count = list_count();
    unsigned page_count = (count + WIFI_UI_PAGE_ROWS - 1U) / WIFI_UI_PAGE_ROWS;
    if (direction && (unsigned)g_list_page + 1U < page_count) ++g_list_page;
    else if (!direction && g_list_page > 0U) --g_list_page;
    render_rows();
    lv_obj_update_layout(g_list_panel);
    lv_obj_scroll_to_y(g_list_panel,g_list_page*WIFI_UI_PAGE_ROWS*WIFI_UI_ROW_HEIGHT,LV_ANIM_OFF);
}

static void page_prev_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(page_commit, 0U);
}

static void page_next_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(page_commit, 1U);
}

static void scan_commit(uintptr_t argument)
{
    int result;
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    if (connection_transition() || g_snapshot.scan_status == WIFI_SCAN_STATUS_SCANNING) {
        set_feedback("操作进行中，请稍后扫描");update_status();return;
    }
    result = wifi_request_scan();
    if (result == RT_EOK) set_feedback("扫描请求已提交");
    else {
        char message[WIFI_UI_TEXT_MAX];
        (void)snprintf(message, sizeof(message), "扫描未提交（%d）", result);
        set_feedback(message);
    }
    update_status();
}

static void scan_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(scan_commit, 0U);
}

static void global_auto_commit(uintptr_t argument)
{
    rt_uint32_t operation = 0U;
    rt_bool_t enabled;
    int result;
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    if (!g_snapshot_valid) return;
    enabled = g_snapshot.network.auto_connect ? RT_FALSE : RT_TRUE;
    result = wifi_request_set_autoconnect(enabled, &operation);
    if (result == RT_EOK) set_feedback("全局自动连接设置等待服务确认");
    else {
        char message[WIFI_UI_TEXT_MAX];
        (void)snprintf(message, sizeof(message), "自动连接设置未提交（%d）", result);
        set_feedback(message);
    }
    update_status();
}

static void global_auto_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(global_auto_commit, 0U);
}

static void remember_commit(uintptr_t argument)
{
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    g_remember_password = !g_remember_password;
    refresh_details();
}

static void remember_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(remember_commit, 0U);
}

static void connect_commit(uintptr_t argument)
{
    rt_uint32_t operation = 0U;
    int result;
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    if (!g_snapshot_valid || !g_details_open) return;
    if(!g_snapshot.network.radio_ready){set_feedback("正在初始化，请稍后连接");update_status();return;}
    if (selected_network_online()) {
        set_feedback("此网络已连接，请先断开");update_status();refresh_details();return;
    }
    if (connection_transition()) {
        set_feedback("连接操作进行中，请先取消或等待");update_status();refresh_details();return;
    }
    if (g_update_password_armed) {
        set_feedback("请先确认或取消密码更新");
        return;
    }
    if (g_selected_ap_valid) {
        const wifi_profile_summary_t *existing;
        if (!ap_connectable(&g_selected_ap)) {
            set_feedback("该扫描记录不支持连接");
            return;
        }
        existing = profile_matching_ap(&g_selected_ap);
        if(existing && g_remember_password && !password_text()[0]) {
            result=wifi_request_connect_saved(existing->id,&operation);
            request_result(result,WIFI_PENDING_CONNECT_SAVED,operation,existing->id,false,
                "已请求连接保存的网络");
            if (result == RT_EOK) return_to_list_after_submit();
            update_status();return;
        }
        if (g_remember_password && !existing &&
            g_snapshot.network.profile_count >= WIFI_PROFILE_MAX) {
            set_feedback("已保存 8 个网络；请先删除一个，或关闭记住密码以临时连接");
            return;
        }
        if (ap_security(&g_selected_ap) != WIFI_SECURITY_OPEN &&
            password_text()[0] == '\0') {
            g_password_context = WIFI_PASSWORD_CONNECT;
            pwd_click_commit(0U);
            return;
        }
        if (ap_security(&g_selected_ap) != WIFI_SECURITY_OPEN) {
            size_t length = strlen(password_text());
            bool valid = length >= 8U && length <= WIFI_PASSWORD_BYTES_MAX;
            for (size_t index=0U; index<length; ++index)
                if ((uint8_t)password_text()[index]<32U || (uint8_t)password_text()[index]>126U) valid=false;
            if (!valid) {
                set_feedback("密码应为 8-63 个 ASCII 字符");
                update_status();refresh_details();return;
            }
        }
        result = wifi_request_connect_network(&g_selected_ap,
            ap_security(&g_selected_ap) == WIFI_SECURITY_OPEN ? "" : password_text(),
            g_remember_password ? RT_TRUE : RT_FALSE, &operation);
        if (result == RT_EOK) {
            begin_pending(WIFI_PENDING_CONNECT_AP, operation, 0U,
                          g_remember_password);
            set_feedback("连接请求已提交；密码会保留到服务确认");
            return_to_list_after_submit();
        } else {
            char message[WIFI_UI_TEXT_MAX];
            (void)snprintf(message, sizeof(message), "连接未提交（%d）；密码草稿保留", result);
            set_feedback(message);
        }
    } else {
        const wifi_profile_summary_t *profile = profile_by_id(g_selected_profile_id);
        if (!profile) return;
        if (profile->ssid_length == 0U ||
            profile->security == WIFI_SECURITY_UNSUPPORTED) {
            set_feedback("此保存配置不支持连接");
            return;
        }
        result = wifi_request_connect_saved(profile->id, &operation);
        request_result(result, WIFI_PENDING_CONNECT_SAVED, operation, profile->id, false,
                       "已请求连接保存的网络");
        if (result == RT_EOK) return_to_list_after_submit();
    }
    update_status();
}

static void connect_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(connect_commit, 0U);
}

static void show_confirm(wifi_confirm_action_t action, const char *title,
                         const char *body, const char *yes)
{
    if (!scr_wifi_is_ready()) return;
    g_confirm_action = action;
    set_label(g_confirm_title, title);
    set_label_safe(g_confirm_body, body);
    set_button_label(g_confirm_yes, yes);
    lv_obj_set_hidden(g_confirm_overlay, false);
}

static void disconnect_begin_commit(uintptr_t argument)
{
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    if (g_snapshot.network.phase == WIFI_PHASE_ONLINE && !connection_request_waiting())
        show_confirm(WIFI_CONFIRM_DISCONNECT, "断开 WiFi？",
            "设备将断开当前无线网络；已保存的网络配置不会被删除。", "确认断开");
    else
        show_confirm(WIFI_CONFIRM_DISCONNECT, "取消连接？",
            "设备将取消当前连接或重试；已保存的网络配置不会被删除。", "取消连接");
}

static void disconnect_begin_cb(lv_event_t *event)
{
    if (click_release(event))
        (void)ui_deferred_post(disconnect_begin_commit, 0U);
}

static void update_password_begin_commit(uintptr_t argument)
{
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    if (!g_update_password_armed) {
        g_password_context = WIFI_PASSWORD_UPDATE;
        pwd_click_commit(0U);
        return;
    }
    if (!password_text()[0]) {
        set_feedback("请输入新密码");
        return;
    }
    show_confirm(WIFI_CONFIRM_UPDATE_PASSWORD, "更新此网络密码？",
        "密码将提交给 WiFi 服务；只有服务确认保存后，屏幕才清除草稿。", "确认更新");
}

static void update_password_begin_cb(lv_event_t *event)
{
    if (click_release(event))
        (void)ui_deferred_post(update_password_begin_commit, 0U);
}

static void profile_auto_commit(uintptr_t argument)
{
    const wifi_profile_summary_t *profile;
    rt_uint32_t operation = 0U;
    int result;
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    profile = profile_by_id(g_selected_profile_id);
    if (!profile) return;
    result = wifi_request_set_profile_autoconnect(profile->id,
        profile->auto_connect ? RT_FALSE : RT_TRUE, &operation);
    if (result == RT_EOK) set_feedback("此网络的自动连接设置等待服务确认");
    else {
        char message[WIFI_UI_TEXT_MAX];
        (void)snprintf(message, sizeof(message), "网络自动连接设置未提交（%d）", result);
        set_feedback(message);
    }
    update_status();
}

static void profile_auto_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(profile_auto_commit, 0U);
}

static void forget_begin_commit(uintptr_t argument)
{
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    if (!profile_by_id(g_selected_profile_id)) return;
    show_confirm(WIFI_CONFIRM_FORGET, "忘记此网络？",
        "此操作会从设备 WiFi 配置中移除此网络。", "忘记网络");
}

static void forget_begin_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(forget_begin_commit, 0U);
}

static void clear_all_begin_commit(uintptr_t argument)
{
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    show_confirm(WIFI_CONFIRM_CLEAR_FIRST, "清空全部 WiFi 配置？",
        "将删除设备已保存的 WiFi 网络，不会影响证据、显示设置或其他系统配置。", "继续");
}

static void clear_all_begin_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(clear_all_begin_commit, 0U);
}

static void retry_save_commit(uintptr_t argument)
{
    rt_uint32_t operation = 0U;
    int result;
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    if (g_snapshot.network.storage != WIFI_STORAGE_FAILED && g_recovery.valid) {
        open_connection_recovery();
        return;
    }
    result = wifi_request_retry_save(&operation);
    if (result == RT_EOK) {
        g_save_retry_requested = true;
        g_save_retry_origin=g_snapshot.network.config_operation_id;
        g_save_retry_revision=g_snapshot.network.config_revision;
        set_feedback("保存重试已提交，等待服务确认");
    } else {
        char message[WIFI_UI_TEXT_MAX];
        (void)snprintf(message, sizeof(message), "保存重试未提交（%d）", result);
        set_feedback(message);
    }
    update_status();
}

static void retry_save_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(retry_save_commit, 0U);
}

static void use_temporary_commit(uintptr_t argument)
{
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    rt_uint32_t operation=0;
    int result=wifi_request_use_temporary(&operation);
    request_result(result,WIFI_PENDING_TEMP,operation,0,false,
        "临时连接请求已提交，等待服务确认");
    update_status();
}
static void use_temporary_cb(lv_event_t *event)
{if(click_release(event))(void)ui_deferred_post(use_temporary_commit,0U);}

static void confirm_cancel_commit(uintptr_t argument)
{
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    g_confirm_action = WIFI_CONFIRM_NONE;
    lv_obj_set_hidden(g_confirm_overlay, true);
}

static void confirm_no_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(confirm_cancel_commit, 0U);
}

static void submit_disconnect(void)
{
    rt_uint32_t operation = 0U;
    int result;
    if (!scr_wifi_is_ready()) return;
    if (g_snapshot.network.phase == WIFI_PHASE_ONLINE && !connection_request_waiting())
        result = wifi_request_disconnect(&operation);
    else result = wifi_request_cancel(&operation);
    request_result(result, WIFI_PENDING_DISCONNECT, operation, 0U, false,
                   "断开请求已提交");
}

static void submit_forget(void)
{
    rt_uint32_t operation = 0U;
    if (!scr_wifi_is_ready()) return;
    int result = wifi_request_forget(g_selected_profile_id, &operation);
    request_result(result, WIFI_PENDING_FORGET, operation,
                   g_selected_profile_id, false, "删除请求已提交，等待配置保存");
}

static void submit_update_password(void)
{
    rt_uint32_t operation = 0U;
    if (!scr_wifi_is_ready()) return;
    int result = wifi_request_update_password(g_selected_profile_id,
                                               password_text(), &operation);
    if (result == RT_EOK) {
        begin_pending(WIFI_PENDING_UPDATE_PASSWORD, operation,
                      g_selected_profile_id, false);
        set_feedback("密码更新已提交；草稿保留到服务确认保存");
        g_update_password_armed = false;
    } else {
        char message[WIFI_UI_TEXT_MAX];
        (void)snprintf(message, sizeof(message), "密码更新未提交（%d）；草稿保留", result);
        set_feedback(message);
    }
}

static void submit_forget_all(void)
{
    rt_uint32_t operation = 0U;
    if (!scr_wifi_is_ready()) return;
    int result = wifi_request_forget_all(&operation);
    request_result(result, WIFI_PENDING_FORGET_ALL, operation, 0U, false,
                   "清除请求已提交，等待配置保存");
}

static void confirm_yes_commit(uintptr_t argument)
{
    if (!scr_wifi_is_ready()) return;
    wifi_confirm_action_t action = g_confirm_action;
    RT_UNUSED(argument);
    if (action == WIFI_CONFIRM_CLEAR_FIRST) {
        show_confirm(WIFI_CONFIRM_CLEAR_FINAL, "再次确认清除",
            "最后确认：只清除本设备保存的 WiFi 网络。此操作不可撤销。", "全部清除");
        return;
    }
    g_confirm_action = WIFI_CONFIRM_NONE;
    lv_obj_set_hidden(g_confirm_overlay, true);
    switch (action) {
    case WIFI_CONFIRM_DISCONNECT: submit_disconnect(); break;
    case WIFI_CONFIRM_FORGET: submit_forget(); break;
    case WIFI_CONFIRM_UPDATE_PASSWORD: submit_update_password(); break;
    case WIFI_CONFIRM_CLEAR_FINAL: submit_forget_all(); break;
    default: break;
    }
    update_status();
    refresh_details();
}

static void confirm_yes_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(confirm_yes_commit, 0U);
}

static void retry_pending_save_state(void)
{
    if (g_save_retry_requested && g_snapshot.network.storage == WIFI_STORAGE_OK &&
        !g_snapshot.network.pending_changes &&
        g_snapshot.network.config_revision==g_save_retry_revision &&
        g_snapshot.network.config_saved_operation_id==g_save_retry_origin) {
        g_save_retry_requested = false;
        if(!g_pending.valid)set_feedback("WiFi 配置保存成功");
    } else if (g_save_retry_requested &&
               (g_snapshot.network.storage == WIFI_STORAGE_CORRUPT ||
                g_snapshot.network.storage == WIFI_STORAGE_FOREIGN)) {
        g_save_retry_requested = false;
        set_feedback("配置格式或来源异常，未覆盖原数据");
    }
}

static void view_commit(uintptr_t view)
{
    if (!scr_wifi_is_ready()) return;
    g_view = view ? WIFI_VIEW_DISPLAY_WEB : WIFI_VIEW_MANAGER;
    if (g_editor_open) scr_wifi_close_editor();
    g_details_open = false;
    g_selected_ap_valid = false;
    g_update_password_armed = false;
    lv_obj_set_hidden(g_details, true);
    lv_obj_set_hidden(g_display_panel, g_view != WIFI_VIEW_DISPLAY_WEB);
    lv_obj_set_hidden(g_status_panel, g_view != WIFI_VIEW_MANAGER);
    lv_obj_set_hidden(g_tab_nearby, g_view != WIFI_VIEW_MANAGER);
    lv_obj_set_hidden(g_tab_saved, g_view != WIFI_VIEW_MANAGER);
    lv_obj_set_hidden(g_page_prev, g_view != WIFI_VIEW_MANAGER);
    lv_obj_set_hidden(g_page_next, g_view != WIFI_VIEW_MANAGER);
    lv_obj_set_hidden(g_page_label, g_view != WIFI_VIEW_MANAGER);
    lv_obj_set_hidden(g_auto_button, g_view != WIFI_VIEW_MANAGER);
    lv_obj_set_hidden(g_scan_button, g_view != WIFI_VIEW_MANAGER);
    lv_obj_set_hidden(g_clear_button, g_view != WIFI_VIEW_MANAGER ||
                      g_list != WIFI_LIST_SAVED);
    lv_obj_set_hidden(g_retry_button, g_view != WIFI_VIEW_MANAGER ||
                      !g_snapshot_valid ||
                      (g_snapshot.network.storage != WIFI_STORAGE_FAILED && !g_recovery.valid));
    lv_obj_set_hidden(g_list_panel, g_view != WIFI_VIEW_MANAGER);
    wifi_button_selected(g_mode_wifi, g_view == WIFI_VIEW_MANAGER);
    wifi_button_selected(g_mode_display, g_view == WIFI_VIEW_DISPLAY_WEB);
    if (g_view == WIFI_VIEW_DISPLAY_WEB) draw_display_view();
}

static void view_wifi_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(view_commit, 0U);
}

static void view_display_cb(lv_event_t *event)
{
    if (click_release(event)) (void)ui_deferred_post(view_commit, 1U);
}

static void motion_refresh_commit(uintptr_t argument)
{
    if (!scr_wifi_is_ready()) return;
    RT_UNUSED(argument);
    ui_model_post(UI_ACTION_REDUCED_MOTION, 0);
}

static void motion_button_cb(lv_event_t *event)
{
    /* The shared UI model owns the actual reduced-motion toggle. */
    if (click_release(event))
        (void)ui_deferred_post(motion_refresh_commit, 0U);
}

static void update_poll_tick(void)
{
    if (!scr_wifi_is_ready()) return;
    rt_tick_t now = rt_tick_get();
    if (g_poll_started && now - g_last_poll_tick <
        rt_tick_from_millisecond(WIFI_UI_POLL_MS)) return;
    g_poll_started = RT_TRUE;
    g_last_poll_tick = now;
    int snapshot_result=wifi_get_snapshot(&g_snapshot);
    if (snapshot_result != RT_EOK) {
        /* A zero-timeout publication-lock miss is not a radio failure. Keep
         * the last good view briefly, but do not present an indefinitely
         * stale connection state as current. No UI-owner blocking wait. */
        if(snapshot_result == -RT_EBUSY) {
            if(g_snapshot_valid && now-g_snapshot_observed_tick <
                rt_tick_from_millisecond(1000))return;
            g_snapshot_valid=false;
            set_label(g_phase_label,"状态更新中");
            set_label(g_meta_label,"WiFi 服务状态暂不可用");
            lv_obj_set_hidden(g_status_disconnect,true);
            return;
        }
        g_snapshot_valid=false;g_snapshot_poll_error=true;
        set_feedback("WiFi 服务状态读取失败");
        update_status();
        return;
    }
    g_snapshot_observed_tick=now;
    if(g_snapshot_poll_error){g_snapshot_poll_error=false;set_feedback("");}
    if (g_snapshot.scan_count > WIFI_SCAN_MAX_APS)
        g_snapshot.scan_count = WIFI_SCAN_MAX_APS;
    if (g_snapshot.network.profile_count > WIFI_PROFILE_MAX)
        g_snapshot.network.profile_count = WIFI_PROFILE_MAX;
    g_snapshot_valid = true;
    if (g_parked_draft.valid && g_parked_draft.profile_id && !profile_by_id(g_parked_draft.profile_id))
        wifi_secure_zero(&g_parked_draft, sizeof(g_parked_draft));
    if (g_recovery.valid &&
        ((int32_t)(g_snapshot.network.connection_operation_id-g_recovery.operation)>0 ||
         (g_recovery.profile_id && !profile_by_id(g_recovery.profile_id))))
        wifi_secure_zero(&g_recovery, sizeof(g_recovery));
    if (g_feedback_scoped &&
        (int32_t)(g_snapshot.network.connection_operation_id-g_feedback_operation)>0)
        set_feedback("");
    if (g_render_scan_generation != g_snapshot.scan_generation ||
        g_render_config_revision != g_snapshot.network.config_revision)
        build_scan_order();
    process_pending();
    retry_pending_save_state();
    refresh_snapshot_ui();
}

void scr_wifi_init(void)
{
    reset_widget_refs();
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot_observed_tick=0;g_snapshot_poll_error=false;
    memset(&g_selected_ap, 0, sizeof(g_selected_ap));
    memset(g_scan_order, 0, sizeof(g_scan_order));
    memset(g_row_kind, 0, sizeof(g_row_kind));
    memset(g_row_token, 0xff, sizeof(g_row_token));
    memset(g_password_before, 0, sizeof(g_password_before));
    memset(g_draft_ssid,0,sizeof(g_draft_ssid));g_draft_ssid_length=0;
    g_draft_security=WIFI_SECURITY_UNSUPPORTED;
    memset(&g_pending, 0, sizeof(g_pending));
    wifi_secure_zero(&g_recovery, sizeof(g_recovery));
    wifi_secure_zero(&g_parked_draft, sizeof(g_parked_draft));
    g_draft_revision = 0U;
    memset(g_feedback, 0, sizeof(g_feedback));
    memset(&g_feedback_target, 0, sizeof(g_feedback_target));
    g_feedback_scoped = false;
    g_feedback_operation = 0U;
    for (unsigned index = 0U; index < 95U; ++index)
        g_password_chars[index] = (char)(32U + index);
    g_password_chars[95] = '\0';
    g_snapshot_valid = false;
    g_details_open = false;
    g_selected_ap_valid = false;
    g_selected_profile_id = 0U;
    g_remember_password = true;
    g_editor_open = false;
    g_password_visible = false;
    g_update_password_armed = false;
    g_save_retry_requested = false;
    g_save_retry_origin=g_save_retry_revision=0;
    g_view = WIFI_VIEW_MANAGER;
    g_list = WIFI_LIST_NEARBY;
    g_list_page = 0U;
    g_password_context = WIFI_PASSWORD_NONE;
    g_confirm_action = WIFI_CONFIRM_NONE;
    g_render_epoch = 1U;
    g_render_scan_generation = UINT32_MAX;
    g_render_config_revision = UINT32_MAX;
    g_render_active_profile = UINT32_MAX;
    g_render_publication_stamp=UINT32_MAX;
    g_render_list = WIFI_LIST_SAVED;
    g_render_page = UINT8_MAX;
    g_last_poll_tick = 0U;
    g_poll_started = RT_FALSE;
}

#if defined(WORK_UI_HOST) && WORK_UI_HOST
void scr_wifi_host_reset_operations(void)
{
    wifi_secure_zero(&g_pending, sizeof(g_pending));
    wifi_secure_zero(&g_recovery, sizeof(g_recovery));
    wifi_secure_zero(&g_parked_draft, sizeof(g_parked_draft));
    memset(g_feedback, 0, sizeof(g_feedback));
    g_feedback_scoped=false;g_feedback_operation=0;
}
#endif

static lv_obj_t *create_overlay(lv_obj_t *parent, int x, int y,
                                int width, int height, lv_color_t color)
{
    lv_obj_t *overlay = lv_obj_create(parent);
    if (!overlay) return NULL;
    lv_obj_set_hidden(overlay, true);
    lv_obj_set_pos(overlay, x, y);
    lv_obj_set_size(overlay, width, height);
    lv_obj_set_layout(overlay, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, color, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_scrollable(overlay, false);
    return overlay;
}

enum {
    WIFI_CREATE_TITLE = 0,
    WIFI_CREATE_MODE_WIFI,
    WIFI_CREATE_MODE_DISPLAY,
    WIFI_CREATE_STATUS_PANEL,
    WIFI_CREATE_PHASE_LABEL,
    WIFI_CREATE_IP_LABEL,
    WIFI_CREATE_META_LABEL,
    WIFI_CREATE_STATUS_DISCONNECT,
    WIFI_CREATE_TAB_NEARBY,
    WIFI_CREATE_TAB_SAVED,
    WIFI_CREATE_PAGE_PREV,
    WIFI_CREATE_PAGE_LABEL,
    WIFI_CREATE_PAGE_NEXT,
    WIFI_CREATE_AUTO_BUTTON,
    WIFI_CREATE_SCAN_BUTTON,
    WIFI_CREATE_CLEAR_BUTTON,
    WIFI_CREATE_RETRY_BUTTON,
    WIFI_CREATE_LIST_PANEL,
    WIFI_CREATE_LIST_SETUP,
    WIFI_CREATE_EMPTY_LABEL,
    WIFI_CREATE_EMPTY_LABEL_ALIGN,
    WIFI_CREATE_ROW_BASE,
    WIFI_CREATE_DISPLAY_PANEL = WIFI_CREATE_ROW_BASE + WIFI_UI_ROWS,
    WIFI_CREATE_DISPLAY_TITLE,
    WIFI_CREATE_WEB_LABEL,
    WIFI_CREATE_MOTION_TITLE,
    WIFI_CREATE_MOTION_BUTTON,
    WIFI_CREATE_DISPLAY_NOTE,
    WIFI_CREATE_HIDE_DISPLAY,
    WIFI_CREATE_DETAILS,
    WIFI_CREATE_DETAIL_TITLE,
    WIFI_CREATE_DETAIL_BACK,
    WIFI_CREATE_DETAIL_INFO,
    WIFI_CREATE_DETAIL_NAME,
    WIFI_CREATE_DETAIL_META,
    WIFI_CREATE_DETAIL_NOTE,
    WIFI_CREATE_DETAIL_FEEDBACK,
    WIFI_CREATE_PASSWORD_CAPTION,
    WIFI_CREATE_PASSWORD_AREA,
    WIFI_CREATE_REMEMBER_BUTTON,
    WIFI_CREATE_DETAIL_CONNECT,
    WIFI_CREATE_DETAIL_DISCONNECT,
    WIFI_CREATE_DETAIL_TEMPORARY,
    WIFI_CREATE_DETAIL_UPDATE,
    WIFI_CREATE_DETAIL_AUTO,
    WIFI_CREATE_DETAIL_FORGET,
    WIFI_CREATE_HIDE_DETAILS,
    WIFI_CREATE_INPUT_PANEL,
    WIFI_CREATE_PASSWORD_COUNT,
    WIFI_CREATE_PASSWORD_NETWORK,
    WIFI_CREATE_PASSWORD_CONTROL_BASE,
    WIFI_CREATE_KEYBOARD = WIFI_CREATE_PASSWORD_CONTROL_BASE + 6U,
    WIFI_CREATE_KEYBOARD_STYLE,
    WIFI_CREATE_KEYBOARD_CONTROLS,
    WIFI_CREATE_KEYBOARD_MAP_LOWER,
    WIFI_CREATE_KEYBOARD_MAP_UPPER,
    WIFI_CREATE_KEYBOARD_MAP_SPECIAL,
    WIFI_CREATE_KEYBOARD_MAP_MORE,
    WIFI_CREATE_KEYBOARD_EVENTS,
    WIFI_CREATE_HIDE_INPUT,
    WIFI_CREATE_CONFIRM,
    WIFI_CREATE_CONFIRM_OPACITY,
    WIFI_CREATE_CONFIRM_CARD,
    WIFI_CREATE_CONFIRM_CARD_STYLE,
    WIFI_CREATE_CONFIRM_TITLE,
    WIFI_CREATE_CONFIRM_BODY,
    WIFI_CREATE_CONFIRM_YES,
    WIFI_CREATE_CONFIRM_NO,
    WIFI_CREATE_HIDE_CONFIRM,
    WIFI_CREATE_FINALIZE,
    WIFI_CREATE_STAGE_COUNT
};

lv_obj_t *scr_wifi_create_begin(lv_obj_t *parent)
{
    if (g_page) return g_page;
    if (!parent) return NULL;

    /* Reset widget handles only. Drafts, snapshots and pending service state
     * owned by scr_wifi_init() deliberately survive preloading. */
    reset_widget_refs();
    g_page = create_overlay(parent, 0, 0, 800, UI_CONTENT_HEIGHT, C_BG);
    return g_page;
}

bool scr_wifi_is_ready(void)
{
    return g_create_ready;
}

unsigned scr_wifi_create_progress(void)
{
    if (g_create_ready) return 100U;
    if (!g_page) return 0U;
    return (g_create_stage * 100U) / WIFI_CREATE_STAGE_COUNT;
}

bool scr_wifi_create_step(void)
{
    static const struct {
        const char *caption;
        uintptr_t action;
        int x, y, width;
    } password_controls[] = {
        {"清空", PWD_CLEAR, 488, 0, 80},
        {"取消", PWD_CANCEL, 576, 0, 88},
        {"完成", PWD_DONE, 672, 0, 88},
        {"显示", PWD_SHOW, 536, 54, 72},
        {"<", PWD_LEFT, 616, 54, 64},
        {">", PWD_RIGHT, 688, 54, 64}
    };

    if (g_create_ready) return true;
    if (!g_page) return false;

    if (g_create_stage >= WIFI_CREATE_ROW_BASE &&
        g_create_stage < WIFI_CREATE_DISPLAY_PANEL) {
        unsigned slot = g_create_stage - WIFI_CREATE_ROW_BASE;
        int y = 2 + (int)slot * WIFI_UI_ROW_HEIGHT;
        g_rows[slot] = wifi_button(g_list_panel, "", 4, y, 768,
                                   row_select_cb, (void *)(uintptr_t)slot);
        lv_obj_set_style_radius(g_rows[slot], 6, 0);
        lv_obj_set_style_pad_all(g_rows[slot], 2, 0);
        lv_obj_set_layout(g_rows[slot], LV_LAYOUT_NONE);
        g_row_titles[slot] = lv_obj_get_child(g_rows[slot], 0);
        lv_obj_set_align(g_row_titles[slot], LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(g_row_titles[slot], 10, 2);
        lv_obj_set_size(g_row_titles[slot], 744, 22);
        lv_obj_set_style_text_font(g_row_titles[slot], F_BODY, 0);
        lv_obj_set_style_text_align(g_row_titles[slot], LV_TEXT_ALIGN_LEFT, 0);
        g_row_meta[slot] = wifi_text(g_rows[slot], "", F_META, C_GRAY,
                                     10, 25, 744, 18);
        lv_obj_set_style_text_align(g_row_meta[slot], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_hidden(g_rows[slot], true);
        goto step_complete;
    }

    if (g_create_stage >= WIFI_CREATE_PASSWORD_CONTROL_BASE &&
        g_create_stage < WIFI_CREATE_KEYBOARD) {
        unsigned index = g_create_stage - WIFI_CREATE_PASSWORD_CONTROL_BASE;
        lv_obj_t *control = wifi_button(g_input_overlay,
            password_controls[index].caption, password_controls[index].x,
            password_controls[index].y, password_controls[index].width,
            password_button_cb, (void *)password_controls[index].action);
        if (password_controls[index].action == PWD_SHOW)
            g_password_show = control;
        goto step_complete;
    }

    switch (g_create_stage) {
    case WIFI_CREATE_TITLE:
        g_title = wifi_text(g_page, "WiFi 管理", F_TITLE, C_TEXT,
                            12, 8, 500, 28);
        break;
    case WIFI_CREATE_MODE_WIFI:
        g_mode_wifi = wifi_button(g_page, "WiFi", 556, 2, 104,
                                  view_wifi_cb, NULL);
        wifi_button_selected(g_mode_wifi, true);
        break;
    case WIFI_CREATE_MODE_DISPLAY:
        g_mode_display = wifi_button(g_page, "显示 / 网页", 668, 2, 120,
                                     view_display_cb, NULL);
        break;
    case WIFI_CREATE_STATUS_PANEL:
        g_status_panel = wifi_panel(g_page, 12, 50, 776, 48);
        break;
    case WIFI_CREATE_PHASE_LABEL:
        g_phase_label = wifi_text(g_status_panel, "待命", F_BODY, C_TEXT,
                                  10, 2, 220, 24);
        break;
    case WIFI_CREATE_IP_LABEL:
        g_ip_label = wifi_text(g_status_panel, "IP：—", F_BODY, C_GRAY,
                               248, 5, 220, 24);
        break;
    case WIFI_CREATE_META_LABEL:
        g_meta_label = wifi_text(g_status_panel,
            "正在读取配置…", F_META, C_GRAY,
            10, 28, 650, 18);
        break;
    case WIFI_CREATE_STATUS_DISCONNECT:
        g_status_disconnect = wifi_button(g_status_panel, "断开", 660, 0,
            104, disconnect_begin_cb, NULL);
        lv_obj_set_hidden(g_status_disconnect, true);
        break;
    case WIFI_CREATE_TAB_NEARBY:
        g_tab_nearby = wifi_button(g_page, "附近网络", 12, 102, 92,
                                   nearby_tab_cb, NULL);
        break;
    case WIFI_CREATE_TAB_SAVED:
        g_tab_saved = wifi_button(g_page, "已保存 (0)", 108, 102, 92,
                                  saved_tab_cb, NULL);
        break;
    case WIFI_CREATE_PAGE_PREV:
        g_page_prev = wifi_button(g_page, "‹", 204, 102, 48,
                                  page_prev_cb, NULL);
        break;
    case WIFI_CREATE_PAGE_LABEL:
        g_page_label = wifi_text(g_page, "0 / 0", F_META, C_GRAY,
                                 258, 116, 78, 20);
        lv_obj_set_style_text_align(g_page_label, LV_TEXT_ALIGN_CENTER, 0);
        break;
    case WIFI_CREATE_PAGE_NEXT:
        g_page_next = wifi_button(g_page, "›", 340, 102, 48,
                                  page_next_cb, NULL);
        break;
    case WIFI_CREATE_AUTO_BUTTON:
        g_auto_button = wifi_button(g_page, "全局自动：关", 396, 102, 148,
                                    global_auto_cb, NULL);
        break;
    case WIFI_CREATE_SCAN_BUTTON:
        g_scan_button = wifi_button(g_page, "扫描", 552, 102, 60,
                                    scan_cb, NULL);
        break;
    case WIFI_CREATE_CLEAR_BUTTON:
        g_clear_button = wifi_button(g_page, "清空全部", 620, 102, 76,
                                     clear_all_begin_cb, NULL);
        lv_obj_set_hidden(g_clear_button, true);
        break;
    case WIFI_CREATE_RETRY_BUTTON:
        g_retry_button = wifi_button(g_page, "重试保存", 704, 102, 84,
                                     retry_save_cb, NULL);
        break;
    case WIFI_CREATE_LIST_PANEL:
        g_list_panel = wifi_panel(g_page, 12, 158, 776, 202);
        break;
    case WIFI_CREATE_LIST_SETUP:
        lv_obj_set_scrollable(g_list_panel, true);
        lv_obj_set_scroll_dir(g_list_panel, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(g_list_panel, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_elastic(g_list_panel, false);
        lv_obj_add_event_cb(g_list_panel, list_scroll_cb, LV_EVENT_SCROLL, NULL);
        break;
    case WIFI_CREATE_EMPTY_LABEL:
        g_empty_label = wifi_text(g_list_panel, "尚无扫描结果，请点击扫描",
                                  F_BODY, C_GRAY, 18, 77, 740, 48);
        break;
    case WIFI_CREATE_EMPTY_LABEL_ALIGN:
        lv_obj_set_style_text_align(g_empty_label, LV_TEXT_ALIGN_CENTER, 0);
        break;
    case WIFI_CREATE_DISPLAY_PANEL:
        g_display_panel = wifi_panel(g_page, 12, 50, 776, 242);
        break;
    case WIFI_CREATE_DISPLAY_TITLE:
        (void)wifi_text(g_display_panel, "显示与网页", F_TITLE, C_TEXT,
                        16, 12, 300, 28);
        break;
    case WIFI_CREATE_WEB_LABEL:
        g_web_label = wifi_text(g_display_panel, "证据网页尚未启动",
                                F_BODY, C_GRAY, 16, 54, 720, 44);
        lv_label_set_long_mode(g_web_label, LV_LABEL_LONG_MODE_WRAP);
        break;
    case WIFI_CREATE_MOTION_TITLE:
        (void)wifi_text(g_display_panel, "减少动态效果", F_TITLE, C_TEXT,
                        16, 112, 300, 28);
        break;
    case WIFI_CREATE_MOTION_BUTTON:
        g_motion_button = wifi_button(g_display_panel, "当前：流畅动效",
            16, 148, 260, motion_button_cb, NULL);
        break;
    case WIFI_CREATE_DISPLAY_NOTE:
        (void)wifi_text(g_display_panel,
            "网页读取与 LCD 相同的已发布证据。显示设置只影响页面动效，不更改 WiFi 配置。",
            F_BODY, C_GRAY, 16, 206, 720, 24);
        break;
    case WIFI_CREATE_HIDE_DISPLAY:
        lv_obj_set_hidden(g_display_panel, true);
        break;
    case WIFI_CREATE_DETAILS:
        g_details = create_overlay(g_page, 0, 0, 800, UI_CONTENT_HEIGHT, C_BG);
        break;
    case WIFI_CREATE_DETAIL_TITLE:
        g_detail_title = wifi_text(g_details, "网络详情", F_TITLE, C_TEXT,
                                   12, 8, 500, 28);
        break;
    case WIFI_CREATE_DETAIL_BACK:
        g_detail_back = wifi_button(g_details, "返回列表", 680, 2, 108,
                                    details_back_cb, NULL);
        break;
    case WIFI_CREATE_DETAIL_INFO:
        g_detail_info_panel = wifi_panel(g_details, 12, 52, 776, 100);
        break;
    case WIFI_CREATE_DETAIL_NAME:
        g_detail_name = wifi_text(g_detail_info_panel, "", F_TITLE, C_TEXT,
                                  12, 5, 748, 26);
        break;
    case WIFI_CREATE_DETAIL_META:
        g_detail_meta = wifi_text(g_detail_info_panel, "", F_BODY, C_GRAY,
                                  12, 34, 748, 22);
        break;
    case WIFI_CREATE_DETAIL_NOTE:
        g_detail_note = wifi_text(g_detail_info_panel, "", F_META, C_GRAY,
                                  12, 62, 748, 22);
        break;
    case WIFI_CREATE_DETAIL_FEEDBACK:
        g_detail_feedback = wifi_text(g_details, "", F_META, C_GRAY,
                                      16, 238, 756, 22);
        break;
    case WIFI_CREATE_PASSWORD_CAPTION:
        g_password_caption = wifi_text(g_details, "密码", F_META,
                                       C_GRAY, 18, 158, 500, 18);
        break;
    case WIFI_CREATE_PASSWORD_AREA:
        g_pwd_area = lv_textarea_create(g_details);
        lv_obj_set_pos(g_pwd_area, 16, 178);
        lv_obj_set_size(g_pwd_area, 512, WIFI_UI_ROW_HEIGHT);
        lv_obj_set_style_text_font(g_pwd_area, F_BODY, 0);
        lv_obj_set_style_text_color(g_pwd_area, C_TEXT, 0);
        lv_obj_set_style_bg_color(g_pwd_area, C_RAISED, 0);
        lv_obj_set_style_border_color(g_pwd_area, C_DIVIDER, 0);
        lv_obj_set_style_pad_top(g_pwd_area, 13, 0);
        lv_obj_set_style_pad_bottom(g_pwd_area, 13, 0);
        lv_obj_set_height(g_pwd_area, WIFI_UI_ROW_HEIGHT);
        lv_textarea_set_one_line(g_pwd_area, true);
        lv_textarea_set_password_mode(g_pwd_area, true);
        lv_textarea_set_password_bullet(g_pwd_area, "*");
        lv_textarea_set_password_show_time(g_pwd_area, 0);
        lv_textarea_set_accepted_chars_static(g_pwd_area, g_password_chars);
        lv_textarea_set_max_length(g_pwd_area, WIFI_PASSWORD_BYTES_MAX);
        lv_textarea_set_placeholder_text(g_pwd_area, "WiFi 密码");
        lv_textarea_set_text(g_pwd_area, "");
        lv_obj_set_scrollbar_mode(g_pwd_area, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_anim_duration(g_pwd_area, 0, LV_PART_CURSOR);
        lv_obj_set_style_anim_duration(g_pwd_area, 0,
            LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_set_style_anim_duration(g_pwd_area, 0,
            LV_PART_CURSOR | LV_STATE_FOCUSED | LV_STATE_EDITED);
        lv_obj_set_style_opa(g_pwd_area, LV_OPA_TRANSP, LV_PART_CURSOR);
        lv_anim_delete(g_pwd_area, NULL);
        lv_obj_add_event_cb(g_pwd_area, password_changed_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(g_pwd_area, pwd_click_cb, LV_EVENT_CLICKED, NULL);
        break;
    case WIFI_CREATE_REMEMBER_BUTTON:
        g_remember_button = wifi_button(g_details, "保存网络：开", 544, 184,
                                        244, remember_cb, NULL);
        break;
    case WIFI_CREATE_DETAIL_CONNECT:
        g_detail_connect = wifi_button(g_details, "连接", 12, 300, 136,
                                       connect_cb, NULL);
        break;
    case WIFI_CREATE_DETAIL_DISCONNECT:
        g_detail_disconnect = wifi_button(g_details, "断开连接", 156, 300,
                                          136, disconnect_begin_cb, NULL);
        break;
    case WIFI_CREATE_DETAIL_TEMPORARY:
        g_detail_temporary = wifi_button(g_details, "继续临时", 156, 300,
                                         136, use_temporary_cb, NULL);
        lv_obj_set_hidden(g_detail_temporary, true);
        break;
    case WIFI_CREATE_DETAIL_UPDATE:
        g_detail_update = wifi_button(g_details, "更新密码", 300, 300, 136,
                                      update_password_begin_cb, NULL);
        break;
    case WIFI_CREATE_DETAIL_AUTO:
        g_detail_auto = wifi_button(g_details, "此网络自动：关", 444, 300,
                                    156, profile_auto_cb, NULL);
        break;
    case WIFI_CREATE_DETAIL_FORGET:
        g_detail_forget = wifi_button(g_details, "忘记网络", 608, 300, 180,
                                      forget_begin_cb, NULL);
        break;
    case WIFI_CREATE_HIDE_DETAILS:
        lv_obj_set_hidden(g_details, true);
        break;
    case WIFI_CREATE_INPUT_PANEL:
        g_input_overlay = wifi_panel(g_page, 12, 8, 776, 356);
        lv_obj_set_style_bg_color(g_input_overlay, C_RAISED, 0);
        break;
    case WIFI_CREATE_PASSWORD_COUNT:
        g_password_count = wifi_text(g_input_overlay, "WiFi 密码  0/63",
                                     F_TITLE, C_TEXT, 12, 8, 472, 28);
        break;
    case WIFI_CREATE_PASSWORD_NETWORK:
        g_password_network = wifi_text(g_input_overlay, "", F_META, C_GRAY,
                                       12, 34, 472, 18);
        break;
    case WIFI_CREATE_KEYBOARD:
        /* LVGL builds the keyboard's internal button matrix in this call;
         * the widget has no public incremental construction interface. */
        g_kb = lv_keyboard_create(g_input_overlay);
        break;
    case WIFI_CREATE_KEYBOARD_STYLE:
        lv_obj_set_align(g_kb, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(g_kb, 12, 112);
        lv_obj_set_size(g_kb, 752, 212);
        lv_obj_set_style_text_font(g_kb, F_BODY, LV_PART_ITEMS);
        lv_obj_set_style_text_color(g_kb, C_TEXT, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(g_kb, C_SURFACE, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(g_kb, C_RAISED, 0);
        lv_obj_set_style_pad_all(g_kb, 4, 0);
        lv_obj_set_style_pad_row(g_kb, 4, 0);
        lv_obj_set_style_border_width(g_kb, 0, 0);
        break;
    case WIFI_CREATE_KEYBOARD_CONTROLS:
        for (unsigned index = 0U; index < 34U; ++index)
            s_key_controls[index] = 1 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                    LV_BUTTONMATRIX_CTRL_NO_REPEAT;
        s_key_controls[30] = s_key_controls[31] = s_key_controls[33] =
            2 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT;
        s_key_controls[32] = 6 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                             LV_BUTTONMATRIX_CTRL_NO_REPEAT;
        break;
    case WIFI_CREATE_KEYBOARD_MAP_LOWER:
        lv_keyboard_set_map(g_kb, LV_KEYBOARD_MODE_TEXT_LOWER,
                            s_keys_lower, s_key_controls);
        break;
    case WIFI_CREATE_KEYBOARD_MAP_UPPER:
        lv_keyboard_set_map(g_kb, LV_KEYBOARD_MODE_TEXT_UPPER,
                            s_keys_upper, s_key_controls);
        break;
    case WIFI_CREATE_KEYBOARD_MAP_SPECIAL:
        lv_keyboard_set_map(g_kb, LV_KEYBOARD_MODE_SPECIAL,
                            s_keys_special, s_key_controls);
        break;
    case WIFI_CREATE_KEYBOARD_MAP_MORE:
        lv_keyboard_set_map(g_kb, LV_KEYBOARD_MODE_USER_1,
                            s_keys_more, s_key_controls);
        break;
    case WIFI_CREATE_KEYBOARD_EVENTS:
        lv_obj_remove_event_cb(g_kb, lv_keyboard_def_event_cb);
        lv_obj_add_event_cb(g_kb, kb_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(g_kb, kb_close_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(g_kb, kb_close_cb, LV_EVENT_CANCEL, NULL);
        break;
    case WIFI_CREATE_HIDE_INPUT:
        lv_obj_set_hidden(g_input_overlay, true);
        break;
    case WIFI_CREATE_CONFIRM:
        g_confirm_overlay = create_overlay(g_page, 0, 0, 800,
                                           UI_CONTENT_HEIGHT, C_BG);
        break;
    case WIFI_CREATE_CONFIRM_OPACITY:
        lv_obj_set_style_bg_opa(g_confirm_overlay, 176, 0);
        break;
    case WIFI_CREATE_CONFIRM_CARD:
        g_confirm_card = wifi_panel(g_confirm_overlay, 140, 70, 520, 220);
        break;
    case WIFI_CREATE_CONFIRM_CARD_STYLE:
        lv_obj_set_style_bg_color(g_confirm_card, C_RAISED, 0);
        break;
    case WIFI_CREATE_CONFIRM_TITLE:
        g_confirm_title = wifi_text(g_confirm_card, "请确认", F_TITLE, C_TEXT,
                                    16, 12, 488, 28);
        break;
    case WIFI_CREATE_CONFIRM_BODY:
        g_confirm_body = wifi_text(g_confirm_card, "", F_BODY, C_GRAY,
                                   16, 50, 488, 92);
        lv_label_set_long_mode(g_confirm_body, LV_LABEL_LONG_MODE_WRAP);
        break;
    case WIFI_CREATE_CONFIRM_YES:
        g_confirm_yes = wifi_button(g_confirm_card, "确认", 252, 160, 112,
                                    confirm_yes_cb, NULL);
        break;
    case WIFI_CREATE_CONFIRM_NO:
        g_confirm_no = wifi_button(g_confirm_card, "取消", 376, 160, 112,
                                   confirm_no_cb, NULL);
        break;
    case WIFI_CREATE_HIDE_CONFIRM:
        lv_obj_set_hidden(g_confirm_overlay, true);
        break;
    case WIFI_CREATE_FINALIZE:
        /* Keep the returned root hidden. The caller that navigates to this
         * screen refreshes its model and decides when to reveal it. */
        break;
    default:
        return false;
    }

step_complete:
    ++g_create_stage;
    if (g_create_stage >= WIFI_CREATE_STAGE_COUNT) {
        g_create_ready = true;
        return true;
    }
    return false;
}

lv_obj_t *scr_wifi_create(lv_obj_t *parent)
{
    lv_obj_t *page = scr_wifi_create_begin(parent);
    if (!page) return NULL;
    while (!scr_wifi_create_step()) { }
    refresh_snapshot_ui();
    render_rows();
    scr_wifi_refresh_preferences();
    lv_obj_set_hidden(g_page, false);
    return g_page;
}

void scr_wifi_refresh_preferences(void)
{
    if (scr_wifi_is_ready() && g_motion_button) draw_display_view();
}

bool scr_wifi_capture_allowed(void)
{
    if (!scr_wifi_is_ready()) return true;
    return !g_pwd_area || lv_obj_is_hidden(g_page) ||
        lv_textarea_get_password_mode(g_pwd_area) || !password_text()[0];
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
/* UI-owner-only screenshot hooks. Selection/confirmation hooks only expose
 * screens; they never submit a network or storage request. */
bool scr_wifi_debug_editor(unsigned action)
{
    if (!scr_wifi_is_ready() || !g_page || lv_obj_is_hidden(g_page)) return false;
    if (action == 0U) pwd_click_commit(0U);
    else if (action == 1U) password_action_commit(PWD_CANCEL);
    else if (action == 2U) password_action_commit(PWD_DONE);
    else if (action == 3U) password_action_commit(PWD_SPECIAL);
    else if (action == 4U) password_action_commit(PWD_MORE);
    else return false;
    return true;
}

bool scr_wifi_debug_select_network(unsigned index)
{
    if (!scr_wifi_is_ready() || !g_page || lv_obj_is_hidden(g_page) || !g_snapshot_valid ||
        index >= g_scan_visible_count) return false;
    view_commit(0U);
    g_list = WIFI_LIST_NEARBY;
    g_list_page = (uint8_t)(index / WIFI_UI_PAGE_ROWS);
    const wifi_ap_record_t *ap=&g_snapshot.access_points[g_scan_order[index]];
    bind_password_draft((const uint8_t*)ap->ssid,ap_ssid_length(ap),ap_security(ap));
    memcpy(&g_selected_ap,
           &g_snapshot.access_points[g_scan_order[index]], sizeof(g_selected_ap));
    g_selected_ap_valid = true;
    g_selected_profile_id = 0U;
    g_details_open = true;
    render_rows();
    lv_obj_set_hidden(g_details, false);
    refresh_details();
    return true;
}

bool scr_wifi_debug_view(unsigned kind)
{
    if(!scr_wifi_is_ready()||!g_page||lv_obj_is_hidden(g_page)||kind>2U)return false;
    if(g_confirm_overlay)confirm_cancel_commit(0U);
    view_commit(kind==1U?1U:0U);
    if(kind==2U)set_list_commit(1U);
    return true;
}

bool scr_wifi_debug_select_saved(uint32_t profile_id)
{
    if (!scr_wifi_is_ready()) return false;
    const wifi_profile_summary_t *profile = profile_by_id(profile_id);
    if (!g_page || lv_obj_is_hidden(g_page) || !g_snapshot_valid || !profile) return false;
    view_commit(0U);
    set_list_commit(1U);
    bind_password_draft((const uint8_t *)profile->ssid, profile->ssid_length, profile->security);
    g_selected_profile_id = profile_id;
    g_selected_ap_valid = false;
    g_details_open = true;
    refresh_details();
    return true;
}

bool scr_wifi_debug_connect_saved_selected(void)
{
    if (!scr_wifi_is_ready()) return false;
    const wifi_profile_summary_t *profile = profile_by_id(g_selected_profile_id);
    if (!g_page || lv_obj_is_hidden(g_page) || !g_snapshot_valid ||
        !g_details_open || g_selected_ap_valid || !profile || !profile->persisted) return false;
    if (selected_network_online() || connection_transition()) return false;
    connect_commit(0U);
    return !g_details_open;
}

bool scr_wifi_debug_confirm_screen(unsigned kind)
{
    if (!scr_wifi_is_ready()) return false;
    if(kind==5U){confirm_cancel_commit(0U);return true;}
    static const struct {
        wifi_confirm_action_t action;
        const char *title, *body, *yes;
    } screens[] = {
        {WIFI_CONFIRM_DISCONNECT, "断开 WiFi？",
         "设备将断开当前无线网络；已保存的网络配置不会被删除。", "确认断开"},
        {WIFI_CONFIRM_FORGET, "忘记此网络？",
         "此操作会从设备 WiFi 配置中移除此网络。", "忘记网络"},
        {WIFI_CONFIRM_UPDATE_PASSWORD, "更新此网络密码？",
         "密码将提交给 WiFi 服务；只有服务确认保存后，屏幕才清除草稿。", "确认更新"},
        {WIFI_CONFIRM_CLEAR_FIRST, "清空全部 WiFi 配置？",
         "将删除设备已保存的 WiFi 网络，不会影响其他系统配置。", "继续"},
        {WIFI_CONFIRM_CLEAR_FINAL, "再次确认清除",
         "最后确认：只清除本设备保存的 WiFi 网络。此操作不可撤销。", "全部清除"}
    };
    if (!g_page || lv_obj_is_hidden(g_page) ||
        kind >= sizeof(screens) / sizeof(screens[0])) return false;
    show_confirm(screens[kind].action, screens[kind].title,
                 screens[kind].body, screens[kind].yes);
    return true;
}
#endif

void scr_wifi_poll(void)
{
    if (!scr_wifi_is_ready() || !g_page) return;
    if (lv_obj_is_hidden(g_page)) {
        scr_wifi_close_editor();
        if (g_details_open) {
            g_details_open = false;
            lv_obj_set_hidden(g_details, true);
        }
        if (g_pending.valid || g_recovery.valid) update_poll_tick();
        return;
    }
    update_poll_tick();
}
