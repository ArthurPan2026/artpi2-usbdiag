#include "ui_widgets.h"
#include "ui_deferred.h"
#include <lvgl/draw/lv_draw_label.h>
#include <lvgl/draw/lv_draw_rect.h>
#include <stdio.h>
#include <string.h>

static bool s_user_reduced_motion;
static bool s_startup_limited;
static bool s_effective_reduced_motion;
static bool s_transition_initialized;
static lv_style_transition_dsc_t s_press_transition;
static const lv_style_prop_t s_press_props[] = {
    LV_STYLE_BG_COLOR, LV_STYLE_BORDER_COLOR, 0
};

static void apply_motion_settings(void)
{
    bool reduced = s_user_reduced_motion || s_startup_limited;
    if (s_transition_initialized && reduced == s_effective_reduced_motion)
        return;

    s_effective_reduced_motion = reduced;
    s_transition_initialized = true;
    lv_style_transition_dsc_init(&s_press_transition, s_press_props,
        lv_anim_path_ease_out, reduced ? 0 : UI_PRESS_MS, 0, NULL);
}

lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, &style_card, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_scrollable(o, false);
    return o;
}
lv_obj_t *ui_text(lv_obj_t *p, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *o = lv_label_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, color, 0);
    lv_label_set_text(o, text ? text : "");
    lv_obj_set_scroll_chain(o, true);
    return o;
}
static void button_click(lv_event_t *event)
{
    if (!ui_event_is_release_click(event)) return;
    uintptr_t packed = (uintptr_t)lv_event_get_user_data(event);
    ui_model_post((ui_action_t)(packed >> 16), (int16_t)(packed & 0xffff));
}
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, int width, ui_action_t action, int32_t arg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, width, UI_TOUCH_HEIGHT);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 4, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, C_RAISED, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C_SURFACE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, C_SURFACE, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_color_filter_opa(b, 0, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(b, C_DIVIDER, 0);
    lv_obj_set_style_border_color(b, C_BLUE, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_outline_color(b, C_BLUE, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
    apply_motion_settings();
    lv_obj_set_style_transition(b, &s_press_transition, 0);
    lv_obj_set_scrollable(b, false);
    lv_obj_set_scroll_chain(b, true);
    lv_obj_t *label = ui_text(b, text, F_BODY, C_TEXT);
    lv_obj_center(label);
    lv_obj_add_event_cb(b, button_click, LV_EVENT_CLICKED,
        (void *)(((uintptr_t)action << 16) | (uint16_t)arg));
    return b;
}
void ui_button_set_selected(lv_obj_t *b, bool selected)
{
    if (!b) return;
    /* USER_1 caches selection; USER_2 distinguishes the first style apply.
     * No project style is bound to either marker state. */
    bool initialized = lv_obj_has_state(b, LV_STATE_USER_2);
    bool was_selected = lv_obj_has_state(b, LV_STATE_USER_1);
    if (initialized && was_selected == selected) return;
    if (selected) lv_obj_add_state(b, LV_STATE_USER_1);
    else lv_obj_remove_state(b, LV_STATE_USER_1);
    if (!initialized) lv_obj_add_state(b, LV_STATE_USER_2);
    lv_obj_set_style_bg_color(b, selected ? C_SURFACE : C_RAISED, 0);
    lv_obj_set_style_border_color(b, selected ? C_BLUE : C_DIVIDER, 0);
    lv_obj_t *label = lv_obj_get_child(b, 0);
    if (label) lv_obj_set_style_text_color(label, selected ? C_BLUE : C_GRAY, 0);
}
void ui_label_set_safe(lv_obj_t *label, const char *text)
{
    char safe[1024];
    if (!label) return;
    ui_font_sanitize_dynamic_text(text, safe, sizeof(safe));
    if (strcmp(lv_label_get_text(label), safe) == 0) return;
    lv_label_set_text(label, safe);
}
void ui_label_set_formatted(lv_obj_t *label, const char *text)
{
    char output[1536], line[1024], safe[1024];
    size_t used = 0;
    if (!label) return;
    if (!text) text = "";
    while (*text && used + 1 < sizeof(output)) {
        const char *end = strchr(text, '\n');
        size_t length = end ? (size_t)(end - text) : strlen(text);
        if (length >= sizeof(line)) length = sizeof(line) - 1;
        memcpy(line, text, length); line[length] = '\0';
        size_t count = ui_font_sanitize_dynamic_text(line, safe, sizeof(safe));
        if (count > sizeof(output) - used - 1) count = sizeof(output) - used - 1;
        memcpy(output + used, safe, count); used += count;
        if (!end) break;
        if (used + 1 < sizeof(output)) output[used++] = '\n';
        text = end + 1;
    }
    output[used] = '\0';
    if (strcmp(lv_label_get_text(label), output) == 0) return;
    lv_label_set_text(label, output);
}
static void enter_exec(void *obj, int32_t value) { lv_obj_set_style_translate_x(obj, value, 0); }
void ui_animate_enter(lv_obj_t *obj, unsigned ms, int distance)
{
    if (!obj) return;
    lv_anim_delete(obj, enter_exec);
    lv_obj_set_style_translate_x(obj, 0, 0);
    if (s_effective_reduced_motion || !ms || !distance) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, enter_exec);
    lv_anim_set_values(&a, distance, 0);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
void ui_widgets_set_reduced_motion(bool value)
{
    s_user_reduced_motion = value;
    apply_motion_settings();
}
void ui_widgets_set_startup_limited(bool value)
{
    s_startup_limited = value;
    apply_motion_settings();
}
lv_color_t ui_verdict_color(usb_rule_verdict_t v)
{
    return v == USB_RULE_PASS ? C_GREEN : v == USB_RULE_FAIL ? C_RED :
        v == USB_RULE_NOT_OBSERVED ? C_ORANGE : C_GRAY;
}

/* Complete payload addressability with only ten cached rows. This surface
 * lives outside the 512 KiB widget allocator, in non-cacheable PSRAM. */
#define HEX_WIDTH 480
#define HEX_ROWS 10
#define HEX_MAX_ROW_HEIGHT 32
#define HEX_NORMAL_COLUMNS 16
#define HEX_LARGE_COLUMNS 8
static uint16_t s_hex_pixels[HEX_WIDTH * HEX_ROWS * HEX_MAX_ROW_HEIGHT]
    __attribute__((section(".lvgl_ram"), aligned(64), used));
static lv_obj_t *s_hex_owner, *s_hex_header, *s_hex_view, *s_hex_spacer, *s_hex_canvas;
static const usb_evidence_raw_event_t *s_hex_event;
static uint32_t s_hex_epoch, s_hex_crc;
static uint16_t s_hex_offset, s_hex_length;
static int s_hex_first = -1;
static int s_hex_row_height = 26;
static bool s_hex_large;
unsigned ui_hex_columns(void) { return s_hex_large ? HEX_LARGE_COLUMNS : HEX_NORMAL_COLUMNS; }

static void draw_text(lv_layer_t *layer, const char *text, int x, int y, int w,
                       const lv_font_t *font, lv_color_t color)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    /* Each delayed task needs its own copy of a reused stack string. */
    d.text_static = 0;
    d.text_local = 1;
    d.flag = LV_TEXT_FLAG_EXPAND;
    d.font = font;
    d.color = color;
    lv_area_t area = {x, y, x + w - 1, y + s_hex_row_height - 1};
    lv_draw_label(layer, &d, &area);
}
static void hex_render(int first)
{
    if (!s_hex_canvas || !s_hex_event) return;
    unsigned columns = ui_hex_columns();
    unsigned pitch = s_hex_large ? 54U : 27U;
    int rows = (s_hex_event->captured_length + columns - 1) / columns;
    if (first < 0) first = 0;
    if (first >= rows) first = rows ? rows - 1 : 0;
    int scroll = lv_obj_get_scroll_y(s_hex_view);
    lv_image_set_offset_y(s_hex_canvas, first * s_hex_row_height - scroll);
    if (first == s_hex_first) return;
    s_hex_first = first;
    uint16_t background = lv_color_to_u16(C_SURFACE);
    for (size_t i = 0; i < sizeof(s_hex_pixels) / sizeof(s_hex_pixels[0]); i++) s_hex_pixels[i] = background;
    lv_layer_t layer;
    lv_canvas_init_layer(s_hex_canvas, &layer);
    for (int row = 0; row < HEX_ROWS && first + row < rows; row++) {
        int y = row * s_hex_row_height;
        unsigned offset = (unsigned)(first + row) * columns;
        char address[8];
        snprintf(address, sizeof(address), "%04X", offset);
        draw_text(&layer, address, 0, y + 3, 40, F_META, C_GRAY);
        for (unsigned cell = 0; cell < columns && offset + cell < s_hex_event->captured_length; cell++) {
            unsigned position = offset + cell;
            bool selected = s_hex_length && position >= s_hex_offset &&
                position - s_hex_offset < s_hex_length;
            int x = 44 + (int)(cell * pitch);
            if (selected) {
                lv_draw_rect_dsc_t d;
                lv_draw_rect_dsc_init(&d);
                d.bg_color = C_RAISED;
                d.border_color = C_BLUE;
                d.border_width = 1;
                d.radius = 3;
                lv_area_t a = { x, y, x + (int)pitch - 2, y + s_hex_row_height - 2 };
                lv_draw_rect(&layer, &d, &a);
            }
            char hex[3];
            snprintf(hex, sizeof(hex), "%02X", s_hex_event->raw_response[position]);
            draw_text(&layer, hex, x + 2, y + 3, (int)pitch - 3,
                s_hex_large ? F_TITLE : F_BODY, selected ? C_BLUE : C_TEXT);
        }
    }
    lv_canvas_finish_layer(s_hex_canvas, &layer);
}
static void hex_scroll(lv_event_t *event)
{
    (void)event;
    int y = lv_obj_get_scroll_y(s_hex_view);
    if (y < 0) y = 0;
    hex_render(y / s_hex_row_height);
}
void ui_hex_set(lv_obj_t *container, const usb_evidence_raw_event_t *event,
                uint16_t off, uint16_t len, bool enlarged)
{
    if (!container) return;
    bool new_owner = container != s_hex_owner;
    if (new_owner) {
        if (s_hex_owner) {
            if (s_hex_header) lv_obj_delete(s_hex_header);
            if (s_hex_view) lv_obj_delete(s_hex_view);
        }
        s_hex_owner = container;
        s_hex_header = ui_text(container, "", F_META, C_GRAY);
        lv_obj_set_width(s_hex_header, lv_pct(100));
        lv_obj_set_pos(s_hex_header, 0, 0);
        s_hex_view = lv_obj_create(container);
        lv_obj_add_style(s_hex_view, &style_transp, 0);
        lv_obj_set_pos(s_hex_view, 0, 26);
        lv_obj_set_size(s_hex_view, lv_pct(100), lv_obj_get_content_height(container) - 26);
        lv_obj_set_scroll_dir(s_hex_view, LV_DIR_VER);
        lv_obj_set_scrollable(s_hex_view, true);
        lv_obj_set_scrollbar_mode(s_hex_view, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_elastic(s_hex_view, false);
        lv_obj_add_event_cb(s_hex_view, hex_scroll, LV_EVENT_SCROLL, NULL);
        s_hex_spacer = lv_obj_create(s_hex_view);
        lv_obj_add_style(s_hex_spacer, &style_transp, 0);
        lv_obj_set_size(s_hex_spacer, 1, 1);
        lv_obj_set_scrollable(s_hex_spacer, false);
        s_hex_canvas = lv_canvas_create(s_hex_view);
        lv_canvas_set_buffer(s_hex_canvas, s_hex_pixels, HEX_WIDTH,
            HEX_ROWS * HEX_MAX_ROW_HEIGHT, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_width(s_hex_canvas, HEX_WIDTH);
        lv_image_set_inner_align(s_hex_canvas, LV_IMAGE_ALIGN_TOP_LEFT);
        lv_obj_set_scrollable(s_hex_canvas, false);
        lv_obj_set_floating(s_hex_canvas, true);
        lv_obj_set_scroll_chain(s_hex_canvas, true);
    }
    const ui_view_state_t *view = ui_model_get();
    bool changed = new_owner || event != s_hex_event || s_hex_epoch != view->snapshot_epoch ||
        (event && event->payload_crc32 != s_hex_crc) || off != s_hex_offset || len != s_hex_length ||
        enlarged != s_hex_large;
    if (!changed) return;
    s_hex_event = event;
    s_hex_epoch = view->snapshot_epoch;
    s_hex_crc = event ? event->payload_crc32 : 0;
    s_hex_offset = off; s_hex_length = len; s_hex_large = enlarged;
    s_hex_row_height = enlarged ? 32 : 26;
    lv_obj_update_layout(container);
    int height = lv_obj_get_content_height(container) - 26;
    if (height < 1) height = 1;
    if (height > HEX_ROWS * HEX_MAX_ROW_HEIGHT) height = HEX_ROWS * HEX_MAX_ROW_HEIGHT;
    lv_obj_set_height(s_hex_view, height);
    lv_obj_set_height(s_hex_canvas, height);
    char caption[128];
    if (!event || !event->captured_length || event->captured_length > USB_EVIDENCE_CAPTURE_MAX) {
        lv_label_set_text(s_hex_header, event ? ui_model_capture_name(event->capture_status) : "来源事件不可用");
        lv_obj_set_hidden(s_hex_canvas, true);
        lv_obj_set_height(s_hex_spacer, 1);
        return;
    }
    bool valid = off <= event->captured_length && len <= event->captured_length - off;
    snprintf(caption, sizeof(caption), "事件 #%lu  %u 字节  %s  +%u:%u%s  %u B/row",
        (unsigned long)event->sequence, event->captured_length,
        ui_model_capture_name(event->capture_status), off, len, valid ? "" : " 引用越界", ui_hex_columns());
    lv_label_set_text(s_hex_header, caption);
    if (!valid) s_hex_length = 0;
    lv_obj_set_hidden(s_hex_canvas, false);
    unsigned columns = ui_hex_columns();
    lv_obj_set_height(s_hex_spacer, ((event->captured_length + columns - 1) / columns) * s_hex_row_height);
    if (changed) {
        s_hex_first = -1;
        unsigned first = valid && len ? off / columns : 0;
        lv_obj_scroll_to_y(s_hex_view, (int)first * s_hex_row_height, LV_ANIM_OFF);
        hex_render((int)first);
    }
}
void ui_hex_semantic(char *buffer, size_t capacity)
{
    if (!buffer || !capacity) return;
    if (!s_hex_event) { snprintf(buffer, capacity, "HEX source=missing"); return; }
    snprintf(buffer, capacity, "HEX event=%lu captured=%u selected=+%u:%u first=%d valid=%u status=%s",
        (unsigned long)s_hex_event->sequence, s_hex_event->captured_length,
        s_hex_offset, s_hex_length, s_hex_first,
        (unsigned)(s_hex_offset <= s_hex_event->captured_length &&
                   s_hex_length <= s_hex_event->captured_length - s_hex_offset),
        ui_model_capture_name(s_hex_event->capture_status));
}
lv_obj_t *ui_hex_canvas_object(void) { return s_hex_canvas; }
