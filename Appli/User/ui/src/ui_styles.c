/**
 * Shared LVGL styles
 */
#include "ui_styles.h"

lv_style_t style_card;
lv_style_t style_transp;
lv_style_t style_title;
lv_style_t style_body;

void ui_styles_init(void)
{
    /* Opaque surfaces avoid temporary alpha layers on the RGB565 target. */
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, C_SURFACE);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, UI_RADIUS);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_border_color(&style_card, C_DIVIDER);
    lv_style_set_shadow_width(&style_card, 0);
    lv_style_set_pad_all(&style_card, UI_MARGIN);
    lv_style_set_pad_gap(&style_card, UI_SPACE);
    lv_style_set_text_font(&style_card, F_BODY);
    lv_style_set_text_color(&style_card, C_TEXT);

    /* Transparent container for flex layouts */
    lv_style_init(&style_transp);
    lv_style_set_bg_opa(&style_transp, LV_OPA_TRANSP);
    lv_style_set_border_width(&style_transp, 0);
    lv_style_set_pad_all(&style_transp, 0);

    /* Title text */
    lv_style_init(&style_title);
    lv_style_set_text_font(&style_title, F_TITLE);
    lv_style_set_text_color(&style_title, C_TEXT);

    /* Body text */
    lv_style_init(&style_body);
    lv_style_set_text_font(&style_body, F_BODY);
    lv_style_set_text_color(&style_body, C_GRAY);
}
