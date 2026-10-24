#ifndef UI_MODEL_H
#define UI_MODEL_H

#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>
#include "usb_evidence.h"

typedef enum { UI_PAGE_LIVE, UI_PAGE_DESCRIPTORS, UI_PAGE_DIAGNOSTICS,
               UI_PAGE_SETTINGS, UI_PAGE_COUNT } ui_page_t;
typedef enum { UI_DETAIL_NONE, UI_DETAIL_RULE, UI_DETAIL_EVENT,
               UI_DETAIL_FIELD, UI_DETAIL_BYTES, UI_DETAIL_SESSION,
               UI_DETAIL_DEVICE } ui_detail_t;
typedef enum {
    UI_ACTION_PAGE, UI_ACTION_STAGE, UI_ACTION_STAGE_PREV, UI_ACTION_STAGE_NEXT,
    UI_ACTION_NODE, UI_ACTION_NODE_TOGGLE, UI_ACTION_FIELD, UI_ACTION_RULE,
    UI_ACTION_EVENT, UI_ACTION_BYTES, UI_ACTION_RULE_REFERENCE,
    UI_ACTION_FOLLOW, UI_ACTION_CLOSE_DETAIL, UI_ACTION_SESSION,
    UI_ACTION_DEVICE, UI_ACTION_REDUCED_MOTION, UI_ACTION_RELATED_RULES,
    UI_ACTION_DESCRIPTOR_TAB, UI_ACTION_DIAGNOSTIC_TAB,
    UI_ACTION_RULE_FILTER, UI_ACTION_EVENT_FILTER, UI_ACTION_REVEAL_SERIAL,
    /* Debug dispatch only, not evidence-model actions. */
    UI_ACTION_WIFI_EDITOR, UI_ACTION_WIFI_SELECT_NETWORK,
    UI_ACTION_WIFI_CONFIRM_SCREEN, UI_ACTION_WIFI_VIEW,
    UI_ACTION_WIFI_SELECT_SAVED, UI_ACTION_WIFI_CONNECT_SAVED_SELECTED,
    UI_ACTION_FEEDBACK_RESET, UI_ACTION_PRELOAD_PROBE /* Debug owner tests only. */
} ui_action_t;

typedef struct {
    const usb_evidence_snapshot_t *snapshot;
    uint32_t snapshot_epoch;
    uint32_t selection_revision;
    ui_page_t page;
    ui_detail_t detail;
    int16_t stage;
    int16_t event_index;
    int16_t node_index;
    int16_t field_index;
    int16_t rule_index;
    uint16_t byte_offset;
    uint16_t byte_length;
    uint64_t collapsed_nodes;
    uint8_t descriptor_tab;  /* 0 fields, 1 bytes */
    uint8_t diagnostic_tab;  /* 0 rules, 1 events */
    uint8_t rule_filter;     /* 0 all, 1 FAIL, 2 NOT_OBSERVED, 3 PASS */
    bool event_stage_filter;
    bool frozen;
    bool updates_available;
    bool reduced_motion;
    bool reveal_serial;
    bool hex_enlarged;
} ui_view_state_t;

typedef void (*ui_action_post_fn)(ui_action_t action, int32_t argument);

/* Only the LVGL owner may call these. The snapshot is the application's
 * reader copy, never a pointer into the producer's mutable publish buffers. */
void ui_model_init(ui_action_post_fn post);
void ui_model_reset(void);
const ui_view_state_t *ui_model_get(void);
const usb_evidence_snapshot_t *ui_model_snapshot(void);
void ui_model_set_snapshot(const usb_evidence_snapshot_t *snapshot);
void ui_model_set_update_available(bool available);
bool ui_model_apply_action(ui_action_t action, int32_t argument);
void ui_model_post(ui_action_t action, int32_t argument);
lv_subject_t *ui_model_page_subject(void);
lv_subject_t *ui_model_selection_subject(void);
lv_subject_t *ui_model_detail_subject(void);

int ui_model_event_stage(const usb_evidence_raw_event_t *event);
const usb_evidence_raw_event_t *ui_model_event_by_sequence(uint32_t sequence);
const usb_evidence_raw_event_t *ui_model_selected_event(void);
int ui_model_event_index(uint32_t sequence);
bool ui_model_range_valid(uint32_t sequence, uint16_t offset, uint16_t length);
unsigned ui_model_stage_event_count(int stage);
unsigned ui_model_stage_event_ordinal(void);
bool ui_model_node_visible(unsigned index);
const char *ui_model_rule_title(unsigned index);
const char *ui_model_verdict_name(usb_rule_verdict_t verdict);
const char *ui_model_capture_name(usb_evidence_capture_status_t status);
const char *ui_model_stage_name(int stage);
const char *ui_model_stage_short_name(int stage);
const char *ui_model_stage_explanation(int stage);
const char *ui_model_event_name(const usb_evidence_raw_event_t *event);

#endif
