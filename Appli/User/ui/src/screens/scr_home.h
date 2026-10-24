#ifndef SCR_HOME_H
#define SCR_HOME_H
#include <rtthread.h>
#include <lvgl/lvgl.h>
#include "usb_evidence.h"
lv_obj_t *scr_home_create(lv_obj_t *parent);
bool scr_home_timeline_is_complete(void);
bool scr_home_deferred_is_complete(void);
bool scr_home_create_timeline_step(void);
bool scr_home_create_details_step(void);
void scr_home_show_timeline(void);
void scr_home_refresh(void);
void scr_home_reset(void);
#if (defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS) || defined(WORK_UI_HOST)
rt_uint8_t scr_home_debug_timeline_visible_count(void);
#endif
#endif
