/*
 * lv_port_indev.c - LVGL input port (GT911 capacitive touch)
 *
 * The GT911 is read by an RT-Thread worker and its explicit DOWN/MOVE/UP /
 * CANCEL events are buffered here.  LVGL's read callback is deliberately
 * non-blocking: it only consumes one event from the fixed queue or repeats
 * the last state while the panel has not published a newer report.
 */
#include "lv_port_indev.h"

#include <finsh.h>
#include <rtthread.h>
#include <lvgl/lvgl.h>
#include "touch_gt911.h"

static lv_indev_t *s_indev;
static lv_point_t s_last_point;
static lv_indev_state_t s_last_state = LV_INDEV_STATE_RELEASED;

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    gt911_touch_event_t event;

    data->point = s_last_point;
    data->state = s_last_state;
    data->timestamp = lv_tick_get();
    if(!gt911_touch_is_ready()){
        data->state=LV_INDEV_STATE_RELEASED;data->continue_reading=false;return;
    }

    if (gt911_touch_pop(&event))
    {
        s_last_point.x = (int32_t)event.x;
        s_last_point.y = (int32_t)event.y;
        if (s_last_point.x >= LV_HOR_RES)
        {
            s_last_point.x = LV_HOR_RES - 1;
        }
        if (s_last_point.y >= LV_VER_RES)
        {
            s_last_point.y = LV_VER_RES - 1;
        }
        data->point = s_last_point;
        data->timestamp = event.timestamp;

        if (event.type == GT911_TOUCH_CANCEL)
        {
            /* LVGL then emits PRESS_LOST and deliberately skips CLICKED. */
            lv_indev_wait_release(indev);
            s_last_state = LV_INDEV_STATE_RELEASED;
        }
        else if (event.type == GT911_TOUCH_UP)
        {
            s_last_state = LV_INDEV_STATE_RELEASED;
        }
        else
        {
            s_last_state = LV_INDEV_STATE_PRESSED;
        }
        data->state = s_last_state;
    }

    /* lv_indev_read() will call this callback again in the same LVGL turn
     * when a buffered transition remains. This preserves DOWN->UP for a
     * quick tap even when both reports arrived between two LVGL turns. */
    data->continue_reading = gt911_touch_pending() != 0U;
}

int lvgl_indev_init(void)
{
    s_indev = lv_indev_create();
    if (s_indev == NULL)
    {
        return -1;
    }

    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, lvgl_touch_read_cb);

    /* Match the physical 4.3-inch panel's roughly 9 px / 8 dp touch slop.
     * LVGL already emits CLICKED only on a release that did not become a
     * scroll; these values make small finger jitter less likely to promote a
     * tap into a scroll while retaining responsive list dragging. */
    lv_indev_set_scroll_limit(s_indev, 12U);
    lv_indev_set_scroll_throw(s_indev, 10U);
    lv_indev_set_long_press_time(s_indev, 400U);
    lv_indev_set_long_press_repeat_time(s_indev, 100U);

    /* Poll the non-blocking queue at the start of every LVGL turn instead of
     * letting an independent LVGL input timer race with the manually owned
     * turn. The producer is a short-period RT-Thread worker. */
    if (lv_indev_get_read_timer(s_indev) != NULL)
    {
        lv_timer_pause(lv_indev_get_read_timer(s_indev));
    }
    if (gt911_touch_start() != 0)
    {
        lv_indev_delete(s_indev);
        s_indev = NULL;
        return -1;
    }
    return 0;
}

void lvgl_indev_poll(void)
{
    if (s_indev != NULL)
    {
        lv_indev_read(s_indev);
    }
}

static const char *touch_event_state_name(uint8_t active)
{
    return active ? "PRESSED" : "RELEASED";
}

static void touch_status_cmd(void)
{
    gt911_touch_stats_t stats;

    gt911_touch_stats_get(&stats);
    rt_kprintf("[TOUCH] irq=%lu frames=%lu no_data=%lu io_err=%lu "
               "down=%lu move=%lu up=%lu cancel=%lu\r\n",
               (unsigned long)stats.irq_count,
               (unsigned long)stats.frames,
               (unsigned long)stats.no_data,
               (unsigned long)stats.io_errors,
               (unsigned long)stats.down_count,
               (unsigned long)stats.move_count,
               (unsigned long)stats.up_count,
               (unsigned long)stats.cancel_count);
    rt_kprintf("[TOUCH] queue=%u/%lu coalesced=%lu dropped_move=%lu "
               "dropped_transition=%lu max_latency=%lums state=%s\r\n",
               (unsigned)stats.queue_depth,
               (unsigned long)stats.queue_high_water,
               (unsigned long)stats.coalesced_moves,
               (unsigned long)stats.dropped_moves,
               (unsigned long)stats.dropped_transitions,
               (unsigned long)stats.max_queue_latency_ms,
               touch_event_state_name(stats.contact_active));
    rt_kprintf("[TOUCH] stages status_err=%lu point_err=%lu clear_err=%lu "
               "bad_status=%lu ctrl_reset=%lu "
               "last_status=0x%02x bad=0x%02x "
               "last_event=%u last=(%u,%u)\r\n",
               (unsigned long)stats.status_errors,
               (unsigned long)stats.point_errors,
               (unsigned long)stats.clear_errors,
               (unsigned long)stats.invalid_status,
               (unsigned long)stats.controller_resets,
               (unsigned)stats.last_status,
               (unsigned)stats.last_bad_status,
               (unsigned)stats.last_event,
               (unsigned)stats.last_x,
               (unsigned)stats.last_y);
}
MSH_CMD_EXPORT(touch_status_cmd, show GT911 input state and queue statistics);
