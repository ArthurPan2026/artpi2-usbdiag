#include "ui_main.h"
#include "ui_styles.h"
#include "usb_evidence.h"
#include <stdlib.h>
#include <string.h>

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
/* Keep the bounded reply out of the 4 KiB FinSH thread stack. */
static ui_inspect_item_t s_items[8] USB_EVIDENCE_RAM_SECTION;
static char s_state[384];
static rt_uint32_t s_request;
static const char *const s_pages[] = {"live", "descriptors", "diagnostics", "settings"};
static void packet(const char *tag, unsigned request, unsigned index, const char *text)
{
    size_t length = strlen(text);
    rt_kprintf("[%s] req=%u index=%u bytes=%u crc32=%08lX\r\n",
        tag, request, index, (unsigned)length,
        (unsigned long)usb_evidence_bytes_crc32(text, length));
    static const char digits[] = "0123456789ABCDEF";
    for (size_t offset = 0; offset < length; offset += 16) {
        size_t count = length - offset < 16 ? length - offset : 16;
        char hex[33];
        for (size_t i = 0; i < count; i++) {
            unsigned char value = text[offset + i];
            hex[2 * i] = digits[value >> 4];
            hex[2 * i + 1] = digits[value & 15];
        }
        hex[2 * count] = '\0';
        rt_kprintf("[%s_HEX] req=%u index=%u offset=%u hex=%s\r\n",
            tag, request, index, (unsigned)offset, hex);
    }
}
static void ui_inspect_cmd(int argc, char **argv)
{
    unsigned request = ++s_request, page = 4, first = 0, total = 0, returned = 0;
    if (argc < 2 || argc > 3) {
        rt_kprintf("[UI_INSPECT] req=%u FAIL usage=page_offset\r\n", request); return;
    }
    for (unsigned i = 0; i < 4; i++) if (!strcmp(argv[1], s_pages[i])) page = i;
    if (argc == 3) {
        char *end;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (*end || value > 4096) page = 4;
        else first = (unsigned)value;
    }
    rt_err_t result = ui_main_debug_inspect(page, first, 1500, s_items, &total, &returned, s_state);
    if (result != RT_EOK) {
        rt_kprintf("[UI_INSPECT] req=%u FAIL error=%d\r\n", request, result); return;
    }
    rt_kprintf("[UI_INSPECT] req=%u page=%s first=%u returned=%u total=%u\r\n",
        request, s_pages[page], first, returned, total);
    packet("UI_STATE", request, 0, s_state);
    for (unsigned i = 0; i < returned; i++) {
        ui_inspect_item_t *item = &s_items[i];
        rt_kprintf("[UI_BOX] req=%u index=%u x=%d y=%d w=%d h=%d\r\n",
            request, first + i, item->x, item->y, item->w, item->h);
        rt_kprintf("[UI_FLAGS] req=%u index=%u clipped=%u touch=%u canvas=%u font=%u disabled=%u\r\n",
            request, first + i, item->clipped, item->touch, item->canvas, item->font_px, item->disabled);
        packet("UI_ITEM", request, first + i, item->text);
    }
    rt_kprintf("[UI_INSPECT_END] req=%u PASS\r\n", request);
}
MSH_CMD_EXPORT(ui_inspect_cmd, debug-only paged UI text geometry and canvas evidence);
static void ui_action_test_cmd(int argc, char **argv)
{
    static const struct { const char *name; ui_action_t action; } actions[] = {
        {"page",UI_ACTION_PAGE}, {"stage",UI_ACTION_STAGE}, {"prev",UI_ACTION_STAGE_PREV},
        {"next",UI_ACTION_STAGE_NEXT}, {"node",UI_ACTION_NODE}, {"fold",UI_ACTION_NODE_TOGGLE},
        {"field",UI_ACTION_FIELD}, {"rule",UI_ACTION_RULE}, {"event",UI_ACTION_EVENT},
        {"bytes",UI_ACTION_BYTES}, {"ref",UI_ACTION_RULE_REFERENCE}, {"follow",UI_ACTION_FOLLOW},
        {"close",UI_ACTION_CLOSE_DETAIL}, {"session",UI_ACTION_SESSION}, {"device",UI_ACTION_DEVICE},
        {"motion",UI_ACTION_REDUCED_MOTION}, {"related",UI_ACTION_RELATED_RULES},
        {"desc_tab",UI_ACTION_DESCRIPTOR_TAB}, {"diag_tab",UI_ACTION_DIAGNOSTIC_TAB},
        {"rule_filter",UI_ACTION_RULE_FILTER}, {"event_filter",UI_ACTION_EVENT_FILTER},
        {"serial",UI_ACTION_REVEAL_SERIAL}, {"wifi_editor",UI_ACTION_WIFI_EDITOR},
        {"wifi_select",UI_ACTION_WIFI_SELECT_NETWORK},
        {"wifi_confirm",UI_ACTION_WIFI_CONFIRM_SCREEN}, {"wifi_view",UI_ACTION_WIFI_VIEW},
        {"wifi_saved",UI_ACTION_WIFI_SELECT_SAVED}, {"wifi_connect",UI_ACTION_WIFI_CONNECT_SAVED_SELECTED},
        {"feedback_reset",UI_ACTION_FEEDBACK_RESET}, {"preload_probe",UI_ACTION_PRELOAD_PROBE}
    };
    unsigned request = ++s_request;
    int32_t arg = 0;
    unsigned index = sizeof(actions) / sizeof(actions[0]);
    if (argc < 2 || argc > 3) {
        rt_kprintf("[UI_ACTION] req=%u FAIL usage=action_value\r\n", request); return;
    }
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
        if (!strcmp(argv[1], actions[i].name)) { index = i; break; }
    if (argc == 3) {
        char *end;
        long value = strtol(argv[2], &end, 10);
        if (*end || value < -1 || value > 4096) index = sizeof(actions) / sizeof(actions[0]);
        else arg = (int32_t)value;
    }
    if (index == sizeof(actions) / sizeof(actions[0])) {
        rt_kprintf("[UI_ACTION] req=%u FAIL invalid_action\r\n", request); return;
    }
    rt_err_t result = ui_main_debug_action(actions[index].action, arg, 1500, s_state);
    if (result != RT_EOK) {
        const char *status=result==-RT_ETIMEOUT?"timeout_cancelled":
            (result==-RT_EBUSY?"busy_or_in_flight":"rejected");
        rt_kprintf("[UI_ACTION] req=%u FAIL error=%d state=%s\r\n", request, result, status); return;
    }
    packet("UI_STATE", request, 0, s_state);
    rt_kprintf("[UI_ACTION_END] req=%u PASS\r\n", request);
}
MSH_CMD_EXPORT(ui_action_test_cmd, debug-only action replay through the LVGL owner);

static void ui_preload_status_cmd(void)
{
    ui_preload_stats_t stats;
    static char json[768];
    ui_main_preload_stats(&stats);
    int n = rt_snprintf(json, sizeof(json),
        "{\"ready_mask\":%lu,\"loading_page\":%lu,\"background_steps\":%lu,\"foreground_steps\":%lu,"
        "\"steps\":[%lu,%lu,%lu,%lu],\"max_step_us\":[%lu,%lu,%lu,%lu],"
        "\"work_us\":[%lu,%lu,%lu,%lu],\"ready_ms\":[%lu,%lu,%lu,%lu],"
        "\"probe\":%lu,\"probe_cancel_steps\":%lu,\"probe_fg_before\":%lu,\"probe_fg_after\":%lu}",
        (unsigned long)stats.ready_mask, (unsigned long)stats.loading_page,
        (unsigned long)stats.background_steps, (unsigned long)stats.foreground_steps,
        (unsigned long)stats.steps[0], (unsigned long)stats.steps[1],
        (unsigned long)stats.steps[2], (unsigned long)stats.steps[3],
        (unsigned long)stats.max_step_us[0], (unsigned long)stats.max_step_us[1],
        (unsigned long)stats.max_step_us[2], (unsigned long)stats.max_step_us[3],
        (unsigned long)stats.work_us[0], (unsigned long)stats.work_us[1],
        (unsigned long)stats.work_us[2], (unsigned long)stats.work_us[3],
        (unsigned long)stats.ready_ms[0], (unsigned long)stats.ready_ms[1],
        (unsigned long)stats.ready_ms[2], (unsigned long)stats.ready_ms[3],
        (unsigned long)stats.probe, (unsigned long)stats.probe_cancel_steps,
        (unsigned long)stats.probe_fg_before, (unsigned long)stats.probe_fg_after);
    if (n <= 0 || (unsigned)n >= sizeof(json)) {
        rt_kprintf("[UI_PRELOAD_END] FAIL capacity\r\n"); return;
    }
    unsigned request = ++s_request;
    packet("UI_PRELOAD", request, 0, json);
    rt_kprintf("[UI_PRELOAD_END] req=%u PASS\r\n", request);
}
MSH_CMD_EXPORT(ui_preload_status_cmd, debug-only cooperative preload RAM counters);
#endif
