#ifndef SCR_WIFI_H
#define SCR_WIFI_H
#include <lvgl/lvgl.h>

void scr_wifi_init(void);
lv_obj_t *scr_wifi_create_begin(lv_obj_t *parent);
bool scr_wifi_create_step(void);
bool scr_wifi_is_ready(void);
unsigned scr_wifi_create_progress(void);
lv_obj_t *scr_wifi_create(lv_obj_t *parent);
void scr_wifi_poll(void);
void scr_wifi_refresh_preferences(void);
/* Preserve the draft but always mask it when leaving settings. */
void scr_wifi_close_editor(void);
/* Debug/text exports must not expose a user-revealed credential. */
bool scr_wifi_capture_allowed(void);
#if defined(WORK_UI_HOST) && WORK_UI_HOST
/* Replacing an isolated host coordinator models a new session, not a late
 * result from the prior one. This is absent from all firmware profiles. */
void scr_wifi_host_reset_operations(void);
#endif
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
typedef enum {
    SCR_WIFI_DEBUG_CONFIRM_DISCONNECT = 0,
    SCR_WIFI_DEBUG_CONFIRM_FORGET,
    SCR_WIFI_DEBUG_CONFIRM_UPDATE_PASSWORD,
    SCR_WIFI_DEBUG_CONFIRM_CLEAR_FIRST,
    SCR_WIFI_DEBUG_CONFIRM_CLEAR_FINAL
} scr_wifi_debug_confirm_kind_t;

bool scr_wifi_debug_editor(unsigned action);
/* action remains: 0 open, 1 cancel, 2 done, 3 symbols, 4 more. */
/* index is the zero-based position in the currently sorted, non-hidden scan
 * rows. It switches to WiFi and opens details without submitting a request. */
bool scr_wifi_debug_select_network(unsigned index);
bool scr_wifi_debug_select_saved(uint32_t profile_id);
/* Replay the real connect handler for an already persisted selected profile.
 * Debug UART only; accepts no credentials and cannot create a new network. */
bool scr_wifi_debug_connect_saved_selected(void);
bool scr_wifi_debug_view(unsigned kind); /* 0 WiFi, 1 display, 2 saved list */
/* kind uses scr_wifi_debug_confirm_kind_t; opens a preview only. It never
 * accepts the dialog or submits a radio/storage request. LVGL-owner only. */
bool scr_wifi_debug_confirm_screen(unsigned kind);
#endif

#endif
