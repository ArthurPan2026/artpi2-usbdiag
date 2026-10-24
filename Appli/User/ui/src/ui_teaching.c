/**
 * ui_teaching — Observed enumeration timeline state
 */
#include "ui_teaching.h"

#define STEP_COUNT        USB_EVIDENCE_TIMELINE_COUNT

static uint8_t       g_steps[STEP_COUNT];

static const char *step_names[] = {
    "端口复位", "设备前 8B", "设置地址", "完整设备描述符",
    "配置描述符", "字符串描述符", "设置配置", "枚举完成"
};

void ui_teaching_reset(void)
{
    for (int step_index = 0; step_index < STEP_COUNT; step_index++) {
        g_steps[step_index] = 0;
    }
}

void ui_teaching_set_step(int step)
{
    if (step >= 0 && step < STEP_COUNT) {
        g_steps[step] = 2;
        if (step + 1 < STEP_COUNT && g_steps[step + 1] == 0) {
            g_steps[step + 1] = 1;
        }
    }
}

void ui_teaching_set_timeline(const usb_evidence_timeline_item_t *timeline,
                              uint8_t timeline_count)
{
    uint8_t index;

    ui_teaching_reset();
    if (!timeline)
    {
        return;
    }
    if (timeline_count > STEP_COUNT)
    {
        timeline_count = STEP_COUNT;
    }
    for (index = 0U; index < timeline_count; index++)
    {
        if (timeline[index].status == USB_EVIDENCE_TIMELINE_OBSERVED)
        {
            g_steps[index] = 2U;
        }
        else if (timeline[index].status == USB_EVIDENCE_TIMELINE_FAILED)
        {
            g_steps[index] = 3U;
        }
    }
}

/* Expose step info for UI */
const char *ui_teaching_step_name(int step)
{
    return (step >= 0 && step < STEP_COUNT) ? step_names[step] : "?";
}

const uint8_t *ui_teaching_step_status(void)
{
    return g_steps;
}
