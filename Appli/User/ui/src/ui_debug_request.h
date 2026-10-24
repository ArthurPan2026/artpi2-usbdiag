#ifndef WORK_UI_DEBUG_REQUEST_H
#define WORK_UI_DEBUG_REQUEST_H
#include <stdbool.h>
#include <stdint.h>

/* Caller/owner serialize these transitions with the RT-Thread critical lock.
 * An unclaimed timeout is cancelled; an already claimed action is explicitly
 * in flight, never silently described as cancelled. No LVGL/IPC call occurs
 * inside this pure, bounded state machine. */
typedef enum { UI_REQUEST_IDLE, UI_REQUEST_QUEUED, UI_REQUEST_RUNNING,
               UI_REQUEST_DONE } ui_request_phase_t;
typedef enum { UI_REQUEST_NONE, UI_REQUEST_EXECUTE, UI_REQUEST_EXPIRED } ui_request_claim_t;
typedef enum { UI_REQUEST_CANCELLED, UI_REQUEST_IN_FLIGHT, UI_REQUEST_COMPLETED } ui_request_timeout_t;
typedef struct {
    ui_request_phase_t phase;
    uint32_t started, budget;
    bool abandoned;
} ui_debug_request_t;
static inline bool ui_debug_request_submit(ui_debug_request_t *r, uint32_t now, uint32_t budget)
{
    if (r->phase != UI_REQUEST_IDLE) return false;
    r->phase=UI_REQUEST_QUEUED;r->started=now;r->budget=budget;r->abandoned=false;
    return true;
}
static inline ui_request_claim_t ui_debug_request_claim(ui_debug_request_t *r, uint32_t now)
{
    if (r->phase != UI_REQUEST_QUEUED) return UI_REQUEST_NONE;
    if ((uint32_t)(now-r->started) >= r->budget) {
        r->phase=UI_REQUEST_DONE;return UI_REQUEST_EXPIRED;
    }
    r->phase=UI_REQUEST_RUNNING;return UI_REQUEST_EXECUTE;
}
static inline ui_request_timeout_t ui_debug_request_timeout(ui_debug_request_t *r)
{
    if (r->phase == UI_REQUEST_DONE) return UI_REQUEST_COMPLETED;
    if (r->phase == UI_REQUEST_RUNNING) {
        r->abandoned=true;return UI_REQUEST_IN_FLIGHT;
    }
    r->phase=UI_REQUEST_IDLE;return UI_REQUEST_CANCELLED;
}
static inline bool ui_debug_request_finish(ui_debug_request_t *r)
{
    if (r->phase != UI_REQUEST_RUNNING) return false;
    r->phase=r->abandoned?UI_REQUEST_IDLE:UI_REQUEST_DONE;
    return !r->abandoned;
}
static inline bool ui_debug_request_consume(ui_debug_request_t *r)
{
    if (r->phase != UI_REQUEST_DONE) return false;
    r->phase=UI_REQUEST_IDLE;return true;
}
#endif
