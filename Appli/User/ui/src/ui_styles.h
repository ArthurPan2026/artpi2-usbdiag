/**
 * Shared LVGL styles — define once, use everywhere
 */
#ifndef UI_STYLES_H
#define UI_STYLES_H
#include <lvgl/lvgl.h>
#include "ui_fonts.h"

/* Application colors are defined only here, including canvas surfaces. */
#define UI_COLOR_BG        0x0B1220
#define UI_COLOR_SURFACE   0x141F2F
#define UI_COLOR_RAISED    0x1C2B40
#define UI_COLOR_TEXT      0xF1F5F9
#define UI_COLOR_SECONDARY 0xAFC0D5
#define UI_COLOR_ACCENT    0x38BDF8
#define UI_COLOR_PASS      0x34D399
#define UI_COLOR_UNKNOWN   0xFBBF24
#define UI_COLOR_FAIL      0xFB7185
#define UI_COLOR_DIVIDER   0x3A4C63
#define C_BG      lv_color_hex(UI_COLOR_BG)
#define C_SURFACE lv_color_hex(UI_COLOR_SURFACE)
#define C_RAISED  lv_color_hex(UI_COLOR_RAISED)
#define C_TEXT    lv_color_hex(UI_COLOR_TEXT)
#define C_GRAY    lv_color_hex(UI_COLOR_SECONDARY)
#define C_BLUE    lv_color_hex(UI_COLOR_ACCENT)
#define C_GREEN   lv_color_hex(UI_COLOR_PASS)
#define C_ORANGE  lv_color_hex(UI_COLOR_UNKNOWN)
#define C_RED     lv_color_hex(UI_COLOR_FAIL)
#define C_DIVIDER lv_color_hex(UI_COLOR_DIVIDER)
#define C_WHITE C_TEXT
#define C_DBLUE C_RAISED
#define C_TEAL C_BLUE
#define C_INDIGO C_BLUE
#define C_LGRAY C_DIVIDER
#define C_BLUE_PALE C_RAISED
#define C_TEAL_PALE C_RAISED
#define C_AMBER_PALE C_RAISED
#define C_RED_PALE C_RAISED
#define C_INDIGO_PALE C_RAISED

/* Font shortcuts */
#define F_TITLE   UI_FONT_20
#define F_BODY    UI_FONT_16
#define F_META    UI_FONT_14
#define F_HERO    UI_FONT_24

#define UI_TOP_BAR_HEIGHT 52
#define UI_NAV_HEIGHT 56
#define UI_CONTENT_HEIGHT 372
#define UI_TOUCH_HEIGHT 48
#define UI_RADIUS 10
#define UI_SPACE 8
#define UI_MARGIN 12
#define UI_PRESS_MS 80
#define UI_NAV_MS 120
#define UI_DETAIL_MS 160

/* Style init */
void ui_styles_init(void);

/* Pre-built styles */
extern lv_style_t style_card;
extern lv_style_t style_transp;
extern lv_style_t style_title;
extern lv_style_t style_body;

#endif
