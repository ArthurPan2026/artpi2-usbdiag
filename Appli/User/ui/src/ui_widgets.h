#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H
#include "ui_styles.h"
#include "ui_model.h"

lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int width, int height);
lv_obj_t *ui_text(lv_obj_t *parent, const char *text, const lv_font_t *font,
                  lv_color_t color);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, int width,
                    ui_action_t action, int32_t argument);
void ui_button_set_selected(lv_obj_t *button, bool selected);
void ui_label_set_safe(lv_obj_t *label, const char *text);
/* Trusted UI formatting only. Caller sanitizes runtime values before adding
 * line breaks. Keep presentation separators out of the untrusted sanitizer. */
void ui_label_set_formatted(lv_obj_t *label, const char *text);
void ui_animate_enter(lv_obj_t *object, unsigned milliseconds, int distance);
void ui_widgets_set_reduced_motion(bool reduced);
void ui_widgets_set_startup_limited(bool limited);
lv_color_t ui_verdict_color(usb_rule_verdict_t verdict);
/* Byte cells use one reusable RGB565 surface. Its contents are also exposed
 * through semantic_debug below, because a canvas has no child labels. */
void ui_hex_set(lv_obj_t *container, const usb_evidence_raw_event_t *event,
                uint16_t offset, uint16_t length, bool enlarged);
void ui_hex_semantic(char *buffer, size_t capacity);
lv_obj_t *ui_hex_canvas_object(void);
unsigned ui_hex_columns(void);

#endif
