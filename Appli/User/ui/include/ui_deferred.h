#ifndef WORK_UI_DEFERRED_H
#define WORK_UI_DEFERRED_H

#include <stdint.h>
#include <lvgl/lvgl.h>

typedef void (*ui_deferred_callback_t)(uintptr_t argument);

/* Queue a UI/business operation for the post-render phase of the current
 * LVGL owner thread. The queue is fixed-size and never allocates. */
int ui_deferred_post(ui_deferred_callback_t callback, uintptr_t argument);

/* Execute the items that were present when this function began. Items posted
 * by a deferred callback are intentionally left for the next LVGL turn. */
void ui_deferred_process(void);

/* Guard application actions: only a genuine LVGL CLICKED generated on a
 * release may enter the deferred business-action queue. */
int ui_event_is_release_click(lv_event_t *event);

#endif /* WORK_UI_DEFERRED_H */
