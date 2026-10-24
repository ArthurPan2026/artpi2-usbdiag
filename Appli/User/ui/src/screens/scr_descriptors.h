/**
 * scr_descriptors — Interactive USB descriptor teaching explorer
 * Two-pane: left indented tree + right field-detail (table/meaning/hex)
 */
#ifndef SCR_DESCRIPTORS_H
#define SCR_DESCRIPTORS_H
#include <stdbool.h>
#include <stddef.h>
#include <rtthread.h>
#include <lvgl/lvgl.h>
#include "usb_evidence.h"

lv_obj_t *scr_descriptors_create_begin(lv_obj_t *parent);
bool scr_descriptors_create_step(void);
bool scr_descriptors_is_ready(void);
unsigned scr_descriptors_create_progress(void);
lv_obj_t *scr_descriptors_create(lv_obj_t *parent);

/* Render the projection from the authoritative UI model snapshot/state. */
void scr_descriptors_refresh(void);

/* Compatibility wrapper; the argument is ignored in favor of the model. */
void scr_descriptors_update_snapshot(const usb_evidence_snapshot_t *snapshot);

/* Reset the page to the idle state. */
void scr_descriptors_clear(void);

/* Start/stop the LVGL-thread-owned descriptor scrolling benchmark. */
void scr_descriptors_set_scroll_test(rt_bool_t enable);
void scr_descriptors_poll_scroll_test(void);

/* Semantic text for currently visible rows and the active byte canvas. */
int scr_descriptors_debug_semantic(char *buffer, size_t capacity);

#endif
