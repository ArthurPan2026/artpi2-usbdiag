#include "ui_deferred.h"

#include <rtthread.h>
#ifndef WORK_UI_HOST
#include "startup_coordinator.h"
#endif

#define UI_DEFERRED_QUEUE_SIZE 24U

typedef struct
{
    ui_deferred_callback_t callback;
    uintptr_t argument;
} ui_deferred_item_t;

static ui_deferred_item_t s_queue[UI_DEFERRED_QUEUE_SIZE];
static uint8_t s_head;
static uint8_t s_tail;
static uint8_t s_count;

int ui_deferred_post(ui_deferred_callback_t callback, uintptr_t argument)
{
    rt_base_t level;

    if (callback == RT_NULL)
    {
        return -1;
    }

    level = rt_hw_interrupt_disable();
    if (s_count >= UI_DEFERRED_QUEUE_SIZE)
    {
        rt_hw_interrupt_enable(level);
        rt_kprintf("[UI] deferred action queue full\r\n");
        return -1;
    }
    s_queue[s_tail].callback = callback;
    s_queue[s_tail].argument = argument;
    s_tail = (uint8_t)((s_tail + 1U) % UI_DEFERRED_QUEUE_SIZE);
    s_count++;
    rt_hw_interrupt_enable(level);
#ifndef WORK_UI_HOST
    startup_notify_ui();
#endif
    return 0;
}

void ui_deferred_process(void)
{
    uint8_t budget;

    /* Take a snapshot so a callback cannot recursively execute work posted by
     * itself in the same phase. */
    {
        rt_base_t level = rt_hw_interrupt_disable();
        budget = s_count;
        rt_hw_interrupt_enable(level);
    }

    while (budget-- != 0U)
    {
        ui_deferred_item_t item;
        rt_base_t level = rt_hw_interrupt_disable();

        if (s_count == 0U)
        {
            rt_hw_interrupt_enable(level);
            break;
        }
        item = s_queue[s_head];
        s_head = (uint8_t)((s_head + 1U) % UI_DEFERRED_QUEUE_SIZE);
        s_count--;
        rt_hw_interrupt_enable(level);

        item.callback(item.argument);
    }
}

int ui_event_is_release_click(lv_event_t *event)
{
    lv_indev_t *indev;

    if (event == RT_NULL || lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return 0;
    }

    indev = lv_indev_active();
    if (indev == RT_NULL || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER)
    {
        /* Keyboard/encoder activation has no pointer release to validate. */
        return 1;
    }

    return lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED &&
           !lv_indev_get_press_moved(indev);
}
