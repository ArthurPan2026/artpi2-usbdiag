#ifndef SCR_EVENTS_H
#define SCR_EVENTS_H
#include <stdbool.h>
#include <stddef.h>
#include <lvgl/lvgl.h>
#include "usb_evidence.h"

lv_obj_t *scr_events_create_begin(lv_obj_t *parent);
bool scr_events_create_step(void);
bool scr_events_is_ready(void);
unsigned scr_events_create_progress(void);
lv_obj_t *scr_events_create(lv_obj_t *parent);
void scr_events_refresh(void);
void scr_events_update_summary(uint32_t total, uint32_t passed,
                               uint32_t failed, uint32_t not_observed,
                               uint32_t not_applicable);
void scr_events_update_evidence(const usb_evidence_snapshot_t *snapshot);
void scr_events_reset(void);
int scr_events_debug_semantic(char *buffer, size_t capacity);
#endif
