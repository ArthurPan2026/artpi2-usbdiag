/**
 * ui_teaching — Observed enumeration timeline state
 */
#ifndef UI_TEACHING_H
#define UI_TEACHING_H
#include <rtthread.h>
#include "usb_evidence.h"

void ui_teaching_reset(void);

/* Mark an observed enumeration step in the frozen evidence timeline. */
void ui_teaching_set_step(int step);
void ui_teaching_set_timeline(const usb_evidence_timeline_item_t *timeline,
                              uint8_t timeline_count);
const char *ui_teaching_step_name(int step);
const uint8_t *ui_teaching_step_status(void);

#endif
