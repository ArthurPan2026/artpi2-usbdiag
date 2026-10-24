#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scr_descriptors.h"
#include "ui_model.h"
#include "../ui_widgets.h"
#include "../ui_styles.h"
#include "ui_fonts.h"
#include "text_preview.h"
#include <lvgl/draw/lv_draw_label.h>
#include <lvgl/draw/lv_draw_rect.h>
#include <lvgl/font/lv_text.h>
#ifndef WORK_UI_HOST
#include "ltdc_irq_adapter.h"
#endif

#define DESC_PAGE_H             372
#define DESC_PAGE_PAD           12
#define DESC_PANE_GAP           8
#define DESC_TREE_W             252
#define DESC_DETAIL_W           516
#define DESC_PANE_H             (DESC_PAGE_H - 2 * DESC_PAGE_PAD)
#define DESC_ROW_H              48
#define DESC_TREE_INSET         8
#define DESC_TREE_VIEW_Y        54
#define DESC_TREE_VIEW_H        (DESC_PANE_H - DESC_TREE_VIEW_Y - DESC_TREE_INSET)
#define DESC_TREE_VIEW_W        (DESC_TREE_W - 2 * DESC_TREE_INSET)
#define DESC_TREE_SLOTS         ((DESC_TREE_VIEW_H + DESC_ROW_H - 1) / DESC_ROW_H + 1)
#define DESC_TOGGLE_W           48
#define DESC_TREE_SELECT_W      (DESC_TREE_VIEW_W - DESC_TOGGLE_W)
#define DESC_DETAIL_INSET       8
#define DESC_DETAIL_BODY_Y      94
#define DESC_DETAIL_BODY_H      (DESC_PANE_H - DESC_DETAIL_BODY_Y - DESC_DETAIL_INSET)
#define DESC_DETAIL_BODY_W      (DESC_DETAIL_W - 2 * DESC_DETAIL_INSET)
#define DESC_FIELD_SLOTS        ((DESC_DETAIL_BODY_H + DESC_ROW_H - 1) / DESC_ROW_H + 1)
#define DESC_BENCH_INTERVAL_MS  16U
#define DESC_BENCH_STEP_PX      8
#define DESC_NONE               (-1)
#define DESCRIPTORS_CREATE_STEP_COUNT \
    (15U + 2U * DESC_TREE_SLOTS + DESC_FIELD_SLOTS)

typedef struct {
    int16_t node_index;
    int32_t y;
    int32_t width;
    uint32_t epoch;
    bool visible;
    bool selected;
    bool collapsed;
    bool expandable;
    bool valid;
} descriptor_tree_slot_cache_t;

typedef struct {
    int16_t field_index;
    int32_t y;
    int32_t width;
    uint32_t epoch;
    bool visible;
    bool selected;
    bool valid;
} descriptor_field_slot_cache_t;

/* The two lists keep only enough floating button objects to cover their
 * viewport, including the partially visible row at either edge. */
static lv_obj_t *s_page;
static lv_obj_t *s_tree;
static lv_obj_t *s_tree_heading;
static lv_obj_t *s_tree_status;
static lv_obj_t *s_tree_view;
static lv_obj_t *s_tree_extent;
static lv_obj_t *s_tree_toggle[DESC_TREE_SLOTS];
static lv_obj_t *s_tree_select[DESC_TREE_SLOTS];
static lv_obj_t *s_tree_meta[DESC_TREE_SLOTS];
static int16_t s_tree_node[DESC_TREE_SLOTS];
static descriptor_tree_slot_cache_t s_tree_slot_cache[DESC_TREE_SLOTS];
static lv_obj_t *s_detail;
static lv_obj_t *s_detail_heading;
static lv_obj_t *s_tabs_box;
static lv_obj_t *s_tabs[2];
static lv_obj_t *s_fields;
static lv_obj_t *s_field_extent;
static lv_obj_t *s_field_rows[DESC_FIELD_SLOTS];
static int16_t s_field_global[DESC_FIELD_SLOTS];
static descriptor_field_slot_cache_t s_field_slot_cache[DESC_FIELD_SLOTS];
static lv_obj_t *s_empty;
static lv_obj_t *s_hex;

typedef enum {
    DESCRIPTORS_CREATE_TREE_PANEL,
    DESCRIPTORS_CREATE_TREE_HEADING,
    DESCRIPTORS_CREATE_TREE_STATUS,
    DESCRIPTORS_CREATE_TREE_VIEW,
    DESCRIPTORS_CREATE_TREE_EXTENT,
    DESCRIPTORS_CREATE_TREE_CANVAS,
    DESCRIPTORS_CREATE_TREE_TOGGLE,
    DESCRIPTORS_CREATE_TREE_SELECT,
    DESCRIPTORS_CREATE_DETAIL_PANEL,
    DESCRIPTORS_CREATE_DETAIL_HEADING,
    DESCRIPTORS_CREATE_TABS_BOX,
    DESCRIPTORS_CREATE_TAB_FIELDS,
    DESCRIPTORS_CREATE_TAB_HEX,
    DESCRIPTORS_CREATE_FIELDS,
    DESCRIPTORS_CREATE_FIELD_EXTENT,
    DESCRIPTORS_CREATE_FIELD_ROW,
    DESCRIPTORS_CREATE_EMPTY,
    DESCRIPTORS_CREATE_HEX
} descriptors_create_phase_t;

static descriptors_create_phase_t s_create_phase;
static uint16_t s_create_index;
static unsigned s_create_units_done;
static bool s_create_ready;

static uint16_t s_node_count;
static uint16_t s_field_count;
static uint16_t s_tree_total;
static uint16_t s_tree_first;
static uint16_t s_field_first;
static int16_t s_selected_node = DESC_NONE;
static rt_bool_t s_scroll_test;
static int32_t s_test_y;
static int8_t s_test_dir = 1;
static uint32_t s_bench_last_tick;
static bool s_bench_tick_initialized;
#ifndef WORK_UI_HOST
static uint32_t s_bench_last_scan;
#endif
static bool s_tree_rendering;
static bool s_field_rendering;
static bool s_tree_render_valid;
static bool s_field_render_valid;
static uint32_t s_tree_cached_epoch;
static uint16_t s_tree_cached_node_count;
static uint64_t s_tree_cached_collapsed;
static int16_t s_tree_cached_selected;
static bool s_tree_cached_frozen;
static int32_t s_tree_cached_scroll_y;
static uint16_t s_tree_cached_first;
static int32_t s_tree_cached_view_w;
static int32_t s_tree_cached_view_h;
static uint16_t s_tree_status_node_count = UINT16_MAX;
static uint32_t s_field_cached_epoch;
static uint16_t s_field_cached_node_count;
static uint16_t s_field_cached_total = UINT16_MAX;
static int16_t s_field_cached_node = DESC_NONE;
static int16_t s_field_cached_selection = DESC_NONE;
static uint8_t s_field_cached_tab = UINT8_MAX;
static int32_t s_field_cached_scroll_y;
static uint16_t s_field_cached_first;
static int32_t s_field_cached_view_w;
static int32_t s_field_cached_view_h;
static uint32_t s_bench_updates;
static uint32_t s_tree_refreshes;
static uint32_t s_tree_text_updates;
static uint32_t s_field_refreshes;
static uint32_t s_field_text_updates;
/* Cache the tree's text and row surfaces, not every frame's glyph tasks.
 * CPU-only metadata/click boxes remain bounded and reusable above it. */
#define TREE_SURFACE_WIDTH 240
static uint16_t s_tree_pixels[TREE_SURFACE_WIDTH * USB_EVIDENCE_DESCRIPTOR_NODE_COUNT * DESC_ROW_H]
    __attribute__((section(".ui_cache"), aligned(64), used));
static lv_obj_t *s_tree_canvas;
static uint32_t s_tree_canvas_epoch = UINT32_MAX;
static uint64_t s_tree_canvas_collapsed;
static uint16_t s_tree_canvas_first = UINT16_MAX;
static int16_t s_tree_canvas_selected = DESC_NONE;
static bool s_tree_canvas_frozen;
static uint32_t s_tree_canvas_renders;
static const ui_view_state_t *view(void);
static const usb_evidence_snapshot_t *snapshot(void);
static int32_t scroll_position(lv_obj_t *object);
static int visible_node_at(uint16_t ordinal);
static bool node_has_children(const usb_evidence_snapshot_t *s,
                               const usb_evidence_descriptor_node_t *node);
static bool node_is_collapsed(const ui_view_state_t *v, unsigned index);
static int tree_text_width(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, INT32_MAX, LV_TEXT_FLAG_EXPAND);
    return size.x;
}

static void tree_surface_text(lv_layer_t *layer, const char *text, int x, int y,
                               int width, const lv_font_t *font, lv_color_t color)
{
    char caption[192];
    work_text_preview_copy(caption, sizeof(caption), text, 0);
    size_t length = strlen(caption);
    bool shortened = false;
    int dots = tree_text_width("...", font);
    while (length && tree_text_width(caption, font) >
           width - (shortened ? dots : 0)) {
        length--;
        while (length && ((uint8_t)caption[length] & 0xc0) == 0x80) length--;
        caption[length] = '\0';
        shortened = true;
    }
    if (shortened && length + 3 < sizeof(caption)) {
        memcpy(caption + length, "...", 4);
    }
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = caption; d.text_local = 1; d.text_static = 0;
    d.flag = LV_TEXT_FLAG_EXPAND;
    d.font = font; d.color = color;
    lv_area_t area = {x, y, x + width - 1, y + font->line_height - 1};
    lv_draw_label(layer, &d, &area);
}
static void tree_surface_refresh(void)
{
    const ui_view_state_t *v = view();
    const usb_evidence_snapshot_t *s = snapshot();
    if (!s_tree_canvas || !v) return;
    lv_obj_set_hidden(s_tree_canvas, s_tree_total == 0);
    if (!s_tree_total) return;
    int y_scroll = scroll_position(s_tree_view);
    lv_image_set_offset_y(s_tree_canvas, -y_scroll);
    if (s_tree_canvas_epoch == v->snapshot_epoch &&
        s_tree_canvas_collapsed == v->collapsed_nodes && s_tree_canvas_selected == v->node_index &&
        s_tree_canvas_frozen == v->frozen) return;
    s_tree_canvas_epoch = v->snapshot_epoch;
    s_tree_canvas_first = s_tree_first;
    s_tree_canvas_collapsed = v->collapsed_nodes;
    s_tree_canvas_selected = v->node_index;
    s_tree_canvas_frozen = v->frozen;
    lv_canvas_set_buffer(s_tree_canvas, s_tree_pixels, TREE_SURFACE_WIDTH,
        s_tree_total * DESC_ROW_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(s_tree_canvas, DESC_TREE_VIEW_W, DESC_TREE_VIEW_H);
    uint16_t color = lv_color_to_u16(C_SURFACE);
    for (size_t i = 0; i < (size_t)TREE_SURFACE_WIDTH * s_tree_total * DESC_ROW_H; i++) s_tree_pixels[i] = color;
    lv_layer_t layer;
    lv_canvas_init_layer(s_tree_canvas, &layer);
    for (unsigned ordinal = 0; ordinal < s_tree_total; ordinal++) {
        int index = visible_node_at((uint16_t)ordinal);
        if (!s || index < 0) continue;
        const usb_evidence_descriptor_node_t *node = &s->descriptor_nodes[index];
        int y = ordinal * DESC_ROW_H;
        bool selected = v->frozen && v->node_index == index;
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = selected ? C_SURFACE : C_RAISED;
        d.border_color = selected ? C_BLUE : C_DIVIDER;
        d.border_width = 1; d.radius = 7;
        lv_area_t row = {0, y, DESC_TREE_VIEW_W - 1, y + DESC_ROW_H - 2};
        lv_draw_rect(&layer, &d, &row);
        char title[128], safe[128], reference[64];
        snprintf(title, sizeof(title), "%.*s · %.*s",
            (int)sizeof(node->type_name), node->type_name, (int)sizeof(node->summary), node->summary);
        ui_font_sanitize_dynamic_text(title, safe, sizeof(safe));
        snprintf(reference, sizeof(reference), "事件 #%lu +%u:%u",
            (unsigned long)node->source_sequence, node->source_offset, node->byte_length);
        tree_surface_text(&layer, safe,
            DESC_TOGGLE_W + 6, y + 3, DESC_TREE_SELECT_W - 12, UI_FONT_16, selected ? C_BLUE : C_TEXT);
        tree_surface_text(&layer, reference,
            DESC_TOGGLE_W + 6, y + 25, DESC_TREE_SELECT_W - 12, UI_FONT_14, C_GRAY);
        tree_surface_text(&layer, node_has_children(s, node) ?
            (node_is_collapsed(v, (unsigned)index) ? "+" : "-") : "",
            19, y + 14, 16, UI_FONT_16, C_GRAY);
    }
    lv_canvas_finish_layer(s_tree_canvas, &layer);
    s_tree_canvas_renders++;
}

static const usb_evidence_snapshot_t *snapshot(void)
{
    return ui_model_snapshot();
}

static const ui_view_state_t *view(void)
{
    return ui_model_get();
}

static bool descriptor_hex_active(const ui_view_state_t *v)
{
    return s_page && lv_obj_is_visible(s_page) && v &&
           v->descriptor_tab == 1 && v->detail == UI_DETAIL_NONE;
}

static int32_t scroll_position(lv_obj_t *object)
{
    int32_t y = object ? lv_obj_get_scroll_y(object) : 0;
    return y > 0 ? y : 0;
}

static uint16_t visible_node_count(void)
{
    uint16_t i;
    uint16_t count = 0;

    for (i = 0; i < s_node_count; ++i)
        if (ui_model_node_visible(i)) ++count;
    return count;
}

static int visible_node_at(uint16_t ordinal)
{
    uint16_t i;
    uint16_t visible = 0;

    for (i = 0; i < s_node_count; ++i) {
        if (!ui_model_node_visible(i)) continue;
        if (visible++ == ordinal) return (int)i;
    }
    return DESC_NONE;
}

static const usb_evidence_raw_event_t *find_event(
    const usb_evidence_snapshot_t *s, uint32_t sequence)
{
    uint16_t i;
    uint16_t count;

    if (!s) return NULL;
    count = s->raw_event_count;
    if (count > USB_EVIDENCE_SNAPSHOT_EVENT_COUNT)
        count = USB_EVIDENCE_SNAPSHOT_EVENT_COUNT;
    for (i = 0; i < count; ++i)
        if (s->raw_events[i].sequence == sequence) return &s->raw_events[i];
    return NULL;
}

static bool node_has_children(const usb_evidence_snapshot_t *s,
                              const usb_evidence_descriptor_node_t *node)
{
    uint16_t i;

    if (!s || !node) return false;
    for (i = 0; i < s_node_count; ++i)
        if (s->descriptor_nodes[i].parent_id == node->node_id &&
            s->descriptor_nodes[i].node_id != node->node_id)
            return true;
    return false;
}

static bool node_is_collapsed(const ui_view_state_t *v, unsigned index)
{
    return v && index < 64U &&
           (v->collapsed_nodes & (UINT64_C(1) << index)) != 0U;
}

static void button_set_text(lv_obj_t *button, const char *text,
                            bool centered, int32_t width)
{
    lv_obj_t *label;

    if (!button) return;
    label = lv_obj_get_child(button, 0);
    if (!label) return;
    ui_label_set_safe(label, text ? text : "");
    lv_obj_set_width(label, width > 12 ? width - 12 : width);
    lv_obj_set_style_text_align(label,
                                centered ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT,
                                0);
    if (centered)
        lv_obj_center(label);
    else
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
}

static void tree_row_set_text(lv_obj_t *button, lv_obj_t *meta,
                              const char *primary, const char *reference,
                              int32_t row_width)
{
    /* Text pixels are rendered once into the tree surface. Updating invisible
     * labels would still run LVGL's expensive glyph layout on every scroll. */
    LV_UNUSED(primary); LV_UNUSED(reference); LV_UNUSED(row_width);
    if (button) {
        lv_obj_t *title = lv_obj_get_child(button, 0);
        if (title && lv_label_get_text(title)[0]) lv_label_set_text(title, "");
    }
    if (meta && lv_label_get_text(meta)[0]) lv_label_set_text(meta, "");
}

static lv_obj_t *virtual_button(lv_obj_t *parent, const char *text,
                                int32_t x, int32_t y,
                                int32_t width, ui_action_t action,
                                int32_t argument)
{
    lv_obj_t *button = ui_button(parent, text, width, action, argument);
    if (!button) return NULL;
    lv_obj_set_size(button, width, DESC_ROW_H);
    lv_obj_set_pos(button, x, y);
    /* Floating rows stay at viewport coordinates while the invisible extent
     * object alone controls scroll range. Release handlers validate their
     * current slot before posting the action through the deferred model. */
    lv_obj_set_floating(button, true);
    lv_obj_set_scrollable(button, false);
    return button;
}

/* The cached tree owns all text pixels. Hit targets need no label children,
 * theme transitions or per-state paint styles; their 48px geometry and real
 * release callbacks remain native LVGL objects. */
static lv_obj_t *tree_hitbox(lv_obj_t *parent, int32_t x, int32_t y, int32_t width)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_add_style(button, &style_transp, 0);
    lv_obj_set_size(button, width, DESC_ROW_H);
    lv_obj_set_pos(button, x, y);
    /* Native scroll moves the hit targets. Reassign content coordinates only
     * when their visible-row mapping changes, not every scroll pixel. */
    lv_obj_set_floating(button, false);
    lv_obj_set_scrollable(button, false);
    return button;
}

static uint16_t field_count_for_node(const usb_evidence_descriptor_node_t *node)
{
    uint16_t available;
    uint16_t count;

    if (!node || node->field_start > s_field_count) return 0;
    available = (uint16_t)(s_field_count - node->field_start);
    count = node->field_count;
    return count < available ? count : available;
}

static bool clicked_button(lv_event_t *event, lv_obj_t **target_out)
{
    lv_obj_t *target;

    /* LV_EVENT_CLICKED is emitted after an in-bounds press/release pair. */
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return false;
    target = lv_event_get_current_target(event);
    if (!target || lv_obj_is_hidden(target)) return false;
    if (target_out) *target_out = target;
    return true;
}

static void post_node_release(lv_event_t *event)
{
    uintptr_t slot = (uintptr_t)lv_event_get_user_data(event);
    lv_obj_t *target;
    intptr_t index;

    if (!s_create_ready || !clicked_button(event, &target) ||
        slot >= DESC_TREE_SLOTS) return;
    index = (intptr_t)lv_obj_get_user_data(target);
    if (index < 0 || index >= s_node_count ||
        s_tree_node[slot] != index ||
        visible_node_at((uint16_t)(s_tree_first + slot)) != index)
        return;
    ui_model_post(UI_ACTION_NODE, (int32_t)index);
}

static void post_toggle_release(lv_event_t *event)
{
    const usb_evidence_snapshot_t *s;
    uintptr_t slot = (uintptr_t)lv_event_get_user_data(event);
    lv_obj_t *target;
    intptr_t index;

    if (!s_create_ready) return;
    s = snapshot();
    if (!clicked_button(event, &target) || slot >= DESC_TREE_SLOTS || !s)
        return;
    index = (intptr_t)lv_obj_get_user_data(target);
    if (index < 0 || index >= s_node_count ||
        s_tree_node[slot] != index ||
        visible_node_at((uint16_t)(s_tree_first + slot)) != index ||
        !node_has_children(s, &s->descriptor_nodes[index]))
        return;
    ui_model_post(UI_ACTION_NODE_TOGGLE, (int32_t)index);
}

static void post_field_release(lv_event_t *event)
{
    const usb_evidence_snapshot_t *s;
    const ui_view_state_t *v;
    uintptr_t slot = (uintptr_t)lv_event_get_user_data(event);
    lv_obj_t *target;
    intptr_t index;
    const usb_evidence_descriptor_node_t *node;
    uint16_t count;
    uint32_t expected;

    if (!s_create_ready) return;
    s = snapshot();
    v = view();
    if (!clicked_button(event, &target) || slot >= DESC_FIELD_SLOTS ||
        !s || !v || v->descriptor_tab == 1 ||
        s_selected_node < 0 || s_selected_node >= s_node_count ||
        v->node_index != s_selected_node)
        return;
    index = (intptr_t)lv_obj_get_user_data(target);
    node = &s->descriptor_nodes[s_selected_node];
    count = field_count_for_node(node);
    expected = (uint32_t)node->field_start + s_field_first + slot;
    if (index < 0 || index >= s_field_count ||
        (uint32_t)index != expected || s_field_global[slot] != index ||
        s_field_first + slot >= count)
        return;
    ui_model_post(UI_ACTION_FIELD, (int32_t)index);
}

static void set_extent(lv_obj_t *extent, lv_obj_t *viewport,
                       uint16_t row_count, int32_t viewport_height)
{
    uint32_t content_height = (uint32_t)row_count * DESC_ROW_H;

    if (!extent || !viewport) return;
    if (content_height < (uint32_t)viewport_height)
        content_height = (uint32_t)viewport_height;
    lv_obj_set_size(extent, lv_obj_get_width(viewport), (int32_t)content_height);
}

static void render_tree(void)
{
    const usb_evidence_snapshot_t *s = snapshot();
    const ui_view_state_t *v = view();
    int32_t y_scroll = scroll_position(s_tree_view);
    int32_t view_w;
    int32_t view_h;
    int32_t select_w;
    uint32_t epoch = v ? v->snapshot_epoch : 0U;
    uint16_t model_node_count = s_node_count;
    uint64_t collapsed_nodes = v ? v->collapsed_nodes : 0U;
    int16_t selected_node = v ? v->node_index : DESC_NONE;
    bool frozen = v && v->frozen;
    bool first_render;
    bool visibility_changed;
    bool selection_changed;
    bool scroll_changed;
    bool width_changed;
    bool height_changed;
    uint16_t total;
    int32_t max_y;
    uint16_t first;
    uint16_t slot;

    if (!s_tree_view || !s_tree_extent || s_tree_rendering) return;
    view_w = lv_obj_get_width(s_tree_view);
    view_h = lv_obj_get_height(s_tree_view);
    if (view_w < 1) view_w = DESC_TREE_VIEW_W;
    if (view_h < 1) view_h = DESC_TREE_VIEW_H;
    select_w = view_w - DESC_TOGGLE_W;
    if (select_w < 1) select_w = 1;

    first_render = !s_tree_render_valid;
    visibility_changed = first_render || epoch != s_tree_cached_epoch ||
        model_node_count != s_tree_cached_node_count ||
        collapsed_nodes != s_tree_cached_collapsed;
    selection_changed = first_render || selected_node != s_tree_cached_selected ||
        frozen != s_tree_cached_frozen;
    scroll_changed = first_render || y_scroll != s_tree_cached_scroll_y;
    width_changed = first_render || view_w != s_tree_cached_view_w;
    height_changed = first_render || view_h != s_tree_cached_view_h;
    if (!visibility_changed && !selection_changed && !scroll_changed &&
        !width_changed && !height_changed)
        return;

    s_tree_rendering = true;
    total = visibility_changed ? visible_node_count() : s_tree_total;
    max_y = (int32_t)total * DESC_ROW_H - view_h;
    if (max_y < 0) max_y = 0;
    if (y_scroll > max_y) {
        y_scroll = max_y;
        lv_obj_scroll_to_y(s_tree_view, y_scroll, LV_ANIM_OFF);
    }
    first = (uint16_t)(y_scroll / DESC_ROW_H);
    if (first_render || total != s_tree_total || width_changed || height_changed)
        set_extent(s_tree_extent, s_tree_view, total, view_h);

    if (s_tree_status && (first_render || total != s_tree_total ||
                          model_node_count != s_tree_status_node_count)) {
        char status[48];
        rt_snprintf(status, sizeof(status), "%u / %u",
                    (unsigned)total, (unsigned)model_node_count);
        ui_label_set_safe(s_tree_status, status);
        s_tree_status_node_count = model_node_count;
    }

    s_tree_total = total;
    s_tree_first = first;

    for (slot = 0; slot < DESC_TREE_SLOTS; ++slot) {
        descriptor_tree_slot_cache_t *cache = &s_tree_slot_cache[slot];
        int index = !visibility_changed && first == s_tree_cached_first ?
            (cache->visible ? cache->node_index : DESC_NONE) :
            visible_node_at((uint16_t)(first + slot));
        int32_t row_y = (int32_t)(first + slot) * DESC_ROW_H;

        if (index < 0 || !s || index >= s_node_count) {
            if (cache->visible) {
                lv_obj_set_hidden(s_tree_toggle[slot], true);
                lv_obj_set_hidden(s_tree_select[slot], true);
                s_tree_node[slot] = DESC_NONE;
                lv_obj_set_user_data(s_tree_toggle[slot], NULL);
                lv_obj_set_user_data(s_tree_select[slot], NULL);
            }
            cache->visible = false;
            cache->valid = true;
            cache->node_index = DESC_NONE;
            cache->epoch = epoch;
            continue;
        }

        if (!s_tree_toggle[slot] || !s_tree_select[slot]) continue;
        {
            const usb_evidence_descriptor_node_t *node = &s->descriptor_nodes[index];
            bool mapping_changed = !cache->valid || !cache->visible ||
                                   cache->node_index != index;
            bool epoch_changed = !cache->valid || cache->epoch != epoch;
            bool row_width_changed = !cache->valid || cache->width != select_w;
            bool position_changed = !cache->valid || !cache->visible ||
                                    cache->y != row_y;
            bool selected = frozen && selected_node == index;
            bool collapsed = node_is_collapsed(v, (unsigned)index);
            bool expandable = mapping_changed || epoch_changed ?
                node_has_children(s, node) : cache->expandable;
            bool toggle_text_changed = mapping_changed || epoch_changed ||
                cache->collapsed != collapsed || cache->expandable != expandable;
            bool selected_changed = mapping_changed || cache->selected != selected;

            if (!cache->visible) {
                lv_obj_set_hidden(s_tree_toggle[slot], false);
                lv_obj_set_hidden(s_tree_select[slot], false);
            }
            if (mapping_changed) {
                s_tree_node[slot] = (int16_t)index;
                lv_obj_set_user_data(s_tree_toggle[slot], (void *)(intptr_t)index);
                lv_obj_set_user_data(s_tree_select[slot], (void *)(intptr_t)index);
            }
            if (row_width_changed) {
                lv_obj_set_size(s_tree_toggle[slot], DESC_TOGGLE_W, DESC_ROW_H);
                lv_obj_set_size(s_tree_select[slot], select_w, DESC_ROW_H);
            }
            if (position_changed) {
                lv_obj_set_pos(s_tree_toggle[slot], 0, row_y);
                lv_obj_set_pos(s_tree_select[slot], DESC_TOGGLE_W, row_y);
            }

            if (mapping_changed || epoch_changed || row_width_changed) {
                char indent[17];
                char text[112];
                char reference[64];
                unsigned depth = node->depth > 4U ? 4U : node->depth;

                memset(indent, ' ', depth * 2U);
                indent[depth * 2U] = '\0';
                rt_snprintf(text, sizeof(text), "%s%.*s · %.*s", indent,
                            (int)sizeof(node->type_name), node->type_name,
                            (int)sizeof(node->summary), node->summary);
                rt_snprintf(reference, sizeof(reference), "事件 #%lu +%u:%u",
                            (unsigned long)node->source_sequence,
                            (unsigned)node->source_offset,
                            (unsigned)node->byte_length);
                tree_row_set_text(s_tree_select[slot], s_tree_meta[slot],
                                  text, reference, select_w);
                s_tree_text_updates++;
            }

            if (toggle_text_changed) {
                button_set_text(s_tree_toggle[slot],
                                expandable ? (collapsed ? "+" : "-") : "",
                                true, DESC_TOGGLE_W);
                s_tree_text_updates++;
            }
            if (!cache->valid || cache->expandable != expandable) {
                if (expandable)
                    lv_obj_remove_state(s_tree_toggle[slot], LV_STATE_DISABLED);
                else
                    lv_obj_add_state(s_tree_toggle[slot], LV_STATE_DISABLED);
            }
            if (selected_changed)
                ui_button_set_selected(s_tree_select[slot], selected);

            cache->node_index = (int16_t)index;
            cache->y = row_y;
            cache->width = select_w;
            cache->epoch = epoch;
            cache->visible = true;
            cache->selected = selected;
            cache->collapsed = collapsed;
            cache->expandable = expandable;
            cache->valid = true;
        }
    }

    s_tree_cached_epoch = epoch;
    s_tree_cached_node_count = model_node_count;
    s_tree_cached_collapsed = collapsed_nodes;
    s_tree_cached_selected = selected_node;
    s_tree_cached_frozen = frozen;
    s_tree_cached_scroll_y = y_scroll;
    s_tree_cached_first = first;
    s_tree_cached_view_w = view_w;
    s_tree_cached_view_h = view_h;
    s_tree_render_valid = true;
    s_tree_refreshes++;
    tree_surface_refresh();
    s_tree_rendering = false;
}

static void render_fields(void)
{
    const usb_evidence_snapshot_t *s = snapshot();
    const ui_view_state_t *v = view();
    const usb_evidence_descriptor_node_t *node = NULL;
    uint16_t count = 0;
    int32_t y_scroll = scroll_position(s_fields);
    int32_t max_y;
    int32_t view_w;
    int32_t view_h;
    uint32_t epoch = v ? v->snapshot_epoch : 0U;
    uint16_t model_field_count = s_field_count;
    int16_t selected_field = v && v->detail == UI_DETAIL_FIELD ?
        v->field_index : DESC_NONE;
    uint8_t tab = v ? v->descriptor_tab : 0U;
    bool first_render;
    bool content_changed;
    bool selection_changed;
    bool tab_changed;
    bool scroll_changed;
    bool size_changed;
    uint16_t first;
    int16_t remainder;
    uint16_t slot;

    if (!s_fields || !s_field_extent || s_field_rendering) return;
    if (s && s_selected_node >= 0 && s_selected_node < s_node_count) {
        node = &s->descriptor_nodes[s_selected_node];
        count = field_count_for_node(node);
    }
    view_w = lv_obj_get_width(s_fields);
    view_h = lv_obj_get_height(s_fields);
    if (view_w < 1) view_w = DESC_DETAIL_BODY_W;
    if (view_h < 1) view_h = DESC_DETAIL_BODY_H;

    first_render = !s_field_render_valid;
    content_changed = first_render || epoch != s_field_cached_epoch ||
        model_field_count != s_field_cached_node_count ||
        s_selected_node != s_field_cached_node || count != s_field_cached_total;
    selection_changed = first_render || selected_field != s_field_cached_selection;
    tab_changed = first_render || tab != s_field_cached_tab;
    scroll_changed = first_render || y_scroll != s_field_cached_scroll_y;
    size_changed = first_render || view_w != s_field_cached_view_w ||
                   view_h != s_field_cached_view_h;
    if (!content_changed && !selection_changed && !tab_changed &&
        !scroll_changed && !size_changed)
        return;

    s_field_rendering = true;
    max_y = (int32_t)count * DESC_ROW_H - view_h;
    if (max_y < 0) max_y = 0;
    if (y_scroll > max_y) {
        y_scroll = max_y;
        lv_obj_scroll_to_y(s_fields, y_scroll, LV_ANIM_OFF);
    }
    first = (uint16_t)(y_scroll / DESC_ROW_H);
    remainder = (int16_t)(y_scroll % DESC_ROW_H);
    s_field_first = first;
    if (first_render || count != s_field_cached_total || size_changed)
        set_extent(s_field_extent, s_fields, count, view_h);
    if (s_empty) {
        if (count == 0U && tab != 1U && content_changed)
            ui_label_set_safe(s_empty, node ? "该节点没有可显示字段" :
                                           "选择左侧描述符查看字段");
        bool empty_hidden = tab == 1U || count != 0U;
        if (lv_obj_is_hidden(s_empty) != empty_hidden)
            lv_obj_set_hidden(s_empty, empty_hidden);
    }

    for (slot = 0; slot < DESC_FIELD_SLOTS; ++slot) {
        descriptor_field_slot_cache_t *cache = &s_field_slot_cache[slot];
        uint32_t field_index;
        int32_t row_y = (int32_t)slot * DESC_ROW_H - remainder;
        bool mapping_changed;
        bool epoch_changed;
        bool row_width_changed;
        bool position_changed;
        bool selected;

        if (!content_changed && first == s_field_cached_first)
            field_index = cache->visible ? (uint32_t)cache->field_index : UINT32_MAX;
        else
            field_index = (uint32_t)first + slot < count && node ?
                (uint32_t)node->field_start + first + slot : UINT32_MAX;

        if (field_index == UINT32_MAX || field_index >= s_field_count || !s || !node) {
            if (cache->visible && s_field_rows[slot])
                lv_obj_set_hidden(s_field_rows[slot], true);
            if (s_field_global[slot] != DESC_NONE) {
                s_field_global[slot] = DESC_NONE;
                if (s_field_rows[slot])
                    lv_obj_set_user_data(s_field_rows[slot], NULL);
            }
            cache->field_index = DESC_NONE;
            cache->epoch = epoch;
            cache->visible = false;
            cache->valid = true;
            continue;
        }

        if (!s_field_rows[slot]) continue;
        mapping_changed = !cache->valid || !cache->visible ||
                          cache->field_index != (int16_t)field_index;
        epoch_changed = !cache->valid || cache->epoch != epoch;
        row_width_changed = !cache->valid || cache->width != view_w;
        position_changed = !cache->valid || !cache->visible || cache->y != row_y;
        selected = selected_field == (int16_t)field_index;

        if (mapping_changed) {
            s_field_global[slot] = (int16_t)field_index;
            lv_obj_set_user_data(s_field_rows[slot],
                                 (void *)(intptr_t)field_index);
        }
        if (!cache->visible) lv_obj_set_hidden(s_field_rows[slot], false);
        if (row_width_changed)
            lv_obj_set_size(s_field_rows[slot], view_w, DESC_ROW_H);
        if (position_changed) lv_obj_set_pos(s_field_rows[slot], 0, row_y);

        if (mapping_changed || epoch_changed || row_width_changed) {
            const usb_evidence_descriptor_field_t *field =
                &s->descriptor_fields[field_index];
            char text[192];
            char raw_value[sizeof(field->value) + 1U];
            char value_preview[73];
            memcpy(raw_value, field->value, sizeof(field->value));
            raw_value[sizeof(field->value)] = '\0';
            (void)work_text_preview_copy(value_preview, sizeof(value_preview),
                                         raw_value, 1);
            rt_snprintf(text, sizeof(text), "%.*s   +%u / %u B   %s",
                        (int)sizeof(field->name), field->name,
                        (unsigned)field->byte_offset,
                        (unsigned)field->byte_length, value_preview);
            button_set_text(s_field_rows[slot], text, false, view_w);
            s_field_text_updates++;
        }
        if (mapping_changed || cache->selected != selected)
            ui_button_set_selected(s_field_rows[slot], selected);

        cache->field_index = (int16_t)field_index;
        cache->y = row_y;
        cache->width = view_w;
        cache->epoch = epoch;
        cache->visible = true;
        cache->selected = selected;
        cache->valid = true;
    }

    s_field_cached_epoch = epoch;
    s_field_cached_node_count = model_field_count;
    s_field_cached_total = count;
    s_field_cached_node = s_selected_node;
    s_field_cached_selection = selected_field;
    s_field_cached_tab = tab;
    s_field_cached_scroll_y = y_scroll;
    s_field_cached_first = first;
    s_field_cached_view_w = view_w;
    s_field_cached_view_h = view_h;
    s_field_render_valid = true;
    s_field_refreshes++;
    s_field_rendering = false;
}

static void render_hex(void)
{
    const usb_evidence_snapshot_t *s = snapshot();
    const ui_view_state_t *v = view();
    const usb_evidence_descriptor_node_t *node;
    const usb_evidence_raw_event_t *event;

    if (!s_hex || !descriptor_hex_active(v)) return;
    if (!s || s_selected_node < 0 || s_selected_node >= s_node_count) {
        ui_hex_set(s_hex, NULL, 0, 0, false);
        return;
    }

    node = &s->descriptor_nodes[s_selected_node];
    event = find_event(s, node->source_sequence);
    if (!event || !ui_model_range_valid(node->source_sequence,
                                        node->source_offset,
                                        node->byte_length)) {
        ui_hex_set(s_hex, NULL, 0, 0, false);
        return;
    }

    /* Pass the snapshot event and its range directly. The shared HEX widget
     * owns the visible-canvas cache; this screen never copies or parses bytes. */
    ui_hex_set(s_hex, event, node->source_offset, node->byte_length, false);
}

static void tree_scroll_event(lv_event_t *event)
{
    LV_UNUSED(event);
    if (!s_create_ready) return;
    render_tree();
}

static void field_scroll_event(lv_event_t *event)
{
    LV_UNUSED(event);
    if (!s_create_ready) return;
    render_fields();
}

lv_obj_t *scr_descriptors_create_begin(lv_obj_t *parent)
{
    if (s_page) return s_page;

    s_create_ready = false;
    s_create_phase = DESCRIPTORS_CREATE_TREE_PANEL;
    s_create_index = 0U;
    s_create_units_done = 0U;
    s_page = lv_obj_create(parent);
    if (!s_page) return NULL;
    lv_obj_set_hidden(s_page, true);
    lv_obj_set_size(s_page, LV_PCT(100), DESC_PAGE_H);
    lv_obj_set_style_bg_color(s_page, C_BG, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, DESC_PAGE_PAD, 0);
    lv_obj_set_style_pad_gap(s_page, DESC_PANE_GAP, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_ROW);
    return s_page;
}

bool scr_descriptors_create_step(void)
{
    uint16_t slot;

    if (s_create_ready) return true;
    if (!s_page) return false;

    switch (s_create_phase) {
    case DESCRIPTORS_CREATE_TREE_PANEL:
        s_tree = ui_panel(s_page, 0, 0, DESC_TREE_W, DESC_PANE_H);
        lv_obj_set_size(s_tree, DESC_TREE_W, DESC_PANE_H);
        lv_obj_set_layout(s_tree, LV_LAYOUT_NONE);
        lv_obj_set_style_pad_all(s_tree, 0, 0);
        s_create_phase = DESCRIPTORS_CREATE_TREE_HEADING;
        break;
    case DESCRIPTORS_CREATE_TREE_HEADING:
        s_tree_heading = ui_text(s_tree, "描述符树", UI_FONT_20, C_TEXT);
        lv_obj_set_pos(s_tree_heading, DESC_TREE_INSET, 8);
        s_create_phase = DESCRIPTORS_CREATE_TREE_STATUS;
        break;
    case DESCRIPTORS_CREATE_TREE_STATUS:
        s_tree_status = ui_text(s_tree, "0 / 0", UI_FONT_14, C_GRAY);
        lv_obj_set_pos(s_tree_status, DESC_TREE_INSET, 32);
        lv_obj_set_width(s_tree_status, DESC_TREE_VIEW_W);
        s_create_phase = DESCRIPTORS_CREATE_TREE_VIEW;
        break;
    case DESCRIPTORS_CREATE_TREE_VIEW:
        s_tree_view = lv_obj_create(s_tree);
        lv_obj_set_pos(s_tree_view, DESC_TREE_INSET, DESC_TREE_VIEW_Y);
        lv_obj_set_size(s_tree_view, DESC_TREE_VIEW_W, DESC_TREE_VIEW_H);
        lv_obj_set_style_bg_opa(s_tree_view, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_tree_view, 0, 0);
        lv_obj_set_style_pad_all(s_tree_view, 0, 0);
        lv_obj_set_scroll_dir(s_tree_view, LV_DIR_VER);
        lv_obj_set_scrollable(s_tree_view, true);
        lv_obj_set_scrollbar_mode(s_tree_view, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_add_event_cb(s_tree_view, tree_scroll_event, LV_EVENT_SCROLL, NULL);
        s_create_phase = DESCRIPTORS_CREATE_TREE_EXTENT;
        break;
    case DESCRIPTORS_CREATE_TREE_EXTENT:
        s_tree_extent = lv_obj_create(s_tree_view);
        lv_obj_set_pos(s_tree_extent, 0, 0);
        lv_obj_set_style_bg_opa(s_tree_extent, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_tree_extent, 0, 0);
        lv_obj_set_style_pad_all(s_tree_extent, 0, 0);
        lv_obj_set_clickable(s_tree_extent, false);
        lv_obj_set_scrollable(s_tree_extent, false);
        s_create_phase = DESCRIPTORS_CREATE_TREE_CANVAS;
        break;
    case DESCRIPTORS_CREATE_TREE_CANVAS:
        s_tree_canvas = lv_canvas_create(s_tree_view);
        lv_canvas_set_buffer(s_tree_canvas, s_tree_pixels, TREE_SURFACE_WIDTH,
                             DESC_TREE_SLOTS * DESC_ROW_H, LV_COLOR_FORMAT_RGB565);
        lv_image_set_inner_align(s_tree_canvas, LV_IMAGE_ALIGN_TOP_LEFT);
        lv_obj_set_size(s_tree_canvas, DESC_TREE_VIEW_W, DESC_TREE_VIEW_H);
        lv_obj_set_floating(s_tree_canvas, true);
        lv_obj_set_scrollable(s_tree_canvas, false);
        lv_obj_set_clickable(s_tree_canvas, false);
        lv_obj_set_scroll_chain(s_tree_canvas, true);
        lv_obj_set_hidden(s_tree_canvas, true);
        s_create_phase = DESCRIPTORS_CREATE_TREE_TOGGLE;
        s_create_index = 0U;
        break;
    case DESCRIPTORS_CREATE_TREE_TOGGLE:
        slot = s_create_index;
        s_tree_node[slot] = DESC_NONE;
        s_tree_toggle[slot] = tree_hitbox(
            s_tree_view, 0, (int32_t)slot * DESC_ROW_H, DESC_TOGGLE_W);
        lv_obj_set_hidden(s_tree_toggle[slot], true);
        lv_obj_add_event_cb(s_tree_toggle[slot], post_toggle_release,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)slot);
        lv_obj_set_style_bg_opa(s_tree_toggle[slot], LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(s_tree_toggle[slot], LV_OPA_TRANSP,
                                LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s_tree_toggle[slot], LV_OPA_TRANSP,
                                LV_STATE_DISABLED);
        lv_obj_set_style_border_opa(s_tree_toggle[slot], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(s_tree_toggle[slot], LV_OPA_TRANSP,
                                    LV_STATE_DISABLED);
        s_create_phase = DESCRIPTORS_CREATE_TREE_SELECT;
        break;
    case DESCRIPTORS_CREATE_TREE_SELECT:
        slot = s_create_index;
        s_tree_select[slot] = tree_hitbox(
            s_tree_view, DESC_TOGGLE_W, (int32_t)slot * DESC_ROW_H,
            DESC_TREE_SELECT_W);
        lv_obj_set_hidden(s_tree_select[slot], true);
        lv_obj_add_event_cb(s_tree_select[slot], post_node_release,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)slot);
        lv_obj_set_style_bg_opa(s_tree_select[slot], LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(s_tree_select[slot], LV_OPA_TRANSP,
                                LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s_tree_select[slot], LV_OPA_TRANSP,
                                LV_STATE_DISABLED);
        lv_obj_set_style_border_opa(s_tree_select[slot], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(s_tree_select[slot], LV_OPA_TRANSP,
                                    LV_STATE_DISABLED);
        s_tree_meta[slot] = NULL;
        if (++s_create_index >= DESC_TREE_SLOTS) {
            s_create_index = 0U;
            s_create_phase = DESCRIPTORS_CREATE_DETAIL_PANEL;
        } else {
            s_create_phase = DESCRIPTORS_CREATE_TREE_TOGGLE;
        }
        break;
    case DESCRIPTORS_CREATE_DETAIL_PANEL:
        s_detail = ui_panel(s_page, 0, 0, DESC_DETAIL_W, DESC_PANE_H);
        lv_obj_set_size(s_detail, DESC_DETAIL_W, DESC_PANE_H);
        lv_obj_set_layout(s_detail, LV_LAYOUT_NONE);
        lv_obj_set_style_pad_all(s_detail, 0, 0);
        s_create_phase = DESCRIPTORS_CREATE_DETAIL_HEADING;
        break;
    case DESCRIPTORS_CREATE_DETAIL_HEADING:
        s_detail_heading = ui_text(s_detail, "描述符内容", UI_FONT_20, C_TEXT);
        lv_obj_set_pos(s_detail_heading, DESC_DETAIL_INSET, 8);
        s_create_phase = DESCRIPTORS_CREATE_TABS_BOX;
        break;
    case DESCRIPTORS_CREATE_TABS_BOX:
        s_tabs_box = lv_obj_create(s_detail);
        lv_obj_set_pos(s_tabs_box, DESC_DETAIL_INSET, 38);
        lv_obj_set_size(s_tabs_box, DESC_DETAIL_BODY_W, DESC_ROW_H);
        lv_obj_set_style_bg_opa(s_tabs_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_tabs_box, 0, 0);
        lv_obj_set_style_pad_all(s_tabs_box, 0, 0);
        lv_obj_set_style_pad_gap(s_tabs_box, DESC_PANE_GAP, 0);
        lv_obj_set_flex_flow(s_tabs_box, LV_FLEX_FLOW_ROW);
        s_create_phase = DESCRIPTORS_CREATE_TAB_FIELDS;
        break;
    case DESCRIPTORS_CREATE_TAB_FIELDS:
        s_tabs[0] = ui_button(s_tabs_box, "字段", 246,
                              UI_ACTION_DESCRIPTOR_TAB, 0);
        lv_obj_set_size(s_tabs[0], 246, DESC_ROW_H);
        s_create_phase = DESCRIPTORS_CREATE_TAB_HEX;
        break;
    case DESCRIPTORS_CREATE_TAB_HEX:
        s_tabs[1] = ui_button(s_tabs_box, "HEX", 246,
                              UI_ACTION_DESCRIPTOR_TAB, 1);
        lv_obj_set_size(s_tabs[1], 246, DESC_ROW_H);
        s_create_phase = DESCRIPTORS_CREATE_FIELDS;
        break;
    case DESCRIPTORS_CREATE_FIELDS:
        s_fields = lv_obj_create(s_detail);
        lv_obj_set_pos(s_fields, DESC_DETAIL_INSET, DESC_DETAIL_BODY_Y);
        lv_obj_set_size(s_fields, DESC_DETAIL_BODY_W, DESC_DETAIL_BODY_H);
        lv_obj_set_style_bg_opa(s_fields, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_fields, 0, 0);
        lv_obj_set_style_pad_all(s_fields, 0, 0);
        lv_obj_set_scroll_dir(s_fields, LV_DIR_VER);
        lv_obj_set_scrollable(s_fields, true);
        lv_obj_set_scrollbar_mode(s_fields, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_add_event_cb(s_fields, field_scroll_event, LV_EVENT_SCROLL, NULL);
        s_create_phase = DESCRIPTORS_CREATE_FIELD_EXTENT;
        break;
    case DESCRIPTORS_CREATE_FIELD_EXTENT:
        s_field_extent = lv_obj_create(s_fields);
        lv_obj_set_pos(s_field_extent, 0, 0);
        lv_obj_set_style_bg_opa(s_field_extent, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_field_extent, 0, 0);
        lv_obj_set_style_pad_all(s_field_extent, 0, 0);
        lv_obj_set_clickable(s_field_extent, false);
        lv_obj_set_scrollable(s_field_extent, false);
        s_create_phase = DESCRIPTORS_CREATE_FIELD_ROW;
        s_create_index = 0U;
        break;
    case DESCRIPTORS_CREATE_FIELD_ROW:
        slot = s_create_index;
        s_field_global[slot] = DESC_NONE;
        s_field_rows[slot] = virtual_button(
            s_fields, "", 0, (int32_t)slot * DESC_ROW_H,
            DESC_DETAIL_BODY_W, UI_ACTION_FIELD, 0);
        lv_obj_set_hidden(s_field_rows[slot], true);
        (void)lv_obj_remove_event(s_field_rows[slot], 0);
        lv_obj_add_event_cb(s_field_rows[slot], post_field_release,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)slot);
        if (++s_create_index >= DESC_FIELD_SLOTS) {
            s_create_phase = DESCRIPTORS_CREATE_EMPTY;
        }
        break;
    case DESCRIPTORS_CREATE_EMPTY:
        s_empty = ui_text(s_detail, "选择左侧描述符查看字段", UI_FONT_16, C_GRAY);
        lv_obj_set_pos(s_empty, DESC_DETAIL_INSET + 12, DESC_DETAIL_BODY_Y + 16);
        lv_obj_set_clickable(s_empty, false);
        s_create_phase = DESCRIPTORS_CREATE_HEX;
        break;
    case DESCRIPTORS_CREATE_HEX:
        s_hex = ui_panel(s_detail, DESC_DETAIL_INSET, DESC_DETAIL_BODY_Y,
                         DESC_DETAIL_BODY_W, DESC_DETAIL_BODY_H);
        lv_obj_set_size(s_hex, DESC_DETAIL_BODY_W, DESC_DETAIL_BODY_H);
        lv_obj_set_style_bg_color(s_hex, C_BG, 0);
        lv_obj_set_style_pad_all(s_hex, 0, 0);
        lv_obj_set_hidden(s_hex, true);
        s_create_ready = true;
        break;
    }

    ++s_create_units_done;
    return s_create_ready;
}

bool scr_descriptors_is_ready(void)
{
    return s_create_ready;
}

unsigned scr_descriptors_create_progress(void)
{
    if (s_create_ready) return 100U;
    return (s_create_units_done * 100U) / DESCRIPTORS_CREATE_STEP_COUNT;
}

lv_obj_t *scr_descriptors_create(lv_obj_t *parent)
{
    lv_obj_t *page = scr_descriptors_create_begin(parent);

    if (!page) return NULL;
    while (!scr_descriptors_create_step()) {
    }
    scr_descriptors_refresh();
    lv_obj_set_hidden(page, false);
    return page;
}

void scr_descriptors_refresh(void)
{
    const usb_evidence_snapshot_t *s;
    const ui_view_state_t *v;
    int16_t selected = DESC_NONE;

    if (!s_create_ready) return;
    s = snapshot();
    v = view();
    if (!s) {
        scr_descriptors_clear();
        return;
    }

    s_node_count = s->descriptor_node_count;
    if (s_node_count > USB_EVIDENCE_DESCRIPTOR_NODE_COUNT)
        s_node_count = USB_EVIDENCE_DESCRIPTOR_NODE_COUNT;
    s_field_count = s->descriptor_field_count;
    if (s_field_count > USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT)
        s_field_count = USB_EVIDENCE_DESCRIPTOR_FIELD_COUNT;
    if (v && v->node_index >= 0 && v->node_index < s_node_count)
        selected = v->node_index;

    if (selected != s_selected_node) {
        s_selected_node = selected;
        if (s_fields) lv_obj_scroll_to_y(s_fields, 0, LV_ANIM_OFF);
    }
    if (v && v->descriptor_tab == 1) {
        if (s_fields) lv_obj_set_hidden(s_fields, true);
        if (s_hex) lv_obj_set_hidden(s_hex, false);
    } else {
        if (s_fields) lv_obj_set_hidden(s_fields, false);
        if (s_hex) lv_obj_set_hidden(s_hex, true);
    }
    if (s_tabs[0]) ui_button_set_selected(s_tabs[0], !v || v->descriptor_tab != 1);
    if (s_tabs[1]) ui_button_set_selected(s_tabs[1], v && v->descriptor_tab == 1);

    render_tree();
    render_fields();
    if (descriptor_hex_active(v)) render_hex();
}

void scr_descriptors_update_snapshot(const usb_evidence_snapshot_t *snapshot_arg)
{
    LV_UNUSED(snapshot_arg);
    scr_descriptors_refresh();
}

void scr_descriptors_clear(void)
{
    uint16_t slot;

    if (!s_create_ready) return;
    s_node_count = 0;
    s_field_count = 0;
    s_tree_total = 0;
    s_tree_first = 0;
    s_field_first = 0;
    s_selected_node = DESC_NONE;
    s_scroll_test = RT_FALSE;
    s_test_y = 0;
    s_test_dir = 1;
    s_bench_tick_initialized = false;
    s_tree_render_valid = false;
    s_field_render_valid = false;
    s_tree_rendering = false;
    s_tree_canvas_epoch = UINT32_MAX;
    if (s_tree_canvas) lv_obj_set_hidden(s_tree_canvas, true);
    s_field_rendering = false;

    if (!s_tree_view) return;
    lv_obj_scroll_to_y(s_tree_view, 0, LV_ANIM_OFF);
    if (s_fields) lv_obj_scroll_to_y(s_fields, 0, LV_ANIM_OFF);
    set_extent(s_tree_extent, s_tree_view, 0, DESC_TREE_VIEW_H);
    set_extent(s_field_extent, s_fields, 0, DESC_DETAIL_BODY_H);
    for (slot = 0; slot < DESC_TREE_SLOTS; ++slot) {
        if (s_tree_toggle[slot])
            lv_obj_set_hidden(s_tree_toggle[slot], true);
        if (s_tree_select[slot])
            lv_obj_set_hidden(s_tree_select[slot], true);
        s_tree_node[slot] = DESC_NONE;
        if (s_tree_toggle[slot])
            lv_obj_set_user_data(s_tree_toggle[slot], NULL);
        if (s_tree_select[slot])
            lv_obj_set_user_data(s_tree_select[slot], NULL);
        s_tree_slot_cache[slot].valid = false;
        s_tree_slot_cache[slot].visible = false;
    }
    for (slot = 0; slot < DESC_FIELD_SLOTS; ++slot) {
        if (s_field_rows[slot])
            lv_obj_set_hidden(s_field_rows[slot], true);
        s_field_global[slot] = DESC_NONE;
        if (s_field_rows[slot])
            lv_obj_set_user_data(s_field_rows[slot], NULL);
        s_field_slot_cache[slot].valid = false;
        s_field_slot_cache[slot].visible = false;
    }
    if (s_empty) lv_obj_set_hidden(s_empty, false);
    if (s_hex) {
        lv_obj_set_hidden(s_hex, true);
        render_hex();
    }
}

void scr_descriptors_set_scroll_test(rt_bool_t enable)
{
    bool was_enabled;

    if (!s_create_ready) return;
    was_enabled = s_scroll_test != RT_FALSE;
    s_scroll_test = enable;
    if (!enable) {
        s_bench_tick_initialized = false;
        return;
    }
    if (was_enabled) return;

    s_bench_updates = 0;
    s_tree_refreshes = 0;
    s_tree_text_updates = 0;
    s_field_refreshes = 0;
    s_field_text_updates = 0;
    s_test_y = scroll_position(s_tree_view);
    {
        int32_t view_h = s_tree_view ? lv_obj_get_height(s_tree_view) : DESC_TREE_VIEW_H;
        int32_t max_y = (int32_t)s_tree_total * DESC_ROW_H - view_h;
        if (max_y < 0) max_y = 0;
        s_test_dir = s_test_y >= max_y ? -1 : 1;
    }
    s_bench_last_tick = (uint32_t)lv_tick_get();
#ifndef WORK_UI_HOST
    s_bench_last_scan = ltdc_frame_count();
#endif
    s_bench_tick_initialized = true;
}

void scr_descriptors_poll_scroll_test(void)
{
    uint32_t now;
    uint32_t elapsed;
    int32_t max_y;
    int32_t view_h;
    int32_t current_y;
    int32_t next_y;

    if (!s_create_ready || !s_scroll_test || !s_tree_view) return;
    now = (uint32_t)lv_tick_get();
    if (!s_bench_tick_initialized) {
        s_bench_last_tick = now;
        s_bench_tick_initialized = true;
        return;
    }
    elapsed = (uint32_t)(now - s_bench_last_tick);
#ifndef WORK_UI_HOST
    LV_UNUSED(elapsed);
    uint32_t scan = ltdc_frame_count();
    if (scan == s_bench_last_scan) return;
    s_bench_last_scan = scan;
#else
    if (elapsed < DESC_BENCH_INTERVAL_MS) return;
#endif
    s_bench_last_tick = now;

    view_h = lv_obj_get_height(s_tree_view);
    if (view_h < 1) view_h = DESC_TREE_VIEW_H;
    max_y = (int32_t)s_tree_total * DESC_ROW_H - view_h;
    if (max_y <= 0) return;
    current_y = scroll_position(s_tree_view);
    if (s_test_y != current_y) s_test_y = current_y;
    next_y = s_test_y + (int32_t)s_test_dir * DESC_BENCH_STEP_PX;
    if (next_y < 0) {
        next_y = 0;
        s_test_dir = 1;
    } else if (next_y > max_y) {
        next_y = max_y;
        s_test_dir = -1;
    }
    if (next_y == current_y) return;
    s_test_y = next_y;
    s_bench_updates++;
    lv_obj_scroll_to_y(s_tree_view, next_y, LV_ANIM_OFF);
    render_tree();
}

static void semantic_append(char *buffer, size_t capacity, size_t *used,
                            const char *format, ...)
{
    va_list args;
    int written;
    size_t remaining;
    char formatted[512];

    if (!buffer || !used || !format || *used >= capacity) return;
    va_start(args, format);
    written = vsnprintf(formatted, sizeof(formatted), format, args);
    va_end(args);
    if (written < 0) return;
    remaining = capacity - *used;
    *used += work_text_preview_copy(buffer + *used, remaining, formatted, 1);
}

int scr_descriptors_debug_semantic(char *buffer, size_t capacity)
{
    const usb_evidence_snapshot_t *s;
    const ui_view_state_t *v;
    bool page_visible;
    size_t used = 0;
    uint16_t slot;

    if (!buffer || capacity == 0) return -1;
    buffer[0] = '\0';
    if (!s_create_ready) return 0;
    s = snapshot();
    v = view();
    page_visible = s_page && lv_obj_is_visible(s_page);
    semantic_append(buffer, capacity, &used,
        "canvas=descriptors page=%u nodes=%u visible=%u selected=%d frozen=%u tab=%u bench=%lu tree_refresh=%lu tree_text=%lu field_refresh=%lu field_text=%lu tree_surface=%lu;",
        page_visible ? 1U : 0U,
        (unsigned)s_node_count, (unsigned)s_tree_total,
        v ? v->node_index : DESC_NONE,
        v && v->frozen ? 1U : 0U,
        v ? (unsigned)v->descriptor_tab : 0U,
        (unsigned long)s_bench_updates,
        (unsigned long)s_tree_refreshes,
        (unsigned long)s_tree_text_updates,
        (unsigned long)s_field_refreshes,
        (unsigned long)s_field_text_updates, (unsigned long)s_tree_canvas_renders);

    for (slot = 0; page_visible && s && slot < DESC_TREE_SLOTS &&
         used < capacity - 1U; ++slot) {
        int index = visible_node_at((uint16_t)(s_tree_first + slot));
        int32_t y = (int32_t)slot * DESC_ROW_H -
                    (scroll_position(s_tree_view) % DESC_ROW_H);
        const usb_evidence_descriptor_node_t *node;

        if (index < 0 || y + DESC_ROW_H <= 0 || y >= DESC_TREE_VIEW_H)
            continue;
        if (s_tree_node[slot] != index || !s_tree_select[slot] ||
            lv_obj_is_hidden(s_tree_select[slot]))
            continue;
        node = &s->descriptor_nodes[index];
        {
            char title_text[128], source_text[64], formatted[128];
            snprintf(formatted, sizeof(formatted), "%.*s · %.*s",
                (int)sizeof(node->type_name), node->type_name,
                (int)sizeof(node->summary), node->summary);
            ui_font_sanitize_dynamic_text(formatted, title_text, sizeof(title_text));
            snprintf(source_text, sizeof(source_text), "事件 #%lu +%u:%u",
                (unsigned long)node->source_sequence, node->source_offset, node->byte_length);
            semantic_append(buffer, capacity, &used,
                "row%u[%d]=%s | %s ref=#%lu+%u/%u;", slot, index,
                title_text, source_text,
                (unsigned long)node->source_sequence,
                (unsigned)node->source_offset, (unsigned)node->byte_length);
        }
    }

    if (descriptor_hex_active(v)) {
        semantic_append(buffer, capacity, &used, "hex=");
        size_t remaining = used < capacity ? capacity - used : 0;
        if (remaining > 0U) {
            ui_hex_semantic(buffer + used, remaining);
            used += strlen(buffer + used);
            if (used >= capacity) used = capacity - 1U;
        }
    } else if (page_visible && v && v->descriptor_tab == 0 &&
               v->detail == UI_DETAIL_NONE && s &&
               s_selected_node >= 0 && s_selected_node < s_node_count) {
        const usb_evidence_descriptor_node_t *node =
            &s->descriptor_nodes[s_selected_node];
        uint16_t count = field_count_for_node(node);
        int32_t y_scroll = scroll_position(s_fields);
        uint16_t first = s_field_first;
        int16_t remainder = (int16_t)(y_scroll % DESC_ROW_H);

        for (slot = 0; slot < DESC_FIELD_SLOTS && used < capacity - 1U; ++slot) {
            uint32_t index = (uint32_t)node->field_start + first + slot;
            int32_t y = (int32_t)slot * DESC_ROW_H - remainder;
            const usb_evidence_descriptor_field_t *field;

            if (first + slot >= count || index >= s_field_count ||
                y + DESC_ROW_H <= 0 || y >= DESC_DETAIL_BODY_H)
                continue;
            if (!s_field_rows[slot] || lv_obj_is_hidden(s_field_rows[slot]) ||
                s_field_global[slot] != (int16_t)index)
                continue;
            field = &s->descriptor_fields[index];
            {
                lv_obj_t *label = lv_obj_get_child(s_field_rows[slot], 0);
                const char *display_text = label ? lv_label_get_text(label) : "";
                semantic_append(buffer, capacity, &used,
                    "field%u[%lu]=%s ref=#%lu+%u/%u;", slot,
                    (unsigned long)index,
                    display_text ? display_text : "",
                    (unsigned long)node->source_sequence,
                    (unsigned)(node->source_offset + field->byte_offset),
                    (unsigned)field->byte_length);
            }
        }
    }

    if (capacity > 0U) buffer[capacity - 1U] = '\0';
    return (int)used;
}
