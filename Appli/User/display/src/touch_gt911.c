#include "touch_gt911.h"

#include <rtdevice.h>
#include <rtthread.h>
#include <drv_gpio.h>
#include "startup_coordinator.h"

#define GT911_I2C_ADDR              0x14U
#define GT911_I2C_ALT_ADDR          0x5DU
#define GT911_I2C_BUS_NAME          "i2c2"

#define GT911_REG_CTRL              0x8040U
#define GT911_REG_PID               0x8140U
#define GT911_REG_TPINFO            0x814EU
#define GT911_REG_TP1               0x8150U

#define TOUCH_INT_PIN               GET_PIN(E, 15)
#define TOUCH_RST_PIN               GET_PIN(N, 12)
#define TOUCH_SDA_PIN               GET_PIN(F, 0)
#define TOUCH_SCL_PIN               GET_PIN(F, 1)

/* The touch worker is deliberately independent from LVGL.  A 10-ms active
 * poll keeps input latency low, while the slower idle poll avoids bus traffic
 * when no finger is down.  This is more robust on the ART-Pi2 than relying on
 * a potentially noisy PE15 edge while preserving the same RT-Thread GPIO
 * ownership. */
#define GT911_ACTIVE_POLL_MS        10U
#define GT911_IDLE_POLL_MS          20U
#define GT911_RELEASE_TIMEOUT_MS    160U
#define GT911_CONTROLLER_RESET_GAP_MS 1000U

#define GT911_EVENT_QUEUE_SIZE      32U
#define GT911_WORKER_STACK_SIZE     2048U
#define GT911_WORKER_PRIORITY       4U
#define GT911_WORKER_TIMESLICE      2U

typedef enum
{
    GT911_FRAME_NO_DATA = 0,
    GT911_FRAME_READY,
    GT911_FRAME_ERROR,
} gt911_frame_status_t;

static struct rt_i2c_bus_device *s_bus;
static uint8_t s_i2c_address = GT911_I2C_ADDR;

static rt_thread_t s_touch_thread;
static rt_bool_t s_touch_started;
static volatile rt_bool_t s_touch_ready;
static rt_bool_t s_touch_active;
static uint8_t s_primary_track_id;
static gt911_point_t s_last_point;
static gt911_point_t s_filtered_point;
static rt_bool_t s_filter_valid;
static rt_tick_t s_last_valid_tick;
static uint8_t s_invalid_status_streak;
static rt_tick_t s_last_controller_reset_tick;
static rt_bool_t s_controller_reset_done;

static gt911_touch_event_t s_event_queue[GT911_EVENT_QUEUE_SIZE];
static uint8_t s_event_head;
static uint8_t s_event_tail;
static uint8_t s_event_count;
static gt911_touch_stats_t s_stats;

static struct rt_i2c_bus_device *gt911_bus(void)
{
    if (s_bus == RT_NULL)
    {
        s_bus = rt_i2c_bus_device_find(GT911_I2C_BUS_NAME);
    }
    return s_bus;
}

static int gt911_write_reg(uint16_t reg, const uint8_t *data, uint8_t length)
{
    uint8_t buffer[34];
    struct rt_i2c_msg message;
    struct rt_i2c_bus_device *bus = gt911_bus();

    if (bus == RT_NULL || data == RT_NULL || length > 32U)
    {
        return -1;
    }
    buffer[0] = (uint8_t)(reg >> 8);
    buffer[1] = (uint8_t)reg;
    rt_memcpy(&buffer[2], data, length);
    message.addr = s_i2c_address;
    message.flags = RT_I2C_WR;
    message.len = (uint16_t)length + 2U;
    message.buf = buffer;
    return rt_i2c_transfer(bus, &message, 1U) == 1 ? 0 : -1;
}

static int gt911_read_reg(uint16_t reg, uint8_t *data, uint8_t length)
{
    uint8_t address[2];
    struct rt_i2c_msg messages[2];
    struct rt_i2c_bus_device *bus = gt911_bus();

    if (bus == RT_NULL || data == RT_NULL || length == 0U)
    {
        return -1;
    }
    address[0] = (uint8_t)(reg >> 8);
    address[1] = (uint8_t)reg;
    messages[0].addr = s_i2c_address;
    messages[0].flags = RT_I2C_WR;
    messages[0].len = sizeof(address);
    messages[0].buf = address;
    messages[1].addr = s_i2c_address;
    messages[1].flags = RT_I2C_RD;
    messages[1].len = length;
    messages[1].buf = data;
    return rt_i2c_transfer(bus, messages, 2U) == 2 ? 0 : -1;
}

static void gt911_hw_reset(void)
{
    /*
     * ART-Pi2 drives the panel reset through an NPN transistor. Therefore
     * the MCU-side PN12 level is inverted at the panel connector:
     *
     *   PN12 high -> TS_RST low  (panel reset asserted)
     *   PN12 low  -> TS_RST high (panel reset released)
     *
     * Keep the address selection sequence that is known to work with the
     * board's 0x14 panel. INT is changed to a pulled input only after the
     * address has latched, so the pull-down cannot affect that sequence.
     */
    rt_pin_mode(TOUCH_RST_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(TOUCH_INT_PIN, PIN_MODE_OUTPUT);

    rt_pin_write(TOUCH_RST_PIN, PIN_HIGH);
    rt_pin_write(TOUCH_INT_PIN, PIN_LOW);
    rt_pin_write(TOUCH_INT_PIN, PIN_HIGH);
    rt_thread_mdelay(1U);
    rt_pin_write(TOUCH_RST_PIN, PIN_LOW);
    rt_thread_mdelay(10U);
    rt_pin_mode(TOUCH_INT_PIN, PIN_MODE_INPUT);
    rt_thread_mdelay(100U);
}

static int gt911_controller_reinit(void)
{
    uint8_t command;

    gt911_hw_reset();

    command = 0x02U;
    if (gt911_write_reg(GT911_REG_CTRL, &command, 1U) != 0)
    {
        return -1;
    }
    rt_thread_mdelay(10U);

    command = 0x00U;
    if (gt911_write_reg(GT911_REG_CTRL, &command, 1U) != 0)
    {
        return -1;
    }
    rt_thread_mdelay(20U);
    return 0;
}

int gt911_init(void)
{
    uint8_t pid[5] = {0};
    uint8_t command;

    if (gt911_bus() == RT_NULL)
    {
        rt_kprintf("[GT911] i2c2 bus not found\r\n");
        return -1;
    }

    gt911_hw_reset();
    /* Probe by reading the PID through the selected RT-Thread bus. This
     * avoids the HAL-only negative probe path and keeps both transport
     * implementations on the same register transaction. */
    s_i2c_address = GT911_I2C_ADDR;
    if (gt911_read_reg(GT911_REG_PID, pid, 4U) == 0)
    {
        s_i2c_address = GT911_I2C_ADDR;
    }
    else
    {
        s_i2c_address = GT911_I2C_ALT_ADDR;
        if (gt911_read_reg(GT911_REG_PID, pid, 4U) != 0)
        {
            rt_kprintf("[GT911] no ACK on 0x14/0x5d, SDA=%d SCL=%d\r\n",
                       (int)rt_pin_read(TOUCH_SDA_PIN),
                       (int)rt_pin_read(TOUCH_SCL_PIN));
            return -1;
        }
    }

    pid[4] = '\0';
    if (rt_strcmp((char *)pid, "911") != 0 &&
        rt_strcmp((char *)pid, "9147") != 0 &&
        rt_strcmp((char *)pid, "9271") != 0 &&
        rt_strcmp((char *)pid, "1158") != 0)
    {
        rt_kprintf("[GT911] unexpected PID '%s'\r\n", pid);
        return -1;
    }

    command = 0x02U;
    if (gt911_write_reg(GT911_REG_CTRL, &command, 1U) != 0)
    {
        rt_kprintf("[GT911] software reset command failed\r\n");
        return -1;
    }
    rt_thread_mdelay(10U);
    command = 0x00U;
    if (gt911_write_reg(GT911_REG_CTRL, &command, 1U) != 0)
    {
        rt_kprintf("[GT911] software reset release failed\r\n");
        return -1;
    }
    /* The panel manual specifies a scan interval above 10 ms. Do not let the
     * asynchronous worker issue its first TPINFO read while the 0x00 reset
     * release is still settling; an early NACK can leave I2C2 BUSY on this
     * H7RS HAL revision. */
    rt_thread_mdelay(20U);
    rt_kprintf("[GT911] PID=%.4s ready at 0x%02x\r\n", pid, s_i2c_address);
    return 0;
}

static gt911_frame_status_t gt911_read_frame(gt911_point_t *points,
                                             uint8_t *count)
{
    uint8_t raw_point[6];
    uint8_t status;
    uint8_t clear_status = 0U;
    uint8_t point_count;
    uint8_t index;

    if (points == RT_NULL || count == RT_NULL)
    {
        return GT911_FRAME_ERROR;
    }
    *count = 0U;

    /* Follow the actual ATK-MD0430R/GT1158 transaction granularity: read
     * TPINFO first, then read each six-byte coordinate record at 0x8150,
     * 0x8158, ... . The vendor manual calls 0x8150 "TP1" and documents
     * exactly X/Y/size; 0x814F is the optional track byte used by some
     * GT9147 revisions and must not shift this module's data. */
    if (gt911_read_reg(GT911_REG_TPINFO, &status, 1U) != 0)
    {
        s_stats.status_errors++;
        return GT911_FRAME_ERROR;
    }
    s_stats.last_status = status;
    if ((status & 0x80U) == 0U)
    {
        s_invalid_status_streak = 0U;
        return GT911_FRAME_NO_DATA;
    }

    point_count = status & 0x0FU;
    if (point_count > GT911_MAX_POINTS)
    {
        s_stats.invalid_status++;
        s_stats.last_bad_status = status;
        s_invalid_status_streak++;

        /* A malformed report must still be acknowledged.  Leaving the
         * controller's data-ready bit set makes every following poll enter
         * the same error path. */
        if (gt911_write_reg(GT911_REG_TPINFO, &clear_status, 1U) != 0)
        {
            s_stats.clear_errors++;
        }
        if (s_invalid_status_streak >= 3U)
        {
            rt_tick_t now = rt_tick_get();

            s_invalid_status_streak = 0U;
            if (!s_controller_reset_done ||
                (now - s_last_controller_reset_tick) >=
                    rt_tick_from_millisecond(GT911_CONTROLLER_RESET_GAP_MS))
            {
                if (gt911_controller_reinit() == 0)
                {
                    s_stats.controller_resets++;
                    s_last_controller_reset_tick = rt_tick_get();
                    s_controller_reset_done = RT_TRUE;
                }
            }
            rt_thread_mdelay(1U);
        }
        return GT911_FRAME_ERROR;
    }
    s_invalid_status_streak = 0U;
    for (index = 0U; index < point_count; index++)
    {
        uint16_t point_register = (uint16_t)(GT911_REG_TP1 + index * 8U);

        if (gt911_read_reg(point_register, raw_point,
                           sizeof(raw_point)) != 0)
        {
            s_stats.point_errors++;
            return GT911_FRAME_ERROR;
        }

        points[index].track_id = index;
        points[index].x = (uint16_t)(((uint16_t)raw_point[1] << 8) |
                                     raw_point[0]);
        points[index].y = (uint16_t)(((uint16_t)raw_point[3] << 8) |
                                     raw_point[2]);
        points[index].size = (uint16_t)(((uint16_t)raw_point[5] << 8) |
                                        raw_point[4]);
    }

    /* Only acknowledge a frame that was read and parsed successfully. A
     * transient NO_DATA state must not manufacture a release event. */
    if (gt911_write_reg(GT911_REG_TPINFO, &clear_status, 1U) != 0)
    {
        s_stats.clear_errors++;
        return GT911_FRAME_ERROR;
    }
    *count = point_count;
    return GT911_FRAME_READY;
}

static uint16_t gt911_filter_axis(uint16_t previous, uint16_t current)
{
    int32_t delta = (int32_t)current - (int32_t)previous;
    int32_t magnitude = delta < 0 ? -delta : delta;

    if (magnitude <= 1)
    {
        return current;
    }
    if (magnitude > 24)
    {
        /* Preserve responsiveness during a quick swipe while taking only
         * the smallest useful edge off a noisy large sample. */
        return (uint16_t)((int32_t)previous + (delta * 7) / 8);
    }
    return (uint16_t)((int32_t)previous + (delta * 3) / 4);
}

static gt911_point_t gt911_filter_move(const gt911_point_t *point)
{
    gt911_point_t filtered = *point;

    filtered.x = gt911_filter_axis(s_filtered_point.x, point->x);
    filtered.y = gt911_filter_axis(s_filtered_point.y, point->y);
    filtered.size = point->size;
    return filtered;
}

static void gt911_queue_remove_offset(uint8_t offset)
{
    uint8_t index;

    for (index = offset; index + 1U < s_event_count; index++)
    {
        uint8_t destination = (uint8_t)((s_event_head + index) %
                                        GT911_EVENT_QUEUE_SIZE);
        uint8_t source = (uint8_t)((s_event_head + index + 1U) %
                                   GT911_EVENT_QUEUE_SIZE);
        s_event_queue[destination] = s_event_queue[source];
    }
    s_event_tail = (uint8_t)((s_event_tail + GT911_EVENT_QUEUE_SIZE - 1U) %
                             GT911_EVENT_QUEUE_SIZE);
    s_event_count--;
}

static void gt911_queue_event(gt911_touch_event_type_t type,
                              const gt911_point_t *point,
                              rt_tick_t timestamp)
{
    gt911_touch_event_t event;
    rt_base_t level;
    uint8_t index;
    int move_offset = -1;

    event.type = type;
    event.x = point ? point->x : s_last_point.x;
    event.y = point ? point->y : s_last_point.y;
    event.size = point ? point->size : s_last_point.size;
    event.track_id = point ? point->track_id : s_primary_track_id;
    event.timestamp = (uint32_t)timestamp;

    level = rt_hw_interrupt_disable();

    /* A fast producer and a temporarily busy LVGL frame may generate many
     * MOVE samples. Keep transitions lossless and collapse only adjacent
     * MOVE samples with the same contact identity. */
    if (s_event_count != 0U && type == GT911_TOUCH_MOVE)
    {
        uint8_t last_index = (uint8_t)((s_event_tail +
                                        GT911_EVENT_QUEUE_SIZE - 1U) %
                                       GT911_EVENT_QUEUE_SIZE);
        if (s_event_queue[last_index].type == GT911_TOUCH_MOVE &&
            s_event_queue[last_index].track_id == event.track_id)
        {
            s_event_queue[last_index] = event;
            s_stats.coalesced_moves++;
            rt_hw_interrupt_enable(level);
            startup_notify_ui();
            return;
        }
    }

    if (s_event_count >= GT911_EVENT_QUEUE_SIZE)
    {
        for (index = 0U; index < s_event_count; index++)
        {
            uint8_t queue_index = (uint8_t)((s_event_head + index) %
                                            GT911_EVENT_QUEUE_SIZE);
            if (s_event_queue[queue_index].type == GT911_TOUCH_MOVE)
            {
                move_offset = index;
                break;
            }
        }
        if (move_offset >= 0)
        {
            gt911_queue_remove_offset((uint8_t)move_offset);
            s_stats.dropped_moves++;
        }
        else
        {
            if (type == GT911_TOUCH_MOVE)
            {
                s_stats.dropped_moves++;
            }
            else
            {
                s_stats.dropped_transitions++;
            }
            rt_hw_interrupt_enable(level);
            return;
        }
    }

    s_event_queue[s_event_tail] = event;
    s_event_tail = (uint8_t)((s_event_tail + 1U) % GT911_EVENT_QUEUE_SIZE);
    s_event_count++;
    if (s_event_count > s_stats.queue_high_water)
    {
        s_stats.queue_high_water = s_event_count;
    }
    s_stats.last_event = (uint8_t)type;
    s_stats.last_x = event.x;
    s_stats.last_y = event.y;
    s_stats.last_size = event.size;
    if (type == GT911_TOUCH_DOWN)
    {
        s_stats.down_count++;
    }
    else if (type == GT911_TOUCH_MOVE)
    {
        s_stats.move_count++;
    }
    else if (type == GT911_TOUCH_UP)
    {
        s_stats.up_count++;
    }
    else
    {
        s_stats.cancel_count++;
    }
    rt_hw_interrupt_enable(level);
    startup_notify_ui();
}

static int gt911_find_primary(const gt911_point_t *points, uint8_t count)
{
    uint8_t index;

    for (index = 0U; index < count; index++)
    {
        if (points[index].track_id == s_primary_track_id)
        {
            return index;
        }
    }
    return -1;
}

static void gt911_process_frame(const gt911_point_t *points, uint8_t count)
{
    rt_tick_t now = rt_tick_get();
    int primary_index;

    s_stats.frames++;
    s_last_valid_tick = now;

    if (count == 0U)
    {
        if (s_touch_active)
        {
            gt911_queue_event(GT911_TOUCH_UP, &s_last_point, now);
            s_touch_active = RT_FALSE;
            s_filter_valid = RT_FALSE;
        }
        return;
    }

    primary_index = s_touch_active ? gt911_find_primary(points, count) : -1;
    if (primary_index < 0)
    {
        if (s_touch_active)
        {
            /* A primary track disappeared while another finger remains.
             * Represent the identity switch as UP followed by DOWN so LVGL
             * never drags one widget into another contact. */
            gt911_queue_event(GT911_TOUCH_UP, &s_last_point, now);
        }
        primary_index = 0;
        s_primary_track_id = points[primary_index].track_id;
        s_last_point = points[primary_index];
        s_filtered_point = s_last_point;
        s_filter_valid = RT_TRUE;
        s_touch_active = RT_TRUE;
        gt911_queue_event(GT911_TOUCH_DOWN, &s_last_point, now);
        return;
    }

    if (!s_filter_valid)
    {
        s_filtered_point = points[primary_index];
        s_filter_valid = RT_TRUE;
    }
    else
    {
        s_filtered_point = gt911_filter_move(&points[primary_index]);
    }
    s_filtered_point.track_id = s_primary_track_id;
    s_last_point = s_filtered_point;
    gt911_queue_event(GT911_TOUCH_MOVE, &s_last_point, now);
}

static void gt911_process_io_error(void)
{
    rt_tick_t now = rt_tick_get();

    s_stats.io_errors++;
    s_last_valid_tick = now;
    if (s_touch_active)
    {
        /* Never turn an I2C failure into a normal UP: that could invoke a
         * button's CLICKED callback even though the finger state is unknown. */
        gt911_queue_event(GT911_TOUCH_CANCEL, &s_last_point, now);
        s_touch_active = RT_FALSE;
        s_filter_valid = RT_FALSE;
    }
}

static void gt911_process_release_timeout(void)
{
    rt_tick_t now = rt_tick_get();
    rt_tick_t timeout = rt_tick_from_millisecond(GT911_RELEASE_TIMEOUT_MS);

    if (s_touch_active && (now - s_last_valid_tick) >= timeout)
    {
        /* A clean but silent panel is treated as a release. I2C errors take
         * the CANCEL path above, so this fallback cannot hide communication
         * failures behind a click. */
        gt911_queue_event(GT911_TOUCH_UP, &s_last_point, now);
        s_touch_active = RT_FALSE;
        s_filter_valid = RT_FALSE;
    }
}

static void gt911_touch_worker_entry(void *parameter)
{
    gt911_point_t points[GT911_MAX_POINTS];
    uint8_t count;

    RT_UNUSED(parameter);
    startup_mark(BOOT_TOUCH_BEGIN);
    if(gt911_init()!=0){
        startup_mark(BOOT_TOUCH_FAILED);return;
    }
    s_touch_ready=RT_TRUE;startup_mark(BOOT_TOUCH_READY);
    for (;;)
    {
        gt911_frame_status_t frame_status;

        frame_status = gt911_read_frame(points, &count);
        if (frame_status == GT911_FRAME_READY)
        {
            gt911_process_frame(points, count);
        }
        else if (frame_status == GT911_FRAME_NO_DATA)
        {
            s_stats.no_data++;
            gt911_process_release_timeout();
        }
        else
        {
            gt911_process_io_error();
        }

        rt_thread_mdelay(s_touch_active ? GT911_ACTIVE_POLL_MS :
                         GT911_IDLE_POLL_MS);
    }
}

int gt911_touch_start(void)
{
    rt_err_t result;

    if (s_touch_started)
    {
        return 0;
    }

    rt_memset(&s_stats, 0, sizeof(s_stats));
    s_event_head = 0U;
    s_event_tail = 0U;
    s_event_count = 0U;
    s_touch_active = RT_FALSE;
    s_filter_valid = RT_FALSE;
    s_last_valid_tick = rt_tick_get();
    s_invalid_status_streak = 0U;
    s_last_controller_reset_tick = 0U;
    s_controller_reset_done = RT_FALSE;

    /* PE15 remains an RT-Thread-owned pulled input. The worker polls the
     * controller status instead of attaching an EXTI handler; this avoids a
     * repeated-edge storm on panels that hold INT high until TPINFO is clear. */
    rt_pin_mode(TOUCH_INT_PIN, PIN_MODE_INPUT_PULLDOWN);

    s_touch_thread = rt_thread_create("touch", gt911_touch_worker_entry,
                                      RT_NULL, GT911_WORKER_STACK_SIZE,
                                      GT911_WORKER_PRIORITY,
                                      GT911_WORKER_TIMESLICE);
    if (s_touch_thread == RT_NULL)
    {
        rt_kprintf("[GT911] worker create failed\r\n");
        return -1;
    }
    s_touch_started = RT_TRUE;
    result = rt_thread_startup(s_touch_thread);
    if (result != RT_EOK)
    {
        rt_kprintf("[GT911] worker startup failed: %d\r\n", result);
        s_touch_started = RT_FALSE;
        return -1;
    }

    rt_kprintf("[GT911] async input ready: RT polling, queue=%u\r\n",
               (unsigned)GT911_EVENT_QUEUE_SIZE);
    return 0;
}

int gt911_touch_is_ready(void){return s_touch_ready!=RT_FALSE;}

int gt911_touch_pop(gt911_touch_event_t *event)
{
    rt_base_t level;

    if (event == RT_NULL)
    {
        return 0;
    }
    level = rt_hw_interrupt_disable();
    if (s_event_count == 0U)
    {
        rt_hw_interrupt_enable(level);
        return 0;
    }
    *event = s_event_queue[s_event_head];
    s_event_head = (uint8_t)((s_event_head + 1U) % GT911_EVENT_QUEUE_SIZE);
    s_event_count--;
    {
        uint32_t latency = (uint32_t)(rt_tick_get() - event->timestamp);
        if (latency > s_stats.max_queue_latency_ms)
        {
            s_stats.max_queue_latency_ms = latency;
        }
    }
    rt_hw_interrupt_enable(level);
    return 1;
}

uint8_t gt911_touch_pending(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint8_t count = s_event_count;
    rt_hw_interrupt_enable(level);
    return count;
}

void gt911_touch_stats_get(gt911_touch_stats_t *stats)
{
    rt_base_t level;

    if (stats == RT_NULL)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    *stats = s_stats;
    stats->queue_depth = s_event_count;
    stats->contact_active = s_touch_active ? 1U : 0U;
    rt_hw_interrupt_enable(level);
}
