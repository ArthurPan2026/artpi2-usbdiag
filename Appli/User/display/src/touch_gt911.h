#ifndef WORK_TOUCH_GT911_H
#define WORK_TOUCH_GT911_H

#include <stdint.h>

#define GT911_MAX_POINTS 5U

typedef struct
{
    uint16_t x;
    uint16_t y;
    uint16_t size;
    uint8_t track_id;
} gt911_point_t;

/* Events are produced by the RT-Thread touch worker and consumed by the
 * LVGL thread.  A CANCEL is deliberately different from UP: it must tear
 * down LVGL's pressed object without producing a click after an I2C fault. */
typedef enum
{
    GT911_TOUCH_DOWN = 0,
    GT911_TOUCH_MOVE,
    GT911_TOUCH_UP,
    GT911_TOUCH_CANCEL,
} gt911_touch_event_type_t;

typedef struct
{
    gt911_touch_event_type_t type;
    uint16_t x;
    uint16_t y;
    uint16_t size;
    uint8_t track_id;
    uint32_t timestamp;
} gt911_touch_event_t;

typedef struct
{
    uint32_t irq_count;
    uint32_t frames;
    uint32_t no_data;
    uint32_t io_errors;
    uint32_t down_count;
    uint32_t move_count;
    uint32_t up_count;
    uint32_t cancel_count;
    uint32_t coalesced_moves;
    uint32_t dropped_moves;
    uint32_t dropped_transitions;
    uint32_t queue_high_water;
    uint32_t max_queue_latency_ms;
    uint32_t status_errors;
    uint32_t point_errors;
    uint32_t clear_errors;
    uint32_t invalid_status;
    uint32_t controller_resets;
    uint8_t queue_depth;
    uint8_t contact_active;
    uint8_t last_status;
    uint8_t last_bad_status;
    uint8_t last_event;
    uint16_t last_x;
    uint16_t last_y;
    uint16_t last_size;
} gt911_touch_stats_t;

int gt911_init(void);

/* Start the asynchronous RT-Thread worker after LVGL has created its input
 * device. The worker never calls LVGL. */
int gt911_touch_start(void);
int gt911_touch_is_ready(void);

/* Non-blocking single-consumer queue API for lv_port_indev.c. */
int gt911_touch_pop(gt911_touch_event_t *event);
uint8_t gt911_touch_pending(void);
void gt911_touch_stats_get(gt911_touch_stats_t *stats);

#endif /* WORK_TOUCH_GT911_H */
